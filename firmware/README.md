# firmware/ — ESP32 telemetry link

Two ESP32 targets for the rocket ⇄ ground-station LoRa link, both built from this one
PlatformIO project:

| Target | Env | Where it lives | Job |
|---|---|---|---|
| **Bridge** | `bridge` | on the ground, USB to the laptop | forward the raw E32 byte stream to USB serial; LED on packet RX. Stays *thin* — no parsing. |
| **Onboard TX** | `onboard_tx` | on the rocket | pack the shared 32-byte frame and transmit at 4 Hz. *(added in the onboard-TX commit)* |

> **The wire format is the contract.** `shared/protocol/PROTOCOL.md` + `shared/protocol/packet.py`
> are the source of truth. The firmware never redefines it — `lib/TelemPacket` is a
> **byte-for-byte C mirror**, verified against a Python-generated reference frame.

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

## Layout

```
firmware/
├── platformio.ini         two build envs (bridge, onboard_tx)
├── lib/
│   ├── E32/               AUX-disciplined E32 driver (shared)
│   └── TelemPacket/       C mirror of PROTOCOL.md + host CRC test (onboard TX)
└── src/
    ├── bridge.cpp         Target 1: raw passthrough + RX LED
    └── onboard_tx.cpp     Target 2: 4 Hz telemetry TX
```
