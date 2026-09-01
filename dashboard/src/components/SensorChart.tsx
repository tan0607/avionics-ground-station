/**
 * SensorChart — a small uPlot line panel for the secondary sensor channels
 * (vertical speed, tilt, body accelerations). Hairline axes on black, an
 * optional dashed zero baseline for signed data, and the same imperative
 * UPlotChart + setData(rev) streaming path as the altitude chart — no per-frame
 * full redraw.
 *
 * Three things it does that the first cut did not:
 *
 * 1. MULTI-TRACE. One panel can carry several channels (ax/ay/az) against a
 *    shared x-axis, which is the only way to read them: what matters about the
 *    body accelerations is how they compare, not their individual shapes.
 *
 * 2. AN AUTO-RANGE WITH A FLOOR. `yDomain` used to pin tilt to a fixed 0..180.
 *    Real tilt sits between 0 deg and 7 deg for almost an entire flight, so the
 *    trace rendered as a flat line welded to the bottom axis — the chart looked
 *    broken because 96% of its height was showing degrees the vehicle will
 *    never reach. The range now follows the data, with `minSpan` stopping it
 *    from collapsing onto a dead-flat channel and `clamp` keeping it inside
 *    what the quantity can physically be.
 *
 * 3. A TRAILING TIME WINDOW. These panels used to plot the whole session, and
 *    an unbounded x-axis breaks a live instrument twice over. Ten minutes of
 *    link puts ~1200 samples behind a ~350 px plot, so a new packet advances
 *    the trace by a third of a pixel and the chart reads as frozen; and the y
 *    auto-range then spans every transient the session ever saw — one knock of
 *    the airframe on the bench set a -12..13 m/s² range and squashed the whole
 *    quiet period into three hairlines. `windowSec` scrolls the x scale
 *    instead, and uPlot ranges y over the visible samples only (accScale runs
 *    on the index window the x range selects), so both axes follow what the
 *    vehicle is doing NOW. Nothing is discarded — the samples stay in the
 *    arrays, only the view moves.
 */
import { useRef } from "react"
import uPlot from "uplot"
import { UPlotChart } from "./UPlotChart"

const MONO = '"Geist Mono Variable", ui-monospace, monospace'

function themeColor(name: string, fallback: string): string {
  if (typeof document === "undefined") return fallback
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  return v || fallback
}

export interface SensorTrace {
  label: string
  /** Samples, index-aligned with `xs`. `null` = no reading, drawn as a gap. */
  ys: (number | null)[]
  /** CSS custom property naming the stroke; defaults to --data (white). */
  color?: string
}

export interface SensorChartProps {
  xs: number[]
  traces: SensorTrace[]
  rev: number
  unit: string
  /** dashed baseline at y=0 (for signed channels like vertical speed) */
  signed?: boolean
  /** hard limits the auto-range may never leave, e.g. tilt is 0..180 */
  clamp?: [number, number]
  /** smallest y-span the auto-range may collapse to, in data units */
  minSpan?: number
  /**
   * Plot only the most recent `windowSec` seconds. Older samples are not
   * dropped — they stay in the arrays, and the altitude chart above still
   * carries the whole session — they just scroll off the left edge. Omitted =
   * plot everything.
   */
  windowSec?: number
}

/**
 * Data x-range -> the visible window, ending hard on the newest sample.
 *
 * While the series is shorter than the window it is shown whole, so a session
 * does not open with its first packets crawling in from the right-hand edge;
 * past that the view scrolls. The right edge is the newest sample rather than
 * a padded value, because on a live instrument "the trace ends at the wall" is
 * the thing that makes it read as live.
 */
function trailingWindow(min: number, max: number, windowSec: number): [number, number] {
  if (min == null || max == null || !Number.isFinite(min) || !Number.isFinite(max)) {
    return [0, windowSec]
  }
  // A single sample spans nothing, and uPlot cannot place a point in a zero
  // span. The invented second goes FORWARD from the data, never backward:
  // reaching back from the first sample puts negative onboard seconds on a
  // mission instrument's axis, which reads as a broken clock.
  if (max - min < 1) return [min, min + 1]
  return [Math.max(min, max - windowSec), max]
}

/**
 * Data range -> axis range. Pads by 8% when the data has real spread, and
 * otherwise opens out to `minSpan` around the data's midpoint so a channel
 * resting near one value gets a readable window instead of a flat line pinned
 * to an axis. `clamp` is applied last and the span is re-expanded inward if
 * clamping crushed it, so hitting a physical limit never re-flattens the trace.
 */
function autoRange(
  min: number | null,
  max: number | null,
  { signed, clamp, minSpan = 0 }: Pick<SensorChartProps, "signed" | "clamp" | "minSpan">,
): [number, number] {
  if (min == null || max == null || !Number.isFinite(min) || !Number.isFinite(max)) {
    // No samples yet (or every one is null): show the clamp, or a unit window.
    return clamp ?? [0, Math.max(1, minSpan)]
  }

  let lo = signed ? Math.min(0, min) : min
  let hi = signed ? Math.max(0, max) : max

  if (hi - lo < minSpan) {
    const mid = (lo + hi) / 2
    lo = mid - minSpan / 2
    hi = mid + minSpan / 2
  } else {
    const pad = (hi - lo) * 0.08
    lo -= pad
    hi += pad
  }

  if (clamp) {
    lo = Math.max(clamp[0], lo)
    hi = Math.min(clamp[1], hi)
    if (hi - lo < minSpan) {
      // Clamping ate the window — push the free end out instead of shrinking.
      if (lo <= clamp[0]) hi = Math.min(clamp[1], lo + minSpan)
      else lo = Math.max(clamp[0], hi - minSpan)
    }
  }
  return [lo, hi]
}

/**
 * SensorLegend — the key for a multi-trace panel. Lives in the card header
 * rather than inside the plot: these panels are ~120 px tall and an overlay
 * legend would cover the data it is naming.
 */
export function SensorLegend({ traces }: { traces: SensorTrace[] }) {
  return (
    <span className="flex items-center gap-2.5">
      {traces.map((t) => (
        <span key={t.label} className="flex items-center gap-1">
          <span
            aria-hidden
            className="inline-block h-[2px] w-2.5 shrink-0"
            style={{ backgroundColor: `var(${t.color ?? "--data"})` }}
          />
          {t.label}
        </span>
      ))}
    </span>
  )
}

/**
 * Build the uPlot option factory for one panel.
 *
 * Module scope, and called exactly once per mounted panel (see the ref in
 * SensorChart): UPlotChart only reads `makeOptions` on mount, so the series
 * roster, the colors and the scale rules are fixed for the life of the panel.
 * Doing this per render instead would re-run getComputedStyle on every
 * published frame — a forced style recalc at telemetry rate, to produce a
 * function nothing ever calls again.
 */
function buildOptions({
  traces,
  unit,
  signed,
  clamp,
  minSpan,
  windowSec,
}: Omit<SensorChartProps, "xs" | "rev">) {
  const spec = traces.map((t) => ({ label: t.label, color: t.color ?? "--data" }))
  const axisInk = themeColor("--ink-mute", "#9a9a9a")
  const grid = themeColor("--border", "#333333")
  // CSS px, NOT device px: uPlot runs axis.font through pxRatioFont() and
  // scales it itself. Pre-multiplying by pxRatio (which this did) drew every
  // axis label at double size on a retina screen, and a doubled label does not
  // fit a 44 px axis — it overflows the left edge of the canvas and gets cut
  // off there. Tilt's "17.5" lost its leading digit and rendered as "7.5";
  // vertical speed's "-0.5" lost its minus. A clipped number is worse than no
  // number, because what survives is still a plausible reading.
  const axisFont = `10px ${MONO}`
  const strokes = spec.map((s) => themeColor(s.color, "#f2f2f2"))

  const baseline: uPlot.Plugin = {
    hooks: {
      draw: (u) => {
        if (!signed || u.data[0].length === 0) return
        const y = u.valToPos(0, "y", true)
        const ctx = u.ctx
        ctx.save()
        ctx.strokeStyle = axisInk
        ctx.globalAlpha = 0.55
        ctx.lineWidth = 1
        ctx.setLineDash([2 * uPlot.pxRatio, 3 * uPlot.pxRatio])
        ctx.beginPath()
        ctx.moveTo(u.bbox.left, y)
        ctx.lineTo(u.bbox.left + u.bbox.width, y)
        ctx.stroke()
        ctx.restore()
      },
    },
  }

  return (width: number, height: number): uPlot.Options => ({
    width,
    height,
    padding: [10, 6, 2, 2],
    cursor: { y: false, points: { show: false }, drag: { x: false, y: false } },
    legend: { show: false },
    scales: {
      x: windowSec
        ? { time: false, range: (_u, min, max) => trailingWindow(min, max, windowSec) }
        : { time: false },
      y: { range: (_u, min, max) => autoRange(min, max, { signed, clamp, minSpan }) },
    },
    series: [
      { label: "t", value: (_u, v) => (v == null ? "--" : `${v.toFixed(1)}s`) },
      ...spec.map((s, i) => ({
        label: s.label,
        stroke: strokes[i],
        width: 1.4 * uPlot.pxRatio,
        paths: uPlot.paths.spline?.(),
        points: { show: false },
        value: (_u: uPlot, v: number | null) =>
          v == null ? "--" : `${v.toFixed(1)}${unit}`,
      })),
    ],
    axes: [
      {
        stroke: axisInk,
        font: axisFont,
        grid: { stroke: grid, width: 1 },
        ticks: { stroke: grid, width: 1, size: 3 },
        values: (_u, vals) => vals.map((v) => `${v}s`),
        gap: 4,
        size: 24,
        // These panels are a third of the chart column wide, so ticks have to
        // be spaced by hand or the labels smear into one band. "137.5s" is
        // ~36 px at this font; 48 px is that plus air to read it as a separate
        // tick. (It was 72 while the font was accidentally rendering at 2x —
        // correct-size labels fit twice as many ticks in the same panel.)
        space: 48,
      },
      {
        stroke: axisInk,
        font: axisFont,
        grid: { stroke: grid, width: 1 },
        ticks: { stroke: grid, width: 1, size: 3 },
        gap: 4,
        // Wide enough for a signed 3-digit label ("-143"): the accel axes
        // reach three figures under thrust, and a clipped minus sign turns a
        // deceleration into an acceleration. At the corrected font that label
        // is ~24 px, so 44 leaves real headroom rather than the deficit it was
        // silently running before.
        size: 44,
        space: 26,
      },
    ],
    plugins: [baseline],
  })
}

export function SensorChart(props: SensorChartProps) {
  const { xs, traces, rev } = props
  // Built on first render and never rebuilt — see buildOptions.
  const makeRef = useRef<((w: number, h: number) => uPlot.Options) | null>(null)
  if (makeRef.current === null) makeRef.current = buildOptions(props)

  const data = [xs, ...traces.map((t) => t.ys)] as unknown as uPlot.AlignedData

  return (
    <UPlotChart makeOptions={makeRef.current} data={data} rev={rev} className="h-full w-full" />
  )
}
