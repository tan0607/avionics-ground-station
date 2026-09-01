#include "State.h"
#include "esp_system.h"

bool imuOK   = false;
bool radioOK = false;
bool sdOK    = false;
bool gpsOK   = false;
bool baroOK  = false;

unsigned long imuRecoveries   = 0;
unsigned long radioRecoveries = 0;
unsigned long sdRecoveries    = 0;
unsigned long baroRecoveries  = 0;

float ax = 0.0, ay = 0.0, az = 0.0;
float gx = 0.0, gy = 0.0, gz = 0.0;
float mx = 0.0, my = 0.0, mz = 0.0;
float heading = 0.0;

unsigned long lastImuUpdate = 0;

// Filtered twins - written by Filters.cpp
float fax = 0.0, fay = 0.0, faz = 0.0;
float fgx = 0.0, fgy = 0.0, fgz = 0.0;
float fmx = 0.0, fmy = 0.0, fmz = 0.0;

float accelNormRaw  = 0.0;
float accelNormFilt = 0.0;

float rollAcc  = 0.0, pitchAcc  = 0.0;
float rollLpf  = 0.0, pitchLpf  = 0.0;
float rollComp = 0.0, pitchComp = 0.0;
float rollKal  = 0.0, pitchKal  = 0.0;

float headingFilt = 0.0;

bool  accelTrusted = false;
bool  gyroCalDone  = false;
float gyroBiasX = 0.0, gyroBiasY = 0.0, gyroBiasZ = 0.0;

float imuHz    = 0.0;
bool  txFiltered = true;


bool   gpsData     = false;
bool   gpsFix      = false;
int    satellites  = 0;
double latitude    = 0.0;
double longitude   = 0.0;
float  gpsAltitude = 0.0;
float  gpsSpeed    = 0.0;
float  gpsCourse   = 0.0;

float vx = 0.0;
float vy = 0.0;

unsigned long lastGpsDataTime = 0;

float pressure   = 0.0;
float baroTemp   = 0.0;
float baroAltMSL = 0.0;

unsigned long lastBaroUpdate = 0;

float vbat    = 0.0;
float vbatMin = 99.0;

unsigned long packetNumber = 0;
unsigned long loopCount    = 0;

// RTC memory survives a reset but NOT a full power loss.
RTC_DATA_ATTR unsigned long bootCount     = 0;
RTC_DATA_ATTR unsigned long brownoutCount = 0;

const char* resetReasonName = "UNKNOWN";


// =====================================================
// FLIGHT LATCH
//
// Also in RTC memory. Survives a brownout or watchdog
// reset, cleared by a real power cycle - which is
// exactly the behaviour we want: pull the battery on
// the bench and it forgets; brown out at 200 m and it
// remembers that the charge has already gone.
//
// The magic word plus checksum means uninitialised RTC
// memory can never be mistaken for a real flight.
// =====================================================

#define LATCH_MAGIC 0x4D524343UL   // "MRCC"

RTC_DATA_ATTR static unsigned long latchMagicWord = 0;
RTC_DATA_ATTR static uint8_t       latchStateVal  = 0;
RTC_DATA_ATTR static uint8_t       latchFiredVal  = 0;
RTC_DATA_ATTR static unsigned long latchLaunchVal = 0;
RTC_DATA_ATTR static unsigned long latchCheck     = 0;


static unsigned long latchChecksum() {
  return LATCH_MAGIC ^
         ((unsigned long)latchStateVal * 31UL) ^
         ((unsigned long)latchFiredVal * 131UL) ^
         (latchLaunchVal * 7UL);
}


void latchWrite(uint8_t state, bool fired, unsigned long launchMs) {
  latchMagicWord = LATCH_MAGIC;
  latchStateVal  = state;
  latchFiredVal  = fired ? 1 : 0;
  latchLaunchVal = launchMs;
  latchCheck     = latchChecksum();
}


bool latchValid() {
  if (latchMagicWord != LATCH_MAGIC) return false;
  return latchCheck == latchChecksum();
}


uint8_t latchState() {
  return latchValid() ? latchStateVal : 0;
}


bool latchFired() {
  return latchValid() && latchFiredVal != 0;
}


unsigned long latchLaunchTime() {
  return latchValid() ? latchLaunchVal : 0;
}


void latchClear() {
  latchMagicWord = 0;
  latchStateVal  = 0;
  latchFiredVal  = 0;
  latchLaunchVal = 0;
  latchCheck     = 0;
}
