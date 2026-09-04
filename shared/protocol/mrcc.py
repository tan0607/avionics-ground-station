"""MRCC text-telemetry codec — the format the SX1278 link ACTUALLY carries today.

`packet.py` remains the project's designed wire format (32 binary bytes, CRC-16).
This module exists because the airborne board currently downlinks something else:
~190 bytes of ASCII key=value, which `packet.PacketParser` can only throw away
(it hunts for 0xAA55 and never finds it).

RATE, measured rather than assumed (live /stats delta over 20 s, 2026-08-20, and
confirmed against flights/2026-08-19T05-54-40Z): the transmitter emits **2 Hz**
-- 500 ms between onboard timestamps -- and sends EVERY PACKET TWICE, ~205 ms
apart. So the receiver prints ~4 lines/s while only 2 of them carry new
telemetry. Both numbers matter and they are not interchangeable: the line rate
is what a serial monitor shows you, the frame rate is how often the console can
actually change, and expecting the first from the second reads as a ground
station lagging its own radio. `LossTracker` already counts the repeats as
`duplicates` (delta == 0), so they inflate neither loss nor the frame count.

One line off the bridge's USB port looks like this — the `len=/RSSI=/SNR=` prefix
is added by the GROUND receiver sketch, everything after the `|` came over the air:

    len=191 RSSI=-53 SNR=10.2 | MRCC,PKT=207,T=207.5,GPSDATA=1,GPSFIX=0,SAT=0,
    LAT=0.000000,LON=0.000000,GALT=0.0,GSPEED=0.00,COURSE=0.0,AX=0.00,AY=0.00,
    AZ=9.81,VX=0.00,VY=0.00,VZ=0.00,ALT=0.0,HDG=103.5,P=101325,STATE=LANDED

The build in `firmware/` today (Radio.cpp) sends short keys throughout, drops
P/VX/VY for the byte budget, and adds the flight and health fields the line above
had no room for:

    len=185 RSSI=-53 SNR=10.2 | MRCC,PKT=207,T=207.5,ST=PAD,AL=0.4,VZ=0.0,MX=0.4,
    AR=0,FI=0,GD=1,GF=1,SAT=8,LAT=4.098600,LON=100.950500,GA=45.0,GS=0.0,CRS=0,
    AX=0.10,AY=0.20,AZ=9.79,GX=0,GY=0,GZ=0,HDG=103,SD=1,BA=1,IM=1

An older revision of the same transmitter emitted a shorter set with a vehicle id
and a battery reading (`MRCC,RKT01,PKT=16,...,BAT=4.15,TEMP=29.6,STATE=ASCENT`).
All three parse here: every field is looked up by NAME, never by position, so
adding or dropping a key on the transmitter cannot silently shift a column.

WHAT THIS DELIBERATELY DOES NOT DO: it does not become a second source of truth.
It maps MRCC onto `packet.Telemetry`, so everything downstream — loss tracking,
telemetry.csv, the WebSocket, the dashboard — keeps consuming the one shape it
already knows. Fields with no home in that shape (pressure, heading, ground
speed, horizontal accel/velocity) are kept on the frame in `extra` rather than
being quietly dropped, so surfacing them later is a UI change, not a re-parse.

Self-test:  python3 -m shared.protocol.mrcc   (run from the repo root -- the
            relative `from . import packet` below needs the package context)
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from typing import Iterator

from . import packet

# --- field-name aliases ----------------------------------------------------
# The transmitter has now shipped THREE spellings of the same telemetry
# (`GPSFIX` -> `GF`, `STATE` -> `ST`, and an older revision with a vehicle id and
# a battery reading). Chasing each rename with a new parser is how a ground
# station ends up silently reading zeros: every unknown key still parses, it just
# lands in `extra` and the field it should have filled stays at its default.
#
# So names are canonicalised here, once, and the rest of the module only ever
# sees the long form. Short first, long second; unknown keys pass through
# untouched and end up in `extra`, which is where you look when a new rename
# appears.
FIELD_ALIASES: dict[str, str] = {
    "GD": "GPSDATA",    # GPS module producing data (NOT the same as having a fix)
    "GF": "GPSFIX",
    "GA": "GALT",
    "GS": "GSPEED",
    "CRS": "COURSE",
    "ST": "STATE",
    # `ALT` -> `AL` was the fourth rename, and the most expensive one yet: the
    # key is consumed into `baro_alt_m`, so an unaliased `AL` did not merely
    # land in `extra`, it left ALTITUDE READING 0 on a flying rocket while the
    # number sat in plain sight in the raw stream. Exactly the failure the note
    # above predicts, which is the argument for canonicalising here rather than
    # teaching each consumer both spellings.
    "AL": "ALT",
}

# --- what the transmitter calls each flight phase --------------------------
# The vehicle ships seven phases (Flight.cpp's stateName) and so does
# packet.FlightState, but they are not the same seven: MRCC has ARMED, the
# protocol additionally splits COAST out of ascent and MAIN out of descent.
# Those two splits are NOT inferable from an MRCC frame, so ASCENT and DESCENT
# map to the earlier member of each pair. The unmapped original string travels
# on the frame as `state_name`, so nothing here is lossy at the point of decode.
#
# ARMED was missing from this table for as long as it has existed, and the cost
# was not a missing label: an unmatched word falls back to PAD, so an armed
# vehicle reported itself as sitting safe on the pad while its pyro bus was
# live. backend/tests.py reads Flight.cpp's stateName() and fails if any word
# it can return is unmapped here.
#
# Matched by PREFIX (longest first), because the transmitter abbreviates the same
# phase differently between builds — ASCENT and ASC are one state, and a rocket
# that reads PAD all the way to the ground because a build shortened a word is a
# far worse failure than a state this table has to guess at.
STATE_PREFIXES: tuple[tuple[str, int], ...] = (
    ("PAD", packet.FlightState.PAD),
    ("ARM", packet.FlightState.ARMED),        # ARM / ARMED
    ("BOOST", packet.FlightState.BOOST),
    ("ASC", packet.FlightState.BOOST),        # ASC / ASCENT
    ("COAST", packet.FlightState.COAST),
    ("APO", packet.FlightState.APOGEE),       # APO / APOGEE
    ("DROGUE", packet.FlightState.DROGUE),
    ("DES", packet.FlightState.DROGUE),       # DES / DESC / DESCENT
    ("MAIN", packet.FlightState.MAIN),
    ("LAND", packet.FlightState.LANDED),      # LAND / LANDED
    ("LND", packet.FlightState.LANDED),
)


def state_to_flight_state(name: str) -> int | None:
    """MRCC's phase word -> packet.FlightState, or None if nothing matches.

    None is returned rather than defaulting to PAD: a caller that silently reads
    PAD for an unrecognised word would show a descending rocket as sitting on the
    pad. The caller counts it instead.
    """
    upper = name.upper()
    for prefix, state in STATE_PREFIXES:
        if upper.startswith(prefix):
            return state
    return None

# GPSFIX: the transmitter's convention is unconfirmed, so accept BOTH the common
# ones. 0 means no lock either way; 2 is only ever 2D; 3 is only ever 3D; a bare
# 1 is the "boolean fix" style and is reported as a 3D lock. Anything else is
# treated as no lock rather than passed through — gps_fix is a uint8 that the
# dashboard switches on, and an unknown value there renders as a phantom lock.
GPS_FIX_MAP: dict[int, int] = {
    0: packet.GpsFix.NONE,
    1: packet.GpsFix.FIX_3D,
    2: packet.GpsFix.FIX_2D,
    3: packet.GpsFix.FIX_3D,
}

# Keys consumed into Telemetry fields. Everything else in the line lands in
# `extra` — listing the consumed set here is what makes that split explicit.
#
# AX/AY/AZ are NOT listed: they feed `tilt_deg` but they also stay in `extra`,
# on purpose. Tilt is a lossy summary of them (an upright vehicle and a dead
# accelerometer both give 0°), so health_from_fields needs the raw axes to tell
# those two apart, and the horizontal pair has no protocol field at all.
_CONSUMED = frozenset({
    "PKT", "T", "GPSFIX", "SAT", "LAT", "LON", "GALT", "ALT", "VZ",
    "BAT", "STATE",
})

# --- the surplus fields, as a RECORD contract ------------------------------
# Everything above lands in `extra`, reaches the dashboard through
# backend/wire.py, and is rendered by AuxReadouts. None of it reached
# telemetry.csv, which had columns only for packet.CSV_COLUMNS -- so every one
# of these was shown live and then dropped from the post-flight record, which is
# the one place it is actually needed. A field you can watch but never review is
# barely instrumented at all.
#
# This is a RECORD roster, not a parser contract. An unlisted key still parses,
# still reaches `extra`, still reaches the screen, and is written to the
# `aux_extra` catch-all rather than being lost -- the same discipline
# FIELD_ALIASES follows, for the same reason: this transmitter renames things.
# Add a name here when it has earned a column of its own.
#
# Order mirrors the dashboard's Aux strip so the CSV reads like the screen --
# but only loosely, and ADD AT THE END. These names order AUX_COLUMNS, so
# inserting one in the middle shifts every column of every MRCC telemetry.csv
# already on disk, and a reader going by position would silently mis-read them.
#
# Only keys that actually reach `extra` belong here. VZ and GALT are absent on
# purpose: `_CONSUMED` already routes them to `vspeed_ms` and `gps_alt_m`, so a
# column for either would be empty in every row of every flight.
#
# The last row is the vehicle's own health and pyro reporting -- SD/BA/IM and
# AR/FI. Each is evidence for a bit the ground station derives (see
# health_from_fields / flags_from_fields), and they are kept here for the same
# reason GPSDATA is: the record should show what the vehicle SAID next to what
# this code concluded from it, so a wrong conclusion is reviewable afterwards.
AUX_FIELDS: tuple[str, ...] = (
    "P", "HDG", "COURSE", "GSPEED", "GPSDATA",
    "AX", "AY", "AZ", "VX", "VY",
    "GX", "GY", "GZ", "TEMP",
    "MX", "AR", "FI", "SD", "BA", "IM",
)

# `aux_`-prefixed and lowercased so an aux field can never collide with a
# protocol column, now or after some future transmitter adds an `ALT`-alike.
AUX_COLUMNS: tuple[str, ...] = tuple(f"aux_{name.lower()}" for name in AUX_FIELDS)

# Per-packet radio quality. The SX1278 reports these per frame and they were
# reaching /ws but not the record, so a post-flight range/link analysis had
# nothing to work from.
LINK_COLUMNS: tuple[str, ...] = ("rssi_dbm", "snr_db")

# Catch-all for keys AUX_FIELDS does not name, as `K=V;K=V`.
EXTRA_COLUMN = "aux_extra"

# telemetry.csv's columns for an MRCC session: the protocol contract FIRST and
# unchanged (the PLDR notebook and every existing reader keep working -- new
# columns only ever appear on the right), then what this format carries on top.
CSV_COLUMNS: list[str] = [
    *packet.CSV_COLUMNS, *LINK_COLUMNS, *AUX_COLUMNS, EXTRA_COLUMN,
]

# `len=N RSSI=-53 SNR=10.2 | ` — added by the ground receiver, not sent over the
# air. Optional: a receiver that prints the bare payload still parses.
_PREFIX_RE = re.compile(
    r"^\s*(?:len=(?P<len>\d+)\s+)?"
    r"(?:RSSI=(?P<rssi>-?\d+)\s+)?"
    r"(?:SNR=(?P<snr>-?\d+(?:\.\d+)?)\s*)?"
    r"(?:\|\s*)?"
)
_KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?\d+(?:\.\d+)?)")
# The flight phase, under either spelling. Anchored to a comma or the start of
# the body so `ST` cannot match the tail of some future key ending in "st".
_STATE_RE = re.compile(r"(?:^|,)\s*(?:STATE|ST)\s*=\s*([A-Za-z_]+)")

MARKER = "MRCC"

# Keys the RECEIVER prints in its prefix. They must never appear inside the
# payload; if they do, a second line's prefix was spliced into this one.
_PREFIX_KEYS = frozenset({"LEN", "RSSI", "SNR"})


@dataclass
class LinkQuality:
    """Per-packet radio metrics. The SX1278 reports these; the old E32 could not,
    which is why `loss.py` still describes seq gaps as the only link signal — for
    an MRCC frame they are no longer the only one."""
    rssi_dbm: int | None = None
    snr_db: float | None = None
    payload_len: int | None = None


@dataclass
class MrccFrame:
    telemetry: packet.Telemetry
    link: LinkQuality = field(default_factory=LinkQuality)
    state_name: str = ""            # the transmitter's own word, before mapping
    extra: dict[str, float] = field(default_factory=dict)
    has_battery: bool = False       # False -> telemetry.vbat_dv is a placeholder, not a reading
    # False -> `state_name` matched nothing in STATE_PREFIXES and flight_state
    # fell back to PAD. The parser counts these; a silent fallback here would
    # show a descending rocket as still on the pad.
    state_recognised: bool = True


def _clamp(value: float, lo: int, hi: int) -> int:
    """Round to int and pin into a protocol field's range.

    Saturating rather than wrapping is the deliberate choice: an altitude past
    int16 should read as a pegged 32767, not fold around to a negative.
    """
    return max(lo, min(hi, int(round(value))))


def tilt_from_accel(ax: float, ay: float, az: float) -> int | None:
    """Angle between the measured acceleration vector and vertical, degrees.

    MRCC has no tilt field but does carry body-frame accelerometer axes. At rest
    that vector is gravity, so its angle off the Z axis is the vehicle's tilt —
    which is exactly what `tilt_deg` means on the pad and in slow flight. Under
    thrust it is thrust+gravity and reads low; that is a property of deriving
    tilt from an accelerometer alone, not of this arithmetic.

    Returns None for a zero-length vector (all three axes reading exactly 0.0 is
    a dead IMU, not a vehicle in free fall) so the caller can leave tilt unset.
    """
    norm = math.sqrt(ax * ax + ay * ay + az * az)
    if norm <= 0.0:
        return None
    return _clamp(math.degrees(math.acos(max(-1.0, min(1.0, az / norm)))), 0, 180)


def decode_line(line: str) -> MrccFrame | None:
    """One text line -> MrccFrame, or None if it isn't an MRCC telemetry line.

    None covers both "not ours" (the receiver's own `alive 32046` chatter) and
    "ours but unusable" (a line chopped in half by a serial-buffer overrun). The
    caller distinguishes them; see MrccParser.
    """
    marker = line.find(MARKER)
    if marker < 0:
        return None

    link = LinkQuality()
    prefix = _PREFIX_RE.match(line[:marker])
    if prefix is not None:
        if prefix.group("len"):
            link.payload_len = int(prefix.group("len"))
        if prefix.group("rssi"):
            link.rssi_dbm = int(prefix.group("rssi"))
        if prefix.group("snr"):
            link.snr_db = float(prefix.group("snr"))

    body = line[marker + len(MARKER):]

    # --- integrity, in the absence of a payload CRC ---------------------------
    # The receiver prints the payload length it actually got. Measured over a
    # real session that matched the payload exactly 1014 times and disagreed
    # only on the 8 damaged lines, which makes it this format's closest thing to
    # a CRC. It is worth using, because the failure it catches is the dangerous
    # kind: the ground station's own receiver races its print buffer (an ISR
    # overwrites it mid-printf) and emits SPLICED lines like
    #   len=163 ... | MRCClen=163 ... | MRCC,PKT=...
    # Those still parse. Without this check they decode into a confident-looking
    # frame carrying another packet's fields — 255 satellites, negative
    # pressure, and BARO/IMU reported LOST off garbage. A frame the operator
    # cannot tell is wrong is far worse than a frame that never arrives.
    if link.payload_len is not None and len(line) - marker != link.payload_len:
        return None

    # Belt and braces for a receiver that doesn't print `len=`: a well-formed
    # payload contains the marker exactly once and never carries the receiver's
    # own prefix keys, so either is proof that two frames got spliced.
    if MARKER in body:
        return None

    fields = {FIELD_ALIASES.get(k.upper(), k.upper()): float(v)
              for k, v in _KV_RE.findall(body)}
    if fields.keys() & _PREFIX_KEYS:
        return None
    if "PKT" not in fields:
        return None  # truncated before the sequence number — unusable

    # STATE is the one non-numeric value, so it is read separately. Anchored to a
    # field boundary so a short key like ST cannot match the tail of another name.
    state_match = _STATE_RE.search(body)
    state_name = state_match.group(1).upper() if state_match else ""

    t = packet.Telemetry()
    # PKT counts up without bound; seq is a uint16. Masking (rather than
    # clamping) is what LossTracker's wrap-safe delta already expects.
    t.seq = int(fields["PKT"]) & 0xFFFF
    t.onboard_ms = max(0, int(fields.get("T", 0.0) * 1000))
    state = state_to_flight_state(state_name)
    t.flight_state = packet.FlightState.PAD if state is None else state

    t.baro_alt_m = _clamp(fields.get("ALT", 0.0), -32768, 32767)
    t.vspeed_dms = _clamp(fields.get("VZ", 0.0) * 10, -32768, 32767)

    t.gps_lat = _clamp(fields.get("LAT", 0.0) * 1e7, -2**31, 2**31 - 1)
    t.gps_lon = _clamp(fields.get("LON", 0.0) * 1e7, -2**31, 2**31 - 1)
    t.gps_alt_m = _clamp(fields.get("GALT", 0.0), -32768, 32767)
    t.gps_sats = _clamp(fields.get("SAT", 0.0), 0, 255)
    t.gps_fix = GPS_FIX_MAP.get(int(fields.get("GPSFIX", 0.0)), packet.GpsFix.NONE)

    tilt = tilt_from_accel(fields.get("AX", 0.0), fields.get("AY", 0.0), fields.get("AZ", 0.0))
    if tilt is not None:
        t.tilt_deg = tilt

    # Only the older transmitter revision sends BAT. `has_battery` is how the
    # caller tells "0.0 V because that is the reading" from "0.0 V because there
    # was no reading" — the second must never reach an operator as a flat battery.
    has_battery = "BAT" in fields
    if has_battery:
        t.vbat_dv = _clamp(fields["BAT"] * 10, 0, 255)

    # health stays 0 here and is NOT inferred: the caller decides what to claim,
    # because "which peripherals are alive" is a policy question (see
    # health_from_fields) and this function's job is only to read the line.
    extra = {k: v for k, v in fields.items() if k not in _CONSUMED}
    return MrccFrame(telemetry=t, link=link, state_name=state_name,
                     extra=extra, has_battery=has_battery,
                     state_recognised=state is not None)


def health_from_fields(frame: MrccFrame) -> tuple[int, int]:
    """Infer (health, health_known) bitmasks from what an MRCC frame reveals.

    MRCC carries no health byte, and the two easy answers are both wrong: claim
    0x3F and six dead peripherals look nominal; claim 0x00 and a working vehicle
    raises six alarms. So each bit is reported only when the frame contains
    evidence either way, and `health_known` marks which bits that is.

    Two of these the vehicle now STATES outright (BA, IM) and the rest are still
    read off the data. A stated bit always wins: it is the flight computer's own
    `baroOK` / `imuOK`, and every inference below is a proxy for exactly that.

      BARO  <- BA, the vehicle's own baroOK. Falls back to P (pressure): a
               barometer that is not answering cannot produce a plausible
               pressure, and 0 means no reading, not a vacuum.
      IMU   <- IM, the vehicle's own imuOK. Falls back to AX/AY/AZ, all three at
               exactly 0.0 being a dead sensor -- a real accelerometer at rest
               still reads ~9.81 on one axis.

               That fallback is WEAK and the IM bit is why it is now only a
               fallback. Firmware before 2026-09-04 skipped the sensor read
               entirely while the IMU was down, so ax/ay/az held their LAST GOOD
               VALUES for the rest of the flight -- never zero, so this test
               reported IMU OK straight through the failure. Only an IMU that
               died before the first ever read downlinks true zeros. On a log
               from that firmware the IMU row is a guess; read it as one.
      GPS   <- GPSDATA, which is the module talking, NOT GPSFIX, which is the
               module having a lock. No lock indoors is normal; no data is not.
      SD    <- SD, which the transmitter added after the 2026-08-19 revision:
               1 = card present and mounted, 0 = not. Note this sets HEALTH_SD
               (mounted) and NOT flag SD_OK (writes currently succeeding) --
               PROTOCOL.md keeps those separate precisely because a mounted card
               whose writes fail is 1 + 0, and this downlink says nothing about
               write success. Inferring the flag from the bit would invent the
               half of the story the vehicle did not tell us.
      PYRO / VBAT: nothing in the frame speaks to these. Left unknown.

    LORA is absent by design, not by oversight. The transmitter only builds a
    packet when its radio is up, so a downlinked "radio OK" could never read
    anything but 1. Link health is the arrival of frames at all, which is
    loss.py's job, not this function's.
    """
    health = 0
    known = 0

    if "BA" in frame.extra:
        known |= packet.HEALTH_BARO
        if frame.extra["BA"] > 0:
            health |= packet.HEALTH_BARO
    elif "P" in frame.extra:
        known |= packet.HEALTH_BARO
        if frame.extra["P"] > 0:
            health |= packet.HEALTH_BARO

    if "IM" in frame.extra:
        known |= packet.HEALTH_IMU
        if frame.extra["IM"] > 0:
            health |= packet.HEALTH_IMU
    else:
        axes = [frame.extra[k] for k in ("AX", "AY", "AZ") if k in frame.extra]
        if axes:
            known |= packet.HEALTH_IMU
            # Read the axes, not the derived tilt: an upright vehicle and a dead
            # accelerometer both give tilt 0°, and only one of them is a fault.
            if any(a != 0.0 for a in axes):
                health |= packet.HEALTH_IMU

    if "GPSDATA" in frame.extra:
        known |= packet.HEALTH_GPS
        if frame.extra["GPSDATA"] > 0:
            health |= packet.HEALTH_GPS

    if "SD" in frame.extra:
        known |= packet.HEALTH_SD
        if frame.extra["SD"] > 0:
            health |= packet.HEALTH_SD

    if frame.has_battery:
        known |= packet.HEALTH_VBAT
        if frame.telemetry.vbat_dv > 0:
            health |= packet.HEALTH_VBAT

    return health, known


def flags_from_fields(frame: MrccFrame) -> tuple[int, int]:
    """Infer (flags, flags_known) bitmasks from an MRCC frame — the flags twin
    of `health_from_fields`, and reported the same way: a bit only when the frame
    is evidence either way, with `flags_known` marking which.

    This used to return nothing at all, and the comment explaining why said the
    downlink "carries no flags". That stopped being true: the transmitter sends
    AR and FI, and the ground station was dropping both into `extra` and then
    rendering ARMED and PYRO as unknown — with a live `FI=1` sitting in the raw
    stream. PYRO FIRED is the single event this console exists to show.

      ARMED      <- AR, the vehicle's own pyroArmed.
      PYRO_FIRED <- FI, the vehicle's own pyroFired. Latched in RTC memory on the
                    vehicle, so it survives a brownout reset and stays 1.
      CONTINUITY: not downlinked. The vehicle prints it on the console but there
                  is no room for it in the packet, so it stays unknown here
                  rather than reading as an open e-match.
      SD_OK:      deliberately NOT taken from the SD field. That field is
                  HEALTH_SD ("card mounted"); this flag is "writes are currently
                  succeeding", and a mounted card whose writes fail is 1 + 0.
                  See health_from_fields for the same distinction from the other
                  side.
    """
    flags = 0
    known = 0

    for name, bit in (("AR", packet.FLAG_ARMED), ("FI", packet.FLAG_PYRO_FIRED)):
        if name in frame.extra:
            known |= bit
            if frame.extra[name] > 0:
                flags |= bit

    return flags, known


def aux_csv_row(frame: MrccFrame) -> dict[str, object]:
    """The aux/link half of one telemetry.csv row -- see CSV_COLUMNS.

    A field the frame did not carry is written EMPTY, never 0. That is the same
    rule the health bits and the flags already follow, and for the same reason:
    this file's whole argument is that "nobody reported it" and "it read zero"
    are different facts, and a record that conflates them invents readings.
    A 0 in `aux_p` would be a barometer reporting vacuum.

    Unnamed keys are folded into `aux_extra` rather than dropped, so the next
    time the transmitter renames a field the data is still in the record --
    findable, if not yet in a column of its own.
    """
    row: dict[str, object] = {
        "rssi_dbm": "" if frame.link.rssi_dbm is None else frame.link.rssi_dbm,
        "snr_db": "" if frame.link.snr_db is None else frame.link.snr_db,
    }
    for name, col in zip(AUX_FIELDS, AUX_COLUMNS):
        row[col] = frame.extra.get(name, "")
    rest = sorted(k for k in frame.extra if k not in AUX_FIELDS)
    row[EXTRA_COLUMN] = ";".join(f"{k}={frame.extra[k]:g}" for k in rest)
    return row


class MrccParser:
    """Line framer for the MRCC text stream — the text-mode twin of
    `packet.PacketParser`, with the same contract: feed arbitrary byte chunks in
    arrival order, get decoded `Telemetry` out, and log the bytes to raw.log
    BEFORE they reach here.

    Counters mirror the binary parser's `crc_errors` so /stats reads the same
    either way:
      parse_errors — lines that named MRCC but could not be decoded (truncated
                     by a receiver-side buffer overrun, or corrupted in the air).
                     This is the text link's equivalent of a CRC failure and it
                     is the number that separates "RF arriving but mangled" from
                     "no RF at all".
      ignored      — lines with no MRCC marker. The receiver sketch prints its
                     own status chatter; that is not an error and must not
                     inflate the error count.
    """

    # A line longer than this is not a telemetry line, it is a stream with no
    # newlines in it (wrong baud, binary firmware on the other end). Cap the
    # buffer so that case leaks memory instead of nothing being noticed.
    MAX_LINE = 8192

    def __init__(self) -> None:
        self._buf = bytearray()
        self.parse_errors = 0
        self.ignored = 0
        self.lines = 0
        self.last_link = LinkQuality()
        # Phase words this build has never seen. Non-zero means the transmitter
        # renamed a state and the ground station is reading PAD for it — the kind
        # of drift that is invisible until someone reads the flight back.
        self.unknown_states: set[str] = set()

    # Named crc_errors as well so callers written against PacketParser keep
    # working; for a text link the concept is "a frame arrived and was garbage".
    @property
    def crc_errors(self) -> int:
        return self.parse_errors

    def feed(self, chunk: bytes) -> Iterator[MrccFrame]:
        self._buf.extend(chunk)
        while True:
            nl = self._buf.find(b"\n")
            if nl < 0:
                if len(self._buf) > self.MAX_LINE:
                    # No newline in a full line's worth of bytes: drop it and
                    # count it, rather than buffering the stream forever.
                    del self._buf[:]
                    self.parse_errors += 1
                return
            raw = bytes(self._buf[:nl])
            del self._buf[:nl + 1]

            # errors="replace" keeps a byte-corrupted line as a line: it then
            # fails field parsing and is counted, instead of raising here.
            line = raw.decode("ascii", errors="replace").strip("\r\0 ")
            if not line:
                continue
            self.lines += 1

            if MARKER not in line:
                self.ignored += 1
                continue

            frame = decode_line(line)
            if frame is None:
                self.parse_errors += 1
                continue
            if not frame.state_recognised and frame.state_name:
                self.unknown_states.add(frame.state_name)
            self.last_link = frame.link
            yield frame


if __name__ == "__main__":
    # Captured verbatim off /dev/cu.usbserial-0001 on 2026-08-19.
    CURRENT = ("len=191 RSSI=-53 SNR=10.2 | MRCC,PKT=207,T=207.5,GPSDATA=1,GPSFIX=0,"
               "SAT=0,LAT=0.000000,LON=0.000000,GALT=0.0,GSPEED=0.00,COURSE=0.0,"
               "AX=0.00,AY=0.00,AZ=9.81,VX=0.00,VY=0.00,VZ=0.00,ALT=0.0,HDG=103.5,"
               "P=101325,STATE=LANDED")
    OLDER = ("len=81 RSSI=-47 | MRCC,RKT01,PKT=16,T=9.0,ALT=180.0,VZ=40.0,P=99255,"
             "TEMP=29.6,BAT=4.15,STATE=ASCENT")

    f = decode_line(CURRENT)
    assert f is not None
    assert f.telemetry.seq == 207, f.telemetry.seq
    assert f.telemetry.onboard_ms == 207500, f.telemetry.onboard_ms
    assert f.telemetry.flight_state == packet.FlightState.LANDED
    assert f.state_name == "LANDED"
    assert f.link.rssi_dbm == -53 and f.link.snr_db == 10.2 and f.link.payload_len == 191
    assert f.telemetry.gps_fix == packet.GpsFix.NONE
    assert f.telemetry.tilt_deg == 0, f.telemetry.tilt_deg   # AZ=9.81 -> upright
    assert not f.has_battery and f.telemetry.vbat_dv == 0
    assert f.extra["P"] == 101325 and f.extra["HDG"] == 103.5

    # Older revision: vehicle id, battery, no GPS block. Still decodes.
    g = decode_line(OLDER)
    assert g is not None
    assert g.telemetry.seq == 16 and g.telemetry.baro_alt_m == 180
    assert g.telemetry.vspeed_dms == 400, g.telemetry.vspeed_dms   # 40.0 m/s
    assert g.telemetry.flight_state == packet.FlightState.BOOST    # ASCENT
    assert g.has_battery and g.telemetry.vbat_dv == 42             # 4.15 V -> 41.5 -> 42
    assert g.link.snr_db is None                                   # prefix had no SNR

    # Tilt: gravity 45 deg off the vehicle's Z axis.
    assert tilt_from_accel(6.94, 0.0, 6.94) == 45
    assert tilt_from_accel(0.0, 0.0, 0.0) is None                  # dead IMU, not free fall

    # Health inference names what it can and stays quiet about the rest.
    health, known = health_from_fields(f)
    assert known & packet.HEALTH_BARO and health & packet.HEALTH_BARO
    assert known & packet.HEALTH_GPS and health & packet.HEALTH_GPS   # GPSDATA=1, no fix
    # Upright on the pad: tilt is 0 deg but AZ=9.81 proves the IMU is alive. The
    # regression this guards is reading tilt instead of the axes and calling a
    # working accelerometer unknown.
    assert known & packet.HEALTH_IMU and health & packet.HEALTH_IMU
    assert not known & packet.HEALTH_SD and not known & packet.HEALTH_PYRO
    assert not known & packet.HEALTH_VBAT                             # no BAT in this revision

    # Genuinely dead IMU: every axis exactly 0.0 -> known, and known-bad.
    dead = decode_line(CURRENT.replace("AZ=9.81", "AZ=0.00"))
    assert dead is not None
    dead_health, dead_known = health_from_fields(dead)
    assert dead_known & packet.HEALTH_IMU and not dead_health & packet.HEALTH_IMU

    # The SHORT field names the transmitter switched to on 2026-08-19. Captured
    # verbatim. Same telemetry, renamed keys — this must decode identically, not
    # quietly read zeros for gps alt and sit at PAD through the whole flight.
    SHORT = ("len=152 RSSI=-50 SNR=10.2 | MRCC,PKT=8,T=5.1,GD=1,GF=0,SAT=0,LAT=0.00000,"
             "LON=0.00000,GA=0.0,GS=0.0,CRS=0,AX=0.2,AY=-0.1,AZ=18.0,VX=1.2,VY=0.8,"
             "VZ=40.0,ALT=20.0,HDG=4,P=101095,ST=ASC")
    h = decode_line(SHORT)
    assert h is not None
    assert h.telemetry.seq == 8 and h.telemetry.onboard_ms == 5100
    assert h.state_name == "ASC" and h.state_recognised
    assert h.telemetry.flight_state == packet.FlightState.BOOST      # ASC == ASCENT
    assert h.telemetry.baro_alt_m == 20 and h.telemetry.vspeed_dms == 400
    # Aliases are canonicalised, so downstream sees the long names only.
    assert "GPSDATA" in h.extra and "GD" not in h.extra
    hh, hk = health_from_fields(h)
    assert hk & packet.HEALTH_GPS and hh & packet.HEALTH_GPS         # GD=1
    assert hk & packet.HEALTH_IMU and hh & packet.HEALTH_IMU         # AZ=18.0 under thrust

    # Both spellings of every phase land on the same FlightState.
    for short, long in (("ASC", "ASCENT"), ("APO", "APOGEE"), ("DES", "DESCENT"),
                        ("LAND", "LANDED"), ("PAD", "PAD")):
        assert state_to_flight_state(short) == state_to_flight_state(long), short
    # An unrecognised word is reported, never silently read as PAD.
    assert state_to_flight_state("TUMBLING") is None

    p3 = MrccParser()
    list(p3.feed(b"MRCC,PKT=1,AZ=9.8,ST=TUMBLING\n"))
    assert p3.unknown_states == {"TUMBLING"}, p3.unknown_states

    # --- spliced lines must be REJECTED, not decoded --------------------------
    # Both captured verbatim from raw.log on 2026-08-19: the receiver's ISR
    # overwrote its print buffer mid-printf. Decoding these produced 255
    # satellites, -9804 Pa, and two peripherals falsely reported LOST.
    SPLICED_MARKER = ("len=163 RSSI=-47 SNR=12.5 | MRCClen=163 RSSI=-47 SNR=12.5 | "
                      "MRCC,PKT=255,T=1.0,SAT=255,AZ=0.0,P=-9804,ST=PAD")
    SPLICED_LEN = ("len=160 RSSI=-45 SNR=10.5 | MRCC,PKT=144,T=74.2,GD=1,GF=1,SAT=11,"
                   "AZ=9.8,VZ=-8.0,ALT=0.4,P=100767,ST=DES")   # body far shorter than 160
    for label, bad in (("marker", SPLICED_MARKER), ("length", SPLICED_LEN)):
        assert decode_line(bad) is None, label

    p4 = MrccParser()
    assert list(p4.feed((SPLICED_MARKER + "\n").encode())) == []
    assert p4.parse_errors == 1 and p4.ignored == 0   # corrupt, not "not ours"

    # A clean line whose declared length matches must still decode.
    body = "MRCC,PKT=7,T=3.5,GD=1,GF=1,SAT=9,AZ=9.8,VZ=0.0,ALT=0.0,P=101325,ST=PAD"
    ok = decode_line(f"len={len(body)} RSSI=-50 SNR=10.0 | {body}")
    assert ok is not None and ok.telemetry.seq == 7
    # ...and the same line with a wrong declared length must not.
    assert decode_line(f"len={len(body) + 3} RSSI=-50 SNR=10.0 | {body}") is None

    # Streaming: split mid-line, interleaved with the receiver's own chatter.
    p = MrccParser()
    stream = (CURRENT + "\nalive 32046\n" + OLDER + "\nMRCC,PKT=\n").encode()
    got = []
    for i in range(0, len(stream), 7):                 # ragged chunks, like serial reads
        got.extend(p.feed(stream[i:i + 7]))
    assert len(got) == 2, len(got)
    assert p.ignored == 1, p.ignored                   # 'alive 32046' is chatter, not an error
    assert p.parse_errors == 1, p.parse_errors         # the truncated MRCC line is
    assert [x.telemetry.seq for x in got] == [207, 16]

    print("ok  mrcc self-test")
