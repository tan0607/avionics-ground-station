/**
 * Readout — one big-number instrument cell: quiet uppercase label, a large
 * tabular-mono value that is the hero, and a muted unit. The value's tone is
 * semantic (default ink; caution/alarm only when the number itself is off-
 * nominal), never decorative. Data is the hero — chrome stays out of the way.
 */
export type Tone = "ink" | "data" | "nominal" | "caution" | "alarm"

const VALUE_INK: Record<Tone, string> = {
  ink: "text-ink",
  data: "text-data",
  nominal: "text-nominal",
  caution: "text-caution",
  alarm: "text-alarm",
}

export interface ReadoutProps {
  label: string
  value: string
  unit?: string
  tone?: Tone
}

export function Readout({ label, value, unit, tone = "ink" }: ReadoutProps) {
  return (
    <div className="flex flex-col gap-1 px-4 py-3">
      <span className="text-[0.6875rem] uppercase tracking-[0.14em] text-ink-mute">
        {label}
      </span>
      <div className="flex items-baseline gap-1.5">
        <span className={`tnum text-3xl leading-none tabular-nums ${VALUE_INK[tone]}`}>
          {value}
        </span>
        {unit && <span className="text-xs text-ink-mute">{unit}</span>}
      </div>
    </div>
  )
}
