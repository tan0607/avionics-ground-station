"""
packet.py -- authoritative telemetry packet codec for the rocket ground station.

SINGLE SOURCE OF TRUTH. The backend serial parser, the fake_telemetry generator,
and the PLDR notebook all import from here. Firmware (C/C++) must match
shared/protocol/PROTOCOL.md byte-for-byte.

Wire frame (32 bytes, little-endian):
    [0:2]   SYNC   = 0xAA 0x55        literal byte sequence, greppable
    [2:30]  BODY   = 28 bytes         (see _BODY struct below)
    [30:32] CRC16  = CRC-16/CCITT-FALSE over BODY, little-endian

Import (from repo root):  from shared.protocol import packet
Self-test:                python3 shared/protocol/packet.py
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

# --- framing ---------------------------------------------------------------
SYNC_BYTES = b"\xAA\x55"
# BODY layout, little-endian, no padding. Order MUST match PROTOCOL.md and firmware.
#   B msg_type | H seq | B flight_state | I onboard_ms | h baro_alt_m | h vspeed_dms |
#   i gps_lat  | i gps_lon | h gps_alt_m | B gps_sats | B gps_fix | B tilt_deg |
#   B vbat_dv  | B flags | B health
_BODY = struct.Struct("<BHBIhhiihBBBBBB")
BODY_SIZE = _BODY.size                              # 28
PACKET_SIZE = len(SYNC_BYTES) + BODY_SIZE + 2       # 32

MSG_TELEMETRY = 0  # msg_type; 1/2 reserved for phase-2 command/ack uplink


class FlightState(IntEnum):
    PAD = 0
    BOOST = 1
    COAST = 2
    APOGEE = 3
    DROGUE = 4   # descent under drogue
    MAIN = 5     # descent under main chute
    LANDED = 6
    # APPENDED, not inserted between PAD and BOOST where it belongs
    # chronologically. 0-6 are on the wire, in every telemetry.csv already
    # written and in the PLDR notebook; renumbering them would silently
    # reinterpret every flight on disk. Chronological order is a DISPLAY
    # concern and the dashboard's timeline carries its own ordering.
    #
    # The vehicle has had this state since the beginning (Flight.cpp's
    # FS_ARMED) and downlinks the word "ARMED". This enum did not have it, so
    # mrcc.py could not map it and every armed frame decoded as PAD -- the
    # console read "on the pad, safe" with the pyro bus live. That is the
    # worst direction for this particular error to fail in.
    ARMED = 7    # on the pad, pyro armed, waiting for the motor


class GpsFix(IntEnum):
    NONE = 0
    FIX_2D = 2
    FIX_3D = 3


# flags bitfield (uint8)
FLAG_CONTINUITY = 1 << 0   # e-match continuity OK
FLAG_PYRO_FIRED = 1 << 1   # a pyro channel has fired
FLAG_SD_OK      = 1 << 2   # onboard SD logging healthy
FLAG_ARMED      = 1 << 3   # flight computer armed
# bits 4-7 reserved

# Every flag a binary frame reports. A source that carries no flags at all (the
# MRCC text downlink) reports none of them, and 0 there must read as "unknown",
# NOT as continuity open + SD failed -- see to_csv_row / backend.wire.
FLAGS_ALL = FLAG_CONTINUITY | FLAG_PYRO_FIRED | FLAG_SD_OK | FLAG_ARMED

# --- health bitfield (uint8) ------------------------------------------------
# PER-PERIPHERAL health. Bit SET = that device initialised and is currently
# responding; bit CLEAR = it is not, and every field it feeds is untrustworthy.
#
# This exists so a single dead peripheral degrades ONE row on the ground station
# instead of collapsing the vehicle into a blanket "AV FAILED". The flight
# computer never aborts boot on an init failure -- it flies with the bit clear.
#
# Init-failure vs in-flight failure is read off the *first* packet of a session:
# a bit clear from the very first frame never came up at all (init failure); a
# bit that goes 1 -> 0 later died in flight. The Log view records both.
HEALTH_BARO = 1 << 0   # barometer responding
HEALTH_IMU  = 1 << 1   # IMU responding
HEALTH_GPS  = 1 << 2   # GPS receiver responding (link alive; NOT fix quality)
HEALTH_SD   = 1 << 3   # SD card present + mounted
HEALTH_PYRO = 1 << 4   # pyro / continuity sense circuit responding
HEALTH_VBAT = 1 << 5   # battery ADC reading in a sane range
# bits 6-7 spare
#
# Note HEALTH_SD ("card mounted") is distinct from FLAG_SD_OK ("writes are
# currently succeeding") -- a mounted card whose writes fail is 1 + 0.

# Ordered roster: (mask, short name, CSV column). Drives the ground-station UI
# and the CSV so adding a peripheral is a one-line change here.
SUBSYSTEMS = (
    (HEALTH_BARO, "BARO", "hw_baro"),
    (HEALTH_IMU,  "IMU",  "hw_imu"),
    (HEALTH_GPS,  "GPS",  "hw_gps"),
    (HEALTH_SD,   "SD",   "hw_sd"),
    (HEALTH_PYRO, "PYRO", "hw_pyro"),
    (HEALTH_VBAT, "VBAT", "hw_vbat"),
)

# All peripherals nominal -- what `health` reads on a clean boot.
HEALTH_ALL_OK = HEALTH_BARO | HEALTH_IMU | HEALTH_GPS | HEALTH_SD | HEALTH_PYRO | HEALTH_VBAT

# Shared CSV column contract -- backend (telemetry.csv) and PLDR notebook agree here.
CSV_COLUMNS = [
    "host_time", "gps_time", "onboard_ms", "seq", "flight_state",
    "baro_alt_m", "vspeed_ms", "gps_lat", "gps_lon", "gps_alt_m",
    "gps_sats", "gps_fix", "tilt_deg", "vbat_v",
    "continuity", "pyro_fired", "sd_ok", "armed",
    *(col for _, _, col in SUBSYSTEMS),
]


@dataclass
class Telemetry:
    seq: int = 0
    flight_state: int = FlightState.PAD
    onboard_ms: int = 0
    baro_alt_m: int = 0          # int16, meters AGL
    vspeed_dms: int = 0          # int16, decimeters/second (dm/s)
    gps_lat: int = 0             # int32, degrees * 1e7
    gps_lon: int = 0             # int32, degrees * 1e7
    gps_alt_m: int = 0           # int16, meters MSL
    gps_sats: int = 0            # uint8
    gps_fix: int = GpsFix.NONE   # uint8
    tilt_deg: int = 0            # uint8, 0..180
    vbat_dv: int = 0             # uint8, volts * 10
    flags: int = 0              # uint8 bitfield
    msg_type: int = MSG_TELEMETRY
    health: int = 0             # uint8 bitfield, HEALTH_* (bit set = peripheral OK)

    # engineering-unit conveniences
    @property
    def vspeed_ms(self) -> float:
        return self.vspeed_dms / 10.0

    @property
    def vbat_v(self) -> float:
        return self.vbat_dv / 10.0

    @property
    def lat_deg(self) -> float:
        return self.gps_lat / 1e7

    @property
    def lon_deg(self) -> float:
        return self.gps_lon / 1e7

    def flag(self, mask: int) -> bool:
        return bool(self.flags & mask)

    def healthy(self, mask: int) -> bool:
        """True if the peripheral(s) in `mask` are initialised and responding."""
        return self.health & mask == mask

    def failed_subsystems(self) -> list[str]:
        """Short names of every peripheral currently reporting unhealthy.

        Empty list = all nominal. Use this instead of a single 'AV OK/FAILED'
        boolean -- the whole point of the health byte is naming what died.
        """
        return [name for mask, name, _ in SUBSYSTEMS if not self.health & mask]

    def to_csv_row(self, host_time: str, gps_time: str = "",
                   health_known: int | None = None,
                   flags_known: int | None = None) -> dict:
        """One dict keyed by CSV_COLUMNS. Backend writes host_time + gps_time.

        `health_known` is a mask of the health bits this frame actually reports.
        None (the default) means all six — a binary frame always carries the
        whole health byte. A source that only knows some of them (see
        shared/protocol/mrcc.py) passes the subset, and the unknown `hw_*` cells
        are written EMPTY rather than 0. That distinction matters downstream:
        PROTOCOL.md is explicit that a 0 means "this peripheral is down", so
        writing 0 for "nobody told us" would put six fabricated failures into the
        post-flight record. `flags_known` does the same for the flags bitfield,
        where a 0 in `continuity` reads as an OPEN e-match circuit.

        `vbat_v` follows the VBAT health bit rather than a flag of its own:
        PROTOCOL.md already defines that bit as gating the field it feeds, and a
        battery reading nobody took must not be recorded as 0.0 V -- on the pad
        that is indistinguishable from a flat pack.
        """
        def flag(mask: int):
            if flags_known is None or flags_known & mask:
                return int(self.flag(mask))
            return ""
        vbat_known = health_known is None or health_known & HEALTH_VBAT
        try:
            state = FlightState(self.flight_state).name
        except ValueError:
            state = self.flight_state
        return {
            "host_time": host_time,
            "gps_time": gps_time,
            "onboard_ms": self.onboard_ms,
            "seq": self.seq,
            "flight_state": state,
            "baro_alt_m": self.baro_alt_m,
            "vspeed_ms": self.vspeed_ms,
            "gps_lat": self.lat_deg,
            "gps_lon": self.lon_deg,
            "gps_alt_m": self.gps_alt_m,
            "gps_sats": self.gps_sats,
            "gps_fix": self.gps_fix,
            "tilt_deg": self.tilt_deg,
            "vbat_v": self.vbat_v if vbat_known else "",
            "continuity": flag(FLAG_CONTINUITY),
            "pyro_fired": flag(FLAG_PYRO_FIRED),
            "sd_ok": flag(FLAG_SD_OK),
            "armed": flag(FLAG_ARMED),
            **{col: (int(bool(self.health & mask))
                     if health_known is None or health_known & mask else "")
               for mask, _, col in SUBSYSTEMS},
        }


# --- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout) ---
def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def encode(t: Telemetry) -> bytes:
    """Telemetry -> 32-byte wire frame."""
    body = _BODY.pack(
        t.msg_type, t.seq, int(t.flight_state), t.onboard_ms,
        t.baro_alt_m, t.vspeed_dms, t.gps_lat, t.gps_lon, t.gps_alt_m,
        t.gps_sats, int(t.gps_fix), t.tilt_deg, t.vbat_dv, t.flags, t.health,
    )
    return SYNC_BYTES + body + struct.pack("<H", crc16_ccitt(body))


def decode(frame: bytes) -> Telemetry | None:
    """Decode exactly one 32-byte frame. Returns None on bad sync or CRC."""
    if len(frame) < PACKET_SIZE or frame[0:2] != SYNC_BYTES:
        return None
    body = frame[2:2 + BODY_SIZE]
    (crc_rx,) = struct.unpack_from("<H", frame, 2 + BODY_SIZE)
    if crc16_ccitt(body) != crc_rx:
        return None
    (msg_type, seq, state, ms, baro, vspd, lat, lon, galt,
     sats, fix, tilt, vbat, flags, health) = _BODY.unpack(body)
    return Telemetry(
        seq=seq, flight_state=state, onboard_ms=ms, baro_alt_m=baro,
        vspeed_dms=vspd, gps_lat=lat, gps_lon=lon, gps_alt_m=galt,
        gps_sats=sats, gps_fix=fix, tilt_deg=tilt, vbat_dv=vbat,
        flags=flags, msg_type=msg_type, health=health,
    )


class PacketParser:
    """Byte-stream framer. Feed arbitrary chunks (e.g. serial reads); it resyncs on
    SYNC_BYTES, validates CRC, and yields decoded Telemetry. CRC failures are counted
    and skipped. 'Raw first, then parse' still applies upstream: log bytes to raw.log
    BEFORE feeding them here.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self.crc_errors = 0

    def feed(self, chunk: bytes):
        self._buf.extend(chunk)
        while True:
            i = self._buf.find(SYNC_BYTES)
            if i < 0:
                # Drop everything except a possible partial sync (trailing 0xAA).
                if self._buf and self._buf[-1] == SYNC_BYTES[0]:
                    del self._buf[:-1]
                else:
                    self._buf.clear()
                return
            if i > 0:
                del self._buf[:i]
            if len(self._buf) < PACKET_SIZE:
                return
            frame = bytes(self._buf[:PACKET_SIZE])
            t = decode(frame)
            if t is not None:
                del self._buf[:PACKET_SIZE]
                yield t
            else:
                self.crc_errors += 1
                del self._buf[:1]  # false sync -- advance one byte and re-search


if __name__ == "__main__":
    # Round-trip + resync self-test.
    sample = Telemetry(
        seq=1234, flight_state=FlightState.BOOST, onboard_ms=5000,
        baro_alt_m=1247, vspeed_dms=1420,
        gps_lat=32_437_000, gps_lon=1_017_061_000, gps_alt_m=1300,
        gps_sats=11, gps_fix=GpsFix.FIX_3D, tilt_deg=4, vbat_dv=79,
        flags=FLAG_CONTINUITY | FLAG_ARMED | FLAG_SD_OK,
        health=HEALTH_ALL_OK,
    )
    frame = encode(sample)
    assert len(frame) == PACKET_SIZE == 32, len(frame)
    assert decode(frame) == sample, "round-trip mismatch"

    # Health byte: a dead peripheral must survive the round trip and be NAMED,
    # not collapse into a blanket failure.
    assert sample.failed_subsystems() == [], sample.failed_subsystems()
    degraded = decode(encode(Telemetry(health=HEALTH_ALL_OK & ~HEALTH_BARO & ~HEALTH_GPS)))
    assert degraded is not None
    assert degraded.failed_subsystems() == ["BARO", "GPS"], degraded.failed_subsystems()
    assert degraded.healthy(HEALTH_IMU) and not degraded.healthy(HEALTH_BARO)
    # An all-zero health byte (pre-health firmware) must not read as "all fine".
    assert len(decode(encode(Telemetry())).failed_subsystems()) == len(SUBSYSTEMS)

    parser = PacketParser()
    noisy = b"\x00\x13junk\xAA" + frame[:7] + frame + b"\xAA" + frame  # garbage + partials + 2 good
    got = []
    for chunk in (noisy[:9], noisy[9:20], noisy[20:]):
        got.extend(parser.feed(chunk))
    assert len(got) == 2, f"expected 2 packets, got {len(got)}"
    print(f"OK  PACKET_SIZE={PACKET_SIZE}  BODY_SIZE={BODY_SIZE}  "
          f"crc16(sample body)=0x{crc16_ccitt(frame[2:2 + BODY_SIZE]):04X}  "
          f"resync parsed={len(got)}  crc_errors={parser.crc_errors}")
