# Mounted tilt and barometer freshness

Goal: correct tilt for the confirmed +Y nose mounting and prevent repeated
barometer samples from advancing altitude filtering or apogee confirmation.
Inspect live reset/arming evidence without changing hardware state.

- Preserve existing uncommitted IMU launch freshness changes and simulation files.
- Add failing host regressions for F2, repeated samples, stale recovery, and actual
  measurement timing; fix both A/B Flight.cpp without changing thresholds,
  backup timing, pyro pulses, or reset recovery policy.
- Confirm A mounting before applying a shared telemetry tilt axis correction.
  Keep transmitted sensor axes unchanged; test upright, sideways, inverted,
  measured pad samples, and decoded telemetry. Check attitude consumers.
- Diagnose ARMED-to-PAD from source and recorded/live data. Do not infer a
  hardware cause from an onboard clock reset or hide reset with forced arming.
- Verify focused fail-first tests, full firmware simulation suite, backend /
  protocol checks, relevant dashboard checks, and A/B ESP32-S3 builds.
- Save source-hashed simulation evidence and a concise validation report.

No flash, serial commands, firing circuit operation, process restart, or claim
of hardware readiness. Existing F3-F6 findings remain separate unless evidence
requires an in-scope correction. Safety finding counts must remain explicit.

## Completion

- Fixed barometer freshness in A/B and verified original F2 probes now pass.
- Applied +Y tilt only to receiver-confirmed B; A retains +Z pending confirmation.
- Full firmware suite: 47 pass, 8 existing expected failures. Backend 48 pass,
  mounted-tilt 5 pass, protocol self-test pass; A/B compile successful.
- Saved source-hashed simulation and validation report under docs/validation.
- Live reset pattern verified without channel switches; actual cause remains
  unconfirmed because radio telemetry lacks reset reason and operator context.
- No flashing, backend restart, or hardware control performed.
