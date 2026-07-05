/**
 * Readouts — the right-hand instrument column. Five hero numbers
 * (ALT / VSPEED / MAX ALT / VBAT / TILT) stacked with 1px dividers, then a
 * go/no-go strip. Off-nominal values (low battery, high tilt) tint their own
 * cell; everything else stays calm ink until it isn't (calm-until-alarm).
 */
import { fmtFixed, fmtInt, fmtSigned } from "@/lib/format"
import { FlightState, isDescending, type TelemetryFrame } from "@/lib/protocol"
import { Readout, type Tone } from "./Readout"
import { StatusLight, type LightState } from "./StatusLight"

/** 2S LiPo: nominal ~7.4–8.4 V. Warn approaching cutoff, alarm below it. */
function vbatTone(v: number): Tone {
  if (v < 7.0) return "alarm"
  if (v < 7.4) return "caution"
  return "nominal"
}

/** Off-vertical: fine on the pad and under chute; concerning during powered flight. */
function tiltTone(deg: number, state: FlightState): Tone {
  const ascent = state === FlightState.BOOST || state === FlightState.COAST
  if (ascent && deg > 30) return "alarm"
  if (ascent && deg > 15) return "caution"
  if (deg > 60 && !isDescending(state) && state !== FlightState.LANDED) return "caution"
  return "ink"
}

const dash = "—"

export function Readouts({ frame, maxAltM }: { frame: TelemetryFrame | null; maxAltM: number }) {
  const cont: LightState = frame ? (frame.continuity ? "go" : "nogo") : "idle"
  const armed: LightState = frame?.armed ? "go" : "idle"
  const sd: LightState = frame ? (frame.sdOk ? "go" : "nogo") : "idle"
  const pyro: LightState = frame?.pyroFired ? "caution" : "idle"

  return (
    <div className="flex h-full flex-col divide-y divide-hairline">
      <Readout
        label="Altitude"
        value={frame ? fmtInt(frame.baroAltM) : dash}
        unit="m"
        tone="data"
      />
      <Readout
        label="V-Speed"
        value={frame ? fmtSigned(frame.vspeedMs, 1) : dash}
        unit="m/s"
      />
      <Readout label="Max Alt" value={fmtInt(maxAltM)} unit="m" />
      <Readout
        label="Vbat"
        value={frame ? fmtFixed(frame.vbatV, 1) : dash}
        unit="V"
        tone={frame ? vbatTone(frame.vbatV) : "ink"}
      />
      <Readout
        label="Tilt"
        value={frame ? fmtInt(frame.tiltDeg) : dash}
        unit="°"
        tone={frame ? tiltTone(frame.tiltDeg, frame.flightState) : "ink"}
      />

      {/* go/no-go strip fills the remaining column height */}
      <div className="mt-auto flex flex-wrap gap-x-6 gap-y-2 px-4 py-3">
        <StatusLight label="CONT" state={cont} />
        <StatusLight label="ARMED" state={armed} />
        <StatusLight label="SD" state={sd} />
        <StatusLight label="PYRO" state={pyro} />
      </div>
    </div>
  )
}
