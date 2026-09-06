#include "Filters.h"
#include "Config.h"
#include "State.h"
#include <math.h>

// =====================================================
// FILTER INSTANCES
//
// Raw in, filtered out. Both are kept in State so the
// log can carry the two side by side.
// =====================================================

static Median3 mAx, mAy, mAz;
static Median3 mGx, mGy, mGz;
static Median3 mMx, mMy, mMz;

static LowPass lAx, lAy, lAz;
static LowPass lGx, lGy, lGz;
static LowPass lMx, lMy, lMz;

static Kalman kRoll, kPitch;

static unsigned long lastFilterUs = 0;

// Gyro zero-rate calibration
#define CAL_IDLE       0
#define CAL_COLLECTING 1
#define CAL_DONE       2
#define CAL_SKIPPED    3

static uint8_t       calState = CAL_IDLE;
static int           calCount = 0;
static float         calSumX = 0, calSumY = 0, calSumZ = 0;
static unsigned long calStart = 0;
static unsigned long lastCalSample = 0;

#if FILTER_ENABLED
static void runGyroCal(float rx, float ry, float rz);
static void updateHeadingTiltCompensated();
#endif


// =====================================================
// 3-SAMPLE MEDIAN
//
// One bad sample in three cannot get through. Costs
// one sample of delay, which at IMU rates is nothing.
// =====================================================

void median3Reset(Median3 &m) {
  m.a = m.b = m.c = 0.0;
  m.primed = false;
}


float median3Push(Median3 &m, float x) {
  if (!m.primed) {
    m.a = m.b = m.c = x;
    m.primed = true;
    return x;
  }

  m.a = m.b;
  m.b = m.c;
  m.c = x;

  float lo = m.a, mid = m.b, hi = m.c;

  if (lo > mid) { float t = lo; lo = mid; mid = t; }
  if (mid > hi) { float t = mid; mid = hi; hi = t; }
  if (lo > mid) { mid = lo; }

  return mid;
}


// =====================================================
// 1st ORDER LOW PASS
//
// alpha comes from the dt that actually happened, not
// from an assumed loop rate. If the loop slows down,
// the cutoff frequency stays where it was put.
// =====================================================

void lowPassReset(LowPass &f) {
  f.y = 0.0;
  f.primed = false;
}


float lowPassPush(LowPass &f, float x, float fc, float dt) {
  if (!f.primed) {
    f.y = x;
    f.primed = true;
    return f.y;
  }

  if (fc <= 0.0 || dt <= 0.0) return f.y;

  float rc    = 1.0 / (2.0 * PI * fc);
  float alpha = dt / (rc + dt);

  f.y += alpha * (x - f.y);
  return f.y;
}


// =====================================================
// KALMAN - angle and gyro bias
//
// Split into predict and update on purpose. Predict runs
// every sample. Update runs only when the accelerometer
// is actually measuring gravity and nothing else.
// =====================================================

void kalmanReset(Kalman &k, float startAngle) {
  k.angle = startAngle;
  k.bias  = 0.0;
  k.rate  = 0.0;

  k.P[0][0] = 0.0;
  k.P[0][1] = 0.0;
  k.P[1][0] = 0.0;
  k.P[1][1] = 0.0;
}


float kalmanPredict(Kalman &k, float newRate, float dt) {
  // State: integrate the bias corrected rate
  k.rate   = newRate - k.bias;
  k.angle += dt * k.rate;

  // Covariance: P = F P F' + Q
  k.P[0][0] += dt * (dt * k.P[1][1] - k.P[0][1] - k.P[1][0] + KF_Q_ANGLE);
  k.P[0][1] -= dt * k.P[1][1];
  k.P[1][0] -= dt * k.P[1][1];
  k.P[1][1] += KF_Q_BIAS * dt;

  return k.angle;
}


float kalmanUpdate(Kalman &k, float newAngle) {
  float S  = k.P[0][0] + KF_R_MEASURE;
  float K0 = k.P[0][0] / S;
  float K1 = k.P[1][0] / S;

  float y = newAngle - k.angle;

  k.angle += K0 * y;
  k.bias  += K1 * y;

  float P00 = k.P[0][0];
  float P01 = k.P[0][1];

  k.P[0][0] -= K0 * P00;
  k.P[0][1] -= K0 * P01;
  k.P[1][0] -= K1 * P00;
  k.P[1][1] -= K1 * P01;

  return k.angle;
}


// =====================================================
// SETUP AND RESET
// =====================================================

void filterInit() {
  filterReset();

  gyroBiasX = 0.0;
  gyroBiasY = 0.0;
  gyroBiasZ = 0.0;

  startGyroCal();
}


void filterReset() {
  median3Reset(mAx); median3Reset(mAy); median3Reset(mAz);
  median3Reset(mGx); median3Reset(mGy); median3Reset(mGz);
  median3Reset(mMx); median3Reset(mMy); median3Reset(mMz);

  lowPassReset(lAx); lowPassReset(lAy); lowPassReset(lAz);
  lowPassReset(lGx); lowPassReset(lGy); lowPassReset(lGz);
  lowPassReset(lMx); lowPassReset(lMy); lowPassReset(lMz);

  kalmanReset(kRoll,  0.0);
  kalmanReset(kPitch, 0.0);

  rollComp  = 0.0;
  pitchComp = 0.0;

  lastFilterUs = micros();
  imuHz        = 0.0;
}


void startGyroCal() {
  calState = CAL_COLLECTING;
  calCount = 0;
  calSumX  = 0.0;
  calSumY  = 0.0;
  calSumZ  = 0.0;
  calStart = millis();
  lastCalSample = 0;

  gyroCalDone = false;

  Serial.println("[FILT] Gyro calibration started - HOLD STILL");
}


bool gyroCalibrating() {
  return (calState == CAL_COLLECTING);
}


// =====================================================
// GYRO ZERO RATE
//
// Averages the gyro while the airframe is still. Any
// movement throws the batch away and starts again, so a
// bump on the pad cannot poison the bias. It never
// blocks and it never waits forever - if the rocket is
// not still within the timeout, the bias stays at zero
// rather than becoming wrong.
// =====================================================

#if FILTER_ENABLED
static void runGyroCal(float rx, float ry, float rz) {
  if (calState != CAL_COLLECTING) return;
  if (lastCalSample != 0 && millis() - lastCalSample > PAD_IMU_MAX_GAP) {
    calCount = 0;
    calSumX = calSumY = calSumZ = 0.0;
  }
  lastCalSample = millis();

  bool moving = (!isfinite(rx) || !isfinite(ry) || !isfinite(rz) ||
                 fabs(rx) > GYRO_CAL_MAX_RATE ||
                 fabs(ry) > GYRO_CAL_MAX_RATE ||
                 fabs(rz) > GYRO_CAL_MAX_RATE);

  if (moving) {
    calCount = 0;
    calSumX  = 0.0;
    calSumY  = 0.0;
    calSumZ  = 0.0;

    if (millis() - calStart > GYRO_CAL_TIMEOUT) {
      calState    = CAL_SKIPPED;
      gyroCalDone = false;

      gyroBiasX = 0.0;
      gyroBiasY = 0.0;
      gyroBiasZ = 0.0;

      Serial.println("[FILT] Gyro cal GAVE UP - never held still.");
      Serial.println("[FILT] Bias left at zero. Unarmed PAD retries automatically.");
    }
    return;
  }

  calSumX += rx;
  calSumY += ry;
  calSumZ += rz;
  calCount++;

  if (calCount < GYRO_CAL_SAMPLES) return;

  gyroBiasX = calSumX / calCount;
  gyroBiasY = calSumY / calCount;
  gyroBiasZ = calSumZ / calCount;

  calState    = CAL_DONE;
  gyroCalDone = true;

  Serial.print("[FILT] Gyro bias  X=");
  Serial.print(gyroBiasX, 3);
  Serial.print("  Y=");
  Serial.print(gyroBiasY, 3);
  Serial.print("  Z=");
  Serial.print(gyroBiasZ, 3);
  Serial.println(" deg/s");
}
#endif


// =====================================================
// THE PIPELINE - one raw sample in, everything out
//
// Called from readIMU the moment a new sample lands, so
// dt is the real interval between IMU samples and not
// the loop period.
// =====================================================

void filterUpdate() {
  unsigned long nowUs = micros();

  float dt = (nowUs - lastFilterUs) * 1.0e-6;
  lastFilterUs = nowUs;

  // First sample, a stall, or a micros() rollover
  if (dt <= 0.0 || dt > DT_MAX) dt = DT_DEFAULT;

  // Effective sample rate, smoothed - shown in status
  float inst = 1.0 / dt;
  imuHz = (imuHz <= 0.0) ? inst : imuHz + 0.02 * (inst - imuHz);

  // ---- ALWAYS AVAILABLE: the raw accelerometer angle.
  // This is the "before" trace. Vibration goes straight
  // through it, which is exactly the point.
  float axyz = sqrt(ax * ax + ay * ay + az * az);
  accelNormRaw = axyz;

  // Mounted attitude frame: (X, Y, Z) = (sensor X, -sensor Z, sensor Y).
  // Both vehicles have +Y toward the nose. Keep logged sensor vectors intact.
  rollAcc  = atan2(-az, ay) * RAD_TO_DEG;
  pitchAcc = atan2(-ax, sqrt(ay * ay + az * az)) * RAD_TO_DEG;

  // AK09916 -> accelerometer frame (my, mx, -mz), then mounted frame
  // (my, mz, mx). Uncompensated baseline in the same frame as headingFilt.
  heading = atan2(mz, my) * RAD_TO_DEG;
  if (heading < 0)       heading += 360.0;
  if (heading >= 360.0)  heading -= 360.0;

#if !FILTER_ENABLED
  // A/B switch for the report: with this off the filtered
  // channels simply mirror the raw ones.
  fax = ax; fay = ay; faz = az;
  fgx = gx; fgy = gy; fgz = gz;
  fmx = mx; fmy = my; fmz = mz;

  accelNormFilt = accelNormRaw;
  rollLpf   = rollAcc;   pitchLpf   = pitchAcc;
  rollComp  = rollAcc;   pitchComp  = pitchAcc;
  rollKal   = rollAcc;   pitchKal   = pitchAcc;
  headingFilt  = heading;
  accelTrusted = true;
  return;
#else

  // ---- STAGE 1: despike ----
  float dax = median3Push(mAx, ax);
  float day = median3Push(mAy, ay);
  float daz = median3Push(mAz, az);

  float dgx = median3Push(mGx, gx);
  float dgy = median3Push(mGy, gy);
  float dgz = median3Push(mGz, gz);

  float dmx = median3Push(mMx, mx);
  float dmy = median3Push(mMy, my);
  float dmz = median3Push(mMz, mz);

  // ---- STAGE 3a: gyro zero rate (needs despiked, unbiased input) ----
  runGyroCal(dgx, dgy, dgz);

  // ---- STAGE 2: low pass ----
  fax = lowPassPush(lAx, dax, LPF_FC_ACCEL, dt);
  fay = lowPassPush(lAy, day, LPF_FC_ACCEL, dt);
  faz = lowPassPush(lAz, daz, LPF_FC_ACCEL, dt);

  fmx = lowPassPush(lMx, dmx, LPF_FC_MAG, dt);
  fmy = lowPassPush(lMy, dmy, LPF_FC_MAG, dt);
  fmz = lowPassPush(lMz, dmz, LPF_FC_MAG, dt);

  // ---- STAGE 3b: low pass then remove the measured bias.
  // The subtraction is linear so the order does not matter,
  // but doing it here keeps the bias out of the filter state.
  fgx = lowPassPush(lGx, dgx, LPF_FC_GYRO, dt) - gyroBiasX;
  fgy = lowPassPush(lGy, dgy, LPF_FC_GYRO, dt) - gyroBiasY;
  fgz = lowPassPush(lGz, dgz, LPF_FC_GYRO, dt) - gyroBiasZ;

  accelNormFilt = sqrt(fax * fax + fay * fay + faz * faz);

  // Angle from the low passed accelerometer. Logged on its
  // own so the report can separate what the low pass did
  // from what the sensor fusion did.
  rollLpf  = atan2(-faz, fay) * RAD_TO_DEG;
  pitchLpf = atan2(-fax, sqrt(fay * fay + faz * faz)) * RAD_TO_DEG;

  // ---- STAGE 5: is the accelerometer telling the truth? ----
  //
  // It only points at gravity when gravity is the only
  // thing acting on it. Under thrust, at burnout, or on
  // parachute snatch the vector is dominated by the
  // manoeuvre and the angle it implies is nonsense.
  accelTrusted = (fabs(accelNormFilt - GRAVITY) < ACC_TRUST_BAND);

  // ---- STAGE 4: Kalman ----
  //
  // Predict every sample from the gyro. Correct only when
  // the gate above says the accelerometer is trustworthy.
  kalmanPredict(kRoll,  fgx, dt);
  kalmanPredict(kPitch, -fgz, dt);

  if (accelTrusted) {
    // Roll wraps at +-180. A wrap is not a 360 deg/s
    // manoeuvre, so jump the state instead of fighting it.
    if (fabs(rollLpf - kRoll.angle) > 90.0) {
      kalmanReset(kRoll, rollLpf);
    }
    else {
      kalmanUpdate(kRoll, rollLpf);
    }

    kalmanUpdate(kPitch, pitchLpf);
  }

  rollKal  = kRoll.angle;
  pitchKal = kPitch.angle;

  // ---- BASELINE: complementary filter ----
  //
  // Same inputs, same gate, one line of maths. Kept so the
  // report can show what the Kalman is actually buying.
  float alpha = COMP_TAU / (COMP_TAU + dt);

  rollComp  += fgx * dt;
  pitchComp += -fgz * dt;

  if (accelTrusted) {
    if (fabs(rollLpf - rollComp) > 90.0) {
      rollComp = rollLpf;
    }
    else {
      rollComp = alpha * rollComp + (1.0 - alpha) * rollLpf;
    }

    pitchComp = alpha * pitchComp + (1.0 - alpha) * pitchLpf;
  }

  updateHeadingTiltCompensated();
#endif
}


// =====================================================
// HEADING
//
// The raw heading above is atan2(mz,my), in the mounted
// frame, and is only correct with the nose upright. Tilt it
// and the number swings by tens of degrees.
//
// This version rotates the magnetometer back to level
// using the Kalman roll and pitch first.
//
// NOTE: the AK09916 inside the ICM20948 does not share
// axes with the accelerometer and gyro. Mag X is accel
// Y, mag Y is accel X, mag Z is inverted. That swap is
// followed by the +Y-nose mounting rotation here, not in
// the logged channels, so MX/MY/MZ and FMX/FMY/FMZ stay
// directly comparable.
//
// This is still an UNCALIBRATED heading. Hard and soft
// iron correction is a separate job - treat it as a
// relative bearing, not a surveyed one.
// =====================================================

#if FILTER_ENABLED
static void updateHeadingTiltCompensated() {
  // Magnetometer -> accelerometer frame -> mounted attitude frame
  float bx =  fmy;
  float by =  fmz;
  float bz =  fmx;

  float r = rollKal  * DEG_TO_RAD;
  float p = pitchKal * DEG_TO_RAD;

  float sr = sin(r), cr = cos(r);
  float sp = sin(p), cp = cos(p);

  float xh = bx * cp + bz * sp;
  float yh = bx * sr * sp + by * cr - bz * sr * cp;

  headingFilt = atan2(yh, xh) * RAD_TO_DEG;

  if (headingFilt < 0)      headingFilt += 360.0;
  if (headingFilt >= 360.0) headingFilt -= 360.0;
}
#endif


// =====================================================
// STATUS
// =====================================================

void printFilterStatus() {
  Serial.print("[FILT] ");

#if FILTER_ENABLED
  Serial.print("ON");
#else
  Serial.print("OFF (compiled out)");
#endif

  Serial.print(" | imu=");
  Serial.print(imuHz, 0);
  Serial.print("Hz | lpf a/g/m=");
  Serial.print(LPF_FC_ACCEL, 0);
  Serial.print("/");
  Serial.print(LPF_FC_GYRO, 0);
  Serial.print("/");
  Serial.print(LPF_FC_MAG, 0);
  Serial.print("Hz | gyro cal=");

  switch (calState) {
    case CAL_IDLE:       Serial.print("IDLE");    break;
    case CAL_COLLECTING: Serial.print("RUNNING"); break;
    case CAL_DONE:       Serial.print("DONE");    break;
    case CAL_SKIPPED:    Serial.print("SKIPPED"); break;
  }

  Serial.print(" | acc=");
  Serial.print(accelTrusted ? "TRUSTED" : "GATED");
  Serial.print(" | roll=");
  Serial.print(rollKal, 1);
  Serial.print(" pitch=");
  Serial.print(pitchKal, 1);
  Serial.print(" | tx=");
  Serial.println(txFiltered ? "FILTERED" : "RAW");
}
