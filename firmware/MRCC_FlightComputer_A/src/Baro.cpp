#include "Baro.h"
#include "Config.h"
#include "State.h"
#include <Wire.h>
#include <math.h>

Adafruit_BMP280 bmp;
uint8_t         baroAddress    = 0;
unsigned long   baroSpikeCount = 0;
float           baroSpikeAlt   = 0.0;

static unsigned long lastBaroRead = 0;

// Samples lost because the sensor would not accept a trigger. Unlike a
// spike, this is not the sensor disagreeing with us - it is the I2C
// write failing, which means the next read would hand back the LAST
// conversion all over again.
unsigned long baroTriggerFails = 0;

// ctrl_meas: osrs_t in 7:5, osrs_p in 4:2, mode in 1:0.
// x1 temperature, x8 pressure, FORCED - the same oversampling the
// sensor ran in normal mode, taken one conversion at a time.
#define BMP280_REG_CTRL_MEAS 0xF4
#define BARO_CTRL_FORCED     ((1 << 5) | (4 << 2) | 1)

// Ask for ONE conversion and return immediately.
//
// Adafruit's takeForcedMeasurement() does this and then spins on the
// status register until the conversion lands - about 23 ms at x8. This
// loop runs at 20 Hz, so that would hand a third of every cycle to a
// busy-wait, on a loop that already concedes it stalls (Flight.cpp's
// dt > 0.5 clamp, serviceLogging()'s catch-up). Trigger here, collect
// on the next pass 50 ms later, when the answer has been sitting in
// the data registers for 27 ms.
static bool triggerForced() {
  Wire.beginTransmission(baroAddress);
  Wire.write(BMP280_REG_CTRL_MEAS);
  Wire.write(BARO_CTRL_FORCED);
  return Wire.endTransmission() == 0;
}

// Spike gate state. Seeded by the first accepted sample and
// re-seeded whenever a rejected value refuses to go away.
static float         lastGoodAlt  = 0.0;
static unsigned long lastGoodTime = 0;
static bool          altSeeded    = false;
static uint8_t       rejectRun    = 0;



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
  // FORCED, not NORMAL.
  //
  // In normal mode the part free-runs: convert, wait t_sb, convert,
  // forever. At STANDBY_MS_1 with x8 pressure that is very close to a
  // 100% duty cycle, and it is the highest sustained current this
  // sensor can be asked to draw. A1R's BMP280 will not survive it - it
  // accepts ctrl_meas 0x33, holds it for ~150 ms, then loses the
  // configuration entirely: ctrl_meas reads back 0x00 and the data
  // registers sit at 0x80000, their reset value. The same part runs
  // the same x8 conversion perfectly in forced mode, so the peak draw
  // is fine and only the SUSTAINED draw is not. That is a supply path
  // with resistance in it, and it is a board fault, not a code one -
  // but forced mode is worth having on a healthy board anyway, for the
  // reason below.
  //
  // FILTER_X4 -> FILTER_X2 is a consequence, not a preference. The IIR
  // filter counts SAMPLES, not milliseconds. Free-running it saw one
  // every ~23 ms, so X4 settled in roughly 100 ms. Triggered at
  // BARO_INTERVAL it sees one every 50 ms, and X4 would stretch that
  // past 200 ms - on the sensor apogee is called from. X2 puts it back
  // near 100 ms. Raise it again only if you also shorten BARO_INTERVAL.
  //
  // STANDBY_MS_1 is now inert; t_sb only applies to normal mode. Left
  // in place so the call still reads as the full configuration.
  bmp.setSampling(
    Adafruit_BMP280::MODE_FORCED,
    Adafruit_BMP280::SAMPLING_X1,    // temperature
    Adafruit_BMP280::SAMPLING_X8,    // pressure
    Adafruit_BMP280::FILTER_X2,
    Adafruit_BMP280::STANDBY_MS_1
  );

  // Put one conversion in flight so the first readBaro() has something
  // to collect rather than re-reading whatever the last power cycle
  // left behind.
  triggerForced();

  // A sensor that has just been (re)initialised has no history
  // worth keeping - the recovery path runs after the part has
  // been down, so the last good altitude is stale by however
  // long that took.
  altSeeded    = false;
  rejectRun    = 0;
  lastGoodTime = millis();

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

  unsigned long now = millis();

  if (now - lastBaroRead < BARO_INTERVAL) return;
  lastBaroRead = now;

  // Collect the conversion triggered on the previous pass. Both reads
  // happen BEFORE the next trigger, so they describe the same sample -
  // triggering between them could let the temperature come from a
  // newer conversion than the pressure it compensates.
  float pa = bmp.readPressure();      // Pa
  float tc = bmp.readTemperature();   // C

  // Re-arm before any gate below can return early. A rejected sample
  // must not stall the pipeline - the next pass still needs something
  // to read.
  //
  // A failed trigger is deliberately NOT treated as a bad sample. The
  // data registers still hold the previous conversion, so reading on
  // would publish a stale altitude with a fresh timestamp and the
  // staleness watchdog in Health.cpp would never fire: a frozen
  // altitude would look perfectly healthy all the way to apogee.
  // Leaving lastBaroUpdate alone lets that watchdog do its job.
  if (!triggerForced()) {
    baroTriggerFails++;
    return;
  }

  // A dead or unplugged sensor reads exactly zero.
  if (pa <= 0.0 || isnan(pa)) return;

  float hPa = pa / 100.0;
  float alt = 44330.0 * (1.0 - pow(pa / (SEA_LEVEL_HPA * 100.0), 0.1903));

  // Absolute net, and only the seed really needs it: with no
  // history the rate gate has nothing to measure against, and a
  // garbage seed would make it reject every good sample after it
  // until the run expires.
  if (hPa < BARO_MIN_HPA || hPa > BARO_MAX_HPA) {
    baroSpikeCount++;
    return;
  }

  // Rate gate, measured against the time actually elapsed since the
  // last accepted sample - NOT against BARO_INTERVAL, which is only
  // a ceiling. The loop stalls, and a fixed distance would tighten
  // precisely when the airframe has had longer to move; Config.h has
  // the arithmetic.
  //
  // Rejecting deliberately leaves lastBaroUpdate alone, so a gate
  // that somehow never let go would show up as the barometer going
  // stale - a loud, already-handled failure - and never as a quietly
  // wrong altitude. BARO_REJECT_RUN is short enough that it cannot
  // get that far.
  float dtGate = (now - lastGoodTime) / 1000.0;
  if (dtGate > BARO_GATE_DT_MAX) dtGate = BARO_GATE_DT_MAX;

  if (altSeeded && fabs(alt - lastGoodAlt) > BARO_MAX_RATE * dtGate) {
    baroSpikeCount++;
    baroSpikeAlt = alt;

    if (++rejectRun < BARO_REJECT_RUN) {
      // Once per burst. A spike that repeats every few seconds for
      // a whole pad wait must not bury the rest of the log.
      if (rejectRun == 1) {
        Serial.print("[BARO] SPIKE rejected - ");
        Serial.print(alt, 0);
        Serial.print(" m (");
        Serial.print(hPa, 1);
        Serial.print(" hPa) against ");
        Serial.print(lastGoodAlt, 0);
        Serial.print(" m after ");
        Serial.print((now - lastGoodTime));
        Serial.println(" ms");
      }
      return;
    }

    Serial.print("[BARO] ");
    Serial.print(rejectRun);
    Serial.print(" rejected in a row - the sensor means it. Re-seeding at ");
    Serial.print(alt, 0);
    Serial.println(" m");

    // The step this is about to publish is exactly what the gate was
    // built to keep out of the alpha-beta filter. Accepting it as a
    // measurement would hand that filter thousands of m/s and then
    // thousands negative on the sample after - which is APOGEE_VEL,
    // several times over. Say so, and let Flight.cpp re-prime instead.
    baroReseeded = true;
  }

  rejectRun    = 0;
  lastGoodAlt  = alt;
  lastGoodTime = now;
  altSeeded    = true;

  pressure   = hPa;
  baroTemp   = tc;
  baroAltMSL = alt;

  lastBaroUpdate = now;
}
