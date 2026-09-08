#pragma once
#include <Arduino.h>

// =====================================================
// FILTERS - ICM20948 signal conditioning
//
// The raw IMU is unusable for attitude on its own.
// The accelerometer is buried in airframe vibration and
// motor noise; the gyro is quiet but drifts. This module
// runs the standard fix, in stages, and keeps BOTH the
// raw and the filtered value so the two can be compared
// side by side afterwards.
//
//   1. DESPIKE    3-sample median. Removes single-sample
//                 outliers from I2C glitches and EMI.
//   2. LOW PASS   1st order RC, cutoff in Hz. Removes
//                 vibration. Recomputed from the MEASURED
//                 dt every sample, so loop jitter does
//                 not change the cutoff.
//   3. BIAS       Gyro zero-rate offset, measured while
//                 the rocket sits still on the pad.
//   4. KALMAN     2 state (angle + gyro bias) per axis.
//                 Gyro gives the short term, accelerometer
//                 corrects the long term drift.
//   5. GATE       The accelerometer only tells you which
//                 way is down when the only force on it
//                 is gravity. Under thrust it is lying,
//                 so the measurement update is skipped
//                 and the Kalman coasts on the gyro.
//
// A complementary filter runs alongside the Kalman on
// the same inputs, purely as a comparison baseline.
//
// MOUNTING - THIS MATTERS.
// Both A and B: IMU +Y points out through the rocket nose.
// Attitude uses a right-handed frame with:
//     X = sensor +X, Y = sensor -Z, Z = sensor +Y (nose)
// Roll and pitch retain the existing Euler convention:
// about attitude X and Y, with attitude Z as the reference.
// In sensor coordinates:
//     roll  = atan2(-az, ay)
//     pitch = atan2(-ax, hypot(ay, az))
// Nose upright on the pad then reads roll 0, pitch 0.
// Gyro inputs are gx and -gz; heading uses the same mounting.
// Raw/filtered sensor vectors and calibration stay in sensor axes.
//
// These are tilt Euler angles, not axial rocket roll/yaw.
// The existing Euler singularity at pitch +/-90 remains.
//
// NOTHING HERE BLOCKS. Gyro calibration collects its
// samples across normal loop iterations.
// =====================================================


// -----------------------------------------------------
// 3-SAMPLE MEDIAN - outlier rejection
// -----------------------------------------------------

struct Median3 {
  float a, b, c;
  bool  primed;
};

void  median3Reset(Median3 &m);
float median3Push(Median3 &m, float x);


// -----------------------------------------------------
// 1st ORDER LOW PASS
//
// alpha is derived from the real dt each sample:
//   RC    = 1 / (2*pi*fc)
//   alpha = dt / (RC + dt)
//
// A fixed alpha would silently change cutoff whenever
// the loop rate changed. This does not.
// -----------------------------------------------------

struct LowPass {
  float y;
  bool  primed;
};

void  lowPassReset(LowPass &f);
float lowPassPush(LowPass &f, float x, float fc, float dt);


// -----------------------------------------------------
// KALMAN - 2 state, angle and gyro bias
//
//   state  = [ angle, bias ]
//   input  = gyro rate  (deg/s)
//   meas   = accelerometer angle (deg)
//
// The bias state is what removes gyro drift without
// ever having to trust the accelerometer on a fast
// moving airframe.
// -----------------------------------------------------

struct Kalman {
  float angle;
  float bias;
  float rate;
  float P[2][2];
};

void  kalmanReset(Kalman &k, float startAngle);
float kalmanPredict(Kalman &k, float newRate, float dt);
float kalmanUpdate(Kalman &k, float newAngle);


// -----------------------------------------------------
// PIPELINE
// -----------------------------------------------------

void filterInit();     // full reset, starts gyro calibration
void filterReset();    // reset states, keeps the measured bias
void filterUpdate();   // one raw IMU sample -> every filtered output

void startGyroCal();   // re-measure the gyro zero point
bool gyroCalibrating();
void printFilterStatus();
