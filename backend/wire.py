"""Telemetry -> WebSocket wire dict.

The dashboard consumes decoded JSON (see dashboard/src/lib/protocol.ts `WireFrame`).
This module is the ONE place that maps a decoded `packet.Telemetry` onto that shape.

Contract discipline: values come straight from the codec's engineering-unit
properties (`vspeed_ms`, `vbat_v`, `lat_deg`, `lon_deg`) and the flags bitfield —
we never re-scale by hand here. Re-deriving `/10` or `/1e7` in a second place is
exactly how two windows drift out of contract.
"""
from __future__ import annotations

from shared.protocol import packet
from shared.protocol.packet import (
    FLAG_ARMED,
    FLAG_CONTINUITY,
    FLAG_PYRO_FIRED,
    FLAG_SD_OK,
)

# Keys mirror packet.CSV_COLUMNS minus the CSV-only time columns, exactly the
# snake_case field set the dashboard's WireFrame declares.
WIRE_KEYS = (
    "host_time", "seq", "flight_state", "onboard_ms", "baro_alt_m", "vspeed_ms",
    "gps_lat", "gps_lon", "gps_alt_m", "gps_sats", "gps_fix", "tilt_deg",
    "vbat_v", "continuity", "pyro_fired", "sd_ok", "armed",
    "health", "failed_subsystems",
    *(col for _, _, col in packet.SUBSYSTEMS),
)


def telemetry_to_wire(t: packet.Telemetry, host_time_ms: int) -> dict:
    """One decoded frame -> the JSON dict broadcast to /ws clients.

    `host_time_ms` is the host receive time as epoch milliseconds (the dashboard
    treats numeric host_time as epoch ms). flight_state / gps_fix are ints;
    booleans are real JSON booleans (the normalizer accepts bool or 0/1).
    """
    return {
        "host_time": host_time_ms,
        "seq": t.seq,
        "flight_state": int(t.flight_state),
        "onboard_ms": t.onboard_ms,
        "baro_alt_m": t.baro_alt_m,
        "vspeed_ms": t.vspeed_ms,      # dm/s -> m/s (codec property)
        "gps_lat": t.lat_deg,          # deg*1e7 -> deg (codec property)
        "gps_lon": t.lon_deg,
        "gps_alt_m": t.gps_alt_m,
        "gps_sats": t.gps_sats,
        "gps_fix": int(t.gps_fix),
        "tilt_deg": t.tilt_deg,
        "vbat_v": t.vbat_v,            # *0.1V -> V (codec property)
        "continuity": t.flag(FLAG_CONTINUITY),
        "pyro_fired": t.flag(FLAG_PYRO_FIRED),
        "sd_ok": t.flag(FLAG_SD_OK),
        "armed": t.flag(FLAG_ARMED),
        # Per-peripheral health. Shipped as the raw byte AND as named booleans +
        # a list of what's down, so the UI can render "BARO LOST" instead of a
        # blanket vehicle failure. Empty list = all nominal.
        "health": t.health,
        "failed_subsystems": t.failed_subsystems(),
        **{col: t.healthy(mask) for mask, _, col in packet.SUBSYSTEMS},
    }
