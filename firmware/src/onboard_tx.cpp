// onboard_tx.cpp -- ONBOARD telemetry transmitter (Target 2).
//
// Runs on the rocket's ESP32-S3 flight computer. Every 250 ms (4 Hz) it packs the
// current flight state into the shared 32-byte frame and hands it to the E32 for a
// 2.4k-air-rate downlink. Framing + CRC come from lib/TelemPacket (proven
// byte-for-byte identical to shared/protocol/packet.py by test/packet_check.cpp);
// the AUX-safe write comes from lib/E32; per-peripheral fault isolation comes from
// lib/Subsystem (proven by test/subsystem_check.cpp).
//
// FAULT ISOLATION -- the rule this file enforces:
//   No single peripheral can stop the vehicle booting or transmitting.
// A sensor whose init() fails does NOT abort setup(). A sensor that dies in flight
// does NOT stall loop(). Either way its health bit clears, the downlink keeps
// running at 4 Hz, and the ground station is told WHICH device died by name --
// never a blanket "AV FAILED". Dead peripherals are re-init'd every ~2 s, so a
// brownout that clears recovers itself mid-flight.
//
// Air-time budget: 32 B at 2.4 kbps ~= 107 ms on air (more with FEC). That fits the
// 250 ms window and the ~150 B/s link budget (GROUND_STATION_PLAN.md §3): 32 B x 4 Hz
// = 128 B/s. It IS tight -- hence the AUX-with-timeout drop below rather than a stall.
// The health byte rides in an existing packet byte, so none of this costs air time.
#include <Arduino.h>
#include <Wire.h>
#include "E32.h"
#include "TelemPacket.h"
#include "Subsystem.h"
#include "I2CRecover.h"

// ---- pin map (adjust for your ESP32-S3 flight-computer board) --------------
static const int PIN_E32_M0  = 25;
static const int PIN_E32_M1  = 26;
static const int PIN_E32_AUX = 27;
static const int PIN_E32_RX  = 16;  // ESP32 RX1 <- E32 TXD
static const int PIN_E32_TX  = 17;  // ESP32 TX1 -> E32 RXD
static const int PIN_LED     = 2;   // TX activity LED (generic GPIO; no-op if unused)
static const int PIN_I2C_SDA = 21;  // shared sensor bus (baro + IMU)
static const int PIN_I2C_SCL = 22;

// ---- link + timing --------------------------------------------------------
static const uint32_t E32_UART_BAUD    = 9600;   // must match the E32's configured UART baud
static const uint32_t TX_PERIOD_MS     = 250;    // 4 Hz
static const uint32_t TX_AUX_TIMEOUT_MS = 200;   // < period: drop the frame if radio still busy
static const uint32_t LED_PULSE_MS     = 30;

// I2C transaction timeout. NOT optional: bus recovery frees a stuck line, but only
// this bounds a single transfer. Without it a wedged slave hangs Wire -- and a hung
// Wire call inside a sensor read is exactly how one dead sensor kills the loop.
static const uint16_t I2C_TIMEOUT_MS   = 25;

// Set to 1, flash once to program the E32 (9600 UART / 2.4k air rate), then set back
// to 0. The airborne and ground E32s must share these params + channel.
#define E32_RUN_CONFIG 0

E32 radio(Serial1, PIN_E32_M0, PIN_E32_M1, PIN_E32_AUX);

static uint16_t g_seq        = 0;
static uint32_t g_next_tx    = 0;
static uint32_t g_led_off_at = 0;

// ===========================================================================
// SENSOR SEAM -- the part you own.
//
// Each peripheral gets an init() and a read(). Fill them from your real drivers.
// The scheduler in lib/Subsystem handles everything else: boot ordering, fault
// counting, retry cadence, and the health byte.
//
// TWO HARD RULES, and the cascade comes back if you break them:
//   1. init() and read() MUST return in a few ms. Never spin waiting for a part.
//      For I2C that means Wire.setTimeOut() (set in setup()) plus i2cBusRecover().
//   2. read() returns false on a fault -- it does NOT retry internally and does
//      NOT abort. Returning false is how a peripheral reports illness; the
//      scheduler decides when that becomes "dead".
//
// Units must match PROTOCOL.md exactly: dm/s, deg*1e7, V*10, tilt 0..180.
// ===========================================================================

// Latest good values. A peripheral that dies leaves its last reading here and its
// health bit CLEAR -- the bit is authoritative, and the ground station greys the
// field out. We deliberately do NOT zero a dead sensor's field: a sudden 0 m
// altitude reads like "on the ground", which is worse than a stale value that the
// operator can already see is stale.
static struct {
  int16_t baro_alt_m;
  int16_t vspeed_dms;
  int32_t gps_lat;
  int32_t gps_lon;
  int16_t gps_alt_m;
  uint8_t gps_sats;
  uint8_t gps_fix;
  uint8_t tilt_deg;
  uint8_t vbat_dv;
  uint8_t flight_state;
  uint8_t flags;
} g = {
  0, 0,
  32437000, 1017061000,   // launch site 3.2437 N 101.7061 E (demo)
  60, 11, GPS_FIX_3D,
  2, 79,
  FS_PAD,
  FLAG_CONTINUITY | FLAG_ARMED,
};

// --- barometer (I2C) -------------------------------------------------------
static bool baro_init() {
  // Clear a slave that is holding SDA low before trusting the bus at all.
  if (!i2cBusIdle(PIN_I2C_SDA, PIN_I2C_SCL)) {
    i2cBusRecover(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);     // bit-banging took the pins back
    Wire.setTimeOut(I2C_TIMEOUT_MS);
  }
  // TODO(flight team): return bmp.begin(BMP_ADDR) (or your driver's probe).
  return true;
}
static bool baro_read() {
  // TODO(flight team): one bounded read -> g.baro_alt_m (m AGL, zeroed on pad)
  // and g.vspeed_dms (dm/s). Return false if the driver reports an error.
  return true;
}

// --- IMU (I2C, shares the bus with the barometer) --------------------------
static bool imu_init() {
  if (!i2cBusIdle(PIN_I2C_SDA, PIN_I2C_SCL)) {
    i2cBusRecover(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
  }
  // TODO(flight team): return imu.begin().
  return true;
}
static bool imu_read() {
  // TODO(flight team): -> g.tilt_deg (0..180 from vertical).
  return true;
}

// --- GPS (UART -- its own bus, so it cannot wedge the I2C sensors) ----------
static bool gps_init() {
  // TODO(flight team): Serial2.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX).
  return true;
}
static bool gps_read() {
  // TODO(flight team): drain the NMEA/UBX parser -> g.gps_lat/lon/alt/sats/fix.
  // Return false only if the RECEIVER stopped answering. A valid receiver with no
  // satellite lock is healthy hardware reporting gps_fix = 0 -- do not conflate
  // "no fix" with "GPS dead"; they need different actions on the ground.
  return true;
}

// --- SD logging (init-only: mount at boot, health = card present) -----------
static bool sd_init() {
  // TODO(flight team): return SD.begin(PIN_SD_CS).
  return true;
}

// --- pyro continuity sense -------------------------------------------------
static bool pyro_init() { return true; }
static bool pyro_read() {
  // TODO(flight team): read the continuity sense pin -> FLAG_CONTINUITY.
  return true;
}

// --- battery ADC -----------------------------------------------------------
static bool vbat_init() { return true; }
static bool vbat_read() {
  // TODO(flight team): analogRead the divider -> g.vbat_dv (V*10). Return false
  // if the reading is outside a sane range -- that means the divider or ADC is
  // broken, and a bogus battery voltage is worse than a missing one.
  return true;
}

// The roster. Order here = boot order = retry stagger order.
static Subsystem g_subsys[] = {
  SUBSYS("BARO", HEALTH_BARO, baro_init, baro_read),
  SUBSYS("IMU",  HEALTH_IMU,  imu_init,  imu_read),
  SUBSYS("GPS",  HEALTH_GPS,  gps_init,  gps_read),
  SUBSYS("SD",   HEALTH_SD,   sd_init,   nullptr),   // init-only
  SUBSYS("PYRO", HEALTH_PYRO, pyro_init, pyro_read),
  SUBSYS("VBAT", HEALTH_VBAT, vbat_init, vbat_read),
};
static SubsystemSet g_health(g_subsys, sizeof(g_subsys) / sizeof(g_subsys[0]));

// ===========================================================================
// Everything below the seam is done + verified. Leave it alone.
// ===========================================================================

// Log every peripheral's boot result by name. This is the console answer to
// "which one failed to initialise?" -- and it prints even when everything died.
static void logBootHealth() {
  Serial.println("--- peripheral init ---");
  for (uint8_t i = 0; i < g_health.count(); i++) {
    const Subsystem& s = g_health.at(i);
    Serial.printf("  %-5s %s\n", s.name, s.healthy ? "OK" : "FAILED (flying without it)");
  }
  const uint8_t failed = g_health.initFailedMask();
  if (failed == 0) {
    Serial.println("--- all peripherals nominal ---");
  } else {
    // Not fatal. Deliberately not fatal. The vehicle still transmits, and the
    // ground station shows exactly which rows are dead.
    Serial.printf("--- init failures mask=0x%02X -- continuing to fly ---\n", failed);
  }
}

static void logHealthChange(uint8_t changed, uint8_t now_mask) {
  for (uint8_t i = 0; i < g_health.count(); i++) {
    const Subsystem& s = g_health.at(i);
    if (!(changed & s.bit)) continue;
    Serial.printf("HEALTH: %s -> %s\n", s.name, (now_mask & s.bit) ? "RECOVERED" : "LOST");
  }
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Serial.begin(115200);                                   // USB debug (CDC on S3)

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(I2C_TIMEOUT_MS);                        // bounds every transfer

  radio.begin(PIN_E32_RX, PIN_E32_TX, E32_UART_BAUD);     // opens Serial1, enters NORMAL

#if E32_RUN_CONFIG
  bool ok = radio.configure();                            // 9600 UART / 2.4k air rate
  Serial.println(ok ? "E32 configure: OK" : "E32 configure: FAILED (check wiring/AUX)");
#endif

  // Bring up every peripheral. Failures are recorded, never fatal -- begin()
  // always returns, even with all six dead.
  g_health.begin(millis());
  logBootHealth();

  g_next_tx = millis();
}

void loop() {
  uint32_t now = millis();

  if ((int32_t)(now - g_next_tx) >= 0) {
    g_next_tx += TX_PERIOD_MS;                            // fixed 4 Hz cadence, no drift

    // Poll healthy peripherals, retry dead ones on their cadence. Bounded work:
    // a dead device costs a retry every ~2 s, not a stall every cycle.
    uint8_t changed = g_health.service(now);
    uint8_t health  = g_health.healthMask();
    if (changed) logHealthChange(changed, health);

    telem_body_t body;
    body.msg_type     = TELEM_MSG_TELEMETRY;
    body.flight_state = g.flight_state;
    body.onboard_ms   = now;
    body.baro_alt_m   = g.baro_alt_m;
    body.vspeed_dms   = g.vspeed_dms;
    body.gps_lat      = g.gps_lat;
    body.gps_lon      = g.gps_lon;
    body.gps_alt_m    = g.gps_alt_m;
    body.gps_sats     = g.gps_sats;
    body.gps_fix      = g.gps_fix;
    body.tilt_deg     = g.tilt_deg;
    body.vbat_dv      = g.vbat_dv;
    // SD health is both a peripheral bit and a legacy flag: HEALTH_SD = card
    // mounted, FLAG_SD_OK = writes currently succeeding.
    body.flags        = (health & HEALTH_SD) ? (uint8_t)(g.flags | FLAG_SD_OK)
                                             : (uint8_t)(g.flags & ~FLAG_SD_OK);
    body.health       = health;
    body.seq          = g_seq;                            // seq assigned before the write

    uint8_t frame[TELEM_PACKET_SIZE];
    telem_build_frame(&body, frame);

    // AUX-disciplined write: writeFrame() waits for AUX HIGH (timeout) and returns
    // false without writing if the radio is still busy. On success we advance seq so
    // the ground station's loss counter measures RF loss only; on a drop we hold seq
    // and retry next cycle -- we NEVER blind-write the E32.
    if (radio.writeFrame(frame, TELEM_PACKET_SIZE, TX_AUX_TIMEOUT_MS)) {
      g_seq++;
      digitalWrite(PIN_LED, HIGH);
      g_led_off_at = now + LED_PULSE_MS;
      Serial.printf("TX seq=%u state=%u alt=%dm vbat=%.1fV health=0x%02X\n",
                    (unsigned)body.seq, (unsigned)body.flight_state,
                    (int)body.baro_alt_m, body.vbat_dv / 10.0, health);
    } else {
      Serial.println("SKIP: AUX busy -- frame dropped, seq held");
    }
  }

  if (g_led_off_at && (int32_t)(now - g_led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    g_led_off_at = 0;
  }
}
