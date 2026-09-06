import { act, render, screen } from "@testing-library/react"
import { afterEach, expect, it, vi } from "vitest"
import { ArmingStatus } from "../src/components/ArmingStatus"
import { AuxReadouts } from "../src/components/AuxReadouts"
import { Flag, FlightState, normalizeFrame, type WireFrame } from "../src/lib/protocol"

function frame(extra: Record<string, number> = { AW: 1, AD: 120, AS: 0 }, armed = false) {
  return normalizeFrame({ seq: 1, onboard_ms: 60000, flight_state: armed ? FlightState.ARMED : FlightState.PAD,
    armed, flags_known: Flag.ARMED, extra } as WireFrame)
}
afterEach(() => vi.useRealTimers())

it("does not duplicate encoded arming fields in the general instrument strip", () => {
  render(<AuxReadouts frame={frame()} />)
  expect(screen.queryByText("AW")).toBeNull()
  expect(screen.queryByText("AD")).toBeNull()
  expect(screen.queryByText("AS")).toBeNull()
})

it("shows onboard countdown and separately waits for an actual acknowledgement", () => {
  const { rerender } = render(<ArmingStatus frame={frame()} link="live" />)
  expect(screen.getByText("02:00")).toBeTruthy()
  expect(screen.getByText(/Earliest auto-arm/)).toBeTruthy()
  rerender(<ArmingStatus frame={frame({ AW: 0, AD: 0, AS: 0 })} link="live" />)
  expect(screen.getByRole("status").textContent).toContain("Waiting for ARMED confirmation")
  expect(screen.queryByText("ARMED confirmed")).toBeNull()
  rerender(<ArmingStatus frame={frame({}, true)} link="live" />)
  expect(screen.getByRole("status").textContent).toContain("ARMED confirmed")
})

it("names concurrent blockers and distinguishes delay from stillness", () => {
  render(<ArmingStatus frame={frame({ AW: 15, AD: 12, AS: 10 })} link="live" />)
  const text = screen.getByRole("status").textContent
  expect(text).toContain("Boot delay")
  expect(text).toContain("Stillness")
  expect(text).toContain("Gyro calibration")
  expect(text).toContain("ICM unavailable / stale")
})

it("never guesses a countdown for old, missing, partial or invalid telemetry", () => {
  const { rerender } = render(<ArmingStatus frame={null} link="down" />)
  expect(screen.getByRole("status").textContent).toContain("No telemetry")
  for (const extra of [{}, { AW: 1, AD: 10 }, { AW: 1, AD: NaN, AS: 0 },
    { AW: 1, AD: -1, AS: 0 }, { AW: 0, AD: 20, AS: 0 }, { AW: 256, AD: 0, AS: 0 }]) {
    rerender(<ArmingStatus frame={frame(extra as Record<string, number>)} link="live" />)
    expect(screen.getByRole("status").textContent).toContain("Countdown unavailable")
  }
})

it("does not count down locally, and repeated old data becomes stale even on an open link", () => {
  vi.useFakeTimers()
  const { rerender } = render(<ArmingStatus frame={frame()} link="live" />)
  act(() => vi.advanceTimersByTime(2000))
  expect(screen.getByText("02:00")).toBeTruthy()
  rerender(<ArmingStatus frame={frame()} link="live" />)
  act(() => vi.advanceTimersByTime(1100))
  expect(screen.getByRole("status").textContent).toContain("Telemetry stale")
  expect(screen.queryByText("02:00")).toBeNull()
})

it("marks armed data as stale too and clears the old mission on reset", () => {
  const { rerender } = render(<ArmingStatus frame={frame({}, true)} link="stale" />)
  expect(screen.getByRole("status").textContent).toContain("Telemetry stale")
  expect(screen.queryByText("ARMED confirmed")).toBeNull()
  rerender(<ArmingStatus frame={null} link="down" />)
  expect(screen.getByRole("status").textContent).toContain("No telemetry")
  rerender(<ArmingStatus frame={{ ...frame({ AW: 3, AD: 178, AS: 10 }), onboardMs: 2000 }} link="live" />)
  expect(screen.getByText("02:58")).toBeTruthy()
})

it("never labels a contradictory state and armed flag as confirmed", () => {
  render(<ArmingStatus frame={{ ...frame(), armed: true }} link="live" />)
  expect(screen.getByRole("status").textContent).toContain("State / armed flag mismatch")
})

it("shows disabled, fired and interlock blockers without claiming a deadline", () => {
  render(<ArmingStatus frame={frame({ AW: 16 | 32 | 64 | 128, AD: 0, AS: 0 })} link="live" />)
  const text = screen.getByRole("status").textContent
  expect(text).toContain("Auto-arm blocked")
  expect(text).toContain("Auto-arm disabled")
  expect(text).toContain("Fired latch")
  expect(text).toContain("Arming interlock")
  expect(screen.queryByText("00:00")).toBeNull()
})
