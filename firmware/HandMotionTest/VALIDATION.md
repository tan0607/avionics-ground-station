# Validation — 2026-09-08

Later SD code changes are recorded in the
[2026-09-09 checkpoint validation](../../docs/validation/2026-09-09-sd-checkpoint-persistence.md).
The results and `tested-source-sha256.json` below describe their historical
revision, not the new SD checkpoint code. The pre-checkpoint active production
baseline is preserved in `source-snapshot-2026-09-09-pre-sd-checkpoint.json`.

The section below is the historical 2026-09-08 LOW-only validation. See the
2026-09-10 update at the end for the current binary/pulse behavior.

Status at this historical revision: source and build verification complete;
**not uploaded and not bench-tested**.

- `python3 -m unittest discover -s firmware/HandMotionTest/tests -p 'test_*.py' -v`: **13 tests passed** (9.767 s).
- Before implementation, motion/output tests failed as intended: original copies stayed PAD during hand-scale inputs and generated a HIGH edge on the bench/fire paths. Packet marker tests also failed before adding `HT=1`.
- Both A and B are exercised by the host checks. Both HAND and ORIGINAL_THRESHOLDS are host-compiled with AddressSanitizer / UndefinedBehaviorSanitizer.
- Covered: idealized hand lift/lower -> barometric APOGEE -> LANDED, stationary/no launch, stale held IMU sample, disarm, original 180 s wait and launch threshold, timer backup reason, original-profile full simulated flight, reset/no refire, console test pulse/repeat, and zero output HIGH edges.
- One stale-sample test fixture was corrected during development: after a held high sample, new low raw IMU readings still have a high filtered tail, which legitimately counts as fresh threshold evidence. The final test holds the sample without injecting fresh readings, including beyond its expiry. No production freshness/filter logic was changed.
- Packet checks compile the actual isolated Radio.cpp builder: `HT=1` decodes through the existing MRCC parser, countdown is transported, state/armed flag are consistent, and the wide-value fixture retains health fields and the 255-byte limit. No physical RF validation.
- Arduino-ESP32 **3.3.11**, FQBN `esp32:esp32:esp32s3`, default HAND profile:
  - A: compile passed; **450086 bytes** program, **26144 bytes** global RAM.
  - B: compile passed; **450090 bytes** program, **26144 bytes** global RAM.
  - Build directories: `/tmp/mrcc-hand-motion-build-A` and `/tmp/mrcc-hand-motion-build-B` (temporary; rebuild from source as needed).
- All **46 original A/B files** match `source-snapshot.json`; the pre-existing uncommitted work is preserved.
- `Flight.cpp`, `Filters.cpp`, `Sensors.cpp`, `Baro.cpp`, and `State.cpp` are byte-identical to the original current files for each vehicle.
- Manual source audit: every pyro GPIO write in both isolated Pyro.cpp files writes LOW, including the shared console/automatic pulse path and timer callback. This is software evidence, not a voltage measurement.
- Source/test digests: `tested-source-sha256.json`.

Still required: identify A/B and correct USB interface, confirm disconnected energetic load, upload, verify the HAND TEST banner and real sensor samples, then measure actual hand-motion behavior. Hand-scale pressure sensitivity is experimental and can false-trigger or fail to detect movement. A HAND-profile success does not validate production flight thresholds. Dashboard currently has no dedicated HAND TEST warning; `HT=1` identifies the raw packets and AR/FI are simulated.

## 2026-09-09 timer-only update

Backup changed from 19 s to 16 s in both isolated sketches and both formal sketches. HandMotionTest 13/13 checks and both isolated Arduino builds passed again. The original snapshot is preserved as `source-snapshot-2026-09-08.json`; current baseline and tested-source hashes were refreshed for the explicitly changed files. Full evidence: [16 s backup validation](../../docs/validation/2026-09-09-backup-timeout-16s.md). No upload or device test was performed in this update.

## 2026-09-10 binary telemetry and measurable bench pulse

Current behavior:

- HandMotionTest A/B now use the production 10 Hz, single-copy binary encoder.
- The reserved `TLM_FLAG_HAND_TEST` bit is always set by the isolated encoder.
  The updated receiver expands it to `MRCC,HT=1,...,AIR=...`; the backend and
  dashboard continue to consume the established text contract.
- Automatic APOGEE, 16 s TIMER BACKUP, and confirmed `T` then `Y` bench test
  issue a real 400 ms HIGH command on the pyro gate. The independent GPTimer
  still performs the LOW cutoff. A retained automatic-fired latch refuses any
  further automatic or manual bench pulse until a complete power cycle.
- This is only for a multimeter or non-energetic dummy load. It must never be
  tested with an e-match, igniter, or energetic material connected.

Verification:

- HandMotionTest: **14/14 passed**, including binary round-trip, `HT=1`, exact
  400 ms HIGH-to-LOW edges, timer backup, stationary/no-fire, and reset/no-refire.
- Production firmware: **133 passed with 2 registered expected failures** for
  the unresolved reset-interrupted-pulse cases.
- Backend: **48/48 passed**. Dashboard: **27/27 passed**, lint passed with four
  existing Fast Refresh warnings, and production build passed.
- Arduino-ESP32 3.3.11 builds passed: Hand A 454410 bytes / 26192 RAM; Hand B
  454414 / 26192; production A 453858 / 26192; production B 453862 / 26192;
  classic ESP32 ground receiver 293272 / 23388.
- `git diff --check` passed. `tested-source-sha256.json` was refreshed.

No firmware was uploaded and no serial port, voltage, current, sensor, RF, or
physical output was tested. A compiled HIGH edge is not measured terminal
voltage; that evidence must come from the multimeter/dummy-load bench run.
