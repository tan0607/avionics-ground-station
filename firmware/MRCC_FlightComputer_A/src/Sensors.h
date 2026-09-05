#pragma once
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "ICM_20948.h"

// =====================================================
// SENSORS - ICM20948 IMU, GPS, supply voltage
//
// Every init returns true/false and NEVER blocks or
// halts. A missing sensor is skipped, not fatal.
// =====================================================

extern TinyGPSPlus    gps;
extern HardwareSerial GPSSerial;
extern ICM_20948_I2C  myICM;

bool initIMU(bool verbose);
void readIMU();

void initGPS();
void readGPS();

void updateGpsSnapshot();
void updateHorizontalVelocity();

void readVbat();
