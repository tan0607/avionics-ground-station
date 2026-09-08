#include "State.h"
#include "esp_system.h"
#include "Flight.h"
#include "soc/rtc.h"
#include "esp_private/esp_clk.h"

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
bool  baroReseeded = false;

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
// RTC memory can retain this across supported warm resets while powered.
// A real power-on starts a new session. A retained fire request is evidence
// of an attempt, not proof that electrical or mechanical deployment completed.
//
// Version, bounds and checksum reject invalid retained records. They do not
// prove RTC power retention or make a torn multiword update atomic.
// =====================================================

// Versioned layout: old images' latches must not validate as this layout.
#define LATCH_MAGIC 0x4D524345UL

RTC_NOINIT_ATTR static volatile uint32_t latchMagicWord;
RTC_NOINIT_ATTR static uint8_t latchStateVal;
RTC_NOINIT_ATTR static uint8_t latchFiredVal;
RTC_NOINIT_ATTR static uint32_t latchLaunchVal;
RTC_NOINIT_ATTR static uint64_t latchLaunchTicks;
RTC_NOINIT_ATTR static uint32_t latchClockCalibration;
RTC_NOINIT_ATTR static uint8_t latchSessionPresent;
RTC_NOINIT_ATTR static uint64_t latchSessionTicks;
RTC_NOINIT_ATTR static uint32_t latchSessionCalibration;
RTC_NOINIT_ATTR static uint32_t latchSessionBootMs;
RTC_NOINIT_ATTR static uint8_t latchDisarmedVal;
RTC_NOINIT_ATTR static uint32_t latchCheck;

static uint32_t latchChecksum() {
  uint32_t hash = 2166136261U;
  const uint32_t words[] = {LATCH_MAGIC, latchStateVal, latchFiredVal,
    latchLaunchVal, static_cast<uint32_t>(latchLaunchTicks),
    static_cast<uint32_t>(latchLaunchTicks >> 32), latchClockCalibration,
    latchSessionPresent, static_cast<uint32_t>(latchSessionTicks),
    static_cast<uint32_t>(latchSessionTicks >> 32), latchSessionCalibration,
    latchSessionBootMs, latchDisarmedVal};
  for (uint32_t word : words) hash = (hash ^ word) * 16777619U;
  return hash;
}

static void invalidateLatch() {
  latchMagicWord = 0;
  __sync_synchronize(); // publish invalidation before changing any payload word
}

static void commitLatch() {
  latchCheck = latchChecksum();
  __sync_synchronize(); // publish payload/checksum before the valid marker
  latchMagicWord = LATCH_MAGIC;
}

void latchBootInit() {
  // .rtc_noinit is deliberately not initialized by startup. A full power-on
  // is always a fresh flight, even if residual RAM happens to pass its checksum.
  if (esp_reset_reason() == ESP_RST_POWERON) latchClear();
}

void latchWrite(uint8_t state, bool fired, unsigned long launchMs) {
  const bool hadRecord = latchValid();
  const bool hadLaunch = hadRecord && latchStateVal >= FS_BOOST;
  const uint64_t ticks = hadLaunch ? latchLaunchTicks : rtc_time_get();
  const uint32_t calibration = hadLaunch ? latchClockCalibration : esp_clk_slowclk_cal_get();
  invalidateLatch();
  if (!hadRecord) {
    // Never checksum uninitialized RTC payload from a cold/invalid record.
    latchSessionPresent = 0;
    latchSessionTicks = 0;
    latchSessionCalibration = 0;
    latchSessionBootMs = 0;
    latchDisarmedVal = 0;
  }
  latchStateVal = state;
  latchFiredVal = fired ? 1 : 0;
  latchLaunchVal = static_cast<uint32_t>(launchMs);
  latchLaunchTicks = state >= FS_BOOST ? ticks : 0;
  latchClockCalibration = state >= FS_BOOST ? calibration : 0;
  commitLatch();
}

bool latchValid() {
  return latchMagicWord == LATCH_MAGIC && latchStateVal <= FS_LANDED &&
         latchFiredVal <= 1 && latchSessionPresent <= 1 && latchDisarmedVal <= 1 &&
         latchCheck == latchChecksum();
}

uint8_t latchState() { return latchValid() ? latchStateVal : 0; }
bool latchFired() { return latchValid() && latchFiredVal != 0; }
unsigned long latchLaunchTime() { return latchValid() ? latchLaunchVal : 0; }

static bool retainedClockElapsed(uint64_t start, uint32_t calibration, uint32_t &elapsedMs) {
  elapsedMs = 0;
  if (!calibration) return false;
  const uint64_t now = rtc_time_get();
  if (now < start) return false;
  const uint64_t ticks = now - start;
  // Check multiplication before the SDK fixed-point conversion. Keep the
  // calibration captured with this anchor: boot may recalibrate the oscillator.
  if (ticks > UINT64_MAX / calibration) return false;
  const uint64_t ms = rtc_time_slowclk_to_us(ticks, calibration) / 1000;
  if (ms > UINT32_MAX) return false;
  elapsedMs = static_cast<uint32_t>(ms);
  return true;
}

bool latchLaunchElapsed(uint32_t &elapsedMs) {
  elapsedMs = 0;
  return latchValid() && latchStateVal >= FS_BOOST &&
         retainedClockElapsed(latchLaunchTicks, latchClockCalibration, elapsedMs);
}

void latchStartPrelaunch(uint32_t bootUptimeMs) {
  if (!latchValid() || latchStateVal > FS_ARMED) return;
  invalidateLatch();
  // Capture current ticks plus matching boot uptime, rather than subtracting
  // uptime from RTC ticks. A reset/recalibration must not move this anchor.
  latchSessionTicks = rtc_time_get();
  latchSessionCalibration = esp_clk_slowclk_cal_get();
  latchSessionBootMs = bootUptimeMs;
  latchSessionPresent = 1;
  commitLatch();
}

bool latchPrelaunchElapsed(uint32_t &elapsedMs) {
  elapsedMs = 0;
  uint32_t sinceAnchor = 0;
  if (!latchValid() || !latchSessionPresent ||
      !retainedClockElapsed(latchSessionTicks, latchSessionCalibration, sinceAnchor) ||
      sinceAnchor > UINT32_MAX - latchSessionBootMs) return false;
  elapsedMs = sinceAnchor + latchSessionBootMs;
  return true;
}

bool latchDisarmed() { return latchValid() && latchDisarmedVal != 0; }

void latchSetDisarmed() {
  if (!latchValid()) return;
  invalidateLatch();
  latchDisarmedVal = 1;
  commitLatch();
}

void latchClear() {
  invalidateLatch();
  latchStateVal = 0;
  latchFiredVal = 0;
  latchLaunchVal = 0;
  latchLaunchTicks = 0;
  latchClockCalibration = 0;
  latchSessionPresent = 0;
  latchSessionTicks = 0;
  latchSessionCalibration = 0;
  latchSessionBootMs = 0;
  latchDisarmedVal = 0;
  latchCheck = 0;
}
