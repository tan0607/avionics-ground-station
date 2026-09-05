/**
 * useFlightRecorder — the operator's RECORD control for a flight folder.
 *
 * The backend records continuously into flights/<session>/ and always has. What
 * it could not do was tell one flight from the next: the session boundary is
 * the backend PROCESS lifetime, so restarting the server split one flight
 * across folders and leaving it up merged every flight into one file (one bench
 * session held 21 hours, 236,950 rows and 394 transmitter reboots in a single
 * telemetry.csv).
 *
 * Neither candidate auto-rule survives contact with this vehicle. Rolling on
 * the onboard clock jumping back would have cut 394 folders out of that same
 * session, and rolling on PAD → BOOST never fires at all on a board that stays
 * in PAD. So the operator says when, and the always-on session recorder
 * underneath means saying it late — or forgetting entirely — costs a tidy
 * folder and never the data.
 *
 * Start/stop are POSTs with an application/json content type. That header is
 * load-bearing: it forces a CORS preflight, so a page on any other origin
 * cannot reach across and stop a recording mid-flight (backend/app.py
 * _json_body explains the rest).
 */
import { useCallback, useEffect, useRef, useState } from "react"
import { apiUrl } from "@/lib/api"

/** One flight folder, as the backend reports it. */
export interface FlightStatus {
  /** Folder name inside the session: "flight-01" or "flight-02_launch-a". */
  flight: string
  index: number
  label: string
  started_utc: string
  elapsed_s: number
  rows: number
  events: number
  raw_bytes: number
  recording: boolean
  stop_reason: string | null
}

interface FlightPayload {
  session: string | null
  recording: boolean
  flight: FlightStatus | null
  completed: FlightStatus[]
}

export interface RecorderState extends FlightPayload {
  status: "loading" | "ok" | "unreachable"
  /** A start/stop is in flight — the button disables itself rather than queueing. */
  busy: boolean
  /** Last failure, shown next to the control. Cleared by the next success. */
  error: string | null
  start: (label?: string) => Promise<boolean>
  stop: () => Promise<boolean>
}

const EMPTY: FlightPayload = { session: null, recording: false, flight: null, completed: [] }
const POLL_MS = 2000

export function useFlightRecorder(): RecorderState {
  const [payload, setPayload] = useState<FlightPayload>(EMPTY)
  const [status, setStatus] = useState<RecorderState["status"]>("loading")
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const alive = useRef(true)
  const mutating = useRef(false)
  const requestVersion = useRef(0)

  const refresh = useCallback(async () => {
    if (mutating.current) return
    const version = ++requestVersion.current
    try {
      const res = await fetch(apiUrl("/flight"), { cache: "no-store" })
      // 503 = the server is up but has no session yet. That is a real answer,
      // not an unreachable backend, and its body is already the empty shape.
      if (!res.ok && res.status !== 503) throw new Error(String(res.status))
      const data = (await res.json()) as FlightPayload
      if (alive.current && version === requestVersion.current) {
        setPayload(data)
        setStatus("ok")
      }
    } catch {
      if (alive.current && version === requestVersion.current) setStatus("unreachable")
    }
  }, [])

  useEffect(() => {
    alive.current = true
    refresh()
    const id = window.setInterval(refresh, POLL_MS)
    return () => {
      alive.current = false
      window.clearInterval(id)
    }
  }, [refresh])

  const post = useCallback(
    async (path: string, body: object) => {
      if (mutating.current) return false
      mutating.current = true
      ++requestVersion.current // invalidate polls that started before this action
      setBusy(true)
      try {
        const res = await fetch(apiUrl(path), {
          method: "POST",
          // Not optional — see the module note. Dropping it turns these into
          // "simple requests" that skip the browser's cross-origin check.
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        })
        const data = await res.json().catch(() => ({}))
        if (!res.ok) {
          setError(typeof data?.error === "string" ? data.error : `HTTP ${res.status}`)
          return false
        } else {
          setError(null)
          // The response IS the new status, so the button settles immediately
          // instead of waiting out the poll.
          if (alive.current) {
            setPayload(data as FlightPayload)
            setStatus("ok")
          }
          return true
        }
      } catch {
        setError("backend unreachable")
        return false
      } finally {
        mutating.current = false
        setBusy(false)
        refresh()
      }
    },
    [refresh],
  )

  const start = useCallback(
    (label?: string) => post("/flight/start", label ? { label } : {}),
    [post],
  )
  const stop = useCallback(() => post("/flight/stop", {}), [post])

  return { ...payload, status, busy, error, start, stop }
}

/**
 * Seconds since a flight started, ticking locally.
 *
 * The backend reports `elapsed_s` at poll time, which would step in 2-second
 * jumps on screen — a recording timer that visibly stutters reads as a
 * recording that is stuttering. Derived from the start timestamp instead, which
 * is a fixed point both sides agree on.
 */
export function recordingElapsedSec(flight: FlightStatus | null, nowMs: number): number {
  if (!flight) return 0
  const started = Date.parse(flight.started_utc)
  if (Number.isNaN(started)) return flight.elapsed_s
  return Math.max(0, (nowMs - started) / 1000)
}
