# A/B mounted orientation

User confirmed both IMU +Y axes point toward the rocket nose. Align attitude
and dashboard tilt with that installation before discussing ejection.

- Use the right-handed attitude frame (X, Y, Z) = (sensor X, -sensor Z, sensor Y).
  Keep sensor-axis raw/filtered vectors and gyro calibration unchanged; transform
  only attitude inputs, including gyro rates and aligned magnetometer vectors.
- Update A/B Filters.cpp, Filters.h and Sensors.cpp together. Raw heading becomes
  the mounted-frame, uncompensated heading; filtered heading uses that same frame.
- Set both receiver channels to +Y tilt; preserve unknown-channel +Z fallback.
- Add fail-first host checks of actual A/B filter code: upright and signed tilts,
  gyro propagation under rejected acceleration, heading and vector preservation,
  including FILTER_ENABLED=0. Add A tilt parsing/CSV coverage.
- Verify focused tests, firmware suite, backend/protocol suite and A/B ESP32-S3
  compilation. No flight/pyro behavior changes, flashing, serial commands, or
  physical firing. Host results do not establish live hardware readiness.

Commands:
`python3 -m unittest discover -s firmware/tests -v`
`backend/.venv/bin/python -m unittest shared.protocol.test_mounted_tilt -v`
`backend/.venv/bin/python -m backend.tests`
`arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A`
`arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B`

Completed: fail-first tests reproduced 44 host subtest failures and 2 protocol
failures. The corrected filter passes all 4 orientation methods (54 synthetic
runs across A/B and filter modes), and all 6 mounted-tilt methods pass. Full
firmware suite: 59 methods, 51 pass and 8 pre-existing expected failures.
Backend: 48 pass; protocol self-test passes. ESP32-S3 A/B compiles pass.
See `docs/validation/2026-09-06-ab-mounted-orientation.md` for validation limits.
