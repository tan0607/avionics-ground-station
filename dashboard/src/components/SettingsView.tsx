/**
 * SettingsView — the Set page. Four flat console cards:
 *   1. Connection & session — read-only status (polls /stats) + a Mock/Live
 *      source switch done as bookmarkable reload links (chooseSource reads the
 *      URL at load, so switching source is a navigation, not a runtime toggle —
 *      this keeps the frozen data layer untouched).
 *   2. Alarms & audio — buzzer master + per-alarm arming + a test beep.
 *   3. Display — high-contrast, table density, row cap.
 *   4. Data export — download the active session's files from the backend.
 */
import { useEffect, useRef, useState, type ReactNode } from "react"
import { cn } from "@/lib/utils"
import { apiUrl } from "@/lib/api"
import { fmtPercent } from "@/lib/format"
import type { TelemetryState } from "@/hooks/useTelemetry"
import type { AlarmSound } from "@/hooks/useAlarmSound"
import { ROW_CAP_OPTIONS, useSettings } from "@/hooks/useSettings"
import { Card } from "@/components/ui/card"
import { Button } from "@/components/ui/button"

// --- backend stats poll ------------------------------------------------------

interface BackendStats {
  session: string | null
  frames_decoded: number
  crc_errors: number
  clients: number
  loss_pct: number
}

type StatsState = { status: "loading" | "ok" | "unreachable"; data: BackendStats | null }

function useBackendStats(intervalMs = 2000): StatsState {
  const [state, setState] = useState<StatsState>({ status: "loading", data: null })
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    const poll = async () => {
      try {
        const res = await fetch(apiUrl("/stats"), { cache: "no-store" })
        if (!res.ok) throw new Error(String(res.status))
        const data = (await res.json()) as BackendStats
        if (alive.current) setState({ status: "ok", data })
      } catch {
        if (alive.current) setState((s) => ({ status: "unreachable", data: s.data }))
      }
    }
    poll()
    const id = window.setInterval(poll, intervalMs)
    return () => {
      alive.current = false
      window.clearInterval(id)
    }
  }, [intervalMs])

  return state
}

// --- small controls ----------------------------------------------------------

function SettingCard({ title, children }: { title: string; children: ReactNode }) {
  return (
    <Card className="flex flex-col">
      <div className="border-b border-hairline px-4 py-2">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">{title}</span>
      </div>
      <div className="flex-1 divide-y divide-hairline px-4">{children}</div>
    </Card>
  )
}

function Row({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return (
    <div className="flex min-h-[2.75rem] items-center justify-between gap-4 py-2">
      <div className="min-w-0">
        <div className="text-xs text-ink-dim">{label}</div>
        {hint && <div className="text-[0.625rem] text-ink-mute">{hint}</div>}
      </div>
      <div className="shrink-0">{children}</div>
    </div>
  )
}

function Toggle({ on, onChange, disabled }: { on: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      disabled={disabled}
      onClick={() => onChange(!on)}
      className={cn(
        "inline-flex h-5 w-9 shrink-0 items-center rounded-full border px-0.5 transition-colors disabled:opacity-40",
        on ? "border-hairline-strong bg-ink/25" : "border-hairline bg-surface-2",
      )}
    >
      <span
        className={cn(
          "size-3.5 rounded-full transition-transform",
          on ? "translate-x-4 bg-ink" : "translate-x-0 bg-ink-mute",
        )}
      />
    </button>
  )
}

function Segmented<T extends string | number>({
  value,
  options,
  onChange,
}: {
  value: T
  options: ReadonlyArray<{ label: string; value: T }>
  onChange: (v: T) => void
}) {
  return (
    <div className="inline-flex overflow-hidden rounded-sm border border-hairline">
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          aria-pressed={o.value === value}
          onClick={() => onChange(o.value)}
          className={cn(
            "px-2.5 py-1 text-[0.6875rem] uppercase tracking-wide tabular-nums transition-colors",
            o.value === value ? "bg-surface-2 text-ink" : "text-ink-mute hover:text-ink-dim",
          )}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}

function Stat({ label, value, ink }: { label: string; value: string; ink?: string }) {
  return (
    <div className="flex items-center justify-between py-2">
      <span className="text-[0.6875rem] uppercase tracking-[0.12em] text-ink-mute">{label}</span>
      <span className={cn("tnum text-xs", ink ?? "text-ink")}>{value}</span>
    </div>
  )
}

// --- cards -------------------------------------------------------------------

function ConnectionCard({ telemetry, stats }: { telemetry: TelemetryState; stats: StatsState }) {
  const reachable = stats.status === "ok"
  const s = stats.data
  const isMock = telemetry.source === "mock"

  return (
    <SettingCard title="Connection & Session">
      <Row label="Source" hint="switch reloads the page">
        <div className="inline-flex overflow-hidden rounded-sm border border-hairline">
          <a
            href="?source=mock"
            aria-current={isMock ? "true" : undefined}
            className={cn(
              "px-2.5 py-1 text-[0.6875rem] uppercase tracking-wide transition-colors",
              isMock ? "bg-surface-2 text-caution" : "text-ink-mute hover:text-ink-dim",
            )}
          >
            Mock
          </a>
          <a
            href="?source=ws"
            aria-current={!isMock ? "true" : undefined}
            className={cn(
              "px-2.5 py-1 text-[0.6875rem] uppercase tracking-wide transition-colors",
              !isMock ? "bg-surface-2 text-ink" : "text-ink-mute hover:text-ink-dim",
            )}
          >
            Live
          </a>
        </div>
      </Row>

      <div className="py-1">
        <Stat label="Socket" value={String(telemetry.status).toUpperCase()} />
        <Stat
          label="Backend"
          value={reachable ? "REACHABLE" : stats.status === "loading" ? "…" : "UNREACHABLE"}
          ink={reachable ? "text-nominal" : stats.status === "loading" ? "text-ink-mute" : "text-caution"}
        />
        <Stat label="Session" value={s?.session ?? (isMock ? "— (mock)" : "—")} ink="text-ink-dim" />
        <Stat label="Frames decoded" value={s ? s.frames_decoded.toLocaleString() : "—"} />
        <Stat label="CRC errors" value={s ? String(s.crc_errors) : "—"} ink={s && s.crc_errors > 0 ? "text-caution" : "text-ink"} />
        <Stat label="Loss" value={s ? fmtPercent(s.loss_pct / 100, 2) : "—"} />
      </div>
    </SettingCard>
  )
}

function AlarmsCard({ alarm }: { alarm: AlarmSound }) {
  const { settings, update } = useSettings()
  return (
    <SettingCard title="Alarms & Audio">
      <Row label="Buzzer" hint={alarm.supported ? "master audio switch" : "Web Audio unavailable"}>
        <Toggle on={settings.buzzer} disabled={!alarm.supported} onChange={(buzzer) => update({ buzzer })} />
      </Row>
      <Row label="Arm · Link stale" hint="beep when the link goes stale">
        <Toggle on={settings.alarms.linkStale} onChange={(linkStale) => update({ alarms: { linkStale } })} />
      </Row>
      <Row label="Arm · No-deploy" hint="beep past apogee with no pyro">
        <Toggle on={settings.alarms.noDeploy} onChange={(noDeploy) => update({ alarms: { noDeploy } })} />
      </Row>
      <Row label="Test" hint="play a test beep + unlock audio">
        <Button variant="outline" size="sm" disabled={!alarm.supported} onClick={alarm.test}>
          Test buzzer
        </Button>
      </Row>
    </SettingCard>
  )
}

function DisplayCard() {
  const { settings, update } = useSettings()
  return (
    <SettingCard title="Display">
      <Row label="High contrast" hint="brighter ink + borders for sunlight">
        <Toggle on={settings.highContrast} onChange={(highContrast) => update({ highContrast })} />
      </Row>
      <Row label="Table density" hint="Log telemetry rows">
        <Segmented
          value={settings.table.density}
          onChange={(density) => update({ table: { density } })}
          options={[
            { label: "Comfortable", value: "comfortable" },
            { label: "Compact", value: "compact" },
          ]}
        />
      </Row>
      <Row label="Row cap" hint="max rows kept in the table">
        <Segmented
          value={settings.table.rowCap}
          onChange={(rowCap) => update({ table: { rowCap } })}
          options={ROW_CAP_OPTIONS.map((n) => ({ label: String(n), value: n }))}
        />
      </Row>
    </SettingCard>
  )
}

function ExportCard({ telemetry, stats }: { telemetry: TelemetryState; stats: StatsState }) {
  const hasSession = stats.status === "ok" && !!stats.data?.session
  const isMock = telemetry.source === "mock"

  const files: Array<{ label: string; path: string }> = [
    { label: "telemetry.csv", path: "/session/telemetry.csv" },
    { label: "events.csv", path: "/session/events.csv" },
    { label: "raw.log", path: "/session/raw.log" },
    { label: "stats.json", path: "/stats" },
  ]

  return (
    <SettingCard title="Data Export">
      <div className="py-3">
        <p className="mb-3 text-[0.6875rem] text-ink-mute">
          {hasSession
            ? "Download the active session for post-launch review (PLDR)."
            : isMock
              ? "No backend session — start the server (backend.app) and select Live to export."
              : "Backend not reachable — nothing to export yet."}
        </p>
        <div className="grid grid-cols-2 gap-2">
          {files.map((f) =>
            hasSession ? (
              <Button key={f.label} asChild variant="outline" size="sm" className="justify-start">
                <a href={apiUrl(f.path)} download>
                  {f.label}
                </a>
              </Button>
            ) : (
              <Button key={f.label} variant="outline" size="sm" className="justify-start" disabled>
                {f.label}
              </Button>
            ),
          )}
        </div>
      </div>
    </SettingCard>
  )
}

export function SettingsView({ telemetry, alarm }: { telemetry: TelemetryState; alarm: AlarmSound }) {
  const stats = useBackendStats() // one poll, shared by the connection + export cards
  return (
    <main className="min-h-0 flex-1 overflow-auto p-2">
      <div className="mx-auto grid max-w-5xl grid-cols-1 gap-2 md:grid-cols-2">
        <ConnectionCard telemetry={telemetry} stats={stats} />
        <AlarmsCard alarm={alarm} />
        <DisplayCard />
        <ExportCard telemetry={telemetry} stats={stats} />
      </div>
    </main>
  )
}
