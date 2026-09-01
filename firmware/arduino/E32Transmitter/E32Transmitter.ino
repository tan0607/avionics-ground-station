/*
  ==========================================================
  E32 TRANSMITTER -- rocket-side telemetry downlink
  ESP32-S3 + ICM-20948 + BMP280 + NEO-M8N  ->  E32 LoRa
  ==========================================================
  The Arduino-IDE twin of firmware/src/onboard_tx.cpp. Same pins, same 32-byte
  frame, same fault isolation -- the only difference is that this one opens in
  the IDE with no PlatformIO involved. Either one talks to E32Receiver and to
  the Python ground station; do not run both on the same board.

  KEPT IN STEP BY HAND. sync_headers.sh guards the copied libraries in this
  folder, but it cannot guard this file against onboard_tx.cpp -- the two have
  different opening comments, so a byte-compare would always fail. Change the
  sensor logic in one and port it to the other in the same commit, or one board
  will fly code the other has never seen.

  ----------------------------------------------------------
  BEFORE THE FIRST COMPILE -- install four libraries
  ----------------------------------------------------------
  Tools > Manage Libraries, then search and install:
      SparkFun 9DoF IMU Breakout - ICM 20948   (by SparkFun)
      Adafruit BMP280 Library                  (by Adafruit)
      Adafruit Unified Sensor                  (by Adafruit -- BMP280 needs it)
      TinyGPSPlus                              (by Mikal Hart)
  The IDE offers to pull Adafruit's dependencies for you; say yes.

  Board: Tools > Board > esp32 > "ESP32S3 Dev Module".
  Serial Monitor at 115200. If nothing prints, set
  Tools > USB CDC On Boot > Enabled -- on the S3, `Serial` is USB.

  ----------------------------------------------------------
  THE OTHER FILES IN THIS FOLDER ARE COPIES. DO NOT EDIT THEM.
  ----------------------------------------------------------
  The Arduino IDE compiles exactly one directory, so TelemPacket.h, E32.*,
  Subsystem.* and I2CRecover.* are duplicated here from firmware/lib/. Editing a
  copy makes this board disagree with the ground station about what byte 22
  means. Change the original under firmware/lib/, then run
      ./firmware/arduino/sync_headers.sh
  ==========================================================
*/

// This sketch is ESP32-only, and not by accident: it needs a second hardware
// UART for the GPS (Serial2) on top of the one the E32 uses, remappable I2C
// pins, and ~330 kB of flash. An Uno has one UART and 32 kB. Porting means
// SoftwareSerial for the GPS, which cannot receive while it transmits.
#if !defined(ARDUINO_ARCH_ESP32)
  #error "E32Transmitter requires an ESP32 (S3 flight computer). Select an ESP32 board."
#endif

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
#include <math.h>
#include "E32.h"
#include "TelemPacket.h"
#include "Subsystem.h"
#include "I2CRecover.h"

// Flight sensors -- install these from Tools > Manage Libraries (see the header).
#include "ICM_20948.h"          // SparkFun ICM-20948 (accel + gyro + mag)
#include <Adafruit_BMP280.h>    // barometric altimeter
#include <TinyGPS++.h>          // NEO-M8N NMEA parser

// ---- pin map (ESP32-S3 flight computer) ------------------------------------
// CHANGED from the pre-sensor version of this file, for two reasons you cannot
// work around by rewiring:
//   1. The old E32 map (M0=25, M1=26, AUX=27) is an ESP32-*classic* map. On the
//      S3, GPIO26-32 are the SPI flash / PSRAM pins -- driving them either does
//      nothing or hangs the chip. The E32 control pins move to 10/11/12.
//   2. The old E32 UART (RX=16, TX=17) collides with the GPS: the bench wiring
//      has the NEO-M8N on GPIO17/18. The E32 UART moves to 13/14 and the GPS
//      keeps 17/18, so the two radios never share a pin.
// The I2C bus is 8/9 to match the bench wiring (ICM-20948 + BMP280 in parallel).
static const int PIN_E32_M0  = 10;
static const int PIN_E32_M1  = 11;
static const int PIN_E32_AUX = 12;
static const int PIN_E32_RX  = 13;  // ESP32 RX1 <- E32 TXD
static const int PIN_E32_TX  = 14;  // ESP32 TX1 -> E32 RXD
static const int PIN_LED     = 2;   // TX activity LED (generic GPIO; no-op if unused)
static const int PIN_I2C_SDA = 8;   // shared sensor bus (baro + IMU)
static const int PIN_I2C_SCL = 9;
static const int PIN_GPS_RX  = 17;  // ESP32 RX2 <- NEO-M8N TXD
static const int PIN_GPS_TX  = 18;  // ESP32 TX2 -> NEO-M8N RXD

// ---- sensor addresses / tuning --------------------------------------------
static const uint8_t ICM_AD0_VAL  = 0;        // AD0 -> GND = 0x68 (1 = 0x69)
static const uint8_t BMP_ADDR     = 0x76;     // SDO -> GND = 0x76 (0x77 if VCC)
static const uint32_t GPS_BAUD    = 9600;     // NEO-M8N factory default

// Sea-level reference. Only affects the *absolute* MSL number we subtract the
// pad from; baro_alt_m is AGL, so a wrong QNH cancels out of the downlink.
static const float SEA_LEVEL_HPA  = 1013.25f;

// Pad zeroing: the ground reference keeps tracking for this long after the first
// good sample (weather drift on the pad), but only while state == PAD, so the
// reference can never chase the rocket up.
static const uint32_t BARO_ZERO_WINDOW_MS = 20000;
static const float    BARO_ZERO_ALPHA     = 0.05f;   // per-sample pull toward now
static const float    BARO_ALT_ALPHA      = 0.30f;   // altitude smoothing
static const float    BARO_VSPEED_ALPHA   = 0.35f;   // vspeed smoothing

// Liveness windows. A device that stops answering for this long is a fault, and
// three consecutive faults (lib/Subsystem) clear its health bit.
static const uint32_t IMU_STALE_MS     = 500;   // no fresh IMU sample = fault
static const uint32_t GPS_SILENCE_MS   = 3000;  // no NMEA bytes at all = fault
static const uint32_t GPS_FIX_AGE_MS   = 3000;  // older than this = not a fix

// Bytes drained from the GPS UART per pass. At 9600 baud a 250 ms window holds
// ~240 bytes; the cap exists so a stuck-high RX line cannot spin loop() forever.
static const uint16_t GPS_DRAIN_BUDGET = 512;

// ---- link + timing --------------------------------------------------------
static const uint32_t E32_UART_BAUD    = 9600;   // must match the E32's configured UART baud
static const uint32_t TX_PERIOD_MS     = 250;    // 4 Hz
static const uint32_t TX_AUX_TIMEOUT_MS = 200;   // < period: drop the frame if radio still busy
static const uint32_t LED_PULSE_MS     = 30;

// I2C transaction timeout. NOT optional: bus recovery frees a stuck line, but only
// this bounds a single transfer. Without it a wedged slave hangs Wire -- and a hung
// Wire call inside a sensor read is exactly how one dead sensor kills the loop.
static const uint16_t I2C_TIMEOUT_MS   = 25;
static const uint32_t I2C_CLOCK_HZ     = 400000;  // fast mode; both sensors support it

// Set to 1, flash once to program the E32 (9600 UART / 2.4k air rate), then set back
// to 0. The airborne and ground E32s must share these params + channel.
#define E32_RUN_CONFIG 0

E32 radio(Serial1, PIN_E32_M0, PIN_E32_M1, PIN_E32_AUX);

// Sensors. Serial1 belongs to the E32, so the GPS gets Serial2.
static ICM_20948_I2C    imu;
static Adafruit_BMP280  baro(&Wire);
static TinyGPSPlus      gps;
#define GPS_PORT Serial2

static uint16_t g_seq        = 0;
static uint32_t g_next_tx    = 0;
static uint32_t g_led_off_at = 0;

// ===========================================================================
// SENSOR SEAM -- the part you own.
//
// Each peripheral gets an init() and a read(). BARO / IMU / GPS are implemented
// against the real flight hardware (BMP280, ICM-20948, NEO-M8N); SD / PYRO / VBAT
// are still stubs that report healthy -- fill them or their health bits will lie.
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
  // 0/0 until the first fix. NOT the launch-site coordinates: a plausible-looking
  // position that is actually a placeholder is worse than an obvious null, and the
  // dashboard already discards (0,0) as well as any fix below 2D
  // (dashboard/src/hooks/useGroundTrack.ts). gps_fix is the authoritative flag.
  0, 0,
  0, 0, GPS_FIX_NONE,
  0, 0,
  FS_PAD,
  FLAG_CONTINUITY | FLAG_ARMED,
};

// Debug-only IMU state. None of this rides on the wire -- the 28-byte body has no
// room for it -- but it is what makes the USB console useful on the bench, and it
// is exactly what the standalone sensor sketch printed.
static struct {
  float ax, ay, az;      // g
  float mx, my, mz;      // uT
  float roll_deg, pitch_deg, heading_deg;
  bool  mag_ok;
} g_imu_dbg = {0, 0, 0, 0, 0, 0, 0, 0, 0, false};

// Magnetic declination for the launch site, degrees (+east / -west).
// KL ~ +0.5, Singapore ~ +0.3, London ~ +0.1, New York ~ -13.0, Sydney ~ +12.5.
static const float MAGNETIC_DECLINATION = 0.5f;

// --- I2C bus custody -------------------------------------------------------
// Re-own the SDA/SCL pads after ANY bus probe or recovery. Not optional, and the
// reason is buried in two places in the ESP32 core:
//
//   1. i2cBusIdle() and i2cBusRecover() both call pinMode(), which lands in
//      gpio_config(): the pad's IOMUX is switched to plain GPIO and the
//      output-signal matrix entry is reset to SIG_GPIO_OUT_IDX. The I2C
//      peripheral can no longer drive SDA/SCL. This happens even on the HEALTHY
//      path -- i2cBusIdle() touches both pins before it has any idea whether the
//      bus was stuck.
//   2. Wire.begin(sda, scl) does NOT undo it. With the bus still initialised it
//      logs "Bus already started in Master Mode." and returns early, never
//      reaching initPins() (arduino-esp32 2.x, libraries/Wire/src/Wire.cpp).
//      The bus must be torn down first.
//
// Without this, one peripheral asking "is the bus stuck?" silently kills I2C for
// every device on it: BARO faults, its 2 s retry runs, and the IMU dies with it.
// That is the exact cascade lib/Subsystem exists to prevent, arriving through the
// back door -- not through a stalled read, but through a stolen pin.
static void i2cReclaim() {
  Wire.end();                                              // drop the peripheral
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);      // ...and re-own the pads
  Wire.setTimeOut(I2C_TIMEOUT_MS);                         // bounds every transfer
}

// Probe the bus, unwedge it if a slave is holding SDA, and hand it back to the
// I2C peripheral. Every init() below starts here.
static void i2cPrepare() {
  if (!i2cBusIdle(PIN_I2C_SDA, PIN_I2C_SCL))
    i2cBusRecover(PIN_I2C_SDA, PIN_I2C_SCL);
  i2cReclaim();     // ALWAYS -- the probe itself took the pins, stuck or not
}

// --- barometer (I2C) -------------------------------------------------------
static bool     g_baro_zeroed    = false;
static float    g_baro_ground_m  = 0.0f;   // pad reference, m MSL
static float    g_baro_alt_m     = 0.0f;   // smoothed, m MSL
static float    g_baro_vspeed_ms = 0.0f;   // smoothed, m/s
static uint32_t g_baro_last_ms   = 0;
static uint32_t g_baro_zero_start_ms = 0;

static bool baro_init() {
  i2cPrepare();                               // unwedge if stuck, always re-own the pads
  if (!baro.begin(BMP_ADDR)) return false;

  // X16 pressure + X16 IIR at 1 ms standby: the quietest altitude this part can
  // give, which matters because vspeed is a derivative and differentiating a
  // noisy altitude is how you get a 20 m/s reading on the pad.
  baro.setSampling(Adafruit_BMP280::MODE_NORMAL,
                   Adafruit_BMP280::SAMPLING_X2,    // temperature
                   Adafruit_BMP280::SAMPLING_X16,   // pressure
                   Adafruit_BMP280::FILTER_X16,
                   Adafruit_BMP280::STANDBY_MS_1);

  // Deliberately NO averaging loop here. The pre-flight version of this sketch
  // took 10 readings 100 ms apart; that is a 1 s blocking init, and init() is
  // also the mid-flight retry path -- a browned-out baro would stall the 4 Hz
  // downlink every 2 s. The pad reference is established from the read path
  // instead (see baro_read), which costs nothing and keeps tracking drift.
  //
  // Note what is NOT reset: g_baro_zeroed. This function is the recovery path
  // too, and a baro that browns out at 800 m and comes back would otherwise
  // re-zero its ground reference *at 800 m* -- the downlink would report 0 m AGL
  // for a rocket still under canopy. The pad reference is established once.
  g_baro_last_ms = 0;      // forces the dt guard to skip one vspeed sample
  return true;
}

static bool baro_read() {
  float pa = baro.readPressure();                 // Pa
  // A dead/unplugged BMP280 reads as 0 or a stuck value, not as an error return,
  // so range-check it: 300..1200 hPa covers sea level to well past our apogee.
  if (!(pa > 30000.0f && pa < 120000.0f) || isnan(pa)) return false;

  // Same formula Adafruit_BMP280::readAltitude uses, computed from the pressure
  // we already have -- readAltitude() would re-read the sensor over I2C.
  float alt = 44330.0f * (1.0f - powf((pa / 100.0f) / SEA_LEVEL_HPA, 0.1903f));
  uint32_t now = millis();

  if (!g_baro_zeroed) {
    g_baro_ground_m      = alt;
    g_baro_alt_m         = alt;
    g_baro_zero_start_ms = now;
    g_baro_last_ms       = now;
    g_baro_zeroed        = true;
  } else {
    // Track pad drift, but ONLY on the pad and only for the opening window --
    // otherwise the reference chases the rocket and AGL flatlines near zero.
    if (g.flight_state == FS_PAD && (now - g_baro_zero_start_ms) < BARO_ZERO_WINDOW_MS)
      g_baro_ground_m += (alt - g_baro_ground_m) * BARO_ZERO_ALPHA;

    float prev = g_baro_alt_m;
    g_baro_alt_m += (alt - g_baro_alt_m) * BARO_ALT_ALPHA;

    float dt = (now - g_baro_last_ms) / 1000.0f;
    if (dt > 0.02f && dt < 2.0f) {                // ignore absurd dt after a stall
      float v = (g_baro_alt_m - prev) / dt;
      g_baro_vspeed_ms += (v - g_baro_vspeed_ms) * BARO_VSPEED_ALPHA;
    }
    g_baro_last_ms = now;
  }

  float agl = g_baro_alt_m - g_baro_ground_m;
  g.baro_alt_m = (int16_t)constrain(lroundf(agl), -32768L, 32767L);
  g.vspeed_dms = (int16_t)constrain(lroundf(g_baro_vspeed_ms * 10.0f), -32768L, 32767L);
  return true;
}

// --- IMU (I2C, shares the bus with the barometer) --------------------------
static uint32_t g_imu_last_ok_ms = 0;
static bool     g_mag_attempted  = false;

static bool imu_init() {
  i2cPrepare();                               // unwedge if stuck, always re-own the pads

  imu.begin(Wire, ICM_AD0_VAL);
  if (imu.status != ICM_20948_Stat_Ok) return false;

  // Ranges sized for flight, not for a desk: +/-8 g clips a J-motor boost less
  // often than +/-4 g, and 1000 dps covers a coning/spin recovery.
  ICM_20948_fss_t fss;
  fss.a = gpm8;
  fss.g = dps1000;
  imu.setFullScale((ICM_20948_Internal_Acc | ICM_20948_Internal_Gyr), fss);
  if (imu.status != ICM_20948_Stat_Ok) return false;

  ICM_20948_dlpcfg_t dlp;
  dlp.a = acc_d473bw_n499bw;
  dlp.g = gyr_d361bw4_n376bw5;
  imu.setDLPFcfg((ICM_20948_Internal_Acc | ICM_20948_Internal_Gyr), dlp);
  imu.enableDLPF(ICM_20948_Internal_Acc, true);
  imu.enableDLPF(ICM_20948_Internal_Gyr, true);

  // The magnetometer is a separate die behind an I2C master inside the ICM, and
  // it is the flaky one. Two rules, both learned the hard way:
  //
  //   Its failure must NOT fail imu_init(). tilt_deg -- the only IMU field on the
  //   wire -- comes from the accelerometer alone. A dead mag costs the debug
  //   heading and nothing else.
  //
  //   It is attempted EXACTLY ONCE, ever. startupMagnetometer() retries up to
  //   MAX_MAGNETOMETER_STARTS (10) times, each with an i2cMasterReset() and a
  //   delay(10); against a mag that never answers that is several hundred ms to
  //   over a second. imu_init() is also the 2 s retry path, so leaving it in
  //   would blow the 250 ms TX window on repeat -- a debug-only field stalling
  //   the downlink. Once is diagnosis; every 2 s is a fault of its own.
  if (!g_mag_attempted) {
    g_mag_attempted = true;
    imu.startupMagnetometer();
    g_imu_dbg.mag_ok = (imu.status == ICM_20948_Stat_Ok);
    imu.status = ICM_20948_Stat_Ok;  // do not let the mag poison the return
    if (!g_imu_dbg.mag_ok)
      Serial.println("IMU: magnetometer did not start -- heading disabled, tilt unaffected");
  }

  g_imu_last_ok_ms = millis();
  return true;
}

// Tilt-compensated magnetic heading, degrees 0..360. Debug/console only.
static float magneticHeading(float mx, float my, float mz, float roll, float pitch) {
  float cosR = cosf(roll),  sinR = sinf(roll);
  float cosP = cosf(pitch), sinP = sinf(pitch);
  float mx2 = mx * cosP + mz * sinP;
  float my2 = mx * sinR * sinP + my * cosR - mz * sinR * cosP;
  float h = atan2f(-my2, mx2) * 180.0f / PI + MAGNETIC_DECLINATION;
  if (h < 0)      h += 360.0f;
  if (h >= 360.0f) h -= 360.0f;
  return h;
}

static bool imu_read() {
  uint32_t now = millis();

  // No new sample yet is not a fault -- the ICM runs its own ODR. Prolonged
  // silence IS one: a part that has stopped answering never asserts data-ready,
  // so without this window a dead IMU would look permanently "just not ready".
  if (!imu.dataReady()) return (now - g_imu_last_ok_ms) < IMU_STALE_MS;

  imu.getAGMT();
  if (imu.status != ICM_20948_Stat_Ok) return false;

  float ax = imu.accX() / 1000.0f;   // mg -> g
  float ay = imu.accY() / 1000.0f;
  float az = imu.accZ() / 1000.0f;

  // Tilt from vertical, 0..180. Assumes the board's +Z axis points along the
  // airframe (nose up on the pad) -- if it is mounted on its side, permute these
  // three names and nothing else in this file changes.
  float lateral = sqrtf(ax * ax + ay * ay);
  float tilt    = atan2f(lateral, az) * 180.0f / PI;
  g.tilt_deg    = (uint8_t)constrain(lroundf(tilt), 0L, 180L);

  g_imu_dbg.ax = ax; g_imu_dbg.ay = ay; g_imu_dbg.az = az;
  float roll  = atan2f(ay, az);
  float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
  g_imu_dbg.roll_deg  = roll  * 180.0f / PI;
  g_imu_dbg.pitch_deg = pitch * 180.0f / PI;

  if (g_imu_dbg.mag_ok) {
    g_imu_dbg.mx = imu.magX();
    g_imu_dbg.my = imu.magY();
    g_imu_dbg.mz = imu.magZ();
    g_imu_dbg.heading_deg = magneticHeading(g_imu_dbg.mx, g_imu_dbg.my, g_imu_dbg.mz,
                                            roll, pitch);
  }

  g_imu_last_ok_ms = now;
  return true;
}

// --- GPS (UART -- its own bus, so it cannot wedge the I2C sensors) ----------
static uint32_t g_gps_last_char_ms = 0;
static uint32_t g_gps_init_ms      = 0;

static bool gps_init() {
  GPS_PORT.end();                                  // idempotent: this is also the retry path
  GPS_PORT.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  g_gps_init_ms      = millis();
  g_gps_last_char_ms = 0;
  return true;   // a UART always opens; whether anything is ON it is gps_read's call
}

// Drain the UART into the NMEA parser. Called every loop pass, not just at 4 Hz:
// the ESP32 RX FIFO is 128 bytes and the NEO-M8N emits ~500 B/s, so a 250 ms gap
// between drains loses sentences. Bounded so a shorted RX line cannot spin here.
static void gps_drain() {
  uint16_t budget = GPS_DRAIN_BUDGET;
  while (GPS_PORT.available() > 0 && budget-- > 0) {
    gps.encode((char)GPS_PORT.read());
    g_gps_last_char_ms = millis();
  }
}

static bool gps_read() {
  uint32_t now = millis();

  // Health here means "the RECEIVER is answering", judged purely on bytes
  // arriving -- never on fix quality. A module sitting indoors with no lock is
  // healthy hardware reporting gps_fix = 0; conflating that with a dead GPS
  // sends the operator hunting a wiring fault that does not exist.
  if (g_gps_last_char_ms == 0) return (now - g_gps_init_ms) < GPS_SILENCE_MS;
  if ((now - g_gps_last_char_ms) > GPS_SILENCE_MS) return false;

  g.gps_sats = gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;

  if (gps.location.isValid() && gps.location.age() < GPS_FIX_AGE_MS) {
    g.gps_lat = (int32_t)lround(gps.location.lat() * 1e7);
    g.gps_lon = (int32_t)lround(gps.location.lng() * 1e7);
    if (gps.altitude.isValid() && gps.altitude.age() < GPS_FIX_AGE_MS) {
      g.gps_alt_m = (int16_t)constrain(lround(gps.altitude.meters()), -32768L, 32767L);
      g.gps_fix   = GPS_FIX_3D;
    } else {
      g.gps_fix = GPS_FIX_2D;      // horizontal only; leave the last altitude alone
    }
  } else {
    // Lock lost. Hold the last known position (a sudden 0,0 reads as "the rocket
    // is off West Africa") and let gps_fix say it is stale.
    g.gps_fix = GPS_FIX_NONE;
  }
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

  i2cReclaim();                                           // opens Wire on 8/9, 400 kHz, bounded

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

  // Every pass, not inside the 4 Hz block: the NEO-M8N outruns the ESP32's
  // 128-byte RX FIFO in well under a transmit period.
  gps_drain();

  if ((int32_t)(now - g_next_tx) >= 0) {
    g_next_tx += TX_PERIOD_MS;                            // fixed 4 Hz cadence, no drift

    // Poll healthy peripherals, retry dead ones on their cadence. Bounded work:
    // a dead device costs a retry every ~2 s, not a stall every cycle.
    uint32_t service_start = millis();
    uint8_t changed = g_health.service(now);
    uint8_t health  = g_health.healthMask();
    if (changed) logHealthChange(changed, health);

    // The contract above says every init()/read() returns in a few ms. This is
    // the thing that tells you when one has quietly stopped honouring it -- a
    // driver that blocks shows up here as a number, long before it shows up as
    // a downlink that has mysteriously gone quiet.
    uint32_t service_ms = millis() - service_start;
    if (service_ms > TX_PERIOD_MS / 2)
      Serial.printf("WARN: sensor service took %lu ms of the %lu ms window\n",
                    (unsigned long)service_ms, (unsigned long)TX_PERIOD_MS);

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
      // One line per frame: everything that went on the wire, plus the heading
      // that did not fit in 28 bytes. This is the bench view the standalone
      // sensor sketch gave, minus the parts the ground station renders better.
      Serial.printf("TX seq=%u st=%u alt=%dm vs=%.1fm/s tilt=%u° "
                    "gps=%.6f,%.6f %usat/fix%u hdg=%.0f° vbat=%.1fV health=0x%02X\n",
                    (unsigned)body.seq, (unsigned)body.flight_state,
                    (int)body.baro_alt_m, body.vspeed_dms / 10.0,
                    (unsigned)body.tilt_deg,
                    body.gps_lat / 1e7, body.gps_lon / 1e7,
                    (unsigned)body.gps_sats, (unsigned)body.gps_fix,
                    g_imu_dbg.heading_deg, body.vbat_dv / 10.0, health);
    } else {
      Serial.println("SKIP: AUX busy -- frame dropped, seq held");
    }
  }

  if (g_led_off_at && (int32_t)(now - g_led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    g_led_off_at = 0;
  }
}
