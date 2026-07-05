/**
 * UPlotChart — a thin React wrapper around an imperative uPlot instance.
 *
 * uPlot renders to a <canvas> and lives outside React's tree, so we create the
 * instance exactly once, drive its size from a ResizeObserver, and push new
 * samples via setData() keyed on a monotonically increasing `rev`. The data
 * arrays are expected to be mutated in place upstream (see useTelemetry) — we
 * never diff them; `rev` is the sole "data changed" signal.
 */
import { useEffect, useRef } from "react"
import uPlot from "uplot"

export interface UPlotChartProps {
  /** Build options for a given pixel size. Called once on mount. */
  makeOptions: (width: number, height: number) => uPlot.Options
  data: uPlot.AlignedData
  /** Bump whenever `data`'s contents change (arrays may be reused). */
  rev: number
  className?: string
}

const floor = (n: number) => Math.max(1, Math.floor(n))

export function UPlotChart({ makeOptions, data, rev, className }: UPlotChartProps) {
  const containerRef = useRef<HTMLDivElement | null>(null)
  const plotRef = useRef<uPlot | null>(null)
  // Keep the latest data/options reachable from the mount-once effect.
  const dataRef = useRef(data)
  dataRef.current = data
  const makeRef = useRef(makeOptions)
  makeRef.current = makeOptions

  useEffect(() => {
    const el = containerRef.current
    if (!el) return
    const rect = el.getBoundingClientRect()
    const plot = new uPlot(
      makeRef.current(floor(rect.width), floor(rect.height)),
      dataRef.current,
      el,
    )
    plotRef.current = plot

    const ro = new ResizeObserver((entries) => {
      const cr = entries[0].contentRect
      plot.setSize({ width: floor(cr.width), height: floor(cr.height) })
    })
    ro.observe(el)

    return () => {
      ro.disconnect()
      plot.destroy()
      plotRef.current = null
    }
  }, [])

  useEffect(() => {
    plotRef.current?.setData(dataRef.current)
  }, [rev])

  return <div ref={containerRef} className={className} />
}
