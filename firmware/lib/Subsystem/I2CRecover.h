// I2CRecover.h -- unwedge a stuck I2C bus.
//
// THE CASCADE THIS FIXES:
// I2C is a shared, open-drain bus. If a slave is interrupted mid-transfer --
// a brownout, a loose SDA line, a reset while it was clocking out a byte -- it
// can be left believing it is still sending, holding SDA LOW indefinitely. A
// held-low SDA is indistinguishable from "bus busy" to every OTHER device, so
// the barometer dying takes the IMU with it, and the next Wire read blocks or
// times out forever. One dead sensor, whole vehicle silent. That is the classic
// "one component stopped and everything stopped" failure.
//
// The fix is in the I2C spec: the master manually clocks SCL until the slave
// finishes the byte it thinks it is sending and releases SDA, then issues a
// STOP to resynchronise the bus.
//
// Use it from a Subsystem init() -- recover first, then re-begin the device:
//
//   static bool baro_init() {
//     if (!i2cBusIdle(PIN_SDA, PIN_SCL)) i2cBusRecover(PIN_SDA, PIN_SCL);
//     return baro.begin(BARO_ADDR);
//   }
//
// ALSO REQUIRED, and not optional: call Wire.setTimeOut(ms) in setup(). Bus
// recovery frees a stuck line, but only the timeout bounds an individual
// transaction. Together they turn "hangs forever" into "returns false".
#pragma once
#include <stdint.h>

// True if SDA is released (bus idle and usable). A false here is the signature
// of a wedged slave -- no amount of retrying Wire will clear it.
bool i2cBusIdle(int sdaPin, int sclPin);

// Manually clock SCL up to `maxClocks` times to walk a stuck slave off SDA,
// then emit a STOP condition. Returns true if the bus came back (SDA released).
//
// Leaves both pins as INPUT_PULLUP. The caller MUST re-run Wire.begin(sda, scl)
// afterwards -- bit-banging the pins takes them away from the I2C peripheral.
bool i2cBusRecover(int sdaPin, int sclPin, uint8_t maxClocks = 9);
