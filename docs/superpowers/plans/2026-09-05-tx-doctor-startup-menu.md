# TX Doctor startup-menu plan

## Goal

Let `TX_Doctor` finish booting and accept menu commands without automatically
running Radio Test 1. Keep Radio Test 1 available on key `1`, so key `8` can be
used immediately for the sensor/BMP diagnostic.

## Scope

- Change only the `TX_Doctor` startup flow.
- Do not change the LoRa pins, SPI restoration, I2C logic, Flight Computer, or
  Ground Station.

## Expected files

- `firmware/TX_Doctor/TX_Doctor.ino`
- `firmware/tests/test_tx_doctor_startup.py`

## Verification

- `python3 firmware/tests/test_tx_doctor_startup.py`
- Compile `firmware/TX_Doctor` for `esp32:esp32:esp32s3` when `arduino-cli` is
  available.
- Inspect the final diff to confirm no other production behavior changed.
