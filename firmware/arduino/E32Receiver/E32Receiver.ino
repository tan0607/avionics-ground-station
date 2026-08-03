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
//
// A module that shipped (or was left) on other parameters hears the RF but
// demodulates nonsense -- mismatched air rates cannot decode each other. This
// is the escape hatch for that.
//
// #ifndef so a one-off run needs no edit here:
//   pio run -e uno -t upload   with PLATFORMIO_BUILD_FLAGS="-DE32_RUN_CONFIG=1"
#ifndef E32_RUN_CONFIG
#define E32_RUN_CONFIG 0
#endif

// Set to 1 to push two known-good frames through the decoder at boot, before
// listening to the radio. Answers the one question a silent receiver cannot:
// "is nothing arriving, or is my decoding broken?" If the self-test prints
// correctly and the air stays quiet, the fault is RF -- antenna, channel, air
// rate, or a transmitter that is not running. Stop debugging this sketch.
//
// #ifndef so a one-off run needs no edit here:
//   pio run -e uno -t upload --build-flag="-DE32_SELFTEST=1"
#ifndef E32_SELFTEST
#define E32_SELFTEST 0
#endif

// Set to 1 to dump every raw byte off the radio as hex + printable ASCII,
// alongside the decoded output.
//
// This is the mode for a link test against SOMEONE ELSE'S transmitter. This
// sketch only prints frames that are 32 bytes, start AA 55, and pass CRC -- so
// a transmitter sending "Hello World" every second, or any other protocol, is
// received perfectly and then silently discarded, looking exactly like a dead
// radio. Raw mode shows the bytes regardless, which tells you the RF link is
// up and the disagreement is in software.
//
//   pio run -e uno -t upload   with PLATFORMIO_BUILD_FLAGS="-DE32_SHOW_RAW=1"
#ifndef E32_SHOW_RAW
#define E32_SHOW_RAW 0
#endif

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
// Every byte the radio handed us, valid frame or not. This is the difference
// between "the RF link is dead" and "the RF link is fine and we disagree about
// the payload format" -- without it, both look like silence.
static uint32_t bytes_seen = 0;
static uint16_t last_seq   = 0;
static bool     have_seq   = false;
static uint16_t since_header = HEADER_EVERY;  // force a header on the first packet
static uint32_t last_pkt_ms   = 0;
static uint32_t last_stats_ms = 0;
static uint32_t led_off_at    = 0;
static bool     signal_warned = false;
static bool     format_warned = false;  // "wrong payload format" is said once

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

// Print a fixed-point integer as a decimal, right-aligned in `width` columns
// plus a trailing space: printFixed(-483, 1, 7) -> "  -48.3 ".
// `scaled` is the raw wire value, `decimals` the number of implied places.
// Width matters: these columns are read under a header, and a number that does
// not sit under its own label is worse than no header at all.
static void printFixed(long scaled, uint8_t decimals, uint8_t width) {
  long div = 1;
  for (uint8_t i = 0; i < decimals; i++) div *= 10;

  // Measure before printing so we know how much left-padding to emit.
  long whole = (scaled < 0 ? -scaled : scaled) / div;
  uint8_t len = (uint8_t)(1 + decimals);         // the '.' plus the fraction
  if (scaled < 0) len++;                          // the '-'
  do { len++; whole /= 10; } while (whole);       // the integer digits
  for (uint8_t i = len; i < width; i++) Serial.print(' ');

  if (scaled < 0) { Serial.print('-'); scaled = -scaled; }
  Serial.print(scaled / div);
  Serial.print('.');
  long frac = scaled % div;
  // Zero-pad the fraction: 3 with 7 decimals is ".0000003", not ".3".
  for (long p = div / 10; p > 1; p /= 10) {
    if (frac < p) Serial.print('0'); else break;
  }
  Serial.print(frac);
  Serial.print(' ');
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
// `letters` is in bit order, LSB first. Padded to `width` + a trailing space so
// it lines up under its header label.
static void printBits(uint8_t value, const __FlashStringHelper* letters,
                      uint8_t n, uint8_t width) {
  const char* p = (const char*)letters;
  for (uint8_t i = 0; i < n; i++) {
#if defined(ARDUINO_ARCH_AVR)
    char c = pgm_read_byte(p + i);
#else
    char c = p[i];
#endif
    Serial.print((value & (1u << i)) ? c : '-');
  }
  for (uint8_t i = n; i < width; i++) Serial.print(' ');
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

  // Widths below match printHeader() exactly -- change one, change both.
  printPad((long)b.seq, 6);
  printFixed((long)(b.onboard_ms / 100), 1, 8);  // ms -> s, 1 decimal
  printFlightState(b.flight_state);
  printPad((long)b.baro_alt_m, 5);
  printFixed((long)b.vspeed_dms, 1, 7);          // dm/s -> m/s
  printFixed((long)b.gps_lat, 7, 12);            // deg * 1e7
  printFixed((long)b.gps_lon, 7, 13);
  printPad((long)b.gps_alt_m, 4);
  printPad((long)b.gps_sats, 3);
  printPad((long)b.gps_fix, 3);
  printPad((long)b.tilt_deg, 4);
  printFixed((long)b.vbat_dv, 1, 4);             // dV -> V
  // Bit order must match TelemPacket.h exactly.
  printBits(b.flags,  F("CPSA"),   4, 5);  // Continuity, Pyro fired, Sd ok, Armed
  printBits(b.health, F("BIGSPV"), 6, 6);  // Baro, Imu, Gps, Sd, Pyro, Vbat
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
  if (expected) printFixed((long)((pkt_lost * 1000UL) / expected), 1, 0);
  else          Serial.print(F("0.0"));
  Serial.print(F("%) | "));
  Serial.print(bytes_seen);   Serial.println(F(" raw B ---"));

  // Raw bytes arriving with zero valid frames is the signature of a transmitter
  // that is on-channel but not speaking this protocol. Worth saying -- ONCE.
  // Reprinting it every 5 s buries the hex dump that actually diagnoses the
  // problem, and the extra serial traffic starves SoftwareSerial of the
  // interrupts it needs, corrupting the very output you are trying to read.
  if (bytes_seen && !pkt_ok && !pkt_crcerr && !format_warned) {
    format_warned = true;
    Serial.println(F("    ^ RF arriving, no frame matched -- wrong payload format."));
  }
  since_header = HEADER_EVERY;
}

#if E32_SHOW_RAW
// Raw hex dump state. A partial line MUST get flushed on idle: a burst of 5
// bytes that never reaches 16 would otherwise sit in the buffer forever and the
// bytes you most need to see -- the first few off a marginal link -- would be
// exactly the ones never printed.
static uint8_t  rawCol = 0;
static char     rawAscii[17];
static uint32_t rawLastByteMs = 0;
static const uint32_t RAW_FLUSH_MS = 300;

// Not `static`, for the same auto-prototype reason as runSelfTest() below.
void flushRawLine() {
  if (rawCol == 0) return;
  for (uint8_t i = rawCol; i < 16; i++) Serial.print(F("   "));  // pad the hex columns
  rawAscii[rawCol] = '\0';
  Serial.print(F(" |"));
  Serial.print(rawAscii);
  Serial.println('|');
  rawCol = 0;
}
#endif

// Feed one received byte through the framer. Split out of loop() so the
// self-test can drive the identical path -- a self-test that used a shortcut
// would prove nothing about the code that actually runs.
static void feedByte(uint8_t b) {
  bytes_seen++;

#if E32_SHOW_RAW
  if (rawCol == 0) Serial.print(F("raw | "));
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
  Serial.print(' ');
  rawAscii[rawCol] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
  rawLastByteMs = millis();
  if (++rawCol == 16) flushRawLine();
  since_header = HEADER_EVERY;
#endif

  // Slide the window left one byte and append. 31 bytes of memmove per
  // received byte is nothing at 4 Hz x 32 B, and it keeps the framing
  // logic to the four lines below.
  memmove(win, win + 1, TELEM_PACKET_SIZE - 1);
  win[TELEM_PACKET_SIZE - 1] = b;
  if (win_fill < TELEM_PACKET_SIZE) win_fill++;

  if (win_fill < TELEM_PACKET_SIZE) return;
  if (win[0] != TELEM_SYNC0 || win[1] != TELEM_SYNC1) return;

  // SYNC is in place -- verify CRC over the 28 BODY bytes (offsets 2..29).
  uint16_t want = telem_crc16_ccitt(win + 2, TELEM_BODY_SIZE);
  uint16_t got  = (uint16_t)win[30] | ((uint16_t)win[31] << 8);  // little-endian
  if (want != got) {
    pkt_crcerr++;
    // Leave the window alone: the real frame may start later inside it, and
    // the next shift will find it.
    return;
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

#if E32_SELFTEST
// Two reference frames, emitted by shared/protocol/packet.py -- the source of
// truth -- and pasted here verbatim. Pushing them through feedByte() exercises
// the real path end to end: sliding window, SYNC match, CRC-16, struct unpack,
// and every print helper. Because Python produced the bytes, a correct decode
// is also a round-trip proof that this sketch and packet.py agree on the wire
// format, the same guarantee test/packet_check.cpp gives the PlatformIO targets.
//
// Regenerate with, from the repo root:
//   python3 -c "import sys;sys.path.insert(0,'.');from shared.protocol import packet as p;\
//   print(p.encode(p.Telemetry(seq=1240)).hex(' '))"
static const uint8_t SELFTEST_FRAMES[] PROGMEM = {
  // nominal COAST, all six peripherals up
  0xAA, 0x55, 0x00, 0xD8, 0x04, 0x02, 0xB8, 0xBB, 0x04, 0x00, 0xB4, 0x04, 0xE3, 0x01, 0x07, 0x9A,
  0xDC, 0x01, 0xEA, 0x37, 0x97, 0x3C, 0xBA, 0x04, 0x09, 0x03, 0x04, 0x4A, 0x0D, 0x3F, 0x5E, 0x8B,
  // baro + GPS dead, still transmitting -- must print a named "^ DOWN:" line
  0xAA, 0x55, 0x00, 0xD9, 0x04, 0x04, 0xB2, 0xBC, 0x04, 0x00, 0x00, 0x00, 0x68, 0xFF, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x47, 0x0F, 0x3A, 0x67, 0x3C,
};

// Not `static`: the Arduino/PlatformIO .ino preprocessor auto-generates a
// prototype for this even when E32_SELFTEST is 0 and the definition below is
// compiled out. A static prototype with no definition warns; a plain one does
// not, since it is simply an unused declaration.
void runSelfTest() {
  Serial.println(F("SELF-TEST: decoding two frames generated by packet.py..."));
  for (uint16_t i = 0; i < sizeof(SELFTEST_FRAMES); i++)
    feedByte(pgm_read_byte(&SELFTEST_FRAMES[i]));

  bool pass = (pkt_ok == 2) && (pkt_crcerr == 0);
  Serial.println(pass ? F("SELF-TEST PASS -- decoder agrees with packet.py.")
                      : F("SELF-TEST FAIL -- the sketch itself is wrong, not the radio."));
  if (pass)
    Serial.println(F("  So if nothing follows, the fault is RF: antenna, channel,\r\n"
                     "  air rate, or a transmitter that is not running."));

  // Reset so the self-test never pollutes the real link statistics -- including
  // bytes_seen, or the 64 bytes fed here would masquerade as received RF.
  pkt_ok = pkt_crcerr = pkt_lost = bytes_seen = 0;
  have_seq = false;
  since_header = HEADER_EVERY;
  Serial.println();
}
#endif

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

#if E32_SELFTEST
  runSelfTest();
#endif

  Serial.println(F("Listening..."));
  last_stats_ms = millis();
}

void loop() {
  while (radio.available()) feedByte((uint8_t)radio.read());

  uint32_t now = millis();

#if E32_SHOW_RAW
  // Print a short burst once the line goes quiet, instead of waiting for a full
  // 16-byte row that may never arrive.
  if (rawCol && (now - rawLastByteMs) > RAW_FLUSH_MS) flushRawLine();
#endif

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
