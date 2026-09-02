/**
 * LogView — the Log page: a live raw-telemetry table (left, main) beside a
 * derived mission event log (right). This file is PRESENTATION ONLY: the single
 * accumulator lives in App (useTelemetryLog) and arrives here as a prop, so it
 * keeps recording on every view instead of only while this page is mounted. It
 * piggybacks on the telemetry snapshot without touching the frozen data layer.
 * One viewport: each panel scrolls internally, the page never does.
 *
 * The telemetry table carries the protocol fields, then radio quality, then one
 * column per aux field the downlink is currently sending — the same surplus the
 * Live view's Aux strip shows, which this table did not record at all. It
 * scrolls sideways rather than dropping columns: the point of this view is that
 * nothing decoded is missing from it.
 */
import { useEffect, useState, type ReactNode } from "react"
import { Eraser } from "lucide-react"
import { cn } from "@/lib/utils"
import { fmtFixed, fmtInt, fmtSigned, fmtTimer } from "@/lib/format"
import { FLIGHT_STATE_NAME, GpsFix } from "@/lib/protocol"
import type {
  EventSeverity,
  LogEvent,
  LogRecording,
  LogRow,
  TelemetryLog,
} from "@/hooks/useTelemetryLog"
import { useSettings, type TableDensity } from "@/hooks/useSettings"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table"

const FIX_NAME: Record<GpsFix, string> = {
  [GpsFix.NONE]: "NO",
  [GpsFix.FIX_2D]: "2D",
  [GpsFix.FIX_3D]: "3D",
}

/**
 * Aux columns, discovered from the rows on screen rather than declared here.
 *
 * The Live view's Aux strip is data-driven off the frame for a reason — this
 * transmitter renames and adds fields between builds — and the log has to
 * follow the same rule or it goes stale the same way. A field the current
 * downlink doesn't send gets no column at all, rather than a column of dashes.
 *
 * ORDER is fixed for the keys we know, then alphabetical for the rest, so the
 * columns don't reshuffle mid-flight as `extra` gains a key.
 */
const AUX_ORDER = ["P", "HDG", "COURSE", "GSPEED", "AZ", "AX", "AY", "VX", "VY", "TEMP"]
/** Consumed into the GPS health row; a bare 1 in a column means nothing here. */
const AUX_HIDDEN = new Set(["GPSDATA"])

/** Shorter than the Aux strip's labels — these are column heads, not readouts. */
const AUX_LABEL: Record<string, string> = {
  P: "Press",
  HDG: "Hdg",
  COURSE: "Crs",
  GSPEED: "GSpd",
  TEMP: "Temp",
}

const AUX_DIGITS: Record<string, number> = { P: 0, HDG: 0, COURSE: 0 }

function auxColumns(rows: LogRow[]): string[] {
  const seen = new Set<string>()
  for (const r of rows) {
    for (const k of Object.keys(r.extra)) if (!AUX_HIDDEN.has(k)) seen.add(k)
  }
  const ranked = AUX_ORDER.filter((k) => seen.has(k))
  const rest = [...seen].filter((k) => !AUX_ORDER.includes(k)).sort()
  return [...ranked, ...rest]
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

/**
 * The REC gutter — which rows are in a flight folder and which are not.
 *
 * The Log view used to have no relation to the archive whatsoever: a thousand
 * rows on screen and no way to tell which of them exist on disk under a name.
 * A filled mark means this frame was written into the flight named in the
 * tooltip; a hairline means it was only ever in the browser. The transition
 * between the two IS the boundary, which is the thing that was missing.
 */
function RecMark({ flight }: { flight: string | null }) {
  return (
    <span
      title={flight ? `recorded into ${flight}` : "not recording — this frame is only in the browser"}
      className={cn(
        "inline-block h-3 w-0.5 rounded-full align-middle",
        flight ? "bg-caution" : "bg-ink-mute/20",
      )}
    />
  )
}

/**
 * The four flag letters, spelled out.
 *
 * The column used to be headed "Flags" and the letters explained nowhere, so
 * what C·P·S·A stands for was something you had to already know — and stop
 * knowing. The header now carries the letters themselves and every letter
 * carries its own name, on the row as well as in the head.
 */
const FLAG_NAME = {
  C: "Continuity — pyro circuit is closed",
  P: "Pyro fired",
  S: "SD log healthy",
  A: "Armed",
} as const

const FLAG_LEGEND = "C continuity · P pyro fired · S SD log OK · A armed — bright letter = true"

/** Compact CONTINUITY·PYRO·SD·ARMED flag strip — present letter bright, absent dim. */
function Flags({ row }: { row: LogRow }) {
  const cells: Array<[keyof typeof FLAG_NAME, boolean, string]> = [
    ["C", row.continuity, "text-nominal"],
    ["P", row.pyroFired, "text-caution"],
    ["S", row.sdOk, "text-ink"],
    ["A", row.armed, "text-caution"],
  ]
  return (
    <span className="tnum tracking-[0.2em]">
      {cells.map(([ch, on, ink]) => (
        <span
          key={ch}
          title={`${FLAG_NAME[ch]} — ${on ? "yes" : "no"}`}
          className={on ? ink : "text-ink-mute/30"}
        >
          {ch}
        </span>
      ))}
    </span>
  )
}

function PanelHead({
  title,
  right,
  badge,
  action,
}: {
  title: string
  right?: string
  badge?: ReactNode
  action?: ReactNode
}) {
  return (
    <div className="flex shrink-0 items-center justify-between gap-2 border-b border-hairline px-3 py-1.5">
      <span className="flex items-center gap-2 truncate text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">
        {title}
        {badge}
      </span>
      <span className="flex shrink-0 items-center gap-2">
        {right && <span className="tnum text-[0.625rem] text-ink-mute">{right}</span>}
        {action}
      </span>
    </div>
  )
}

/**
 * Clear the operator's working copy of the log.
 *
 * Armed like every other destructive control in this console: one click asks,
 * a second inside three seconds does it, and it disarms itself. Cheap to arm
 * because what it destroys is recoverable in principle — telemetry.csv on the
 * backend is the authoritative record — but it is the only copy of what THIS
 * console has seen, including the frames that arrived while REC was off.
 */
const CLEAR_ARM_MS = 3000

function ClearLog({ rows, events, onClear }: { rows: number; events: number; onClear: () => void }) {
  const [armed, setArmed] = useState(false)

  useEffect(() => {
    if (!armed) return
    const timer = window.setTimeout(() => setArmed(false), CLEAR_ARM_MS)
    return () => window.clearTimeout(timer)
  }, [armed])

  const total = rows + events
  return (
    <Button
      variant="ghost"
      size="xs"
      disabled={total === 0}
      onClick={() => {
        if (!armed) {
          setArmed(true)
          return
        }
        setArmed(false)
        onClear()
      }}
      aria-label={armed ? "Confirm clearing the log" : "Clear the log"}
      title={
        armed
          ? "Click again to discard every row and event on this console"
          : `Clear ${rows.toLocaleString()} rows and ${events.toLocaleString()} events. The backend's telemetry.csv is not touched.`
      }
      className={cn(
        "gap-1 text-[0.5625rem] uppercase tracking-[0.12em]",
        armed ? "text-alarm hover:text-alarm" : "text-ink-mute hover:text-ink-dim",
      )}
    >
      <Eraser aria-hidden />
      {armed ? "Confirm" : "Clear"}
    </Button>
  )
}

function TelemetryTable({
  rows,
  rowCap,
  density,
  recording,
  action,
}: {
  rows: LogRow[]
  rowCap: number
  density: TableDensity
  recording: LogRecording
  action?: ReactNode
}) {
  const pad = density === "compact" ? "py-0.5" : "py-1.5"
  const aux = auxColumns(rows)

  return (
    <Card className="flex min-h-0 min-w-0 flex-col overflow-hidden">
      <PanelHead
        title="Telemetry · raw frames"
        right={rows.length ? `${rows.length} / ${rowCap} rows` : "awaiting link"}
        action={action}
        badge={
          recording.recording ? (
            <span className="flex items-center gap-1 text-[0.5625rem] uppercase tracking-[0.12em] text-caution">
              <span className="size-1.5 animate-pulse rounded-full bg-caution" />
              {recording.flight ?? "recording"}
            </span>
          ) : undefined
        }
      />
      {rows.length === 0 ? (
        <div className="flex flex-1 items-center justify-center text-xs text-ink-mute">
          awaiting link — no frames yet
        </div>
      ) : (
        <div className="min-h-0 flex-1 overflow-auto">
          {/* min-w-max so the aux columns extend the table and the container
              scrolls, instead of `w-full` compressing every column to fit. */}
          <Table className="tnum min-w-max">
            <TableHeader className="sticky top-0 z-10 bg-surface">
              <TableRow className="hover:bg-transparent">
                <TableHead className="w-4 px-1" title="Recorded into a flight folder">
                  <span className="sr-only">Recorded</span>
                </TableHead>
                <TableHead>Seq</TableHead>
                <TableHead className="text-right">T · s</TableHead>
                <TableHead>State</TableHead>
                <TableHead className="text-right">Alt · m</TableHead>
                <TableHead className="text-right">V · m/s</TableHead>
                <TableHead className="text-right">Tilt</TableHead>
                <TableHead className="text-right">GPS</TableHead>
                <TableHead className="text-right">VBat</TableHead>
                <TableHead className="text-right" title={FLAG_LEGEND}>
                  <span className="tracking-[0.14em]">C·P·S·A</span>
                </TableHead>
                <TableHead className="text-right">RSSI</TableHead>
                <TableHead className="text-right">SNR</TableHead>
                {aux.map((k) => (
                  <TableHead key={k} className="text-right">
                    {AUX_LABEL[k] ?? k}
                  </TableHead>
                ))}
              </TableRow>
            </TableHeader>
            <TableBody>
              {rows.map((r) => (
                // key MUST be r.id, never r.seq: seq wraps, restarts on a flight
                // computer reboot, and repeats when a transmitter sends each
                // packet twice. Duplicate keys make React reuse the wrong rows
                // and the log renders blocks of itself over and over.
                <TableRow key={r.id}>
                  <TableCell className={cn(pad, "w-4 px-1")}>
                    {/* Rows restored from an older localStorage log predate this
                        field entirely — undefined is "unknown", same as off. */}
                    <RecMark flight={r.flight ?? null} />
                  </TableCell>
                  <TableCell className={cn(pad, "text-ink-mute")}>
                    {r.seq.toString().padStart(4, "0")}
                    {/* The same packet heard twice is a real second reception, so
                        it stays in the log — marked, not hidden, and not silently
                        collapsed into one row. */}
                    {r.duplicate && <span className="ml-1 text-ink-mute/50" title="repeat of previous seq">·2</span>}
                  </TableCell>
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
                  {/* A source that doesn't measure these renders "—", not 0 —
                      0 dBm would read as an impossibly strong signal. */}
                  <TableCell className={cn(pad, "text-right text-ink-mute")}>
                    {fmtInt(r.rssiDbm)}
                  </TableCell>
                  <TableCell className={cn(pad, "text-right text-ink-mute")}>
                    {fmtFixed(r.snrDb, 1)}
                  </TableCell>
                  {aux.map((k) => (
                    <TableCell key={k} className={cn(pad, "text-right text-ink-dim")}>
                      {/* Absent from THIS frame while present in others is a
                          real distinction — an em dash, never a 0. */}
                      {k in r.extra ? fmtFixed(r.extra[k], AUX_DIGITS[k] ?? 2) : "—"}
                    </TableCell>
                  ))}
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

/**
 * `log` is a PROP, not a hook call. The accumulator lives in App so it keeps
 * recording while the operator is on the Live or Map view — mounting it here
 * meant the log only existed while it was being looked at.
 */
export function LogView({ log, recording }: { log: TelemetryLog; recording: LogRecording }) {
  const { settings } = useSettings()
  const rows = log.rows.slice(0, settings.table.rowCap)

  return (
    <main className="grid min-h-0 flex-1 grid-cols-[1fr_20rem] gap-2 p-2">
      <TelemetryTable
        rows={rows}
        rowCap={settings.table.rowCap}
        density={settings.table.density}
        recording={recording}
        action={
          <ClearLog rows={log.rows.length} events={log.events.length} onClear={log.clear} />
        }
      />
      <EventLog events={log.events} />
    </main>
  )
}
