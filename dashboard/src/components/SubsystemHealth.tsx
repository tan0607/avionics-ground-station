/**
 * SubsystemHealth — per-peripheral status, one row each.
 *
 * This panel is the ground-station half of the fault-isolation contract: the
 * vehicle reports a health bit per peripheral, so the
 * operator sees "BARO LOST — altitude stale" and not a single useless
 * "AV FAILED". Everything else on the vehicle is still flying and still
 * transmitting; only the named row is down.
 *
 * Rows are triple-encoded (position, color, word) like GoNoGo, per PRODUCT.md §6.
 */
import { SUBSYSTEMS, isKnown, type TelemetryFrame } from "@/lib/protocol"
import { Card } from "@/components/ui/card"
import { StatusLight, type LightState } from "./StatusLight"

function rowState(frame: TelemetryFrame | null, mask: number): LightState {
  // No link, a firmware build that predates the health byte, or a downlink that
  // simply doesn't carry this peripheral: unknown, not failed. A red light for a
  // field nobody reported would be a false alarm, and the operator has no way to
  // tell a fabricated alarm from a real one.
  if (!isKnown(frame, mask)) return "idle"
  return frame!.health & mask ? "go" : "nogo"
}

function rowDetail(frame: TelemetryFrame | null, mask: number): string {
  if (!isKnown(frame, mask)) return "—"
  return frame!.health & mask ? "OK" : "LOST"
}

export function SubsystemHealth({ frame }: { frame: TelemetryFrame | null }) {
  const down = SUBSYSTEMS.filter(
    (s) => isKnown(frame, s.mask) && !(frame!.health & s.mask),
  )

  return (
    <Card className="shrink-0">
      <div className="flex items-baseline justify-between border-b border-hairline px-4 py-2">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">
          Peripherals
        </span>
        {down.length > 0 && (
          <span className="text-[0.625rem] uppercase tracking-[0.12em] tabular-nums text-alarm">
            {down.length} down
          </span>
        )}
      </div>

      <div className="divide-y divide-hairline px-4">
        {SUBSYSTEMS.map((s) => (
          <StatusLight
            key={s.name}
            label={s.name}
            state={rowState(frame, s.mask)}
            detail={rowDetail(frame, s.mask)}
          />
        ))}
      </div>

      {/* Name the consequence, not just the fault — which readouts to distrust. */}
      {down.length > 0 && (
        <div className="border-t border-hairline px-4 py-2">
          <p className="text-[0.625rem] leading-relaxed uppercase tracking-[0.12em] text-ink-mute">
            Stale: {down.map((s) => s.feeds).join(" · ")}
          </p>
        </div>
      )}
    </Card>
  )
}
