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
#   B vbat_dv  | B flags | B reserved
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

# Shared CSV column contract -- backend (telemetry.csv) and PLDR notebook agree here.
CSV_COLUMNS = [
    "host_time", "gps_time", "onboard_ms", "seq", "flight_state",
    "baro_alt_m", "vspeed_ms", "gps_lat", "gps_lon", "gps_alt_m",
    "gps_sats", "gps_fix", "tilt_deg", "vbat_v",
    "continuity", "pyro_fired", "sd_ok", "armed",
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
    reserved: int = 0

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

    def to_csv_row(self, host_time: str, gps_time: str = "") -> dict:
        """One dict keyed by CSV_COLUMNS. Backend writes host_time + gps_time."""
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
            "vbat_v": self.vbat_v,
            "continuity": int(self.flag(FLAG_CONTINUITY)),
            "pyro_fired": int(self.flag(FLAG_PYRO_FIRED)),
            "sd_ok": int(self.flag(FLAG_SD_OK)),
            "armed": int(self.flag(FLAG_ARMED)),
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
        t.gps_sats, int(t.gps_fix), t.tilt_deg, t.vbat_dv, t.flags, t.reserved,
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
     sats, fix, tilt, vbat, flags, reserved) = _BODY.unpack(body)
    return Telemetry(
        seq=seq, flight_state=state, onboard_ms=ms, baro_alt_m=baro,
        vspeed_dms=vspd, gps_lat=lat, gps_lon=lon, gps_alt_m=galt,
        gps_sats=sats, gps_fix=fix, tilt_deg=tilt, vbat_dv=vbat,
        flags=flags, msg_type=msg_type, reserved=reserved,
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
    )
    frame = encode(sample)
    assert len(frame) == PACKET_SIZE == 32, len(frame)
    assert decode(frame) == sample, "round-trip mismatch"

    parser = PacketParser()
    noisy = b"\x00\x13junk\xAA" + frame[:7] + frame + b"\xAA" + frame  # garbage + partials + 2 good
    got = []
    for chunk in (noisy[:9], noisy[9:20], noisy[20:]):
        got.extend(parser.feed(chunk))
    assert len(got) == 2, f"expected 2 packets, got {len(got)}"
    print(f"OK  PACKET_SIZE={PACKET_SIZE}  BODY_SIZE={BODY_SIZE}  "
          f"crc16(sample body)=0x{crc16_ccitt(frame[2:2 + BODY_SIZE]):04X}  "
          f"resync parsed={len(got)}  crc_errors={parser.crc_errors}")
