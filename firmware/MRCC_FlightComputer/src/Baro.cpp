#include "Baro.h"
#include "Config.h"
#include "State.h"
#include <Wire.h>
#include <math.h>

Adafruit_BMP280 bmp;
uint8_t         baroAddress = 0;

static unsigned long lastBaroRead = 0;


// =====================================================
// INIT
//
// Tries both addresses. Plenty of boards sold as
// "BMP280" are wired to 0x77, and a few are actually
// BME280s, which answer with a different chip id.
// =====================================================

bool initBaro(bool verbose) {
  // Harmless if the IMU already did this.
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  baroAddress = 0;

  if (bmp.begin(BARO_ADDR_PRIMARY)) {
    baroAddress = BARO_ADDR_PRIMARY;
  }
  else if (bmp.begin(BARO_ADDR_FALLBACK)) {
    baroAddress = BARO_ADDR_FALLBACK;
  }

  if (baroAddress == 0) {
    if (verbose) {
      Serial.println("[BARO] FAILED - no BMP280 on 0x76 or 0x77");
      Serial.println("[BARO] Continuing WITHOUT the barometer.");
      Serial.println("[BARO] >>> APOGEE WOULD BE TIMER ONLY <<<");
      Serial.println("[BARO] Will retry automatically every 5 s.");
    }
    return false;
  }

  // Pressure oversampling x8 with the IIR filter at 4
  // is the useful compromise: about 0.2 m of noise at
  // roughly 25 Hz, which comfortably feeds our 20 Hz
  // state machine.
  bmp.setSampling(
    Adafruit_BMP280::MODE_NORMAL,
    Adafruit_BMP280::SAMPLING_X1,    // temperature
    Adafruit_BMP280::SAMPLING_X8,    // pressure
    Adafruit_BMP280::FILTER_X4,
    Adafruit_BMP280::STANDBY_MS_1
  );

  if (verbose) {
    Serial.print("[BARO] SUCCESS at 0x");
    Serial.println(baroAddress, HEX);
  }

  lastBaroUpdate = millis();
  return true;
}


// =====================================================
// READ - 20 Hz
//
// Altitude is computed from the pressure we already
// have, rather than calling readAltitude(), which
// would go back to the sensor for a second reading.
// =====================================================

void readBaro() {
  if (!baroOK) return;

  if (millis() - lastBaroRead < BARO_INTERVAL) return;
  lastBaroRead = millis();

  float pa = bmp.readPressure();      // Pa

  // A dead or unplugged sensor reads exactly zero.
  if (pa <= 0.0 || isnan(pa)) return;

  pressure   = pa / 100.0;            // hPa
  baroTemp   = bmp.readTemperature();
  baroAltMSL = 44330.0 * (1.0 - pow(pa / (SEA_LEVEL_HPA * 100.0), 0.1903));

  lastBaroUpdate = millis();
}
