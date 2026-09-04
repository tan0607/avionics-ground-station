/**
 * useTelemetry — the single reactive source of truth for the dashboard.
 *
 * It selects a source (the mock sim by default; a native WebSocket when a URL
 * is configured), ingests normalized frames, and derives everything the UI
 * reasons about: link freshness, the T+ mission clock, packet-loss %, the
 * measured arrival rate, running max altitude, the streaming altitude series,
 * and the safety alarms.
 *
 * TWO CLOCKS, deliberately. Anything describing the VEHICLE (altitude, state,
 * the per-frame T+ stamped onto log rows) comes from the last frame and is only
 * as fresh as the link. Anything describing TIME (the mission clock, link age)
 * runs off Date.now() on the UI heartbeat, so it keeps moving between packets
 * and after the link dies. Deriving the second kind from the first is what made
 * the mission clock freeze on a quiet link.
 *
 * Design: high-rate mutable accumulators (chart arrays, the loss window) live
 * in refs so they never trigger React reconciliation. A compact immutable
 * SNAPSHOT is published to state on each frame and on a steady UI tick, and the
 * chart series carries a `rev` counter so uPlot can diff without React
 * comparing hundreds of samples. See DESIGN_SPECS §2 (native WS) / §3 (link).
 */
import { useEffect, useRef, useState } from "react"
import {
  Flag,
  FlightState,
  LINK_STALE_MS,
  PACKET_INTERVAL_MS,
  hasLaunched,
  isDescending,
  isFlagKnown,
  normalizeFrame,
  type TelemetryFrame,
  type WireFrame,
} from "@/lib/protocol"
import { MockFlightSim } from "@/lib/mock"
import { TelemetrySocket, type SocketStatus } from "@/lib/wsClient"

/** Link freshness derived from time-since-last-packet + socket state. */
export type LinkState = "live" | "stale" | "down"

/** Where the frames are coming from this session. */
export type TelemetrySource = "mock" | "ws"

/** Streaming chart series. Arrays are mutated in place; `rev` bumps to signal. */
export interface ChartSeries {
  xs: number[] // onboard seconds
  ys: number[] // baro altitude, m AGL
  apogee: { t: number; alt: number } | null
  /**
   * Onboard seconds at liftoff — the x of the first non-PAD frame; null until
   * the vehicle leaves the pad. The altitude chart anchors its window here so
   * the flight profile is not compressed into a sliver by an hour of pad wait.
   *
   * It is an x value, NOT a timestamp: the two launch refs above are clocks
   * (host and browser) for the mission clock, and neither is in the chart's
   * coordinate space. Reusing one of them here would put the window's left
   * edge somewhere off the axis entirely.
   */
  liftoffT: number | null
  /**
   * Onboard seconds at touchdown, once a flight we actually SAW has ended;
   * null otherwise. The altitude chart stops extending its right edge here, so
   * however long the airframe then sits on the ground does not re-compress the
   * profile the operator is trying to read.
   *
   * Cleared again if the vehicle reports an in-flight state afterwards — a
   * bounced detector, or a second flight on one power cycle. A window that has
   * quietly stopped following a moving vehicle is worse than a wide one.
   */
  landedT: number | null
  rev: number
}

/**
 * The flags the no-deploy alarm has to read. Exported because the Settings card
 * shows the alarm as unavailable on a downlink that carries neither, and it has
 * to test the same bits this hook does.
 */
export const NO_DEPLOY_FLAGS = Flag.PYRO_FIRED | Flag.CONTINUITY

export interface Alarms {
  /** No packet within the stale window — the operator has lost the vehicle. */
  linkStale: boolean
  /**
   * Past apogee / descending but no pyro event + continuity still closed.
   * Always false on a downlink that does not report pyro + continuity: an
   * alarm that cannot be evaluated must stay silent, not guess.
   */
  noDeploy: boolean
}

export interface TelemetryState {
  source: TelemetrySource
  status: SocketStatus | "mock"
  link: LinkState
  linkAgeMs: number
  frame: TelemetryFrame | null
  /** Seconds since first BOOST; null before liftoff. Runs on the WALL CLOCK. */
  tPlusSec: number | null
  /**
   * T+ of the CURRENT FRAME, from its own host timestamp — "when was this frame
   * sent", not "how long has the vehicle been flying". The log stamps rows with
   * this so a row keeps the time it belongs to; the top bar shows `tPlusSec`.
   */
  frameTPlusSec: number | null
  /** Fraction lost over the recent window, 0..1. */
  lossFraction: number
  /**
   * Measured arrival rates over the recent window — what the link is ACTUALLY
   * doing, as opposed to what protocol.ts guesses it does.
   *
   * `lineHz` counts every decoded frame, which is what a serial monitor shows.
   * `frameHz` counts only frames carrying a new seq. The current transmitter
   * sends every packet twice, so `frameHz` is about half of `lineHz` and it is
   * `frameHz` that bounds how often any readout can change. Showing only one of
   * them is what made "the dashboard is slower than the serial monitor" an
   * unanswerable question.
   */
  lineHz: number
  frameHz: number
  maxAltM: number
  chart: ChartSeries
  alarms: Alarms
}

// --- tuning -----------------------------------------------------------------
const UI_TICK_MS = 100 // republish link age / clock at 10 Hz even when idle
const LOSS_WINDOW_MS = 10_000 // sliding window for packet-loss stats
const DOWN_AFTER_MS = LINK_STALE_MS * 2 // "stale" → "down" once well past
const MAX_CHART_POINTS = 4000 // safety cap (a real flight is ~300 pts)
const SESSION_RESET_MS = 1500 // onboard clock jumping back = new flight/session

/** One received-packet event in the loss window: how many were missed before it. */
interface LossEvent {
  t: number
  missed: number
  /** Repeat of the previous seq — a real reception, but not new telemetry. */
  duplicate: boolean
}

function chooseSource(): { source: TelemetrySource; url: string | null } {
  const params = new URLSearchParams(window.location.search)
  const q = params.get("source")
  const envUrl = import.meta.env.VITE_WS_URL as string | undefined
  // Explicit ?source=mock always wins (handy for demos even if a URL is set).
  if (q === "mock") return { source: "mock", url: null }
  if (q === "ws" || envUrl) return { source: "ws", url: envUrl ?? defaultWsUrl() }
  // A production build is served BY the backend, so /ws is same-origin and real.
  // Defaulting to the simulator there is the worst possible failure mode: the
  // page looks alive, the numbers move, and none of it came off the radio. Only
  // `vite dev` (which serves the page itself, with no backend behind it) falls
  // back to the mock.
  if (!import.meta.env.DEV) return { source: "ws", url: defaultWsUrl() }
  return { source: "mock", url: null }
}

function defaultWsUrl(): string {
  const proto = window.location.protocol === "https:" ? "wss" : "ws"
  // Same origin, including the port: `--port` is a backend flag, so hardcoding
  // 8000 here would break every non-default run.
  const host = import.meta.env.DEV ? `${window.location.hostname}:8000` : window.location.host
  return `${proto}://${host}/ws`
}

export function useTelemetry(): TelemetryState {
  const [{ source, url }] = useState(chooseSource)

  // --- mutable accumulators (never cause a render on their own) ------------
  const frameRef = useRef<TelemetryFrame | null>(null)
  const lastArrivalRef = useRef<number>(0)
  const statusRef = useRef<SocketStatus | "mock">(source === "mock" ? "mock" : "connecting")
  // Two launch baselines, taken at the same instant and NOT interchangeable:
  //   launchHostRef — the first non-PAD frame's hostTime (the backend's clock).
  //                   Used to stamp a PER-FRAME T+ onto log rows, where the
  //                   right answer is "when was this frame sent", not "now".
  //   launchAtRef   — Date.now() at that same ingest (this browser's clock).
  //                   Drives the live mission clock, which has to keep running
  //                   between packets and after the link dies.
  // Subtracting one from the other would fold in host/browser clock skew.
  const launchHostRef = useRef<number | null>(null)
  const launchAtRef = useRef<number | null>(null)
  const maxAltRef = useRef<number>(0)
  const lastSeqRef = useRef<number | null>(null)
  const lossRef = useRef<LossEvent[]>([])
  const chartRef = useRef<ChartSeries>({
    xs: [], ys: [], apogee: null, liftoffT: null, landedT: null, rev: 0,
  })

  const [snapshot, setSnapshot] = useState<TelemetryState>(() => ({
    source,
    status: statusRef.current,
    link: "down",
    linkAgeMs: 0,
    frame: null,
    tPlusSec: null,
    frameTPlusSec: null,
    lossFraction: 0,
    lineHz: 0,
    frameHz: 0,
    maxAltM: 0,
    chart: chartRef.current,
    alarms: { linkStale: false, noDeploy: false },
  }))

  useEffect(() => {
    let raf = 0

    /** Recompute the published snapshot from refs. Cheap; runs ~10 Hz + per frame. */
    const publish = () => {
      const now = Date.now()
      const frame = frameRef.current
      const ageMs = lastArrivalRef.current ? now - lastArrivalRef.current : Infinity

      const connected = statusRef.current === "open" || statusRef.current === "mock"
      let link: LinkState
      if (ageMs < LINK_STALE_MS && connected) link = "live"
      else if (ageMs < DOWN_AFTER_MS) link = "stale"
      else link = "down"

      pruneLoss(now)
      const lossFraction = computeLoss()
      const [lineHz, frameHz] = computeRates(now)

      // THE MISSION CLOCK RUNS ON THE WALL CLOCK, not on packet arrivals.
      //
      // This used to read `(frame.hostTime - launchHost) / 1000`, so T+ only
      // moved when a packet landed: it advanced in 500 ms steps on a 2 Hz link,
      // stuttered across the duplicate pair, and froze outright the moment the
      // link went quiet — at precisely the moment an operator most needs to know
      // how long the vehicle has been flying. It also made the comment on the
      // heartbeat interval below a lie; that timer republished a clock that
      // could not tick.
      //
      // Baseline is `launchAtRef` (browser Date.now at the first non-PAD frame),
      // NOT the frame's hostTime, so the subtraction never mixes the backend's
      // clock with this one.
      const tPlusSec =
        launchAtRef.current != null ? (now - launchAtRef.current) / 1000 : null
      const frameTPlusSec =
        frame && launchHostRef.current != null
          ? (frame.hostTime - launchHostRef.current) / 1000
          : null

      // Only evaluable on a downlink that actually reports the two flags. The
      // MRCC text format carries neither, so `pyroFired`/`continuity` arrive as
      // the coerceBool default rather than as measurements — and this test read
      // that default as fact. It happened to land on `false` (no alarm), the
      // right answer for the wrong reason: one flipped default away from an
      // unsilenceable no-deploy alarm on every descent. GoNoGo gates its
      // Continuity/Pyro rows on the same mask; this is the audible half of it.
      const noDeploy = Boolean(
        frame &&
          isFlagKnown(frame, NO_DEPLOY_FLAGS) &&
          isDescending(frame.flightState) &&
          !frame.pyroFired &&
          frame.continuity,
      )

      setSnapshot({
        source,
        status: statusRef.current,
        link,
        linkAgeMs: ageMs === Infinity ? 0 : ageMs,
        frame,
        tPlusSec,
        frameTPlusSec,
        lossFraction,
        lineHz,
        frameHz,
        maxAltM: maxAltRef.current,
        chart: chartRef.current,
        alarms: { linkStale: link !== "live", noDeploy },
      })
    }

    /** Ingest one decoded frame: update loss stats, launch clock, chart, max alt. */
    const ingest = (frame: TelemetryFrame) => {
      const now = Date.now()

      // Session reset: the onboard clock jumping backwards means a fresh flight
      // (the mock loops; real hardware would be a reboot). Keep seq/loss stats
      // continuous — same radio session — but reset the flight-scoped series.
      const prev = frameRef.current
      if (prev && frame.onboardMs + SESSION_RESET_MS < prev.onboardMs) {
        resetFlight()
      }

      const duplicate = recordLoss(frame.seq, now)
      lastArrivalRef.current = now
      frameRef.current = frame

      if (launchHostRef.current == null && hasLaunched(frame.flightState)) {
        launchHostRef.current = frame.hostTime
        launchAtRef.current = now
        // The chart's own x for this instant. Recorded even if this frame is a
        // duplicate: a repeat carries the same onboard time, so the anchor is
        // identical either way, and waiting for a non-duplicate would push the
        // anchor a packet later for no gain.
        chartRef.current.liftoffT = frame.onboardMs / 1000
      }

      // Touchdown, but only after a liftoff this session actually observed: a
      // vehicle sitting on the bench can report LANDED (mrcc.py's own example
      // downlink line does exactly that), and honouring it would freeze the
      // altitude window before the flight had even started.
      if (frame.flightState === FlightState.LANDED) {
        if (chartRef.current.liftoffT != null && chartRef.current.landedT == null) {
          chartRef.current.landedT = frame.onboardMs / 1000
        }
      } else if (chartRef.current.landedT != null && hasLaunched(frame.flightState)) {
        chartRef.current.landedT = null
      }
      if (frame.baroAltM > maxAltRef.current) maxAltRef.current = frame.baroAltM

      // A repeat reception carries no NEW telemetry -- same seq, same onboard
      // time, same readings -- so it must not become a chart sample. Appending
      // it puts a second point at an x the series already has: the trace stops
      // advancing for a sample and then jumps, which reads as the chart lagging
      // the numbers. It also burns the point budget at the repeat rate, and on
      // this link that is ~2x typical and up to 12x in bursts (measured on the
      // 2026-08-19 bench log), so MAX_CHART_POINTS stops being the ~10x headroom
      // over a real flight that it is sized to be.
      //
      // Everything a duplicate legitimately proves is still recorded above: the
      // link is alive (lastArrival), it counts in the line rate, and its own
      // RSSI/SNR -- which genuinely differ per copy -- reach the readouts via
      // frameRef. Only the flight-history series skips it.
      if (!duplicate) {
        const c = chartRef.current
        const tSec = frame.onboardMs / 1000
        c.xs.push(tSec)
        c.ys.push(frame.baroAltM)
        if (c.xs.length > MAX_CHART_POINTS) {
          c.xs.shift()
          c.ys.shift()
        }
        if (frame.flightState === FlightState.APOGEE || frame.baroAltM >= maxAltRef.current) {
          c.apogee = { t: tSec, alt: maxAltRef.current }
        }
        c.rev += 1
      }

      publish()
    }

    function resetFlight() {
      launchHostRef.current = null
      launchAtRef.current = null
      maxAltRef.current = 0
      const c = chartRef.current
      c.xs.length = 0
      c.ys.length = 0
      c.apogee = null
      c.liftoffT = null
      c.landedT = null
      // rev bumped by the caller's push; a cleared array + new points reads fine.
    }

    /** seq gap → number missed since the previous received packet. Handles wrap.
     *  Returns true if this was a repeat of the previous seq. */
    function recordLoss(seq: number, t: number): boolean {
      const last = lastSeqRef.current
      if (last != null) {
        const gap = (seq - last + 0x10000) & 0xffff
        const missed = gap > 0 ? gap - 1 : 0
        // gap === 0 is the same packet heard again. It is a genuine reception —
        // it belongs in the line rate — but it carries no new telemetry, so it
        // must not count toward the frame rate and must not be "expected" by
        // the loss math (mirrors backend LossTracker.duplicates).
        lossRef.current.push({ t, missed, duplicate: gap === 0 })
        lastSeqRef.current = seq
        return gap === 0
      }
      lossRef.current.push({ t, missed: 0, duplicate: false })
      lastSeqRef.current = seq
      return false
    }

    function pruneLoss(now: number) {
      const cutoff = now - LOSS_WINDOW_MS
      const w = lossRef.current
      let i = 0
      while (i < w.length && w[i].t < cutoff) i += 1
      if (i > 0) w.splice(0, i)
    }

    function computeLoss(): number {
      const w = lossRef.current
      if (w.length === 0) return 0
      const missed = w.reduce((sum, e) => sum + e.missed, 0)
      // Duplicates are excluded from `received` so this matches the backend's
      // LossTracker exactly. Counting them would inflate the denominator by the
      // repeat rate — on this link that is ~2x, which halves every loss figure
      // the operator reads and disagrees with /stats on the same window.
      const received = w.reduce((n, e) => n + (e.duplicate ? 0 : 1), 0)
      const expected = received + missed
      return expected > 0 ? missed / expected : 0
    }

    /** Measured arrivals per second over the window: [every line, new frames]. */
    function computeRates(now: number): [number, number] {
      const w = lossRef.current
      if (w.length < 2) return [0, 0]
      // Measure over the window we actually hold, not LOSS_WINDOW_MS, so the
      // readout is honest in the first seconds after connect instead of
      // reporting a tenth of the real rate while the window fills.
      const spanMs = Math.min(LOSS_WINDOW_MS, now - w[0].t)
      if (spanMs <= 0) return [0, 0]
      const unique = w.reduce((n, e) => n + (e.duplicate ? 0 : 1), 0)
      return [(w.length * 1000) / spanMs, (unique * 1000) / spanMs]
    }

    // --- wire up the chosen source ------------------------------------------
    let socket: TelemetrySocket | null = null
    let mockTimer: ReturnType<typeof setInterval> | null = null
    let uiTimer: ReturnType<typeof setInterval> | null = null

    const onWire = (data: unknown) => {
      if (data && typeof data === "object") ingest(normalizeFrame(data as WireFrame))
    }

    if (source === "ws" && url) {
      socket = new TelemetrySocket({
        url,
        onMessage: onWire,
        onStatus: (s: SocketStatus) => {
          statusRef.current = s
          publish()
        },
      })
      socket.start()
    } else {
      const sim = new MockFlightSim()
      statusRef.current = "mock"
      mockTimer = setInterval(() => {
        const wire = sim.step()
        if (wire) onWire(wire) // null = a dropped packet; loss shows it via seq gap
      }, PACKET_INTERVAL_MS)
    }

    // Steady heartbeat so link age / mission clock advance even with no packets.
    uiTimer = setInterval(() => {
      raf = requestAnimationFrame(publish)
    }, UI_TICK_MS)

    return () => {
      socket?.stop()
      if (mockTimer) clearInterval(mockTimer)
      if (uiTimer) clearInterval(uiTimer)
      cancelAnimationFrame(raf)
    }
  }, [source, url])

  return snapshot
}
