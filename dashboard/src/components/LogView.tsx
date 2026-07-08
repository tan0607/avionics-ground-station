/**
 * LogView — the Log page: a live raw-telemetry table (left, main) beside a
 * derived mission event log (right). useTelemetryLog is called ONCE here (one
 * accumulator, one source of truth) and its output is handed to the two
 * presentational panels. It piggybacks on the telemetry snapshot without
 * touching the frozen data layer. One viewport: each panel scrolls internally,
 * the page never does.
 */
import { cn } from "@/lib/utils"
import { fmtFixed, fmtInt, fmtSigned, fmtTimer } from "@/lib/format"
import { FLIGHT_STATE_NAME, GpsFix } from "@/lib/protocol"
import type { TelemetryState } from "@/hooks/useTelemetry"
import { useTelemetryLog, type EventSeverity, type LogEvent, type LogRow } from "@/hooks/useTelemetryLog"
import { useSettings, type TableDensity } from "@/hooks/useSettings"
import { Card } from "@/components/ui/card"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table"

const FIX_NAME: Record<GpsFix, string> = {
  [GpsFix.NONE]: "NO",
  [GpsFix.FIX_2D]: "2D",
  [GpsFix.FIX_3D]: "3D",
}

const SEVERITY_INK: Record<EventSeverity, string> = {
  info: "text-ink-dim",
  nominal: "text-nominal",
  caution: "text-caution",
  alarm: "text-alarm",
}
const SEVERITY_DOT: Record<EventSeverity, string> = {
  info: "bg-ink-mute",
  nominal: "bg-nominal",
  caution: "bg-caution",
  alarm: "bg-alarm",
}

/** Compact CONTINUITY·PYRO·SD·ARMED flag strip — present letter bright, absent dim. */
function Flags({ row }: { row: LogRow }) {
  const cells: Array<[string, boolean, string]> = [
    ["C", row.continuity, "text-nominal"],
    ["P", row.pyroFired, "text-caution"],
    ["S", row.sdOk, "text-ink"],
    ["A", row.armed, "text-caution"],
  ]
  return (
    <span className="tnum tracking-[0.2em]">
      {cells.map(([ch, on, ink]) => (
        <span key={ch} className={on ? ink : "text-ink-mute/30"}>
          {ch}
        </span>
      ))}
    </span>
  )
}

function PanelHead({ title, right }: { title: string; right?: string }) {
  return (
    <div className="flex shrink-0 items-baseline justify-between border-b border-hairline px-3 py-2">
      <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">{title}</span>
      {right && <span className="tnum text-[0.625rem] text-ink-mute">{right}</span>}
    </div>
  )
}

function TelemetryTable({
  rows,
  rowCap,
  density,
}: {
  rows: LogRow[]
  rowCap: number
  density: TableDensity
}) {
  const pad = density === "compact" ? "py-0.5" : "py-1.5"

  return (
    <Card className="flex min-h-0 min-w-0 flex-col overflow-hidden">
      <PanelHead
        title="Telemetry · raw frames"
        right={rows.length ? `${rows.length} / ${rowCap} rows` : "awaiting link"}
      />
      {rows.length === 0 ? (
        <div className="flex flex-1 items-center justify-center text-xs text-ink-mute">
          awaiting link — no frames yet
        </div>
      ) : (
        <div className="min-h-0 flex-1 overflow-auto">
          <Table className="tnum">
            <TableHeader className="sticky top-0 z-10 bg-surface">
              <TableRow className="hover:bg-transparent">
                <TableHead>Seq</TableHead>
                <TableHead className="text-right">T · s</TableHead>
                <TableHead>State</TableHead>
                <TableHead className="text-right">Alt · m</TableHead>
                <TableHead className="text-right">V · m/s</TableHead>
                <TableHead className="text-right">Tilt</TableHead>
                <TableHead className="text-right">GPS</TableHead>
                <TableHead className="text-right">VBat</TableHead>
                <TableHead className="text-right">Flags</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {rows.map((r) => (
                <TableRow key={r.seq}>
                  <TableCell className={cn(pad, "text-ink-mute")}>{r.seq.toString().padStart(4, "0")}</TableCell>
                  <TableCell className={cn(pad, "text-right text-ink-dim")}>{(r.onboardMs / 1000).toFixed(1)}</TableCell>
                  <TableCell className={cn(pad, "text-ink")}>{FLIGHT_STATE_NAME[r.flightState]}</TableCell>
                  <TableCell className={cn(pad, "text-right text-ink")}>{fmtInt(r.baroAltM)}</TableCell>
                  <TableCell className={cn(pad, "text-right text-ink")}>{fmtSigned(r.vspeedMs, 1)}</TableCell>
                  <TableCell className={cn(pad, "text-right text-ink-dim")}>{fmtInt(r.tiltDeg)}°</TableCell>
                  <TableCell className={cn(pad, "text-right text-ink-dim")}>
                    {r.gpsSats}·{FIX_NAME[r.gpsFix] ?? r.gpsFix}
                  </TableCell>
                  <TableCell className={cn(pad, "text-right text-ink-dim")}>{fmtFixed(r.vbatV, 1)}</TableCell>
                  <TableCell className={cn(pad, "text-right")}>
                    <Flags row={r} />
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      )}
    </Card>
  )
}

function EventLog({ events }: { events: LogEvent[] }) {
  return (
    <Card className="flex min-h-0 flex-col overflow-hidden">
      <PanelHead title="Event Log" right={events.length ? `${events.length}` : undefined} />
      {events.length === 0 ? (
        <div className="flex flex-1 items-center justify-center text-xs text-ink-mute">no events yet</div>
      ) : (
        <ul className="min-h-0 flex-1 divide-y divide-hairline overflow-auto">
          {events.map((e) => (
            <li key={e.id} className="flex items-baseline gap-2.5 px-3 py-1.5">
              <span aria-hidden className={cn("mt-1.5 size-1.5 shrink-0 rounded-full", SEVERITY_DOT[e.severity])} />
              <span className="tnum w-[4.5rem] shrink-0 text-[0.6875rem] text-ink-mute">
                {e.tPlusSec != null ? fmtTimer(e.tPlusSec) : `${(e.onboardMs / 1000).toFixed(1)}s`}
              </span>
              <span className={cn("text-[0.6875rem] uppercase tracking-wide", SEVERITY_INK[e.severity])}>
                {e.message}
              </span>
              {e.detail && <span className="tnum ml-auto text-[0.6875rem] text-ink-mute">{e.detail}</span>}
            </li>
          ))}
        </ul>
      )}
    </Card>
  )
}

export function LogView({ telemetry }: { telemetry: TelemetryState }) {
  const log = useTelemetryLog(telemetry)
  const { settings } = useSettings()
  const rows = log.rows.slice(0, settings.table.rowCap)

  return (
    <main className="grid min-h-0 flex-1 grid-cols-[1fr_20rem] gap-2 p-2">
      <TelemetryTable rows={rows} rowCap={settings.table.rowCap} density={settings.table.density} />
      <EventLog events={log.events} />
    </main>
  )
}
