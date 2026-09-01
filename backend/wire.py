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
    "vbat_v", "continuity", "pyro_fired", "sd_ok", "armed", "flags_known",
    "health", "health_known", "failed_subsystems",
    *(col for _, _, col in packet.SUBSYSTEMS),
    "rssi_dbm", "snr_db", "extra",
)


def telemetry_to_wire(t: packet.Telemetry, host_time_ms: int,
                      health_known: int | None = None,
                      flags_known: int | None = None,
                      rssi_dbm: int | None = None,
                      snr_db: float | None = None,
                      extra: dict[str, float] | None = None) -> dict:
    """One decoded frame -> the JSON dict broadcast to /ws clients.

    `host_time_ms` is the host receive time as epoch milliseconds (the dashboard
    treats numeric host_time as epoch ms). flight_state / gps_fix are ints;
    booleans are real JSON booleans (the normalizer accepts bool or 0/1).

    `health_known` is the mask of health bits this frame actually reports; None
    means all six, which is what a binary frame always carries. Bits outside the
    mask ship as JSON `null`, not `false` — see the health block below.
    `flags_known` is the same idea for the flags bitfield.
    """
    known = packet.HEALTH_ALL_OK if health_known is None else health_known
    fknown = packet.FLAGS_ALL if flags_known is None else flags_known
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
        # Gated on the VBAT health bit, which PROTOCOL.md already defines as
        # covering the field it feeds. A downlink with no battery field must not
        # render 0.0 V — on the pad that is indistinguishable from a flat pack,
        # and it is the one readout an operator scrubs a launch over.
        "vbat_v": t.vbat_v if known & packet.HEALTH_VBAT else None,
        # Flags stay real booleans (nothing downstream has to handle a tri-state
        # here), and `flags_known` says which of them mean anything. A source
        # carrying no flags reports 0, and rendering that as OPEN continuity +
        # FAILED SD would be two fabricated alarms on the safety panel.
        "continuity": t.flag(FLAG_CONTINUITY),
        "pyro_fired": t.flag(FLAG_PYRO_FIRED),
        "sd_ok": t.flag(FLAG_SD_OK),
        "armed": t.flag(FLAG_ARMED),
        "flags_known": fknown,
        # Per-peripheral health. Shipped as the raw byte AND as named booleans +
        # a list of what's down, so the UI can render "BARO LOST" instead of a
        # blanket vehicle failure. Empty list = all nominal.
        #
        # `health_known` marks which bits the frame actually speaks to. Sources
        # that report only some (mrcc.py infers BARO/IMU/GPS and cannot see
        # SD/PYRO/VBAT at all) leave the rest null — a peripheral nobody asked
        # about must not light up as a failure, and `failed_subsystems` names
        # only what is known-bad for the same reason.
        "health": t.health,
        "health_known": known,
        "failed_subsystems": [name for mask, name, _ in packet.SUBSYSTEMS
                              if known & mask and not t.health & mask],
        **{col: (t.healthy(mask) if known & mask else None)
           for mask, _, col in packet.SUBSYSTEMS},
        # Per-packet radio quality. The SX1278 reports these and the E32 could
        # not, so seq gaps are no longer the only link signal — null for a source
        # that doesn't measure them (the binary frame body has no room for them).
        "rssi_dbm": rssi_dbm,
        "snr_db": snr_db,
        # Decoded fields with no slot in PROTOCOL.md — pressure, heading, ground
        # speed, course, horizontal accel/velocity. Passed through verbatim so
        # the UI can show them without a protocol change; the keys are whatever
        # the source produced (mrcc.py canonicalises its aliases first).
        "extra": extra or {},
    }
