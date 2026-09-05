# IMU launch confirmation: consume each sample once

## Approved scope

Fix the repeated-IMU-sample launch confirmation in both A/B flight computers.
Use existing `lastImuUpdate`, preserve the 50 ms flight service cadence, >3g
threshold and five confirmations. Do not change barometric apogee, arming dwell,
backup/reset strategy, pyro pulse, pins, or configuration constants. No flashing.

## Implementation

1. Promote the original F1 A/B probes from expected failures to required tests.
   Add executable tests for held samples, equal-valued new samples, fresh low
   samples, health loss, timeout, and a new sample after a long service gap.
2. Add a last-consumed IMU timestamp local to Flight.cpp; initialize it when
   arming so already-seen pad data cannot start a launch confirmation.
3. In FS_ARMED, count only usable, unconsumed samples, at most once per flight
   service. Hold the count on an ordinary missing update, reset on new below-
   threshold evidence, declared IMU failure or the existing IMU_STALE timeout.
   Also break confirmation across a >IMU_STALE gap even if a new sample has just
   arrived when the loop resumes.
4. Require the same new/usable IMU evidence when a healthy IMU corroborates the
   existing barometric launch fallback. Preserve baro-only fallback when the IMU
   is explicitly marked unavailable; do not reinterpret a timeout as authority
   to launch from pressure alone.
5. Apply identical flight logic to A/B. Retain all other known failing probes.

## Verification and deliverables

- Fail-first targeted simulation tests, then all firmware tests with host ASan/UBSan.
- Compile A and B using `arduino-cli compile --fqbn esp32:esp32:esp32s3`.
- Compare nominal launch/apogee timing against the saved pre-fix evidence.
- Preserve the original report; write a new source-hashed results JSON and a
  concise fix report under docs/validation.
- Check A/B source parity and review the production diff for scope.

Host simulation does not establish actual sample delivery, ESP32 reset behavior,
electrical output or deployment reliability. Outstanding F2–F6 stay outstanding.

## Completion

- Before changes: launch-focused tests produced 10 failed assertions across A/B.
- After changes: all 12 launch-focused test methods pass.
- Full firmware suite: 50 methods, 40 pass and 10 remaining expected failures.
- Both ESP32-S3 sketches compile successfully with installed core 3.3.11.
- Nominal state/GPIO event sequences match the pre-fix report for every tested
  cadence/noise case. A/B Flight.cpp are byte-identical; production diff is
  confined to IMU launch sample freshness and its fallback corroboration.
- Results: docs/validation/2026-09-05-imu-launch-freshness.md and .json.
- No flashing or hardware operations performed.
