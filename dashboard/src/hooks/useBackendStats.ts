/**
 * useBackendStats — polls the backend's /stats endpoint.
 *
 * Lives at app level (not inside the Settings view) because one field on it,
 * `source_error`, is a flight-critical alarm: it is non-null when the backend
 * cannot read the radio at all — the serial port is held by another program
 * (the Arduino IDE's Serial Monitor is the usual culprit), the cable is out, or
 * the port name is wrong.
 *
 * That state used to be invisible here. The WebSocket stays connected and
 * simply carries no frames, so the console showed "NO LINK" — identical to a
 * quiet radio, and identical to a rocket that has genuinely stopped
 * transmitting. Those need different responses from an operator, and only the
 * backend knows which one it is.
 */
import { useEffect, useRef, useState } from "react"
import { apiUrl } from "@/lib/api"

/**
 * What the BACKEND is reading. `kind` is the field that matters: the browser
 * cannot tell a radio from a looping `--replay` on its own — both arrive as
 * frames on the same WebSocket — and for a while the top bar called both of
 * them LIVE. A replayed capture that says LIVE is worse than no dashboard.
 */
export interface BackendSource {
  kind: "serial" | "replay" | "fake" | string
  /** serial: the port being read. */
  port?: string
  /** replay: the raw.log being played back. */
  path?: string
  /** replay/fake: playing continuously. */
  loop?: boolean
}

/**
 * Per-packet radio metrics off the last decoded frame (mrcc.py LinkQuality).
 * Null on the binary format, whose 32-byte frame has no room for them: seq-gap
 * loss really is the only link signal there, so the UI must show nothing rather
 * than a zero that reads as a dead-flat carrier.
 */
export interface BackendLink {
  rssi_dbm: number | null
  snr_db: number | null
  payload_len: number | null
}

export interface BackendStats {
  session: string | null
  format?: string
  /** Absent only if the backend predates this field. */
  source?: BackendSource
  /** Null when the wire format carries no radio metrics. */
  link?: BackendLink | null
  frames_decoded: number
  /** Frames that arrived and failed their integrity check (CRC, or MRCC's length field). */
  crc_errors: number
  /**
   * Non-null when the BYTE SOURCE itself is broken, as opposed to the link being
   * quiet. Null while the port is healthy.
   */
  source_error?: string | null
  /** Flight-phase words the backend's parser did not recognise (format drift). */
  unknown_states?: string[]
  clients: number
  loss_pct: number
}

export type StatsState = {
  status: "loading" | "ok" | "unreachable"
  data: BackendStats | null
}

export function useBackendStats(intervalMs = 2000): StatsState {
  const [state, setState] = useState<StatsState>({ status: "loading", data: null })
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    const poll = async () => {
      try {
        const res = await fetch(apiUrl("/stats"), { cache: "no-store" })
        if (!res.ok) throw new Error(String(res.status))
        const data = (await res.json()) as BackendStats
        if (alive.current) setState({ status: "ok", data })
      } catch {
        // Keep the last good data so the UI can distinguish "backend went away"
        // from "backend never answered".
        if (alive.current) setState((s) => ({ status: "unreachable", data: s.data }))
      }
    }
    poll()
    const id = window.setInterval(poll, intervalMs)
    return () => {
      alive.current = false
      window.clearInterval(id)
    }
  }, [intervalMs])

  return state
}
