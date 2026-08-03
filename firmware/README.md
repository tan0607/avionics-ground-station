# firmware/ — ESP32 telemetry link

Two ESP32 targets for the rocket ⇄ ground-station LoRa link, both built from this one
PlatformIO project:

| Target | Env | Where it lives | Job |
|---|---|---|---|
| **Bridge** | `bridge` | on the ground, USB to the laptop | forward the raw E32 byte stream to USB serial; LED on packet RX. Stays *thin* — no parsing. |
| **Onboard TX** | `onboard_tx` | on the rocket (ESP32-S3) | pack the shared 32-byte frame from your sensors and transmit at 4 Hz, air-rate 2.4k. |

> **The wire format is the contract.** `shared/protocol/PROTOCOL.md` + `shared/protocol/packet.py`
> are the source of truth. The firmware never redefines it — `lib/TelemPacket` is a
> **byte-for-byte C mirror**, verified against a Python-generated reference frame.

There is also a third, non-PlatformIO target: **[`arduino/E32Receiver`](arduino/)** —
a standalone receiver for the **Arduino IDE** that decodes and prints telemetry to the
Serial Monitor with no laptop backend. It runs on a Uno/Nano/Mega as well as an ESP32,
so you can prove the RF link works on the bench before the ground station is wired up.
See [`arduino/README.md`](arduino/README.md).

## Why an ESP32 devkit for the bridge?

Per `GROUND_STATION_PLAN.md §2`: the E32 is a **3.3 V** part, so a 5 V Arduino's TX line
would need level shifting. The ESP32 is natively 3.3 V, is ready for the phase-2 uplink
with no rewire, and shares a toolchain with the airborne board. Use the cheapest
DevKitC / WROOM-32 you have.

## Wiring (both targets, default pin map)

| E32 pin | ESP32 GPIO | Notes |
|---|---|---|
| M0  | 25 | mode select bit 0 |
| M1  | 26 | mode select bit 1 |
| AUX | 27 | **status line — must be connected.** Add a 4.7 kΩ pull-up to 3.3 V. |
| TXD | 16 (RX2) | E32 → ESP32 |
| RXD | 17 (TX2) | ESP32 → E32 |
| VCC | 3.3 V | **not 5 V** |
| GND | GND | |

Onboard LED (packet-RX indicator on the bridge) = GPIO2 on most DevKitC boards.
Pins are constants at the top of each `src/*.cpp` — change them to match your board.

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

## The one rule: AUX discipline

The E32 has no flow control other than the **AUX** pin. `AUX LOW = busy`
(transmitting / receiving / waking). Writing to the module's UART while AUX is LOW
corrupts its buffer and is the **#1 reported cause of the module locking up**
(`GROUND_STATION_PLAN.md §3`).

`lib/E32` centralises this: **every** UART write goes through `writeFrame()`, which
calls `waitAux()` first — and `waitAux()` has a **timeout**, so a mis-wired AUX can
never hard-stall the flight loop (it drops the frame instead of hanging).

E32 modes (`mode = (M1<<1)|M0`):

| Mode | M1 M0 | Use |
|---|---|---|
| 0 NORMAL | 0 0 | transparent TX/RX — normal flight/receive |
| 1 WAKEUP | 0 1 | adds a wake-up preamble |
| 2 POWERSAVE | 1 0 | RX only, low power |
| 3 CONFIG | 1 1 | write air-rate / UART / channel parameters |

### Configuring the module (air-rate 2.4k, 9600 UART)

Both E32s must share **air-rate 2.4k, 9600 UART, same channel** (`PROTOCOL.md`).
`E32::configure()` programs and *verifies* this in CONFIG mode. To use it: set
`#define E32_RUN_CONFIG 1` at the top of the sketch, flash once (the bridge blinks
3× on success / flutters on failure), then set it back to `0` and re-flash for normal
operation. 2.4k is the lowest practical air rate → longest range.

> The exact `SPED` / `OPTION` register bytes are documented in `lib/E32/E32.h` and are
> known-good for the **E32-433T20D**. Power/option bits differ on T30D / T30S — check
> your module's datasheet before a range test.

## Build & flash

```bash
# once, with a network (downloads the ESP32 platform + Arduino framework):
pio pkg install

# thereafter, fully offline:
pio run -e bridge -t upload          # build + flash the bridge
pio device monitor -b 115200          # watch the raw stream reach USB
```

The laptop backend opens the bridge's USB serial port at **115200** (`BRIDGE_USB_BAUD`);
that is independent of the 9600 baud on the E32-side UART.

Prefer the Arduino IDE? Copy `lib/E32/*` and (for onboard TX) `lib/TelemPacket/*` into a
sketch folder alongside the relevant `src/*.cpp` renamed to `<folder>.ino`.

## Onboard TX (Target 2)

`src/onboard_tx.cpp` is the **radio + packet layer** of the flight computer. Every
250 ms it services the peripherals, builds the frame, and writes it AUX-safely.

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
- **Never blind-write.** If AUX isn't HIGH within 200 ms the frame is *dropped* (seq
  held) rather than stalling the flight loop — the anti-lock-up rule, applied airborne.

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

## ⚠️ RFI risk: does the E32 TX knock out the GPS fix? (§8)

A 433 MHz transmitter sitting centimetres from a NEO-M8N can desense the GPS front-end
(the RocketTalk 1 W build had to add shielding + ferrite beads over exactly this). This
is a **must-test-before-flight** item (`GROUND_STATION_PLAN.md §8`). Planned bench test:

1. **Baseline** — power the flight computer with the **E32 TX disabled** (comment out
   the `radio.writeFrame(...)` call, or hold the E32 in POWERSAVE). Let the NEO-M8N get
   a 3D fix outdoors/by a window. Log for ~5 min: `gps_fix`, `gps_sats`, and (from
   u-center if available) C/N0. This is the "radio quiet" reference.
2. **Radiating** — re-enable the 4 Hz downlink into a **real 433 MHz antenna** (never
   TX into no load) at the intended flight power. Log the same fields for ~5 min.
3. **Compare** — did `gps_fix` drop from 3 → 2/0? Did `gps_sats` or mean C/N0 fall
   sharply the moment TX starts? A clear correlation = RFI desense.
4. **Duty-cycle sweep** (if step 3 is marginal) — try 1 Hz / 2 Hz / 4 Hz to see whether
   the fix survives at a lower TX duty cycle.
5. **Mitigations if it fails** — maximise E32↔GPS-antenna separation, add ground-plane
   shielding + a ferrite bead on the E32 supply, move the GPS antenna away from the E32
   and its feedline, and/or reduce TX power or rate. Re-run steps 1–3 after each change.

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
├── platformio.ini         two build envs (bridge, onboard_tx)
├── lib/
│   ├── E32/               AUX-disciplined E32 driver (shared)
│   ├── Subsystem/         per-peripheral fault isolation + I2C bus recovery
│   └── TelemPacket/       C mirror of PROTOCOL.md (header-only)
├── test/
│   ├── packet_check.cpp   host proof: C frame == packet.py frame
│   └── subsystem_check.cpp host proof: one dead sensor != dead vehicle
└── src/
    ├── bridge.cpp         Target 1: raw passthrough + RX LED
    └── onboard_tx.cpp     Target 2: 4 Hz telemetry TX + sensor seam
```
