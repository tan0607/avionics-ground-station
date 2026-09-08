# Hand-motion dry run — 2026-09-08

Goal: independent flashable A/B sketches using real sensors and the current flight-state code for a hand lift/lower test. Never energize the pyro output.

Scope: only firmware/HandMotionTest/ and this plan. Snapshot the current uncommitted A/B sources; preserve every original file. Graph index refers to historical source, so inspect current source directly.

Implementation:
1. Copy current A/B sketches into isolated MRCC_HandMotion_A/B folders and record source SHA256.
2. Add failing host tests for hand-scale launch/ascent/descent, stationary/no-launch, fallback reason, stale samples, original-threshold profile, and all pyro entry paths remaining LOW.
3. Use hand-test-only thresholds (15 s boot wait, unchanged 10 s stillness/calibration, 1.25 g x 2 launch, 0.5 m max gain, -0.3 m/s x 4 descent); preserve flight state, filters, freshness and 19 s backup. Add optional original-threshold compile profile.
4. Replace the isolated output HIGH operation with unconditional LOW, including console bench tests. Keep simulated latches/status and label USB and RF data as HAND TEST. No runtime option can enable output.
5. Document real-sensor limits, commands, profile differences and operation. Compile both ESP32-S3 sketches and run host tests. Verify original hashes unchanged.

Verification: python3 firmware/HandMotionTest/tests/test_hand_motion.py -v; arduino-cli compile --fqbn esp32:esp32:esp32s3 <each sketch>; source hash comparison.

Flash boundary: user must identify A/B and confirm disconnected energetic load before any port reset/upload. USB inventory alone does not establish board identity. No upload or serial open until resolved. Host synthetic inputs/builds do not validate real sensor response, radio, electrical output or flight readiness.

## Completion evidence

Independent sketches, README and validation record saved in `firmware/HandMotionTest/`. Thirteen host tests passed; default HAND A/B Arduino builds passed with ESP32 core 3.3.11. Original 46-file snapshot unchanged. Upload remains pending board identification and physical load disconnection confirmation; no port was opened or reset during this task.
