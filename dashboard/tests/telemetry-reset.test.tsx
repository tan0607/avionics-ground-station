import { act, renderHook } from "@testing-library/react"
import { afterEach, expect, it, vi } from "vitest"
import { useTelemetry } from "../src/hooks/useTelemetry"
import { useSensorSeries } from "../src/hooks/useSensorSeries"
import { useOnboardLog } from "../src/hooks/useOnboardLog"

const socket = vi.hoisted(() => ({ onMessage: null as null | ((frame: unknown) => void), starts: 0 }))
vi.mock("@/lib/wsClient", () => ({
  TelemetrySocket: class {
    constructor(options: { onMessage: (frame: unknown) => void }) { socket.onMessage = options.onMessage }
    start() { socket.starts++ }
    stop() {}
  },
}))
afterEach(() => { window.history.replaceState({}, "", "/"); socket.starts = 0 })
function send(seq: number, ms: number, alt: number) {
  act(() => socket.onMessage?.({ seq, onboard_ms: ms, baro_alt_m: alt, vspeed_ms: 5,
    flight_state: 1, host_time_ms: Date.now(), extra: { AX: 1, AY: 2, AZ: 9.8, SDF: 1, SDL: 50, SDE: 0 } }))
}

it("clears altitude, sensor histories, maxima and latched SD data without reconnecting", () => {
  window.history.replaceState({}, "", "/?source=ws")
  const { result, rerender } = renderHook(({ mission, accept }) => {
    const telemetry = useTelemetry(mission, accept)
    return { telemetry, sensors: useSensorSeries(telemetry), onboard: useOnboardLog(telemetry) }
  }, { initialProps: { mission: "A1R", accept: true } })
  send(10, 1000, 100)
  send(11, 1500, 200)
  expect(result.current.telemetry.chart.ys).toEqual([100, 200])
  expect(result.current.sensors.vspeed).toHaveLength(2)
  expect(result.current.onboard.lines).toBe(50)
  rerender({ mission: "A2R", accept: false })
  expect(result.current.telemetry.chart.xs).toEqual([])
  expect(result.current.telemetry.chart.apogee).toBeNull()
  expect(result.current.telemetry.frame).toBeNull()
  expect(result.current.telemetry.maxAltM).toBe(0)
  expect(result.current.telemetry.tPlusSec).toBeNull()
  expect(result.current.sensors.vspeed).toEqual([])
  expect(result.current.sensors.az).toEqual([])
  expect(result.current.onboard.lines).toBeNull()
  send(12, 2000, 999) // old channel can still arrive before the receiver confirms B
  expect(result.current.telemetry.chart.xs).toEqual([])
  rerender({ mission: "A2R", accept: true })
  send(11, 30000, 3) // new vehicle: same seq, later uptime (no reboot heuristic)
  expect(result.current.telemetry.chart.ys).toEqual([3])
  expect(result.current.sensors.vspeed).toEqual([5])
  expect(result.current.telemetry.lossFraction).toBe(0)
  expect(socket.starts).toBe(1)
  rerender({ mission: "A1R", accept: false })
  expect(result.current.telemetry.chart.xs).toEqual([])
})
