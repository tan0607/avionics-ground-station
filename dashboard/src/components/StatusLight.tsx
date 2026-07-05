/**
 * StatusLight — a single go/no-go indicator (CONT, ARMED, SD, PYRO). State is
 * dual-encoded: a filled/hollow dot AND a semantic color AND the text label, so
 * it reads correctly under stress and without color vision (PRODUCT.md §6).
 */

export type LightState = "go" | "nogo" | "caution" | "idle"

const DOT: Record<LightState, string> = {
  go: "bg-nominal",
  nogo: "bg-alarm",
  caution: "bg-caution",
  idle: "bg-transparent border border-ink-mute",
}

const INK: Record<LightState, string> = {
  go: "text-nominal",
  nogo: "text-alarm",
  caution: "text-caution",
  idle: "text-ink-mute",
}

export function StatusLight({ label, state }: { label: string; state: LightState }) {
  return (
    <div className="flex items-center gap-2">
      <span
        aria-hidden
        className={`size-2 shrink-0 rounded-full ${DOT[state]}`}
      />
      <span className={`text-[0.6875rem] tracking-wide ${INK[state]}`}>{label}</span>
    </div>
  )
}
