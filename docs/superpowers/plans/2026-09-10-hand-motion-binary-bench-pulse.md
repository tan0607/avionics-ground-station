# HandMotion binary telemetry and bench pulse

Goal: update the isolated A/B HandMotionTest sketches to use the current 10 Hz
binary LoRa telemetry contract and emit an identifiable `HT=1` line after the
ground receiver decodes it. Restore a real, timer-bounded 400 ms pyro-gate pulse
in HandMotionTest so a multimeter or non-energetic dummy load can measure it.

Affected code:

- `firmware/HandMotionTest/MRCC_HandMotion_A` and `_B`: current binary codec,
  10 Hz/single-copy radio settings, HAND marker flag, and bench pulse messaging.
- `firmware/MRCC_GroundStation/MRCC_GroundStation.ino`: decode the reserved HAND
  flag back to `HT=1` without changing the production packet layout.
- production A/B `Config.h`: name the reserved flag consistently; production
  encoders leave it unset.
- HandMotion and downlink tests, source manifests, README, and validation record.

TDD sequence:

1. Make HandMotion packet tests require a 52-67 byte binary frame that decodes
   to `HT=1`, and make HandMotion GPIO tests require one HIGH edge followed by
   the existing timer cutoff and no re-fire.
2. Confirm focused tests fail because HandMotion still sends ASCII and forces
   the gate LOW.
3. Port the production binary radio path into both isolated sketches, set the
   HAND flag only there, teach the receiver to emit `HT=1`, and enable the real
   400 ms isolated bench pulse.
4. Run focused tests, full HandMotion/firmware/backend/dashboard suites, and
   compile ground station plus production and HandMotion A/B sketches.

Safety boundary: no flashing or serial reconnect. The bench pulse is for a
multimeter or non-energetic dummy load only; e-match, igniter, and energetic
material must remain physically disconnected. Production firing behavior and
the 400 ms duration are not changed.

Completed: focused fail-first tests exposed the legacy ASCII/LOW-only behavior;
the binary HAND flag, receiver expansion, and bounded bench pulse are now green.
Full verification is recorded in `firmware/HandMotionTest/VALIDATION.md`. No
upload or hardware action was performed.
