/**
 * KpiRow — the six hero instruments as one segmented panel (ALT / APOGEE /
 * V-SPEED / TILT / VBAT / GPS), divided by 1px hairlines rather than six
 * floating cards, so it reads as an instrument cluster, not a KPI card grid.
 * Calm-until-alarm: only altitude carries the cyan data accent; the rest stay
 * ink until a value goes off-nominal (low battery, high tilt, degraded fix).
 */
import { Card } from "@/components/ui/card"
import { fmtFixed, fmtInt, fmtSigned } from "@/lib/format"
import { FlightState, GpsFix, isDescending, type TelemetryFrame } from "@/lib/protocol"
import { KpiCell, type Tone } from "./KpiCell"

/** 2S LiPo: ink while healthy, warn approaching cutoff, alarm below it. */
function vbatTone(v: number): Tone {
  if (v < 7.0) return "alarm"
  if (v < 7.4) return "caution"
  return "ink"
}

/** Off-vertical: fine on the pad / under chute; concerning during powered flight. */
function tiltTone(deg: number, state: FlightState): Tone {
  const ascent = state === FlightState.BOOST || state === FlightState.COAST
  if (ascent && deg > 30) return "alarm"
  if (ascent && deg > 15) return "caution"
  if (deg > 60 && !isDescending(state) && state !== FlightState.LANDED) return "caution"
  return "ink"
}

/** GPS: ink at a healthy 3D fix, amber at 2D, alarm with no fix. */
function gpsTone(fix: number): Tone {
  if (fix >= GpsFix.FIX_3D) return "ink"
  if (fix === GpsFix.FIX_2D) return "caution"
  return "alarm"
}

const dash = "—"

export function KpiRow({ frame, maxAltM }: { frame: TelemetryFrame | null; maxAltM: number }) {
  return (
    <div className="px-2 pt-2">
      <Card className="flex-row divide-x divide-hairline overflow-hidden">
        <KpiCell label="Altitude" value={frame ? fmtInt(frame.baroAltM) : dash} unit="m" tone="data" />
        <KpiCell label="Apogee" value={fmtInt(maxAltM)} unit="m" />
        <KpiCell label="V-Speed" value={frame ? fmtSigned(frame.vspeedMs, 1) : dash} unit="m/s" />
        <KpiCell
          label="Tilt"
          value={frame ? fmtInt(frame.tiltDeg) : dash}
          unit="°"
          tone={frame ? tiltTone(frame.tiltDeg, frame.flightState) : "ink"}
        />
        <KpiCell
          label="Vbat"
          value={frame ? fmtFixed(frame.vbatV, 1) : dash}
          unit="V"
          tone={frame ? vbatTone(frame.vbatV) : "ink"}
        />
        <KpiCell
          label="GPS"
          value={frame ? fmtInt(frame.gpsSats) : dash}
          unit="sats"
          tone={frame ? gpsTone(frame.gpsFix) : "ink"}
        />
      </Card>
    </div>
  )
}
