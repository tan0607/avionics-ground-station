// subsystem_check.cpp -- host-side proof of the fault-isolation policy.
//
// Proves, with no hardware, the guarantees lib/Subsystem exists to make:
//   1. An init failure does NOT abort boot -- the other peripherals still come
//      up and the set stays usable.
//   2. A device that never initialised is reported as an INIT failure,
//      distinctly from one that worked and later died.
//   3. A single bad read does not drop a good sensor; SUBSYS_FAULT_THRESHOLD
//      consecutive ones do.
//   4. A dead device is retried on a cadence, not hammered every loop.
//   5. A device that starts answering again recovers by itself, mid-flight.
//   6. Every peripheral failing at once still leaves a live, transmitting set.
//
// Build & run on the laptop (no ESP32 toolchain needed):
//   cc -std=c++11 -I firmware/lib/TelemPacket -I firmware/lib/Subsystem \
//      firmware/test/subsystem_check.cpp firmware/lib/Subsystem/Subsystem.cpp \
//      -lstdc++ -o firmware/test/subsystem_check
//   ./firmware/test/subsystem_check
#include <cstdio>
#include "Subsystem.h"
#include "TelemPacket.h"

static int failures = 0;

static void check(bool cond, const char* what) {
  printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) failures++;
}

// --- fake peripherals: globals drive whether they answer ---------------------
static bool g_baro_up = true,  g_baro_reads = true;
static bool g_imu_up  = true,  g_imu_reads  = true;
static int  g_baro_init_calls = 0;

static bool baro_init() { g_baro_init_calls++; return g_baro_up; }
static bool baro_read() { return g_baro_reads; }
static bool imu_init()  { return g_imu_up; }
static bool imu_read()  { return g_imu_reads; }

int main() {
  Subsystem items[] = {
      SUBSYS("BARO", HEALTH_BARO, baro_init, baro_read),
      SUBSYS("IMU",  HEALTH_IMU,  imu_init,  imu_read),
  };
  SubsystemSet set(items, 2);

  // ---- 1 + 2: BARO fails to init; IMU must still come up ------------------
  g_baro_up = false;
  uint32_t t = 1000;
  uint8_t mask = set.begin(t);

  check(mask == HEALTH_IMU, "boot with dead BARO -> IMU still healthy");
  check(set.initFailedMask() == HEALTH_BARO, "dead BARO reported as INIT failure");
  check(items[0].ever_healthy == false, "BARO never_healthy (never came up)");
  check(items[1].ever_healthy == true, "IMU ever_healthy");

  // ---- 4: the dead BARO is retried on a cadence, not every tick ------------
  int calls_before = g_baro_init_calls;
  for (int i = 0; i < 8; i++) set.service(t += 100);      // 800 ms of ticks
  check(g_baro_init_calls == calls_before,
        "dead BARO not retried before SUBSYS_RETRY_MS");

  // ---- 5: BARO starts answering -> it recovers on its own ------------------
  g_baro_up = true;
  uint8_t changed = 0;
  for (int i = 0; i < 40; i++) {                          // push past the retry
    changed = set.service(t += 100);
    if (changed) break;
  }
  check(changed == HEALTH_BARO, "BARO recovery reported as a health transition");
  check(set.healthMask() == (HEALTH_BARO | HEALTH_IMU), "both healthy after recovery");
  check(set.initFailedMask() == 0, "recovered BARO no longer an init failure");

  // ---- 3: one bad read must NOT drop a good sensor ------------------------
  g_imu_reads = false;
  changed = set.service(t += 100);
  check(changed == 0 && set.healthMask() == (HEALTH_BARO | HEALTH_IMU),
        "single failed read does not drop IMU");

  // ...but a streak does, on exactly the threshold-th consecutive failure.
  for (uint8_t i = 1; i < SUBSYS_FAULT_THRESHOLD; i++) changed = set.service(t += 100);
  check(changed == HEALTH_IMU, "IMU dropped at SUBSYS_FAULT_THRESHOLD faults");
  check(set.healthMask() == HEALTH_BARO, "only IMU's bit cleared -- BARO untouched");
  check(set.initFailedMask() == 0, "in-flight death is NOT an init failure");

  // A good read partway through a streak clears it (no slow drift to dead).
  g_imu_reads = true;
  for (int i = 0; i < 40; i++) if (set.service(t += 100)) break;
  check(set.healthMask() == (HEALTH_BARO | HEALTH_IMU), "IMU recovered after reads resume");
  g_imu_reads = false;
  set.service(t += 100);
  g_imu_reads = true;
  set.service(t += 100);
  check(items[1].consecutive_faults == 0, "a good read clears the fault streak");

  // ---- 6: everything dead at once still returns a usable, live set --------
  g_baro_up = g_baro_reads = g_imu_up = g_imu_reads = false;
  for (int i = 0; i < 60; i++) set.service(t += 100);
  check(set.healthMask() == 0, "all peripherals dead -> health mask 0");
  check(set.count() == 2, "set still usable with every peripheral dead");
  // Reaching this line at all is the real assertion: no hang, no abort.

  printf("\n%s\n", failures == 0
      ? "PASS: one dead peripheral degrades one bit -- never the whole vehicle"
      : "FAIL: fault isolation is broken");
  return failures == 0 ? 0 : 1;
}
