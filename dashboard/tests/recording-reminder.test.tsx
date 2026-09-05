import { fireEvent, render, screen } from "@testing-library/react"
import { expect, it, vi } from "vitest"
import { RecordingReminder } from "../src/components/RecordingReminder"
import type { RecorderState } from "../src/hooks/useFlightRecorder"

const recorder: RecorderState = { session: "test", recording: false, flight: null,
  completed: [], status: "ok", busy: false, error: null,
  start: vi.fn(async () => true), stop: vi.fn(async () => true) }

it("keeps an actionable reminder visible until backend recording is confirmed", () => {
  const start = vi.fn()
  const { rerender } = render(<RecordingReminder liveSource recorder={recorder} mission="A1R" onStart={start} />)
  expect(screen.getByRole("status").textContent).toContain("Not recording a flight")
  fireEvent.click(screen.getByRole("button", { name: "Start recording A1R" }))
  expect(start).toHaveBeenCalledOnce()
  rerender(<RecordingReminder liveSource recorder={{ ...recorder, busy: true }} mission="A1R" onStart={start} />)
  expect(screen.getByRole("status")).toBeTruthy()
  expect(screen.getByRole("button").hasAttribute("disabled")).toBe(true)
  rerender(<RecordingReminder liveSource recorder={{ ...recorder, recording: true }} mission="A1R" onStart={start} />)
  expect(screen.queryByRole("status")).toBeNull()
})

it("does not nag simulations, but warns when recording status cannot be verified", () => {
  const { rerender } = render(<RecordingReminder liveSource={false} recorder={recorder} mission="A2R" onStart={() => {}} />)
  expect(screen.queryByRole("status")).toBeNull()
  rerender(<RecordingReminder liveSource recorder={{ ...recorder, recording: true, status: "unreachable" }} mission="A2R" onStart={() => {}} />)
  expect(screen.getByRole("status").textContent).toContain("Recording status unavailable")
})

it("shows a start failure and never claims the flight is recording", () => {
  render(<RecordingReminder liveSource recorder={{ ...recorder, error: "disk unavailable" }} mission="A2R" onStart={() => {}} />)
  expect(screen.getByRole("status").textContent).toContain("disk unavailable")
  expect(screen.getByRole("button", { name: "Start recording A2R" })).toBeTruthy()
})

it("disables the top-bar REC until the receiver has confirmed the new vehicle", async () => {
  const { TopBar } = await import("../src/components/TopBar")
  const state = { source: "ws", status: "open", link: "live", linkAgeMs: 0,
    frame: null, tPlusSec: null, frameTPlusSec: null,
    lossFraction: 0, lineHz: 2, frameHz: 2, maxAltM: 0,
    chart: { xs: [], ys: [], apogee: null, liftoffT: null, landedT: null, rev: 0 },
    alarms: { linkStale: false, noDeploy: false } } as const
  const mission = { missions: [], mission: { name: "A2R", channel: "B" },
    confirmed: { name: "A1R", channel: "A" }, pending: true, supported: true,
    busy: false, requested: null, acceptTelemetry: false, select: vi.fn(), cancel: vi.fn(), confirm: vi.fn() } as const
  render(<TopBar state={state} recorder={recorder} mission={mission} />)
  expect(screen.getByRole("button", { name: "REC" }).hasAttribute("disabled")).toBe(true)
})
