/**
 * useSensorSeries — companion accumulator for the secondary sensor charts
 * (vertical speed, tilt). It deliberately does NOT touch useTelemetry (that
 * hook's ref+rev architecture is frozen); instead it piggybacks on the altitude
 * chart's `rev` — one bump per ingested frame — and appends the same frame's
 * other channels, sharing the altitude x-axis by reference so every series
 * stays index-aligned. Reset and the 4000-point cap are mirrored from the
 * altitude series (a shorter series length ⇒ session reset; equal length on a
 * new frame ⇒ the altitude buffer rotated at the cap, so rotate too).
 */
import { useEffect, useRef, useState } from "react"
import type { TelemetryState } from "./useTelemetry"

export interface SensorSeries {
  xs: number[] // shared onboard-seconds axis (same array as the altitude chart)
  vspeed: number[]
  tilt: number[]
  rev: number
}

export function useSensorSeries(t: TelemetryState): SensorSeries {
  const ref = useRef<SensorSeries>({ xs: [], vspeed: [], tilt: [], rev: 0 })
  const lastRev = useRef(-1)
  const [, forceRender] = useState(0)

  useEffect(() => {
    // one bump per ingested frame; the guard makes it idempotent under StrictMode
    if (t.chart.rev === lastRev.current) return
    lastRev.current = t.chart.rev

    const s = ref.current
    const xs = t.chart.xs
    const frame = t.frame
    if (!frame) return

    if (xs.length < s.vspeed.length) {
      // session reset — the altitude series was cleared
      s.vspeed.length = 0
      s.tilt.length = 0
    }

    if (xs.length > s.vspeed.length) {
      // normal case: append until aligned with the altitude series (usually +1)
      while (s.vspeed.length < xs.length) {
        s.vspeed.push(frame.vspeedMs)
        s.tilt.push(frame.tiltDeg)
      }
    } else if (xs.length === s.vspeed.length && s.vspeed.length > 0) {
      // at the point cap the altitude buffer rotates in place (shift+push) —
      // rotate ours the same way so the x-axis and channels stay aligned
      s.vspeed.shift()
      s.vspeed.push(frame.vspeedMs)
      s.tilt.shift()
      s.tilt.push(frame.tiltDeg)
    }

    s.xs = xs
    s.rev += 1
    forceRender((n) => n + 1)
  }, [t.chart.rev, t.frame, t.chart])

  return ref.current
}
