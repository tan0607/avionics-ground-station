// onboard_tx.cpp -- ONBOARD telemetry transmitter (Target 2).
//
// Runs on the rocket's ESP32-S3 flight computer. Every 250 ms (4 Hz) it packs the
// current flight state into the shared 32-byte frame and hands it to the E32 for a
// 2.4k-air-rate downlink. Framing + CRC come from lib/TelemPacket (proven
// byte-for-byte identical to shared/protocol/packet.py by test/packet_check.cpp);
// the AUX-safe write comes from lib/E32.
//
// This file is the RADIO + PACKET layer. On the real vehicle it drops into your
// flight-computer firmware: keep read_sensors() as the single seam where your
// sensors/state-machine fill the body, and leave everything below it alone.
//
// Air-time budget: 32 B at 2.4 kbps ~= 107 ms on air (more with FEC). That fits the
// 250 ms window and the ~150 B/s link budget (GROUND_STATION_PLAN.md §3): 32 B x 4 Hz
// = 128 B/s. It IS tight -- hence the AUX-with-timeout drop below rather than a stall.
#include <Arduino.h>
#include "E32.h"
#include "TelemPacket.h"

// ---- pin map (adjust for your ESP32-S3 flight-computer board) --------------
static const int PIN_E32_M0  = 25;
static const int PIN_E32_M1  = 26;
static const int PIN_E32_AUX = 27;
static const int PIN_E32_RX  = 16;  // ESP32 RX1 <- E32 TXD
static const int PIN_E32_TX  = 17;  // ESP32 TX1 -> E32 RXD
static const int PIN_LED     = 2;   // TX activity LED (generic GPIO; no-op if unused)

// ---- link + timing --------------------------------------------------------
static const uint32_t E32_UART_BAUD    = 9600;   // must match the E32's configured UART baud
static const uint32_t TX_PERIOD_MS     = 250;    // 4 Hz
static const uint32_t TX_AUX_TIMEOUT_MS = 200;   // < period: drop the frame if radio still busy
static const uint32_t LED_PULSE_MS     = 30;

// Set to 1, flash once to program the E32 (9600 UART / 2.4k air rate), then set back
// to 0. The airborne and ground E32s must share these params + channel.
#define E32_RUN_CONFIG 0

E32 radio(Serial1, PIN_E32_M0, PIN_E32_M1, PIN_E32_AUX);

static uint16_t g_seq       = 0;
static uint32_t g_next_tx   = 0;
static uint32_t g_led_off_at = 0;

// ---------------------------------------------------------------------------
// SENSOR SEAM -- the one function you own.
//
// TODO(flight team): replace this demo with real reads and your state machine:
//   - baro_alt_m : barometer altitude, m AGL (zeroed on the pad)
//   - vspeed_dms : vertical speed in dm/s  (m/s * 10)
//   - gps_*      : NEO-M8N lat/lon (deg * 1e7), alt (m MSL), sats, fix (0/2/3)
//   - tilt_deg   : IMU tilt from vertical, 0..180
//   - vbat_dv    : battery volts * 10 (from an ADC divider)
//   - flight_state : FS_PAD/BOOST/COAST/APOGEE/DROGUE/MAIN/LANDED
//   - flags      : FLAG_CONTINUITY | FLAG_PYRO_FIRED | FLAG_SD_OK | FLAG_ARMED
// Keep the UNITS exactly as above (they match PROTOCOL.md). Do NOT set b->seq here --
// the TX loop owns the sequence counter. Everything below this seam is done + verified.
static void read_sensors(telem_body_t* b) {
  b->msg_type     = TELEM_MSG_TELEMETRY;
  b->flight_state = FS_PAD;
  b->onboard_ms   = millis();
  b->baro_alt_m   = 0;
  b->vspeed_dms   = 0;
  b->gps_lat      = 32437000;    // launch site 3.2437 N (demo)
  b->gps_lon      = 1017061000;  //             101.7061 E
  b->gps_alt_m    = 60;
  b->gps_sats     = 11;
  b->gps_fix      = GPS_FIX_3D;
  b->tilt_deg     = 2;
  b->vbat_dv      = 79;          // 7.9 V
  b->flags        = FLAG_CONTINUITY | FLAG_SD_OK | FLAG_ARMED;
  b->reserved     = 0;
}
// ---------------------------------------------------------------------------

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Serial.begin(115200);                                   // USB debug (CDC on S3)
  radio.begin(PIN_E32_RX, PIN_E32_TX, E32_UART_BAUD);     // opens Serial1, enters NORMAL

#if E32_RUN_CONFIG
  bool ok = radio.configure();                            // 9600 UART / 2.4k air rate
  Serial.println(ok ? "E32 configure: OK" : "E32 configure: FAILED (check wiring/AUX)");
#endif
  g_next_tx = millis();
}

void loop() {
  uint32_t now = millis();

  if ((int32_t)(now - g_next_tx) >= 0) {
    g_next_tx += TX_PERIOD_MS;                            // fixed 4 Hz cadence, no drift

    telem_body_t body;
    read_sensors(&body);
    body.seq = g_seq;                                     // seq assigned before the write

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
      Serial.printf("TX seq=%u state=%u alt=%dm vbat=%.1fV\n",
                    (unsigned)body.seq, (unsigned)body.flight_state,
                    (int)body.baro_alt_m, body.vbat_dv / 10.0);
    } else {
      Serial.println("SKIP: AUX busy -- frame dropped, seq held");
    }
  }

  if (g_led_off_at && (int32_t)(now - g_led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    g_led_off_at = 0;
  }
}
