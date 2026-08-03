# Telemetry Packet Protocol

> Authoritative wire spec for the rocket ⇄ ground-station link.
> `packet.py` is the source of truth; this document mirrors it in human-readable form.
> **Firmware must match this byte-for-byte.** Little-endian throughout.

## Framing

Every packet is a fixed **32 bytes**:

```
+--------+--------------------+-----------+
| SYNC   | BODY               | CRC16     |
| 2 B    | 28 B               | 2 B (LE)  |
| AA 55  | (fields below)     |           |
+--------+--------------------+-----------+
  0    1  2 ................ 29  30     31
```

- **SYNC** = the two literal bytes `0xAA 0x55` (that order on the wire). Marks packet boundaries in a byte stream.
- **BODY** = 28 bytes of fields (table below).
- **CRC16** = CRC-16/CCITT-FALSE over the 28 BODY bytes only (offsets 2..29), stored little-endian.

Rate: 4 Hz nominal (~128 B/s). E32 air-rate 2.4 kbps, 9600 baud UART.

## Field table (BODY — offsets are relative to frame start)

| Off | Size | Field        | Type | Units / scaling                                    |
|----:|-----:|--------------|------|----------------------------------------------------|
| 2   | 1    | msg_type     | u8   | 0 = telemetry (1/2 reserved: phase-2 command/ack)  |
| 3   | 2    | seq          | u16  | +1 per packet, wraps at 65535 → drives loss rate   |
| 5   | 1    | flight_state | u8   | enum (below)                                       |
| 6   | 4    | onboard_ms   | u32  | ms since flight-computer boot                      |
| 10  | 2    | baro_alt_m   | i16  | barometric altitude, m AGL                         |
| 12  | 2    | vspeed_dms   | i16  | vertical speed, dm/s (÷10 = m/s)                   |
| 14  | 4    | gps_lat      | i32  | latitude, degrees × 1e7                            |
| 18  | 4    | gps_lon      | i32  | longitude, degrees × 1e7                           |
| 22  | 2    | gps_alt_m    | i16  | GPS altitude, m MSL                                |
| 24  | 1    | gps_sats     | u8   | satellites used                                    |
| 25  | 1    | gps_fix      | u8   | 0 none / 2 = 2D / 3 = 3D                            |
| 26  | 1    | tilt_deg     | u8   | tilt from vertical, 0..180°                        |
| 27  | 1    | vbat_dv      | u8   | battery volts × 10 (÷10 = V)                        |
| 28  | 1    | flags        | u8   | bitfield (below)                                   |
| 29  | 1    | health       | u8   | per-peripheral health bitfield (below)             |
| 30  | 2    | crc16        | u16  | CRC-16/CCITT-FALSE over bytes 2..29                 |

Python `struct` format (little-endian, no padding): BODY = `<BHBIhhiihBBBBBB` (28 bytes).

## Enums

**flight_state**

| Value | Name   | Meaning                  |
|------:|--------|--------------------------|
| 0     | PAD    | on the pad, pre-launch   |
| 1     | BOOST  | motor burning            |
| 2     | COAST  | unpowered ascent         |
| 3     | APOGEE | peak altitude            |
| 4     | DROGUE | descent under drogue     |
| 5     | MAIN   | descent under main chute |
| 6     | LANDED | on the ground            |

**gps_fix**: 0 = no fix, 2 = 2D, 3 = 3D.

## flags (u8 bitfield)

| Bit | Mask | Name       | Meaning                    |
|----:|-----:|------------|----------------------------|
| 0   | 0x01 | CONTINUITY | e-match continuity OK      |
| 1   | 0x02 | PYRO_FIRED | a pyro channel has fired   |
| 2   | 0x04 | SD_OK      | onboard SD logging healthy |
| 3   | 0x08 | ARMED      | flight computer armed      |
| 4–7 | —    | reserved   | 0                          |

## health (u8 bitfield)

Bit **set** = that peripheral initialised and is currently responding.
Bit **clear** = it is not, and every field it feeds is untrustworthy.

| Bit | Mask | Name | Feeds                    |
|----:|-----:|------|--------------------------|
| 0   | 0x01 | BARO | `baro_alt_m`, `vspeed_dms` |
| 1   | 0x02 | IMU  | `tilt_deg`               |
| 2   | 0x04 | GPS  | `gps_*`                  |
| 3   | 0x08 | SD   | onboard logging          |
| 4   | 0x10 | PYRO | continuity sense         |
| 5   | 0x20 | VBAT | `vbat_dv`                |
| 6–7 | —    | spare | 0                       |

`0x3F` = all nominal.

**Why this byte exists.** A peripheral failure must degrade *one row* on the
ground station, never the whole vehicle. The flight computer does not abort boot
when a sensor's init fails and does not stall its loop when one dies in flight —
it clears the bit, keeps transmitting at 4 Hz, and lets the operator see exactly
what is down. Enforced by `firmware/lib/Subsystem`, proven by
`firmware/test/subsystem_check.cpp`.

**Init failure vs in-flight failure** is read off the *first* frame of a session:
a bit clear from the very first packet never came up at all; a bit that goes
1 → 0 later died in flight. Both are recorded in the Log view.

**Distinct from `FLAG_SD_OK`.** `HEALTH_SD` = card present and mounted;
`FLAG_SD_OK` = writes are currently succeeding. A mounted card with failing
writes is `1` + `0`.

**A `health` of `0x00` means "no peripherals up", not "field absent."** Decoders
reading pre-health logs should treat a missing byte as *unknown* and render it
as such — six red alarms for an old capture would be a false alarm.

## CRC-16/CCITT-FALSE

Polynomial `0x1021`, init `0xFFFF`, **no** input/output reflection, **no** final XOR.
Computed over the 28 BODY bytes (offsets 2..29); stored little-endian at 30..31.

C reference (matches `packet.crc16_ccitt` exactly):

```c
uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
```

## C struct (firmware)

ESP32 is little-endian, so a packed struct maps straight onto the wire BODY:

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  msg_type;      // 0 = telemetry
    uint16_t seq;
    uint8_t  flight_state;
    uint32_t onboard_ms;
    int16_t  baro_alt_m;
    int16_t  vspeed_dms;    // dm/s
    int32_t  gps_lat;       // deg * 1e7
    int32_t  gps_lon;       // deg * 1e7
    int16_t  gps_alt_m;
    uint8_t  gps_sats;
    uint8_t  gps_fix;
    uint8_t  tilt_deg;
    uint8_t  vbat_dv;       // V * 10
    uint8_t  flags;
    uint8_t  health;        // HEALTH_* bitfield
} body_t;                   // sizeof == 28
#pragma pack(pop)

// wire = { 0xAA, 0x55 } ++ body_t ++ crc16_ccitt((uint8_t*)&body, 28)   // CRC little-endian
```

TX discipline: wait for the E32 **AUX pin HIGH** before every UART write (GROUND_STATION_PLAN.md §3) — never blind-write.

## CSV contract

Backend `telemetry.csv` and the PLDR notebook share one column order (`packet.CSV_COLUMNS`):

```
host_time, gps_time, onboard_ms, seq, flight_state, baro_alt_m, vspeed_ms,
gps_lat, gps_lon, gps_alt_m, gps_sats, gps_fix, tilt_deg, vbat_v,
continuity, pyro_fired, sd_ok, armed,
hw_baro, hw_imu, hw_gps, hw_sd, hw_pyro, hw_vbat
```

The `hw_*` columns are the decoded `health` bits (1 = peripheral OK), so a
post-flight analysis can tell "the altitude went flat because the baro died at
T+14" from "the rocket stopped climbing".

`host_time` = laptop receive time; `gps_time` = from GPS if present. Values are decoded engineering
units (m/s, volts, decimal degrees); booleans are 0/1. `packet.Telemetry.to_csv_row(host_time, gps_time)`
returns exactly this dict.

## SD-card log

The onboard SD log should use the **same `body_t`** (a superset with extra high-rate fields is fine),
so one Python parser handles both SD and GS data. Downlink is a 4 Hz decimated view; the SD card is the
full-rate master record.

## Using the codec (Python, from repo root)

```python
from shared.protocol import packet

frame = packet.encode(t)      # Telemetry -> 32 bytes
t = packet.decode(frame)      # 32 bytes -> Telemetry | None (checks sync + CRC)

p = packet.PacketParser()     # streaming framer for serial chunks
for t in p.feed(chunk):       # yields decoded Telemetry, resyncs past noise
    ...
print(p.crc_errors)           # count of CRC failures seen
```
