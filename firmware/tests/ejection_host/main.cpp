// Drive the actual firmware modules through their public interface.
// Each invocation starts fresh C++ statics, including for a simulated reset.
#include "Arduino.h"
#include "Config.h"
#include "State.h"
#include "Filters.h"
#include "Flight.h"
#include "Pyro.h"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

unsigned long hostNowMs = 0;
HostSerial Serial;
static int gate = LOW;
static unsigned long rises = 0;
static unsigned long falls = 0;
static unsigned long imuSamples = 0;
static unsigned long baroSamples = 0;
static char noseAxis = 'z'; // legacy tests; MOUNT y selects confirmed A/B install

static void snapshot(const std::string& kind, const std::string& label = "") {
  const ArmReadiness ready = armReadiness();
  std::cout << std::setprecision(9)
    << "{\"kind\":\"" << kind << "\",\"label\":\"" << label
    << "\",\"vehicle\":\"" << VEHICLE_NAME << "\",\"ms\":" << millis()
    << ",\"state\":\"" << stateName(flightState) << "\",\"gate\":" << gate
    << ",\"armed\":" << pyroArmed << ",\"fired\":" << pyroFired
    << ",\"firing\":" << pyroFiring << ",\"rises\":" << rises
    << ",\"falls\":" << falls << ",\"fire_count\":" << fireCount
    << ",\"launch_ms\":" << launchTime << ",\"backup\":" << timerBackupUsed
    << ",\"reason\":\"" << (lastFireReason ? lastFireReason : "")
    << "\",\"alt\":" << altFiltered << ",\"max_alt\":" << maxAlt
    << ",\"vz\":" << vertVel << ",\"accel\":" << accelMag
    << ",\"ax\":" << ax << ",\"ay\":" << ay << ",\"az\":" << az
    << ",\"fax\":" << fax << ",\"fay\":" << fay << ",\"faz\":" << faz
    << ",\"gyro\":" << gyroMag << ",\"roll\":" << rollKal << ",\"pitch\":" << pitchKal
    << ",\"baro_input\":" << baroAltMSL
    << ",\"imu_ok\":" << imuOK << ",\"baro_ok\":" << baroOK
    << ",\"gyro_cal\":" << gyroCalDone << ",\"imu_samples\":" << imuSamples
    << ",\"arm_wait\":" << static_cast<int>(ready.wait)
    << ",\"arm_delay_ms\":" << ready.delayRemainingMs
    << ",\"arm_still_ms\":" << ready.stillRemainingMs
    << ",\"baro_samples\":" << baroSamples
    << ",\"imu_age_ms\":" << (millis() - lastImuUpdate)
    << ",\"baro_age_ms\":" << (millis() - lastBaroUpdate)
    << ",\"latch_valid\":" << latchValid()
    << ",\"latch_state\":" << static_cast<int>(latchState())
    << ",\"latch_fired\":" << latchFired()
    << ",\"latch_launch_ms\":" << latchLaunchTime() << "}\n";
}

void pinMode(int, int) {}
int digitalRead(int) { return HIGH; }
int analogReadMilliVolts(int) { return 0; }
void digitalWrite(int pin, int value) {
  if (pin != PYRO_GATE_PIN) throw std::runtime_error("unexpected GPIO");
  if (gate == value) return;
  gate = value;
  if (value == HIGH) ++rises; else ++falls;
  snapshot(value == HIGH ? "rise" : "fall");
}

int main() {
  try {
    bool booted = false;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      std::istringstream in(line);
      std::string command;
      in >> command;
      if (command == "BOOT") {
        unsigned long bootMs, savedLaunch;
        int savedState, savedFired;
        if (booted || !(in >> bootMs >> savedState >> savedFired >> savedLaunch))
          throw std::runtime_error("invalid BOOT");
        // Reconstruct a valid retained latch in a fresh process. All other
        // firmware globals/statics retain their genuine startup initializers.
        if (savedState >= 0)
          latchWrite(static_cast<uint8_t>(savedState), savedFired != 0, savedLaunch);
        pyroSafeInit();
        hostNowMs = bootMs; // setup's initial delay; sensor init delays excluded
        initPyro(false);
        initFlight(false);
        filterInit();
        booted = true;
        snapshot("boot");
      } else if (!booted) {
        throw std::runtime_error("BOOT required");
      } else if (command == "MOUNT") {
        if (!(in >> noseAxis) || (noseAxis != 'y' && noseAxis != 'z'))
          throw std::runtime_error("MOUNT must be y or z");
      } else if (command == "STEP") {
        unsigned long next;
        float altitude, accelG, gyroZ;
        int imuHealthy, baroHealthy, freshImu, freshBaro, reseeded;
        if (!(in >> next >> altitude >> accelG >> gyroZ >> imuHealthy
                 >> baroHealthy >> freshImu >> freshBaro >> reseeded)
            || next <= hostNowMs)
          throw std::runtime_error("invalid or non-monotonic STEP");
        hostNowMs = next;
        servicePyro(); // same ordering as the flight sketch's loop
        imuOK = imuHealthy != 0;
        baroOK = baroHealthy != 0;
        if (imuOK && freshImu) {
          ax = 0;
          ay = noseAxis == 'y' ? accelG * GRAVITY : 0;
          az = noseAxis == 'z' ? accelG * GRAVITY : 0;
          gx = gy = 0;
          gz = gyroZ;
          lastImuUpdate = millis();
          ++imuSamples;
          filterUpdate();
        }
        if (baroOK && freshBaro) {
          // Accepted altitude at the Baro.cpp -> State boundary. This does
          // NOT run BMP280 conversion, spike rejection, or health detection.
          pressure = 1013.25f; // valid reference sentinel, not pressure simulation
          baroAltMSL = altitude;
          lastBaroUpdate = millis();
          ++baroSamples;
        }
        if (reseeded) baroReseeded = true;
        const auto before = flightState;
        serviceFlight();
        if (before != flightState) snapshot("state");
      } else if (command == "SNAP") {
        std::string label;
        if (!(in >> label) || label.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
              != std::string::npos)
          throw std::runtime_error("invalid SNAP label");
        snapshot("snapshot", label);
      } else if (command == "DISARM") {
        disarmFlight();
        snapshot("disarm");
      } else if (command == "TRY_FIRE") {
        firePyro("HOST GUARD TEST");
        snapshot("try_fire");
      } else {
        throw std::runtime_error("unknown command");
      }
      std::string extra;
      if (in >> extra) throw std::runtime_error("unexpected trailing input");
    }
    if (!booted) throw std::runtime_error("empty input");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
