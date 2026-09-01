#pragma once
#include <Arduino.h>

// =====================================================
// SHARED FLIGHT STATE
//
// One place for everything the modules pass around.
// Definitions live in State.cpp.
// =====================================================


// -----------------------------------------------------
// SUBSYSTEM HEALTH
//
// Nothing in this project ever halts. A subsystem that
// fails is marked down here, skipped, and retried in
// the background. The flight continues regardless.
// -----------------------------------------------------

extern bool imuOK;
extern bool radioOK;
extern bool sdOK;
extern bool gpsOK;
extern bool baroOK;

extern unsigned long imuRecoveries;
extern unsigned long radioRecoveries;
extern unsigned long sdRecoveries;
extern unsigned long baroRecoveries;


// -----------------------------------------------------
// IMU - acceleration m/s2, gyro deg/s, mag uT
// -----------------------------------------------------

extern float ax, ay, az;
extern float gx, gy, gz;
extern float mx, my, mz;
extern float heading;

extern unsigned long lastImuUpdate;


// -----------------------------------------------------
// IMU - FILTERED
//
// Every raw channel above has a filtered twin here. Both
// are logged on every line so a run can be replayed
// before and after filtering with no extra flight.
// -----------------------------------------------------

extern float fax, fay, faz;      // despiked + low passed accel, m/s2
extern float fgx, fgy, fgz;      // despiked + low passed + unbiased gyro
extern float fmx, fmy, fmz;      // despiked + low passed mag, uT

extern float accelNormRaw;       // |a| before filtering, m/s2
extern float accelNormFilt;      // |a| after

extern float rollAcc,  pitchAcc;   // accelerometer only, RAW      (before)
extern float rollLpf,  pitchLpf;   // accelerometer, low passed
extern float rollComp, pitchComp;  // complementary filter baseline
extern float rollKal,  pitchKal;   // Kalman                       (after)

extern float headingFilt;        // tilt compensated, deg

extern bool  accelTrusted;       // false while the accel is gated off
extern bool  gyroCalDone;
extern float gyroBiasX, gyroBiasY, gyroBiasZ;

extern float imuHz;              // measured IMU sample rate
extern bool  txFiltered;         // RADIO ONLY: send filtered values.
                                 // Never a flight-logic input - the
                                 // state machine keys off FILTER_ENABLED.



// -----------------------------------------------------
// GPS SNAPSHOT - shared by the log and the radio
// -----------------------------------------------------

extern bool   gpsData;
extern bool   gpsFix;
extern int    satellites;
extern double latitude;
extern double longitude;
extern float  gpsAltitude;
extern float  gpsSpeed;
extern float  gpsCourse;

extern float vx;   // east,  m/s
extern float vy;   // north, m/s

extern unsigned long lastGpsDataTime;


// -----------------------------------------------------
// BAROMETER - the primary apogee sensor
// -----------------------------------------------------

extern float pressure;     // hPa
extern float baroTemp;     // degC
extern float baroAltMSL;   // m, raw, referenced to SEA_LEVEL_HPA

extern unsigned long lastBaroUpdate;


// -----------------------------------------------------
// SUPPLY
// -----------------------------------------------------

extern float vbat;
extern float vbatMin;


// -----------------------------------------------------
// COUNTERS
// -----------------------------------------------------

extern unsigned long packetNumber;
extern unsigned long loopCount;

extern unsigned long bootCount;
extern unsigned long brownoutCount;
extern const char*   resetReasonName;


// -----------------------------------------------------
// FLIGHT LATCH - survives a reset, not a power loss
//
// THIS IS THE SINGLE MOST IMPORTANT SAFETY FEATURE IN
// THE PROJECT.
//
// Pyro shares the main battery. Firing sags the rail,
// a sag can brown out the ESP32, and a brownout reset
// would otherwise bring the board back up in PAD with
// no memory of having fired - and fire again, halfway
// down. The latch is what makes that impossible.
//
// Written on every state change. Read once at boot.
// -----------------------------------------------------

void    latchWrite(uint8_t state, bool fired, unsigned long launchMs);
bool    latchValid();
uint8_t latchState();
bool    latchFired();
unsigned long latchLaunchTime();
void    latchClear();
