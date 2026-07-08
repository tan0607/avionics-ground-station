/**
 * App — the single-screen ground-station console (monochrome terminal).
 * Left view rail · top mission bar · KPI instrument strip · main split:
 * a stacked sensor-chart column (altitude, then vertical-speed + tilt) on the
 * left, GO/NO-GO + flight-state timeline on the right. Pure black, 1px hairline
 * panels, white data / semantic status only. One viewport, no scroll.
 * The whole data layer (useTelemetry off the mock) is unchanged; the secondary
 * channels come from useSensorSeries, which piggybacks on it without touching it.
 */
import { useEffect, useState, type ReactNode } from "react"
import { cn } from "@/lib/utils"
import { useTelemetry } from "@/hooks/useTelemetry"
import { useSensorSeries } from "@/hooks/useSensorSeries"
import { useSettings } from "@/hooks/useSettings"
import { useAlarmSound } from "@/hooks/useAlarmSound"
import { Card } from "@/components/ui/card"
import { SideNav, type ViewId } from "@/components/SideNav"
import { TopBar } from "@/components/TopBar"
import { KpiRow } from "@/components/KpiRow"
import { AltitudeChart } from "@/components/AltitudeChart"
import { SensorChart } from "@/components/SensorChart"
import { GoNoGo } from "@/components/GoNoGo"
import { FlightTimeline } from "@/components/FlightTimeline"
import { FlightMap } from "@/components/FlightMap"
import { LogView } from "@/components/LogView"
import { SettingsView } from "@/components/SettingsView"

function ChartCard({
  title,
  right,
  className,
  children,
}: {
  title: string
  right?: ReactNode
  className?: string
  children: ReactNode
}) {
  return (
    <Card className={cn("min-h-0 min-w-0 overflow-hidden", className)}>
      <div className="flex items-baseline justify-between border-b border-hairline px-4 py-1.5">
        <span className="text-[0.6875rem] uppercase tracking-[0.14em] text-ink-mute">{title}</span>
        {right && <span className="text-[0.625rem] tabular-nums text-ink-mute">{right}</span>}
      </div>
      <div className="min-h-0 flex-1 p-2">{children}</div>
    </Card>
  )
}

function App() {
  const [view, setView] = useState<ViewId>("live")
  const telemetry = useTelemetry()
  const sensors = useSensorSeries(telemetry)
  const { settings } = useSettings()
  // Buzzer lives at app level so alarms sound on every view, not just Settings.
  const alarm = useAlarmSound(telemetry, settings)

  // High-contrast (sunlight) mode: a root class bumps the ink/border vars.
  useEffect(() => {
    document.documentElement.classList.toggle("contrast-high", settings.highContrast)
  }, [settings.highContrast])

  return (
    <div className="flex h-screen w-screen overflow-hidden bg-background text-foreground">
      <h1 className="sr-only">Rocket ground station — live telemetry</h1>
      <SideNav active={view} onSelect={setView} />

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar state={telemetry} />

        {view === "live" && (
          <>
            <KpiRow frame={telemetry.frame} maxAltM={telemetry.maxAltM} />

            <main className="grid min-h-0 flex-1 grid-cols-[1fr_17rem] gap-2 p-2">
              {/* sensor-chart column */}
              <div className="flex min-h-0 min-w-0 flex-col gap-2">
                <ChartCard
                  title="Altitude · m AGL"
                  right={telemetry.frame ? `${telemetry.chart.xs.length} pts` : "awaiting link"}
                  className="flex-[1.7]"
                >
                  <AltitudeChart chart={telemetry.chart} />
                </ChartCard>

                <div className="grid min-h-0 flex-1 grid-cols-2 gap-2">
                  <ChartCard title="Vertical Speed · m/s">
                    <SensorChart xs={sensors.xs} ys={sensors.vspeed} rev={sensors.rev} unit="" signed />
                  </ChartCard>
                  <ChartCard title="Tilt · deg">
                    <SensorChart xs={sensors.xs} ys={sensors.tilt} rev={sensors.rev} unit="°" yDomain={[0, 180]} />
                  </ChartCard>
                </div>
              </div>

              {/* safety + phase */}
              <div className="flex min-h-0 flex-col gap-2">
                <GoNoGo frame={telemetry.frame} />
                <FlightTimeline state={telemetry.frame?.flightState ?? null} />
              </div>
            </main>
          </>
        )}

        {view === "map" && (
          <main className="min-h-0 flex-1 p-2">
            <FlightMap frame={telemetry.frame} link={telemetry.link} />
          </main>
        )}

        {view === "log" && <LogView telemetry={telemetry} />}

        {view === "settings" && <SettingsView telemetry={telemetry} alarm={alarm} />}
      </div>
    </div>
  )
}

export default App
