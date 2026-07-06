/**
 * GoNoGo — the safety panel: continuity / pyro / SD log / armed as triple-
 * encoded status rows inside a flat console Card. Lights change color the
 * instant state changes (at apogee the mock fires pyro → continuity flips
 * OK→OPEN and pyro SAFE→FIRED), so a deploy is impossible to miss.
 */
import { Card } from "@/components/ui/card"
import type { TelemetryFrame } from "@/lib/protocol"
import { StatusLight, type LightState } from "./StatusLight"

interface Row {
  label: string
  state: LightState
  detail: string
}

function rows(frame: TelemetryFrame | null): Row[] {
  if (!frame) {
    return [
      { label: "Continuity", state: "idle", detail: "—" },
      { label: "Pyro", state: "idle", detail: "—" },
      { label: "SD Log", state: "idle", detail: "—" },
      { label: "Armed", state: "idle", detail: "—" },
    ]
  }
  return [
    {
      label: "Continuity",
      state: frame.continuity ? "go" : "nogo",
      detail: frame.continuity ? "OK" : "OPEN",
    },
    {
      label: "Pyro",
      state: frame.pyroFired ? "caution" : "idle",
      detail: frame.pyroFired ? "FIRED" : "SAFE",
    },
    { label: "SD Log", state: frame.sdOk ? "go" : "nogo", detail: frame.sdOk ? "OK" : "FAIL" },
    { label: "Armed", state: frame.armed ? "caution" : "idle", detail: frame.armed ? "ARMED" : "SAFE" },
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
