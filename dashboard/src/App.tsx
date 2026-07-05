/**
 * App — the single-screen ground-station console (DESIGN_SPECS §3).
 * Top status strip, then a two-pane instrument row: the streaming altitude
 * chart takes the space, a fixed instrument column carries the hero numbers
 * and go/no-go. One viewport, no scroll, 1px dividers, no floating cards.
 */
import { useTelemetry } from "@/hooks/useTelemetry"
import { StatusBar } from "@/components/StatusBar"
import { AltitudeChart } from "@/components/AltitudeChart"
import { Readouts } from "@/components/Readouts"

function App() {
  const telemetry = useTelemetry()

  return (
    <div className="flex h-screen w-screen flex-col overflow-hidden bg-background text-foreground">
      <h1 className="sr-only">Rocket ground station — live telemetry</h1>
      <StatusBar state={telemetry} />

      <main className="flex min-h-0 flex-1">
        {/* chart pane */}
        <section className="flex min-w-0 flex-1 flex-col">
          <div className="flex items-baseline justify-between border-b border-hairline px-4 py-2">
            <span className="text-[0.6875rem] uppercase tracking-[0.14em] text-ink-mute">
              Altitude · m AGL
            </span>
            <span className="text-[0.6875rem] tabular-nums text-ink-mute">
              {telemetry.frame ? `${telemetry.chart.xs.length} pts` : "awaiting link"}
            </span>
          </div>
          <div className="min-h-0 flex-1 p-2">
            <AltitudeChart chart={telemetry.chart} />
          </div>
        </section>

        {/* instrument column — panel chrome (surface) against the chart's darker
            scope area (bg), so the two read as distinct instrument surfaces */}
        <aside className="w-64 shrink-0 border-l border-hairline bg-surface">
          <Readouts frame={telemetry.frame} maxAltM={telemetry.maxAltM} />
        </aside>
      </main>
    </div>
  )
}

export default App
