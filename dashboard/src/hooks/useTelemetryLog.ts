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
 */
import { useEffect, useRef, useState } from "react"
import {
  FLIGHT_STATE_NAME,
  type FlightState,
  type GpsFix,
} from "@/lib/protocol"
import type { LinkState, TelemetryState } from "./useTelemetry"

/** A captured frame, flattened for tabular display. */
export interface LogRow {
  seq: number
  onboardMs: number
  tPlusSec: number | null
  flightState: FlightState
  baroAltM: number
  vspeedMs: number
  tiltDeg: number
  gpsSats: number
  gpsFix: GpsFix
  vbatV: number
  continuity: boolean
  pyroFired: boolean
  sdOk: boolean
  armed: boolean
}

export type EventSeverity = "info" | "nominal" | "caution" | "alarm"

export interface LogEvent {
  id: number
  onboardMs: number
  tPlusSec: number | null
  kind: "session" | "state" | "pyro" | "link" | "no-deploy"
  severity: EventSeverity
  message: string
  /** Small trailing context (altitude, state name…). */
  detail?: string
}

export interface TelemetryLog {
  rows: LogRow[] // newest-first
  events: LogEvent[] // newest-first
  rev: number
}

// Internal caps. rows is generous (LogView slices to the user's rowCap);
// events is a mission log, so a few hundred is plenty for one flight.
const MAX_ROWS = 2000
const MAX_EVENTS = 300
// Onboard clock jumping this far backwards = new flight (mirrors useTelemetry).
const SESSION_RESET_MS = 1500

const LINK_EVENT: Record<LinkState, { severity: EventSeverity; message: string }> = {
  live: { severity: "nominal", message: "LINK LIVE" },
  stale: { severity: "caution", message: "LINK STALE" },
  down: { severity: "alarm", message: "LINK LOST" },
}

export function useTelemetryLog(t: TelemetryState): TelemetryLog {
  const ref = useRef<TelemetryLog>({ rows: [], events: [], rev: 0 })

  // Latest snapshot, readable inside effects without widening their deps.
  const tRef = useRef(t)
  tRef.current = t

  // Diff bookkeeping.
  const lastChartRev = useRef(-1)
  const prevState = useRef<FlightState | null>(null)
  const prevPyro = useRef(false)
  const prevNoDeploy = useRef(false)
  const prevOnboardMs = useRef<number | null>(null)
  const prevLink = useRef<LinkState | null>(null)
  const nextId = useRef(1)

  const [, forceRender] = useState(0)

  const pushEvent = (e: Omit<LogEvent, "id">) => {
    const events = ref.current.events
    events.unshift({ id: nextId.current++, ...e })
    if (events.length > MAX_EVENTS) events.length = MAX_EVENTS
  }

  // --- frame stream: rows + frame-derived events -----------------------------
  useEffect(() => {
    if (t.chart.rev === lastChartRev.current) return // idempotent under StrictMode
    lastChartRev.current = t.chart.rev

    const snap = tRef.current
    const frame = snap.frame
    if (!frame) return

    // Session reset: onboard clock jumped backwards → clear rows, log a marker,
    // and forget the previous flight's edges so the new flight logs cleanly.
    const prevOn = prevOnboardMs.current
    if (prevOn != null && frame.onboardMs + SESSION_RESET_MS < prevOn) {
      ref.current.rows = []
      prevState.current = null
      prevPyro.current = false
      prevNoDeploy.current = false
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.tPlusSec,
        kind: "session",
        severity: "info",
        message: "SESSION RESET",
        detail: "new flight",
      })
    }

    const row: LogRow = {
      seq: frame.seq,
      onboardMs: frame.onboardMs,
      tPlusSec: snap.tPlusSec,
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
    }
    const rows = ref.current.rows
    rows.unshift(row)
    if (rows.length > MAX_ROWS) rows.length = MAX_ROWS

    const stateName = FLIGHT_STATE_NAME[frame.flightState]
    const altDetail = `${Math.round(frame.baroAltM)} m`

    if (prevState.current === null) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.tPlusSec,
        kind: "session",
        severity: "info",
        message: "SESSION START",
        detail: stateName,
      })
    } else if (prevState.current !== frame.flightState) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.tPlusSec,
        kind: "state",
        severity: "info",
        message: stateName,
        detail: altDetail,
      })
    }

    if (!prevPyro.current && frame.pyroFired) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.tPlusSec,
        kind: "pyro",
        severity: "caution",
        message: "PYRO FIRED",
        detail: stateName,
      })
    }

    if (!prevNoDeploy.current && snap.alarms.noDeploy) {
      pushEvent({
        onboardMs: frame.onboardMs,
        tPlusSec: snap.tPlusSec,
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
      tPlusSec: snap.tPlusSec,
      kind: "link",
      severity,
      message,
    })
    ref.current.rev += 1
    forceRender((n) => n + 1)
  }, [t.link])

  return ref.current
}
