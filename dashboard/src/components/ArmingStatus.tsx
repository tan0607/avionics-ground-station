import { useEffect, useState } from "react"
import { Flag, FlightState, LINK_STALE_MS, hasLaunched, isFlagKnown, type TelemetryFrame } from "@/lib/protocol"
import type { LinkState } from "@/hooks/useTelemetry"

// Flight.h / AW. This describes onboard gates, never recomputes flight logic.
const WAIT = { delay: 1, still: 2, cal: 4, imu: 8, blocked: 16, disabled: 32, fired: 64, interlock: 128 }
const clock = (seconds: number) => `${Math.floor(seconds / 60).toString().padStart(2, "0")}:${(seconds % 60).toString().padStart(2, "0")}`

export function ArmingStatus({ frame, link }: { frame: TelemetryFrame | null; link: LinkState }) {
  const onboardMs = frame?.onboardMs ?? null
  const [lastProgress, setLastProgress] = useState(() => Date.now())
  const [now, setNow] = useState(() => Date.now())
  // Repeated radio copies / a backend replaying one cached frame cannot keep
  // an old arming report fresh. Do NOT decrement onboard values on this clock.
  useEffect(() => { setLastProgress(Date.now()); setNow(Date.now()) }, [onboardMs])
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 250)
    return () => window.clearInterval(timer)
  }, [])

  let title = "No telemetry"
  let detail = "Waiting for the selected vehicle."
  let remaining: number | null = null
  let confirmed = false
  let gates: { label: string; value: string }[] = []
  if (frame) {
    const stale = link !== "live" || now - lastProgress >= LINK_STALE_MS
    const known = isFlagKnown(frame, Flag.ARMED)
    if (stale) {
      title = "Telemetry stale"
      detail = "Arming status unknown — countdown suspended."
    } else if (hasLaunched(frame.flightState)) {
      title = "Flight sequence active"
      detail = "Prelaunch countdown ended. Follow the reported flight state."
    } else if (known && frame.armed !== (frame.flightState === FlightState.ARMED)) {
      title = "State / armed flag mismatch"
      detail = "Arming status cannot be confirmed."
    } else if (known && frame.armed && frame.flightState === FlightState.ARMED) {
      title = "ARMED confirmed"
      detail = "Onboard state + armed flag received. Waiting for launch."
      confirmed = true
    } else {
      const { AW: wait, AD: delay, AS: still } = frame.extra
      const valid = known && frame.flightState === FlightState.PAD &&
        [wait, delay, still].every(n => Number.isInteger(n) && n >= 0) &&
        wait <= 255 && delay <= 65535 && still <= 65535 &&
        Boolean(wait & WAIT.delay) === (delay > 0) && Boolean(wait & WAIT.still) === (still > 0)
      if (!valid) {
        title = "Countdown unavailable"
        detail = "Missing or invalid onboard arming report; older firmware may not support it."
      } else {
        title = wait === 0 ? "Waiting for ARMED confirmation" : "Earliest auto-arm"
        detail = "Last onboard report. Time alone does not arm the vehicle."
        if (wait & ~(WAIT.delay | WAIT.still)) title = "Auto-arm inhibited"
        else if (wait !== 0) remaining = Math.max(delay, still)
        gates = [
          { label: "Boot delay", value: delay ? `${delay} s remaining` : "Complete" },
          { label: "Stillness", value: still ? `${still} s remaining · resets on motion` : "Complete" },
          { label: "Gyro calibration", value: wait & WAIT.cal ? "Waiting / retrying" : "Complete" },
          ...(wait & WAIT.imu ? [{ label: "ICM unavailable / stale", value: "Required to arm" }] : []),
          ...(wait & WAIT.blocked ? [{ label: "Auto-arm blocked", value: "Disarmed by operator" }] : []),
          ...(wait & WAIT.disabled ? [{ label: "Auto-arm disabled", value: "Firmware setting" }] : []),
          ...(wait & WAIT.fired ? [{ label: "Fired latch", value: "Re-arming blocked" }] : []),
          ...(wait & WAIT.interlock ? [{ label: "Arming interlock", value: "Not satisfied" }] : []),
        ]
      }
    }
  }

  return (
    <section role="status" aria-live="polite" aria-label="Auto-arm status" className="flex flex-wrap items-baseline gap-x-6 gap-y-1 border border-hairline bg-surface px-3 py-2">
      <div className={`flex flex-wrap items-baseline gap-x-3 gap-y-1 text-sm font-medium ${confirmed ? "text-caution" : "text-ink"}`}>
        <span>{title}</span>
        {remaining != null && <span className="font-mono tabular-nums">{clock(remaining)}</span>}
      </div>
      {gates.length > 0 && <dl className="flex flex-wrap gap-x-5 gap-y-1 text-xs">
        {gates.map(({ label, value }) => <div key={label} className="flex flex-wrap gap-x-2">
          <dt className="text-ink-dim">{label}</dt><dd className="text-ink text-right tabular-nums">{value}</dd>
        </div>)}
      </dl>}
      <p className="basis-full text-xs leading-relaxed text-ink-dim">{detail}</p>
    </section>
  )
}
