/**
 * GoNoGo — the safety panel: continuity / pyro / SD log / armed as triple-
 * encoded status rows inside a flat console Card. Lights change color the
 * instant state changes (at apogee the mock fires pyro → continuity flips
 * OK→OPEN and pyro SAFE→FIRED), so a deploy is impossible to miss.
 */
import { Card } from "@/components/ui/card"
import { Flag, isFlagKnown, type TelemetryFrame } from "@/lib/protocol"
import { StatusLight, type LightState } from "./StatusLight"

interface Row {
  label: string
  state: LightState
  detail: string
}

const UNKNOWN: Omit<Row, "label"> = { state: "idle", detail: "—" }

function rows(frame: TelemetryFrame | null): Row[] {
  // A flag the downlink does not carry is unknown, not safe and not failed. The
  // MRCC text format carries none of these four, and rendering its zeros would
  // put OPEN continuity and a FAILED SD card on the safety panel — two alarms
  // nobody raised, on the one panel that must never cry wolf.
  const known = (mask: number) => isFlagKnown(frame, mask)
  return [
    {
      label: "Continuity",
      ...(known(Flag.CONTINUITY)
        ? {
            state: frame!.continuity ? "go" : "nogo",
            detail: frame!.continuity ? "OK" : "OPEN",
          }
        : UNKNOWN),
    },
    {
      label: "Pyro",
      ...(known(Flag.PYRO_FIRED)
        ? {
            state: frame!.pyroFired ? "caution" : "idle",
            detail: frame!.pyroFired ? "FIRED" : "SAFE",
          }
        : UNKNOWN),
    },
    {
      label: "SD Log",
      ...(known(Flag.SD_OK)
        ? { state: frame!.sdOk ? "go" : "nogo", detail: frame!.sdOk ? "OK" : "FAIL" }
        : UNKNOWN),
    },
    {
      label: "Armed",
      ...(known(Flag.ARMED)
        ? {
            state: frame!.armed ? "caution" : "idle",
            detail: frame!.armed ? "ARMED" : "SAFE",
          }
        : UNKNOWN),
    },
  ]
}

export function GoNoGo({ frame }: { frame: TelemetryFrame | null }) {
  return (
    <Card className="shrink-0">
      <div className="border-b border-hairline px-4 py-2">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Go / No-Go</span>
      </div>
      <div className="divide-y divide-hairline px-4">
        {rows(frame).map((r) => (
          <StatusLight key={r.label} label={r.label} state={r.state} detail={r.detail} />
        ))}
      </div>
    </Card>
  )
}
