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
 *
 * The SD row carries the vehicle's RECORDER as well as its card. The health bit
 * says the card is mounted; `useOnboardLog` says whether the flight is
 * actually being written to it, which the vehicle used to report only on a USB
 * console nobody can reach once the rocket is closed up.
 */
import { Health, SUBSYSTEMS, isKnown, type TelemetryFrame } from "@/lib/protocol"
import { groupThousands } from "@/lib/format"
import type { OnboardLog } from "@/hooks/useOnboardLog"
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

/**
 * The SD row, once the recorder has reported.
 *
 * A mounted card that has stopped accepting writes reads SD OK forever — the
 * bit is about the card, not about the log — so a line count that has stopped
 * moving is a CAUTION on this row rather than a number in a footer nobody
 * scans. It is not a NO-GO: the vehicle is still flying and still transmitting,
 * and the loss is the post-flight record, not the flight.
 */
function sdRow(
  frame: TelemetryFrame | null,
  rec: OnboardLog,
): { state: LightState; detail: string } {
  const state = rowState(frame, Health.SD)
  if (state !== "go") return { state, detail: rowDetail(frame, Health.SD) }

  if (rec.stalled) return { state: "caution", detail: "NOT WRITING" }
  if (rec.rateHz != null) return { state: "go", detail: `${rec.rateHz.toFixed(1)} Hz` }
  return { state: "go", detail: "OK" }
}

/** How long ago the last recorder report arrived, shown only once it is late. */
function stale(rec: OnboardLog, now: number): string | null {
  if (rec.reportedAt == null) return null
  const sec = Math.floor((now - rec.reportedAt) / 1000)
  // Reports ride a 5 s cadence on a link that drops packets; three missed in a
  // row is the point at which the numbers below stop describing the present.
  return sec >= 15 ? `${sec}s ago` : null
}

export function SubsystemHealth({
  frame,
  onboardLog,
}: {
  frame: TelemetryFrame | null
  /** The VEHICLE's SD log. Not the ground station's recorder (useFlightRecorder). */
  onboardLog: OnboardLog
}) {
  // Read at render: App republishes on a UI tick, so the age below keeps
  // counting up when the link goes quiet instead of freezing at its last value.
  const ageLabel = stale(onboardLog, Date.now())
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
        {SUBSYSTEMS.map((s) => {
          const row =
            s.mask === Health.SD
              ? sdRow(frame, onboardLog)
              : { state: rowState(frame, s.mask), detail: rowDetail(frame, s.mask) }
          return <StatusLight key={s.name} label={s.name} state={row.state} detail={row.detail} />
        })}
      </div>

      {/*
        The [SD] console line, on the ground: which file the flight is in, how
        much of it has been written, and whether any writes were refused. It
        appears only once the vehicle has reported — an empty recorder line
        would be indistinguishable from one reading zero.
      */}
      {onboardLog.reportedAt != null && (
        <div className="flex items-baseline justify-between gap-2 border-t border-hairline px-4 py-2">
          <span className="truncate text-[0.625rem] uppercase tracking-[0.12em] text-ink-mute">
            REC {onboardLog.fileName ?? "no file"}
          </span>
          <span className="flex shrink-0 items-baseline gap-2 text-[0.625rem] tabular-nums tracking-[0.08em] text-ink-dim">
            <span>{onboardLog.lines != null ? `${groupThousands(onboardLog.lines)} ln` : "—"}</span>
            {onboardLog.errors != null && onboardLog.errors > 0 && (
              <span className="text-alarm">{onboardLog.errors} err</span>
            )}
            {ageLabel && <span className="text-ink-mute">{ageLabel}</span>}
          </span>
        </div>
      )}

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
