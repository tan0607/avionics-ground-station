/**
 * FlightsView — persistent, read-only access to operator-declared recordings.
 * The left index is reconstructed from disk by the backend; the right pane
 * shows bounded tails rather than loading a whole multi-hour CSV into memory.
 */
import { useEffect, useMemo, useState } from "react"
import { Download, RefreshCw, Trash2 } from "lucide-react"
import { cn } from "@/lib/utils"
import {
  recordedFlightFileUrl,
  useRecordedFlightDetail,
  useRecordedFlights,
  type ArchiveState,
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

function flightId(flight: RecordedFlight): string {
  return `${flight.session}/${flight.flight}`
}

function localTime(value: string | null): string {
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

function ArchiveIndex({
  flights,
  selectedId,
  onSelect,
}: {
  flights: RecordedFlight[]
  selectedId: string | null
  onSelect: (flight: RecordedFlight) => void
}) {
  return (
    <div className="min-h-0 flex-1 overflow-auto">
      {flights.map((flight) => {
        const selected = flightId(flight) === selectedId
        return (
          <button
            key={flightId(flight)}
            type="button"
            aria-pressed={selected}
            onClick={() => onSelect(flight)}
            className={cn(
              "w-full border-b border-hairline border-l-2 px-3 py-2.5 text-left outline-none transition-colors focus-visible:bg-surface-2 focus-visible:ring-1 focus-visible:ring-inset focus-visible:ring-ring",
              selected
                ? "border-l-data bg-surface-2 text-ink"
                : "border-l-transparent text-ink-dim hover:bg-surface-2/60 hover:text-ink",
            )}
          >
            <div className="flex items-center justify-between gap-2">
              <span className="tnum truncate text-xs font-medium">
                {flight.label || flight.flight}
              </span>
              <span
                className={cn(
                  "flex shrink-0 items-center gap-1 text-[0.5625rem] uppercase tracking-[0.12em]",
                  flight.recording ? "text-caution" : "text-ink-mute",
                )}
              >
                <span className={cn("size-1.5 rounded-full", flight.recording ? "bg-caution" : "bg-ink-mute")} />
                {flight.recording ? "recording" : "saved"}
              </span>
            </div>
            <div className="mt-1 truncate text-[0.625rem] text-ink-mute">
              {localTime(flight.started_utc)}
            </div>
            <div className="tnum mt-1 flex gap-3 text-[0.625rem] text-ink-mute">
              <span>{duration(flight.duration_s)}</span>
              <span>{flight.rows.toLocaleString()} rows</span>
              <span>{bytes(flight.raw_bytes)}</span>
            </div>
          </button>
        )
      })}
    </div>
  )
}

function EmptyArchive({ root }: { root?: string }) {
  return (
    <div className="flex min-h-0 flex-1 flex-col items-center justify-center px-6 text-center">
      <div className="text-xs uppercase tracking-[0.14em] text-ink-dim">No recorded flights</div>
      <p className="mt-2 max-w-sm text-[0.6875rem] leading-relaxed text-ink-mute">
        Press REC in the top bar before the next flight, then STOP when the run is complete.
        The saved flight will appear here automatically.
      </p>
      {root && <code className="tnum mt-3 max-w-full break-all text-[0.625rem] text-ink-mute">{root}</code>}
    </div>
  )
}

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="min-w-0 border-r border-hairline px-3 py-2 last:border-r-0">
      <div className="text-[0.5625rem] uppercase tracking-[0.14em] text-ink-mute">{label}</div>
      <div className="tnum mt-1 truncate text-xs text-ink" title={value}>{value}</div>
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
                {columns.map((column) => <TableHead key={column}>{column}</TableHead>)}
              </TableRow>
            </TableHeader>
            <TableBody>
              {detail.telemetry.rows.map((row, index) => (
                <TableRow key={`${row.host_time ?? "row"}-${row.seq ?? index}-${index}`}>
                  {columns.map((column) => (
                    <TableCell key={column} className="py-1 text-ink-dim">{row[column] || "—"}</TableCell>
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
 * a flight with it. The backend refuses two cases outright — the flight that is
 * recording right now, and a folder holding a file the ground station did not
 * write — and whatever it says comes back onto the screen verbatim rather than
 * being flattened into "failed".
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

  // Re-arming has to reset when the operator selects a different flight, or the
  // confirm they armed on one row would apply to the next one they click.
  const id = `${flight.session}/${flight.flight}`
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
        aria-label={armed ? `Confirm deleting ${flight.flight}` : `Delete ${flight.flight}`}
        title={
          armed
            ? "Click again to permanently delete this flight folder"
            : `Permanently delete ${flight.flight}`
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

  if (detail.status === "loading" || detail.status === "idle") {
    return (
      <div aria-label="Loading flight detail" className="flex flex-1 flex-col gap-3 p-4">
        <div className="h-4 w-40 animate-pulse rounded-sm bg-surface-2" />
        <div className="h-10 animate-pulse rounded-sm bg-surface-2" />
        <div className="min-h-0 flex-1 animate-pulse rounded-sm bg-surface-2" />
      </div>
    )
  }

  if (detail.status === "error" || !detail.data) {
    return (
      <div className="flex min-h-0 flex-1 flex-col items-center justify-center gap-3 text-center">
        <div className="text-xs uppercase tracking-[0.14em] text-caution">Flight detail unavailable</div>
        <Button variant="outline" size="sm" onClick={detail.refresh}>Retry</Button>
      </div>
    )
  }

  const data = detail.data
  const codec = typeof data.packet.codec === "string" ? data.packet.codec.toUpperCase() : "—"

  return (
    <>
      <div className="flex shrink-0 flex-wrap items-center justify-between gap-2 border-b border-hairline px-3 py-2">
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <span className="tnum truncate text-sm font-medium text-ink">{data.label || data.flight}</span>
            <span className="tnum text-[0.625rem] text-ink-mute">{data.flight}</span>
          </div>
          <div className="tnum mt-0.5 truncate text-[0.625rem] text-ink-mute" title={data.path}>{data.path}</div>
        </div>
        <div className="flex flex-wrap justify-end gap-1.5">
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
      <div className="grid shrink-0 grid-cols-3 border-b border-hairline sm:grid-cols-6">
        <Metric label="Started" value={localTime(data.started_utc)} />
        <Metric label="Duration" value={duration(data.duration_s)} />
        <Metric label="Rows" value={data.rows.toLocaleString()} />
        <Metric label="Raw" value={bytes(data.raw_bytes)} />
        <Metric label="Codec" value={codec} />
        <Metric label="Closed" value={(data.stop_reason || (data.recording ? "recording" : "unknown")).toUpperCase()} />
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
  const flights = archive.data?.flights ?? EMPTY_FLIGHTS
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const selected = flights.find((flight) => flightId(flight) === selectedId) ?? null

  useEffect(() => {
    if (archive.status !== "ok") return
    if (!selected && flights.length) setSelectedId(flightId(flights[0]))
    if (!flights.length && selectedId) setSelectedId(null)
  }, [archive.status, flights, selected, selectedId])

  return (
    <main className="min-h-0 flex-1 overflow-hidden p-2">
      <div className="grid h-full min-h-0 grid-cols-1 grid-rows-[minmax(11rem,35%)_minmax(0,1fr)] gap-2 md:grid-cols-[18rem_minmax(0,1fr)] md:grid-rows-1">
        <Card className="min-h-0 overflow-hidden">
          <div className="flex shrink-0 items-center justify-between border-b border-hairline px-3 py-2">
            <div>
              <div className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Recorded Flights</div>
              <div className="tnum mt-0.5 text-[0.625rem] text-ink-dim">
                {archive.status === "ok" ? `${flights.length} on disk` : "reading archive"}
              </div>
            </div>
            <Button
              variant="ghost"
              size="icon-xs"
              onClick={archive.refresh}
              disabled={archive.status === "loading"}
              aria-label="Refresh recorded flights"
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
          ) : flights.length ? (
            <ArchiveIndex flights={flights} selectedId={selectedId} onSelect={(flight) => setSelectedId(flightId(flight))} />
          ) : (
            <EmptyArchive root={archive.data?.root} />
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
              {flights.length ? "Select a recorded flight." : "Flight summary and downloadable files will appear here."}
            </div>
          )}
        </Card>
      </div>
    </main>
  )
}
