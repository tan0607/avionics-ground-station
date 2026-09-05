# Remove unused arm-switch sensing

## Goal

Keep the existing launch procedure unchanged and remove the unused arm-switch
sensing experiment. The vehicle still auto-arms after startup and requires no
serial input. The physical MOSFET/pyro switch remains the hardware interlock.

Restore the original MRCC telemetry fields and precision:

- no `SW` field;
- GPS altitude (`GA`) at one decimal place;
- accelerometer fields (`AX/AY/AZ`) at two decimal places.

## Scope

- Restore the original arm-switch configuration and pyro implementation.
- Remove arm-switch output from the local console/status display.
- Remove the unused Stage 4 switch monitor from `Pyro_Doctor` while preserving
  its existing MOSFET bench tests.
- Keep the unrelated recorder-status work (`SDF/SDL/SDE`) intact.
- Do not change `Flight.cpp` or the auto-arm state machine.

## Expected files

- `firmware/MRCC_FlightComputer/src/Config.h`
- `firmware/MRCC_FlightComputer/src/Console.cpp`
- `firmware/MRCC_FlightComputer/src/Health.cpp`
- `firmware/MRCC_FlightComputer/src/Pyro.cpp`
- `firmware/MRCC_FlightComputer/src/Pyro.h`
- `firmware/Pyro_Doctor/Pyro_Doctor.ino`
- `firmware/tests/test_arm_switch.py`
- `firmware/README.md`

## Verification

- `python3 firmware/tests/test_arm_switch.py`
- `python3 -m backend.tests`
- `npx tsc --noEmit` in `dashboard/`
- Compile both firmware sketches for `esp32:esp32:esp32s3`.
- Inspect the final diff to confirm recorder-status changes remain and no arm
  switch field or reduced-precision telemetry remains.
