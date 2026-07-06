/**
 * SensorChart — a small monochrome uPlot line for a secondary sensor channel
 * (vertical speed, tilt). White spline trace on black, hairline axes, an
 * optional dashed zero baseline for signed data. Same imperative UPlotChart +
 * setData(rev) streaming path as the altitude chart — no per-frame full redraw.
 */
import { useMemo } from "react"
import uPlot from "uplot"
import { UPlotChart } from "./UPlotChart"

const MONO = '"Geist Mono Variable", ui-monospace, monospace'

function themeColor(name: string, fallback: string): string {
  if (typeof document === "undefined") return fallback
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  return v || fallback
}

export interface SensorChartProps {
  xs: number[]
  ys: number[]
  rev: number
  unit: string
  /** dashed baseline at y=0 (for signed channels like vertical speed) */
  signed?: boolean
  /** fixed y-domain, e.g. tilt 0..180; omit for auto-range */
  yDomain?: [number, number]
}

export function SensorChart({ xs, ys, rev, unit, signed, yDomain }: SensorChartProps) {
  const makeOptions = useMemo(() => {
    const stroke = themeColor("--data", "#f2f2f2")
    const axisInk = themeColor("--ink-mute", "#9a9a9a")
    const grid = themeColor("--border", "#333333")
    const axisFont = `${Math.round(10 * uPlot.pxRatio)}px ${MONO}`

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
        x: { time: false },
        y: yDomain
          ? { range: () => yDomain }
          : {
              range: (_u, min, max) =>
                signed
                  ? [Math.min(0, min) * 1.1 - 1, Math.max(0, max) * 1.1 + 1]
                  : [Math.min(0, min), max <= 0 ? 10 : max * 1.08],
            },
      },
      series: [
        { label: "t", value: (_u, v) => (v == null ? "--" : `${v.toFixed(1)}s`) },
        {
          label: "y",
          stroke,
          width: 1.4 * uPlot.pxRatio,
          paths: uPlot.paths.spline?.(),
          points: { show: false },
          value: (_u, v) => (v == null ? "--" : `${Math.round(v)}${unit}`),
        },
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
        },
        {
          stroke: axisInk,
          font: axisFont,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid, width: 1, size: 3 },
          gap: 4,
          size: 34,
        },
      ],
      plugins: [baseline],
    })
  }, [signed, unit, yDomain])

  return (
    <UPlotChart makeOptions={makeOptions} data={[xs, ys]} rev={rev} className="h-full w-full" />
  )
}
