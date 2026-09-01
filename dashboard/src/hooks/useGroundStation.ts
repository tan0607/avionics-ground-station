/**
 * useGroundStation — which channel the receiver box is listening to, and the
 * control to change it.
 *
 * WHY THIS POLLS RATHER THAN TRACKING WHAT IT SENT. Two rockets fly on two
 * channels (see firmware/README.md), and a receiver on the wrong one is not
 * weak or garbled — it is silent. So the console must never draw a channel it
 * merely asked for. `setChannel` sends the same key an operator would type at
 * the serial monitor and reports nothing back; the backend learns the real
 * channel from the box's own announcements and serves it on /gs. A command that
 * is written but never acted on therefore shows up as the channel simply not
 * changing, which is the truth.
 *
 * `channel` is null until the box has said something — "not heard from yet",
 * never "probably A".
 */
import { useCallback, useEffect, useRef, useState } from "react"
import { apiUrl } from "@/lib/api"

export type GroundStationState = {
  channel: string | null
  channels: string[]
  supported: boolean
  reason: string | null
  error: string | null
}

const EMPTY: GroundStationState = {
  channel: null,
  channels: ["A", "B"],
  supported: false,
  reason: null,
  error: null,
}

// Slow on purpose: the channel changes a handful of times a launch day, and
// this shares a backend with a live telemetry socket.
const POLL_MS = 3000

export function useGroundStation() {
  const [state, setState] = useState<GroundStationState>(EMPTY)
  const [busy, setBusy] = useState(false)
  const [lastError, setLastError] = useState<string | null>(null)
  const alive = useRef(true)

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(apiUrl("/gs"), { cache: "no-store" })
      if (!res.ok) throw new Error(String(res.status))
      const data = (await res.json()) as GroundStationState
      if (alive.current) setState(data)
    } catch {
      // A backend that is not there is already obvious from the connection
      // card; do not stack a second red banner on top of it.
      if (alive.current) setState((s) => ({ ...s, supported: false }))
    }
  }, [])

  useEffect(() => {
    alive.current = true
    void refresh()
    const id = setInterval(() => void refresh(), POLL_MS)
    return () => {
      alive.current = false
      clearInterval(id)
    }
  }, [refresh])

  const setChannel = useCallback(
    async (channel: string) => {
      setBusy(true)
      setLastError(null)
      try {
        const res = await fetch(apiUrl("/gs/channel"), {
          method: "POST",
          // Not optional — the same reasoning as useFlightRecorder. Dropping it
          // makes this a "simple request" that skips the browser's cross-origin
          // check.
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ channel }),
        })
        if (!res.ok) {
          const body = (await res.json().catch(() => ({}))) as { error?: string }
          throw new Error(body.error || `HTTP ${res.status}`)
        }
        // Give the box a beat to retune and announce, then read the truth.
        setTimeout(() => void refresh(), 600)
      } catch (e) {
        if (alive.current) setLastError(e instanceof Error ? e.message : String(e))
      } finally {
        if (alive.current) setBusy(false)
      }
    },
    [refresh],
  )

  return { ...state, busy, lastError, setChannel, refresh }
}
