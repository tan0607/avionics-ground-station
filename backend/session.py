"""Per-flight session folder + durable writers.

One folder per run under flights/<session-id>/ (session id = UTC start timestamp):

    flights/2026-07-06T13-45-30Z/
    ├── metadata.json    # start time, source, packet/CSV contract, raw.log format
    ├── raw.log          # length-prefixed raw byte records (raw FIRST, before parse)
    ├── telemetry.csv    # one decoded row per frame, flushed per line
    └── events.csv       # flight-state transitions (liftoff / apogee / deploys / landed)

Golden rule (GROUND_STATION_PLAN.md §4): append raw bytes with a host timestamp
BEFORE decoding, so a parser bug can never lose data — raw.log is replayable.
"""
from __future__ import annotations

import csv
import json
import struct
from datetime import datetime, timezone
from pathlib import Path

from shared.protocol import packet

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


class SessionWriter:
    def __init__(self, flights_root: Path, source_desc: dict) -> None:
        self.started = datetime.now(timezone.utc)
        self.session_id = self.started.strftime("%Y-%m-%dT%H-%M-%SZ")
        self.dir = flights_root / self.session_id
        self.dir.mkdir(parents=True, exist_ok=True)

        # raw FIRST: open the raw sink in binary-append before anything else.
        self._raw = (self.dir / "raw.log").open("ab", buffering=0)

        self._csv_fh = (self.dir / "telemetry.csv").open("w", newline="")
        self._csv = csv.DictWriter(self._csv_fh, fieldnames=packet.CSV_COLUMNS)
        self._csv.writeheader()
        self._csv_fh.flush()

        self._events_fh = (self.dir / "events.csv").open("w", newline="")
        self._events = csv.DictWriter(self._events_fh, fieldnames=EVENT_COLUMNS)
        self._events.writeheader()
        self._events_fh.flush()

        self._write_metadata(source_desc)

    def _write_metadata(self, source_desc: dict) -> None:
        meta = {
            "session_id": self.session_id,
            "created_utc": self.started.isoformat(timespec="seconds"),
            "source": source_desc,
            "packet": {
                "size_bytes": packet.PACKET_SIZE,
                "sync_hex": packet.SYNC_BYTES.hex(),
                "rate_hz": 4,
                "protocol": "shared/protocol/PROTOCOL.md",
            },
            "csv_columns": packet.CSV_COLUMNS,
            "raw_log_format": RAW_LOG_FORMAT,
            "note": "raw.log is written before parsing (raw first). telemetry.csv "
                    "host_time is ISO-8601 UTC; the WebSocket broadcasts epoch ms.",
        }
        (self.dir / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n")

    def write_raw(self, host_ms: int, chunk: bytes) -> None:
        """Append one raw byte record. Called BEFORE the parser sees the bytes."""
        self._raw.write(_RAW_HEADER.pack(host_ms, len(chunk)))
        self._raw.write(chunk)

    def write_row(self, t: packet.Telemetry, host_ms: int, gps_time: str = "") -> None:
        self._csv.writerow(t.to_csv_row(utc_iso_ms(host_ms), gps_time))
        self._csv_fh.flush()

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
        return label

    def close(self) -> None:
        for fh in (self._raw, self._csv_fh, self._events_fh):
            try:
                fh.close()
            except Exception:
                pass
