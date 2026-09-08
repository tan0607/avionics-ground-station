/**
 * FlightsView — persistent, read-only access to the recorded archive.
 *
 * The index lists SESSIONS with the operator-declared flights nested inside
 * them, because listing only the flights hid almost everything: on this machine
 * 29 of 31 session folders had nobody press REC in them, and two of those held
 * the longest serial captures in the archive. A session is the same five
 * durable files a flight is, so it previews, downloads and deletes through the
 * same routes — see SESSION_SELF in backend/session.py.
 *
 * Every row states its PROVENANCE. Without it a replay of a simulator run was
 * presented identically to a launch, and the archive here contains a replay of
 * a replay of a simulated flight. Nothing else in the row can tell you that.
 *
 * The right pane shows bounded tails rather than loading a whole multi-hour CSV
 * into memory.
 */
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react"
import { Download, RefreshCw, Trash2 } from "lucide-react"
import { cn } from "@/lib/utils"
import {
  recordedFlightFileUrl,
  useRecordedFlightDetail,
  useRecordedFlights,
  type ArchiveState,
  type ProvenanceKind,
  type RecordedFlight,
  type RecordedFlightDetail,
} from "@/hooks/useRecordedFlights"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table"

const CORE_COLUMNS = [
  "host_time",
  "seq",
  "onboard_ms",
  "flight_state",
  "baro_alt_m",
  "vspeed_ms",
  "rssi_dbm",
  "snr_db",
  "lat_deg",
  "lon_deg",
]
const EMPTY_FLIGHTS: RecordedFlight[] = []
const NUMERIC_COLUMNS = new Set([
  "seq", "onboard_ms", "baro_alt_m", "vspeed_ms", "rssi_dbm", "snr_db",
  "lat_deg", "lon_deg", "gps_lat", "gps_lon",
])

function flightId(flight: RecordedFlight): string {
  return `${flight.session}/${flight.flight}`
}

function localTime(value: string | null | undefined): string {
  if (!value) return "—"
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return new Intl.DateTimeFormat("en-GB", {
    day: "2-digit",
    month: "short",
    year: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(date)
}

/** Date and clock, split — the index leads with the date, rows lead with the clock. */
function localParts(value: string | null | undefined): { date: string; clock: string } | null {
  if (!value) return null
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return null
  return {
    date: new Intl.DateTimeFormat("en-GB", { day: "2-digit", month: "short" }).format(date),
    clock: new Intl.DateTimeFormat("en-GB", {
      hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
    }).format(date),
  }
}

function duration(seconds: number | null): string {
  if (seconds == null || !Number.isFinite(seconds)) return "—"
  const whole = Math.max(0, Math.round(seconds))
  const hours = Math.floor(whole / 3600)
  const minutes = Math.floor((whole % 3600) / 60)
  const secs = whole % 60
  return hours
    ? `${hours}:${String(minutes).padStart(2, "0")}:${String(secs).padStart(2, "0")}`
    : `${minutes}:${String(secs).padStart(2, "0")}`
}

function bytes(value: number): string {
  if (value < 1024) return `${value} B`
  if (value < 1024 ** 2) return `${(value / 1024).toFixed(1)} KB`
  if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MB`
  return `${(value / 1024 ** 3).toFixed(2)} GB`
}

/**
 * The identity a row leads with.
 *
 * NEVER the folder name on its own. `flight-NN` is a per-session counter that
 * restarts at 01 every session, so the archive showed two different recordings
 * both called "flight-01" and nothing else to separate them. The operator's
 * label wins when there is one; the wall-clock time when there is not. The
 * folder name still appears beside it, dim — it is the route identity, just not
 * a name.
 */
function rowTitle(row: RecordedFlight): string {
  if (row.label) return row.label
  const parts = localParts(row.started_utc)
  if (!parts) return row.kind === "session" ? row.session : row.flight
  return row.kind === "session" ? `${parts.date} · ${parts.clock}` : parts.clock
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------
const PROVENANCE_LABEL: Record<ProvenanceKind, string> = {
  live: "LIVE",
  replay: "REPLAY",
  sim: "SIM",
  unknown: "?",
}

/**
 * Colour carries the meaning here, so it follows the console's existing rule:
 * green is the real thing, amber is synthetic, dim is a copy. A REPLAY badge
 * must never read as more authoritative than a LIVE one.
 */
const PROVENANCE_INK: Record<ProvenanceKind, string> = {
  live: "border-nominal/40 bg-nominal-bg text-nominal",
  replay: "border-hairline bg-surface-2 text-ink-mute",
  sim: "border-caution/40 bg-caution-bg text-caution",
  unknown: "border-hairline bg-surface-2 text-ink-mute",
}

const PROVENANCE_TITLE: Record<ProvenanceKind, string> = {
  live: "Captured from a radio on a serial port",
  replay: "Re-run of an older raw.log — not a capture",
  sim: "Generated by fake_telemetry — no vehicle involved",
  unknown: "This folder's metadata does not say where its bytes came from",
}

function ProvenanceBadge({ kind, className }: { kind: ProvenanceKind; className?: string }) {
  return (
    <span
      title={PROVENANCE_TITLE[kind]}
      className={cn(
        "shrink-0 rounded-sm border px-1 py-px text-[0.5rem] font-medium uppercase tracking-[0.12em]",
        PROVENANCE_INK[kind],
        className,
      )}
    >
      {PROVENANCE_LABEL[kind]}
    </span>
  )
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------
type KindFilter = "all" | ProvenanceKind
const KIND_FILTERS: Array<{ id: KindFilter; label: string }> = [
  { id: "all", label: "All" },
  { id: "live", label: "Live" },
  { id: "replay", label: "Replay" },
  { id: "sim", label: "Sim" },
]

/**
 * Two controls, both earning their place against this archive: a third of the
 * folders are replays or simulator runs competing for attention with captures,
 * and five more hold zero rows because a start failed. Empty is hidden by
 * DEFAULT — an empty folder is the one thing in here that can never be the
 * flight you are looking for.
 */
function ArchiveFilters({
  kind,
  onKind,
  hideEmpty,
  onHideEmpty,
  counts,
  hidden,
}: {
  kind: KindFilter
  onKind: (k: KindFilter) => void
  hideEmpty: boolean
  onHideEmpty: (v: boolean) => void
  counts: Record<KindFilter, number>
  hidden: number
}) {
  return (
    <div className="shrink-0 border-b border-hairline px-2 py-1.5">
      <div className="flex gap-1" role="group" aria-label="Filter archive by provenance">
        {KIND_FILTERS.map((f) => {
          const active = kind === f.id
          return (
            <button
              key={f.id}
              type="button"
              aria-pressed={active}
              disabled={counts[f.id] === 0 && !active}
              onClick={() => onKind(f.id)}
              className={cn(
                "flex-1 rounded-sm border px-1 py-1 text-[0.5625rem] uppercase tracking-[0.1em] transition-colors",
                "focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring",
                "disabled:cursor-not-allowed disabled:opacity-30",
                active
                  ? "border-ink-mute bg-surface-2 text-ink"
                  : "border-hairline text-ink-mute hover:text-ink-dim",
              )}
            >
              {f.label}
              <span className="tnum ml-1 opacity-60">{counts[f.id]}</span>
            </button>
          )
        })}
      </div>
      <label className="mt-1.5 flex cursor-pointer items-center gap-1.5 text-[0.5625rem] text-ink-mute hover:text-ink-dim">
        <input
          type="checkbox"
          checked={hideEmpty}
          onChange={(e) => onHideEmpty(e.target.checked)}
          className="size-3 accent-ink-mute"
        />
        <span className="uppercase tracking-[0.1em]">Hide empty</span>
        {hideEmpty && hidden > 0 && (
          <span className="tnum ml-auto opacity-70">{hidden} hidden</span>
        )}
      </label>
    </div>
  )
}

function SkeletonRows() {
  return (
    <div aria-label="Loading recorded flights" className="divide-y divide-hairline">
      {[0, 1, 2].map((row) => (
        <div key={row} className="space-y-2 px-3 py-3">
          <div className="h-3 w-24 animate-pulse rounded-sm bg-surface-2" />
          <div className="h-2.5 w-40 animate-pulse rounded-sm bg-surface-2" />
        </div>
      ))}
    </div>
  )
}

// ---------------------------------------------------------------------------
// Index
// ---------------------------------------------------------------------------
interface SessionGroup {
  session: RecordedFlight
  flights: RecordedFlight[]
}

function ArchiveRow({
  row,
  nested,
  selected,
  onSelect,
}: {
  row: RecordedFlight
  nested: boolean
  selected: boolean
  onSelect: (row: RecordedFlight) => void
}) {
  const empty = row.rows === 0
  const origin = row.provenance.origin
  return (
    <button
      type="button"
      aria-pressed={selected}
      data-archive-id={flightId(row)}
      onClick={() => onSelect(row)}
      className={cn(
        "relative w-full border-b border-hairline py-2 pr-3 pl-6 text-left outline-none transition-colors",
        "focus-visible:bg-surface-2 focus-visible:ring-1 focus-visible:ring-inset focus-visible:ring-ring",
        selected
          ? "bg-surface-2 text-ink ring-1 ring-inset ring-ink-mute"
          : "text-ink-dim hover:bg-surface-2/60 hover:text-ink",
      )}
    >
      {nested && <span aria-hidden className="absolute left-2 top-2 text-xs text-ink-mute">↳</span>}
      <div className="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-2">
        <span className={cn("tnum truncate text-xs", empty ? "text-ink-mute" : "font-medium")}>
          {rowTitle(row)}
        </span>
        <span className="flex items-center justify-end gap-1.5">
          <span aria-hidden={!row.recording} className={cn("flex w-8 shrink-0 items-center gap-1 text-[0.5625rem] uppercase text-caution", !row.recording && "invisible")}>
            <span className="size-1.5 rounded-full bg-caution" />
            rec
          </span>
          <ProvenanceBadge kind={row.provenance.kind} />
        </span>
      </div>
      <div className="tnum mt-1 flex min-w-0 items-baseline gap-2 text-[0.625rem] text-ink-mute">
        <span className="min-w-0 truncate" title={row.kind === "session" ? row.session : row.flight}>{row.kind === "session" ? row.session : row.flight}</span>
        {/* A replay names what it re-ran, so it can never be mistaken for the
            capture it copied — these nest three deep in this archive. */}
        {origin && <span className="truncate opacity-70">← {origin}</span>}
        {row.flight_count > 0 && (
          <span className="ml-auto shrink-0 text-ink-dim">
            {row.flight_count} flight{row.flight_count === 1 ? "" : "s"}
          </span>
        )}
      </div>
      <div className="tnum mt-1 grid grid-cols-[minmax(0,1fr)_minmax(0,1.4fr)_minmax(0,1fr)] gap-x-2 text-right text-[0.625rem] text-ink-mute [&>span]:min-w-0 [&>span]:break-words">
        <span>{duration(row.duration_s)}</span>
        <span className={cn("text-right", empty && "text-ink-mute/60")}>
          {empty ? "no rows" : `${row.rows.toLocaleString()} rows`}
        </span>
        <span className="text-right">{bytes(row.raw_bytes)}</span>
      </div>
    </button>
  )
}

function ArchiveIndex({
  groups,
  selectedId,
  onSelect,
}: {
  groups: SessionGroup[]
  selectedId: string | null
  onSelect: (row: RecordedFlight) => void
}) {
  const list = useRef<HTMLDivElement>(null)
  const anchor = useRef<{ id: string; top: number } | null>(null)
  const previous = useRef<Set<string> | null>(null)
  const [newIds, setNewIds] = useState<string[]>([])
  const captureAnchor = () => {
    const el = list.current
    if (!el || el.scrollTop < 4) { anchor.current = null; return }
    const top = el.getBoundingClientRect().top
    const first = Array.from(el.querySelectorAll<HTMLElement>("[data-archive-id]"))
      .find((row) => row.getBoundingClientRect().bottom > top)
    anchor.current = first ? { id: first.dataset.archiveId!, top: first.getBoundingClientRect().top - top } : null
  }
  useLayoutEffect(() => {
    const ids = new Set(groups.flatMap((group) => [flightId(group.session), ...group.flights.map(flightId)]))
    const added = previous.current ? [...ids].filter((id) => !previous.current!.has(id)) : []
    const el = list.current
    if (el && anchor.current) {
      const saved = anchor.current
      const row = Array.from(el.querySelectorAll<HTMLElement>("[data-archive-id]"))
        .find((item) => item.dataset.archiveId === saved.id)
      if (row) el.scrollTop += row.getBoundingClientRect().top - el.getBoundingClientRect().top - saved.top
      if (added.length) setNewIds((current) => [...new Set([...current, ...added])])
    }
    previous.current = ids
    captureAnchor()
  }, [groups])
  return (
    <div className="relative flex min-h-0 flex-1 flex-col">
    {newIds.length > 0 && <button type="button" className="absolute right-2 top-1 z-10 border border-ink-mute bg-surface px-3 py-1 text-xs text-ink" onClick={() => {
      const rows = groups.flatMap((group) => [group.session, ...group.flights])
      const target = rows.find((row) => newIds.includes(flightId(row)))
      if (target) {
        onSelect(target)
        const node = Array.from(list.current?.querySelectorAll<HTMLElement>("[data-archive-id]") ?? [])
          .find((item) => item.dataset.archiveId === flightId(target))
        node?.scrollIntoView({ block: "nearest" })
      }
      setNewIds([])
      captureAnchor()
    }}>↑ New recordings · {newIds.length}</button>}
    <div ref={list} onScroll={captureAnchor} className="min-h-0 flex-1 overflow-auto [overflow-anchor:none]">
      {groups.map((group) => (
        <div key={group.session.session}>
          <ArchiveRow
            row={group.session}
            nested={false}
            selected={flightId(group.session) === selectedId}
            onSelect={onSelect}
          />
          {group.flights.map((flight) => (
            <ArchiveRow
              key={flightId(flight)}
              row={flight}
              nested
              selected={flightId(flight) === selectedId}
              onSelect={onSelect}
            />
          ))}
        </div>
      ))}
    </div>
    </div>
  )
}

function EmptyArchive({ root, filtered }: { root?: string; filtered: boolean }) {
  return (
    <div className="flex min-h-0 flex-1 flex-col items-center justify-center px-6 text-center">
      <div className="text-xs uppercase tracking-[0.14em] text-ink-dim">
        {filtered ? "Nothing matches this filter" : "No recordings"}
      </div>
      <p className="mt-2 max-w-sm text-[0.6875rem] leading-relaxed text-ink-mute">
        {filtered
          ? "Every recording on disk is filtered out. Widen the provenance filter, or show empty folders."
          : "The backend records into a session folder whenever it runs. Press REC in the top bar to cut a named flight out of one."}
      </p>
      {root && !filtered && (
        <code className="tnum mt-3 max-w-full break-all text-[0.625rem] text-ink-mute">{root}</code>
      )}
    </div>
  )
}

/** A session opening this long before its first frame is worth reporting. */
const OPENED_EARLY_S = 60

function gapSeconds(from: string | null | undefined, to: string | null | undefined): number {
  if (!from || !to) return 0
  const a = new Date(from).getTime()
  const b = new Date(to).getTime()
  if (Number.isNaN(a) || Number.isNaN(b)) return 0
  return Math.abs(b - a) / 1000
}

function Metric({ label, value, ink }: { label: string; value: string; ink?: string }) {
  return (
    <div className="min-w-0 border-r border-hairline px-3 py-2 last:border-r-0">
      <div className="text-[0.5625rem] uppercase tracking-[0.14em] text-ink-mute">{label}</div>
      <div className={cn("tnum mt-1 break-words text-xs", ink ?? "text-ink")} title={value}>
        {value}
      </div>
    </div>
  )
}

function missionInk(line: string): string {
  if (/\salarm\s/i.test(line)) return "text-alarm"
  if (/\scaution\s/i.test(line)) return "text-caution"
  if (/\snominal\s/i.test(line)) return "text-nominal"
  return "text-ink-dim"
}

function MissionPreview({ detail }: { detail: RecordedFlightDetail }) {
  return (
    <section className="flex min-h-0 flex-col border-b border-hairline">
      <div className="flex shrink-0 items-baseline justify-between border-b border-hairline px-3 py-1.5">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Mission log · latest</span>
        <span className="tnum text-[0.5625rem] text-ink-mute">
          {detail.mission.lines.length} lines{detail.mission.truncated ? " · tail" : ""}
        </span>
      </div>
      <div className="tnum min-h-0 flex-1 overflow-auto px-3 py-2 text-[0.6875rem] leading-5">
        {detail.mission.lines.length ? detail.mission.lines.map((line, index) => (
          <div key={`${index}-${line}`} className={cn("min-w-max whitespace-pre", missionInk(line))}>{line || " "}</div>
        )) : <div className="text-ink-mute">No mission events were written.</div>}
      </div>
    </section>
  )
}

function TelemetryPreview({ detail }: { detail: RecordedFlightDetail }) {
  const columns = useMemo(() => {
    const core = CORE_COLUMNS.filter((column) => detail.telemetry.columns.includes(column))
    const rest = detail.telemetry.columns.filter((column) => !core.includes(column))
    return [...core, ...rest].slice(0, 10)
  }, [detail.telemetry.columns])

  return (
    <section className="flex min-h-0 flex-col">
      <div className="flex shrink-0 items-baseline justify-between border-b border-hairline px-3 py-1.5">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Telemetry · latest frames</span>
        <span className="tnum text-[0.5625rem] text-ink-mute">
          {detail.telemetry.rows.length} rows{detail.telemetry.truncated ? " · tail" : ""}
        </span>
      </div>
      <div className="min-h-0 flex-1 overflow-auto">
        {detail.telemetry.rows.length ? (
          <Table className="tnum min-w-max text-[0.6875rem]">
            <TableHeader className="sticky top-0 z-10 bg-surface">
              <TableRow className="hover:bg-transparent">
                {columns.map((column) => (
                  <TableHead key={column} className={NUMERIC_COLUMNS.has(column) ? "text-right" : undefined}>{column}</TableHead>
                ))}
              </TableRow>
            </TableHeader>
            <TableBody>
              {detail.telemetry.rows.map((row, index) => (
                <TableRow key={`${row.host_time ?? "row"}-${row.seq ?? index}-${index}`}>
                  {columns.map((column) => (
                    <TableCell key={column} className={cn("py-1 text-ink-dim", NUMERIC_COLUMNS.has(column) && "text-right")}>{row[column] || "—"}</TableCell>
                  ))}
                </TableRow>
              ))}
            </TableBody>
          </Table>
        ) : (
          <div className="flex h-full items-center justify-center text-xs text-ink-mute">No telemetry rows were written.</div>
        )}
      </div>
    </section>
  )
}

/**
 * DeleteFlight — the one control in this console that destroys flight data.
 *
 * Armed, not immediate: the first click turns the button into CONFIRM and it
 * disarms itself after a few seconds, so a misclick in the archive cannot take
 * a recording with it. The backend refuses three cases outright — the flight
 * that is recording right now, the session this backend is writing into, and
 * any folder holding a file the ground station did not write. A session holding
 * flight folders is refused by that last rule, so emptying a session is always
 * deliberate: its flights go first. Whatever the backend says comes back onto
 * the screen verbatim rather than being flattened into "failed".
 */
const CONFIRM_MS = 4000

function DeleteFlight({
  flight,
  remove,
  onDeleted,
}: {
  flight: RecordedFlight
  remove: ArchiveState["remove"]
  onDeleted: () => void
}) {
  const [armed, setArmed] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const noun = flight.kind === "session" ? `session ${flight.session}` : flight.flight

  // Re-arming has to reset when the operator selects a different recording, or
  // the confirm they armed on one row would apply to the next one they click.
  const id = flightId(flight)
  useEffect(() => {
    setArmed(false)
    setError(null)
  }, [id])

  useEffect(() => {
    if (!armed) return
    const timer = window.setTimeout(() => setArmed(false), CONFIRM_MS)
    return () => window.clearTimeout(timer)
  }, [armed])

  const onClick = async () => {
    if (busy) return
    if (!armed) {
      setArmed(true)
      return
    }
    setArmed(false)
    setBusy(true)
    const result = await remove(flight)
    setBusy(false)
    if (result.ok) onDeleted()
    else setError(result.error ?? "delete failed")
  }

  return (
    <span className="flex items-center gap-2">
      {error && (
        <span role="alert" className="max-w-[18rem] truncate text-[0.625rem] text-alarm" title={error}>
          {error}
        </span>
      )}
      <Button
        variant="outline"
        size="xs"
        onClick={onClick}
        disabled={busy}
        aria-label={armed ? `Confirm deleting ${noun}` : `Delete ${noun}`}
        title={
          armed
            ? "Click again to permanently delete this folder"
            : `Permanently delete ${noun}`
        }
        className={cn(
          armed
            ? "border-alarm/60 bg-alarm/10 text-alarm hover:bg-alarm/20"
            : "text-ink-mute hover:text-alarm",
        )}
      >
        <Trash2 aria-hidden />
        {armed ? "Confirm" : busy ? "Deleting…" : "Delete"}
      </Button>
    </span>
  )
}

function DetailPane({
  flight,
  remove,
  onDeleted,
}: {
  flight: RecordedFlight
  remove: ArchiveState["remove"]
  onDeleted: () => void
}) {
  const detail = useRecordedFlightDetail(flight)

  if (!detail.data && (detail.status === "loading" || detail.status === "idle")) {
    return (
      <div aria-label="Loading flight detail" className="flex flex-1 flex-col gap-3 p-4">
        <div className="h-4 w-40 animate-pulse rounded-sm bg-surface-2" />
        <div className="h-10 animate-pulse rounded-sm bg-surface-2" />
        <div className="min-h-0 flex-1 animate-pulse rounded-sm bg-surface-2" />
      </div>
    )
  }

  if (!detail.data) {
    return (
      <div className="flex min-h-0 flex-1 flex-col items-center justify-center gap-3 text-center">
        <div className="text-xs uppercase tracking-[0.14em] text-caution">Recording detail unavailable</div>
        <Button variant="outline" size="sm" onClick={detail.refresh}>Retry</Button>
      </div>
    )
  }

  const data = detail.data
  const codec = typeof data.packet.codec === "string" ? data.packet.codec.toUpperCase() : "—"
  const isSession = data.kind === "session"
  const { kind, detail: origin } = data.provenance
  // The folder is named for the moment the backend opened it, which is not
  // when data arrived — one session here was opened a full day before its first
  // frame. Say so rather than letting the two dates quietly disagree, but only
  // when the gap is worth a sentence: every session opens a fraction of a
  // second before its first row, and saying so every time is noise.
  const openedEarly = isSession && gapSeconds(data.opened_utc, data.started_utc) >= OPENED_EARLY_S

  return (
    <>
      {detail.status === "error" && <div role="status" className="px-3 py-1 text-xs text-caution">Update failed · showing last received data <button type="button" onClick={detail.refresh}>Retry</button></div>}
      <div className="flex shrink-0 flex-col gap-2 border-b border-hairline px-3 py-2">
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <ProvenanceBadge kind={kind} />
            <span className="tnum truncate text-sm font-medium text-ink">{rowTitle(data)}</span>
            <span className="tnum shrink-0 text-[0.625rem] uppercase tracking-[0.12em] text-ink-mute">
              {isSession ? "session" : "flight"}
            </span>
          </div>
          <div className="tnum mt-0.5 truncate text-[0.625rem] text-ink-mute" title={data.path}>
            {data.path}
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-1.5">
          {data.files.map((file) => (
            <Button key={file.name} asChild variant="outline" size="xs">
              <a href={recordedFlightFileUrl(data, file.name)} download title={`Download ${file.name} · ${bytes(file.bytes)}`}>
                <Download aria-hidden />
                {file.name}
              </a>
            </Button>
          ))}
          <DeleteFlight flight={flight} remove={remove} onDeleted={onDeleted} />
        </div>
      </div>
      {(kind !== "live" || openedEarly) && (
        <div className="shrink-0 border-b border-hairline px-3 py-1.5 text-[0.625rem] text-ink-mute">
          {kind === "replay" && <span>Re-run of {origin || "an external log"} — not a capture. </span>}
          {kind === "sim" && <span>Generated by fake_telemetry — no vehicle involved. </span>}
          {kind === "unknown" && <span>This folder's metadata does not record where its bytes came from. </span>}
          {openedEarly && (
            <span className="tnum">Folder opened {localTime(data.opened_utc)}, first frame {localTime(data.started_utc)}.</span>
          )}
        </div>
      )}
      <div className="grid shrink-0 grid-cols-3 border-b border-hairline xl:grid-cols-[minmax(13rem,1.5fr)_repeat(5,minmax(0,1fr))]">
        <Metric label={isSession ? "First frame" : "Started"} value={localTime(data.started_utc)} />
        <Metric label="Duration" value={duration(data.duration_s)} />
        <Metric
          label="Rows"
          value={data.rows.toLocaleString()}
          ink={data.rows === 0 ? "text-ink-mute" : undefined}
        />
        <Metric label="Raw" value={bytes(data.raw_bytes)} />
        <Metric label="Codec" value={codec} />
        {isSession ? (
          <Metric label="Flights" value={data.flight_count ? String(data.flight_count) : "none declared"} />
        ) : (
          <Metric label="Closed" value={(data.stop_reason || (data.recording ? "recording" : "unknown")).toUpperCase()} />
        )}
      </div>
      <div className="grid min-h-0 flex-1 grid-rows-[minmax(9rem,0.8fr)_minmax(12rem,1.2fr)]">
        <MissionPreview detail={data} />
        <TelemetryPreview detail={data} />
      </div>
    </>
  )
}

export function FlightsView() {
  const archive = useRecordedFlights()
  const sessions = archive.data?.sessions ?? EMPTY_FLIGHTS
  const flights = archive.data?.flights ?? EMPTY_FLIGHTS
  const [kind, setKind] = useState<KindFilter>("all")
  const [hideEmpty, setHideEmpty] = useState(true)
  const [selectedId, setSelectedId] = useState<string | null>(null)

  const counts = useMemo(() => {
    const tally: Record<KindFilter, number> = { all: sessions.length, live: 0, replay: 0, sim: 0, unknown: 0 }
    for (const session of sessions) tally[session.provenance.kind] += 1
    return tally
  }, [sessions])

  const groups = useMemo<SessionGroup[]>(() => {
    const bySession = new Map<string, RecordedFlight[]>()
    for (const flight of flights) {
      const list = bySession.get(flight.session)
      if (list) list.push(flight)
      else bySession.set(flight.session, [flight])
    }
    return sessions
      .filter((session) => kind === "all" || session.provenance.kind === kind)
      // A session holding a declared flight is never hidden as empty: the
      // flight inside it is the whole reason someone would look for it.
      .filter((session) => !hideEmpty || session.recording || session.rows > 0 || (bySession.get(session.session)?.length ?? 0) > 0)
      .map((session) => ({ session, flights: bySession.get(session.session) ?? [] }))
  }, [sessions, flights, kind, hideEmpty])

  const visible = useMemo(
    () => groups.flatMap((group) => [group.session, ...group.flights]),
    [groups],
  )
  const selected = visible.find((row) => flightId(row) === selectedId) ?? null

  // Keep a selection on screen as filters change, rather than blanking the
  // detail pane every time the operator narrows the list.
  useEffect(() => {
    if (archive.status !== "ok") return
    if (!selected && visible.length) setSelectedId(flightId(visible[0]))
    if (!visible.length && selectedId) setSelectedId(null)
  }, [archive.status, visible, selected, selectedId])

  const hiddenByEmpty = useMemo(
    () => sessions.filter((s) => s.rows === 0 && !s.recording && !flights.some((f) => f.session === s.session)).length,
    [sessions, flights],
  )
  const loaded = archive.status === "ok" || archive.data != null

  return (
    <main className="min-h-0 flex-1 overflow-hidden p-2">
      <div className="grid h-full min-h-0 grid-cols-1 grid-rows-[minmax(11rem,35%)_minmax(0,1fr)] gap-2 md:grid-cols-[19rem_minmax(0,1fr)] md:grid-rows-1">
        <Card className="min-h-0 overflow-hidden">
          <div className="flex shrink-0 items-center justify-between border-b border-hairline px-3 py-2">
            <div>
              <div className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Archive</div>
              <div className="tnum mt-0.5 text-[0.625rem] text-ink-dim">
                {loaded
                  ? `${sessions.length} session${sessions.length === 1 ? "" : "s"} · ${flights.length} flight${flights.length === 1 ? "" : "s"}`
                  : "reading archive"}
              </div>
            </div>
            <Button
              variant="ghost"
              size="icon-xs"
              onClick={archive.refresh}
              disabled={archive.status === "loading"}
              aria-label="Refresh archive"
              title="Refresh archive"
            >
              <RefreshCw aria-hidden />
            </Button>
          </div>
          {archive.status === "loading" && !archive.data ? (
            <SkeletonRows />
          ) : archive.status === "unreachable" && !archive.data ? (
            <div className="flex min-h-0 flex-1 flex-col items-center justify-center gap-3 px-4 text-center">
              <div className="text-xs uppercase tracking-[0.14em] text-caution">Archive unavailable</div>
              <p className="text-[0.6875rem] text-ink-mute">Backend is not reachable or needs to be restarted with the Flights routes.</p>
              <Button variant="outline" size="sm" onClick={archive.refresh}>Retry</Button>
            </div>
          ) : sessions.length ? (
            <>
              <ArchiveFilters
                kind={kind}
                onKind={setKind}
                hideEmpty={hideEmpty}
                onHideEmpty={setHideEmpty}
                counts={counts}
                hidden={hiddenByEmpty}
              />
              {groups.length ? (
                <ArchiveIndex key={`${kind}/${hideEmpty}`} groups={groups} selectedId={selectedId} onSelect={(row) => setSelectedId(flightId(row))} />
              ) : (
                <EmptyArchive filtered />
              )}
            </>
          ) : (
            <EmptyArchive root={archive.data?.root} filtered={false} />
          )}
        </Card>

        <Card className="min-h-0 min-w-0 overflow-hidden">
          {selected ? (
            <DetailPane
              key={flightId(selected)}
              flight={selected}
              remove={archive.remove}
              onDeleted={() => setSelectedId(null)}
            />
          ) : (
            <div className="flex min-h-0 flex-1 items-center justify-center px-6 text-center text-xs text-ink-mute">
              {visible.length ? "Select a recording." : "Summary and downloadable files will appear here."}
            </div>
          )}
        </Card>
      </div>
    </main>
  )
}
