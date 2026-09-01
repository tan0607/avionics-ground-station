# Firmware

Two Arduino sketches, two boards, one radio link.

| Sketch | Board | FQBN | Job |
|---|---|---|---|
| `MRCC_FlightComputer/` | ESP32-**S3** | `esp32:esp32:esp32s3` | fly the rocket: sensors, filters, flight state, pyro, SD log, and a 2 Hz MRCC downlink |
| `MRCC_GroundStation/` | classic **ESP32** | `esp32:esp32:esp32` | receive that downlink and print it to USB for the backend |
| `SD_Doctor/` | ESP32-**S3** | `esp32:esp32:esp32s3` | bench-only SD card fault finder — no radio, no sensors. Flash it when the card won't mount, then drive it from the serial monitor |

Plus `tools/` — host-side, never compiled into the flight build. See
[Filter figures](#filter-figures-tools).

The ground station prints one line per frame:

```
len=231 RSSI=-53 SNR=10.2 | MRCC,PKT=207,T=207.5,ST=LANDED,AL=0.0,...
```

That exact shape is the contract with the laptop — `shared/protocol/mrcc.py`
parses it, and the `len=` field is what lets the backend tell a truncated frame
from a clean one. Do not reorder or rename the prefix.

---

## Flashing: the one thing you must not get wrong

**Every flash names a vehicle.** Both sketches carry the same block near the top:

```c
#define VEHICLE  VEHICLE_A          // <<<< CHANGE ME PER ROCKET
```

`VEHICLE_A` → 433.3 MHz  ·  `VEHICLE_B` → 434.1 MHz

Four boards, two settings, and they pair up:

| Board | Sketch | Set to |
|---|---|---|
| Rocket A (S3) | `MRCC_FlightComputer` | `VEHICLE_A` |
| Ground station A (ESP32) | `MRCC_GroundStation` | `VEHICLE_A` |
| Rocket B (S3) | `MRCC_FlightComputer` | `VEHICLE_B` |
| Ground station B (ESP32) | `MRCC_GroundStation` | `VEHICLE_B` |

*One rocket, one letter, both its boards.*

### Why two rockets cannot share a channel

LoRa does not pair. A receiver decodes **every** packet whose
frequency / SF / BW / CR / syncword match, no matter which airframe sent it. On
one channel you get both failures at once:

- each ground station decodes the **other** rocket's frames — `PKT` jumps, the
  loss count turns to noise, and the map hops between airframes; and
- the two transmitters **collide on air**, so neither link survives.

The second one is decisive. One telemetry cycle is two copies of a ~231-byte
packet at SF7 / BW 250 kHz / CR 4-5: ~182 ms of air each, plus the 60 ms
`COPY_GAP`, inside a 500 ms `SEND_INTERVAL`. **One rocket alone already radiates
~73% of the time** and its transmit sequence occupies ~85% of every window.
There is no room to share, and no setting short of a different frequency makes
room. Check the `air=` figure the flight computer prints against that 182 ms.

Both channels sit inside Malaysia's 433 MHz ISM allocation (MCMC:
433.05 – 434.79 MHz) and are 800 kHz apart — comfortably wider than the 250 kHz
occupied bandwidth, so the two links do not overlap even at the skirts.

### A wrong flash is invisible until the pad

A board flashed for the wrong vehicle boots fine, transmits fine, and says
"ready". Nothing looks wrong until the other airframe powers on. There is also
no partial-reception failure mode to warn you: a mismatched channel gives **zero
packets, forever**, not weak or garbled ones.

So both sketches print their channel at boot. **Read it before you close the
airframe:**

```
[LORA] SUCCESS - vehicle A @ 433.300 MHz  (ground station must match)
RX ready - vehicle A @ 433.300 MHz  (rocket must match)
```

A typo in `VEHICLE` is a compile error, not a silent fallback.

---

## Radio parameters

Both ends must agree on all five. A mismatch in **any** of them fails the same
silent way a wrong channel does.

| | Value | Rocket | Ground station |
|---|---|---|---|
| Frequency | per vehicle (above) | `src/Config.h` | `.ino` |
| Spreading factor | 7 | `src/Radio.cpp` | `.ino` |
| Bandwidth | 250 kHz | `src/Radio.cpp` | `.ino` |
| Coding rate | 4/5 | `src/Radio.cpp` | `.ino` |
| Preamble | 8 | `src/Radio.cpp` | `.ino` |
| Sync word | `0x12` | `src/Radio.cpp` | `.ino` |
| Hardware CRC | enabled | `src/Radio.cpp` | `.ino` |

`0x12` is also the SX1278's reset default, so the link worked before the ground
station set it explicitly. That was luck, not design — it is set on both ends now.

---

## Build

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer
```

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/MRCC_GroundStation
```

Add `--upload -p <port>` to flash. Find the port with `arduino-cli board list`.

Libraries: `LoRa` (sandeepmistry), plus the flight computer's sensor stack
(ICM-20948, BMP280, TinyGPS++). Install once with `arduino-cli lib install`.

---

## Filter figures (`tools/`)

Host-side only. `tools/replay.cpp` **links `MRCC_FlightComputer/src/Filters.cpp`
directly**, so the report's before/after graphs are produced by the same C++ that
flies — change a cutoff in `Config.h`, re-run, and the figures move with the
firmware. There is no second implementation to drift.

```bash
cd firmware/tools && ./make_report_figures.sh
```

That synthesises a flight with known true attitude, runs it through the flight
filter code, and writes `figures/`. Pass a `FLIGHTnnn.CSV` off the SD card to
plot a real one instead. `tools/README.md` has the figure list and the one real
caveat (the card logs at 10 Hz, so the spectrum figure is only meaningful on the
replay path).

`tools/` needs `g++` and Python with numpy/scipy/matplotlib. Its output —
`replay`, the demo CSVs, `figures/` — is gitignored: ~4 MB of derived data that
the one command above regenerates.

**`tools/` must stay a sibling of `MRCC_FlightComputer/`.** The build script
reaches the firmware through `../MRCC_FlightComputer/src`.

---

## What used to be here

This directory previously held a **PlatformIO** project — `src/bridge.cpp`,
`src/onboard_tx.cpp`, `lib/LoRaLink`, `lib/TelemPacket`, `lib/Subsystem`, plus
Arduino-IDE twins under `arduino/` — that spoke the 32-byte binary protocol in
`shared/protocol/PROTOCOL.md` over an EBYTE E32, later a bare SX1278.

**None of it was ever flown.** It transmitted binary at BW 125 kHz with hardware
CRC off; the real vehicle sends ASCII MRCC at BW 250 kHz with CRC on. Those two
cannot hear each other, so `bridge.cpp` could never have decoded the rocket it
appeared to serve. Meanwhile the firmware that does fly lived outside the repo
with no version control at all — this swap fixes that.

It is preserved in history rather than deleted outright:

```bash
git log --oneline --all -- firmware/lib/LoRaLink
```

To bring any of it back:

```bash
git checkout f735151 -- firmware/
```
