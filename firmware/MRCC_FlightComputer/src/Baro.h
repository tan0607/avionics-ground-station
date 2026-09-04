#pragma once
#include <Arduino.h>
#include <Adafruit_BMP280.h>

// =====================================================
// BAROMETER - BMP280, the primary apogee sensor
//
// Shares the IMU's I2C bus. ICM20948 is at 0x68, the
// BMP280 at 0x76 or 0x77, so they cannot collide.
//
// Same contract as every other subsystem here: init
// returns a result, never halts, and a dead sensor is
// retried in the background every 5 s.
//
// If this sensor is down, apogee falls back to the
// launch timer. That is a real downgrade in safety,
// so it is announced loudly rather than hidden.
// =====================================================

extern Adafruit_BMP280 bmp;
extern uint8_t         baroAddress;

// Samples the spike gate threw away. Non-zero means the I2C
// link to this sensor is not clean - see Config.h.
extern unsigned long   baroSpikeCount;

// The last value thrown away, so the bench can see the size of
// the thing without watching serial at the moment it happens.
extern float           baroSpikeAlt;

bool initBaro(bool verbose);
void readBaro();
