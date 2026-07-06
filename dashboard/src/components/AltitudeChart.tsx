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

/** Read a themed color from :root; fall back to a literal if unset (SSR/tests). */
function themeColor(name: string, fallback: string): string {
  if (typeof document === "undefined") return fallback
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  return v || fallback
}

export function AltitudeChart({ chart }: { chart: ChartSeries }) {
  // The draw plugin reads the *current* apogee each frame via this ref, so the
  // marker tracks without rebuilding the (create-once) uPlot options.
  const apogeeRef = useRef(chart.apogee)
  apogeeRef.current = chart.apogee

  const makeOptions = useMemo(() => {
    const dataStroke = themeColor("--data", "#f2f2f2")
    const dataFill = themeColor("--data-bg", "rgba(242,242,242,0.05)")
    const axisInk = themeColor("--ink-mute", "#9a9a9a")
    const grid = themeColor("--border", "#333333")
    const apogeeInk = themeColor("--caution", "#f4b740")

    const axisFont = `${Math.round(10.5 * uPlot.pxRatio)}px ${MONO}`

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
          ctx.font = axisFont
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
        x: { time: false },
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
