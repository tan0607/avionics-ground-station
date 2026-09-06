# Firmware

Separate flight computer sketches for vehicles A and B, plus the ground station.

### A/B auto-arm and dashboard countdown (2026-09-07)

Normal prelaunch arming requires all three: **180 s of boot uptime**, **the
latest 10 s of observed still IMU data**, and **completed gyro calibration**.
The two clocks run concurrently, not 180 + 10 s in sequence. Stillness retains
the existing limits: acceleration magnitude within 0.5 m/s² of gravity and gyro
magnitude below 5°/s. A missing/stale IMU cannot auto-arm, even with a healthy
barometer. Fresh, equal-valued samples count; repeatedly reusing one sample does
not advance the stillness timer. A gap over 250 ms breaks the PAD observation
window and any partially collected gyro-calibration batch.

Failed gyro calibration retries only in unarmed PAD, without a console command.
`X` blocks auto-arm until reboot; the manual `A` path does not bypass the delay,
IMU, calibration or operator-block gates. A prelaunch reboot restarts the boot
delay. In-flight reset recovery and post-arming launch/ejection logic are
unchanged. **PAD does not detect launch: confirm ARMED before launching.**

The Live dashboard's arming strip uses the onboard report, not a browser timer.
It shows the earliest remaining time and blocking reasons. `ARMED confirmed`
requires received ARMED state and an armed flag; zero seconds alone never means
armed. A stale link or non-advancing onboard timestamp suspends the display after
the existing 3 s freshness window. Missing/invalid fields show countdown
unavailable (including older firmware). Confirmation is visual; no new audible
alert or hardware buzzer is implemented.

PAD packets append `AW` (blocker bitmask), `AD` (boot-delay seconds remaining),
and `AS` (stillness seconds remaining), rounded **up** to whole seconds. AW bits:
1 delay, 2 stillness, 4 gyro calibration, 8 unavailable/stale IMU, 16 operator
block, 32 auto-arm disabled, 64 fired latch, 128 arming interlock. The three-field
block is all-or-none within 255 bytes and has priority over optional SDF/SDL/SDE
while in PAD; core flight/health fields retain their original precision. The
backend forwards it and records `aux_aw`, `aux_ad`, `aux_as`. Do not latch an old
countdown across a packet that omits it.

This is a software waiting window, not proof assembly is complete or a substitute
for verified hardware isolation. Firmware must be flashed and bench-validated
separately; a build or host simulation is not flight readiness. See
`docs/validation/2026-09-07-auto-arm-countdown.md` for evidence and remaining
reset/pulse safety findings.

### Confirmed A/B installation (2026-09-06)

Both flight computers mount with **IMU +Y toward the rocket nose**. Attitude
coordinates are `(X, Y, Z) = (sensor +X, sensor -Z, sensor +Y)`; upright and
stationary means roll/pitch near zero. These retain the existing Euler tilt
convention, rather than naming rotation about the nose "roll". Gyro prediction
and magnetometer heading use the same frame. Raw and filtered sensor-axis
vectors remain unchanged in telemetry and SD logs.

The ground station uses +Y for tilt on both receiver-confirmed A and B channels;
an input without channel context retains the legacy +Z fallback. Raw heading is
now the mounted-frame uncompensated baseline, so heading and angle logs from
older firmware use a different reference. Replaying old raw telemetry applies
the current channel mounting to tilt; saved CSV files are not rewritten.

This is an installation-coordinate correction. The existing two-angle filter,
Euler singularity and uncalibrated magnetic heading limitations remain. It does
not change flight-state or pyro conditions. Flash each matching A/B sketch and
reload the backend before expecting these source changes in live readings;
verify the mounted hardware separately from host tests.

| Sketch | Board | FQBN | Job |
|---|---|---|---|
| `MRCC_FlightComputer_A/` | ESP32-**S3** | `esp32:esp32:esp32s3` | vehicle A: sensors, filters, flight state, pyro, SD log, and a 2 Hz MRCC downlink |
| `MRCC_FlightComputer_B/` | ESP32-**S3** | `esp32:esp32:esp32s3` | vehicle B: same flight logic, with B's pins and radio channel |
| `MRCC_GroundStation/` | classic **ESP32** | `esp32:esp32:esp32` | receive that downlink and print it to USB for the backend |
| `SD_Doctor/` | ESP32-**S3** | `esp32:esp32:esp32s3` | bench-only SD card fault finder — no radio, no sensors. Flash it when the card won't mount, then drive it from the serial monitor |
| `GS_Doctor/` | classic **ESP32** | `esp32:esp32:esp32` | bench-only LoRa link fault finder — the receiver's twin of `SD_Doctor`. Flash it to the ground-station board when packets stop arriving |
| `TX_Doctor/` | ESP32-**S3** | `esp32:esp32:esp32s3` | bench-only fault finder for the **flight computer** — radio, IMU, baro, GPS, brownout. The half of the link `GS_Doctor` cannot see |
| `PinForce/` | ESP32-**S3** | `esp32:esp32:esp32s3` | bench-only drive test on the six LoRa lines. `TX_Doctor` reports a line as held; this one puts ~40 mA behind the pad to say whether that hold is a soft clamp or a hard short. Run it on both boards and diff the tables |
| `Pyro_Doctor/` | ESP32-**S3** | `esp32:esp32:esp32s3` | bench-only MOSFET module check — the only sketch here that raises the pyro gate on purpose. Drives it from the serial monitor so a meter can answer whether the FET switches, **with a resistor across OUT and no e-match anywhere** |

### When the link is silent, flash `GS_Doctor`

Almost every fault on this link looks identical from the operator's seat. Wrong
frequency, wrong SF/BW/CR, wrong sync word, CRC off at one end, DIO0 in the wrong
hole, or a rocket that simply isn't switched on — all of them give you **zero
packets and no error message**. `MRCC_GroundStation` prints `RX ready` and then
says nothing for the rest of the day.

`GS_Doctor` exists to turn that one silence into distinguishable answers. It
talks to the SX1278 through raw registers with no LoRa library, for the same
reason `SD_Doctor` carries a bit-bang path: a diagnosis should not depend on the
library you are trying to diagnose.

| key | test | the question it answers |
|---|---|---|
| 1 | Radio present? | is the module wired — separately for the read path and the write path, because a dead MOSI passes a read test perfectly |
| 2 | Link parameters | does the modem accept and hold every value in the contract, and what are they, so you can eyeball them against `Radio.cpp` |
| 3 | Listen | **polled**, so it works with DIO0 unwired. Counts CRC failures separately: "RF arriving and mangled" is a different problem from "nothing arriving" |
| 4 | DIO0 wiring | the fault test 3 deliberately cannot see. A perfect link with DIO0 in the wrong hole is exactly as silent as no antenna, and `MRCC_GroundStation` only ever prints from that interrupt |
| 5 | Band scan | RSSI sweep of 433.0–434.8 MHz. A hump ~250 kHz wide is a transmitter — read its centre off the scale to find what a rocket is *actually* flashed to |
| 6 | A/B check | 10 s on each channel. The fastest answer to "is this box on the wrong rocket" |
| 7 | TX beacon | makes this box transmit, to test a second one without waiting for a rocket. Payload deliberately carries no `MRCC` substring so it can never be half-parsed into a flight record |
| 8 | SPI pin scan | holds three pins and sweeps the fourth — built for one jumper in the wrong hole, which is what actually happens on a bench |
| N | stored channel | what's in NVS, which is what the box will actually boot onto — not the `#define` |

Tests 1 and 2 run automatically at boot. Tests 4, 5 and 6 need the rocket powered
and transmitting.

### …and `TX_Doctor` for the other end

`GS_Doctor` can only tell you what does or does not arrive. Every fault on the
rocket end is invisible to it, so the transmitter gets its own.

| key | test | the question it answers |
|---|---|---|
| 1 | Radio present? | same two-direction SPI handshake as `GS_Doctor` |
| 2 | Link parameters | modem readback vs the contract, with the reset-default column that makes a refused write visible |
| 3 | **Transmit** | the one worth flashing for — see below |
| 4 | DIO0 TxDone | `Radio.cpp` transmits async and waits on this interrupt. Unwired, telemetry **does not stop** — it falls back to the `TX_MAX_AIR` timeout and pays up to 300 ms of a 500 ms budget per packet, silently. `txFallbackCount` is the only evidence and nobody reads it |
| 5 | Power sweep | transmits at rising power to find where the supply gives out. It can't measure sag (no VBAT divider), so it stamps the level into RTC memory — **if the board reboots, that is the result**; come back and read the boot banner |
| 6 | Listen | the other half of `GS_Doctor`'s beacon. Run both and the pair proves the link in each direction with nobody in a field |
| 7 | I2C bus scan | both sensors share one bus, so one device holding SDA low takes out the other — the symptom is "the barometer died" when the fault is the IMU |
| 8 | Sensors | IMU **sample rate**, not just "the chip answers" — plus **which axis reads ±1 g upright**, which is the one measurement needed to fix the rotated mount. Then a compensated baro pressure and MSL altitude |
| 9 | GPS | raw NMEA with a baud hunt — a module reflashed to 38400 looks exactly like a module that isn't wired |

**Test 3 is the headline.** Air time is a *fingerprint* of the modem settings, so
a stopwatch verifies SF and bandwidth with no receiver, no second person, and no
antenna range — and it catches what test 2 can't: a register that reads back
correctly while the modem does something else.

| payload | SF7 (the contract) | SF8 | SF9 |
|---|---|---|---|
| 237 bytes | **187 ms** | 328 ms | 584 ms |

At SF9 a single copy outlasts the whole 500 ms `SEND_INTERVAL`. At SF8 every
transmission would exceed `TX_MAX_AIR` and the fallback would fire on every
packet with telemetry still flowing. A board on the wrong spreading factor can't
hide from a clock.

Test 3 also reports the duty cycle, which moves whenever the packet grows: two
187 ms copies plus the 60 ms gap is 434 ms of a 500 ms window — **87%**, with
66 ms of margin. Re-run it after adding a field.

**Pyro is never touched.** The gate pin is driven LOW in `setup()` and never
raised; the sketch has no fire path at all.

Plus `tools/` — host-side, never compiled into the flight build. See
[Filter figures](#filter-figures-tools).

The ground station prints one line per frame:

```
len=237 RSSI=-53 SNR=10.2 | MRCC,PKT=207,T=207.5,ST=LANDED,AL=0.0,...,SD=1,BA=1,IM=1
```

That exact shape is the contract with the laptop — `shared/protocol/mrcc.py`
parses it, and the `len=` field is what lets the backend tell a truncated frame
from a clean one. Do not reorder or rename the prefix.

**One packet in ten carries the recorder block**, appended to the line above:

```
,SDF=7,SDL=3412,SDE=0
```

The open `/FLIGHT%03d.CSV` by index, its line count, and the count of failed
writes — what `printStatus` prints to USB every 5 s, for the operator who is
19 km from that port. `SD=1` says the card is MOUNTED; a card that mounts, opens
a file and then stops accepting writes says `SD=1` for the whole flight, and a
line count that stops moving is the only thing that shows it. The ground station
differences `SDL` between reports for the write rate, so no rate is sent.

Every tenth packet, not every packet, because the packet is 189–204 bytes
measured against a 255-byte limit and the duty cycle above has 66 ms of margin.
`SD_BLOCK_EVERY` in `Config.h` sets the cadence, and the block is **dropped
rather than truncated** when the flight fields leave no room for it: what sits
at the tail of the packet is the health block, and losing `SDL` for one tick
costs nothing next to losing the fields that say something is wrong.

**Renaming a payload key is a breaking change, even though nothing errors.**
`mrcc.py` looks every field up by name, so an unknown key parses fine and lands
in a catch-all — the field it was supposed to fill just keeps its default. That
is how `ALT` → `AL` left the ground station showing 0 m altitude on a flying
rocket with the real number sitting in `raw.log`. If you rename a key here, add
the alias in `mrcc.py:FIELD_ALIASES` in the same commit;
`backend/tests.py:test_every_field_the_firmware_sends_has_somewhere_to_land`
reads this sketch's format string and fails if you don't.

---

## Flashing: the one thing you must not get wrong

**Open the sketch folder matching the rocket.** Each `.ino` has the same name
as its folder. The flight computer's vehicle is already set in `src/Config.h`;
there is no need to edit it when switching rockets. Its boot banner prints
`MRCC FLIGHT COMPUTER A` or `MRCC FLIGHT COMPUTER B`.

| Flight computer | `LORA_SCK` | `SD_CS` | `VEHICLE` | Frequency |
|---|---|---|---|---|
| `MRCC_FlightComputer_A` | 12 | 7 | `VEHICLE_A` | 433.3 MHz |
| `MRCC_FlightComputer_B` | 47 | 6 | `VEHICLE_B` | 434.1 MHz |

All other settings and flight logic are identical. Continuity sensing stays
disabled (`PYRO_CONT_ENABLED=0`); B uses GPIO6 for SD chip select.
Shared flight-code changes must be applied to both copies.

Match the receiver channel to the rocket:

| Board | Sketch | Set to |
|---|---|---|
| Rocket A (S3) | `MRCC_FlightComputer_A` | `VEHICLE_A` |
| Ground station A (ESP32) | `MRCC_GroundStation` | `VEHICLE_A` |
| Rocket B (S3) | `MRCC_FlightComputer_B` | `VEHICLE_B` |
| Ground station B (ESP32) | `MRCC_GroundStation` | `VEHICLE_B` |

*One rocket, one letter, both its boards.*

### One ground station for both rockets

You do not need two receiver boxes. The ground station's channel is switchable
at runtime — from the dashboard, or from a serial monitor — so one box covers a
whole launch day:

| control | does |
|---|---|
| **Set → Radio Channel** in the dashboard | A/B switch from the laptop — the usual way |
| `A` on the serial monitor | listen to rocket A (433.3 MHz) |
| `B` on the serial monitor | listen to rocket B (434.1 MHz) |
| `?` on the serial monitor | print the current channel, packet count and last RSSI/SNR |

Both airframes can sit powered on the pad at once — they are on separate
channels, so they do not collide and neither one interferes with the other. Fly
A, switch, fly B. No reflash, no reboot, and the backend's serial connection
survives the switch. `VEHICLE` in the sketch is only the power-on default.

The dashboard shows the channel the box **reports**, not the one it was asked
for, so a command that does not land reads as the channel simply not changing.
It stays blank until the receiver has announced itself.

The onboard LED (GPIO2) pulses on every packet received. That is a link-alive
indicator you can read across a field with the laptop shut — lit and flickering
means frames are arriving. It says nothing about which channel; the console and
the boot banner are the authorities there.

The switch prints a marker into the stream, so `raw.log` records when it
happened:

```
### GS CHANNEL=B FREQ=434.100MHz PREV_PKTS=1834 ###
```

**Loss counters restart with the link, automatically.** The two rockets number
their packets independently, so a switch is a `seq` jump — and left alone, a jump
under `LossTracker`'s `RESET_GAP` books hundreds of losses that never happened,
while one over it re-baselines but keeps the first rocket in the denominator
forever. Neither is a number to read during a flight, so the backend resets the
counters when it sees the channel change and notes it in `mission.log`. The
per-flight CSVs still carry `seq`, so anything finer can be recomputed later.

You only need a second receiver box if you want to watch both rockets at the
same time. One SX1278 tunes one frequency at a time; there is no scan mode that
would not drop packets while it looked away.

**The channel survives a reboot** (stored in NVS). That is not a nicety:
attaching the backend *resets this board* on purpose (`sources.py::_reset_board`,
for a known state and a boot banner). Without persistence, every reconnect would
drag the box back to the compile-time default — you switch to B, the laptop
reconnects, and you are on A again with nothing saying so. `VEHICLE` in the
sketch is therefore only the **factory** default: first boot, or after a flash
erase. The boot banner says which you got:

```
RX ready - vehicle B @ 434.100 MHz  (restored from last switch; rocket must match)
RX ready - vehicle A @ 433.300 MHz  (compile-time default; rocket must match)
```

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
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
```

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/MRCC_GroundStation
```

Add `--upload -p <port>` to flash. Find the port with `arduino-cli board list`.

Libraries: `LoRa` (sandeepmistry), plus the flight computer's sensor stack
(ICM-20948, BMP280, TinyGPS++). Install once with `arduino-cli lib install`.

---

## Filter figures (`tools/`)

Host-side only. `tools/replay.cpp` **links `MRCC_FlightComputer_A/src/Filters.cpp`
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

**`tools/` must stay a sibling of `MRCC_FlightComputer_A/`.** The build script
reaches the firmware through `../MRCC_FlightComputer_A/src`.

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
