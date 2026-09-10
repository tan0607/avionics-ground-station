// BENCH ONLY. See ../README.md before upload.
#include "src/Config.h"
#include "src/State.h"
#include "src/Sensors.h"
#include "src/Filters.h"
#include "src/Storage.h"
#include "src/Radio.h"
#include "src/Health.h"
#include "src/Console.h"
#include "src/Baro.h"
#include "src/Pyro.h"
#include "src/Flight.h"

// =====================================================
// MRCC FLIGHT COMPUTER
// ESP32-S3 + GPS + ICM20948 + SX1278 + SD
//
//   Telemetry : every 0.5 s, sent twice, non-blocking
//   SD log    : 10 Hz
//   Flight SM : 20 Hz, single deployment at apogee
//
// DESIGN RULE - NOTHING EVER HALTS.
// Every subsystem init returns a result instead of
// trapping. A missing or broken part is marked down,
// skipped, and retried in the background every 5 s.
// The flight computer keeps flying on whatever works.
//
// Layout:
//   src/Config.h   - every pin and tunable
//   src/State.*    - shared flight state, health flags
//   src/Sensors.*  - IMU, GPS, supply voltage
//   src/Filters.*  - IMU despike, low pass, Kalman
//   src/Storage.*  - SD probe, mount, logging, recovery
//   src/Radio.*    - LoRa init, non-blocking transmit
//   src/Baro.*     - BMP280, the apogee sensor
//   src/Pyro.*     - ejection channel, arming, firing
//   src/Flight.*   - flight state machine
//   src/Health.*   - reset diagnosis, auto recovery
//   src/Console.*  - serial commands
//
// ONE EXCEPTION TO THE NEVER-HALT RULE: the pyro gate.
// It is driven low before anything else runs and is
// re-asserted low on every pass of the loop.
// =====================================================

static unsigned long lastFlush  = 0;
static unsigned long lastStatus = 0;


void setup() {
  // ---------------------------------------------------
  // NOTHING GOES ABOVE THIS LINE.
  //
  // Between reset and here the gate pin floats. Serial
  // takes 1.5 s to come up; a floating gate for 1.5 s
  // with a match connected is how people lose fingers.
  // The 10k hardware pulldown covers the microseconds
  // before even this runs.
  // ---------------------------------------------------
  pyroSafeInit();

  Serial.begin(115200);
  delay(1500);

  Serial.println("HAND TEST ONLY - 400 ms BENCH PULSE ENABLED - NOT FLIGHT FIRMWARE");
#if HAND_TEST_ORIGINAL_THRESHOLDS
  Serial.println("PROFILE=ORIGINAL_THRESHOLDS; multimeter/dummy load only");
#else
  Serial.println("PROFILE=HAND; experimental thresholds, not production validation");
#endif
  reportResetReason();

  // Pyro and flight state before any sensor, so that an
  // in-flight reset is diagnosed and safed immediately
  // rather than after a slow SD probe.
  initPyro(true);
  initFlight(true);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" MRCC HAND TEST - COMPUTER " VEHICLE_NAME);
  Serial.println("========================================");

  // SD first, before anything else loads the supply rail
  sdOK = initSD(true);

  initGPS();

  // initIMU() only proves the chip answers on I2C. imuOK means
  // SAMPLES ARE ARRIVING, so wait briefly for a real one - exactly
  // as initGPS() waits for its first NMEA - or the READY banner
  // below reports a handshake and calls it a working sensor.
  if (initIMU(true)) {
    unsigned long t0 = millis();
    while (!imuOK && millis() - t0 < IMU_STALE) {
      readIMU();          // raises imuOK on the first real sample
      delay(2);
    }

    if (!imuOK) {
      Serial.println("[IMU] WARNING - chip answers but has sent no");
      Serial.println("[IMU] data. Continuing - it may appear later.");
    }
  }

  // Starts the gyro zero-rate measurement. It collects
  // across normal loop passes and needs the board held
  // still - it does not block waiting for that.
  filterInit();

  baroOK  = initBaro(true);
  radioOK = initRadio(true);

  Serial.println();
  Serial.println("========================================");
  Serial.print(" READY   IMU=");
  Serial.print(imuOK   ? "OK" : "DOWN");
  Serial.print("  GPS=");
  Serial.print(gpsOK   ? "OK" : "DOWN");
  Serial.print("  LORA=");
  Serial.print(radioOK ? "OK" : "DOWN");
  Serial.print("  SD=");
  Serial.print(sdOK  ? "OK" : "DOWN");
  Serial.print("  BARO=");
  Serial.println(baroOK ? "OK" : "DOWN");
  Serial.print(" FLIGHT=");
  Serial.print(stateName(flightState));
  Serial.print("  PYRO=");
  Serial.println(pyroFired ? "FIRED (latched)" : "SAFE");
  Serial.println("========================================");

  if (!baroOK) {
    Serial.println(" *** NO BAROMETER - apogee would be");
    Serial.println(" *** TIMER ONLY. Do not fly like this.");
  }

  if (!imuOK || !radioOK || !sdOK) {
    Serial.println(" Some subsystems are DOWN. Flying anyway.");
    Serial.println(" They will be retried every 5 seconds.");
  }

  printMenu();
}


void loop() {
  loopCount++;

  // Nothing below this line is allowed to block.

  // The GPTimer ISR independently cuts the pyro pulse LOW. Service it first
  // here for cleanup and a secondary LOW fallback; later loop work can stall.
  servicePyro();

  readGPS();
  readIMU();
  readBaro();
  readVbat();

  // These used to live inside the radio's TX path, which
  // meant GPS data froze whenever the radio was down.
  updateGpsSnapshot();
  updateHorizontalVelocity();

  serviceFlight();      // unchanged 20 Hz state machine

  // Frequent USB feedback uses the values actually consumed by Flight.cpp.
  static unsigned long lastHandStatus = 0;
  if (millis() - lastHandStatus >= 250) {
    lastHandStatus = millis();
    Serial.print("[HAND TEST] ST="); Serial.print(stateName(flightState));
    Serial.print(" g="); Serial.print(accelMag / GRAVITY, 2);
    Serial.print(" ALT="); Serial.print(altFiltered, 2);
    Serial.print(" MAX="); Serial.print(maxAlt, 2);
    Serial.print(" VZ="); Serial.print(vertVel, 2);
    Serial.print(" IM="); Serial.print(imuOK);
    Serial.print(" BA="); Serial.print(baroOK);
    Serial.print(" IM_AGE_MS="); Serial.print(millis() - lastImuUpdate);
    Serial.print(" BA_AGE_MS="); Serial.print(millis() - lastBaroUpdate);
    Serial.print(" PULSE_LATCH="); Serial.print(pyroFired);
    Serial.print(" REASON="); Serial.print(lastFireReason);
    Serial.print(" GPIO="); Serial.println(pyroFiring ? "HIGH(commanded)" : "LOW(commanded)");
  }

  handleSerialCommands();

  serviceTelemetry();   // radio state machine
  serviceLogging();     // 10 Hz to the card

  if (millis() - lastFlush >= FLUSH_INTERVAL) {
    lastFlush = millis();
    flushSD();
  }

  if (millis() - lastStatus >= STATUS_INTERVAL) {
    lastStatus = millis();
    printStatus();
  }

  serviceHealth();      // bring back anything that failed
}
