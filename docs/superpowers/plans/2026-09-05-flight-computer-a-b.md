# Separate flight computer sketches for vehicles A and B

## Goal and scope

Rename the existing sketch to `firmware/MRCC_FlightComputer_A` and copy it to
`firmware/MRCC_FlightComputer_B`. Keep A's configuration and flight logic.
B differs only in `LORA_SCK=47`, `SD_CS=6`, and `VEHICLE=VEHICLE_B`.
Both sketch filenames match their folders; the boot banner identifies the vehicle.
Update active documentation, test paths, editor includes, and the filter replay source path.
Preserve historical plans. No flashing or hardware operation is part of this task.

## Verification

1. Add and run focused failing tests for the two sketches, their effective
   pin/channel configuration, and identical source outside the three settings.
2. Rename/copy the sketches, update paths and instructions, and rerun the tests.
3. Compare A against the original Git source to verify unchanged flight logic.
4. Run `python3 -m unittest discover -s firmware/tests`, `python3 -m backend.tests`,
   and Arduino CLI compile for both with `--fqbn esp32:esp32:esp32s3`.
5. Review the diff, commit, push to `main`, and verify the remote commit.

GPIO6 is also the configured continuity input, but `PYRO_CONT_ENABLED=0`;
preserve that disabled setting in both copies.

## Results

- Focused split tests failed before the sketches existed, then passed.
- Firmware suite: 14 tests passed. Backend suite: 48 tests passed.
- A's 22 source files match the original except the vehicle banner and two comments.
- A/B source parity passes with only the three requested configuration differences.
- Arduino ESP32-S3 compilation passed for both (A: 441002 bytes, B: 441006 bytes;
  each uses 26048 bytes of global memory).
- Filter replay C++ syntax check passed using the renamed A source directory.
- Hardware flashing and bench validation were not performed.
