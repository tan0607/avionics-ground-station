// Drive the actual firmware modules through their public interface.
// Each invocation starts fresh C++ statics, including for a simulated reset.
#include "Arduino.h"
#include "Config.h"
#include "State.h"
#include "Filters.h"
#include "Flight.h"
#include "Pyro.h"
#include "driver/gptimer.h"
#include "soc/rtc.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#if defined(__SANITIZE_ADDRESS__)
#define HOST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HOST_ASAN 1
#endif
#endif
#ifdef HOST_ASAN
#include <sanitizer/asan_interface.h>
#endif

unsigned long hostNowMs = 0;
HostSerial Serial;
static std::string timerFail;
static uint64_t hostOriginalLaunchTicks = 0;
struct host_timer {
  bool enabled = false, running = false;
  uint64_t due = 0, count = 0, alarm = 0;
  gptimer_event_callbacks_t callbacks{};
};
static host_timer hostTimer;
static int timerResult(const char* op) { return timerFail == op ? ESP_FAIL : ESP_OK; }
esp_err_t gptimer_new_timer(const gptimer_config_t*, gptimer_handle_t* out) {
  if (timerResult("new")) return ESP_FAIL;
  hostTimer = {}; *out = &hostTimer; return ESP_OK;
}
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t t, const gptimer_event_callbacks_t* c, void*) {
  if (timerResult("callback")) return ESP_FAIL;
  t->callbacks = *c; return ESP_OK;
}
esp_err_t gptimer_enable(gptimer_handle_t t) { if (timerResult("enable")) return ESP_FAIL; t->enabled = true; return ESP_OK; }
esp_err_t gptimer_disable(gptimer_handle_t t) { t->enabled = false; return ESP_OK; }
esp_err_t gptimer_del_timer(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t t) { t->running = false; return ESP_OK; }
esp_err_t gptimer_set_raw_count(gptimer_handle_t t, uint64_t n) { if (timerResult("count")) return ESP_FAIL; t->count = n; return ESP_OK; }
esp_err_t gptimer_set_alarm_action(gptimer_handle_t t, const gptimer_alarm_config_t* c) {
  if (timerResult("alarm")) return ESP_FAIL;
  t->alarm = c ? c->alarm_count : 0; return ESP_OK;
}
esp_err_t gptimer_start(gptimer_handle_t t) {
  if (timerResult("start") || !t->enabled) return ESP_FAIL;
  t->running = true; t->due = hostNowMs + (t->alarm - t->count) / 1000; return ESP_OK;
}
static void advanceTime(unsigned long next) {
  if (hostTimer.running && hostTimer.alarm && hostTimer.due <= next) {
    hostNowMs = hostTimer.due;
    hostTimer.alarm = 0; // one shot; counter keeps running until main stops it
    hostTimer.callbacks.on_alarm(&hostTimer, nullptr, nullptr);
  }
  hostNowMs = next;
}
static int gate = LOW;
static unsigned long rises = 0;
static unsigned long falls = 0;
static unsigned long imuSamples = 0;
static unsigned long baroSamples = 0;
static char noseAxis = 'z'; // legacy tests; MOUNT y selects confirmed A/B install

#ifdef __APPLE__
extern unsigned char hostRtcStart[] asm("section$start$__DATA$mrcc_rtc");
extern unsigned char hostRtcEnd[] asm("section$end$__DATA$mrcc_rtc");
#else
extern unsigned char hostRtcStart[] asm("__start_mrcc_rtc");
extern unsigned char hostRtcEnd[] asm("__stop_mrcc_rtc");
#endif
static size_t rtcImageSize() {
  return reinterpret_cast<uintptr_t>(hostRtcEnd) - reinterpret_cast<uintptr_t>(hostRtcStart);
}
static bool rtcByteIsPadding(size_t i) {
#ifdef HOST_ASAN
  // Host ASan inserts redzones between globals. They are not RTC payload and
  // must never be copied. Keep sanitizer checks enabled for all actual data.
  return __asan_address_is_poisoned(hostRtcStart + i);
#else
  (void)i;
  return false;
#endif
}
static std::string rtcImage() {
  const char* hex = "0123456789abcdef";
  std::string image;
  for (size_t i = 0; i < rtcImageSize(); ++i) {
    const unsigned char value = rtcByteIsPadding(i) ? 0 : hostRtcStart[i];
    image += hex[value >> 4];
    image += hex[value & 15];
  }
  return image;
}
static void restoreRtc(const std::string& image) {
  if (image.size() != rtcImageSize() * 2 ||
      image.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("invalid RTC image");
  for (size_t i = 0; i < rtcImageSize(); ++i) {
    if (!rtcByteIsPadding(i))
      hostRtcStart[i] = static_cast<unsigned char>(std::stoul(image.substr(i * 2, 2), nullptr, 16));
  }
}

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
    << ",\"rtc_ticks\":" << rtc_time_get()
    << ",\"launch_ticks\":" << hostOriginalLaunchTicks
    << ",\"latch_launch_ms\":" << latchLaunchTime()
    << ",\"rtc_image\":\"" << rtcImage() << "\"}\n";
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
    bool retainedImageLoaded = false;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      std::istringstream in(line);
      std::string command;
      in >> command;
      if (command == "RESET_REASON") {
        in >> hostResetReason;
      } else if (command == "RTC_LOAD") {
        std::string image;
        if (booted || retainedImageLoaded || !(in >> image))
          throw std::runtime_error("RTC_LOAD must precede BOOT");
        restoreRtc(image);
        retainedImageLoaded = true;
      } else if (command == "RTC_CAL") {
        if (booted || !(in >> hostCalibration)) throw std::runtime_error("invalid RTC_CAL");
      } else if (command == "TIMER_FAIL") {
        in >> timerFail;
      } else if (command == "BOOT") {
        unsigned long bootMs, savedLaunch;
        int savedState, savedFired;
        if (booted || !(in >> bootMs >> savedState >> savedFired >> savedLaunch))
          throw std::runtime_error("invalid BOOT");
        // Reconstruct a valid retained latch in a fresh process. All other
        // firmware globals/statics retain their genuine startup initializers.
        uint64_t savedTicks = savedLaunch * 1000ULL, nowTicks = savedTicks + bootMs * 1000ULL;
        if (in.peek() != EOF) in >> savedTicks >> nowTicks;
        if (savedState >= 0) {
          hostOriginalLaunchTicks = savedTicks;
          hostNowMs = savedLaunch;
          hostRtcOffsetUs = savedTicks - hostNowMs * 1000ULL;
          if (!retainedImageLoaded)
            latchWrite(static_cast<uint8_t>(savedState), savedFired != 0, savedLaunch);
        }
        hostNowMs = bootMs;
        hostRtcOffsetUs = savedState >= 0 ? nowTicks - bootMs * 1000ULL : 0;
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
        advanceTime(next);
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
        if (before == FS_ARMED && flightState == FS_BOOST) hostOriginalLaunchTicks = rtc_time_get();
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
      } else if (command == "WAIT") {
        unsigned long next;
        if (!(in >> next) || next <= hostNowMs) throw std::runtime_error("invalid WAIT");
        advanceTime(next); // interrupts only; application loop is not serviced
      } else if (command == "TEST_FIRE") {
        testFirePyro();
        snapshot("test_fire");
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
