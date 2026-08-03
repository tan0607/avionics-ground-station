"""Offline self-tests for the backend's pure logic (no server, no sockets).

Run from the repo root:  backend/.venv/bin/python -m backend.tests
Covers the bits a browser smoke-test wouldn't clearly exercise: loss math across
the uint16 wrap, the exact wire-key contract, and raw.log record framing.
"""
from __future__ import annotations

import struct
import tempfile
from pathlib import Path

from shared.protocol import packet
from shared.protocol.packet import FLAG_ARMED, FLAG_CONTINUITY, FlightState, GpsFix

from .loss import LossTracker
from .session import SessionWriter, _RAW_HEADER
from .wire import WIRE_KEYS, telemetry_to_wire


def test_loss_perfect_sequence() -> None:
    lt = LossTracker()
    for seq in range(1, 101):
        assert lt.observe(seq) == 0
    assert lt.lost == 0 and lt.received == 100 and lt.fraction == 0.0


def test_loss_single_gap() -> None:
    lt = LossTracker()
    lt.observe(10)
    missing = lt.observe(13)          # 11, 12 lost
    assert missing == 2 and lt.lost == 2
    assert lt.expected == 4 and abs(lt.fraction - 0.5) < 1e-9


def test_loss_wraparound() -> None:
    lt = LossTracker()
    lt.observe(65534)
    assert lt.observe(65535) == 0
    assert lt.observe(0) == 0          # 65535 -> 0 is a clean +1 across the wrap
    assert lt.observe(2) == 1          # dropped seq 1
    assert lt.lost == 1


def test_loss_reset_on_huge_jump() -> None:
    lt = LossTracker()
    lt.observe(5000)
    lt.observe(10)                     # backward-looking jump = restart, not 60k lost
    assert lt.lost == 0 and lt.resets == 1


def test_loss_duplicate() -> None:
    lt = LossTracker()
    lt.observe(42)
    assert lt.observe(42) == 0
    assert lt.duplicates == 1 and lt.lost == 0


def test_wire_contract() -> None:
    # Wire keys must exactly match the dashboard WireFrame field set.
    assert set(WIRE_KEYS) == {
        "host_time", "seq", "flight_state", "onboard_ms", "baro_alt_m", "vspeed_ms",
        "gps_lat", "gps_lon", "gps_alt_m", "gps_sats", "gps_fix", "tilt_deg",
        "vbat_v", "continuity", "pyro_fired", "sd_ok", "armed",
        "health", "failed_subsystems",
        "hw_baro", "hw_imu", "hw_gps", "hw_sd", "hw_pyro", "hw_vbat",
    }
    t = packet.Telemetry(
        seq=7, flight_state=FlightState.BOOST, onboard_ms=5000,
        baro_alt_m=1247, vspeed_dms=1420,
        gps_lat=32_437_000, gps_lon=1_017_061_000, gps_alt_m=1300,
        gps_sats=11, gps_fix=GpsFix.FIX_3D, tilt_deg=4, vbat_dv=79,
        flags=FLAG_CONTINUITY | FLAG_ARMED,
        health=packet.HEALTH_ALL_OK,
    )
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000)
    assert set(w) == set(WIRE_KEYS)
    assert w["host_time"] == 1_700_000_000_000
    assert w["flight_state"] == 1 and isinstance(w["flight_state"], int)
    assert abs(w["vspeed_ms"] - 142.0) < 1e-9      # 1420 dm/s
    assert abs(w["vbat_v"] - 7.9) < 1e-9           # 79 * 0.1V
    assert abs(w["gps_lat"] - 3.2437) < 1e-6       # deg*1e7 -> deg
    assert abs(w["gps_lon"] - 101.7061) < 1e-6
    assert w["continuity"] is True and w["armed"] is True
    assert w["pyro_fired"] is False and w["sd_ok"] is False
    # All peripherals nominal -> every hw_* true, nothing named as failed.
    assert w["failed_subsystems"] == [] and w["health"] == packet.HEALTH_ALL_OK
    assert all(w[col] is True for _, _, col in packet.SUBSYSTEMS)

    # A dead barometer must degrade exactly ONE row and NAME it -- the whole
    # point of the health byte is that the operator never sees "AV FAILED".
    t.health = packet.HEALTH_ALL_OK & ~packet.HEALTH_BARO
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000)
    assert w["failed_subsystems"] == ["BARO"], w["failed_subsystems"]
    assert w["hw_baro"] is False
    assert all(w[col] is True for mask, _, col in packet.SUBSYSTEMS
               if mask != packet.HEALTH_BARO)


def test_session_writes_and_raw_framing() -> None:
    t = packet.Telemetry(seq=1, flight_state=FlightState.PAD, onboard_ms=0,
                          baro_alt_m=0, vspeed_dms=0)
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        blob = b"\xAA\x55raw-bytes\x00\x01"
        sw.write_raw(1_700_000_000_000, blob)
        sw.write_row(t, 1_700_000_000_000)
        sw.write_event(t, 1_700_000_000_000)
        sw.close()

        raw = (sw.dir / "raw.log").read_bytes()
        host_ms, n = _RAW_HEADER.unpack(raw[:_RAW_HEADER.size])
        body = raw[_RAW_HEADER.size:_RAW_HEADER.size + n]
        assert host_ms == 1_700_000_000_000 and n == len(blob) and body == blob

        header = (sw.dir / "telemetry.csv").read_text().splitlines()[0]
        assert header.split(",") == packet.CSV_COLUMNS
        assert (sw.dir / "metadata.json").exists()
        assert "landed" not in (sw.dir / "events.csv").read_text()  # PAD isn't labeled landed


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        fn()
        print(f"ok  {fn.__name__}")
    print(f"\nPASS  {len(tests)} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
