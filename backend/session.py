"""Per-flight session folder + durable writers.

TWO nested records, because "one backend run" and "one flight" are not the same
span and pretending they were is what made the old layout unreliable:

    flights/2026-07-06T13-45-30Z/         <- SESSION: one backend process, always on
    ├── metadata.json                     # start time, source, packet/CSV contract, raw.log format
    ├── raw.log                           # length-prefixed raw byte records (raw FIRST, before parse)
    ├── telemetry.csv                     # one decoded row per frame, flushed per line
    ├── events.csv                        # flight-state transitions (liftoff / apogee / deploys / landed)
    ├── mission.log                       # human-readable ground log (states, pyro, link, health)
    └── flight-01_launch-a/               <- FLIGHT: cut by the operator, same five files
        ├── metadata.json
        ├── raw.log
        ├── telemetry.csv
        ├── events.csv
        └── mission.log

The session recorder never stops: forgetting to hit RECORD costs you a tidy
folder, never the data. The flight recorder is opened and closed by the operator
(POST /flight/start, /flight/stop) because nothing else can be trusted to know
where a flight begins -- the onboard clock jumps back on every transmitter
reboot (394 times inside one 21-hour bench session), and a vehicle that never
leaves PAD never announces a liftoff at all.

A flight folder holds the SAME five files as its session, so it is a complete
record on its own: `--replay flights/<session>/flight-01` works exactly like
replaying the session, and metadata.json inside it names the same codec.

Golden rule (GROUND_STATION_PLAN.md §4): append raw bytes with a host timestamp
BEFORE decoding, so a parser bug can never lose data -- raw.log is replayable.
"""
from __future__ import annotations

import csv
import json
import re
import struct
from collections import deque
from collections.abc import Iterator
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from shared.protocol import mrcc, packet

# The two wire formats the link can be carrying. "binary" is the designed one
# (PROTOCOL.md, 32 bytes, CRC-16) and stays the default. "mrcc" is the ASCII
# key=value stream the airborne board actually downlinks today — see
# shared/protocol/mrcc.py. They differ ONLY in the parser and in how much they
# can record: raw.log, the loss tracker and /ws are identical either way, which
# is the point of mapping MRCC onto packet.Telemetry rather than forking the
# pipeline.
PACKET_DESC = {
    "binary": {
        "codec": "binary",
        "size_bytes": packet.PACKET_SIZE,
        "sync_hex": packet.SYNC_BYTES.hex(),
        "rate_hz": 4,
        "protocol": "shared/protocol/PROTOCOL.md",
    },
    "mrcc": {
        "codec": "mrcc",
        "framing": "newline-delimited ASCII key=value",
        # MEASURED, not assumed: a 20 s /stats delta on 2026-08-20 read 2.00
        # unique frames/s and 2.00 duplicates/s, and the onboard timestamps in
        # flights/2026-08-19T05-54-40Z step by exactly 500 ms. This field used to
        # say 1 Hz and the dashboard's PACKET_HZ said 4; the link was doing
        # neither, and nothing in the system was measuring it to find out.
        "rate_hz": 2,
        # Every packet is transmitted TWICE, ~205 ms apart, so the receiver
        # prints ~4 lines/s. Both numbers are recorded because they get mistaken
        # for each other constantly: the line rate is what a serial monitor
        # shows you, the frame rate is how often the console can actually
        # change, and expecting the first from the second reads as a ground
        # station lagging its own radio.
        "tx_repeat": 2,
        "line_rate_hz": 4,
        "protocol": "shared/protocol/mrcc.py",
        "note": "no CRC on the payload; health is inferred per-bit, not reported; "
                "each packet is sent twice — LossTracker counts the repeat as a "
                "duplicate, not as a frame",
    },
}

# telemetry.csv columns per format. MRCC decodes more than PROTOCOL.md defines,
# so it records more — see mrcc.CSV_COLUMNS and SessionWriter below.
CSV_COLUMNS_FOR = {
    "binary": packet.CSV_COLUMNS,
    "mrcc": mrcc.CSV_COLUMNS,
}

# raw.log record = <host_epoch_ms: u64 LE><nbytes: u32 LE><raw bytes>. Verbatim
# bytes, unambiguous boundaries -> a replay tool reconstructs the exact stream.
_RAW_HEADER = struct.Struct("<QI")
RAW_LOG_FORMAT = "record = <host_epoch_ms u64 LE><len u32 LE><raw bytes>"

EVENT_COLUMNS = [
    "host_time", "onboard_ms", "seq", "event", "flight_state",
    "baro_alt_m", "vspeed_ms",
]

# Friendly event label per flight state, for events.csv readability.
_EVENT_LABEL = {
    packet.FlightState.PAD: "pad",
    packet.FlightState.BOOST: "liftoff",
    packet.FlightState.COAST: "burnout",
    packet.FlightState.APOGEE: "apogee",
    packet.FlightState.DROGUE: "drogue_deploy",
    packet.FlightState.MAIN: "main_deploy",
    packet.FlightState.LANDED: "landed",
}


def event_label(flight_state: int) -> str:
    try:
        return _EVENT_LABEL.get(packet.FlightState(flight_state), f"state_{flight_state}")
    except ValueError:
        return f"state_{flight_state}"


def utc_iso_ms(epoch_ms: int) -> str:
    """Epoch ms -> ISO-8601 UTC string (what telemetry.csv stores for host_time)."""
    return datetime.fromtimestamp(epoch_ms / 1000, tz=timezone.utc).isoformat(
        timespec="milliseconds"
    )


# ----------------------------------------------------------------------------
# mission.log — the ground log, as a file
# ----------------------------------------------------------------------------
# The Log view derives exactly these events in the browser and kept them in
# localStorage: one rolling 600-row buffer under a single key, overwritten by
# the next flight and gone with the browser profile. It was the one part of the
# console that never reached the disk. The backend already sees everything it
# needs to derive them, so the file is written here, per flight, alongside the
# telemetry it annotates.
MISSION_LOG_HEADER = (
    "# mission.log — ground-station event log\n"
    "# <utc>  <+seconds from this file's start>  <severity>  <message>  <detail>\n"
)

# Mirror the dashboard's link thresholds (dashboard/src/lib/protocol.ts
# LINK_STALE_MS and useTelemetry's DOWN_AFTER_MS) so the file and the screen
# never disagree about when the link was lost.
LINK_STALE_MS = 3000
LINK_DOWN_MS = 2 * LINK_STALE_MS
# Onboard clock jumping this far backwards = the flight computer restarted
# (mirrors useTelemetry / useTelemetryLog).
SESSION_RESET_MS = 1500


@dataclass
class MissionEvent:
    host_ms: int
    kind: str          # session | state | pyro | link | health | record
    severity: str      # info | nominal | caution | alarm
    message: str
    detail: str = ""
    onboard_ms: int = 0
    seq: int = 0


@dataclass
class MissionDeriver:
    """Turns the frame stream into mission events, the way the Log view does.

    Kept here rather than in app.py so backend/tests.py can exercise it without
    importing FastAPI, and so the browser is not the only thing in the system
    that knows a link went stale.
    """

    _link: str = "down"
    _prev_state: int | None = None
    _prev_pyro: bool = False
    _prev_onboard: int | None = None
    _prev_health: int | None = None
    _last_arrival_ms: int | None = None
    _reported_link: str | None = field(default=None)

    def observe(self, t: packet.Telemetry, host_ms: int,
                health_known: int | None = None,
                flags_known: int | None = None) -> list[MissionEvent]:
        """One decoded frame -> the events it implies."""
        out: list[MissionEvent] = []
        known = packet.HEALTH_ALL_OK if health_known is None else health_known
        fknown = packet.FLAGS_ALL if flags_known is None else flags_known

        def ev(kind: str, severity: str, message: str, detail: str = "") -> None:
            out.append(MissionEvent(host_ms, kind, severity, message, detail,
                                    t.onboard_ms, t.seq))

        # A frame arriving IS the link coming back. Emitted before anything else
        # so "LINK LIVE" precedes the telemetry it re-enabled.
        if self._reported_link != "live":
            self._reported_link = "live"
            ev("link", "nominal", "LINK LIVE")
        self._last_arrival_ms = host_ms

        # Onboard clock jumped backwards: the flight computer restarted. Marked,
        # never used to roll a folder — see this module's docstring.
        if self._prev_onboard is not None and t.onboard_ms + SESSION_RESET_MS < self._prev_onboard:
            ev("session", "info", "SESSION RESET", "onboard clock jumped back")
            self._prev_state = None
            self._prev_pyro = False
            self._prev_health = None
        self._prev_onboard = t.onboard_ms

        state_name = event_label(t.flight_state).upper()
        if self._prev_state is None:
            ev("session", "info", "FIRST FRAME", state_name)
        elif self._prev_state != t.flight_state:
            ev("state", "info", state_name, f"{t.baro_alt_m:.0f} m")
        self._prev_state = t.flight_state

        # Gated on flags_known: the MRCC downlink carries no flags at all, and a
        # never-set bit must not read as "pyro has not fired" any more than it
        # reads as "pyro fired".
        if fknown & packet.FLAG_PYRO_FIRED:
            pyro = t.flag(packet.FLAG_PYRO_FIRED)
            if pyro and not self._prev_pyro:
                ev("pyro", "caution", "PYRO FIRED", state_name)
            self._prev_pyro = pyro

        # Per-peripheral health edges, for the same reason the health panel
        # exists: "BARO LOST" is a different call from a blanket vehicle failure.
        if self._prev_health is None:
            self._prev_health = t.health
        elif t.health != self._prev_health:
            for mask, name, _ in packet.SUBSYSTEMS:
                if not known & mask:
                    continue
                was, now = self._prev_health & mask, t.health & mask
                if was and not now:
                    ev("health", "alarm", f"{name.upper()} LOST")
                elif now and not was:
                    ev("health", "nominal", f"{name.upper()} OK")
            self._prev_health = t.health

        return out

    def tick(self, host_ms: int) -> list[MissionEvent]:
        """Wall-clock check for the events that happen when NOTHING arrives.

        Link loss is the one event a frame-driven pipeline can never emit: the
        evidence is the absence of frames. Called on a timer by the ingest side.
        """
        if self._last_arrival_ms is None:
            return []
        age = host_ms - self._last_arrival_ms
        state = "down" if age >= LINK_DOWN_MS else "stale" if age >= LINK_STALE_MS else "live"
        if state == "live" or state == self._reported_link:
            return []
        self._reported_link = state
        severity, message = (("alarm", "LINK LOST") if state == "down"
                             else ("caution", "LINK STALE"))
        # Stamped with the moment the ground station NOTICED, not the last
        # frame's onboard time — the last frame is by definition stale exactly
        # when this fires, and stamping it that way records the loss as having
        # happened before it did.
        return [MissionEvent(host_ms, "link", severity, message,
                             f"{age / 1000:.1f} s since last frame")]


# Downloads are served in 64 KB blocks. Big enough that a 50 MB raw.log is not
# a million yields, small enough that a chunk is never a meaningful stall.
SNAPSHOT_BLOCK = 64 * 1024


def file_snapshot(path: Path, block: int = SNAPSHOT_BLOCK) -> tuple[int, Iterator[bytes]]:
    """(size, byte-iterator) for a file that is still being APPENDED to.

    EVERY file this ground station exports is live -- downloading the flight
    record mid-flight is the point of the export, not an edge case. A plain file
    response stats the file to set Content-Length and only then starts streaming
    it, so the ingest loop writing one more row during the transfer makes the
    body outrun its own header and the download dies with "Response content
    longer than Content-Length".

    So the length is fixed once, here, and exactly that many bytes are yielded:
    what the operator gets is a clean PREFIX of the record as it stood the
    moment they asked for it. A prefix of a CSV is still a readable CSV; a
    transfer that aborts at 90% is nothing at all.
    """
    size = path.stat().st_size

    def chunks() -> Iterator[bytes]:
        remaining = size
        with path.open("rb") as fh:
            while remaining > 0:
                chunk = fh.read(min(block, remaining))
                if not chunk:
                    # Truncated under us (never happens to an append-only log,
                    # but a short body beats hanging on a promise we can't keep).
                    break
                remaining -= len(chunk)
                yield chunk

    return size, chunks()


def slugify(label: str) -> str:
    """Operator's flight name -> a safe path component.

    The label reaches this from an HTTP body, so it is sanitised rather than
    trusted: everything outside [a-z0-9-] collapses to a dash, and the result
    can be empty (the caller falls back to the bare flight-NN name). A name that
    could contain "/" or ".." would be a path traversal in a directory name.
    """
    slug = re.sub(r"[^a-z0-9]+", "-", label.strip().lower()).strip("-")
    return slug[:40]


# ----------------------------------------------------------------------------
# Recorded-flight archive — disk discovery for the portal
# ----------------------------------------------------------------------------
# These are directory identities, not merely display patterns. HTTP route
# parameters pass through resolve_recorded_flight_dir() before any filename is
# joined, so neither `..` nor a symlink can escape the configured flights root.
_SESSION_DIR_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z$")
_FLIGHT_DIR_RE = re.compile(r"^flight-\d{2,}(?:_[a-z0-9-]+)?$")

# A flight is a complete record only when it can expose the same durable five
# files FlightWriter owns. Order is the UI/download order, not filesystem order.
RECORDED_FLIGHT_FILES = (
    "metadata.json",
    "telemetry.csv",
    "events.csv",
    "mission.log",
    "raw.log",
)


def resolve_recorded_flight_dir(flights_root: Path, session_id: str,
                                flight_name: str) -> Path | None:
    """Resolve one archive directory without allowing traversal or symlinks."""
    if not _SESSION_DIR_RE.fullmatch(session_id) or not _FLIGHT_DIR_RE.fullmatch(flight_name):
        return None
    root = flights_root.resolve()
    session = (root / session_id).resolve()
    if session.parent != root or not session.is_dir():
        return None
    flight = (session / flight_name).resolve()
    if flight.parent != session or not flight.is_dir():
        return None
    return flight


def resolve_recorded_flight_file(flights_root: Path, session_id: str,
                                 flight_name: str, name: str) -> Path | None:
    """Resolve one whitelisted durable file, rejecting file-level symlinks."""
    if name not in RECORDED_FLIGHT_FILES:
        return None
    directory = resolve_recorded_flight_dir(flights_root, session_id, flight_name)
    if directory is None:
        return None
    path = (directory / name).resolve()
    if path.parent != directory or not path.is_file():
        return None
    return path


class FlightFolderNotEmpty(RuntimeError):
    """The folder holds something this code did not write, so it is not deleted."""


def delete_recorded_flight(flights_root: Path, session_id: str,
                           flight_name: str) -> dict | None:
    """Permanently remove one flight folder. None if there is no such flight.

    This is the ONE operation in the ground station that destroys flight data,
    so it is deliberately narrow: it unlinks exactly the durable files this
    module writes and then removes the (now empty) directory. There is no
    recursive tree walk. If the operator has put anything else in the folder --
    a plot, notes, a copy of the onboard SD log -- the delete REFUSES rather
    than quietly taking it along, because "delete the flight I selected" must
    never turn into "delete a file you did not know was in there".

    Callers must separately refuse to delete the flight that is recording right
    now; this function cannot see the live session.
    """
    directory = resolve_recorded_flight_dir(flights_root, session_id, flight_name)
    if directory is None:
        return None

    removed, freed = [], 0
    extras = []
    for entry in sorted(directory.iterdir()):
        if entry.name in RECORDED_FLIGHT_FILES and entry.is_file() and not entry.is_symlink():
            removed.append(entry.name)
            try:
                freed += entry.stat().st_size
            except OSError:
                pass
        else:
            extras.append(entry.name)
    if extras:
        raise FlightFolderNotEmpty(
            f"{flight_name} also holds {', '.join(extras[:5])} — delete those by hand first"
        )

    for name in removed:
        (directory / name).unlink()
    directory.rmdir()
    return {"session": session_id, "flight": flight_name,
            "removed": removed, "freed_bytes": freed}


def _recorded_flight_summary(directory: Path, session_id: str) -> dict | None:
    """Read one trustworthy archive row; malformed metadata makes it invisible."""
    try:
        raw = json.loads((directory / "metadata.json").read_text())
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(raw, dict):
        return None
    try:
        rows = max(0, int(raw.get("rows") or 0))
        events = max(0, int(raw.get("events") or 0))
        raw_bytes = max(0, int(raw.get("raw_bytes") or 0))
        raw_duration = raw.get("duration_s")
        duration_s = max(0.0, float(raw_duration)) if raw_duration is not None else None
    except (TypeError, ValueError):
        return None

    files = []
    for name in RECORDED_FLIGHT_FILES:
        path = (directory / name).resolve()
        if path.parent != directory or not path.is_file():
            continue
        try:
            size = path.stat().st_size
        except OSError:
            continue
        files.append({"name": name, "bytes": size})

    # The folder names are the route identity. Metadata values remain useful
    # display facts, but a copied/stale metadata file cannot redirect a download.
    return {
        "session": session_id,
        "flight": directory.name,
        "path": str(directory),
        "label": str(raw.get("label") or ""),
        "started_utc": raw.get("started_utc") if isinstance(raw.get("started_utc"), str) else None,
        "stopped_utc": raw.get("stopped_utc") if isinstance(raw.get("stopped_utc"), str) else None,
        "duration_s": duration_s,
        "stop_reason": raw.get("stop_reason") if isinstance(raw.get("stop_reason"), str) else None,
        "recording": bool(raw.get("recording", False)),
        "rows": rows,
        "events": events,
        "raw_bytes": raw_bytes,
        "source": raw.get("source") if isinstance(raw.get("source"), dict) else {},
        "packet": raw.get("packet") if isinstance(raw.get("packet"), dict) else {},
        "files": files,
    }


def recorded_flights(flights_root: Path) -> list[dict]:
    """Discover every operator-declared flight currently present on disk.

    Nothing here depends on SessionWriter's in-memory `flights` list, so the
    portal remains an archive after the backend restarts.
    """
    root = flights_root.resolve()
    if not root.is_dir():
        return []
    found: list[dict] = []
    try:
        sessions = list(root.iterdir())
    except OSError:
        return []
    for session in sessions:
        if not _SESSION_DIR_RE.fullmatch(session.name) or not session.is_dir():
            continue
        try:
            entries = list(session.iterdir())
        except OSError:
            continue
        for entry in entries:
            directory = resolve_recorded_flight_dir(root, session.name, entry.name)
            if directory is None:
                continue
            summary = _recorded_flight_summary(directory, session.name)
            if summary is not None:
                found.append(summary)
    # ISO UTC timestamps sort chronologically as strings. Folder identities are
    # stable tie-breakers for old/incomplete metadata without a start time.
    found.sort(
        key=lambda f: (str(f.get("started_utc") or ""), f["session"], f["flight"]),
        reverse=True,
    )
    return found


def _tail_text_lines(path: Path, limit: int) -> dict:
    keep = max(1, min(int(limit), 500))
    lines: deque[str] = deque(maxlen=keep)
    count = 0
    try:
        with path.open(errors="replace") as fh:
            for raw in fh:
                lines.append(raw.rstrip("\r\n"))
                count += 1
    except OSError:
        return {"lines": [], "truncated": False}
    return {"lines": list(lines), "truncated": count > keep}


def _tail_telemetry_rows(path: Path, limit: int) -> dict:
    keep = max(1, min(int(limit), 200))
    rows: deque[dict[str, str]] = deque(maxlen=keep)
    count = 0
    columns: list[str] = []
    try:
        with path.open(newline="", errors="replace") as fh:
            reader = csv.DictReader(fh)
            columns = list(reader.fieldnames or [])
            for row in reader:
                rows.append({key: value for key, value in row.items() if key is not None})
                count += 1
    except (OSError, csv.Error):
        return {"columns": [], "rows": [], "truncated": False}
    return {"columns": columns, "rows": list(rows), "truncated": count > keep}


def recorded_flight_detail(flights_root: Path, session_id: str, flight_name: str,
                           mission_lines: int = 120,
                           telemetry_rows: int = 40) -> dict | None:
    """Return one archive summary plus bounded tail previews."""
    directory = resolve_recorded_flight_dir(flights_root, session_id, flight_name)
    if directory is None:
        return None
    summary = _recorded_flight_summary(directory, session_id)
    if summary is None:
        return None
    return {
        **summary,
        "mission": _tail_text_lines(directory / "mission.log", mission_lines),
        "telemetry": _tail_telemetry_rows(directory / "telemetry.csv", telemetry_rows),
    }


# ----------------------------------------------------------------------------
# The recorders
# ----------------------------------------------------------------------------
class _Recorder:
    """The five durable sinks that make a folder a complete, replayable record.

    Shared by the session (one backend run) and by each flight cut inside it, so
    a flight folder is not a lesser record than its session — same files, same
    framing, same metadata contract, replayable on its own.
    """

    def __init__(self, directory: Path, csv_columns: list[str]) -> None:
        self.dir = directory
        self.dir.mkdir(parents=True, exist_ok=True)
        self.started = datetime.now(timezone.utc)
        self._columns = list(csv_columns)
        # Counters, so the UI and the closing metadata can say what was actually
        # captured rather than making the operator stat the files.
        self.rows = 0
        self.raw_bytes = 0
        self.events = 0

        # raw FIRST: open the raw sink in binary-append before anything else.
        self._raw = (self.dir / "raw.log").open("ab", buffering=0)

        self._csv_fh = (self.dir / "telemetry.csv").open("w", newline="")
        # restval="": a column this frame said nothing about is written EMPTY,
        # which is the same distinction hw_* and the flags already draw — an
        # unreported reading must never be recorded as a zero one.
        self._csv = csv.DictWriter(self._csv_fh, fieldnames=self._columns, restval="")
        self._csv.writeheader()
        self._csv_fh.flush()

        self._events_fh = (self.dir / "events.csv").open("w", newline="")
        self._events = csv.DictWriter(self._events_fh, fieldnames=EVENT_COLUMNS)
        self._events.writeheader()
        self._events_fh.flush()

        self._mission_fh = (self.dir / "mission.log").open("w")
        self._mission_fh.write(MISSION_LOG_HEADER)
        self._mission_fh.flush()

    def write_raw(self, host_ms: int, chunk: bytes) -> None:
        """Append one raw byte record. Called BEFORE the parser sees the bytes."""
        self._raw.write(_RAW_HEADER.pack(host_ms, len(chunk)))
        self._raw.write(chunk)
        self.raw_bytes += len(chunk)

    def write_row(self, t: packet.Telemetry, host_ms: int, gps_time: str = "",
                  health_known: int | None = None, flags_known: int | None = None,
                  aux: dict[str, object] | None = None) -> None:
        """One decoded frame -> one telemetry.csv row.

        `aux` carries the columns beyond packet.CSV_COLUMNS that this session's
        format declared (radio quality and the surplus fields — see
        mrcc.aux_csv_row). Keys outside `self._columns` are a contract bug, so
        DictWriter is left to raise on them rather than dropping them quietly:
        silently discarding a field is the exact failure this parameter exists
        to fix, and it must not come back in a different disguise.
        """
        row = t.to_csv_row(utc_iso_ms(host_ms), gps_time, health_known, flags_known)
        if aux:
            row.update(aux)
        self._csv.writerow(row)
        self._csv_fh.flush()
        self.rows += 1

    def write_event(self, t: packet.Telemetry, host_ms: int) -> str:
        label = event_label(t.flight_state)
        self._events.writerow({
            "host_time": utc_iso_ms(host_ms),
            "onboard_ms": t.onboard_ms,
            "seq": t.seq,
            "event": label,
            "flight_state": int(t.flight_state),
            "baro_alt_m": t.baro_alt_m,
            "vspeed_ms": t.vspeed_ms,
        })
        self._events_fh.flush()
        self.events += 1
        return label

    def write_mission(self, e: MissionEvent) -> None:
        """One mission event -> one human-readable mission.log line."""
        rel = e.host_ms / 1000 - self.started.timestamp()
        detail = f"  — {e.detail}" if e.detail else ""
        self._mission_fh.write(
            f"{utc_iso_ms(e.host_ms)}  {rel:+8.1f}s  {e.severity:<7}  {e.message}{detail}\n"
        )
        self._mission_fh.flush()

    def close(self) -> None:
        for fh in (self._raw, self._csv_fh, self._events_fh, self._mission_fh):
            try:
                fh.close()
            except Exception:
                pass


class FlightWriter(_Recorder):
    """One operator-declared flight, inside its session folder.

    Cut by POST /flight/start and closed by /flight/stop (or by the server
    shutting down, which closes it with reason="shutdown" rather than leaving a
    folder that claims to still be recording).
    """

    def __init__(self, session_dir: Path, index: int, label: str,
                 session_id: str, source_desc: dict, packet_desc: dict,
                 csv_columns: list[str]) -> None:
        self.index = index
        self.label = label
        slug = slugify(label)
        self.name = f"flight-{index:02d}" + (f"_{slug}" if slug else "")
        self._session_id = session_id
        self._source_desc = source_desc
        self._packet_desc = packet_desc
        super().__init__(session_dir / self.name, csv_columns)
        self.stopped: datetime | None = None
        self.reason = ""
        self._write_metadata()

    def _write_metadata(self) -> None:
        meta = {
            "flight": self.name,
            "index": self.index,
            "label": self.label,
            # Which run of the backend this was cut from, so a flight folder
            # moved elsewhere can still be traced back to its full session.
            "session_id": self._session_id,
            "started_utc": self.started.isoformat(timespec="milliseconds"),
            "stopped_utc": self.stopped.isoformat(timespec="milliseconds") if self.stopped else None,
            "duration_s": round(self.elapsed_s, 3) if self.stopped else None,
            "stop_reason": self.reason or None,
            "recording": self.stopped is None,
            "rows": self.rows,
            "events": self.events,
            "raw_bytes": self.raw_bytes,
            "source": self._source_desc,
            # Same codec key the session writes, in the same place: ReplaySource
            # .detect_format() reads metadata.json beside the raw.log it opens,
            # so a flight folder replays without being told its format.
            "packet": self._packet_desc,
            "csv_columns": self._columns,
            "raw_log_format": RAW_LOG_FORMAT,
        }
        (self.dir / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n")

    @property
    def elapsed_s(self) -> float:
        end = self.stopped or datetime.now(timezone.utc)
        return (end - self.started).total_seconds()

    def status(self) -> dict:
        return {
            "flight": self.name,
            "index": self.index,
            "label": self.label,
            "started_utc": self.started.isoformat(timespec="milliseconds"),
            "elapsed_s": round(self.elapsed_s, 1),
            "rows": self.rows,
            "events": self.events,
            "raw_bytes": self.raw_bytes,
            "recording": self.stopped is None,
            "stop_reason": self.reason or None,
        }

    def close(self, reason: str = "operator") -> dict:
        self.stopped = datetime.now(timezone.utc)
        self.reason = reason
        # Metadata is rewritten on close so the folder's own file records the
        # duration and the row count. A folder found later must not still say
        # "recording": true just because the process died mid-flight -- that is
        # the difference between "this is the whole flight" and "this is as much
        # of it as survived".
        self._write_metadata()
        super().close()
        return self.status()


class SessionWriter(_Recorder):
    """One backend run. Always recording; owns the flight folders cut inside it."""

    def __init__(self, flights_root: Path, source_desc: dict,
                 packet_desc: dict | None = None,
                 csv_columns: list[str] | None = None) -> None:
        # `packet_desc` describes the wire format this session is decoding, for
        # metadata.json. It is a parameter and not a constant because the link
        # no longer always carries the binary frame — see backend/app.py
        # --format and shared/protocol/mrcc.py. A replay tool reads raw.log
        # through whichever codec this names.
        self._packet_desc = packet_desc or PACKET_DESC["binary"]
        # ...and so are the CSV columns, for the same reason. A format that
        # decodes MORE than PROTOCOL.md defines has to be able to RECORD more
        # than PROTOCOL.md defines, or the surplus is parsed, broadcast, drawn
        # on screen, and then thrown away at the one step that was supposed to
        # keep it. See mrcc.CSV_COLUMNS: extra columns are only ever appended,
        # so a reader that knows packet.CSV_COLUMNS still reads these files.
        self._source_desc = source_desc
        started = datetime.now(timezone.utc)
        self.session_id = started.strftime("%Y-%m-%dT%H-%M-%SZ")
        super().__init__(flights_root / self.session_id,
                         list(csv_columns or packet.CSV_COLUMNS))

        self.flight: FlightWriter | None = None
        self.flights: list[dict] = []   # closed flights, newest last
        self._flight_count = 0

        self._write_metadata()

    def _write_metadata(self) -> None:
        meta = {
            "session_id": self.session_id,
            "created_utc": self.started.isoformat(timespec="seconds"),
            "source": self._source_desc,
            "packet": self._packet_desc,
            "csv_columns": self._columns,
            "raw_log_format": RAW_LOG_FORMAT,
            "note": "raw.log is written before parsing (raw first). telemetry.csv "
                    "host_time is ISO-8601 UTC; the WebSocket broadcasts epoch ms. "
                    "flight-NN/ subfolders are operator-declared flights holding "
                    "the same five files for their span only.",
        }
        (self.dir / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n")

    # --- flight recording ---------------------------------------------------
    def start_flight(self, label: str = "") -> FlightWriter:
        """Cut a new flight folder. Raises if one is already open."""
        if self.flight is not None:
            raise RuntimeError(f"already recording {self.flight.name}")
        self._flight_count += 1
        self.flight = FlightWriter(self.dir, self._flight_count, label,
                                   self.session_id, self._source_desc,
                                   self._packet_desc, self._columns)
        return self.flight

    def stop_flight(self, reason: str = "operator") -> dict | None:
        if self.flight is None:
            return None
        summary = self.flight.close(reason)
        self.flights.append(summary)
        self.flight = None
        return summary

    def flight_status(self) -> dict:
        return {
            "session": self.session_id,
            "recording": self.flight is not None,
            "flight": self.flight.status() if self.flight else None,
            "completed": self.flights,
        }

    # --- fan-out: everything the session records, an open flight records too --
    def write_raw(self, host_ms: int, chunk: bytes) -> None:
        super().write_raw(host_ms, chunk)
        if self.flight is not None:
            self.flight.write_raw(host_ms, chunk)

    def write_row(self, t: packet.Telemetry, host_ms: int, gps_time: str = "",
                  health_known: int | None = None, flags_known: int | None = None,
                  aux: dict[str, object] | None = None) -> None:
        super().write_row(t, host_ms, gps_time, health_known, flags_known, aux)
        if self.flight is not None:
            self.flight.write_row(t, host_ms, gps_time, health_known, flags_known, aux)

    def write_event(self, t: packet.Telemetry, host_ms: int) -> str:
        label = super().write_event(t, host_ms)
        if self.flight is not None:
            self.flight.write_event(t, host_ms)
        return label

    def write_mission(self, e: MissionEvent) -> None:
        super().write_mission(e)
        if self.flight is not None:
            self.flight.write_mission(e)

    def close(self) -> None:
        # The flight first, so its metadata is finalised while its files are
        # still open -- a killed server leaves a closed, honest flight folder.
        self.stop_flight("shutdown")
        super().close()
