# A/B delayed auto-arm and ground countdown

## Approved behavior
- Normal prelaunch auto-arm: minimum 180 seconds since boot AND the latest 10 seconds of observed stationary, valid, updating ICM data AND gyro calibration complete.
- ICM unavailable prevents auto-arm; remove barometer-only auto-arm. Preserve post-arming launch/apogee/backup/pulse and in-flight reset behavior.
- Retry failed gyro calibration only while unarmed in PAD. Do not count missing samples or boot time as observed stillness.
- Show onboard remaining delay, remaining stillness and blocking reasons on the existing dashboard. Confirm ARMED from actual state/flag telemetry, never from a countdown reaching zero. Old/missing/stale telemetry stays explicitly unknown.

## Implementation / test-first sequence
1. Actual A/B C++ host tests for delay, motion, gaps, calibration retry, IMU loss, disarm, restart and no prelaunch output. Implement readiness API in Flight and calibration gap handling in Filters.
2. Add compact bounded prelaunch AW (blocker bits), AD (delay seconds), AS (stillness seconds) telemetry before optional SD recorder fields. Compile/test the actual packet builder; preserve precision and 255-byte bound. Add record fields and backend transport/CSV tests.
3. Add dashboard component tests for countdown, blocking reasons, confirmed arming, legacy/missing/invalid data, stale link and channel/reboot reset. Integrate into Go/No-Go with existing styles; no audio requirement or hardware buzzer promise.
4. Move synthetic liftoff to 200 s for the new policy; retain actual production constants in flight tests. Add arming telemetry to exporter and re-run A/B scenarios. Existing pulse/reset safety failures remain visible.
5. Run firmware suites, backend/protocol suites, dashboard tests/build, A/B compile, source parity, browser rendering and replay checks. Record evidence and limitations.

## Boundaries
No flashing, serial commands, hardware firing, changes to deployment thresholds or pulse duration. Existing dirty orientation/simulation work is preserved. A timeout is not physical isolation or proof the operator has left. UI countdown is a last-reported earliest opportunity, not an unconditional arming deadline. Packet omission must not leave a cached countdown looking current.

## Completed 2026-09-07
- Implemented and verified approved A/B arming policy, readiness telemetry, recording columns, and Live countdown/confirmation strip.
- Actual-C++ firmware suite: 67 passes plus 6 existing expected safety failures; strict safety retains six failures. Backend 48, protocol mount 6, dashboard 20 tests pass. Both ESP32-S3 sketches compile.
- Full A/B replays use ports 8004/8005; short previews crop the same C++ output to boot 174–185 s on 8006/8007. Existing backends preserved.
- Browser red/green: right-rail insertion clipped the flight-state list at 1366×768; compact full-width strip and short-screen timeline now show all phases. Both channel countdown/ARMED/stale transitions verified.
- Evidence and remaining boundaries: `docs/validation/2026-09-07-auto-arm-countdown.md`.
