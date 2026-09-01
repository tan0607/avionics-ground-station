#include "Flight.h"
#include "Config.h"
#include "State.h"
#include "Pyro.h"
#include "Filters.h"
#include <math.h>

uint8_t flightState = FS_PAD;

float groundAlt   = 0.0;
float altAGL      = 0.0;
float altFiltered = 0.0;
float vertVel     = 0.0;
float maxAlt      = 0.0;
float apogeeAlt   = 0.0;

float accelMag = 0.0;
float gyroMag  = 0.0;

unsigned long launchTime = 0;
unsigned long apogeeTime = 0;

bool timerBackupUsed = false;

static unsigned long lastFlightTick = 0;
static unsigned long stillSince     = 0;
static unsigned long burnoutSince   = 0;
static unsigned long landRefTime    = 0;

static float   landRef        = 0.0;
static uint8_t launchSamples  = 0;
static uint8_t apogeeSamples  = 0;
static bool    filterPrimed   = false;
static bool    groundPrimed   = false;


static void enterState(uint8_t s);
static void updateAltitude(float dt);
static void updateMagnitudes();


const char* stateName(uint8_t s) {
  switch (s) {
    case FS_PAD:     return "PAD";
    case FS_ARMED:   return "ARMED";
    case FS_BOOST:   return "BOOST";
    case FS_COAST:   return "COAST";
    case FS_APOGEE:  return "APOGEE";
    case FS_DESCENT: return "DESCENT";
    case FS_LANDED:  return "LANDED";
    default:         return "?";
  }
}


// =====================================================
// INIT
//
// Recovers from the RTC latch. A board that resets in
// flight must come back knowing where it was - and, if
// the charge has already gone, knowing never to fire
// again.
//
// One honest caveat: millis() restarts at zero after a
// reset, so the recovered launch time is meaningless.
// The timer backup is therefore restarted from the
// reset, which makes it LATE. The barometer stays the
// primary detector across a reset, which is the real
// reason it is worth having.
// =====================================================

void initFlight(bool verbose) {
  flightState = FS_PAD;

  if (!latchValid()) {
    if (verbose) Serial.println("[FLIGHT] Clean start - state PAD");
    latchWrite(FS_PAD, false, 0);
    return;
  }

  uint8_t saved = latchState();

  if (saved <= FS_ARMED) {
    if (verbose) Serial.println("[FLIGHT] Latch says pre-launch - state PAD");
    latchWrite(FS_PAD, false, 0);
    return;
  }

  // We were in the air when the board restarted.
  Serial.println();
  Serial.println("****************************************");
  Serial.print  ("* IN-FLIGHT RESET. Latched state was ");
  Serial.println(stateName(saved));

  if (latchFired()) {
    flightState = FS_DESCENT;
    apogeeTime  = millis();

    Serial.println("* The charge has ALREADY been fired.");
    Serial.println("* Coming up in DESCENT, pyro stays SAFE.");
  }
  else {
    flightState = saved;
    launchTime  = millis();          // see caveat above
    pyroArmed   = true;              // it is in the air - do NOT ask

    Serial.println("* The charge has NOT fired yet.");
    Serial.println("* Re-arming automatically and continuing.");
    Serial.println("* Timer backup restarted from this reset.");
  }

  Serial.println("****************************************");
  Serial.println();

  latchWrite(flightState, latchFired(), launchTime);
}


static void enterState(uint8_t s) {
  flightState = s;
  latchWrite(s, pyroFired, launchTime);

  Serial.print("[FLIGHT] -> ");
  Serial.print(stateName(s));
  Serial.print("  t=");
  Serial.print(millis() / 1000.0, 2);
  Serial.print("s  alt=");
  Serial.print(altFiltered, 1);
  Serial.print("m  v=");
  Serial.print(vertVel, 1);
  Serial.println("m/s");
}


// =====================================================
// ARM / DISARM
// =====================================================

bool armFlight() {
  if (flightState != FS_PAD) {
    Serial.print("[FLIGHT] ARM REFUSED - state is ");
    Serial.println(stateName(flightState));
    return false;
  }

  if (!imuOK && !baroOK) {
    Serial.println("[FLIGHT] ARM REFUSED - no IMU and no baro");
    return false;
  }

  if (imuOK && (millis() - stillSince < PAD_STILL_TIME)) {
    Serial.print("[FLIGHT] ARM REFUSED - not settled, need ");
    Serial.print((PAD_STILL_TIME - (millis() - stillSince)) / 1000.0, 1);
    Serial.println(" s more of stillness");

    // An uncalibrated gyro bias sits in gyroMag forever
    // and would look exactly like a rocket that never
    // stops moving. Say so rather than let someone stand
    // at the pad pressing A.
    if (!gyroCalDone) {
      Serial.print("[FLIGHT] Gyro is NOT zeroed (|g|=");
      Serial.print(gyroMag, 1);
      Serial.println(" deg/s) - run K with the board still");
    }
    return false;
  }

  if (!armPyro()) return false;

  groundAlt   = baroAltMSL;
  altAGL      = 0.0;
  altFiltered = 0.0;
  vertVel     = 0.0;
  maxAlt      = 0.0;

  launchSamples = 0;
  apogeeSamples = 0;
  filterPrimed  = true;

  if (!baroOK) {
    Serial.println("[FLIGHT] *** WARNING - BARO IS DOWN ***");
    Serial.println("[FLIGHT] Apogee will be TIMER ONLY.");
  }

  enterState(FS_ARMED);
  return true;
}


void disarmFlight() {
  disarmPyro();

  if (flightState == FS_ARMED) {
    enterState(FS_PAD);
  }
}


// =====================================================
// SENSOR MAGNITUDES
//
// Vector magnitudes, never a single axis. Nothing in
// this project tracks which way the board is mounted.
//
// These come from the FILTERED channels, not the raw
// ones. That is the whole point of Filters.*:
//
//   - the raw accelerometer picks up airframe and motor
//     vibration, and a single vibration spike past 3 g
//     would otherwise be enough to declare a launch;
//   - the raw gyro carries a zero-rate offset, which
//     would sit in the stillness check forever and stop
//     the board ever arming.
//
// The despike + low pass has group delay of a few tens
// of ms at these cutoffs. Against a MIN_COAST_TIME of
// 1.5 s that is irrelevant, and false triggering is the
// far more expensive failure.
//
// If the filter is switched off we fall back to raw, so
// the state machine still works either way.
// =====================================================

static void updateMagnitudes() {
#if FILTER_ENABLED
  accelMag = accelNormFilt;
  gyroMag  = sqrt(fgx * fgx + fgy * fgy + fgz * fgz);
#else
  accelMag = accelNormRaw;
  gyroMag  = sqrt(gx * gx + gy * gy + gz * gz);
#endif
}


// =====================================================
// ALTITUDE - alpha-beta filter
//
// Predict forward on the current velocity, correct on
// the new measurement. Gives a usable vertical velocity
// without needing orientation, which we do not have.
// =====================================================

static void updateAltitude(float dt) {
  if (!baroOK || dt <= 0.0) return;

  altAGL = baroAltMSL - groundAlt;

  if (!filterPrimed) {
    altFiltered  = altAGL;
    vertVel      = 0.0;
    filterPrimed = true;
    return;
  }

  float predAlt = altFiltered + vertVel * dt;
  float resid   = altAGL - predAlt;

  altFiltered = predAlt + FILTER_ALPHA * resid;
  vertVel     = vertVel + (FILTER_BETA / dt) * resid;

  if (altFiltered > maxAlt) maxAlt = altFiltered;
}


// =====================================================
// SERVICE - 20 Hz
// =====================================================

void serviceFlight() {
  if (millis() - lastFlightTick < FLIGHT_INTERVAL) return;

  float dt = (millis() - lastFlightTick) / 1000.0;
  lastFlightTick = millis();

  if (dt > 0.5) dt = FLIGHT_INTERVAL / 1000.0;   // first tick, or a stall

  updateMagnitudes();
  updateAltitude(dt);

  switch (flightState) {

    // -------------------------------------------------
    // PAD - track stillness and the ground reference
    // -------------------------------------------------
    case FS_PAD: {
      bool still = imuOK &&
                   fabs(accelMag - GRAVITY) < PAD_ACCEL_TOL &&
                   gyroMag < PAD_GYRO_TOL;

      if (!still) stillSince = millis();

      // Slowly follow the weather while we sit there.
      //
      // Primed on the first real sample. Slewing up from
      // zero instead would take ~15 s to converge, and
      // anyone who armed during that window would get a
      // ground reference hundreds of metres out - which
      // feeds straight into the MIN_ALT_GAIN fire gate.
      if (baroOK) {
        if (!groundPrimed) {
          groundAlt    = baroAltMSL;
          groundPrimed = true;
        }
        else {
          groundAlt = groundAlt * 0.99 + baroAltMSL * 0.01;
        }
      }
      break;
    }

    // -------------------------------------------------
    // ARMED - waiting for the motor
    // -------------------------------------------------
    case FS_ARMED: {
      bool launched = false;

      if (imuOK && accelMag > LAUNCH_ACCEL) {
        launchSamples++;
        if (launchSamples >= LAUNCH_CONFIRM) launched = true;
      }
      else {
        launchSamples = 0;
      }

      // Baro fallback, in case the IMU died on the pad
      if (!launched && baroOK && altFiltered > LAUNCH_ALT && vertVel > 5.0) {
        launched = true;
        Serial.println("[FLIGHT] Launch detected by BARO (IMU unavailable)");
      }

      if (launched) {
        launchTime   = millis();
        burnoutSince = millis();
        enterState(FS_BOOST);
      }
      break;
    }

    // -------------------------------------------------
    // BOOST - under thrust
    // -------------------------------------------------
    case FS_BOOST: {
      if (!imuOK || accelMag < BURNOUT_ACCEL) {
        if (millis() - burnoutSince >= BURNOUT_CONFIRM) {
          enterState(FS_COAST);
        }
      }
      else {
        burnoutSince = millis();
      }

      if (millis() - launchTime > MOTOR_BURN_MAX) {
        Serial.println("[FLIGHT] Burn time exceeded - forcing COAST");
        enterState(FS_COAST);
      }
      break;
    }

    // -------------------------------------------------
    // COAST - this is where the charge goes
    //
    // Three independent guards. All three must pass.
    // -------------------------------------------------
    case FS_COAST: {
      bool timeOK = (millis() - launchTime) >= MIN_COAST_TIME;
      bool altOK  = baroOK && (maxAlt >= MIN_ALT_GAIN);

      if (baroOK && vertVel < APOGEE_VEL) apogeeSamples++;
      else                                apogeeSamples = 0;

      bool falling = (apogeeSamples >= APOGEE_CONFIRM);

      if (timeOK && altOK && falling) {
        enterState(FS_APOGEE);
        break;
      }

      // Backup. Fires on the clock if the baro never
      // called it - a dead sensor must not mean a
      // ballistic recovery.
      if (timeOK && (millis() - launchTime) >= APOGEE_TIMEOUT) {
        Serial.println("[FLIGHT] *** APOGEE TIMER BACKUP ***");
        timerBackupUsed = true;
        enterState(FS_APOGEE);
      }
      break;
    }

    // -------------------------------------------------
    // APOGEE - fire, then move straight on. The pulse
    // itself is timed by servicePyro().
    // -------------------------------------------------
    case FS_APOGEE: {
      apogeeAlt  = altFiltered;
      apogeeTime = millis();

      firePyro(timerBackupUsed ? "TIMER BACKUP" : "APOGEE");

      landRef     = altFiltered;
      landRefTime = millis();

      enterState(FS_DESCENT);
      break;
    }

    // -------------------------------------------------
    // DESCENT - under the chute, watching for the ground
    // -------------------------------------------------
    case FS_DESCENT: {
      // With the baro down this is an accelerometer-only
      // test, which under a chute will call LANDED early.
      // The charge has already gone by this point, so the
      // consequence is a cosmetic log entry, not a safety
      // one - but do not read a no-baro LANDED as truth.
      bool moving = false;

      if (baroOK && fabs(altFiltered - landRef) > LAND_ALT_BAND) moving = true;
      if (imuOK  && fabs(accelMag - GRAVITY) > 3.0)                 moving = true;

      if (moving) {
        landRef     = altFiltered;
        landRefTime = millis();
      }
      else if (millis() - landRefTime >= LAND_CONFIRM) {
        enterState(FS_LANDED);
      }
      break;
    }

    // -------------------------------------------------
    // LANDED - safe everything, once
    // -------------------------------------------------
    case FS_LANDED: {
      if (pyroArmed) {
        Serial.println("[FLIGHT] Landed - safing the pyro channel");
        disarmPyro();
      }
      break;
    }
  }
}
