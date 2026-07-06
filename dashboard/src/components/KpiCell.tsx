/**
 * KpiCell — one readout inside the instrument strip: quiet uppercase label and
 * a large tabular-mono value (the hero) + unit. No per-cell border or caption;
 * cells are divided by the strip's 1px hairlines. Value tone is calm-until-
 * alarm — ink by default, semantic only when the number itself is off-nominal.
 */
export type Tone = "ink" | "data" | "nominal" | "caution" | "alarm"

const VALUE_INK: Record<Tone, string> = {
  ink: "text-ink",
  data: "text-data",
  nominal: "text-nominal",
  caution: "text-caution",
  alarm: "text-alarm",
}

export interface KpiCellProps {
  label: string
  value: string
  unit?: string
  tone?: Tone
}

export function KpiCell({ label, value, unit, tone = "ink" }: KpiCellProps) {
  return (
    <div className="flex min-w-0 flex-1 flex-col justify-center px-4 py-2.5">
      <span className="text-[0.625rem] uppercase tracking-[0.14em] text-ink-mute">{label}</span>
      <div className="mt-1 flex items-baseline gap-1.5">
        <span className={`tnum truncate text-2xl leading-none tabular-nums ${VALUE_INK[tone]}`}>
          {value}
        </span>
        {unit && <span className="shrink-0 text-[0.625rem] text-ink-mute">{unit}</span>}
      </div>
    </div>
  )
}
