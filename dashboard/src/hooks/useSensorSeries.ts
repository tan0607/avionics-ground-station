/**
 * useSensorSeries — companion accumulator for the secondary sensor charts
 * (vertical speed, tilt, body accelerations). It deliberately does NOT touch
 * useTelemetry (that hook's ref+rev architecture is frozen); instead it
 * piggybacks on the altitude chart's `rev` — one bump per ingested frame — and
 * appends the same frame's other channels, sharing the altitude x-axis by
 * reference so every series stays index-aligned. Reset and the 4000-point cap
 * are mirrored from the altitude series (a shorter series length ⇒ session
 * reset; equal length on a new frame ⇒ the altitude buffer rotated at the cap,
 * so rotate too).
 *
 * Channels are `(number | null)[]`, and null means "this frame carried no
 * reading" — uPlot draws a gap. That distinction matters here more than in most
 * charts: the MRCC downlink sends AX=AY=AZ=0.00 whenever the IMU is not
 * answering (28% of frames in the 2026-08-19 bench log), and tilt derived from
 * a dead accelerometer is 0°, which is also exactly what a perfectly upright
 * vehicle reads. Plotting that as 0 draws a confident flat line through the
 * period where the ground station in fact knew nothing. So both tilt and the
 * accel axes go null whenever the frame reports the IMU as known-bad.
 */
import { useEffect, useRef, useState } from "react"
import { Health, isHealthy, isKnown, type TelemetryFrame } from "@/lib/protocol"
import type { TelemetryState } from "./useTelemetry"

export interface SensorSeries {
  xs: number[] // shared onboard-seconds axis (same array as the altitude chart)
  vspeed: (number | null)[]
  tilt: (number | null)[]
  /** Body-frame accelerations, m/s². From `extra` — the protocol has no slot. */
  ax: (number | null)[]
  ay: (number | null)[]
  az: (number | null)[]
  rev: number
}

/** Every channel that tracks `xs`, so append/rotate/reset stay in one list. */
const CHANNELS = ["vspeed", "tilt", "ax", "ay", "az"] as const
type Channel = (typeof CHANNELS)[number]

/** The frame's contribution to each channel, or null where it has no reading. */
function sample(frame: TelemetryFrame): Record<Channel, number | null> {
  // Known-bad IMU: the axes it reports are zeros from a sensor that is not
  // answering, and the tilt derived from them is a fabricated 0°.
  const imuDead = isKnown(frame, Health.IMU) && !isHealthy(frame, Health.IMU)
  const axis = (key: string) =>
    imuDead || typeof frame.extra[key] !== "number" ? null : frame.extra[key]

  return {
    vspeed: frame.vspeedMs,
    tilt: imuDead ? null : frame.tiltDeg,
    ax: axis("AX"),
    ay: axis("AY"),
    az: axis("AZ"),
  }
}

export function useSensorSeries(t: TelemetryState): SensorSeries {
  const ref = useRef<SensorSeries>({
    xs: [], vspeed: [], tilt: [], ax: [], ay: [], az: [], rev: 0,
  })
  const lastRev = useRef(-1)
  const [, forceRender] = useState(0)

  // Read at RENDER time. `t.chart` is a live mutable ref object, so reading
  // `t.chart.rev` inside the effect below would see whatever had been ingested
  // by the time effects flush — not the frame this render is carrying. Marking
  // that newer rev consumed then drops every frame in between.
  const rev = t.chart.rev

  useEffect(() => {
    // one bump per ingested frame; the guard makes it idempotent under StrictMode
    if (rev === lastRev.current) return
    lastRev.current = rev

    const s = ref.current
    const xs = t.chart.xs
    const frame = t.frame
    if (!frame) return

    const next = sample(frame)
    // vspeed is the length reference: every channel is appended in lockstep.
    const have = s.vspeed.length

    if (xs.length < have) {
      // session reset — the altitude series was cleared
      for (const c of CHANNELS) s[c].length = 0
    }

    if (xs.length > s.vspeed.length) {
      // Normally there is exactly one new slot and it is this frame's. More
      // than one means frames were ingested between this render and this
      // effect: their aux readings were never in a rendered frame and are gone.
      // Those slots get null — a gap — rather than a copy of this frame, which
      // would draw a confident flat line through samples the ground station
      // never saw. That is the same lie the dead-IMU nulls above exist to stop.
      //
      // This frame's own slot is found by its onboard time instead of assumed
      // to be the last one, so a reading can never land on another frame's x.
      const tSec = frame.onboardMs / 1000
      let at = xs.length - 1
      while (at > 0 && xs[at] > tSec) at -= 1

      while (s.vspeed.length < at) {
        for (const c of CHANNELS) s[c].push(null)
      }
      if (s.vspeed.length === at) {
        for (const c of CHANNELS) s[c].push(next[c])
      }
      while (s.vspeed.length < xs.length) {
        for (const c of CHANNELS) s[c].push(null)
      }
    } else if (xs.length === s.vspeed.length && s.vspeed.length > 0) {
      // at the point cap the altitude buffer rotates in place (shift+push) —
      // rotate ours the same way so the x-axis and channels stay aligned
      for (const c of CHANNELS) {
        s[c].shift()
        s[c].push(next[c])
      }
    }

    s.xs = xs
    s.rev += 1
    forceRender((n) => n + 1)
  }, [rev, t.frame, t.chart])

  return ref.current
}
