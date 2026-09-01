/** Read-only access to the backend's persistent recorded-flight archive. */
import { useCallback, useEffect, useState } from "react"
import { apiUrl } from "@/lib/api"

export interface RecordedFile {
  name: "metadata.json" | "telemetry.csv" | "events.csv" | "mission.log" | "raw.log" | string
  bytes: number
}

export interface RecordedFlight {
  session: string
  flight: string
  path: string
  label: string
  started_utc: string | null
  stopped_utc: string | null
  duration_s: number | null
  stop_reason: string | null
  recording: boolean
  rows: number
  events: number
  raw_bytes: number
  source: Record<string, unknown>
  packet: Record<string, unknown>
  files: RecordedFile[]
}

interface ArchivePayload {
  root: string
  count: number
  flights: RecordedFlight[]
}

export interface FlightPreview {
  lines: string[]
  truncated: boolean
}

export interface TelemetryPreview {
  columns: string[]
  rows: Array<Record<string, string>>
  truncated: boolean
}

export interface RecordedFlightDetail extends RecordedFlight {
  mission: FlightPreview
  telemetry: TelemetryPreview
}

export interface DeleteResult {
  ok: boolean
  error?: string
}

export type ArchiveState = {
  status: "loading" | "ok" | "unreachable"
  data: ArchivePayload | null
  refresh: () => Promise<void>
  /**
   * Permanently delete one recorded flight folder.
   *
   * Returns the backend's refusal rather than throwing, because every refusal
   * here is something the operator needs to read: the flight is still
   * recording, or the folder holds a file this ground station did not write.
   */
  remove: (flight: RecordedFlight) => Promise<DeleteResult>
}

const ARCHIVE_POLL_MS = 5000

export function useRecordedFlights(): ArchiveState {
  const [status, setStatus] = useState<ArchiveState["status"]>("loading")
  const [data, setData] = useState<ArchivePayload | null>(null)

  const refresh = useCallback(async () => {
    try {
      const response = await fetch(apiUrl("/flights"), { cache: "no-store" })
      if (!response.ok) throw new Error(String(response.status))
      setData((await response.json()) as ArchivePayload)
      setStatus("ok")
    } catch {
      setStatus("unreachable")
    }
  }, [])

  const remove = useCallback(
    async (flight: RecordedFlight): Promise<DeleteResult> => {
      const path = `/flights/${encodeURIComponent(flight.session)}/${encodeURIComponent(flight.flight)}`
      try {
        const response = await fetch(apiUrl(path), { method: "DELETE" })
        const body = (await response.json().catch(() => ({}))) as { error?: string }
        if (!response.ok) return { ok: false, error: body.error ?? `HTTP ${response.status}` }
        // Drop it locally straight away: the 5 s poll would otherwise leave a
        // deleted flight on screen long enough to be clicked again.
        setData((prev) =>
          prev
            ? {
                ...prev,
                flights: prev.flights.filter(
                  (f) => !(f.session === flight.session && f.flight === flight.flight),
                ),
                count: Math.max(0, prev.count - 1),
              }
            : prev,
        )
        refresh()
        return { ok: true }
      } catch {
        return { ok: false, error: "backend unreachable" }
      }
    },
    [refresh],
  )

  useEffect(() => {
    refresh()
    const id = window.setInterval(refresh, ARCHIVE_POLL_MS)
    return () => window.clearInterval(id)
  }, [refresh])

  return { status, data, refresh, remove }
}

export type DetailState = {
  status: "idle" | "loading" | "ok" | "error"
  data: RecordedFlightDetail | null
  refresh: () => Promise<void>
}

export function useRecordedFlightDetail(flight: RecordedFlight | null): DetailState {
  const [status, setStatus] = useState<DetailState["status"]>("idle")
  const [data, setData] = useState<RecordedFlightDetail | null>(null)

  const session = flight?.session ?? null
  const name = flight?.flight ?? null
  const refresh = useCallback(async () => {
    if (!session || !name) {
      setStatus("idle")
      setData(null)
      return
    }
    setStatus("loading")
    try {
      const path = `/flights/${encodeURIComponent(session)}/${encodeURIComponent(name)}`
      const response = await fetch(apiUrl(path), { cache: "no-store" })
      if (!response.ok) throw new Error(String(response.status))
      setData((await response.json()) as RecordedFlightDetail)
      setStatus("ok")
    } catch {
      setData(null)
      setStatus("error")
    }
  }, [session, name])

  useEffect(() => {
    refresh()
  }, [refresh])

  return { status, data, refresh }
}

export function recordedFlightFileUrl(flight: RecordedFlight, name: string): string {
  return apiUrl(
    `/flights/${encodeURIComponent(flight.session)}/${encodeURIComponent(flight.flight)}` +
      `/files/${encodeURIComponent(name)}`,
  )
}
