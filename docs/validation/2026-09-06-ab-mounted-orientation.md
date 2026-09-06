# A/B +Y nose orientation — 2026-09-06

Both vehicles' +Y nose installation was confirmed by the user in this session.
Source implementation is complete. No board was flashed, backend restarted,
serial command sent, or firing circuit operated.

## Coordinate contract

Attitude `(X, Y, Z) = (sensor X, -sensor Z, sensor Y)`. This is a right-handed
rotation retaining sensor +X as the transverse reference and making attitude +Z
the nose. Nose upright gives roll/pitch near zero. Existing roll/pitch field
names denote Euler tilt angles about attitude X/Y, not spin about the nose.

Raw/LPF roll uses `atan2(-az, ay)`; pitch's acceleration formula is algebraically
unchanged. Kalman and complementary gyro propagation use `(gx, -gz)`.
Magnetometer alignment retains the existing AK09916-to-IMU mapping, then applies
the mounting rotation: `(my, mz, mx)`. Raw heading and tilt-compensated heading
now share this reference, including FILTER_ENABLED=0.

Raw and filtered sensor vectors, acceleration norms, gyro bias calibration,
flight state machine, pyro logic, pin settings and thresholds are unchanged.
Both channel-confirmed A/B parser streams use +Y tilt; unknown channel context
retains +Z. Channel switching, wire output and CSV output are covered.

Old firmware angle/heading logs use the previous reference. Existing CSV files
are preserved; replaying raw data derives tilt using today's channel mapping.

## Evidence

- Before changes: 44 host subtest assertions failed, including +Y upright roll
  at 90 degrees and incorrect gyro pitch response. Two protocol tests failed:
  A's representative stationary sample produced 87 instead of 3 degrees and A
  upright after a channel switch produced 90 instead of 0 degrees.
- After changes: 4 orientation test methods pass, exercising real State.cpp and
  Filters.cpp in 54 synthetic runs across A/B, including filter enabled/disabled,
  upright and +/-30 degree tilts, mounted gyro axes with acceleration rejected,
  heading quadrants and separate roll/pitch compensation. AddressSanitizer and
  UndefinedBehaviorSanitizer enabled; no errors reported.
- Mounted tilt: 6 methods pass, including A/B dashboard wire and CSV checks.
- Full firmware suite: 59 methods, **51 pass, 8 existing expected failures**.
  A/B source parity check passes. Existing flight regression suite passes its
  ordinary cases; its input harness still uses sensor +Z and is not an installed
  +Y deployment test. No new ejection behavior is implemented or claimed.
- Backend: 48 tests pass. MRCC protocol self-test passes.
- ESP32-S3 compile A: 441942 bytes flash, 26088 bytes RAM.
- ESP32-S3 compile B: 441946 bytes flash, 26088 bytes RAM.
- `git diff --check` passes; Flight.cpp, Pyro.cpp and Config.h have no changes.

## Boundary

The 8 expected failures concern existing settle-window, reset/deadline and pulse
timing behavior; they are not passes or fixed by this work. Ejection review is
deferred as requested. The existing two-angle filter is not replaced with full
3D attitude estimation, and magnetic calibration/Euler singularities remain.
Hardware IMU/magnetometer alignment, live angle signs and installed readings
still require physical verification after flashing the matching A/B firmware
and loading the updated backend. These builds do not establish flight readiness.
