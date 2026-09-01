# firmware/ — ESP32 telemetry link

Two ESP32 targets for the rocket ⇄ ground-station LoRa link, both built from this one
PlatformIO project:

| Target | Envs | Where it lives | Job |
|---|---|---|---|
| **Bridge** | `bridge_a` / `bridge_b` | on the ground, USB to the laptop | forward each received packet's bytes to USB serial; LED on packet RX. Stays *thin* — no parsing. |
| **Onboard TX** | `onboard_tx_a` / `onboard_tx_b` | on the rocket (ESP32-S3) | pack the shared 32-byte frame from your sensors and transmit at 4 Hz. |

**Every env names a vehicle, and `-e` is required to flash.** Two rockets flying
the same day cannot share a channel — LoRa does not pair, so each ground station
would decode *both* rockets (wrecking the loss count and hopping the map between
vehicles) while the two transmitters collided on air. `_a` builds 433.3 MHz, `_b`
builds 434.1 MHz; the reasoning and the numbers live in `lib/LoRaLink/LoRaLink.h`
and `platformio.ini`. A board flashed from the wrong env behaves perfectly until
the other rocket powers on, so `onboard_tx` prints its channel at boot — read it
before you close the airframe.

The radio is a **bare SX1278** (Ra-01 / Ra-02 / RFM95-style) driven over SPI with
`sandeepmistry/LoRa`. It is *not* an EBYTE E32: there is no wrapper MCU, so no
UART to the radio, no M0/M1/AUX, and no CONFIG mode. LoRa also delivers whole
packets with their boundaries already known, which is why the bridge no longer
hunts for a SYNC header in a byte stream.

> **The wire format is the contract.** `shared/protocol/PROTOCOL.md` + `shared/protocol/packet.py`
> are the source of truth. The firmware never redefines it — `lib/TelemPacket` is a
> **byte-for-byte C mirror**, verified against a Python-generated reference frame.

> ⚠️ **`arduino/` is E32-era and has not been ported.** Those four sketches
> (`E32Hello`, `E32Receiver`, `E32ReceiverSolo`, `E32Transmitter`) all talk UART to
> an EBYTE E32 and cannot hear an SX1278. Same for `lib/E32`, which neither
> PlatformIO target builds any more. See [`arduino/README.md`](arduino/README.md).

## Why an ESP32 devkit for the bridge?

The SX1278 is a **3.3 V** part on every pin, so a 5 V Arduino would need level
shifting on all four SPI lines. The ESP32 is natively 3.3 V, is ready for the
phase-2 uplink with no rewire, and shares a toolchain with the airborne board. Use
the cheapest DevKitC / WROOM-32 you have.

## Wiring

The two targets do **not** share a pin map, and that is not a matter of taste: on the
ESP32-S3 the classic board's GPIO26-32 are the SPI flash / PSRAM pins, and 17/18 belong
to the GPS.

| SX1278 pin | Bridge (ESP32 classic) | Onboard TX (ESP32-S3) |
|---|---|---|
| NSS / CS | 5 | 10 |
| MOSI | 23 | 11 |
| SCK | 18 | 12 |
| MISO | 19 | 13 |
| RST | 14 | 14 |
| DIO0 | 26 | 15 |
| VCC | **3.3 V — never 5 V** | 3.3 V |
| GND | GND | GND |

Both columns are that chip's own default SPI bus, so this is the wiring the common
Ra-02 guides already show. Pins are constants at the top of each `src/*.cpp`.

**Never power the module without an antenna on the SMA** — the reflected power has
nowhere to go but back into the PA.

Onboard LED (packet-RX indicator on the bridge) = GPIO2 on most DevKitC boards.

## The other rule: one dead peripheral is not a dead vehicle

`lib/Subsystem` enforces the guarantee that **no single peripheral can stop the
vehicle booting or transmitting**:

- **Init failure is never fatal.** `setup()` tries every peripheral once and
  records the result. There is no `while (1)` in that path. A board with all six
  devices dead still boots and still downlinks — telling you all six are dead.
- **In-flight death never stalls the loop.** Three consecutive failed reads mark
  a peripheral dead; it is then skipped and re-init'd every ~2 s, so a brownout
  that clears recovers on its own.
- **The ground station is told *which* device died,** by name, via the `health`
  byte (`PROTOCOL.md`). Never a blanket "AV FAILED".

**Your half of the contract:** `init()` and `read()` must return in a few ms.
The scheduler bounds *how often* a broken device is touched; it cannot bound a
call that never returns. For I2C that means `Wire.setTimeOut()` **and**
`I2CRecover.h` — a slave that browns out mid-transfer holds SDA low and wedges
the bus for every other device on it, which is the classic "one sensor died and
everything died" cascade.

```bash
cc -std=c++11 -I firmware/lib/TelemPacket -I firmware/lib/Subsystem \
   firmware/test/subsystem_check.cpp firmware/lib/Subsystem/Subsystem.cpp \
   -lstdc++ -o firmware/test/subsystem_check
./firmware/test/subsystem_check
```

## The one rule: both ends share one copy of the settings

Two LoRa radios only hear each other if **frequency, spreading factor, bandwidth,
coding rate and sync word all match**. That is the same class of bug the E32's
channel/air-rate mismatch was, and it fails the same silent way: perfect wiring,
perfect power, and nothing ever arrives.

So there is exactly one home for those numbers — **`lib/LoRaLink/LoRaLink.h`**.
Both `bridge.cpp` and `onboard_tx.cpp` call `loraLinkBegin()` and neither one names
a parameter itself, so they cannot drift apart. Change a value there, reflash
**both** boards.

| Setting | Value | Why |
|---|---|---|
| Frequency | 433 MHz | the module's band |
| Spreading factor | 7 | ~72 ms on air for a 32-byte frame → 29% duty at 4 Hz |
| Bandwidth | 125 kHz | standard |
| Coding rate | 4/5 | standard |
| Sync word | 0x12 | private network (LoRaWAN's is 0x34) |
| TX power | 17 dBm | PA_BOOST pin |
| Hardware CRC | **off** | see below |

**Why SF7 and not something longer-range.** SF9 costs ~247 ms per frame and simply
does not fit the 250 ms window. At 17 dBm into ~-123 dBm sensitivity there is still
roughly 40 dB of margin at 5 km, so the spreading factor is not what limits a model
rocket link. If you raise it, drop the downlink rate to match.

**Why hardware CRC is off.** The payload already carries its own CRC-16/CCITT
(`PROTOCOL.md`, bytes 30-31), and the ground station counts CRC failures to tell
"RF arriving but corrupt" apart from "no RF at all". LoRa's hardware CRC drops a bad
packet *inside* `parsePacket()`, which would erase exactly that evidence before
`raw.log` ever saw it — leaving a marginal link looking identical to a dead one.
One CRC, checked on the laptop.

There is no config-mode step any more: an SX1278 has no stored parameters, so the
old `E32_RUN_CONFIG` one-shot has nothing left to do. The radio is configured over
SPI at every boot.

## Build & flash

```bash
# once, with a network (downloads the ESP32 platform + Arduino framework):
pio pkg install

# thereafter, fully offline:
pio run -e bridge_a -t upload         # build + flash rocket A's bridge (433.3 MHz)
pio device monitor -b 115200          # watch the raw stream reach USB
```

The laptop backend opens the bridge's USB serial port at **115200**
(`BRIDGE_USB_BAUD`), which is now the backend's default — `--baud` is only needed if
you change `bridge.cpp`. There is no second baud rate to keep in step: the SX1278 has
no UART at all.

> This used to be a real trap. The backend defaulted to `--baud 9600` (the *radio's*
> old UART rate, never the rate of this link), and reading a 115200 stream at 9600 is
> undecodable garbage that looks exactly like a dead radio.

## Onboard TX (Target 2)

`src/onboard_tx.cpp` is the **radio + packet layer** of the flight computer. Every
250 ms it services the peripherals, builds the frame, and hands it to the radio.

- **The sensor seam.** Each peripheral owns an `init()` and a `read()` pair, listed
  in the `g_subsys[]` table. Those are the functions you fill from your real baro /
  NEO-M8N GPS / IMU / vbat drivers; readings land in the `g` struct. They ship as
  working *demo* stubs (pad-state, launch-site GPS fix) so the board transmits on
  the bench immediately. **Keep the units exactly as `PROTOCOL.md`** (dm/s, deg×1e7,
  V×10, tilt 0..180). Do not touch framing/CRC/TX below the seam.
- **Adding a peripheral** is one `SUBSYS(...)` line in `g_subsys[]` plus a
  `HEALTH_*` bit in `packet.py` + `TelemPacket.h` + `protocol.ts`.
- **A failing `read()` returns false** — it does not retry internally and does not
  abort. Returning false is how a peripheral reports illness; `lib/Subsystem`
  decides when that becomes "dead".
- **Boot logs every peripheral by name**, so "which one failed to initialise?" is
  answered on the serial console before you even look at the dashboard.
- **Sequence & loss.** The TX loop owns `seq` and increments it **only on a successful
  write**, so the ground station's loss counter reflects RF loss, not onboard hiccups.
- **A transmit never stalls the loop.** `LoRa.endPacket(true)` is the *async* form:
  it hands the frame to the radio and returns instead of blocking for the ~72 ms of
  air time, so `gps_drain()` and the sensor service pass keep their cadence. If the
  previous frame is still going out, `beginPacket()` returns 0 and this one is
  *dropped* with seq held — the same contract the old AUX check gave.

### Verify the packet layer byte-for-byte (no hardware)

`lib/TelemPacket` is a byte-for-byte C mirror of `shared/protocol/packet.py`.
`test/packet_check.cpp` rebuilds the reference frame `packet.py` generates and asserts
every one of the 32 bytes matches (frame `aa5500…744e`, CRC `0x4e74`):

```bash
cc -std=c++11 -I firmware/lib/TelemPacket firmware/test/packet_check.cpp -o firmware/test/packet_check
./firmware/test/packet_check         # -> PASS: firmware frame is byte-for-byte identical to packet.py
```

Run this whenever `packet.py` / `PROTOCOL.md` changes — it's the guardrail that keeps
firmware and ground station on the same wire format.

## ⚠️ RFI risk: does the radio TX knock out the GPS fix? (§8)

A 433 MHz transmitter sitting centimetres from a NEO-M8N can desense the GPS front-end
(the RocketTalk 1 W build had to add shielding + ferrite beads over exactly this). This
is a **must-test-before-flight** item (`GROUND_STATION_PLAN.md §8`). Planned bench test:

1. **Baseline** — power the flight computer with the **downlink disabled** (comment
   out the `LoRa.beginPacket()` block). Let the NEO-M8N get
   a 3D fix outdoors/by a window. Log for ~5 min: `gps_fix`, `gps_sats`, and (from
   u-center if available) C/N0. This is the "radio quiet" reference.
2. **Radiating** — re-enable the 4 Hz downlink into a **real 433 MHz antenna** (never
   TX into no load) at the intended flight power. Log the same fields for ~5 min.
3. **Compare** — did `gps_fix` drop from 3 → 2/0? Did `gps_sats` or mean C/N0 fall
   sharply the moment TX starts? A clear correlation = RFI desense.
4. **Duty-cycle sweep** (if step 3 is marginal) — try 1 Hz / 2 Hz / 4 Hz to see whether
   the fix survives at a lower TX duty cycle.
5. **Mitigations if it fails** — maximise radio↔GPS-antenna separation, add ground-plane
   shielding + a ferrite bead on the radio supply, move the GPS antenna away from the
   module and its feedline, and/or reduce `LORA_TX_DBM` or the rate. Re-run steps 1–3
   after each change.

The demo `gps_read()` reports `gps_fix`/`gps_sats` in the live downlink, so this test
can be watched **on the dashboard's link panel in real time** — flip the radio on and
see whether the fix indicator drops.

> Note the distinction the health byte draws here: RFI desense makes a *healthy*
> receiver report `gps_fix = 0`. That is `HEALTH_GPS = 1` with a bad fix — not a dead
> GPS. Only stop returning true from `gps_read()` when the receiver itself goes quiet;
> conflating the two would send you hunting a wiring fault when the real problem is
> shielding.

## Layout

```
firmware/
├── platformio.ini         four build envs: {bridge,onboard_tx}_{a,b} — role x vehicle
├── lib/
│   ├── LoRaLink/          the SX1278 settings both ends share (header-only)
│   ├── Subsystem/         per-peripheral fault isolation + I2C bus recovery
│   ├── TelemPacket/       C mirror of PROTOCOL.md (header-only)
│   └── E32/               DEAD: old E32 driver, built by neither target
├── test/
│   ├── packet_check.cpp   host proof: C frame == packet.py frame
│   └── subsystem_check.cpp host proof: one dead sensor != dead vehicle
└── src/
    ├── bridge.cpp         Target 1: packet passthrough + RX LED
    └── onboard_tx.cpp     Target 2: 4 Hz telemetry TX + sensor seam
```
