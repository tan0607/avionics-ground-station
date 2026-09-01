/*
  ==========================================================
  E32 RECEIVER (SOLO) -- one file, nothing else needed
  ==========================================================
  Decodes the rocket's 32-byte telemetry frames off an EBYTE E32 (433 MHz) and
  prints them as readable columns. No other files, no libraries to install, no
  PlatformIO, no laptop backend. Copy this .ino to any machine with the Arduino
  IDE and it compiles.

  It answers one question on the bench, before the real ground station exists:
  IS THE LINK ACTUALLY WORKING, and if not, which half is broken?

  Boards: Uno / Nano (SoftwareSerial), Mega 2560 (Serial1), ESP32 (Serial2).
  Serial Monitor at 115200.

  ----------------------------------------------------------
  WIRING -- the E32 is a 3.3 V part
  ----------------------------------------------------------
                    Uno / Nano      Mega 2560     ESP32
      M0            4               4             25
      M1            5               5             26
      AUX           6               6             27
      TXD -> board  2  (SoftSer)    19 (Serial1)  16
      RXD <- board  3   **DIVIDE**  18 **DIVIDE** 17
      VCC           3.3 V           3.3 V         3.3 V
      GND           GND             GND           GND

  M0 AND M1 ARE NOT OPTIONAL. Left floating they read HIGH, which is mode 3 --
  sleep/config -- and a module in mode 3 receives NOTHING. That looks exactly
  like a channel mismatch or a dead antenna from the Serial Monitor. This sketch
  drives both pins LOW at boot to force NORMAL mode, but it can only do that if
  they are actually wired to pins 4 and 5. Wiring them straight to GND works too;
  what does not work is leaving them in the air.

  On a 5 V board, board TX -> E32 RXD MUST be divided down (1k series + 2k to
  GND, or a level shifter). 5 V into the module's RXD damages it. A receiver
  never drives that line, so you can leave it DISCONNECTED unless you set
  E32_RUN_CONFIG below.

  AUX should be connected, with a 4.7k pull-up to 3.3 V -- the community fix for
  the module "locking up". Note that an UNWIRED AUX also reads HIGH through the
  internal pull-up, so the boot check below cannot tell "idle" from "not
  connected". It only catches a genuinely stuck-LOW line.

  ----------------------------------------------------------
  BOTH RADIOS MUST AGREE: channel 0x17, air rate 2.4k, UART 9600
  ----------------------------------------------------------
  Two E32s that disagree on any of those three are silent forever, and nothing
  in normal operation tells you. To program a module: set E32_RUN_CONFIG to 1,
  upload, watch for "config VERIFIED", then set it back to 0 and upload again.
  The TX divider must be fitted first -- this is the one path that writes.

  ----------------------------------------------------------
  PROVE YOUR OWN HALF FIRST: set E32_SELFTEST to 1
  ----------------------------------------------------------
  That feeds a known-good frame -- the exact bytes the project's Python codec
  produces -- through this decoder, with no radio involved. If the self-test
  passes and the air is still silent, stop debugging this sketch: the fault is
  on the RF side, or nothing is transmitting.
  ==========================================================
*/

// ---- switches -------------------------------------------------------------
#define E32_RUN_CONFIG 0   // 1 = program channel/air-rate/baud once, then set back to 0
#define E32_SELFTEST   0   // 1 = decode a known-good frame at boot, no radio needed
#define E32_SHOW_RAW   0   // 1 = hex-dump every byte that arrives (see "formats disagree")

// ---- board / pin map ------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)
  static const int PIN_M0 = 25, PIN_M1 = 26, PIN_AUX = 27;
  static const int PIN_E32_RX = 16, PIN_E32_TX = 17;
  #define E32_PORT Serial2
#elif defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
  static const int PIN_M0 = 4, PIN_M1 = 5, PIN_AUX = 6;
  #define E32_PORT Serial1
#else
  // Uno / Nano. RX/TX are 2/3 here, NOT the 10/11 used elsewhere in this project
  // -- that is the rig this sketch was cut for. Change these two lines to match
  // however yours is actually soldered; nothing else in the file depends on them.
  #include <SoftwareSerial.h>
  static const int PIN_M0 = 4, PIN_M1 = 5, PIN_AUX = 6;
  static const uint8_t PIN_E32_RX = 2;   // Uno receives  <- E32 TXD
  static const uint8_t PIN_E32_TX = 3;   // Uno transmits -> E32 RXD (through divider)
  SoftwareSerial e32SoftSerial(PIN_E32_RX, PIN_E32_TX);
  #define E32_PORT e32SoftSerial
#endif

static const uint32_t E32_BAUD        = 9600;   // must match the module's stored UART baud
static const uint8_t  E32_CHANNEL     = 0x17;   // both modules, or they never meet
static const uint8_t  E32_SPED_9600_2K4 = 0x1A; // 8N1 / 9600 UART / 2.4k air rate
static const uint8_t  E32_OPTION      = 0xC4;   // transparent, FEC on, max power (T20D)

static const uint32_t STATS_PERIOD_MS  = 5000;
static const uint32_t SIGNAL_TIMEOUT_MS = 3000;

// ===========================================================================
//  WIRE FORMAT
//
//  This is a hand-inlined copy of the project's TelemPacket.h, because a
//  single-file sketch cannot #include it. THE AUTHORITATIVE DEFINITION IS
//  shared/protocol/packet.py; firmware/lib/TelemPacket mirrors it, and this is
//  a mirror of that mirror. Nothing here is machine-checked against them --
//  the self-test frame below is the only guard, so run it after any protocol
//  change. Little-endian throughout.
//
//    [0:2]   SYNC  = 0xAA 0x55
//    [2:30]  BODY  = 28 packed bytes
//    [30:32] CRC16 = CRC-16/CCITT-FALSE over BODY, little-endian
// ===========================================================================
static const uint8_t TELEM_SYNC0 = 0xAA;
static const uint8_t TELEM_SYNC1 = 0x55;
static const uint8_t TELEM_BODY_SIZE = 28;
static const uint8_t TELEM_PACKET_SIZE = 32;

#pragma pack(push, 1)
typedef struct {
  uint8_t  msg_type;      // 0 = telemetry
  uint16_t seq;           // +1 per packet, wraps at 65535 -> drives loss rate
  uint8_t  flight_state;
  uint32_t onboard_ms;    // ms since flight-computer boot
  int16_t  baro_alt_m;    // m AGL
  int16_t  vspeed_dms;    // dm/s  (m/s * 10)
  int32_t  gps_lat;       // deg * 1e7
  int32_t  gps_lon;       // deg * 1e7
  int16_t  gps_alt_m;     // m MSL
  uint8_t  gps_sats;
  uint8_t  gps_fix;       // 0 none / 2 = 2D / 3 = 3D
  uint8_t  tilt_deg;      // 0..180 from vertical
  uint8_t  vbat_dv;       // V * 10
  uint8_t  flags;
  uint8_t  health;        // bit SET = that peripheral is up
} telem_body_t;
#pragma pack(pop)

static_assert(sizeof(telem_body_t) == 28, "telem_body_t must be 28 bytes");

// flags bitfield
#define FLAG_CONTINUITY (1u << 0)
#define FLAG_PYRO_FIRED (1u << 1)
#define FLAG_SD_OK      (1u << 2)
#define FLAG_ARMED      (1u << 3)

// health bitfield -- a CLEAR bit means that peripheral is down and every field
// it feeds is untrustworthy.
#define HEALTH_BARO (1u << 0)
#define HEALTH_IMU  (1u << 1)
#define HEALTH_GPS  (1u << 2)
#define HEALTH_SD   (1u << 3)
#define HEALTH_PYRO (1u << 4)
#define HEALTH_VBAT (1u << 5)

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
static uint16_t telemCrc16(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// The exact 32 bytes shared/protocol/packet.py emits for a reference sample.
// If this decodes correctly, the framing, the struct layout and the CRC in this
// file all agree with the ground station. Decodes to:
//   seq 1234, BOOST, 1247 m, 142.0 m/s, 3.2437000 / 101.7061000, 11 sat, 7.9 V
static const uint8_t SELFTEST_FRAME[32] = {
  0xAA, 0x55, 0x00, 0xD2, 0x04, 0x01, 0x88, 0x13, 0x00, 0x00, 0xDF, 0x04,
  0x8C, 0x05, 0x08, 0xF3, 0xEE, 0x01, 0x88, 0x1E, 0x9F, 0x3C, 0x14, 0x05,
  0x0B, 0x03, 0x04, 0x4F, 0x0D, 0x3F, 0xC8, 0x89
};

// ===========================================================================
//  E32 CONTROL -- the one rule: NEVER write to the UART unless AUX is HIGH.
//  AUX LOW means the module is busy; blind-writing then corrupts its buffer and
//  is the #1 cause of the module "locking up".
// ===========================================================================
// Plain constants rather than an enum, deliberately. The Arduino IDE generates
// function prototypes and injects them at the TOP of the sketch, above anything
// you declare down here -- so a function taking an enum parameter gets a
// prototype referring to a type that does not exist yet, and the build dies with
// "'E32Mode' was not declared in this scope". In a multi-file sketch the type
// would live in a header and the problem would not arise; in one file, custom
// types must stay out of function signatures.
static const uint8_t E32_MODE_NORMAL    = 0;   // mode = (M1<<1)|M0, EBYTE truth table
static const uint8_t E32_MODE_WAKEUP    = 1;
static const uint8_t E32_MODE_POWERSAVE = 2;
static const uint8_t E32_MODE_CONFIG    = 3;

static bool e32WaitAux(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - start >= timeoutMs) return false;   // busy -> do NOT write
    delay(1);
  }
  return true;
}

static void e32SetMode(uint8_t mode) {
  digitalWrite(PIN_M0, (mode & 0x01) ? HIGH : LOW);
  digitalWrite(PIN_M1, (mode & 0x02) ? HIGH : LOW);
  delay(3);
  e32WaitAux(1000);
  delay(3);
}

// Always compiled, called only when E32_RUN_CONFIG is 1 (see setup). Wrapping a
// function definition in #if is the other single-file trap: the IDE's prototype
// generator does not honour preprocessor conditionals, so it emits a prototype
// for a function that was compiled out -- "declared static but never defined".
static bool e32Configure() {
  e32SetMode(E32_MODE_CONFIG);               // params are only writable in mode 3
  while (E32_PORT.available()) E32_PORT.read();

  const uint8_t cmd[6] = {0xC0, 0x00, 0x00, E32_SPED_9600_2K4, E32_CHANNEL, E32_OPTION};
  if (!e32WaitAux(1000)) { e32SetMode(E32_MODE_NORMAL); return false; }
  E32_PORT.write(cmd, sizeof(cmd));
  E32_PORT.flush();
  e32WaitAux(1000);

  uint8_t resp[6] = {0};
  uint8_t got = 0;
  uint32_t start = millis();
  while (got < sizeof(resp) && millis() - start < 1000)
    if (E32_PORT.available()) resp[got++] = (uint8_t)E32_PORT.read();

  e32SetMode(E32_MODE_NORMAL);
  return (got == sizeof(resp)) && (resp[3] == E32_SPED_9600_2K4) &&
         (resp[4] == E32_CHANNEL) && (resp[5] == E32_OPTION);
}

// ===========================================================================
//  PRINTING -- no printf, no floats. Scaled integers are formatted by hand so
//  this behaves identically on an 8-bit AVR and an ESP32, and so a lat/lon of
//  deg*1e7 keeps all seven decimals (a 32-bit float would lose two of them).
// ===========================================================================
// Worst real case is a longitude: "-101.7061000" = 12 chars + NUL. Callers pass
// 24 so snprintf never has to truncate -- a silently shortened lat/lon would put
// the rocket somewhere it is not, which is worse than no position at all.
static void fixedStr(char* out, uint8_t n, long value, uint8_t decimals) {
  if (decimals > 9) decimals = 9;                       // frac[] holds 9 + NUL
  bool neg = value < 0;
  unsigned long v = neg ? (unsigned long)(-value) : (unsigned long)value;
  unsigned long div = 1;
  for (uint8_t i = 0; i < decimals; i++) div *= 10UL;

  char frac[10];
  unsigned long fp = v % div;
  for (int8_t i = decimals - 1; i >= 0; i--) { frac[i] = '0' + (char)(fp % 10); fp /= 10; }
  frac[decimals] = '\0';

  snprintf(out, n, "%s%lu.%s", neg ? "-" : "", v / div, frac);
}

static void printCol(const char* s, uint8_t width, bool leftAlign) {
  uint8_t len = strlen(s);
  if (!leftAlign) for (uint8_t i = len; i < width; i++) Serial.write(' ');
  Serial.print(s);
  if (leftAlign) for (uint8_t i = len; i < width; i++) Serial.write(' ');
  Serial.write(' ');
}

static void printNum(long v, uint8_t width, bool leftAlign) {
  char buf[14];
  snprintf(buf, sizeof(buf), "%ld", v);
  printCol(buf, width, leftAlign);
}

static const char* stateName(uint8_t s) {
  switch (s) {
    case 0: return "PAD";
    case 1: return "BOOST";
    case 2: return "COAST";
    case 3: return "APOGEE";
    case 4: return "DROGUE";
    case 5: return "MAIN";
    case 6: return "LANDED";
    default: return "?";
  }
}

// ===========================================================================
//  LINK STATS
// ===========================================================================
static uint32_t g_ok = 0, g_crc_err = 0, g_lost = 0, g_raw_bytes = 0;
static uint16_t g_last_seq = 0;
static bool     g_have_seq = false;
static uint32_t g_last_frame_ms = 0;
static uint32_t g_next_stats_ms = 0;
static bool     g_signal_warned = false;

static void printHeader() {
  Serial.println();
  Serial.println(F("seq    t(s)     state  alt   vspeed  lat          lon           gAlt sat fix tilt vbat flags health"));
  Serial.println(F("------ -------- ------ ----- ------- ------------ ------------- ---- --- --- ---- ---- ----- ------"));
}

static void printFrame(const telem_body_t& b) {
  char buf[24];

  printNum(b.seq, 6, false);
  fixedStr(buf, sizeof(buf), (long)(b.onboard_ms / 100), 1);  // ms -> s, 1 decimal
  printCol(buf, 8, true);
  printCol(stateName(b.flight_state), 6, true);
  printNum(b.baro_alt_m, 5, false);
  fixedStr(buf, sizeof(buf), b.vspeed_dms, 1);
  printCol(buf, 7, true);
  fixedStr(buf, sizeof(buf), b.gps_lat, 7);
  printCol(buf, 12, true);
  fixedStr(buf, sizeof(buf), b.gps_lon, 7);
  printCol(buf, 13, true);
  printNum(b.gps_alt_m, 4, false);
  printNum(b.gps_sats, 3, false);
  printNum(b.gps_fix, 3, false);
  printNum(b.tilt_deg, 4, false);
  fixedStr(buf, sizeof(buf), b.vbat_dv, 1);
  printCol(buf, 4, false);

  char flags[5] = {
    (char)((b.flags & FLAG_CONTINUITY) ? 'C' : '-'),
    (char)((b.flags & FLAG_PYRO_FIRED) ? 'P' : '-'),
    (char)((b.flags & FLAG_SD_OK)      ? 'S' : '-'),
    (char)((b.flags & FLAG_ARMED)      ? 'A' : '-'), '\0'
  };
  printCol(flags, 5, true);

  char health[7] = {
    (char)((b.health & HEALTH_BARO) ? 'B' : '-'),
    (char)((b.health & HEALTH_IMU)  ? 'I' : '-'),
    (char)((b.health & HEALTH_GPS)  ? 'G' : '-'),
    (char)((b.health & HEALTH_SD)   ? 'S' : '-'),
    (char)((b.health & HEALTH_PYRO) ? 'P' : '-'),
    (char)((b.health & HEALTH_VBAT) ? 'V' : '-'), '\0'
  };
  printCol(health, 6, true);
  Serial.println();

  // Name the dead peripherals. Never a blanket "AV FAILED" -- the operator needs
  // to know WHICH device died, and which numbers above to stop believing.
  if ((b.health & 0x3F) != 0x3F) {
    Serial.print(F("    ^ DOWN:"));
    if (!(b.health & HEALTH_BARO)) Serial.print(F(" BARO(alt,vspeed)"));
    if (!(b.health & HEALTH_IMU))  Serial.print(F(" IMU(tilt)"));
    if (!(b.health & HEALTH_GPS))  Serial.print(F(" GPS(lat,lon,gAlt,sat)"));
    if (!(b.health & HEALTH_SD))   Serial.print(F(" SD"));
    if (!(b.health & HEALTH_PYRO)) Serial.print(F(" PYRO"));
    if (!(b.health & HEALTH_VBAT)) Serial.print(F(" VBAT(vbat)"));
    Serial.println();
  }
}

static void acceptFrame(const uint8_t* frame) {
  telem_body_t body;
  memcpy(&body, frame + 2, TELEM_BODY_SIZE);

  if (g_have_seq) {
    uint16_t gap = (uint16_t)(body.seq - g_last_seq - 1);   // wraps naturally at 65535
    if (gap < 1000) g_lost += gap;                          // ignore an absurd jump (TX reboot)
  }
  g_last_seq = body.seq;
  g_have_seq = true;
  g_ok++;
  g_last_frame_ms = millis();
  g_signal_warned = false;

  printFrame(body);
}

// ===========================================================================
//  FRAMING -- find AA 55, take 32 bytes, check CRC, resync past noise.
// ===========================================================================
static uint8_t g_buf[TELEM_PACKET_SIZE];
static uint8_t g_idx = 0;

static void feedByte(uint8_t b) {
  g_raw_bytes++;

  if (g_idx == 0) {
    if (b == TELEM_SYNC0) g_buf[g_idx++] = b;
    return;
  }
  if (g_idx == 1) {
    if (b == TELEM_SYNC1)      g_buf[g_idx++] = b;
    else if (b == TELEM_SYNC0) g_buf[0] = b;          // AA AA -> keep the second
    else                       g_idx = 0;
    return;
  }

  g_buf[g_idx++] = b;
  if (g_idx < TELEM_PACKET_SIZE) return;

  uint16_t want = telemCrc16(g_buf + 2, TELEM_BODY_SIZE);
  uint16_t got  = (uint16_t)g_buf[30] | ((uint16_t)g_buf[31] << 8);   // CRC is little-endian

  if (want == got) {
    acceptFrame(g_buf);
    g_idx = 0;
    return;
  }

  // Bad CRC. That AA 55 may have been noise that happened to look like sync, so
  // rescan the buffer for a later one and keep those bytes instead of throwing
  // all 32 away -- on a marginal link that is the difference between resyncing
  // in one frame and never resyncing at all.
  g_crc_err++;
  uint8_t start = 0;
  for (uint8_t i = 1; i + 1 < TELEM_PACKET_SIZE; i++) {
    if (g_buf[i] == TELEM_SYNC0 && g_buf[i + 1] == TELEM_SYNC1) { start = i; break; }
  }
  if (start == 0) { g_idx = 0; return; }
  g_idx = TELEM_PACKET_SIZE - start;
  memmove(g_buf, g_buf + start, g_idx);
}

static void printStats() {
  uint32_t expected = g_ok + g_lost;
  Serial.print(F("--- "));
  Serial.print(g_ok);        Serial.print(F(" ok | "));
  Serial.print(g_crc_err);   Serial.print(F(" crc err | "));
  Serial.print(g_lost);      Serial.print(F(" lost ("));
  if (expected > 0) {
    char pct[24];
    fixedStr(pct, sizeof(pct), (long)((g_lost * 1000UL) / expected), 1);
    Serial.print(pct);
  } else {
    Serial.print(F("0.0"));
  }
  Serial.print(F("%) | "));
  Serial.print(g_raw_bytes); Serial.println(F(" raw B ---"));

  // The single most useful line in this sketch. Bytes arriving but no frame ever
  // matching means the RF link is FINE and the payload formats disagree -- which
  // on screen is otherwise indistinguishable from a dead link.
  if (g_ok == 0 && g_crc_err == 0 && g_raw_bytes > 0)
    Serial.println(F("    ^ RF IS ARRIVING but nothing matched. The link works; the payload format does not."));
}

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  pinMode(PIN_AUX, INPUT_PULLUP);   // external 4.7k to 3.3 V preferred

#if defined(ARDUINO_ARCH_ESP32)
  E32_PORT.begin(E32_BAUD, SERIAL_8N1, PIN_E32_RX, PIN_E32_TX);
#else
  E32_PORT.begin(E32_BAUD);
#endif

  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F("  E32 RECEIVER (solo)  --  ch 0x17, 2.4k air, 9600 UART"));
  Serial.println(F("=========================================="));

  e32SetMode(E32_MODE_NORMAL);
  if (digitalRead(PIN_AUX) == LOW)
    Serial.println(F("AUX=LOW (busy/unwired?) -- check AUX and its pull-up"));

  // Runtime `if` on a compile-time constant, not #if: the branch is folded away
  // and the unused code dropped by the optimiser, but it is still type-checked,
  // and no function definition ends up inside a preprocessor conditional.
  if (E32_RUN_CONFIG) {
    Serial.println(F("programming module..."));
    Serial.println(e32Configure() ? F("config VERIFIED -- now set E32_RUN_CONFIG back to 0")
                                  : F("config FAILED -- TX divider fitted? AUX wired? 3.3 V?"));
  }

  if (E32_SELFTEST) {
    Serial.println(F("SELFTEST: feeding a known-good frame (no radio involved)"));
    printHeader();
    for (uint8_t i = 0; i < sizeof(SELFTEST_FRAME); i++) feedByte(SELFTEST_FRAME[i]);
    Serial.println(g_ok == 1 ? F("SELFTEST PASS -- decoder agrees with the ground station")
                             : F("SELFTEST FAIL -- this sketch's packet layout is wrong"));
    g_ok = g_crc_err = g_lost = g_raw_bytes = 0;   // don't pollute the live stats
    g_have_seq = false;
  }

  printHeader();
  g_next_stats_ms = millis() + STATS_PERIOD_MS;
  g_last_frame_ms = millis();
}

void loop() {
  while (E32_PORT.available() > 0) {
    uint8_t b = (uint8_t)E32_PORT.read();
    if (E32_SHOW_RAW) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X ", b);
      Serial.print(hex);
    }
    feedByte(b);
  }

  uint32_t now = millis();

  if ((int32_t)(now - g_next_stats_ms) >= 0) {
    g_next_stats_ms = now + STATS_PERIOD_MS;
    printStats();
  }

  if (!g_signal_warned && (now - g_last_frame_ms) > SIGNAL_TIMEOUT_MS) {
    g_signal_warned = true;
    Serial.println(F("--- NO SIGNAL ---"));
  }
}
