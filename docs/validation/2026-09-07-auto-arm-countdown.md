# A/B 180-second auto-arm, telemetry and dashboard

## Scope and provenance

User approved minimum 180 s boot wait AND latest 10 s stable ICM data AND completed
gyro calibration, no ICM-down/baro-only prelaunch arming, plus ground countdown
and armed confirmation. Both boards retain +Y nose mounting. Existing orientation
and simulation work is preserved. No flash, serial command or physical pyro action
was performed. Impeccable product guidance informed a compact existing-style status
strip; short laptop screens use a two-column flight-state list so it remains visible.

Actual production State/Filters/Flight/Pyro C++ runs in the host harness. The radio
test compiles the verbatim current buildTelemetryPacket body with those modules;
it does not exercise SPI, RF or LoRa drivers. Simulation replay is an explicitly
labelled 20 Hz analysis stream, not the physical 2 Hz downlink.

## Red → green

- New auto-arm cases initially: 14 failed A/B subcases and two missing-readiness
  field errors. Old code armed too early, allowed barometer-only arming and did
  not retry timed-out calibration. Calibration-gap test separately failed on A/B.
- Current auto-arm tests: 8 methods pass on actual A/B production constants;
  observed-window/stale-data handling, operator block, prelaunch reset, delayed
  movement and autonomous post-installation calibration are covered.
- Actual packet test initially lacked AW/AD/AS. Three packet/transport/CSV methods
  now pass for both vehicles, including oversized optional-block omission and
  real ARMED state/flag reporting. Host wrapper explicitly returns from renamed
  main to avoid C++ undefined fallthrough in the packet harness.
- UI tests initially lacked the component; a later test exposed duplicate raw
  AW/AD/AS fields in the general strip. Current arming tests cover confirmation,
  concurrent blockers, zero-but-not-armed, legacy/missing/invalid reports,
  duplicates/staleness, reboot/channel reset and general-strip deduplication.

## Final checks

- `backend/.venv/bin/python -m unittest discover -s firmware/tests -p 'test_*.py' -v`:
  **73 methods: 67 passed, 6 expected failures** (existing safety findings below).
- `python3 firmware/tests/test_ejection_simulation.py --strict-safety -k test_safety_ -v`:
  12 methods, six passes and six actual failures; nonzero status retained.
- `backend/.venv/bin/python -m backend.tests`: 48 passed.
- `python3 -m shared.protocol.mrcc`: self-test passed.
- `python3 -m unittest shared.protocol.test_mounted_tilt -v`: 6 passed.
- Dashboard `npm test`: 20 passed; `npm run build`: passed, existing >500 kB bundle
  advisory remains. No dependency updates were made.
- ESP32-S3 A compile: 442466 bytes flash, 26104 bytes static RAM.
- ESP32-S3 B compile: 442470 bytes flash, 26104 bytes static RAM.
- A/B source-parity test and `git diff --check`: passed.

## Current actual C++ simulation events

New output: `flights/simulations/20260907-autoarm-180s/` (runtime artifacts).
No production constants overridden. Synthetic liftoff moved from 20 s to 200 s
so the simulation actually waits through the new prelaunch gate.

| Event | A and B boot uptime |
|---|---:|
| ARMED | 180.010 s |
| BOOST | 200.260 s |
| COAST | 202.210 s |
| Normal APOGEE | 212.860 s |
| Normal GPIO HIGH → LOW | 212.910 → 213.310 s |
| Baro-loss backup GPIO HIGH → LOW | 219.310 → 219.710 s |

Normal output is T+12.910 s relative to prescribed liftoff; backup is T+19.310 s.
Both pulses are 400 ms in the serviced host loop. Pad-only auto-arms but never
launches or fires. Pad input is AX=0, AY=+9.80665, AZ=0; launch still uses the
filtered vector magnitude and ejection still uses flight state/barometric
apogee or backup time, not orientation angles. Inputs are prescribed synthetic
motion, not a validated motor/airframe/parachute dynamics model.

## Browser/backend verification

Full actual-C++ replays: A on port 8004, B on 8005. Separate arming-preview
replays on 8006/8007 crop the SAME C++ output to boot 174–185 s and loop it;
they do not run accelerated or shortened production arming logic.

Isolated Playwright Chromium used because the listed in-app browser skill cache
was missing. Observed both channels' countdown, actual `ARMED confirmed`, and
`Telemetry stale` on disconnect, with zero browser page errors. Backend /stats
reports MRCC/replay with zero CRC/unknown-state/source errors; /gs confirms A/B.
The first 1366×768 render exposed a clipped flight-state list; the final strip
layout and short-screen two-column timeline fix it. Final screenshots are
`final-8006-{countdown,armed,stale}.png` and equivalent 8007 names in the output
directory. List-item bounds verified inside the card and the 768 px viewport.
The existing frontend loss estimator may briefly spike when a replay loops and
sequence numbers rewind; backend loss remains zero. This is a replay-display
limitation, not measured RF loss, and is outside the arming change.

## Remaining safety boundaries

The full observed-stillness finding is fixed (previously two expected failures).
Three pre-existing limitations remain on BOTH vehicles (six failures):

1. In-flight reset restarts backup time: probe fires 30.300 s after original
   detected launch instead of the 19 s deadline.
2. A 900 ms loop stall while GPIO is high stretches a nominal 400 ms pulse to
   900 ms.
3. Reset 10 ms into a pulse leaves the fired latch set and does not resume it.
   This exposes an anti-refire tradeoff, not a recommendation to refire.

They are not solved or hidden by the arming changes. Serial power, mounted ICM
readings, I2C timing, physical interlocks, MOSFET electrical output, igniter energy
and successful recovery all remain hardware/flight-validation work. The 250 ms
PAD data-gap limit must be checked against actual hardware sample timing.
