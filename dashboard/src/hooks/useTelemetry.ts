/**
 * useTelemetry — the single reactive source of truth for the dashboard.
 *
 * It selects a source (the mock sim by default; a native WebSocket when a URL
 * is configured), ingests normalized frames, and derives everything the UI
 * reasons about: link freshness, the T+ mission clock, packet-loss %, running
 * max altitude, the streaming altitude series, and the safety alarms.
 *
 * Design: high-rate mutable accumulators (chart arrays, the loss window) live
 * in refs so they never trigger React reconciliation. A compact immutable
 * SNAPSHOT is published to state on each frame and on a steady UI tick, and the
 * chart series carries a `rev` counter so uPlot can diff without React
 * comparing hundreds of samples. See DESIGN_SPECS §2 (native WS) / §3 (link).
 */
import { useEffect, useRef, useState } from "react"
import {
  FlightState,
  LINK_STALE_MS,
  PACKET_INTERVAL_MS,
  isDescending,
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
  rev: number
}

export interface Alarms {
  /** No packet within the stale window — the operator has lost the vehicle. */
  linkStale: boolean
  /** Past apogee / descending but no pyro event + continuity still closed. */
  noDeploy: boolean
}

export interface TelemetryState {
  source: TelemetrySource
  status: SocketStatus | "mock"
  link: LinkState
  linkAgeMs: number
  frame: TelemetryFrame | null
  /** Seconds since first BOOST; null before liftoff. */
  tPlusSec: number | null
  /** Fraction lost over the recent window, 0..1. */
  lossFraction: number
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
}

function chooseSource(): { source: TelemetrySource; url: string | null } {
  const params = new URLSearchParams(window.location.search)
  const q = params.get("source")
  const envUrl = import.meta.env.VITE_WS_URL as string | undefined
  // Explicit ?source=mock always wins (handy for demos even if a URL is set).
  if (q === "mock") return { source: "mock", url: null }
  if (q === "ws" || envUrl) return { source: "ws", url: envUrl ?? defaultWsUrl() }
  return { source: "mock", url: null }
}

function defaultWsUrl(): string {
  const proto = window.location.protocol === "https:" ? "wss" : "ws"
  return `${proto}://${window.location.hostname}:8000/ws`
}

export function useTelemetry(): TelemetryState {
  const [{ source, url }] = useState(chooseSource)

  // --- mutable accumulators (never cause a render on their own) ------------
  const frameRef = useRef<TelemetryFrame | null>(null)
  const lastArrivalRef = useRef<number>(0)
  const statusRef = useRef<SocketStatus | "mock">(source === "mock" ? "mock" : "connecting")
  const launchHostRef = useRef<number | null>(null) // hostTime of first BOOST
  const maxAltRef = useRef<number>(0)
  const lastSeqRef = useRef<number | null>(null)
  const lossRef = useRef<LossEvent[]>([])
  const chartRef = useRef<ChartSeries>({ xs: [], ys: [], apogee: null, rev: 0 })

  const [snapshot, setSnapshot] = useState<TelemetryState>(() => ({
    source,
    status: statusRef.current,
    link: "down",
    linkAgeMs: 0,
    frame: null,
    tPlusSec: null,
    lossFraction: 0,
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

      const tPlusSec =
        frame && launchHostRef.current != null
          ? (frame.hostTime - launchHostRef.current) / 1000
          : null

      const noDeploy = Boolean(
        frame && isDescending(frame.flightState) && !frame.pyroFired && frame.continuity,
      )

      setSnapshot({
        source,
        status: statusRef.current,
        link,
        linkAgeMs: ageMs === Infinity ? 0 : ageMs,
        frame,
        tPlusSec,
        lossFraction,
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

      recordLoss(frame.seq, now)
      lastArrivalRef.current = now
      frameRef.current = frame

      if (launchHostRef.current == null && frame.flightState !== FlightState.PAD) {
        launchHostRef.current = frame.hostTime
      }
      if (frame.baroAltM > maxAltRef.current) maxAltRef.current = frame.baroAltM

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

      publish()
    }

    function resetFlight() {
      launchHostRef.current = null
      maxAltRef.current = 0
      const c = chartRef.current
      c.xs.length = 0
      c.ys.length = 0
      c.apogee = null
      // rev bumped by the caller's push; a cleared array + new points reads fine.
    }

    /** seq gap → number missed since the previous received packet. Handles wrap. */
    function recordLoss(seq: number, t: number) {
      const last = lastSeqRef.current
      if (last != null) {
        const gap = (seq - last + 0x10000) & 0xffff
        const missed = gap > 0 ? gap - 1 : 0
        lossRef.current.push({ t, missed })
      } else {
        lossRef.current.push({ t, missed: 0 })
      }
      lastSeqRef.current = seq
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
      const received = w.length
      const expected = received + missed
      return expected > 0 ? missed / expected : 0
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
