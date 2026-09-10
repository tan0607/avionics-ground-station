import { render, screen } from "@testing-library/react"
import { expect, it } from "vitest"
import { SubsystemHealth } from "../src/components/SubsystemHealth"
import { FlightState, Health, normalizeFrame, type WireFrame } from "../src/lib/protocol"
import { UNFITTED_PERIPHERALS, isUnfitted } from "../src/lib/mission"
import type { OnboardLog } from "../src/hooks/useOnboardLog"

const ALL = Health.BARO | Health.IMU | Health.GPS | Health.SD | Health.PYRO | Health.VBAT

/** A frame reporting every peripheral, with the named ones down. */
function frame(...down: number[]) {
  const bad = down.reduce((a, b) => a | b, 0)
  return normalizeFrame({ seq: 1, onboard_ms: 1000, flight_state: FlightState.PAD,
    health: ALL & ~bad, health_known: ALL } as WireFrame)
}

/** A recorder that has reported and is writing nothing — the failed-card case. */
const REC: OnboardLog = {
  fileIndex: 0, fileName: null, lines: 0, errors: 0,
  rateHz: null, reportedAt: Date.now(), stalled: true,
}

it("draws a peripheral this flight is not carrying as NOT USED, not as a fault", () => {
  expect(isUnfitted("SD")).toBe(true)   // the launch this was written for
  render(<SubsystemHealth frame={frame(Health.SD)} onboardLog={REC} />)

  // Read the SD row itself rather than the panel: OK appears three times over
  // on the peripherals that ARE fitted, and what matters is which word landed
  // on this one. LOST is what the health bit says, NOT WRITING is what the
  // recorder says, and OK would be a claim about hardware reporting failure.
  expect(screen.getByText("SD").parentElement!.textContent).toBe("SDNOT USED")
  expect(screen.queryByText("LOST")).toBeNull()
  expect(screen.queryByText("NOT WRITING")).toBeNull()
  // Not counted, not blamed for a stale readout, and no recorder footer.
  expect(screen.queryByText(/down$/i)).toBeNull()
  expect(screen.queryByText(/Stale:/)).toBeNull()
  expect(screen.queryByText(/^REC/)).toBeNull()
})

it("still reports every peripheral that IS fitted, and counts only those", () => {
  render(<SubsystemHealth frame={frame(Health.SD, Health.BARO)} onboardLog={REC} />)

  expect(screen.getByText("LOST")).toBeTruthy()          // BARO, which is fitted
  expect(screen.getByText("1 down")).toBeTruthy()        // BARO alone — not 2
  expect(screen.getByText(/Stale: Altitude$/)).toBeTruthy()   // not "· Onboard log"
})

it("is a declaration, never inferred from the health bit", () => {
  // A fitted peripheral that fails must still be able to say so. If this ever
  // reads true for a name not written down in mission.ts, the panel has started
  // hiding faults instead of hiding absences.
  expect(isUnfitted("BARO")).toBe(false)
  render(<SubsystemHealth frame={frame(Health.BARO)} onboardLog={REC} />)
  expect(screen.getByText("LOST")).toBeTruthy()
  expect(screen.getByText("1 down")).toBeTruthy()
  expect(UNFITTED_PERIPHERALS).not.toContain("BARO")
})
