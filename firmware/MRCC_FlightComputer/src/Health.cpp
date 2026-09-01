#include "Health.h"
#include "Config.h"
#include "State.h"
#include "Sensors.h"
#include "Storage.h"
#include "Radio.h"
#include "Filters.h"
#include "Baro.h"
#include "Flight.h"
#include "Pyro.h"
#include "esp_system.h"

static unsigned long lastHealthCheck = 0;
static unsigned long lastSdRetry     = 0;

static unsigned long lastRateLines = 0;
static unsigned long lastRateTime  = 0;


// =====================================================
// WHY DID THE BOARD RESTART?
// =====================================================

void reportResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  bootCount++;

  switch (reason) {
    case ESP_RST_POWERON:   resetReasonName = "POWER ON (clean start)"; break;
    case ESP_RST_EXT:       resetReasonName = "EXTERNAL RESET BUTTON";  break;
    case ESP_RST_SW:        resetReasonName = "SOFTWARE RESET";         break;
    case ESP_RST_PANIC:     resetReasonName = "CRASH / PANIC";          break;
    case ESP_RST_INT_WDT:   resetReasonName = "INTERRUPT WATCHDOG";     break;
    case ESP_RST_TASK_WDT:  resetReasonName = "TASK WATCHDOG";          break;
    case ESP_RST_WDT:       resetReasonName = "WATCHDOG";               break;
    case ESP_RST_BROWNOUT:  resetReasonName = "BROWNOUT - SUPPLY DIPPED"; break;
    case ESP_RST_DEEPSLEEP: resetReasonName = "DEEP SLEEP WAKE";        break;
    default:                resetReasonName = "UNKNOWN";                break;
  }

  if (reason == ESP_RST_BROWNOUT) brownoutCount++;

  Serial.println();
  Serial.println("--------- RESET REASON ---------");
  Serial.print("[SYS] Boot #");
  Serial.println(bootCount);
  Serial.print("[SYS] Last reset: ");
  Serial.println(resetReasonName);

  if (reason == ESP_RST_BROWNOUT) {
    Serial.println("[SYS] >>> THE SUPPLY VOLTAGE COLLAPSED <<<");
    Serial.println("[SYS] Under vibration this is a POWER CONNECTION");
    Serial.println("[SYS] breaking contact, not an SD or code fault.");
  }

  if (brownoutCount > 0) {
    Serial.print("[SYS] Brownouts since power up: ");
    Serial.println(brownoutCount);
  }

  if (bootCount == 1 && reason != ESP_RST_POWERON) {
    Serial.println("[SYS] RTC memory was cleared - the board lost");
    Serial.println("[SYS] power COMPLETELY, not just a dip.");
  }

  Serial.println("--------------------------------");
}


// =====================================================
// RECOVERY
//
// Runs every 5 s. Marks stale subsystems down and tries
// to bring failed ones back. Retries are silent - only
// a change of state is announced, so a permanently
// missing sensor never floods the log.
// =====================================================

void serviceHealth() {
  if (millis() - lastHealthCheck < HEALTH_INTERVAL) return;
  lastHealthCheck = millis();

  // ---- IMU ----
  if (imuOK && millis() - lastImuUpdate > IMU_STALE) {
    Serial.println("[IMU] No data for 2 s - marking DOWN");
    imuOK = false;
  }

  if (!imuOK) {
    if (initIMU(false)) {
      imuOK = true;
      imuRecoveries++;
      Serial.print("[IMU] *** RECOVERED *** (recovery #");
      Serial.print(imuRecoveries);
      Serial.println(")");

      // An IMU that was missing at boot never got a
      // zero-rate measurement. Take it now if the board
      // is still on the pad. Once flying, a moving
      // airframe rejects its own samples anyway.
      if (!gyroCalDone && !gyroCalibrating()) startGyroCal();
    }
  }

  // ---- BARO ----
  // The apogee sensor. Losing it downgrades deployment
  // to a timer, so its state changes are announced.
  if (baroOK && millis() - lastBaroUpdate > BARO_STALE) {
    Serial.println("[BARO] No data for 1 s - marking DOWN");
    Serial.println("[BARO] *** APOGEE IS NOW TIMER ONLY ***");
    baroOK = false;
  }

  if (!baroOK) {
    if (initBaro(false)) {
      baroOK = true;
      baroRecoveries++;
      Serial.print("[BARO] *** RECOVERED *** (recovery #");
      Serial.print(baroRecoveries);
      Serial.println(")");
    }
  }

  // ---- RADIO ----
  if (!radioOK) {
    if (initRadio(false)) {
      radioOK = true;
      radioRecoveries++;
      Serial.print("[LORA] *** RECOVERED *** (recovery #");
      Serial.print(radioRecoveries);
      Serial.println(")");
    }
  }

  // ---- SD ----
  // Retried less often because a full probe is slow.
  if (!sdOK && millis() - lastSdRetry > 30000) {
    lastSdRetry = millis();

    if (initSD(false)) {
      sdOK = true;
      sdRecoveries++;
      Serial.print("[SD] *** RECOVERED *** (recovery #");
      Serial.print(sdRecoveries);
      Serial.print(") -> ");
      Serial.println(logFileName);
    }
  }

  // ---- GPS ----
  gpsOK = (millis() - lastGpsDataTime < GPS_STALE);
}


// =====================================================
// STATUS LINE
// =====================================================

void printStatus() {
  unsigned long now = millis();
  unsigned long dt  = now - lastRateTime;

  float logHz  = 0.0;
  float loopHz = 0.0;

  if (dt > 0) {
    logHz  = (logLineCount - lastRateLines) * 1000.0 / dt;
    loopHz = loopCount * 1000.0 / dt;
  }

  lastRateLines = logLineCount;
  lastRateTime  = now;
  loopCount     = 0;

  // The flight line goes first. On the pad this is the
  // only line anyone actually reads.
  Serial.print("[FLT] ");
  Serial.print(stateName(flightState));
  Serial.print(" | pyro=");
  Serial.print(pyroArmed ? "ARMED" : "safe");

  if (pyroFired) {
    Serial.print(" FIRED(");
    Serial.print(lastFireReason);
    Serial.print(")");
  }

#if PYRO_CONT_ENABLED
  Serial.print(" | cont=");
  Serial.print(contOK ? "OK" : "OPEN");
#endif

  if (baroOK) {
    Serial.print(" | alt=");
    Serial.print(altFiltered, 1);
    Serial.print("m v=");
    Serial.print(vertVel, 1);
    Serial.print("m/s max=");
    Serial.print(maxAlt, 1);
    Serial.print("m");
  }
  else {
    Serial.print(" | NO BARO - apogee would be TIMER ONLY");
  }

  if (flightState == FS_PAD && imuOK) {
    Serial.print(" | a=");
    Serial.print(accelMag, 2);
    Serial.print(" g=");
    Serial.print(gyroMag, 1);
  }

  Serial.println();

  // Subsystem health next - one glance tells you what is alive
  Serial.print("[SYS] IMU=");
  Serial.print(imuOK   ? "OK"   : "DOWN");
  Serial.print(" GPS=");
  Serial.print(gpsOK   ? "OK"   : "DOWN");
  Serial.print(" LORA=");
  Serial.print(radioOK ? "OK"   : "DOWN");
  Serial.print(" SD=");
  Serial.print(sdOK    ? "OK"   : "DOWN");
  Serial.print(" BARO=");
  Serial.print(baroOK  ? "OK"   : "DOWN");
  Serial.print(" | boot=");
  Serial.print(bootCount);
  Serial.print(" | loop=");
  Serial.print(loopHz, 0);
  Serial.print("Hz");

#if VBAT_ENABLED
  Serial.print(" | V=");
  Serial.print(vbat, 2);
  Serial.print(" min=");
  Serial.print(vbatMin, 2);
#endif

  if (brownoutCount > 0) {
    Serial.print(" | BROWNOUTS=");
    Serial.print(brownoutCount);
  }

  if (imuRecoveries || radioRecoveries || sdRecoveries || baroRecoveries) {
    Serial.print(" | recovered I:");
    Serial.print(imuRecoveries);
    Serial.print(" L:");
    Serial.print(radioRecoveries);
    Serial.print(" S:");
    Serial.print(sdRecoveries);
    Serial.print(" B:");
    Serial.print(baroRecoveries);
  }

  Serial.println();

  if (imuOK) printFilterStatus();

  if (sdOK && logFile) {
    logFile.flush();

    Serial.print("[SD]  ");
    Serial.print(logFileName);
    Serial.print(" | lines=");
    Serial.print(logLineCount);
    Serial.print(" | bytes=");
    Serial.print(logFile.size());
    Serial.print(" | rate=");
    Serial.print(logHz, 1);
    Serial.print("Hz | err=");
    Serial.println(sdErrorCount);
  }

  if (txBusyCount || txTimeoutCount || txFallbackCount) {
    Serial.print("[TX]  busy=");
    Serial.print(txBusyCount);
    Serial.print(" timeout=");
    Serial.print(txTimeoutCount);
    Serial.print(" noIrq=");
    Serial.println(txFallbackCount);
  }
}
