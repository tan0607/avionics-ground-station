# Ejection logic simulation on the host

## Goal and boundaries

Test the current A and B flight computers without flight tests or hardware access.
Compile each vehicle's real `Filters.cpp`, `Flight.cpp`, `Pyro.cpp`, and `State.cpp`
against a host-only Arduino shim. Drive time, raw IMU samples, accepted barometer
altitudes, and health flags; observe flight transitions, GPIO edges and RTC latch.
Do not change production behavior, flash a board, or operate a firing circuit.
Preserve the user's in-progress A/B split and all other worktree changes.

## Implementation

1. Add a failing smoke test for a host simulator, then implement its smallest
   executable harness. Store harness and shim under `firmware/tests/ejection_host`.
2. Add behavioral assertions for auto-arm, nominal ascent/apogee, no-launch pad
   waits, pressure disturbances, launch confirmation, missing sensors, timer
   fallback, single firing, reset recovery, and delayed loop service.
3. Model reset with a fresh process and transfer only the actual latch's public
   values; never reuse module statics to pretend a reboot occurred.
4. Keep ordinary regression checks separate from safety expectations that fail
   on the current firmware. Record reproducible inputs, observations, and limits.
5. Save a readable results report and a command that reruns the evidence.

## Verification

- `python3 -m unittest discover -s firmware/tests -p test_ejection_simulation.py -v`
- `python3 -m unittest discover -s firmware/tests -v`
- Compile both host binaries with warnings and address/undefined sanitizers.
- Review changes and hash the exercised production sources for the report.

## Limits

This is logic simulation, not an ESP32 emulator. It does not validate I2C/BMP280
drivers, health detection latency, electrical output, actual RTC retention,
boot-time GPIO behavior, physical deployment, or flight readiness. Sensor inputs
are synthetic; no claim is made that they reproduce the final OpenRocket model.
Host `unsigned long` width differs from ESP32; timer rollover is out of scope.

## Completed evidence

- Implemented the host harness and 30 simulation test methods for both vehicles.
- Initial smoke test failed because the harness did not exist, then passed.
- Ran unmet safety expectations as normal tests: 12 failures, representing six
  reproduced conditions across A/B. Kept those visible as expected failures;
  `--strict-safety` restores a nonzero safety gate.
- Firmware suite: 44 methods, 32 passing and 12 expected failures.
- Report and source-hashed event evidence: `docs/validation/2026-09-05-ejection-simulation.*`.
- Production firmware unchanged. No flashing or hardware tests performed.
