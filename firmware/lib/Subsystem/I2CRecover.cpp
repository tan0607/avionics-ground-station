#include "I2CRecover.h"
#include <Arduino.h>

// ~100 kHz half-period. Slow on purpose: recovery runs once on a broken bus,
// never in the hot path, and slow edges are kinder to a confused slave.
static const uint8_t HALF_PERIOD_US = 5;

bool i2cBusIdle(int sdaPin, int sclPin) {
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, INPUT_PULLUP);
  delayMicroseconds(HALF_PERIOD_US);
  return digitalRead(sdaPin) == HIGH;
}

bool i2cBusRecover(int sdaPin, int sclPin, uint8_t maxClocks) {
  if (i2cBusIdle(sdaPin, sclPin)) return true;

  // Drive SCL, leave SDA as an input with its pull-up: we are only supplying
  // clock so the stuck slave can finish its byte and let go of SDA. Never drive
  // SDA low here -- that would be indistinguishable from another master.
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT);

  for (uint8_t i = 0; i < maxClocks; i++) {
    digitalWrite(sclPin, LOW);
    delayMicroseconds(HALF_PERIOD_US);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(HALF_PERIOD_US);
    if (digitalRead(sdaPin) == HIGH) break;   // slave let go
  }

  // STOP condition: SDA low->high while SCL is high. Without this the slave can
  // still consider itself mid-transaction on the next start.
  pinMode(sdaPin, OUTPUT);
  digitalWrite(sdaPin, LOW);
  delayMicroseconds(HALF_PERIOD_US);
  pinMode(sclPin, INPUT_PULLUP);              // release SCL high
  delayMicroseconds(HALF_PERIOD_US);
  pinMode(sdaPin, INPUT_PULLUP);              // release SDA high == STOP
  delayMicroseconds(HALF_PERIOD_US);

  return digitalRead(sdaPin) == HIGH;
}
