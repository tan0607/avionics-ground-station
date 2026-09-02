/**
 * useTelemetryLog — companion accumulator for the Log view.
 *
 * Like useSensorSeries, it deliberately does NOT touch useTelemetry (frozen);
 * it piggybacks on the published snapshot. Two things are derived here:
 *
 *   • rows   — one captured frame per ingested packet (newest-first, capped),
 *              driven off `chart.rev` (bumps exactly once per frame) so each
 *              packet is recorded once, not on every ~10 Hz UI republish.
 *   • events — a mission log built by DIFFING successive snapshots: flight-state
 *              transitions, pyro rising-edge, link live/stale/down changes, the
 *              no-deploy alarm, and onboard-clock jump-back (session reset).
 *
 * Cross-cutting fields (link, alarms, tPlusSec) update on UI ticks too, so the
 * latest snapshot is mirrored into `tRef` and read inside the effects — that
 * keeps the frame effect from having to re-run 10 Hz just to see fresh values.
 *
 * BOTH LISTS SURVIVE THE TAB CLOSING. They are mirrored to localStorage on a
 * quiet interval and flushed on pagehide, and restored on mount. The log had
 * been in-memory only, so it evaporated on quit and on every receiver reboot —
 * which the backend triggers itself, on every connect, by pulsing the ESP32's
 * reset line. Rows also carry the frame's `extra` fields and radio metrics now:
 * the Live view was showing a dozen readouts that the one view built for
 * reading telemetry back did not record at all.
 *
 * The backend's telemetry.csv remains the authoritative flight record; this is
 * the operator's working copy, and it does not need the backend to be up.
 */
import { useCallback, useEffect, useRef, useState } from "react"
import {
  FLIGHT_STATE_NAME,
  type FlightState,
  type GpsFix,
} from "@/lib/protocol"
import type { LinkState, TelemetryState } from "./useTelemetry"

/** A captured frame, flattened for tabular display. */
export interface LogRow {
  /**
   * Unique, monotonic, per-session. This is the React key — `seq` is NOT usable
   * as one: it wraps at 65535, it restarts whenever the flight computer reboots,
   * and the current transmitter sends every packet twice. Duplicate keys make
   * React reuse the wrong rows, which renders as blocks of the log repeating
   * themselves — data corruption that is entirely in the display layer.
   */
  id: number
  seq: number
  /** True when this row repeats the previous row's seq (the same packet heard twice). */
  duplicate: boolean
  onboardMs: number
  tPlusSec: number | null
  flightState: FlightState
  baroAltM: number
  vspeedMs: number
  tiltDeg: number
  gpsSats: number
  gpsFix: GpsFix
  /** null when the downlink carries no battery reading — rendered "—", not 0.0. */
  vbatV: number | null
  continuity: boolean
  pyroFired: boolean
  sdOk: boolean
  armed: boolean
  /** Per-packet radio quality, null when the source doesn't measure it. */
  rssiDbm: number | null
  snrDb: number | null
  /**
   * Every decoded field the protocol has no slot for — pressure, heading,
   * course, ground speed, body accel/velocity. Carried here because the Live
   * view showed all of them and the log showed none, so the one view meant for
   * reading telemetry back was the only one missing most of it.
   */
  extra: Record<string, number>
  /**
   * The flight folder this frame was recorded into, or null if REC was off.
   *
   * The Log view had no relation to the archive at all: you could scroll a
   * thousand rows without a hint of which of them are in a flight folder and
   * which only ever existed in the browser. Carried per row rather than derived
   * later, because the answer changes mid-table and the recorder's own history
   * is not queryable from here.
   */
  flight: string | null
}

export type EventSeverity = "info" | "nominal" | "caution" | "alarm"

export interface LogEvent {
  id: number
  onboardMs: number
  tPlusSec: number | null
  kind: "session" | "state" | "pyro" | "link" | "no-deploy" | "record"
  severity: EventSeverity
  message: string
  /** Small trailing context (altitude, state name…). */
  detail?: string
}

export interface TelemetryLog {
  rows: LogRow[] // newest-first
  events: LogEvent[] // newest-first
  rev: number
  /**
   * Empty the operator's working copy — rows, events and the persisted mirror.
   *
   * Deliberately MANUAL. Clearing automatically on a new session was the
   * obvious version and it is the same trap the recorder already learned:
   * the backend pulses the receiver's reset line on every connect, so "new
   * session" fires whenever the server restarts, and an auto-clear would wipe
   * the table mid-flight for no reason the operator caused. The authoritative
   * record is telemetry.csv on the backend either way; this list is what the
   * operator is looking at, so the operator says when it goes.
   */
  clear: () => void
}

// Internal caps. rows is generous (LogView slices to the user's rowCap);
// events is a mission log, so a few hundred is plenty for one flight.
const MAX_ROWS = 2000
const MAX_EVENTS = 300
// Onboard clock jumping this far backwards = new flight (mirrors useTelemetry).
const SESSION_RESET_MS = 1500

// --- persistence -------------------------------------------------------------
// The log used to live only in this hook's ref, so closing the tab threw the
// whole thing away — and so did every reboot of the receiver, because the
// backend pulses the ESP32's reset line on connect and the resulting onboard
// clock jump was treated as "new flight, clear the table".
//
// telemetry.csv on the backend is still the authoritative record. This is the
// operator's working copy: what they were looking at, restored where they left
// it, without needing the backend to be reachable or a file to be opened.
const STORAGE_KEY = "apex.log.v1"
// Well under the ~5 MB localStorage budget: 600 rows carrying aux fields is
// ~200 KB of JSON. Capped separately from MAX_ROWS because what is worth
// keeping in memory for this session and what is worth writing to disk for the
// next one are different questions.
const PERSIST_ROWS = 600
// Writing on every frame would serialise the whole log 2x a second for no
// benefit; a quiet flush plus a hard flush on hide covers both the crash case
// and the ordinary "close the lid" case.
const PERSIST_INTERVAL_MS = 2000

interface PersistedLog {
  rows: LogRow[]
  events: LogEvent[]
}

function loadPersisted(): PersistedLog | null {
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY)
    if (!raw) return null
    const parsed = JSON.parse(raw) as Partial<PersistedLog>
    if (!Array.isArray(parsed.rows) || !Array.isArray(parsed.events)) return null
    return { rows: parsed.rows, events: parsed.events }
  } catch {
    // Corrupt blob, quota-disabled storage, private mode — start empty rather
    // than taking the Log view down with it.
    return null
  }
}

function savePersisted(log: TelemetryLog): void {
  try {
    window.localStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({
        rows: log.rows.slice(0, PERSIST_ROWS),
        events: log.events.slice(0, MAX_EVENTS),
      }),
    )
  } catch {
    // Over quota or storage disabled: the in-memory log is unaffected, and a
    // failed save must never interrupt a live flight.
  }
}

const LINK_EVENT: Record<LinkState, { severity: EventSeverity; message: string }> = {
  live: { severity: "nominal", message: "LINK LIVE" },
  stale: { severity: "caution", message: "LINK STALE" },
  down: { severity: "alarm", message: "LINK LOST" },
}

/** What the recorder is doing, as much of it as the log needs. */
export interface LogRecording {
  recording: boolean
  /** Flight folder name while recording, e.g. "flight-02_apex-1-1224". */
  flight: string | null
}

const NOT_RECORDING: LogRecording = { recording: false, flight: null }

export function useTelemetryLog(
  t: TelemetryState,
  rec: LogRecording = NOT_RECORDING,
): TelemetryLog {
  // Seeded from localStorage so reopening the console shows the log where it
  // was left, not an empty table. useRef's initialiser runs on every render, so
  // the read is done once via a lazy useState instead.
  const [restored] = useState(loadPersisted)
  const ref = useRef<TelemetryLog>({
    rows: restored?.rows ?? [],
    events: restored?.events ?? [],
    rev: 0,
    // Replaced below with the real implementation; the ref's initialiser
    // cannot see the callbacks that have not been declared yet.
    clear: () => {},
  })

  // Latest snapshot, readable inside effects without widening their deps.
  const tRef = useRef(t)
  tRef.current = t

  // Same trick for the recorder: it polls on its own 2 s clock, and the frame
  // effect must not re-run just because that poll returned.
  const recRef = useRef(rec)
  recRef.current = rec

  // Diff bookkeeping.
  const lastChartRev = useRef(-1)
  const prevState = useRef<FlightState | null>(null)
  const prevPyro = useRef(false)
  const prevNoDeploy = useRef(false)
  const prevOnboardMs = useRef<number | null>(null)
  const prevLink = useRef<LinkState | null>(null)
  // Both lists are newest-first, so [0] holds the highest id. RESUMING past the
  // restored maximum is not optional: ids are the React keys, and restarting at
  // 0 alongside a restored log would hand React duplicate keys, which makes it
  // reuse the wrong rows and render blocks of the log repeating themselves.
  const nextRowId = useRef((restored?.rows[0]?.id ?? -1) + 1)
  const nextId = useRef((restored?.events[0]?.id ?? 0) + 1)

  const [, forceRender] = useState(0)

  const pushEvent = (e: Omit<LogEvent, "id">) => {
    const events = ref.current.events
    events.unshift({ id: nextId.current++, ...e })
    if (events.length > MAX_EVENTS) events.length = MAX_EVENTS
  }

  const clear = useCallback(() => {
    ref.current.rows = []
    ref.current.events = []
    // A cleared log that is simply empty looks like a log that never recorded.
    // One surviving line says which it was, and when.
    pushEvent({
      onboardMs: tRef.current.frame?.onboardMs ?? 0,
      tPlusSec: tRef.current.tPlusSec,
      kind: "session",
      severity: "info",
      message: "LOG CLEARED",
      detail: "by operator",
    })
    // Drop the persisted mirror too, or the next reload restores exactly what
    // was just cleared. The 2 s flush re-writes the emptied log straight after.
    try {
      window.localStorage.removeItem(STORAGE_KEY)
    } catch {
      // Storage disabled or over quota — the in-memory clear already happened.
    }
    // nextRowId/nextId are refs and keep climbing, so the ids handed to React
    // after a clear can never collide with the ones it just unmounted.
    ref.current.rev += 1
    forceRender((n) => n + 1)
    // pushEvent closes over refs only, so this callback never needs rebuilding.
  }, [])
  ref.current.clear = clear

  // --- frame stream: rows + frame-derived events -----------------------------
  useEffect(() => {
    if (t.chart.rev === lastChartRev.current) return // idempotent under StrictMode
    lastChartRev.current = t.chart.rev

    const snap = tRef.current
    const frame = snap.frame
    if (!frame) return

    // Session reset: the onboard clock jumped backwards. Log a marker and forget
    // the previous flight's edges so the new flight's transitions log cleanly.
    //
    // The rows are KEPT. This used to clear them, which sounds tidy and is the
    // second way the log disappeared: the backend pulses the receiver's reset
    // line on every connect, so merely restarting the server wiped the operator's
    // whole table. The SESSION RESET event below is the boundary marker — that
    // is enough to tell two flights apart, and it costs nothing, whereas
    // throwing away the previous flight's telemetry is unrecoverable from here.
    // Old rows age out at MAX_ROWS on their own.
    const prevOn = prevOnboardMs.current
    if (prevOn != null && frame.onboardMs + SESSION_RESET_MS < prevOn) {
      prevState.current = null
      prevPyro.current = false
      prevNoDeploy.current = false
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.frameTPlusSec,
        kind: "session",
        severity: "info",
        message: "SESSION RESET",
        detail: "new flight",
      })
    }

    const rowsNow = ref.current.rows
    const row: LogRow = {
      id: nextRowId.current++,
      seq: frame.seq,
      // rows[0] is the newest (unshift below), so that is the previous frame.
      duplicate: rowsNow.length > 0 && rowsNow[0].seq === frame.seq,
      onboardMs: frame.onboardMs,
      tPlusSec: snap.frameTPlusSec,
      flightState: frame.flightState,
      baroAltM: frame.baroAltM,
      vspeedMs: frame.vspeedMs,
      tiltDeg: frame.tiltDeg,
      gpsSats: frame.gpsSats,
      gpsFix: frame.gpsFix,
      vbatV: frame.vbatV,
      continuity: frame.continuity,
      pyroFired: frame.pyroFired,
      sdOk: frame.sdOk,
      armed: frame.armed,
      rssiDbm: frame.rssiDbm,
      snrDb: frame.snrDb,
      extra: frame.extra,
      flight: recRef.current.recording ? recRef.current.flight : null,
    }
    const rows = rowsNow
    rows.unshift(row)
    if (rows.length > MAX_ROWS) rows.length = MAX_ROWS

    const stateName = FLIGHT_STATE_NAME[frame.flightState]
    const altDetail = `${Math.round(frame.baroAltM)} m`

    if (prevState.current === null) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.frameTPlusSec,
        kind: "session",
        severity: "info",
        message: "SESSION START",
        detail: stateName,
      })
    } else if (prevState.current !== frame.flightState) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.frameTPlusSec,
        kind: "state",
        severity: "info",
        message: stateName,
        detail: altDetail,
      })
    }

    if (!prevPyro.current && frame.pyroFired) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.frameTPlusSec,
        kind: "pyro",
        severity: "caution",
        message: "PYRO FIRED",
        detail: stateName,
      })
    }

    if (!prevNoDeploy.current && snap.alarms.noDeploy) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.frameTPlusSec,
        kind: "no-deploy",
        severity: "alarm",
        message: "NO-DEPLOY",
        detail: "past apogee · no pyro",
      })
    }

    prevState.current = frame.flightState
    prevPyro.current = frame.pyroFired
    prevNoDeploy.current = snap.alarms.noDeploy
    prevOnboardMs.current = frame.onboardMs

    ref.current.rev += 1
    forceRender((n) => n + 1)
  }, [t.chart.rev])

  // --- recording boundaries --------------------------------------------------
  // The one boundary in this log an operator draws deliberately. SESSION RESET
  // below marks where the VEHICLE restarted; this marks where the archive
  // folder opened and closed, which is the line that decides what ends up on
  // disk under a name.
  const prevRecording = useRef<boolean | null>(null)
  useEffect(() => {
    // Seed without emitting: arriving on a console that is already recording is
    // not a start, and the backend poll resolves after the first render.
    if (prevRecording.current === null) {
      prevRecording.current = rec.recording
      return
    }
    if (prevRecording.current === rec.recording) return
    prevRecording.current = rec.recording

    const snap = tRef.current
    pushEvent({
      onboardMs: snap.frame?.onboardMs ?? 0,
      tPlusSec: snap.tPlusSec,
      kind: "record",
      severity: "info",
      message: rec.recording ? "RECORDING STARTED" : "RECORDING STOPPED",
      // Through the ref, not the prop: the flag and the folder name arrive in
      // the same poll, and depending on the name would log a second start for
      // the same recording.
      detail: recRef.current.flight ?? undefined,
    })
    ref.current.rev += 1
    forceRender((n) => n + 1)
  }, [rec.recording])

  // --- link transitions ------------------------------------------------------
  useEffect(() => {
    // Seed the first observed link state without emitting (avoids a spurious
    // "LINK LOST" at boot, before any packet has arrived).
    if (prevLink.current === null) {
      prevLink.current = t.link
      return
    }
    if (prevLink.current === t.link) return
    prevLink.current = t.link

    const { severity, message } = LINK_EVENT[t.link]
    const snap = tRef.current
    pushEvent({
      onboardMs: snap.frame?.onboardMs ?? 0,
      // The WALL-CLOCK T+, not the last frame's: a link event happens when the
      // ground station notices, and the last frame is by definition stale
      // exactly when this fires. Stamping "LINK LOST" with the timestamp of the
      // final packet would record the loss as having happened before it did.
      tPlusSec: snap.tPlusSec,
      kind: "link",
      severity,
      message,
    })
    ref.current.rev += 1
    forceRender((n) => n + 1)
  }, [t.link])

  // --- persistence: quiet ticks + a hard flush when the page goes away --------
  useEffect(() => {
    const flush = () => savePersisted(ref.current)
    const timer = window.setInterval(flush, PERSIST_INTERVAL_MS)

    // `pagehide` (not `beforeunload`) is the one that actually fires on a tab
    // close and on mobile/bfcache suspends, which is precisely the "I quit it"
    // case this whole block exists for. `visibilitychange` covers switching
    // away without closing.
    const onHide = () => flush()
    window.addEventListener("pagehide", onHide)
    document.addEventListener("visibilitychange", onHide)

    return () => {
      window.clearInterval(timer)
      window.removeEventListener("pagehide", onHide)
      document.removeEventListener("visibilitychange", onHide)
      flush()
    }
  }, [])

  return ref.current
}
