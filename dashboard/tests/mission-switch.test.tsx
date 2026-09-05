import { act, renderHook } from "@testing-library/react"
import { describe, expect, it, vi } from "vitest"
import { useMission } from "../src/hooks/useMission"
import type { GroundStation } from "../src/hooks/useGroundStation"

function station(channel = "A"): GroundStation {
  return { channel, channels: ["A", "B"], supported: true, reason: null, error: null,
    busy: false, lastError: null, setChannel: vi.fn(async () => {}), refresh: vi.fn() }
}

describe("mission switch", () => {
  it("requests confirmation; cancel preserves the mission and sends no channel command", () => {
    const gs = station()
    const { result } = renderHook(() => useMission(gs))
    act(() => result.current.select("A2R"))
    expect(result.current.mission.name).toBe("A1R")
    expect(result.current.requested?.name).toBe("A2R")
    expect(gs.setChannel).not.toHaveBeenCalled()
    act(() => result.current.cancel())
    expect(result.current.requested).toBeNull()
    expect(result.current.mission.name).toBe("A1R")
  })

  it("finishes the current recording before switching and waits for receiver confirmation", async () => {
    const gs = station()
    const before = vi.fn(async () => true)
    const { result, rerender } = renderHook(({ gs }) => useMission(gs, before), { initialProps: { gs } })
    act(() => result.current.select("A2R"))
    await act(() => result.current.confirm())
    expect(before).toHaveBeenCalledOnce()
    expect(gs.setChannel).toHaveBeenCalledWith("B")
    expect(result.current.mission.name).toBe("A2R")
    expect(result.current.acceptTelemetry).toBe(false)
    rerender({ gs: { ...gs, channel: "B" } })
    expect(result.current.acceptTelemetry).toBe(true)
  })

  it("does not switch or clear the display if saving the recording fails", async () => {
    const gs = station()
    const { result } = renderHook(() => useMission(gs, async () => false))
    act(() => result.current.select("A2R"))
    await act(() => result.current.confirm())
    expect(result.current.mission.name).toBe("A1R")
    expect(result.current.requested?.name).toBe("A2R")
    expect(gs.setChannel).not.toHaveBeenCalled()
  })

  it("switches back through confirmation, and ignores reselecting the current mission", async () => {
    const gs = station("B")
    const { result } = renderHook(() => useMission(gs))
    act(() => result.current.select("A2R"))
    expect(result.current.requested).toBeNull()
    act(() => result.current.select("A1R"))
    await act(() => result.current.confirm())
    expect(gs.setChannel).toHaveBeenCalledWith("A")
  })
})

it("returns keyboard focus to the picker after cancellation", () => {
  const picker = document.createElement("button")
  const other = document.createElement("button")
  document.body.append(picker, other)
  const { result } = renderHook(() => useMission(station()))
  act(() => result.current.select("A2R", picker))
  other.focus()
  act(() => result.current.cancel())
  expect(document.activeElement).toBe(picker)
  picker.remove()
  other.remove()
})
