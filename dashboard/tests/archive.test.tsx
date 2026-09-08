import { act, fireEvent, render, renderHook, screen, waitFor, within } from "@testing-library/react"
import { afterEach, expect, it, vi } from "vitest"
import { FlightsView } from "../src/components/FlightsView"
import { useRecordedFlightDetail, type RecordedFlight } from "../src/hooks/useRecordedFlights"

const row = (session: string, extra = {}): RecordedFlight => ({
  session, flight: "-", kind: "session", flight_count: 0, path: session,
  label: session, started_utc: null, stopped_utc: null, duration_s: 0,
  stop_reason: null, recording: false, rows: 12, events: 0, raw_bytes: 100,
  source: {}, packet: {}, provenance: { kind: "live", detail: "", origin: null }, files: [], ...extra,
})
const detail = (r: RecordedFlight) => ({ ...r, mission: { lines: [], truncated: false }, telemetry: { columns: [], rows: [], truncated: false } })
const response = (body: object) => new Response(JSON.stringify(body))
afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers() })

it("keeps zero-frame recordings visible with their provenance and preserves selection on insertion", async () => {
  let sessions = [row("older")]
  vi.stubGlobal("fetch", vi.fn(async (url) => String(url).endsWith("/flights")
    ? response({ sessions, flights: [], root: "test" }) : response(detail(sessions[0]))))
  render(<FlightsView />)
  const older = await screen.findByRole("button", { name: /older.*12 rows/ })
  await waitFor(() => expect(older.getAttribute("aria-pressed")).toBe("true"))
  sessions = [row("new recording", { rows: 0, recording: true }), ...sessions]
  fireEvent.click(screen.getByRole("button", { name: "Refresh archive" }))
  const active = await screen.findByRole("button", { name: /new recording.*rec/i })
  expect(within(active).getByText("LIVE")).toBeTruthy()
  expect(older.getAttribute("aria-pressed")).toBe("true")
})

it("refreshes detail after summary changes and retains content during background reads", async () => {
  let resolve!: (response: Response) => void
  let calls = 0
  vi.stubGlobal("fetch", vi.fn(() => ++calls === 1
    ? Promise.resolve(response(detail(row("current", { recording: true }))))
    : new Promise<Response>((r) => { resolve = r })))
  const { result, rerender } = renderHook(({ flight }) => useRecordedFlightDetail(flight), {
    initialProps: { flight: row("current", { recording: true }) },
  })
  await waitFor(() => expect(result.current.status).toBe("ok"))
  rerender({ flight: row("current", { recording: false, rows: 25 }) })
  await waitFor(() => expect(calls).toBe(2))
  expect(result.current.status).toBe("ok")
  expect(result.current.data?.rows).toBe(12)
  await act(async () => resolve(response(detail(row("current", { rows: 25 })))))
  expect(result.current.data?.rows).toBe(25)
})
