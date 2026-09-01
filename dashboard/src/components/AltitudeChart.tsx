/**
 * AltitudeChart — the streaming baro-altitude trace (cyan --data), hairline
 * axes on near-black, and a dashed apogee marker. The canvas reads its colors
 * from the console's CSS custom properties so it always matches the theme
 * (no second source of truth). x is onboard seconds (relative), y is m AGL.
 */
import { useMemo, useRef } from "react"
import uPlot from "uplot"
import { UPlotChart } from "./UPlotChart"
import type { ChartSeries } from "@/hooks/useTelemetry"

const MONO = '"Geist Mono Variable", ui-monospace, monospace'

/**
 * Pad time kept to the LEFT of liftoff, in seconds.
 *
 * The window cannot anchor on liftoff itself. The vehicle decides it has left
 * the pad and reports it in the next packet, so the ground station learns about
 * it up to one packet late — 0.25 s on the 2026-08-31 bench link, 0.5 s at
 * 2 Hz — against a burn measured at 2.00 s / 9 frames. Anchoring on the flag
 * would clip the first 12-25% of the boost.
 *
 * It does not have to. The series has been accumulating since the socket
 * opened (20 frames / 5.0 s were already buffered before the first BOOST frame
 * in that recording), so this only has to reach back far enough to cover the
 * detection lag and the run-up. Ten seconds is ~40x the observed lag.
 */
const LIFTOFF_LEAD_IN_S = 10

/**
 * How much history the chart shows BEFORE the vehicle leaves the pad, in
 * seconds. Pre-launch the trace is a flat line at 0 m, so nothing is lost by
 * trailing it — and it keeps the x-axis labels readable instead of spreading
 * them over however many hours the airframe has been powered.
 */
const PAD_WINDOW_S = 90

/**
 * Seconds of ground time kept to the RIGHT of touchdown.
 *
 * Enough that the trace visibly flattens out and lands rather than running off
 * the edge of the panel, without handing the post-flight sit any more of the
 * width than that.
 */
const LANDED_TAIL_S = 10

/** Read a themed color from :root; fall back to a literal if unset (SSR/tests). */
function themeColor(name: string, fallback: string): string {
  if (typeof document === "undefined") return fallback
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  return v || fallback
}

/**
 * Data x-range -> the visible window.
 *
 * Before liftoff: trail the last PAD_WINDOW_S. After: everything from
 * LIFTOFF_LEAD_IN_S before liftoff to the newest sample, so the flight profile
 * grows across the full width instead of being squeezed into whatever fraction
 * of the session it happens to occupy. An airframe powered for three hours
 * (which is a real number off this bench — the onboard clock read ~11,000 s)
 * used to draw a 3-minute flight across 2% of the panel.
 *
 * Once the vehicle is down the right edge stops at touchdown + a tail, so the
 * profile is not squeezed all over again by however long the airframe then
 * sits in a field. The three instrument panels below stay live, so a frozen
 * altitude trace never becomes the only thing telling the operator the link is
 * still up.
 *
 * `Math.max(min, …)` matters: at the 4000-point cap the buffer rotates, so
 * liftoff can age out of the series entirely. Asking for an x that no longer
 * exists would leave dead space on the left.
 */
function flightWindow(
  min: number,
  max: number,
  liftoffT: number | null,
  landedT: number | null,
): [number, number] {
  if (min == null || max == null || !Number.isFinite(min) || !Number.isFinite(max)) {
    return [0, PAD_WINDOW_S]
  }
  // A single sample spans nothing, and uPlot cannot place a point in a zero
  // span. The invented second goes FORWARD from the data, never backward:
  // reaching back from the first sample puts negative onboard seconds on the
  // axis, which reads as a broken clock.
  if (max - min < 1) return [min, min + 1]

  const from = Math.max(min, liftoffT != null ? liftoffT - LIFTOFF_LEAD_IN_S : max - PAD_WINDOW_S)
  // `Math.min(max, …)` so the edge still tracks the vehicle through the first
  // seconds after touchdown, then holds once the tail is filled.
  const to = landedT != null ? Math.min(max, landedT + LANDED_TAIL_S) : max

  // Sit on the ground long enough and the buffer rotates the whole flight out
  // from under this window, leaving `to` behind `from`. uPlot cannot draw an
  // inverted range, and the honest fallback is whatever is actually left.
  if (to - from < 1) return [min, max]
  return [from, to]
}

export function AltitudeChart({ chart }: { chart: ChartSeries }) {
  // The draw plugin reads the *current* apogee each frame via this ref, so the
  // marker tracks without rebuilding the (create-once) uPlot options.
  const apogeeRef = useRef(chart.apogee)
  apogeeRef.current = chart.apogee
  // Same reason as apogeeRef: makeOptions is built once, so the x-range
  // callback has to read the CURRENT liftoff through a ref rather than closing
  // over the value it happened to see on the first render (null, always).
  const liftoffRef = useRef(chart.liftoffT)
  liftoffRef.current = chart.liftoffT
  const landedRef = useRef(chart.landedT)
  landedRef.current = chart.landedT

  const makeOptions = useMemo(() => {
    const dataStroke = themeColor("--data", "#f2f2f2")
    const dataFill = themeColor("--data-bg", "rgba(242,242,242,0.05)")
    const axisInk = themeColor("--ink-mute", "#9a9a9a")
    const grid = themeColor("--border", "#333333")
    const apogeeInk = themeColor("--caution", "#f4b740")

    // Two fonts, because they go to two different places. uPlot runs axis.font
    // through pxRatioFont() and scales it itself, so the axis wants CSS px —
    // pre-multiplying (which this did) drew every label at 2x on a retina
    // screen and overflowed the 44 px axis, so a "1200" altitude would have
    // rendered as "200". The apogee marker below writes with fillText straight
    // onto the device-pixel canvas, so that one does need the scaled size.
    const axisFont = `10.5px ${MONO}`
    const markerFont = `${Math.round(10.5 * uPlot.pxRatio)}px ${MONO}`

    const apogeeMarker: uPlot.Plugin = {
      hooks: {
        draw: (u) => {
          const a = apogeeRef.current
          if (!a || u.data[0].length === 0) return
          const y = u.valToPos(a.alt, "y", true)
          const left = u.bbox.left
          const right = u.bbox.left + u.bbox.width
          const ctx = u.ctx
          ctx.save()
          ctx.strokeStyle = apogeeInk
          ctx.lineWidth = 1
          ctx.setLineDash([4 * uPlot.pxRatio, 4 * uPlot.pxRatio])
          ctx.beginPath()
          ctx.moveTo(left, y)
          ctx.lineTo(right, y)
          ctx.stroke()
          ctx.setLineDash([])
          ctx.fillStyle = apogeeInk
          ctx.font = markerFont
          ctx.textAlign = "right"
          ctx.textBaseline = "bottom"
          ctx.fillText(`APOGEE ${Math.round(a.alt)} m`, right - 4 * uPlot.pxRatio, y - 3 * uPlot.pxRatio)
          ctx.restore()
        },
      },
    }

    return (width: number, height: number): uPlot.Options => ({
      width,
      height,
      padding: [12, 8, 4, 4],
      cursor: {
        y: false,
        points: { show: false },
        drag: { x: false, y: false },
      },
      legend: { show: false },
      scales: {
        x: {
          time: false,
          range: (_u, min, max) =>
            flightWindow(min, max, liftoffRef.current, landedRef.current),
        },
        y: { range: (_u, min, max) => [Math.min(0, min), max <= 0 ? 10 : max * 1.08] },
      },
      series: [
        { label: "t", value: (_u, v) => (v == null ? "--" : `${v.toFixed(1)}s`) },
        {
          label: "alt",
          stroke: dataStroke,
          width: 1.5 * uPlot.pxRatio,
          fill: dataFill,
          // smooth the flight arc; falls back to linear if the build lacks spline
          paths: uPlot.paths.spline?.(),
          points: { show: false },
          value: (_u, v) => (v == null ? "--" : `${Math.round(v)} m`),
        },
      ],
      axes: [
        {
          stroke: axisInk,
          font: axisFont,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid, width: 1, size: 4 },
          values: (_u, vals) => vals.map((v) => `${v}s`),
          gap: 4,
          size: 30,
          // uPlot's default 50 px tick spacing is fine for whole seconds but
          // this axis is relative onboard time, so a short window makes it pick
          // half-second increments — "257.5s" labels every 50 px run together
          // into an unreadable band. That label is ~38 px at this font, so 56
          // is the widest one with air around it, at any zoom the mission clock
          // reaches. (It was 96 while the font was rendering at double size.)
          space: 56,
        },
        {
          stroke: axisInk,
          font: axisFont,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid, width: 1, size: 4 },
          values: (_u, vals) => vals.map((v) => `${v}`),
          gap: 4,
          size: 44,
        },
      ],
      plugins: [apogeeMarker],
    })
  }, [])

  return (
    <UPlotChart
      makeOptions={makeOptions}
      data={[chart.xs, chart.ys]}
      rev={chart.rev}
      className="h-full w-full"
    />
  )
}
