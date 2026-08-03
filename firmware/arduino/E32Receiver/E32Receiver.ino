// E32Receiver.ino -- standalone GROUND-SIDE receiver for the rocket telemetry link.
//
//   rocket E32  ~~433 MHz~~>  E32  --UART-->  Arduino  --USB-->  Serial Monitor
//
// Unlike firmware/src/bridge.cpp (which forwards raw bytes to the Python backend
// and parses NOTHING), this sketch is self-contained: it finds frames, checks
// CRC, unpacks every field, and prints human-readable telemetry. No laptop
// backend required -- open the Serial Monitor at 115200 and you have a receiver.
//
// Use it to prove the RF link works before wiring the real ground station up.
//
// ---------------------------------------------------------------------------
// BOARD SUPPORT
//   Arduino Uno / Nano  -- SoftwareSerial (bench testing)
//   Arduino Mega 2560   -- hardware Serial1
//   ESP32 DevKitC       -- hardware Serial2, same pin map as bridge.cpp
// Pick your board in the IDE; the pin map below switches automatically.
//
// ---------------------------------------------------------------------------
// !! 5 V BOARDS NEED A LEVEL SHIFTER !!
// The E32 is a 3.3 V part. On a Uno/Nano/Mega:
//   - E32 TXD -> Arduino RX  is fine (3.3 V clears the 5 V board's ~3.0 V V_IH,
//     though with little margin -- if RX is flaky, that margin is why).
//   - Arduino TX -> E32 RXD  MUST be divided down (e.g. 1k series + 2k to GND)
//     or you will damage the module. A pure receiver never drives this line, so
//     you can leave it disconnected UNLESS you set E32_RUN_CONFIG = 1 below.
//   - E32 VCC is 3.3 V, NOT 5 V.
//
// ---------------------------------------------------------------------------
// The wire format is NOT defined here. TelemPacket.h is a byte-for-byte copy of
// firmware/lib/TelemPacket/TelemPacket.h, which mirrors shared/protocol/packet.py
// (the source of truth). Re-sync it with ./sync_headers.sh after any protocol
// change -- see README.md in this folder.
#include "TelemPacket.h"
#include "E32Link.h"

// ---- pin map --------------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)
  // Identical to firmware/src/bridge.cpp so the two boards are interchangeable.
  static const int PIN_E32_M0  = 25;
  static const int PIN_E32_M1  = 26;
  static const int PIN_E32_AUX = 27;
  static const int PIN_E32_RX  = 16;  // ESP32 RX2 <- E32 TXD
  static const int PIN_E32_TX  = 17;  // ESP32 TX2 -> E32 RXD
  static const int PIN_LED     = 2;
  #define E32_PORT Serial2

#elif defined(ARDUINO_ARCH_AVR) && (defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__))
  static const int PIN_E32_M0  = 4;
  static const int PIN_E32_M1  = 5;
  static const int PIN_E32_AUX = 6;
  static const int PIN_E32_RX  = 19;  // Serial1 RX, fixed by hardware
  static const int PIN_E32_TX  = 18;  // Serial1 TX, fixed by hardware -- DIVIDE THIS
  static const int PIN_LED     = 13;
  #define E32_PORT Serial1

#else  // Uno / Nano and friends
  static const int PIN_E32_M0  = 4;
  static const int PIN_E32_M1  = 5;
  static const int PIN_E32_AUX = 6;
  static const int PIN_E32_RX  = 10;  // SoftwareSerial RX <- E32 TXD
  static const int PIN_E32_TX  = 11;  // SoftwareSerial TX -> E32 RXD -- DIVIDE THIS
  static const int PIN_LED     = 13;
  E32Serial E32_PORT(PIN_E32_RX, PIN_E32_TX);
#endif

// ---- link parameters ------------------------------------------------------
static const uint32_t E32_UART_BAUD = 9600;    // must match the module's configured baud
static const uint32_t USB_BAUD      = 115200;  // open the Serial Monitor at THIS

// Set to 1, flash once to program THIS module to 9600 UART / 2.4k air rate /
// channel 0x17, then set back to 0. Both ends of the link must share these.
// On a 5 V board the TX divider must be fitted before you do this.
#define E32_RUN_CONFIG 0

// Reprint the column header every N packets so a long scroll stays readable.
static const uint16_t HEADER_EVERY = 20;
// Link-summary cadence, and how long without a frame before we call it lost.
static const uint32_t STATS_EVERY_MS = 5000;
static const uint32_t SIGNAL_LOST_MS = 3000;
static const uint32_t LED_PULSE_MS   = 40;

E32Link radio(E32_PORT, PIN_E32_M0, PIN_E32_M1, PIN_E32_AUX);

// ---- frame assembly -------------------------------------------------------
// A 32-byte sliding window. Every incoming byte shifts the window left by one;
// whenever the window happens to start with SYNC we CRC-check it as a candidate
// frame. This self-resynchronises for free: a corrupted or partial frame simply
// fails CRC and slides out of the window, and a 0xAA 0x55 pair that appears
// inside payload data is rejected by the same check. There is no framing state
// machine to get stuck in.
static uint8_t  win[TELEM_PACKET_SIZE];
static uint8_t  win_fill = 0;    // bytes seen, saturating at TELEM_PACKET_SIZE

// ---- link statistics ------------------------------------------------------
static uint32_t pkt_ok     = 0;  // frames that passed CRC
static uint32_t pkt_crcerr = 0;  // SYNC matched but CRC did not
static uint32_t pkt_lost   = 0;  // inferred from seq gaps
static uint16_t last_seq   = 0;
static bool     have_seq   = false;
static uint16_t since_header = HEADER_EVERY;  // force a header on the first packet
static uint32_t last_pkt_ms   = 0;
static uint32_t last_stats_ms = 0;
static uint32_t led_off_at    = 0;
static bool     signal_warned = false;

// ---------------------------------------------------------------------------
// Printing helpers.
//
// Everything is integer-only on purpose: no float, no dtostrf. The wire format
// is already fixed-point (PROTOCOL.md), so converting to float would only add
// rounding error and, on the Uno, ~1.5 KB of flash for nothing.
// ---------------------------------------------------------------------------

// Right-align `v` in `width` columns, then a trailing space.
static void printPad(long v, uint8_t width) {
  long probe = v < 0 ? -v : v;
  uint8_t digits = (v < 0) ? 2 : 1;   // 1 digit minimum, +1 for a '-'
  while (probe >= 10) { probe /= 10; digits++; }
  for (uint8_t i = digits; i < width; i++) Serial.print(' ');
  Serial.print(v);
  Serial.print(' ');
}

// Print a fixed-point integer as a decimal: printFixed(-483, 1) -> "-48.3".
// `scaled` is the raw wire value, `decimals` the number of implied places.
static void printFixed(long scaled, uint8_t decimals) {
  if (scaled < 0) { Serial.print('-'); scaled = -scaled; }
  long div = 1;
  for (uint8_t i = 0; i < decimals; i++) div *= 10;
  Serial.print(scaled / div);
  Serial.print('.');
  long frac = scaled % div;
  // Zero-pad the fraction: 3 with 7 decimals is ".0000003", not ".3".
  for (long p = div / 10; p > 1; p /= 10) {
    if (frac < p) Serial.print('0'); else break;
  }
  Serial.print(frac);
}

static void printFlightState(uint8_t s) {
  switch (s) {
    case FS_PAD:    Serial.print(F("PAD   ")); break;
    case FS_BOOST:  Serial.print(F("BOOST ")); break;
    case FS_COAST:  Serial.print(F("COAST ")); break;
    case FS_APOGEE: Serial.print(F("APOGEE")); break;
    case FS_DROGUE: Serial.print(F("DROGUE")); break;
    case FS_MAIN:   Serial.print(F("MAIN  ")); break;
    case FS_LANDED: Serial.print(F("LANDED")); break;
    default:        Serial.print(F("UNK"));
                    Serial.print(s);
                    if (s < 10) Serial.print(' ');
                    Serial.print(F("  ")); break;
  }
  Serial.print(' ');
}

// Render a bitfield as letters: bit set -> its letter, bit clear -> '-'.
// `letters` is in bit order, LSB first.
static void printBits(uint8_t value, const __FlashStringHelper* letters, uint8_t n) {
  const char* p = (const char*)letters;
  for (uint8_t i = 0; i < n; i++) {
#if defined(ARDUINO_ARCH_AVR)
    char c = pgm_read_byte(p + i);
#else
    char c = p[i];
#endif
    Serial.print((value & (1u << i)) ? c : '-');
  }
  Serial.print(' ');
}

static void printHeader() {
  Serial.println();
  Serial.println(F("seq    t(s)     state  alt   vspeed  lat          lon           gAlt sat fix tilt vbat flags health"));
  Serial.println(F("------ -------- ------ ----- ------- ------------ ------------- ---- --- --- ---- ---- ----- ------"));
}

// ---------------------------------------------------------------------------

static void handleFrame(const telem_body_t& b) {
  pkt_ok++;
  last_pkt_ms = millis();
  digitalWrite(PIN_LED, HIGH);
  led_off_at = last_pkt_ms + LED_PULSE_MS;

  // Loss is inferred from sequence gaps. uint16 arithmetic wraps the same way
  // the counter does, so a rollover at 65535 costs nothing.
  if (have_seq) {
    uint16_t gap = (uint16_t)(b.seq - last_seq - 1);
    // A huge "gap" is far more likely a transmitter restart than 60k lost
    // frames; don't let that poison the statistics.
    if (gap > 0 && gap < 1000) pkt_lost += gap;
  }
  last_seq = b.seq;
  have_seq = true;

  if (++since_header >= HEADER_EVERY) { printHeader(); since_header = 0; }

  printPad((long)b.seq, 6);
  printFixed((long)(b.onboard_ms / 100), 1);  // ms -> s, 1 decimal
  Serial.print(F("   "));
  printFlightState(b.flight_state);
  printPad((long)b.baro_alt_m, 5);
  printFixed((long)b.vspeed_dms, 1);          // dm/s -> m/s
  Serial.print(F("  "));
  printFixed((long)b.gps_lat, 7);             // deg * 1e7
  Serial.print(' ');
  printFixed((long)b.gps_lon, 7);
  Serial.print(' ');
  printPad((long)b.gps_alt_m, 4);
  printPad((long)b.gps_sats, 3);
  printPad((long)b.gps_fix, 3);
  printPad((long)b.tilt_deg, 4);
  printFixed((long)b.vbat_dv, 1);             // dV -> V
  Serial.print(' ');
  // Bit order must match TelemPacket.h exactly.
  printBits(b.flags,  F("CPSA"),   4);  // Continuity, Pyro fired, Sd ok, Armed
  printBits(b.health, F("BIGSPV"), 6);  // Baro, Imu, Gps, Sd, Pyro, Vbat
  Serial.println();

  // Call out a peripheral that is down -- the health byte exists precisely so
  // one dead sensor reads as one dead sensor, not a dead vehicle.
  if (b.health != HEALTH_ALL_OK) {
    Serial.print(F("       ^ DOWN:"));
    if (!(b.health & HEALTH_BARO)) Serial.print(F(" baro"));
    if (!(b.health & HEALTH_IMU))  Serial.print(F(" imu"));
    if (!(b.health & HEALTH_GPS))  Serial.print(F(" gps"));
    if (!(b.health & HEALTH_SD))   Serial.print(F(" sd"));
    if (!(b.health & HEALTH_PYRO)) Serial.print(F(" pyro"));
    if (!(b.health & HEALTH_VBAT)) Serial.print(F(" vbat"));
    Serial.println(F("  (fields it feeds are untrustworthy)"));
    since_header = HEADER_EVERY;  // the extra line broke the columns
  }
}

static void printStats() {
  uint32_t expected = pkt_ok + pkt_lost;
  Serial.print(F("--- "));
  Serial.print(pkt_ok);      Serial.print(F(" ok | "));
  Serial.print(pkt_crcerr);  Serial.print(F(" crc err | "));
  Serial.print(pkt_lost);    Serial.print(F(" lost ("));
  // Permille then one decimal, to stay in integer maths.
  if (expected) printFixed((long)((pkt_lost * 1000UL) / expected), 1);
  else          Serial.print(F("0.0"));
  Serial.println(F("%) ---"));
  since_header = HEADER_EVERY;
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(USB_BAUD);
  while (!Serial && millis() < 3000) { /* native-USB boards: wait briefly */ }

  radio.begin(E32_UART_BAUD, PIN_E32_RX, PIN_E32_TX);  // opens the UART, enters NORMAL

  Serial.println();
  Serial.println(F("E32 telemetry receiver -- 433 MHz, 9600 UART, 2.4k air rate"));
  Serial.print(F("Frame: 32 B, SYNC AA 55, CRC-16/CCITT-FALSE. AUX="));
  Serial.println(radio.auxHigh() ? F("HIGH (idle)") : F("LOW (busy/unwired?)"));

#if E32_RUN_CONFIG
  Serial.println(F("Programming module (C0)..."));
  bool ok = radio.configure();
  Serial.println(ok ? F("  config VERIFIED -- set E32_RUN_CONFIG back to 0")
                    : F("  config FAILED -- check AUX, the TX divider, and 3.3 V"));
  for (int i = 0; i < (ok ? 3 : 12); i++) {
    digitalWrite(PIN_LED, HIGH); delay(ok ? 150 : 60);
    digitalWrite(PIN_LED, LOW);  delay(ok ? 150 : 60);
  }
#endif

  Serial.println(F("Listening..."));
  last_stats_ms = millis();
}

void loop() {
  while (radio.available()) {
    uint8_t b = (uint8_t)radio.read();

    // Slide the window left one byte and append. 31 bytes of memmove per
    // received byte is nothing at 4 Hz x 32 B, and it keeps the framing
    // logic to the four lines below.
    memmove(win, win + 1, TELEM_PACKET_SIZE - 1);
    win[TELEM_PACKET_SIZE - 1] = b;
    if (win_fill < TELEM_PACKET_SIZE) win_fill++;

    if (win_fill < TELEM_PACKET_SIZE) continue;
    if (win[0] != TELEM_SYNC0 || win[1] != TELEM_SYNC1) continue;

    // SYNC is in place -- verify CRC over the 28 BODY bytes (offsets 2..29).
    uint16_t want = telem_crc16_ccitt(win + 2, TELEM_BODY_SIZE);
    uint16_t got  = (uint16_t)win[30] | ((uint16_t)win[31] << 8);  // little-endian
    if (want != got) {
      pkt_crcerr++;
      // Leave the window alone: the real frame may start later inside it, and
      // the next shift will find it.
      continue;
    }

    // memcpy into the struct rather than casting win+2 -- the packed struct is
    // safe to read field-wise, but an unaligned pointer cast is not.
    telem_body_t body;
    memcpy(&body, win + 2, TELEM_BODY_SIZE);

    if (body.msg_type == TELEM_MSG_TELEMETRY) {
      handleFrame(body);
    } else {
      // msg_type 1/2 are reserved for the phase-2 command/ack path.
      Serial.print(F("(non-telemetry frame, msg_type="));
      Serial.print(body.msg_type);
      Serial.println(F(")"));
      since_header = HEADER_EVERY;
    }

    // Consume the window so this frame cannot re-trigger on the next shift.
    win_fill = 0;
    memset(win, 0, TELEM_PACKET_SIZE);

    if (signal_warned) {
      Serial.println(F("--- signal reacquired ---"));
      signal_warned = false;
      since_header  = HEADER_EVERY;
    }
  }

  uint32_t now = millis();

  if (led_off_at && (int32_t)(now - led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    led_off_at = 0;
  }

  if (now - last_stats_ms >= STATS_EVERY_MS) {
    last_stats_ms = now;
    printStats();
  }

  // Distinguish "nothing is arriving" from "arriving corrupted" -- with a dead
  // link you see neither ok frames nor CRC errors, which points at antenna,
  // channel, or air-rate mismatch rather than noise.
  if (!signal_warned && pkt_ok && (now - last_pkt_ms) > SIGNAL_LOST_MS) {
    Serial.println(F("--- NO SIGNAL (no valid frame for 3 s) ---"));
    signal_warned = true;
    since_header  = HEADER_EVERY;
  }
}
