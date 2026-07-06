/**
 * StatusLight — one go/no-go row for the GO/NO-GO panel: label (left), a short
 * state word + a filled/hollow dot (right). Triple-encoded (position, color,
 * word) so it reads under stress and without color vision (PRODUCT.md §6).
 */
export type LightState = "go" | "nogo" | "caution" | "idle"

const DOT: Record<LightState, string> = {
  go: "bg-nominal",
  nogo: "bg-alarm",
  caution: "bg-caution",
  idle: "border border-ink-mute bg-transparent",
}

const INK: Record<LightState, string> = {
  go: "text-nominal",
  nogo: "text-alarm",
  caution: "text-caution",
  idle: "text-ink-mute",
}

export function StatusLight({
  label,
  state,
  detail,
}: {
  label: string
  state: LightState
  detail: string
}) {
  return (
    <div className="flex items-center justify-between py-1.5">
      <span className="text-[0.6875rem] uppercase tracking-[0.12em] text-ink-dim">{label}</span>
      <span className="flex items-center gap-2">
        <span className={`text-[0.6875rem] uppercase tracking-wide tabular-nums ${INK[state]}`}>
          {detail}
        </span>
        <span aria-hidden className={`size-2 shrink-0 rounded-full ${DOT[state]}`} />
      </span>
    </div>
  )
}
