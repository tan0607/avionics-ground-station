import { act, renderHook, waitFor } from "@testing-library/react"
import { afterEach, expect, it, vi } from "vitest"
import { useFlightRecorder } from "../src/hooks/useFlightRecorder"

const idle = { session: "test", recording: false, flight: null, completed: [] }
const response = (body: object, status = 200) => new Response(JSON.stringify(body), { status })
afterEach(() => vi.unstubAllGlobals())

it("reports failed stop so a mission switch cannot continue", async () => {
  vi.stubGlobal("fetch", vi.fn(async (_url, options) =>
    options?.method === "POST" ? response({ error: "disk unavailable" }, 500) : response(idle)))
  const { result } = renderHook(useFlightRecorder)
  await waitFor(() => expect(result.current.status).toBe("ok"))
  let saved
  await act(async () => { saved = await result.current.stop() })
  expect(saved).toBe(false)
  expect(result.current.error).toBe("disk unavailable")
})

it("accepts recording only after a successful POST and ignores an older GET", async () => {
  let finishOldPoll!: (r: Response) => void
  const recording = { ...idle, recording: true }
  let gets = 0
  vi.stubGlobal("fetch", vi.fn((_url, options) => {
    if (options?.method === "POST") return Promise.resolve(response(recording))
    if (++gets === 1) return new Promise<Response>((resolve) => { finishOldPoll = resolve })
    return Promise.resolve(response(recording))
  }))
  const { result } = renderHook(useFlightRecorder)
  let started
  await act(async () => { started = await result.current.start("A2R-test") })
  expect(started).toBe(true)
  expect(result.current.recording).toBe(true)
  await act(async () => finishOldPoll(response(idle)))
  expect(result.current.recording).toBe(true)
})
