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
static unsigned long lastStillImuUpdate = 0;
static bool stillWindowActive = false;
static bool bootDelayComplete = false;
static unsigned long burnoutSince   = 0;
static unsigned long landRefTime    = 0;
static unsigned long padSince       = 0;
static unsigned long lastAutoArmTry = 0;
static unsigned long lastStuckReport = 0;
static unsigned long lastLaunchImuUpdate = 0;
static unsigned long lastAltitudeBaroUpdate = 0;
static bool altitudeSampleSeen = false;

static float   landRef        = 0.0;
static uint8_t launchSamples  = 0;
static uint8_t apogeeSamples  = 0;
static bool    filterPrimed   = false;
static bool    groundPrimed   = false;
static bool    autoArmBlocked = false;

// Diagnostic history only: preserve the sample that reset the pad timer.
// A status print several seconds later may show healthy values again.
static const char* padResetReason = "NONE";
static unsigned long padResetTime = 0;
static float padResetAccel = 0.0;
static float padResetGyro = 0.0;


static void enterState(uint8_t s);
static bool updateAltitude();
static void updateMagnitudes();
static void announceAutoArm();
static void reportAutoArmStuck();
static void tryAutoArm();


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

// A board that arms without being asked must say so at
// boot. Someone who does not know it is coming will put
// the thing down on the bench, walk off, and leave a
// live state machine behind them.
static void announceAutoArm() {
#if AUTO_ARM_ENABLED
  Serial.print("[FLIGHT] AUTO-ARM is ON - minimum boot wait ");
  Serial.print(AUTO_ARM_DELAY / 1000);
  Serial.print(" s AND ");
  Serial.print(PAD_STILL_TIME / 1000);
  Serial.println(" s of stillness");
  Serial.println("[FLIGHT] X disarms it until the next power cycle");
#else
  Serial.println("[FLIGHT] AUTO-ARM is OFF - press A to arm");
#endif
}


void initFlight(bool verbose) {
  flightState = FS_PAD;
  padSince    = millis();
  stillWindowActive = false;
  lastStillImuUpdate = 0;
  bootDelayComplete = millis() >= AUTO_ARM_DELAY;

  if (!latchValid()) {
    if (verbose) {
      Serial.println("[FLIGHT] Clean start - state PAD");
      announceAutoArm();
    }
    latchWrite(FS_PAD, false, 0);
    return;
  }

  uint8_t saved = latchState();

  if (saved <= FS_ARMED) {
    if (verbose) {
      Serial.println("[FLIGHT] Latch says pre-launch - state PAD");
      announceAutoArm();
    }
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

ArmReadiness armReadiness() {
  if (flightState != FS_PAD) return {0, 0, 0};
  const unsigned long now = millis();
  const bool imuUsable = imuOK && lastImuUpdate != 0 &&
    now - lastImuUpdate <= PAD_IMU_MAX_GAP &&
    isfinite(accelMag) && isfinite(gyroMag);
  const unsigned long delayLeft = bootDelayComplete || now >= AUTO_ARM_DELAY
    ? 0 : AUTO_ARM_DELAY - now;
  // Advance only to the last OBSERVED sample, never along a held value.
  const unsigned long observed = imuUsable && stillWindowActive
    ? lastStillImuUpdate - stillSince : 0;
  const unsigned long stillLeft = observed >= PAD_STILL_TIME ? 0 : PAD_STILL_TIME - observed;
  uint8_t wait = 0;
  if (delayLeft) wait |= ARM_WAIT_DELAY;
  if (stillLeft) wait |= ARM_WAIT_STILL;
  if (!gyroCalDone) wait |= ARM_WAIT_CAL;
  if (!imuUsable) wait |= ARM_WAIT_IMU;
  if (autoArmBlocked) wait |= ARM_WAIT_BLOCKED;
  if (!AUTO_ARM_ENABLED) wait |= ARM_WAIT_DISABLED;
  if (pyroFired) wait |= ARM_WAIT_FIRED;
  if (!armSwitchClosed() || (PYRO_CONT_ENABLED && !contOK))
    wait |= ARM_WAIT_INTERLOCK;
  return {wait, delayLeft, stillLeft};
}

bool armFlight() {
  if (flightState != FS_PAD) {
    Serial.print("[FLIGHT] ARM REFUSED - state is ");
    Serial.println(stateName(flightState));
    return false;
  }

  if (armReadiness().wait & ~ARM_WAIT_DISABLED) {
    printArmReadiness();
    return false;
  }


  if (!armPyro()) return false;

  // Arming re-zeros the altitude on the spot: this is the reference
  // the whole flight is measured against, and the one MIN_ALT_GAIN
  // gates the charge on, so it is worth a line in the log. The pad
  // has usually drifted a few tenths of a metre since boot.
  if (baroOK && groundPrimed) {
    Serial.print("[BARO] Re-zeroed at arm: ground reference = ");
    Serial.print(baroAltMSL, 1);
    Serial.print(" m MSL (was ");
    Serial.print(groundAlt, 1);
    Serial.println(")");
  }

  groundAlt   = baroAltMSL;
  altAGL      = 0.0;
  altFiltered = 0.0;
  vertVel     = 0.0;
  maxAlt      = 0.0;

  launchSamples = 0;
  lastLaunchImuUpdate = lastImuUpdate; // pad sample is already consumed
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

  // A deliberate disarm has to STICK. Without this the
  // auto-arm below walks the board straight back to ARMED
  // PAD_STILL_TIME later, which makes X look broken and
  // permanently blocks the bench test path - testFirePyro()
  // refuses while armed, so T would never get a window.
  //
  // Only a power cycle clears it. That is the point: on the
  // pad, "I disarmed this" must not quietly expire.
  autoArmBlocked = true;

  if (flightState == FS_ARMED) {
    enterState(FS_PAD);
  }
}


// =====================================================
// AUTO ARM - FS_PAD only
//
// Calls the same armFlight() the A key does. Every gate
// that function enforces is still enforced here; this
// only supplies the keystroke nobody is at the pad to
// press.
//
// A failed IMU cannot prove stillness and therefore cannot auto-arm.
// Readiness is shared with telemetry; zero blockers is not an ACK.
// Gyro calibration retries only in unarmed PAD after a timeout.
//
// Testing settled BEFORE calling armFlight() is what keeps
// the log readable: the ordinary "not settled yet" refusal
// would otherwise print 20 times a second for the entire
// pad wait and bury everything else. The refusals that get
// past it are the ones worth reading, and they are rate
// limited to one per AUTO_ARM_RETRY.
// =====================================================

// Printed with regular status and S, not on every flight tick.
// Last-reset values are historical, not the current sensor readings.
void printArmReadiness() {
  if (flightState != FS_PAD) return;

  Serial.print("[ARM] auto=");
  Serial.print(AUTO_ARM_ENABLED ? "ON" : "OFF");
  Serial.print(" | blocked_by_X=");
  Serial.print(autoArmBlocked ? "YES" : "NO");
  Serial.print(" | fired=");
  Serial.print(pyroFired ? "YES" : "NO");
  Serial.print(" | gyro_cal=");
  Serial.print(gyroCalDone ? "DONE" : "PENDING");
  const ArmReadiness ready = armReadiness();
  Serial.print(" | boot_wait_remaining=");
  Serial.print(ready.delayRemainingMs / 1000.0, 2);
  Serial.print("s | still_remaining=");
  Serial.print(ready.stillRemainingMs / 1000.0, 2);
  Serial.print("s | wait_mask=");
  Serial.print(ready.wait);
  Serial.print("s | last_reset=");
  Serial.print(padResetReason);
  if (padResetTime != 0) {
    Serial.print(" age=");
    Serial.print((millis() - padResetTime) / 1000.0, 2);
    Serial.print("s a=");
    Serial.print(padResetAccel, 3);
    Serial.print("m/s2 g=");
    Serial.print(padResetGyro, 3);
    Serial.print("deg/s");
  }
  Serial.println();
}

// Explain a settle failure persisting for AUTO_ARM_STUCK_AFTER.
static void reportAutoArmStuck() {
  Serial.println();
  Serial.println("[FLIGHT] *** NOT ARMED YET ***");
  Serial.print  ("[FLIGHT] In PAD for ");
  Serial.print((millis() - padSince) / 1000);
  Serial.println(" s - the settle test has not passed.");
  printArmReadiness();

  if (!imuOK) {
    Serial.println("[FLIGHT] IMU is DOWN - auto-arm inhibited; no baro-only arming.");
    if (!baroOK) {
      Serial.println("[FLIGHT] Baro is DOWN too. NOTHING can arm this board.");
    }
    return;
  }

  if (!gyroCalDone) {
    Serial.println("[FLIGHT] Gyro is NOT zeroed - hold still; calibration retries in PAD.");
  }

  Serial.print("[FLIGHT] |a| = ");
  Serial.print(accelMag, 2);
  Serial.print(" m/s2   needs ");
  Serial.print(GRAVITY - PAD_ACCEL_TOL, 2);
  Serial.print(" .. ");
  Serial.println(GRAVITY + PAD_ACCEL_TOL, 2);

  Serial.print("[FLIGHT] |g| = ");
  Serial.print(gyroMag, 1);
  Serial.print(" deg/s   needs under ");
  Serial.println(PAD_GYRO_TOL, 1);

  if (gyroMag < PAD_GYRO_TOL && fabs(accelMag - GRAVITY) >= PAD_ACCEL_TOL) {
    Serial.println("[FLIGHT] Acceleration is outside the pad window.");
    Serial.println("[FLIGHT] Check mounting, vibration and sensor readings");
    Serial.println("[FLIGHT] before changing calibration or the limits.");
  }
}


static void tryAutoArm() {
  if (autoArmBlocked || pyroFired)                  return;
  if (millis() - lastAutoArmTry < AUTO_ARM_RETRY)   return;

  bool settled = armReadiness().wait == 0;

  if (!settled) {
    if (bootDelayComplete &&
        millis() - padSince        >= AUTO_ARM_STUCK_AFTER &&
        millis() - lastStuckReport >= AUTO_ARM_STUCK_AFTER) {
      lastStuckReport = millis();
      reportAutoArmStuck();
    }
    return;
  }

  lastAutoArmTry = millis();

  if (armFlight()) {
    Serial.println("[FLIGHT] AUTO-ARMED - no key was pressed");
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

static bool updateAltitude() {
  const unsigned long sampleTime = lastBaroUpdate;
  if (!baroOK || pressure <= 0.0 || millis() - sampleTime > BARO_STALE) {
    // Missing/expired data cannot supply descent evidence. Re-prime when
    // readings recover rather than extrapolate velocity across the outage.
    filterPrimed = false;
    apogeeSamples = 0;
    return false;
  }
  if (altitudeSampleSeen && sampleTime == lastAltitudeBaroUpdate) return false;

  const unsigned long sampleDt = sampleTime - lastAltitudeBaroUpdate;
  if (!altitudeSampleSeen || sampleDt > BARO_STALE) {
    // Also catches an outage hidden by a loop stall followed by a new sample.
    filterPrimed = false;
    apogeeSamples = 0;
  }
  altitudeSampleSeen = true;
  lastAltitudeBaroUpdate = sampleTime;
  const float dt = sampleDt / 1000.0f;


  // A barometer reads ALTITUDE ABOVE SEA LEVEL for a fixed
  // SEA_LEVEL_HPA, so on the pad it reports the pad's elevation for
  // the day's weather - tens or hundreds of metres, never 0. The
  // ground reference is what turns that into "0 m on the pad", and
  // it has to exist BEFORE the first AGL sample, not one tick after.
  //
  // It used to be primed in the FS_PAD block below, which runs AFTER
  // this function. So the first sample went through as a raw MSL
  // reading and the alpha-beta filter treated the correction on the
  // NEXT tick as a real 45 m drop in 50 ms: altitude rang from +45 m
  // down through -7 m, vertical velocity spiked past -90 m/s, and
  // maxAlt latched ~31 m before anyone touched the arm key. It
  // settled after ~2 s, but maxAlt kept the bogus peak and MX
  // downlinked it until the next arm.
  //
  // pressure > 0 is the test for "a real reading has landed":
  // readBaro() sets it only after rejecting the zero/NaN a dead
  // sensor returns. baroOK alone is not enough - initBaro() succeeds
  // a full loop pass before the first sample, and priming off
  // baroAltMSL's initial 0.0 would rebuild the same bug.
  if (!groundPrimed && pressure > 0.0) {
    if (flightState <= FS_ARMED) {
      groundAlt = baroAltMSL;

      Serial.print("[BARO] Ground reference = ");
      Serial.print(groundAlt, 1);
      Serial.println(" m MSL - altitude now reads 0 m on the pad");
    }
    else {
      // The barometer came up IN THE AIR - down at boot, recovered
      // mid flight. There is no pad reading to reference, and taking
      // the current one would declare this altitude zero, which is
      // the number MIN_ALT_GAIN gates the charge on: the deployment
      // would be locked out for the rest of the flight.
      //
      // So fall back to raw MSL. The altitude then reads high by the
      // site elevation, which is wrong but honest and monotonic, and
      // vertVel is a difference so it is unaffected either way.
      groundAlt = 0.0;

      Serial.println("[BARO] RECOVERED IN FLIGHT - no pad reference.");
      Serial.println("[BARO] ALT is MSL, not AGL. VZ is still good.");
    }

    groundPrimed = true;

    // Re-seed the alpha-beta below rather than feed it a step of one
    // whole site elevation, which is what rang the pad readout for
    // two seconds and left a bogus maxAlt behind it.
    filterPrimed = false;
  }

  // The barometer gave up arguing with a reading it had been
  // rejecting and re-seeded on it. That is a step, not a
  // measurement: fed through the residual below it becomes
  // thousands of m/s and then thousands negative on the next
  // sample, which satisfies APOGEE_VEL many times over. Same
  // treatment as a ground reference that has just moved -
  // re-prime, do not integrate.
  //
  // The altitude is now wrong by whatever the sensor decided,
  // and left that way on purpose. Apogee is called on velocity,
  // which is a difference, so a constant offset still finds the
  // top - and re-zeroing maxAlt here could withhold MIN_ALT_GAIN
  // for the rest of a flight that has already passed its peak.
  if (baroReseeded) {
    baroReseeded = false;
    filterPrimed = false;
    apogeeSamples = 0;
    Serial.println("[FLIGHT] Baro re-seeded - re-priming the altitude filter");
  }

  altAGL = baroAltMSL - groundAlt;

  if (!filterPrimed) {
    altFiltered  = altAGL;
    vertVel      = 0.0;
    filterPrimed = true;
    return true;
  }

  float predAlt = altFiltered + vertVel * dt;
  float resid   = altAGL - predAlt;

  altFiltered = predAlt + FILTER_ALPHA * resid;
  vertVel     = vertVel + (FILTER_BETA / dt) * resid;

  if (altFiltered > maxAlt) maxAlt = altFiltered;
  return true;
}


// =====================================================
// SERVICE - 20 Hz
// =====================================================

void serviceFlight() {
  if (millis() - lastFlightTick < FLIGHT_INTERVAL) return;

  lastFlightTick = millis();

  updateMagnitudes();
  const bool newAltitude = updateAltitude();

  switch (flightState) {

    // -------------------------------------------------
    // PAD - track stillness and the ground reference
    // -------------------------------------------------
    case FS_PAD: {
      if (millis() >= AUTO_ARM_DELAY) bootDelayComplete = true;
      const bool usable = imuOK && lastImuUpdate != 0 &&
        millis() - lastImuUpdate <= PAD_IMU_MAX_GAP &&
        isfinite(accelMag) && isfinite(gyroMag);
      const bool newImu = usable && lastImuUpdate != lastStillImuUpdate;
      const bool gap = newImu && lastStillImuUpdate != 0 &&
        lastImuUpdate - lastStillImuUpdate > PAD_IMU_MAX_GAP;
      bool still = usable &&
                   fabs(accelMag - GRAVITY) < PAD_ACCEL_TOL &&
                   gyroMag < PAD_GYRO_TOL;

      if (!still || gap) {
        stillWindowActive = false;
        stillSince = millis();
        padResetTime = stillSince;
        padResetAccel = accelMag;
        padResetGyro = gyroMag;
        const bool accelRejected = !(fabs(accelMag - GRAVITY) < PAD_ACCEL_TOL);
        const bool gyroRejected = !(gyroMag < PAD_GYRO_TOL);
        padResetReason = !usable ? "IMU_DOWN_OR_STALE" : gap ? "IMU_GAP" :
                         accelRejected && gyroRejected ? "ACCEL+GYRO" :
                         accelRejected ? "ACCEL" : "GYRO";
      }
      if (newImu) {
        lastStillImuUpdate = lastImuUpdate;
        if (still && !stillWindowActive) {
          stillSince = lastImuUpdate;
          stillWindowActive = true;
        }
        // No calibration restart once armed/in flight. A timeout during
        // installation must not permanently strand an unattended vehicle.
        if (!pyroArmed && !autoArmBlocked && !gyroCalDone && !gyroCalibrating()) {
          startGyroCal();
        }
      }

      // Slowly follow the weather while we sit there.
      //
      // Priming lives in updateAltitude() now, which runs before
      // this and is the only place that can prime it in time. What
      // is left here is the slow drift track, which must NOT run
      // until there is a reference to drift from - slewing up from
      // zero would take ~15 s, and anyone who armed inside that
      // window would get a ground reference hundreds of metres out,
      // which feeds straight into the MIN_ALT_GAIN fire gate.
      if (newAltitude && groundPrimed) {
        groundAlt = groundAlt * 0.99 + baroAltMSL * 0.01;
      }

      // Last in the block: armFlight() re-zeros the ground
      // reference off groundAlt, so let the drift track above
      // land this tick's sample first.
#if AUTO_ARM_ENABLED
      tryAutoArm();
#endif
      break;
    }

    // -------------------------------------------------
    // ARMED - waiting for the motor
    // -------------------------------------------------
    case FS_ARMED: {
      bool launched = false;

      // The loop may run many times without readIMU() receiving a sample.
      // Count each timestamp at most once, still at the 50 ms flight cadence.
      const unsigned long imuSampleTime = lastImuUpdate;
      const bool imuUsable = imuOK &&
                            (millis() - imuSampleTime <= IMU_STALE);
      const bool newLaunchImu = imuUsable &&
                               (imuSampleTime != lastLaunchImuUpdate);

      // Also break the run if a fresh sample arrives after a long loop stall:
      // its own age is small, but the earlier confirmations have expired.
      if (!imuUsable || millis() - lastLaunchImuUpdate > IMU_STALE) {
        launchSamples = 0;
      }

      if (newLaunchImu) {
        if (accelMag > LAUNCH_ACCEL) {
          launchSamples++;
          if (launchSamples >= LAUNCH_CONFIRM) launched = true;
        }
        else {
          launchSamples = 0;
        }
      }
      // Missing but not stale: hold the count. Invalid data is consumed too,
      // so changing a health flag alone cannot turn it into new evidence.
      lastLaunchImuUpdate = imuSampleTime;

      // Baro fallback, in case the IMU died on the pad.
      //
      // The IMU test is the point of this clause and it used to be
      // missing: the comment said "in case the IMU died" but the
      // condition never asked whether it had, so a live IMU sitting
      // flat and insisting nothing had moved could not veto a launch
      // called on a pressure step alone.
      //
      // That is not a theoretical hole on this range. The vehicle
      // waits ARMED on the rail for hours while nine other rockets
      // fly, and 15 m of LAUNCH_ALT is only ~1.8 hPa - inside what a
      // gust across imperfect static ports, or a neighbouring motor,
      // can produce. A launch called there is not a late deployment;
      // APOGEE_TIMEOUT has no altitude gate, so the charge fires 19 s
      // later, on the pad, with people on the range.
      //
      // So ask for corroboration. A real launch always carries
      // acceleration, and BURNOUT_ACCEL is a low bar it clears by a
      // wide margin, so nothing legitimate is refused. If the IMU is
      // genuinely down the clause falls back to exactly what it did
      // before, which is what it was written for.
      bool imuAgrees = !imuOK ||
                       (newLaunchImu && accelMag > BURNOUT_ACCEL);

      if (!launched && newAltitude && imuAgrees &&
          altFiltered > LAUNCH_ALT && vertVel > 5.0) {
        launched = true;
        Serial.print("[FLIGHT] Launch detected by BARO (IMU ");
        Serial.println(imuOK ? "agrees)" : "unavailable)");
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

      // Each accepted sample counts once. Ordinary gaps hold the run;
      // expiry, sensor failure and re-seeding clear it in updateAltitude().
      if (newAltitude) {
        if (vertVel < APOGEE_VEL) apogeeSamples++;
        else                     apogeeSamples = 0;
      }

      bool falling = newAltitude && (apogeeSamples >= APOGEE_CONFIRM);

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
