"""Offline self-tests for the backend's pure logic (no server, no sockets).

Run from the repo root:  backend/.venv/bin/python -m backend.tests
Covers the bits a browser smoke-test wouldn't clearly exercise: loss math across
the uint16 wrap, the exact wire-key contract, and raw.log record framing.
"""
from __future__ import annotations

import json
import re
import struct
import tempfile
import time
from pathlib import Path

from shared.protocol import mrcc, packet
from shared.protocol.packet import FLAG_ARMED, FLAG_CONTINUITY, FlightState, GpsFix

from .loss import LossTracker
from .session import (
    PACKET_DESC,
    MissionDeriver,
    SessionWriter,
    _RAW_HEADER,
    recorded_flight_detail,
    FlightFolderNotEmpty,
    delete_recorded_flight,
    event_label,
    file_snapshot,
    recorded_flights,
    resolve_recorded_flight_dir,
    resolve_recorded_flight_file,
    slugify,
)
from .sources import ReplaySource
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
        "vbat_v", "continuity", "pyro_fired", "sd_ok", "armed", "flags_known",
        "health", "health_known", "failed_subsystems",
        "hw_baro", "hw_imu", "hw_gps", "hw_sd", "hw_pyro", "hw_vbat",
        "rssi_dbm", "snr_db", "extra",
    }
    t = packet.Telemetry(
        seq=7, flight_state=FlightState.BOOST, onboard_ms=5000,
        baro_alt_m=1247, vspeed_dms=1420,
        gps_lat=40_986_000, gps_lon=1_009_505_000, gps_alt_m=1300,
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
    assert abs(w["gps_lat"] - 4.0986) < 1e-6       # deg*1e7 -> deg
    assert abs(w["gps_lon"] - 100.9505) < 1e-6
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


def test_wire_partial_health_is_null_not_false() -> None:
    """A source that only knows SOME peripherals must not report the rest as
    failed. This is the difference between an operator seeing three unknowns and
    seeing three alarms for hardware nobody asked about."""
    t = packet.Telemetry(seq=1, health=packet.HEALTH_BARO | packet.HEALTH_GPS)
    known = packet.HEALTH_BARO | packet.HEALTH_IMU | packet.HEALTH_GPS
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000, health_known=known)

    assert w["hw_baro"] is True and w["hw_gps"] is True
    assert w["hw_imu"] is False            # known, and known to be down
    assert w["hw_sd"] is None              # not known — must be null, not false
    assert w["hw_pyro"] is None and w["hw_vbat"] is None
    assert w["failed_subsystems"] == ["IMU"], w["failed_subsystems"]
    assert w["health_known"] == known

    # Default (binary frames) is unchanged: every bit reported, nothing null.
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000)
    assert w["health_known"] == packet.HEALTH_ALL_OK
    assert w["hw_sd"] is False
    assert set(w["failed_subsystems"]) == {"IMU", "SD", "PYRO", "VBAT"}


def test_wire_unreported_flags_do_not_become_alarms() -> None:
    """A downlink carrying no flags at all (MRCC) reports flags_known == 0. That
    must not surface as OPEN continuity + FAILED SD — the safety panel is the one
    place a fabricated alarm is most expensive."""
    t = packet.Telemetry(seq=1, flags=0, health=packet.HEALTH_BARO)
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000,
                          health_known=packet.HEALTH_BARO, flags_known=0)
    assert w["flags_known"] == 0
    # The booleans stay false; `flags_known` is what tells the UI to ignore them.
    assert w["continuity"] is False and w["sd_ok"] is False
    # No battery reading was taken -> null, never 0.0 V.
    assert w["vbat_v"] is None

    row = t.to_csv_row("x", health_known=packet.HEALTH_BARO, flags_known=0)
    assert row["continuity"] == "" and row["sd_ok"] == "" and row["armed"] == ""
    assert row["vbat_v"] == ""

    # Binary frames are unchanged: all four flags reported, vbat is a number.
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000)
    assert w["flags_known"] == packet.FLAGS_ALL and w["vbat_v"] == 0.0


def test_csv_unknown_health_cells_are_empty() -> None:
    """PROTOCOL.md: a 0 in hw_* means the peripheral is DOWN. So an unknown bit
    has to be written empty, or the post-flight record gains failures that never
    happened."""
    t = packet.Telemetry(seq=1, health=packet.HEALTH_BARO)
    row = t.to_csv_row("2026-08-19T00:00:00.000+00:00",
                       health_known=packet.HEALTH_BARO | packet.HEALTH_IMU)
    assert row["hw_baro"] == 1
    assert row["hw_imu"] == 0              # reported, and reported down
    assert row["hw_sd"] == "" and row["hw_vbat"] == ""
    # Unchanged when the caller says nothing.
    assert t.to_csv_row("x")["hw_sd"] == 0


def test_mrcc_decodes_the_real_downlink() -> None:
    """A downlink line in the shape the bridge emits must land in the same
    Telemetry shape the binary codec produces, so everything downstream (loss,
    CSV, /ws, dashboard) stays identical between the two formats.

    This is the LONG-key spelling (GPSFIX/GALT/GSPEED/COURSE/STATE) with a fix
    filled in. NOTE the 2026-08-19 capture in flights/ used the later SHORT keys
    (GD/GF/GA/GS/ST) — mrcc.py aliases them, but nothing here asserts that."""
    body = ("MRCC,PKT=207,T=207.5,GPSDATA=1,GPSFIX=0,"
            "SAT=0,LAT=4.098600,LON=100.950500,GALT=45.0,GSPEED=0.00,COURSE=0.0,"
            "AX=0.00,AY=0.00,AZ=9.81,VX=0.00,VY=0.00,VZ=-3.50,ALT=120.0,HDG=103.5,"
            "P=101325,STATE=DESCENT")
    # `len=` is the receiver's payload-length field and mrcc.py validates against
    # it, so it is computed here rather than written by hand — a stale literal
    # would make this fixture a corrupt frame.
    line = f"len={len(body)} RSSI=-53 SNR=10.2 | {body}\n".encode()
    p = mrcc.MrccParser()
    frames = list(p.feed(line))
    assert len(frames) == 1 and p.parse_errors == 0

    t = frames[0].telemetry
    assert t.seq == 207 and t.onboard_ms == 207_500
    assert t.flight_state == FlightState.DROGUE       # MRCC "DESCENT"
    assert t.baro_alt_m == 120
    assert abs(t.vspeed_ms - (-3.5)) < 1e-9           # VZ m/s -> dm/s -> back
    assert abs(t.lat_deg - 4.0986) < 1e-6 and abs(t.lon_deg - 100.9505) < 1e-6
    assert t.gps_alt_m == 45 and t.gps_fix == GpsFix.NONE
    assert frames[0].link.rssi_dbm == -53 and frames[0].link.snr_db == 10.2

    # Fields with no protocol home are kept, not silently dropped.
    assert frames[0].extra["P"] == 101325 and frames[0].extra["HDG"] == 103.5

    # No battery in this revision -> vbat must read as unknown, never as flat.
    assert frames[0].has_battery is False
    _, known = mrcc.health_from_fields(frames[0])
    assert not known & packet.HEALTH_VBAT

    # Those surplus fields must survive all the way onto the wire, or they are
    # parsed-then-dropped — visibly present in the serial stream and visibly
    # absent from the screen, which reads as a broken ground station.
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000,
                          rssi_dbm=frames[0].link.rssi_dbm,
                          snr_db=frames[0].link.snr_db,
                          extra=frames[0].extra)
    assert w["rssi_dbm"] == -53 and w["snr_db"] == 10.2
    assert w["extra"]["P"] == 101325 and w["extra"]["HDG"] == 103.5
    # Binary frames measure neither, and must say so rather than report 0 dBm.
    w = telemetry_to_wire(t, host_time_ms=1_700_000_000_000)
    assert w["rssi_dbm"] is None and w["snr_db"] is None and w["extra"] == {}


def test_mrcc_loss_tracking_matches_binary() -> None:
    """MRCC's PKT counter has to drive LossTracker exactly like `seq` does —
    that is the whole reason it is mapped onto Telemetry.seq."""
    p = mrcc.MrccParser()
    lt = LossTracker()
    stream = b"".join(
        f"MRCC,PKT={n},T={n}.5,AZ=9.81,ALT=0.0,P=101325,STATE=PAD\n".encode()
        for n in (10, 11, 14, 15)          # 12 and 13 lost
    )
    for frame in p.feed(stream):
        lt.observe(frame.telemetry.seq)
    assert lt.received == 4 and lt.lost == 2

    # PKT counts past a uint16; seq wraps, and the wrap must not look like loss.
    p2, lt2 = mrcc.MrccParser(), LossTracker()
    for n in (65535, 65536, 65537):
        for frame in p2.feed(f"MRCC,PKT={n},AZ=9.81,STATE=PAD\n".encode()):
            lt2.observe(frame.telemetry.seq)
    assert lt2.lost == 0 and lt2.resets == 0, (lt2.lost, lt2.resets)


def test_mrcc_ignores_chatter_but_counts_corruption() -> None:
    """The receiver sketch prints its own status lines. Those are not errors and
    must not inflate the count that tells the operator the link is degraded."""
    p = mrcc.MrccParser()
    frames = list(p.feed(b"=== RX BOOT ===\nRX ready\nalive 32046\n"))
    assert frames == [] and p.parse_errors == 0 and p.ignored == 3

    # A line cut in half mid-air IS the text link's version of a CRC failure.
    list(p.feed(b"MRCC,PK\n"))
    assert p.parse_errors == 1


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


def test_mrcc_csv_columns_extend_the_protocol_contract() -> None:
    """MRCC's extra columns may only ever be APPENDED to packet.CSV_COLUMNS.

    Every existing reader (the PLDR notebook, any pandas load, the export in the
    dashboard's Settings view) is positional-or-named against that prefix. Adding
    a column in the middle would silently shift a flight's worth of data one cell
    to the left, which is the kind of corruption nobody notices until analysis.
    """
    assert mrcc.CSV_COLUMNS[:len(packet.CSV_COLUMNS)] == packet.CSV_COLUMNS
    added = mrcc.CSV_COLUMNS[len(packet.CSV_COLUMNS):]
    assert added[:2] == ["rssi_dbm", "snr_db"]
    assert added[-1] == mrcc.EXTRA_COLUMN
    assert len(set(mrcc.CSV_COLUMNS)) == len(mrcc.CSV_COLUMNS)  # no collisions

    # A field the codec already consumes must NOT also get an aux column, or it
    # is empty in every row of every flight.
    for name in mrcc.AUX_FIELDS:
        assert name not in mrcc._CONSUMED, name


def test_mrcc_aux_row_records_what_the_screen_shows() -> None:
    """The Aux strip's fields have to survive into telemetry.csv.

    Regression guard for the actual bug: these were parsed, broadcast, drawn, and
    then dropped at the only step that outlives the session.
    """
    body = ("MRCC,PKT=207,T=207.5,GD=1,GF=0,SAT=0,LAT=0.0,LON=0.0,GA=0.0,GS=1.25,"
            "CRS=90.0,AX=0.10,AY=-0.20,AZ=9.81,VX=0.00,VY=0.00,VZ=0.00,ALT=0.0,"
            "HDG=103.5,P=101325,ROLL=12.5,ST=PAD")
    line = f"len={len(body)} RSSI=-53 SNR=10.2 | {body}"
    frame = mrcc.decode_line(line)
    assert frame is not None

    row = mrcc.aux_csv_row(frame)
    assert set(row) <= set(mrcc.CSV_COLUMNS)                 # DictWriter would raise
    assert row["rssi_dbm"] == -53 and row["snr_db"] == 10.2
    assert row["aux_p"] == 101325 and row["aux_hdg"] == 103.5
    assert row["aux_gspeed"] == 1.25 and row["aux_course"] == 90.0   # short names resolved
    assert row["aux_az"] == 9.81 and row["aux_ax"] == 0.10

    # A key with no column of its own is kept, not dropped — the transmitter
    # renames fields between builds and the record must survive that.
    assert row[mrcc.EXTRA_COLUMN] == "ROLL=12.5"

    # A field this frame never carried reads EMPTY, never 0: aux_temp of 0 would
    # be a post-flight record claiming the airframe was at freezing point.
    assert row["aux_temp"] == "" and row["aux_gx"] == ""

    # Binary sessions keep the narrow contract, so nothing gains dead columns.
    assert "aux_p" not in packet.CSV_COLUMNS


def test_session_writes_mrcc_aux_columns() -> None:
    """End to end: a widened session must actually put the values in the file."""
    body = "MRCC,PKT=9,T=4.0,AZ=9.81,ALT=12.0,P=101300,HDG=88.0,ST=PAD"
    frame = mrcc.decode_line(f"len={len(body)} RSSI=-41 SNR=9.5 | {body}")
    assert frame is not None

    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"}, PACKET_DESC["mrcc"],
                           mrcc.CSV_COLUMNS)
        sw.write_row(frame.telemetry, 1_700_000_000_000, flags_known=0,
                     aux=mrcc.aux_csv_row(frame))
        sw.close()

        text = (sw.dir / "telemetry.csv").read_text().splitlines()
        assert text[0].split(",") == mrcc.CSV_COLUMNS
        row = dict(zip(text[0].split(","), text[1].split(",")))
        assert row["baro_alt_m"] == "12"
        assert row["rssi_dbm"] == "-41" and row["snr_db"] == "9.5"
        assert row["aux_p"] == "101300.0" and row["aux_hdg"] == "88.0"
        assert row["aux_temp"] == ""             # not carried -> empty, not 0

        # metadata.json must describe the file that was actually written, or a
        # replay tool builds the wrong reader.
        meta = json.loads((sw.dir / "metadata.json").read_text())
        assert meta["csv_columns"] == mrcc.CSV_COLUMNS
        assert meta["packet"]["rate_hz"] == 10 and meta["packet"]["tx_repeat"] == 1


def test_mrcc_sd_field_drives_the_sd_health_bit() -> None:
    """The transmitter added SD after the 2026-08-19 revision: 1 = card present
    and mounted, 0 = not. Before this it fell through to `extra`, so the vehicle
    reported its SD state and the peripherals panel still read unknown."""
    def frame(body: str):
        f = mrcc.decode_line(f"MRCC,PKT=1,T=1.0,AX=0.1,AY=0.0,AZ=9.8,{body}")
        assert f is not None
        return mrcc.health_from_fields(f)

    health, known = frame("SD=1")
    assert known & packet.HEALTH_SD and health & packet.HEALTH_SD

    health, known = frame("SD=0")
    assert known & packet.HEALTH_SD and not health & packet.HEALTH_SD

    # No SD field (the 2026-08-19 downlink): unknown, NOT a reported failure.
    health, known = frame("P=101325")
    assert not known & packet.HEALTH_SD

    # HEALTH_SD is "card mounted"; FLAG_SD_OK is "writes succeeding". The
    # downlink speaks to the first only, so the flag must stay unknown even now
    # that MRCC does report flags -- SD=1 says the card mounted, and says
    # nothing whatever about whether writes are landing on it.
    f = mrcc.decode_line("MRCC,PKT=1,T=1.0,AX=0.1,AY=0.0,AZ=9.8,SD=1,AR=1")
    assert f is not None
    _, fknown = mrcc.flags_from_fields(f)
    assert not fknown & packet.FLAG_SD_OK
    row = f.telemetry.to_csv_row("t", flags_known=fknown)
    assert row["sd_ok"] == "", row["sd_ok"]


# The exact line firmware/MRCC_FlightComputer_A/src/Radio.cpp builds today, with a
# fix and a fired charge so every field carries a value worth asserting on.
_CURRENT_BODY = (
    "MRCC,PKT=207,T=207.5,ST=DESCENT,AL=120.5,VZ=-3.5,MX=284.0,AR=1,FI=1,"
    "GD=1,GF=1,SAT=8,LAT=4.098600,LON=100.950500,GA=45.0,GS=1.2,CRS=110,"
    "AX=0.10,AY=0.20,AZ=9.79,GX=1,GY=-2,GZ=0,HDG=103,SD=1,BA=1,IM=1"
)


def _current_frame(body: str = _CURRENT_BODY):
    f = mrcc.decode_line(f"len={len(body)} RSSI=-53 SNR=10.2 | {body}")
    assert f is not None, body
    return f


def test_mrcc_short_keys_do_not_cost_the_altitude() -> None:
    """`ALT` -> `AL` was a transmitter rename that reached nothing on the ground.

    Every unknown key still parses -- it just lands in `extra` and the field it
    should have filled keeps its default -- so this rename did not raise an
    error anywhere. It made a flying rocket read 0 m while the altitude sat in
    plain sight in raw.log. `MX` was lost the same way, and AR/FI with it.
    """
    f = _current_frame()
    t = f.telemetry

    assert t.baro_alt_m == 120                      # AL, not ALT
    assert abs(t.vspeed_ms - (-3.5)) < 1e-9
    assert t.flight_state == FlightState.DROGUE     # ST, not STATE
    assert t.gps_sats == 8 and t.gps_fix == GpsFix.FIX_3D
    assert t.gps_alt_m == 45                        # GA, not GALT

    # Canonicalised, so nothing downstream has to learn a second spelling.
    assert "AL" not in f.extra and "ALT" not in f.extra
    assert f.extra["MX"] == 284.0                   # apogee as the VEHICLE has it


def test_mrcc_recorder_block_reaches_the_screen_and_the_record() -> None:
    """One packet in ten carries the vehicle's recording state: SDF/SDL/SDE.

    Which file is open, how many lines are in it, how many writes failed -- the
    `[SD]` console line, for the operator who is 19 km from the USB port. The
    packet is too full to carry them at 2 Hz, so they ride a 5 s cadence and the
    ground station sees them on about a tenth of the frames. Both halves of that
    have to hold: the fields must land somewhere on the frames that DO carry
    them, and their absence on the other nine must never read as a zero -- a
    flight recorded as `0 lines, 0 errors` is a working recorder described as a
    dead one.
    """
    f = _current_frame(_CURRENT_BODY + ",SDF=7,SDL=3412,SDE=2")

    assert f.extra["SDF"] == 7
    assert f.extra["SDL"] == 3412
    assert f.extra["SDE"] == 2

    row = mrcc.aux_csv_row(f)
    assert set(row) <= set(mrcc.CSV_COLUMNS)             # DictWriter would raise
    assert row["aux_sdf"] == 7 and row["aux_sdl"] == 3412 and row["aux_sde"] == 2
    # Columns of their own, not the catch-all: a field nobody named is a field
    # nobody plots, and the write rate is read off SDL across two rows.
    assert row[mrcc.EXTRA_COLUMN] == ""

    # The nine packets in between carry no recorder fields at all. Empty, not 0.
    quiet = mrcc.aux_csv_row(_current_frame())
    assert quiet["aux_sdf"] == "" and quiet["aux_sdl"] == "" and quiet["aux_sde"] == ""

    # SD is still the only health evidence. SDE counts failed WRITES on a card
    # that is still mounted, and a mounted card is what HEALTH_SD means -- the
    # ground station reads a stalled line count as a recording fault itself,
    # rather than this parser inventing a peripheral failure the vehicle never
    # reported.
    health, known = mrcc.health_from_fields(f)
    assert known & packet.HEALTH_SD and health & packet.HEALTH_SD


def test_mrcc_health_prefers_what_the_vehicle_states() -> None:
    """BA/IM are the flight computer's own baroOK/imuOK. They beat every proxy.

    The IMU proxy is the one that mattered: firmware before 2026-09-04 skipped
    the sensor read while the IMU was down, so ax/ay/az held their LAST GOOD
    values and "all three exactly zero" never came true. A dead IMU downlinked
    9.79 forever and this console called it healthy.
    """
    health, known = mrcc.health_from_fields(_current_frame())
    for mask in (packet.HEALTH_BARO, packet.HEALTH_IMU,
                 packet.HEALTH_GPS, packet.HEALTH_SD):
        assert known & mask and health & mask, mask

    # The case the axis heuristic could not see: vehicle says IMU down, axes
    # still reading a plausible 1 g because they are frozen, not zeroed.
    frozen = _CURRENT_BODY.replace("IM=1", "IM=0")
    health, known = mrcc.health_from_fields(_current_frame(frozen))
    assert known & packet.HEALTH_IMU and not health & packet.HEALTH_IMU

    # Same for the barometer: BA=0 with a stale altitude still in the packet.
    health, known = mrcc.health_from_fields(_current_frame(_CURRENT_BODY.replace("BA=1", "BA=0")))
    assert known & packet.HEALTH_BARO and not health & packet.HEALTH_BARO

    # Older revisions carry neither bit, and the proxies must still work there.
    older = "MRCC,PKT=1,T=1.0,GD=1,AX=0.0,AY=0.0,AZ=9.8,ALT=0.0,P=101325,ST=PAD"
    health, known = mrcc.health_from_fields(_current_frame(older))
    assert known & packet.HEALTH_BARO and health & packet.HEALTH_BARO   # P > 0
    assert known & packet.HEALTH_IMU and health & packet.HEALTH_IMU     # AZ != 0

    # ...and a genuinely dead-from-boot IMU on that older format still reads DOWN.
    dead = "MRCC,PKT=1,T=1.0,GD=1,AX=0.0,AY=0.0,AZ=0.0,ALT=0.0,P=101325,ST=PAD"
    health, known = mrcc.health_from_fields(_current_frame(dead))
    assert known & packet.HEALTH_IMU and not health & packet.HEALTH_IMU

    # No LORA bit, ever: a packet that arrived is its own proof of link.
    assert not any(name == "LORA" for _, name, _ in packet.SUBSYSTEMS)


def test_mrcc_reports_the_pyro_state_it_is_sent() -> None:
    """AR/FI reached `extra` and stopped there, so ARMED and PYRO FIRED rendered
    as unknown with the vehicle actively reporting both."""
    flags, known = mrcc.flags_from_fields(_current_frame())
    assert known & FLAG_ARMED and flags & FLAG_ARMED
    assert known & packet.FLAG_PYRO_FIRED and flags & packet.FLAG_PYRO_FIRED

    safe = _CURRENT_BODY.replace("AR=1,FI=1", "AR=0,FI=0")
    flags, known = mrcc.flags_from_fields(_current_frame(safe))
    assert known & FLAG_ARMED and not flags & FLAG_ARMED
    assert known & packet.FLAG_PYRO_FIRED and not flags & packet.FLAG_PYRO_FIRED

    # Not downlinked -> still unknown. A missing continuity field must not read
    # as an open e-match, which is the alarm an operator scrubs a launch over.
    assert not known & FLAG_CONTINUITY and not known & packet.FLAG_SD_OK

    # An older frame carries neither, and must report neither.
    _, known = mrcc.flags_from_fields(_current_frame("MRCC,PKT=1,T=1.0,AZ=9.8,ST=PAD"))
    assert known == 0

    # End to end: a fired charge has to survive onto the wire as a real boolean
    # the safety panel will light up, not as a null.
    f = _current_frame()
    flags, fknown = mrcc.flags_from_fields(f)
    f.telemetry.flags = flags
    w = telemetry_to_wire(f.telemetry, host_time_ms=1_700_000_000_000, flags_known=fknown)
    assert w["pyro_fired"] is True and w["armed"] is True
    assert w["flags_known"] & packet.FLAG_PYRO_FIRED


def test_every_flight_phase_the_firmware_sends_is_recognised() -> None:
    """The vehicle's stateName(), checked against this parser's prefix table.

    The twin of the field-name test above, and it exists because the same class
    of silence bit twice. An unmapped phase word does not raise: decode_line
    falls back to PAD. So ARMED -- a state the vehicle has always had -- decoded
    as "on the pad, safe" on a console watching a vehicle whose pyro bus was
    live. The dashboard flagged it only because MrccParser happens to count
    unknown_states; nothing failed, and nothing had to.

    A phase must map to SOME FlightState. It need not be its own: the protocol
    deliberately folds ASCENT into BOOST and DESCENT into DROGUE, because an
    MRCC frame cannot distinguish the pairs. Falling back to PAD is the one
    outcome this rejects.
    """
    src = Path(__file__).resolve().parent.parent / "firmware/MRCC_FlightComputer_A/src/Flight.cpp"
    assert src.is_file(), f"flight state source moved: {src}"

    body = src.read_text()
    start = body.index("stateName(uint8_t")
    block = body[start:body.index("}", body.index("switch", start))]

    # `case FS_ARMED:   return "ARMED";` -> ARMED. The default arm returns "?",
    # which is not a phase and is excluded by the character class.
    words = sorted(set(re.findall(r'return\s+"([A-Z]+)"', block)))
    assert "PAD" in words and "ARMED" in words, words   # the extraction works

    unmapped = [w for w in words if mrcc.state_to_flight_state(w) is None]
    assert not unmapped, (
        f"firmware sends flight phases {unmapped} and mrcc.py maps none of them "
        f"-- each one decodes as PAD, so the console reports a vehicle that is "
        f"not on the pad as being on it. Add a prefix to STATE_PREFIXES."
    )

    # ARMED specifically must NOT collapse into PAD, which is what it did.
    assert mrcc.state_to_flight_state("ARMED") == packet.FlightState.ARMED
    assert mrcc.state_to_flight_state("ARM") == packet.FlightState.ARMED

    # Every mapped phase needs a mission-event label, or the Log view records
    # `state_7` for the moment the pyro went live.
    for w in words:
        state = mrcc.state_to_flight_state(w)
        assert not event_label(state).startswith("state_"), (w, state)


def test_armed_is_on_the_pad_not_in_flight() -> None:
    """ARMED must not read as launched anywhere that matters.

    The console starts its T+ clock on the first frame that is not PAD. Adding
    a state without touching that test would have started the mission clock the
    moment someone armed -- minutes before the motor, on every chart and every
    recorded flight. hasLaunched() in the dashboard carries the same rule; this
    asserts the backend half.
    """
    body = ("MRCC,PKT=9,T=4.5,ST=ARMED,AL=0.2,VZ=0.0,MX=0.2,AR=1,FI=0,"
            "GD=1,GF=1,SAT=9,AX=0.0,AY=0.0,AZ=9.8,SD=1,BA=1,IM=1")
    f = _current_frame(body)
    assert f.state_name == "ARMED" and f.state_recognised
    assert f.telemetry.flight_state == packet.FlightState.ARMED

    # The flag and the phase agree, and both are reported.
    flags, fknown = mrcc.flags_from_fields(f)
    assert fknown & FLAG_ARMED and flags & FLAG_ARMED

    # A parser that fell back would record it as an unknown word; it must not.
    p = mrcc.MrccParser()
    list(p.feed(f"len={len(body)} RSSI=-50 SNR=9.0 | {body}\n".encode()))
    assert p.unknown_states == set(), p.unknown_states


def test_every_field_the_firmware_sends_has_somewhere_to_land() -> None:
    """The transmitter's format string, checked against this parser.

    This is the test the last four renames needed and did not have. mrcc.py
    cannot fail loudly on an unknown key -- looking fields up by name is exactly
    what stops a rename shifting a column -- so a dropped field is silent, and
    `AL` proved it can be silent for a whole flight. The firmware lives in this
    repo, so the contract is checkable here rather than on the pad.

    A key must be either CONSUMED into a Telemetry field or named in AUX_FIELDS
    (which gives it a telemetry.csv column). Landing only in `aux_extra` is not
    enough: that is the catch-all, and a field nobody named is a field nobody
    plotted.
    """
    # The GROUND STATION, not the vehicle. The downlink is binary now, so the
    # transmitter has no format strings to read: the keys are written by the
    # decoder that expands the frame, and that is also the honest place to check
    # them, because it is literally what reaches this laptop.
    src = Path(__file__).resolve().parent.parent / "firmware/MRCC_GroundStation/MRCC_GroundStation.ino"
    assert src.is_file(), f"receiver source moved: {src}"

    text = src.read_text()
    start = text.index("---- TLM DECODE BEGIN ----")
    body = text[start:text.index("---- TLM DECODE END ----", start)]

    # `AL=%.1f` -> AL. Only inside the format strings, so the argument lists and
    # the comments around them cannot contribute false keys.
    keys = {m for m in re.findall(r'([A-Z][A-Z0-9]*)=%', body)}
    assert "PKT" in keys and "AL" in keys, keys      # the extraction itself works

    known = set(mrcc._CONSUMED) | set(mrcc.AUX_FIELDS)
    homeless = sorted(mrcc.FIELD_ALIASES.get(k, k) for k in keys) 
    homeless = [k for k in homeless if k not in known]
    assert not homeless, (
        f"firmware sends {homeless} and mrcc.py has no home for them -- add an "
        f"alias, consume it, or give it an AUX_FIELDS column"
    )


def _write_raw_log(dir_: Path, records: list[tuple[int, bytes]],
                   codec: str | None = None, truncate: bool = False) -> Path:
    """A minimal session dir: raw.log in session.py's framing (+ metadata.json)."""
    dir_.mkdir(parents=True, exist_ok=True)
    blob = b"".join(_RAW_HEADER.pack(ms, len(c)) + c for ms, c in records)
    if truncate:
        blob = blob[:-3]                      # tear the last record's payload
    (dir_ / "raw.log").write_bytes(blob)
    if codec:
        (dir_ / "metadata.json").write_text(json.dumps({"packet": {"codec": codec}}))
    return dir_


def _drain(source: ReplaySource) -> list[bytes]:
    import asyncio

    async def go() -> list[bytes]:
        return [c async for c in source.chunks()]

    return asyncio.run(go())


def test_replay_yields_recorded_chunks_verbatim() -> None:
    """Chunk boundaries are part of the recording, not an implementation detail.

    Where a serial read split the stream is exactly what exercises the parser's
    resync, so a replay that re-chunked would test a stream the radio never sent.
    """
    recs = [(1_700_000_000_000, b"MRCC,PKT=1,"), (1_700_000_000_250, b"AZ=9.8\n"),
            (1_700_000_000_500, b"MRCC,PKT=2,AZ=9.9\n")]
    with tempfile.TemporaryDirectory() as tmp:
        d = _write_raw_log(Path(tmp) / "s", recs)
        src = ReplaySource(d, fast=True)
        assert src.records == 3 and src.bytes == sum(len(c) for _, c in recs)
        assert not src.truncated
        assert _drain(src) == [c for _, c in recs]


def test_replay_accepts_a_session_dir_or_the_log_itself() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        d = _write_raw_log(Path(tmp) / "s", [(1, b"x")])
        assert ReplaySource(d, fast=True).path == ReplaySource(d / "raw.log", fast=True).path


def test_replay_stops_cleanly_on_a_torn_tail() -> None:
    """'Raw first' means bytes hit the disk before anything validates them, so a
    killed run leaves a half-written record. That ends the replay; it never raises."""
    recs = [(1, b"whole-record"), (2, b"half-written")]
    with tempfile.TemporaryDirectory() as tmp:
        d = _write_raw_log(Path(tmp) / "s", recs, truncate=True)
        src = ReplaySource(d, fast=True)
        assert src.records == 1 and src.truncated
        assert _drain(src) == [b"whole-record"]


def test_replay_reads_its_codec_from_the_session_metadata() -> None:
    """Replaying MRCC bytes through the binary parser decodes nothing and looks
    exactly like a dead link. The recording knows which codec wrote it."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        assert ReplaySource(_write_raw_log(root / "m", [(1, b"x")], codec="mrcc"),
                            fast=True).detect_format() == "mrcc"
        assert ReplaySource(_write_raw_log(root / "b", [(1, b"x")], codec="binary"),
                            fast=True).detect_format() == "binary"
        # No metadata, or a codec this build doesn't know: say so instead of guessing.
        assert ReplaySource(_write_raw_log(root / "n", [(1, b"x")]),
                            fast=True).detect_format() is None
        assert ReplaySource(_write_raw_log(root / "j", [(1, b"x")], codec="martian"),
                            fast=True).detect_format() is None


def test_replay_clamps_the_dead_air_between_bench_runs() -> None:
    """raw.log is appended per session dir, so one file spans days of runs. Real
    pacing is kept; the multi-hour gaps between runs are collapsed to the cap."""
    hour = 3_600_000
    recs = [(0, b"a"), (200, b"b"), (200 + hour, b"c")]
    with tempfile.TemporaryDirectory() as tmp:
        src = ReplaySource(_write_raw_log(Path(tmp) / "s", recs), max_gap=0.5)
        started = time.monotonic()
        assert _drain(src) == [b"a", b"b", b"c"]
        elapsed = time.monotonic() - started
        # 0.2 s honoured + the 1-hour gap clamped to 0.5 s -- not 3600 s.
        assert 0.7 <= elapsed < 3.0, elapsed


def test_replay_rejects_an_empty_log_at_construction() -> None:
    """Fail at the CLI, not with a server up and a dashboard that never fills in."""
    with tempfile.TemporaryDirectory() as tmp:
        d = _write_raw_log(Path(tmp) / "s", [])
        try:
            ReplaySource(d)
        except ValueError:
            pass
        else:
            raise AssertionError("empty raw.log should have been rejected")


# ----------------------------------------------------------------------------
# Flight recording: the operator-declared folder inside a session
# ----------------------------------------------------------------------------
def _pad(seq: int = 1, alt: int = 0, state: int = FlightState.PAD) -> packet.Telemetry:
    return packet.Telemetry(seq=seq, flight_state=state, onboard_ms=seq * 500,
                            baro_alt_m=alt, vspeed_dms=0)


def test_flight_folder_is_a_complete_record() -> None:
    """A flight folder must hold the SAME five files as its session.

    Anything less makes it a summary rather than a record, and the whole point
    of cutting one is that it can be handed to the PLDR notebook (or replayed)
    on its own.
    """
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"}, PACKET_DESC["binary"])
        flight = sw.start_flight("Launch A")
        sw.write_raw(1_700_000_000_000, b"\xAA\x55bytes")
        sw.write_row(_pad(1), 1_700_000_000_000)
        sw.write_event(_pad(1), 1_700_000_000_000)
        summary = sw.stop_flight()
        sw.close()

        for name in ("metadata.json", "raw.log", "telemetry.csv", "events.csv",
                     "mission.log"):
            assert (flight.dir / name).is_file(), f"flight is missing {name}"
        assert flight.dir.name == "flight-01_launch-a"
        assert summary["rows"] == 1 and summary["recording"] is False

        meta = json.loads((flight.dir / "metadata.json").read_text())
        # Rewritten on close: a folder that still claims to be recording is
        # indistinguishable from one holding a whole flight.
        assert meta["recording"] is False and meta["stop_reason"] == "operator"
        assert meta["stopped_utc"] and meta["duration_s"] is not None
        assert meta["rows"] == 1
        # The codec key lives where ReplaySource.detect_format() looks for it.
        assert meta["packet"]["codec"] == "binary"


def test_flight_records_only_its_own_span() -> None:
    """Frames outside start..stop belong to the session, never to the flight."""
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        sw.write_row(_pad(1), 1_700_000_000_000)      # before RECORD
        flight = sw.start_flight()
        sw.write_row(_pad(2), 1_700_000_001_000)      # during
        sw.write_row(_pad(3), 1_700_000_002_000)
        sw.stop_flight()
        sw.write_row(_pad(4), 1_700_000_003_000)      # after STOP
        sw.close()

        def seqs(path: Path) -> list[str]:
            rows = path.read_text().splitlines()
            cols = rows[0].split(",")
            return [dict(zip(cols, r.split(",")))["seq"] for r in rows[1:]]

        assert seqs(flight.dir / "telemetry.csv") == ["2", "3"]
        # The session keeps everything: hitting RECORD late costs a tidy folder,
        # never the data.
        assert seqs(sw.dir / "telemetry.csv") == ["1", "2", "3", "4"]


def test_double_start_is_refused_rather_than_rolling() -> None:
    """A second START must not close the flight already recording.

    Two browser tabs, or one double-click, would otherwise cut a live flight in
    half at the worst possible moment.
    """
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        first = sw.start_flight("one")
        try:
            sw.start_flight("two")
        except RuntimeError:
            pass
        else:
            raise AssertionError("second start_flight should have been refused")
        assert sw.flight is first
        sw.close()


def test_session_close_finalises_an_open_flight() -> None:
    """A killed server must leave a closed, honest flight folder."""
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        flight = sw.start_flight()
        sw.write_row(_pad(1), 1_700_000_000_000)
        sw.close()                                   # no stop_flight() first

        meta = json.loads((flight.dir / "metadata.json").read_text())
        assert meta["recording"] is False and meta["stop_reason"] == "shutdown"


def test_flight_label_cannot_escape_the_session_folder() -> None:
    """The label arrives in an HTTP body, so it is sanitised, not trusted."""
    assert slugify("../../etc/passwd") == "etc-passwd"
    assert slugify("Launch #2 (wet)") == "launch-2-wet"
    assert slugify("///") == ""                      # falls back to bare flight-NN
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        flight = sw.start_flight("../escape")
        assert flight.dir.parent == sw.dir
        sw.close()


def test_flight_folder_replays_on_its_own() -> None:
    """`--replay flights/<session>/flight-01` must work like replaying a session."""
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"}, PACKET_DESC["mrcc"],
                           mrcc.CSV_COLUMNS)
        flight = sw.start_flight()
        for i, chunk in enumerate((b"MRCC,PKT=1,AZ=9.81,STATE=PAD\n",
                                   b"MRCC,PKT=2,AZ=9.80,STATE=PAD\n")):
            sw.write_raw(1_700_000_000_000 + i * 500, chunk)
        sw.stop_flight()
        sw.close()

        src = ReplaySource(flight.dir, fast=True)
        assert src.records == 2
        # The flight's own metadata.json names the codec, so the CLI does not
        # have to be told -- the same silent failure --format warns about.
        assert src.detect_format() == "mrcc"


def test_mission_deriver_logs_state_pyro_and_link_edges() -> None:
    """The events the Log view derives in the browser, derived on the backend."""
    md = MissionDeriver()

    msgs = [e.message for e in md.observe(_pad(1), 1_000)]
    assert msgs == ["LINK LIVE", "FIRST FRAME"]

    # A repeat of the same state says nothing new.
    assert md.observe(_pad(2), 1_500) == []

    boost = [e.message for e in md.observe(_pad(3, alt=12, state=FlightState.BOOST), 2_000)]
    assert boost == ["LIFTOFF"]

    # Link loss is proven by SILENCE, so only the wall-clock tick can find it.
    # The last frame arrived at 2_000; the thresholds are 3 s and 6 s of quiet.
    assert md.tick(4_500) == []                       # 2.5 s quiet: still live
    assert [e.message for e in md.tick(5_500)] == ["LINK STALE"]
    assert md.tick(6_000) == []                       # edge-triggered, not repeated
    assert [e.message for e in md.tick(8_500)] == ["LINK LOST"]
    # ...and a frame arriving IS the recovery.
    assert "LINK LIVE" in [e.message for e in md.observe(_pad(4), 9_500)]


def test_mission_deriver_does_not_invent_a_pyro_it_cannot_see() -> None:
    """A never-set bit is unknown, not 'has not fired'.

    Same rule the safety panel already follows: flags_known gates the reading,
    so a source reporting no flags must not produce a PYRO event either way.
    MRCC does now report ARMED and PYRO_FIRED (see flags_from_fields), but it
    still reports no continuity and no SD_OK, and the older revisions of the
    downlink report nothing at all -- so the gate still has to hold.
    """
    md = MissionDeriver()
    t = _pad(1)
    t.flags = packet.FLAG_PYRO_FIRED
    msgs = [e.message for e in md.observe(t, 1_000, flags_known=0)]
    assert "PYRO FIRED" not in msgs

    md2 = MissionDeriver()
    md2.observe(_pad(1), 1_000)                       # binary source: all flags known
    t2 = _pad(2)
    t2.flags = packet.FLAG_PYRO_FIRED
    assert "PYRO FIRED" in [e.message for e in md2.observe(t2, 1_500)]


def test_mission_log_is_written_to_both_session_and_flight() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        sw = SessionWriter(Path(tmp), {"kind": "test"})
        md = MissionDeriver()
        for e in md.observe(_pad(1), 1_700_000_000_000):
            sw.write_mission(e)
        flight = sw.start_flight()
        for e in md.observe(_pad(2, alt=9, state=FlightState.BOOST), 1_700_000_001_000):
            sw.write_mission(e)
        sw.stop_flight()
        sw.close()

        session_log = (sw.dir / "mission.log").read_text()
        flight_log = (flight.dir / "mission.log").read_text()
        assert "FIRST FRAME" in session_log and "LIFTOFF" in session_log
        # The flight file starts when RECORD was pressed, so it holds the
        # liftoff and not the frame that preceded it.
        assert "LIFTOFF" in flight_log and "FIRST FRAME" not in flight_log


# ----------------------------------------------------------------------------
# Recorded-flight archive: persistent portal discovery + bounded previews
# ----------------------------------------------------------------------------
def _write_recorded_flight(root: Path, session: str, flight: str,
                           started: str, rows: int = 3) -> Path:
    directory = root / session / flight
    directory.mkdir(parents=True)
    (directory / "metadata.json").write_text(json.dumps({
        "flight": flight,
        "session_id": session,
        "label": flight.replace("flight-", "Launch "),
        "started_utc": started,
        "stopped_utc": started,
        "duration_s": 12.5,
        "stop_reason": "operator",
        "recording": False,
        "rows": rows,
        "events": 1,
        "raw_bytes": 42,
        "source": {"kind": "test"},
        "packet": {"codec": "mrcc"},
    }))
    (directory / "telemetry.csv").write_text(
        "host_time,seq,baro_alt_m\n" +
        "".join(f"2026-09-01T00:00:0{i}Z,{i},{i * 10}\n" for i in range(rows))
    )
    (directory / "events.csv").write_text("host_time,event\n")
    (directory / "mission.log").write_text(
        "# mission.log\n" + "".join(f"event {i}\n" for i in range(rows))
    )
    (directory / "raw.log").write_bytes(b"recorded bytes")
    return directory


def test_recorded_flights_are_discovered_from_disk_newest_first() -> None:
    """The portal index must survive a backend restart, not rely on memory."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        older = _write_recorded_flight(
            root, "2026-08-31T23-59-00Z", "flight-01", "2026-08-31T23:59:10.000+00:00"
        )
        _write_recorded_flight(
            root, "2026-09-01T04-12-57Z", "flight-01_launch-a",
            "2026-09-01T04:24:02.717+00:00",
        )

        flights = recorded_flights(root)
        assert [f["session"] for f in flights] == [
            "2026-09-01T04-12-57Z", "2026-08-31T23-59-00Z",
        ]
        assert flights[0]["flight"] == "flight-01_launch-a"
        assert {f["name"] for f in flights[0]["files"]} == {
            "metadata.json", "telemetry.csv", "events.csv", "mission.log", "raw.log",
        }
        assert Path(flights[1]["path"]) == older.resolve()


def test_recorded_flights_skip_malformed_metadata() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        valid = _write_recorded_flight(
            root, "2026-09-01T04-12-57Z", "flight-01", "2026-09-01T04:24:02+00:00"
        )
        broken = valid.parent / "flight-02"
        broken.mkdir()
        (broken / "metadata.json").write_text("{not json")
        nonsensical = valid.parent / "flight-03"
        nonsensical.mkdir()
        (nonsensical / "metadata.json").write_text(json.dumps({
            "flight": "flight-03", "rows": "many", "raw_bytes": {},
        }))
        assert [f["flight"] for f in recorded_flights(root)] == ["flight-01"]


def test_recorded_flight_path_resolution_rejects_traversal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        directory = _write_recorded_flight(
            root, "2026-09-01T04-12-57Z", "flight-01", "2026-09-01T04:24:02+00:00"
        )
        assert resolve_recorded_flight_dir(
            root, "2026-09-01T04-12-57Z", "flight-01"
        ) == directory.resolve()
        assert resolve_recorded_flight_dir(root, "../outside", "flight-01") is None
        assert resolve_recorded_flight_dir(
            root, "2026-09-01T04-12-57Z", "../../outside"
        ) is None


def test_recorded_flight_file_resolution_rejects_symlinks() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        directory = _write_recorded_flight(
            root, "2026-09-01T04-12-57Z", "flight-01", "2026-09-01T04:24:02+00:00"
        )
        outside = root.parent / f"{root.name}-outside.txt"
        outside.write_text("not part of the flight")
        (directory / "mission.log").unlink()
        (directory / "mission.log").symlink_to(outside)
        try:
            assert resolve_recorded_flight_file(
                root, "2026-09-01T04-12-57Z", "flight-01", "mission.log"
            ) is None
            assert resolve_recorded_flight_file(
                root, "2026-09-01T04-12-57Z", "flight-01", "../../secret"
            ) is None
        finally:
            outside.unlink(missing_ok=True)


def test_recorded_flight_detail_previews_are_bounded() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write_recorded_flight(
            root, "2026-09-01T04-12-57Z", "flight-01",
            "2026-09-01T04:24:02+00:00", rows=5,
        )
        detail = recorded_flight_detail(
            root, "2026-09-01T04-12-57Z", "flight-01",
            mission_lines=2, telemetry_rows=2,
        )
        assert detail is not None
        assert detail["mission"]["lines"] == ["event 3", "event 4"]
        assert detail["mission"]["truncated"] is True
        assert detail["telemetry"]["columns"] == ["host_time", "seq", "baro_alt_m"]
        assert [row["seq"] for row in detail["telemetry"]["rows"]] == ["3", "4"]
        assert detail["telemetry"]["truncated"] is True


def test_download_snapshot_is_a_prefix_of_a_file_still_being_written() -> None:
    """The export endpoints serve files the ingest loop is APPENDING to.

    A plain file response fixes Content-Length by stat and then streams, so one
    more telemetry row written mid-download made the body outrun its own header
    and the transfer died with "Response content longer than Content-Length" --
    which is exactly when an operator exports: during the flight.
    """
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "telemetry.csv"
        path.write_bytes(b"host_time,seq\n" + b"2026-01-01,1\n" * 200)
        original = path.read_bytes()

        size, chunks = file_snapshot(path)
        # ...the flight carries on between the header and the body.
        with path.open("ab") as fh:
            fh.write(b"2026-01-01,999\n" * 500)

        body = b"".join(chunks)
        assert size == len(original)
        assert body == original, "body must be exactly the promised byte count"


def test_delete_removes_only_the_files_it_wrote() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        sw = SessionWriter(root, {"kind": "test"})
        flight = sw.start_flight("scrub")
        sw.write_row(_pad(1), 1_700_000_000_000)
        sw.stop_flight()
        sw.close()

        assert len(recorded_flights(root)) == 1
        out = delete_recorded_flight(root, sw.session_id, flight.dir.name)
        assert out is not None and out["flight"] == flight.dir.name
        assert sorted(out["removed"]) == ["events.csv", "metadata.json", "mission.log",
                                          "raw.log", "telemetry.csv"]
        assert not flight.dir.exists()
        assert recorded_flights(root) == []
        # The SESSION is untouched -- deleting a flight is not deleting the run.
        assert (sw.dir / "telemetry.csv").is_file()


def test_delete_refuses_a_folder_holding_anything_else() -> None:
    """"Delete the flight I picked" must never become "delete a file you forgot"."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        sw = SessionWriter(root, {"kind": "test"})
        flight = sw.start_flight()
        sw.stop_flight()
        sw.close()
        (flight.dir / "apogee-plot.png").write_bytes(b"not ours")

        try:
            delete_recorded_flight(root, sw.session_id, flight.dir.name)
        except FlightFolderNotEmpty as exc:
            assert "apogee-plot.png" in str(exc)
        else:
            raise AssertionError("delete should have refused the unknown file")
        assert (flight.dir / "apogee-plot.png").is_file()
        assert (flight.dir / "telemetry.csv").is_file(), "refusal must delete nothing"


def test_delete_rejects_traversal_and_unknown_flights() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "keep.txt").write_text("not a flight")
        sw = SessionWriter(root, {"kind": "test"})
        sw.close()
        for session_id, flight_name in (
            ("..", "flight-01"),
            (sw.session_id, ".."),
            (sw.session_id, "../../etc"),
            (sw.session_id, "flight-99"),
            ("2026-01-01T00-00-00Z", "flight-01"),
        ):
            assert delete_recorded_flight(root, session_id, flight_name) is None
        assert (root / "keep.txt").is_file()


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        fn()
        print(f"ok  {fn.__name__}")
    print(f"\nPASS  {len(tests)} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
