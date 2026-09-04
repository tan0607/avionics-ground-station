/**
 * FlightTimeline — the mission phases PAD→…→LANDED as a vertical progress axis.
 * Dots are joined by a 1px connector (brighter for the flown portion) so it
 * reads as one timeline, not scattered points. Progression is dual-encoded by
 * fill AND ink weight; no motion (motion competes with the 4 Hz chart).
 */
import { cn } from "@/lib/utils"
import { Card } from "@/components/ui/card"
import { FLIGHT_STATE_NAME, FlightState } from "@/lib/protocol"

// CHRONOLOGICAL, which is not enum order: ARMED is 7 because the protocol
// could only append it, and it belongs here between PAD and BOOST. This array
// is the display contract; the enum value is just an identity.
const ORDER: FlightState[] = [
  FlightState.PAD,
  FlightState.ARMED,
  FlightState.BOOST,
  FlightState.COAST,
  FlightState.APOGEE,
  FlightState.DROGUE,
  FlightState.MAIN,
  FlightState.LANDED,
]

export function FlightTimeline({ state }: { state: FlightState | null }) {
  const activeIdx = state == null ? -1 : ORDER.indexOf(state)

  return (
    <Card className="min-h-0 flex-1 overflow-hidden">
      <div className="border-b border-hairline px-4 py-2">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Flight State</span>
      </div>
      <ol className="flex min-h-0 flex-1 flex-col px-4 py-3">
        {ORDER.map((s, i) => {
          const done = activeIdx >= 0 && i < activeIdx
          const active = i === activeIdx
          const last = i === ORDER.length - 1
          return (
            <li key={s} className="flex flex-1 gap-3">
              {/* dot + connector column */}
              <div className="flex w-2.5 flex-col items-center">
                <span
                  aria-hidden
                  className={cn(
                    "mt-1 size-2.5 shrink-0 rounded-full",
                    active
                      ? "bg-data"
                      : done
                        ? "bg-ink-dim"
                        : "border border-ink-mute bg-surface",
                  )}
                />
                {!last && (
                  <span
                    aria-hidden
                    className={cn("w-px flex-1", done ? "bg-ink-dim/60" : "bg-hairline")}
                  />
                )}
              </div>
              <span
                aria-current={active ? "step" : undefined}
                className={cn(
                  "text-xs uppercase tracking-[0.12em]",
                  active ? "text-ink" : done ? "text-ink-dim" : "text-ink-mute",
                )}
              >
                {FLIGHT_STATE_NAME[s]}
              </span>
            </li>
          )
        })}
      </ol>
    </Card>
  )
}
