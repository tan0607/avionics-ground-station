#include "Sensors.h"
#include "Config.h"
#include "State.h"
#include "Filters.h"
#include <Wire.h>
#include <math.h>

TinyGPSPlus    gps;
HardwareSerial GPSSerial(1);
ICM_20948_I2C  myICM;

// The chip answered begin() and took its config. NOT the same
// question as imuOK, which asks whether samples are arriving -
// see readIMU(). Keeping them apart is what stopped the DOWN /
// RECOVERED flap described in Health.cpp.
static bool imuChipOpen = false;


// =====================================================
// IMU
//
// Returns false if the sensor is absent or unresponsive.
// The caller marks it down and carries on - it is never
// allowed to stop the flight computer.
// =====================================================

bool initIMU(bool verbose) {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  myICM.begin(Wire, AD0_VAL);

  if (myICM.status != ICM_20948_Stat_Ok) {
    if (verbose) {
      Serial.print("[IMU] FAILED: ");
      Serial.println(myICM.statusString());
      Serial.println("[IMU] Continuing WITHOUT the IMU.");
      Serial.println("[IMU] Will retry automatically every 5 s.");
    }
    imuChipOpen = false;
    return false;
  }

  // -----------------------------------------------------
  // FULL SCALE RANGE
  //
  // The library's default is +/-2 g and +/-250 dps.
  // A rocket boosts at 5-15 g and can roll well past
  // 250 deg/s, so on the defaults the accelerometer
  // CLIPS AT 2 g and the gyro rails - which would make
  // the 3 g launch threshold impossible to reach.
  //
  // This is not optional. Nothing downstream works
  // without it.
  // -----------------------------------------------------

  ICM_20948_fss_t fss;
  fss.a = gpm16;      // +/-16 g
  fss.g = dps2000;    // +/-2000 deg/s

  myICM.setFullScale((ICM_20948_Internal_Acc | ICM_20948_Internal_Gyr), fss);

  if (myICM.status != ICM_20948_Stat_Ok) {
    if (verbose) {
      Serial.print("[IMU] FAILED to set full scale: ");
      Serial.println(myICM.statusString());
    }
    imuChipOpen = false;
    return false;
  }

  // ~100 Hz is plenty for a 20 Hz state machine and a
  // 10 Hz log, and it leaves the shared I2C bus free
  // for the barometer.
  ICM_20948_smplrt_t rate;
  rate.a = 10;        // 1125 / (1 + 10) = 102 Hz
  rate.g = 10;

  myICM.setSampleRate((ICM_20948_Internal_Acc | ICM_20948_Internal_Gyr), rate);

  if (verbose) {
    Serial.println("[IMU] SUCCESS  range +/-16 g, +/-2000 dps @ 100 Hz");
  }

  // A recovery restarts the filter states but KEEPS the
  // gyro bias already measured on the pad - there is no
  // opportunity to re-measure it mid flight.
  filterReset();

  // lastImuUpdate is deliberately NOT stamped here. Opening the
  // chip is not a sample, and stamping it was what let a sensor
  // delivering nothing look fresh for another 2 s on every retry.
  // Only readIMU() advances that clock, and only with data in hand.
  imuChipOpen = true;
  return true;
}


void readIMU() {
  // Gated on the chip being open, NOT on imuOK - a down IMU still
  // has to be polled or it could never come back. This is also why
  // ax/ay/az no longer freeze at their last good values while the
  // sensor is dead: nothing skips the read.
  if (!imuChipOpen) {
    return;
  }

  if (!myICM.dataReady()) {
    return;
  }

  myICM.getAGMT();

  // SparkFun reports acceleration in mg
  ax = myICM.accX() * 0.00980665;
  ay = myICM.accY() * 0.00980665;
  az = myICM.accZ() * 0.00980665;

  gx = myICM.gyrX();
  gy = myICM.gyrY();
  gz = myICM.gyrZ();

  mx = myICM.magX();
  my = myICM.magY();
  mz = myICM.magZ();

  // RAW heading - level-only, and it shows. Kept as the
  // "before" trace; Filters.cpp produces the tilt
  // compensated one next to it.
  heading = atan2(my, mx) * 180.0 / PI;

  if (heading < 0)       heading += 360.0;
  if (heading >= 360.0)  heading -= 360.0;

  // Everything above is untouched sensor output. Run the
  // filter chain here, on the new sample, so dt is the
  // real IMU interval and not the loop period.
  filterUpdate();

  lastImuUpdate = millis();

  // A real sample is the ONLY evidence that counts, so this is the
  // one place imuOK is ever raised. Silently: serviceHealth() owns
  // the announcement and the recovery counter, the same way it owns
  // them for every other subsystem.
  imuOK = true;
}


// =====================================================
// GPS
// =====================================================

void initGPS() {
  // Big RX buffer so no NMEA is lost if the loop stalls
  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  Serial.println("[GPS] UART started");
  Serial.println("[GPS] Waiting up to 5 s for data...");

  unsigned long t0 = millis();

  while (millis() - t0 < GPS_START_TIMEOUT) {
    while (GPSSerial.available() > 0) {
      gps.encode(GPSSerial.read());
      lastGpsDataTime = millis();
      gpsOK = true;
    }

    if (gpsOK) break;
    delay(10);
  }

  if (gpsOK) {
    Serial.println("[GPS] SUCCESS - data received");
  }
  else {
    Serial.println("[GPS] WARNING - no data yet");
    Serial.println("[GPS] Check TX -> GPIO18, VCC and GND.");
    Serial.println("[GPS] Continuing - it may appear later.");
  }
}


void readGPS() {
  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
    lastGpsDataTime = millis();
  }
}


void updateGpsSnapshot() {
  gpsData = (millis() - lastGpsDataTime < GPS_STALE);
  gpsOK   = gpsData;

  satellites = 0;

  if (gps.satellites.isValid()) {
    satellites = gps.satellites.value();
  }

  gpsFix = (gps.location.isValid() &&
            gps.location.age() < 3000 &&
            satellites >= 4);

  latitude  = 0.0;
  longitude = 0.0;

  if (gpsFix) {
    latitude  = gps.location.lat();
    longitude = gps.location.lng();
  }

  gpsAltitude = 0.0;
  gpsSpeed    = 0.0;
  gpsCourse   = 0.0;

  if (gps.altitude.isValid()) gpsAltitude = gps.altitude.meters();
  if (gps.speed.isValid())    gpsSpeed    = gps.speed.mps();
  if (gps.course.isValid())   gpsCourse   = gps.course.deg();
}


void updateHorizontalVelocity() {
  if (gps.speed.isValid() && gps.course.isValid()) {
    float speed     = gps.speed.mps();
    float courseRad = gps.course.deg() * PI / 180.0;

    vx = speed * sin(courseRad);   // east
    vy = speed * cos(courseRad);   // north
  }
}


// =====================================================
// SUPPLY VOLTAGE
// =====================================================

void readVbat() {
#if VBAT_ENABLED
  vbat = analogReadMilliVolts(VBAT_PIN) * VBAT_DIVIDER / 1000.0;

  if (vbat < vbatMin) {
    vbatMin = vbat;
  }
#endif
}
