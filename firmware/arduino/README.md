# firmware/arduino/ — standalone receiver for the Arduino IDE

`E32Receiver/` is a **self-contained ground-side receiver**: it finds frames in the
E32's byte stream, checks CRC, unpacks every field, and prints readable telemetry
to the Serial Monitor. No laptop backend, no Python, no PlatformIO.

Its job is to answer *"is the RF link actually working?"* on the bench before the
real ground station is wired up.

## How this differs from the PlatformIO targets

| | `src/bridge.cpp` (PlatformIO) | `arduino/E32Receiver` (this) |
|---|---|---|
| Parses frames | **no** — raw passthrough | **yes** — CRC + all 15 fields |
| Needs the Python backend | yes | no |
| Output | bytes for `backend/` | human-readable columns |
| Boards | ESP32 only | Uno / Nano / Mega / ESP32 |

They are **not** redundant. `bridge.cpp` stays deliberately dumb so there is exactly
one parser in the real data path (`shared/protocol/packet.py`) and `raw.log` can
never miss a byte. This sketch is a diagnostic instrument — it trades that purity
for being able to tell you, standing in a field with no laptop, that packets are
arriving and what is in them.

## Open it

Arduino IDE → **File ▸ Open** → `firmware/arduino/E32Receiver/E32Receiver.ino`.

The IDE requires the sketch folder and the `.ino` to share a name, so open the
**folder's** `.ino`, not the parent. The other files (`E32Link.*`, `TelemPacket.h`)
are compiled automatically because they sit beside it.

Then: **Tools ▸ Board** → your board, **Tools ▸ Port** → your port, upload, and open
**Serial Monitor at 115200**.

For the ESP32 you also need the ESP32 core installed:
Preferences ▸ *Additional board manager URLs* →
`https://espressif.github.io/arduino-esp32/package_esp32_index.json`, then
Boards Manager ▸ "esp32".

## Wiring

**The E32 is a 3.3 V part.** On a 5 V board (Uno/Nano/Mega) this matters:

- **E32 TXD → Arduino RX** is fine. 3.3 V clears the ATmega's ~3.0 V input-high
  threshold — but with little margin. If RX is flaky, that margin is why.
- **Arduino TX → E32 RXD must be divided down** (1 kΩ series + 2 kΩ to GND, or any
  proper level shifter). 5 V straight into the module's RXD will damage it.
  A receiver never drives this line, so you can **leave it disconnected** unless you
  set `E32_RUN_CONFIG = 1`.
- **VCC is 3.3 V, not 5 V.** The E32 can pull ~120 mA in transmit; the Uno's onboard
  3.3 V regulator is marginal for a transmitter but fine for receive-only.
- **AUX must be connected.** Add a 4.7 kΩ pull-up to 3.3 V — the community fix for
  the module "locking up". The driver refuses to write while AUX is LOW.

| E32 | Uno / Nano | Mega 2560 | ESP32 |
|---|---|---|---|
| M0 | 4 | 4 | 25 |
| M1 | 5 | 5 | 26 |
| AUX | 6 | 6 | 27 |
| TXD → board RX | 10 *(SoftwareSerial)* | 19 *(Serial1)* | 16 *(RX2)* |
| RXD ← board TX | 11 **divide** | 18 **divide** | 17 |
| VCC | 3.3 V | 3.3 V | 3.3 V |
| GND | GND | GND | GND |

Pins are constants at the top of the `.ino`. The ESP32 map is identical to
`src/bridge.cpp`, so the two are interchangeable on the same harness.

## First run: make both radios agree

Two E32s only hear each other if their **channel, air rate, and UART baud match**.
This project uses 9600 baud UART, 2.4 k air rate, channel `0x17` (`PROTOCOL.md`).

To program a module: set `#define E32_RUN_CONFIG 1`, upload, watch the Serial
Monitor for `config VERIFIED`, then set it back to `0` and upload again. The LED
blinks 3× slow on success, flutters on failure. **The TX divider must be fitted
first** — this is the one path that writes to the module.

Do this for the receiving module *and* the airborne one.

## Link test against someone else's transmitter

Two radios agreeing on RF is **not** the same as two radios agreeing on the payload.
This sketch only prints frames that are 32 bytes, start `AA 55`, and pass CRC. A
transmitter sending `"Hello World"` — or any other protocol — is received perfectly
and then silently discarded, which on screen is indistinguishable from a dead link.

So the stats line counts **raw bytes** separately from valid frames:

```
--- 0 ok | 0 crc err | 0 lost (0.0%) | 448 raw B ---
    ^ RF IS ARRIVING but no frame ever matched. The link works; the payload format does not.
```

| ok | raw B | What it means |
|---|---|---|
| climbing | climbing | working — you are decoding real telemetry |
| 0 | 0 | no RF at all → channel, air rate, antenna, or TX not running |
| 0 | climbing | **RF is fine, formats disagree** → their sketch is not sending this frame |
| 0, crc err climbing | climbing | right format, damaged in the air → weak signal or antenna |

For the third case, rebuild with raw mode to see exactly what they are sending:

```bash
cd firmware/arduino && PLATFORMIO_BUILD_FLAGS="-DE32_SHOW_RAW=1" pio run -e uno -t upload
```

```
raw | 48 65 6C 6C 6F 20 57 6F 72 6C 64 0D 0A 48 65 6C  |Hello World..Hel|
```

**Both ends must therefore run the same protocol.** The transmitter side of this
project is `firmware/src/onboard_tx.cpp` (ESP32-S3, `pio run -e onboard_tx -t upload`).
If your partner is on a 5 V Arduino instead, they need an Arduino-IDE transmitter
sketch built on the same `TelemPacket.h` — there isn't one in this repo yet.

Before any range test, also confirm:

- **Antennas fitted on both modules.** Transmitting into an unfitted antenna port
  can damage the PA.
- **Both modules programmed identically** — channel `0x17`, 2.4 k air rate, 9600
  baud (see above). This is the single most common reason two E32s never meet.
- **Prove your own half first** with `-DE32_SELFTEST=1`. If the self-test passes and
  the air is silent, stop debugging this sketch — the fault is on the RF side.

## Reading the output

```
seq    t(s)     state  alt   vspeed  lat          lon           gAlt sat fix tilt vbat flags health
------ -------- ------ ----- ------- ------------ ------------- ---- --- --- ---- ---- ----- ------
  1240 310.2    COAST   1204 48.3    3.1234567    101.6543210   1210   9   3    4  7.4 C--A  BIGSPV
```

- `flags` — **C**ontinuity, **P**yro fired, **S**D ok, **A**rmed. `-` = clear.
- `health` — **B**aro, **I**MU, **G**PS, **S**D, **P**yro, **V**bat. `-` = that
  peripheral is **down**, and every field it feeds is untrustworthy. A down
  peripheral also prints a named `^ DOWN:` line underneath — never a blanket
  "AV FAILED" (see `PROTOCOL.md` § health).

Every 5 s you get a link summary:

```
--- 20 ok | 0 crc err | 0 lost (0.0%) ---
```

Reading it:

- **ok climbing, 0 crc err** — healthy link.
- **crc err climbing** — you *are* receiving RF, but it is corrupted. Marginal
  signal, interference, or antenna. Real telemetry is getting through the air.
- **neither climbing** — nothing is arriving at all. That is a channel, air-rate,
  or antenna mismatch, not noise. After 3 s of silence you also get
  `--- NO SIGNAL ---`.

That distinction is the whole point of showing CRC errors separately.

## Keeping the protocol header honest

`E32Receiver/TelemPacket.h` is a **copy** of `firmware/lib/TelemPacket/TelemPacket.h`,
because the Arduino IDE can only compile files inside the sketch folder. Copies
drift, and drift in a wire format means two boards that quietly disagree about
what byte 22 means.

After **any** protocol change:

```bash
./firmware/arduino/sync_headers.sh --check
```

Exit 1 and a diff means the copy has drifted; run it without `--check` to re-copy.
The full chain of truth is `packet.py` → `lib/TelemPacket` (proven by
`test/packet_check.cpp`) → this copy (proven by that script).

## Troubleshooting

| Symptom | Cause |
|---|---|
| `AUX=LOW (busy/unwired?)` at boot | AUX not connected, or no pull-up |
| Nothing at all, no CRC errors | channel / air-rate / baud mismatch between the two modules |
| CRC errors climbing | weak or noisy signal, antenna missing — RF is arriving, just damaged |
| Garbage characters in the monitor | Serial Monitor not set to **115200** |
| Uno: nothing received, ESP32 works | 3.3 V into the 5 V board's RX is marginal — level-shift it up |
| `config FAILED` | TX divider not fitted, AUX unwired, or module not on 3.3 V |
