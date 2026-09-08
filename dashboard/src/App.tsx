/**
 * App — the single-screen ground-station console (monochrome terminal).
 * Left view rail · top mission bar · KPI instrument strip · main split:
 * a stacked sensor-chart column (altitude, then vertical-speed + tilt + the
 * body accelerations) on the left, GO/NO-GO + flight-state timeline on the right. Pure black, 1px hairline
 * panels, white data / semantic status only. One viewport; only the status column scrolls.
 * useTelemetry owns the live data and mission reset; the secondary channels
 * follow its chart revision through useSensorSeries.
 */
import { useCallback, useEffect, useMemo, useState, type ReactNode } from "react"
import { cn } from "@/lib/utils"
import { useTelemetry } from "@/hooks/useTelemetry"
import { useSensorSeries } from "@/hooks/useSensorSeries"
import { useTelemetryLog } from "@/hooks/useTelemetryLog"
import { useFlightRecorder } from "@/hooks/useFlightRecorder"
import { useOnboardLog } from "@/hooks/useOnboardLog"
import { useSettings } from "@/hooks/useSettings"
import { useBackendStats } from "@/hooks/useBackendStats"
import { useGroundStation } from "@/hooks/useGroundStation"
import { useMission } from "@/hooks/useMission"
import { defaultFlightLabel } from "@/lib/mission"
import { MissionSwitchDialog } from "@/components/MissionSwitchDialog"
import { RecordingReminder } from "@/components/RecordingReminder"
import { AlertStack } from "@/components/AlertStack"
import { useAlarmSound } from "@/hooks/useAlarmSound"
import { Card } from "@/components/ui/card"
import { SideNav, type ViewId } from "@/components/SideNav"
import { TopBar } from "@/components/TopBar"
import { KpiRow } from "@/components/KpiRow"
import { AltitudeChart } from "@/components/AltitudeChart"
import { SensorChart, SensorLegend, type SensorTrace } from "@/components/SensorChart"
import { GoNoGo } from "@/components/GoNoGo"
import { ArmingStatus } from "@/components/ArmingStatus"
import { SubsystemHealth } from "@/components/SubsystemHealth"
import { AuxReadouts } from "@/components/AuxReadouts"
import { FlightTimeline } from "@/components/FlightTimeline"
import { FlightMap } from "@/components/FlightMap"
import { LogView } from "@/components/LogView"
import { FlightsView } from "@/components/FlightsView"
import { SettingsView } from "@/components/SettingsView"

/**
 * How much recent history the three instrument panels show.
 *
 * They used to plot the whole session, which is wrong for an instrument in two
 * ways at once: after ten minutes on the pad a new packet moves the trace by a
 * fraction of a pixel (the panel reads as frozen), and the y auto-range spans
 * every transient since power-up, so one knock of the airframe flattens the
 * live signal into a hairline. 90 s is long enough to hold a whole boost →
 * apogee → deploy sequence and short enough that a 2 Hz link advances the trace
 * by a visible ~2 px per packet. The altitude chart above is deliberately NOT
 * windowed — the flight arc is the one thing you want whole.
 */
const SENSOR_WINDOW_SEC = 90

/**
 * The three body-axis accelerations, as one panel. They are read together —
 * "is the thrust axis still the thrust axis" is a comparison, not three
 * separate questions — so they share a y-scale rather than getting a card each.
 *
 * Colors are named as CSS custom properties (SensorChart resolves them off
 * :root, and the legend swatch uses the same var) so the console stays the one
 * source of truth for its palette. Az gets the white --data stroke because it
 * is the axis that carries thrust and gravity; the lateral pair is tinted only
 * to separate it, and the legend directly above says so — the semantic reading
 * of green/amber belongs to the status panels, not here.
 */
const ACCEL_TRACES: (SensorTrace & { channel: "ax" | "ay" | "az" })[] = [
  { label: "Az", channel: "az", color: "--data", ys: [] },
  { label: "Ax", channel: "ax", color: "--nominal", ys: [] },
  { label: "Ay", channel: "ay", color: "--caution", ys: [] },
]

/**
 * The window marker for a scrolling panel. Without it the operator has no way
 * to tell a windowed instrument from a broken one that lost its history.
 */
function WindowTag() {
  return <span className="text-ink-mute">{SENSOR_WINDOW_SEC}s</span>
}

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
      {/*
        The chart column is a third as wide per panel as it used to be, so the
        header has to survive a title and a legend competing for it: the title
        truncates and the right slot never does. Losing a character of "Accel"
        is recoverable; losing the key that says which trace is Az is not.
      */}
      <div className="flex items-baseline justify-between gap-2 border-b border-hairline px-3 py-1.5">
        <span className="truncate text-[0.6875rem] uppercase tracking-[0.14em] text-ink-mute">
          {title}
        </span>
        {right && (
          <span className="shrink-0 text-[0.625rem] tabular-nums text-ink-mute">{right}</span>
        )}
      </div>
      <div className="min-h-0 flex-1 p-2">{children}</div>
    </Card>
  )
}

function App() {
  const [view, setView] = useState<ViewId>("live")
  const gs = useGroundStation()
  // Recorder and mission controls live across every view. Finish an active
  // flight before retuning so a named flight folder does not mix vehicles.
  const recorder = useFlightRecorder()
  const { recording: isRecording, status: recorderStatus, stop: stopRecording } = recorder
  const beforeSwitch = useCallback(async () => {
    if (gs.supported && recorderStatus !== "ok") return false
    return isRecording ? stopRecording() : true
  }, [gs.supported, recorderStatus, isRecording, stopRecording])
  const mission = useMission(gs, beforeSwitch)
  const telemetry = useTelemetry(mission.mission.name, mission.acceptTelemetry)
  // Live sources only: in mock mode there is no backend to be broken.
  const stats = useBackendStats(telemetry.source === "mock" ? 60_000 : 2000)
  const sensors = useSensorSeries(telemetry)
  // The VEHICLE's SD card, latched: its state rides one packet in ten, so it
  // cannot be read off the current frame the way the health bits are. Distinct
  // from `recorder`, which is this laptop cutting its own flight folders.
  const onboardLog = useOnboardLog(telemetry)

  // What the log needs from the recorder, narrowed here so the accumulator does
  // not take a dependency on the whole recorder state (and re-run on its poll).
  const recording = useMemo(
    () => ({ recording: recorder.recording, flight: recorder.flight?.flight ?? null }),
    [recorder.recording, recorder.flight?.flight],
  )

  // The log accumulates at APP level, not inside LogView. It used to be called
  // from that component, which App only mounts while the Log tab is open — so
  // the log recorded nothing at all whenever the operator was looking at any
  // other view, and switching tabs threw away what it had. A flight recorder
  // that only records while you watch it is not a flight recorder.
  // It takes the recorder's state so each row is stamped with the flight
  // folder it landed in — which is why the recorder is declared above it.
  const log = useTelemetryLog(telemetry, recording)
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
      <MissionSwitchDialog mission={mission} recorder={recorder} />

      {/* Bottom-right, over everything: what is wrong and what to do about it.
          Derived from live state, so a card clears when its cause does. */}
      <AlertStack
        backendStatus={stats.status}
        sourceError={stats.data?.source_error}
        sourceKind={stats.data?.source?.port}
        framesDecoded={stats.data?.frames_decoded}
        crcErrors={stats.data?.crc_errors}
        unknownStates={stats.data?.unknown_states}
        socketStatus={telemetry.status}
        channelError={gs.lastError}
      />
      <SideNav active={view} onSelect={setView} />

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar state={telemetry} backend={stats.data?.source} recorder={recorder} mission={mission} />
        <RecordingReminder
          liveSource={telemetry.source === "ws" && stats.data?.source?.kind === "serial"}
          recorder={recorder}
          mission={mission.mission.name}
          switching={mission.busy || mission.pending || Boolean(mission.requested)}
          onStart={() => void recorder.start(defaultFlightLabel(mission.mission.name))}
        />

        {view === "live" && (
          <div className="flex min-h-0 flex-1 flex-col overflow-hidden" role="region" aria-label="Live telemetry">
            <KpiRow frame={telemetry.frame} maxAltM={telemetry.maxAltM} />
            <AuxReadouts frame={telemetry.frame} />
            <div className="shrink-0 px-2 pt-2">
              <ArmingStatus key={mission.mission.name} frame={telemetry.frame} link={telemetry.link} />
            </div>

            <main className="grid min-h-0 flex-1 grid-cols-[1fr_17rem] grid-rows-[minmax(0,1fr)] gap-2 p-2">
              {/* sensor-chart column */}
              <div className="flex min-h-0 min-w-0 flex-col gap-2">
                <ChartCard
                  title="Altitude · m AGL"
                  right={telemetry.frame ? `${telemetry.chart.xs.length} pts` : "awaiting link"}
                  className="flex-[1.7]"
                >
                  <AltitudeChart chart={telemetry.chart} />
                </ChartCard>

                <div className="grid min-h-0 flex-1 grid-cols-3 gap-2">
                  <ChartCard title="Vertical Speed · m/s" right={<WindowTag />}>
                    <SensorChart
                      xs={sensors.xs}
                      traces={[{ label: "Vz", ys: sensors.vspeed }]}
                      rev={sensors.rev}
                      unit=" m/s"
                      signed
                      minSpan={2}
                      windowSec={SENSOR_WINDOW_SEC}
                    />
                  </ChartCard>
                  {/*
                    Tilt is clamped to what the quantity can physically be but
                    NOT pinned to it: on a real flight it lives between 0 and a
                    few degrees, so a hard 0..180 axis drew a flat line along
                    the bottom and read as a broken chart. minSpan keeps a
                    near-upright vehicle from being magnified into noise.
                  */}
                  <ChartCard title="Tilt · deg" right={<WindowTag />}>
                    <SensorChart
                      xs={sensors.xs}
                      traces={[{ label: "Tilt", ys: sensors.tilt }]}
                      rev={sensors.rev}
                      unit="°"
                      clamp={[0, 180]}
                      minSpan={10}
                      windowSec={SENSOR_WINDOW_SEC}
                    />
                  </ChartCard>
                  <ChartCard
                    title="Accel · m/s²"
                    right={
                      <span className="flex items-center gap-2">
                        <WindowTag />
                        <SensorLegend traces={ACCEL_TRACES} />
                      </span>
                    }
                  >
                    <SensorChart
                      xs={sensors.xs}
                      traces={ACCEL_TRACES.map((t) => ({ ...t, ys: sensors[t.channel] }))}
                      rev={sensors.rev}
                      unit=" m/s²"
                      signed
                      minSpan={4}
                      windowSec={SENSOR_WINDOW_SEC}
                    />
                  </ChartCard>
                </div>
              </div>

              {/* safety + phase */}
              <div
                className="flex min-h-0 flex-col gap-2 overflow-y-auto overscroll-y-contain focus-visible:outline focus-visible:outline-1 focus-visible:-outline-offset-1 focus-visible:outline-ring"
                role="region"
                aria-label="Flight status"
                tabIndex={0}
              >
                <GoNoGo frame={telemetry.frame} />
                <SubsystemHealth frame={telemetry.frame} onboardLog={onboardLog} />
                <FlightTimeline state={telemetry.frame?.flightState ?? null} />
              </div>
            </main>
          </div>
        )}

        {view === "map" && (
          <main className="min-h-0 flex-1 p-2">
            <FlightMap key={mission.mission.name} frame={telemetry.frame} link={telemetry.link} />
          </main>
        )}

        {view === "log" && <LogView log={log} recording={recording} />}

        {view === "flights" && <FlightsView />}

        {view === "settings" && (
          <SettingsView telemetry={telemetry} alarm={alarm} gs={gs} mission={mission} />
        )}
      </div>
    </div>
  )
}

export default App
