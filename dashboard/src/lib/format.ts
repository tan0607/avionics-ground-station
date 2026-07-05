/** Display formatters for telemetry readouts. Grouping uses a narrow no-break
 * space (U+202F) so "1 247" reads as one number and digits stay tabular. */

const NNBSP = " "

export function groupThousands(value: number): string {
  const sign = value < 0 ? "-" : ""
  const digits = Math.abs(Math.trunc(value)).toString()
  const grouped = digits.replace(/\B(?=(\d{3})+(?!\d))/g, NNBSP)
  return sign + grouped
}

/** Rounded, thousands-grouped integer: 1247.6 → "1 247". */
export function fmtInt(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) return "—"
  return groupThousands(Math.round(value))
}

/** Fixed-decimal, grouped, always signed: +142.0 → "+142". */
export function fmtSigned(value: number | null | undefined, digits = 0): string {
  if (value == null || !Number.isFinite(value)) return "—"
  const rounded = Number(value.toFixed(digits))
  const sign = rounded > 0 ? "+" : rounded < 0 ? "-" : ""
  const abs = Math.abs(rounded)
  const whole = groupThousands(Math.trunc(abs))
  if (digits === 0) return sign + whole
  const frac = abs.toFixed(digits).split(".")[1]
  return `${sign}${whole}.${frac}`
}

export function fmtFixed(value: number | null | undefined, digits = 1): string {
  if (value == null || !Number.isFinite(value)) return "—"
  return value.toFixed(digits)
}

/** Mission clock: seconds → "T+ 00:12.4" (or "T- 00:05.0" before liftoff). */
export function fmtTimer(seconds: number | null | undefined): string {
  if (seconds == null || !Number.isFinite(seconds)) return "T+ --:--.-"
  const sign = seconds < 0 ? "T-" : "T+"
  const s = Math.abs(seconds)
  const mm = Math.floor(s / 60)
  const ss = Math.floor(s % 60)
  const tenths = Math.floor((s * 10) % 10)
  const pad = (n: number, w = 2) => n.toString().padStart(w, "0")
  return `${sign} ${pad(mm)}:${pad(ss)}.${tenths}`
}

/** Seconds-since-last-packet, compact: "0.3s" / "8.2s". */
export function fmtLinkAge(ms: number | null | undefined): string {
  if (ms == null || !Number.isFinite(ms)) return "--"
  return `${(ms / 1000).toFixed(1)}s`
}

export function fmtPercent(fraction: number | null | undefined, digits = 1): string {
  if (fraction == null || !Number.isFinite(fraction)) return "—"
  return `${(fraction * 100).toFixed(digits)}%`
}

export function fmtLatLon(lat: number, lon: number): string {
  const ns = lat >= 0 ? "N" : "S"
  const ew = lon >= 0 ? "E" : "W"
  return `${Math.abs(lat).toFixed(4)}°${ns} ${Math.abs(lon).toFixed(4)}°${ew}`
}
