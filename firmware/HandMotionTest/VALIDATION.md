# Validation — 2026-09-08

Status: source and build verification complete; **not uploaded and not bench-tested**.

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
