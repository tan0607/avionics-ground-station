/*
  ==========================================================
  E32 HELLO -- the smallest possible proof that the link works
  ==========================================================
  No telemetry frames. No CRC. No struct. No Python. One board sends the text
  "hello <n>" once a second, the other prints whatever bytes come out of the
  air. That is the entire program.

  Flash the SAME file to both boards, changing one line:

      board A:  #define E32_ROLE_TX 1     <- the talker
      board B:  #define E32_ROLE_TX 0     <- the listener

  Serial Monitor at 115200 on both.

  ----------------------------------------------------------
  READ THIS PART -- it is why the link has been silent
  ----------------------------------------------------------
  Before it sends anything, this sketch INTERROGATES its own module: it drops
  into config mode and asks the E32 to report the settings it actually has
  stored, plus its firmware version. It prints them decoded.

  That single step splits your problem cleanly in half, and it is the step that
  a week of staring at a blank Serial Monitor cannot do:

    * Module does NOT answer  -> the fault is on your desk. Wiring, power, a
      dead module, TXD/RXD swapped, M0/M1 not connected. Nothing to do with RF,
      the other board, antennas, or channels. Stop testing the air.

    * Module DOES answer      -> the module, the wiring and the UART are all
      proven good. Now run it on the other board and put the two printouts side
      by side. Any line that differs is your bug. Two E32s that disagree on
      channel, air rate or UART baud are silent forever and never say why.

  Config mode runs at a FIXED 9600 8N1 no matter what baud the module has
  stored. That is what makes this test trustworthy: it answers even when the
  stored baud is wrong -- which is itself a very common reason normal mode
  stays silent while everything "looks" correctly wired.

  ----------------------------------------------------------
  WIRING -- 5 V POWER, 3.3 V LOGIC. THE TWO ARE NOT THE SAME PIN.
  ----------------------------------------------------------
  E32 pin        Uno / Nano      Mega 2560     ESP32-S3   ESP8266
   1  M0         4               4             10         D1 (GPIO5)
   2  M1         5               5             11         D2 (GPIO4)
   3  RXD  <-    3   **DIVIDE**  18 **DIVIDE** 14         D6 (GPIO12)
   4  TXD  ->    2  (SoftSer)    19 (Serial1)  13         D5 (GPIO14)
   5  AUX        6               6             12         D7 (GPIO13)
   6  VCC        5 V             5 V           5 V        5 V
   7  GND        GND             GND           GND        GND
   8/9/10        mounting holes, not electrical

  VCC IS 5 V, NOT 3.3 V. The E32-433T20D datasheet (section 2.2) gives operating
  voltage min 2.3 / typ 5.0 / max 5.5, with the remark "=>5.0 V ensures output
  power". The module runs off 3.3 V, which is why a 3.3 V rig looks like it
  works -- but rated 20 dBm output is NOT guaranteed there, so the link comes up
  short-range or marginal for a reason no amount of channel-checking will find.
  Above 5.5 V is permanent damage (section 2.1). USB 5 V is inside the window.

  Powering from 5 V also gets the radio off the board's own 3.3 V LDO. TX draws
  106 mA in bursts (RX is 15 mA); sharing the regulator that is also feeding an
  ESP is how you get a brownout that only ever happens while transmitting.

  THE LOGIC PINS STAY 3.3 V no matter what VCC is -- the module regulates
  internally, and the datasheet warns 5 V TTL "may be at risk of burning down".
  So a 5 V board still needs the divider on RXD, and any AUX pull-up goes to
  3.3 V, never to the 5 V rail feeding pin 6.

  On the ESP8266, wire by the D-numbers silkscreened on the board, not the GPIO
  numbers in the code. Neither ESP board needs the RXD divider -- both already
  drive 3.3 V logic. See the ESP8266 pin-map comment below for pins to avoid.

  M0 AND M1 ARE NOT OPTIONAL. Floating, they read HIGH = mode 3 = sleep/config,
  and a module in mode 3 receives NOTHING. From the Serial Monitor that looks
  exactly like a channel mismatch or a dead antenna. This sketch drives both
  pins, but only if they are actually wired.

  On a 5 V board, board TX -> E32 RXD MUST be divided down (1k series + 2k to
  GND, or a level shifter). 5 V into RXD damages the module. Unlike a pure
  receiver, THIS SKETCH NEEDS THAT LINE ON BOTH BOARDS -- the interrogation
  writes to the module. If you have been running receive-only until now, this
  may be the first time that wire has ever mattered.

  ----------------------------------------------------------
  POWER -- the failure that survives a week of correct wiring
  ----------------------------------------------------------
  Datasheet numbers: 106 mA transmitting (instant burst), 15 mA receiving. That
  gap is the whole problem -- a rail that sags only under the TX burst gives you
  a module that enumerates fine, answers config commands fine, receives fine,
  and fails ONLY when transmitting. The listener stays blank and the talker
  reports no error at all, because from its side nothing went wrong.

  Feed pin 6 from the board's 5 V pin (USB), which is a straight path, not from
  the 3.3 V LDO that is simultaneously running an ESP. Add a 100 uF cap across
  the module's VCC/GND to cover the burst. An Uno's 3.3 V pin is the worst case
  of all -- rated ~50 mA against a 106 mA draw.

  NEVER POWER A T20D WITHOUT AN ANTENNA ON THE SMA. At 20 dBm the reflected
  power has nowhere to go but back into the PA.

  And do not bench the two modules next to each other: -144 dBm sensitivity
  against a +20 dBm transmitter 20 cm away overloads the receiver front end, and
  the resulting silence or garbage looks exactly like a config mismatch. Put at
  least 1-2 m between them.
  ==========================================================
*/

// ---- THE ONE LINE YOU CHANGE ----------------------------------------------
#define E32_ROLE_TX  1     // 1 = send "hello", 0 = listen and print

// ---- switches -------------------------------------------------------------
#define E32_INTERROGATE 1  // 1 = ask the module what it is before running
#define E32_SHOW_HEX    0  // 1 = listener also hex-dumps every byte

// ---- board / pin map ------------------------------------------------------
// Matches the rest of this project: the S3 map is onboard_tx.cpp's, the Uno map
// is E32ReceiverSolo's (2/3, NOT the 10/11 used elsewhere). Change to match how
// yours is soldered; nothing else in the file depends on these.
#if defined(ARDUINO_ARCH_ESP32)
  static const int PIN_M0 = 10, PIN_M1 = 11, PIN_AUX = 12;
  static const int PIN_E32_RX = 13, PIN_E32_TX = 14;
  #define E32_PORT Serial1
#elif defined(ARDUINO_ARCH_ESP8266)
  // ---- ESP8266 (NodeMCU / Wemos D1 mini) ---------------------------------
  // The numbers below are GPIO numbers, which are NOT the D-numbers silkscreened
  // on the board. Wire by the D column:
  //
  //     M0  -> D1 (GPIO5)      AUX -> D7 (GPIO13)
  //     M1  -> D2 (GPIO4)      E32 TXD -> D5 (GPIO14)
  //     E32 RXD <- D6 (GPIO12)
  //
  // NEVER use GPIO6..GPIO11 on an ESP8266 for anything. They are hard-wired to
  // the SPI flash chip; touching one resets the board in a boot loop that looks
  // like a power fault. GPIO6 is exactly where this sketch's Uno pin map would
  // have put AUX, so the fall-through would have "failed" in a way that has
  // nothing to do with the radio.
  //
  // Also avoided here: GPIO0/2/15 (boot straps -- driving them at reset stops
  // the chip booting) and GPIO16 (no pull-up, so INPUT_PULLUP on AUX is a lie).
  //
  // No divider needed on this board: the ESP8266 drives 3.3 V logic already.
  //
  // VCC (E32 pin 6) goes to the board's 5 V rail, and WHICH PAD THAT IS depends
  // on which ESP8266 board you have -- the labels are not interchangeable:
  //     Wemos D1 mini : the pad marked "5V"   (straight off USB VBUS)
  //     NodeMCU LoLin : the pad marked "VU"   ("VIN" is the regulator INPUT)
  //     NodeMCU Amica : "VIN" -- no VU pad exists on this revision
  // Revisions differ in whether a diode sits between USB and that pad, so METER
  // IT FIRST: probe the pad against GND with the board on USB and confirm 4.7 to
  // 5.1 V before wiring pin 6 to it. GND is "G" on a D1 mini, "GND" on a NodeMCU.
  //
  // SoftwareSerial rather than a hardware UART, and there is no choice about it.
  // UART0 is the USB Serial Monitor. UART1 (GPIO2) is TRANSMIT-ONLY on this chip
  // -- it physically cannot receive, so it can neither listen nor read back the
  // interrogation reply. The core bundles EspSoftwareSerial; nothing to install.
  #include <SoftwareSerial.h>
  static const int PIN_M0 = 5, PIN_M1 = 4, PIN_AUX = 13;
  static const int PIN_E32_RX = 14;   // board receives  <- E32 TXD
  static const int PIN_E32_TX = 12;   // board transmits -> E32 RXD
  SoftwareSerial e32SoftSerial(PIN_E32_RX, PIN_E32_TX);
  #define E32_PORT e32SoftSerial
#elif defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
  static const int PIN_M0 = 4, PIN_M1 = 5, PIN_AUX = 6;
  #define E32_PORT Serial1
#else
  #include <SoftwareSerial.h>
  static const int PIN_M0 = 4, PIN_M1 = 5, PIN_AUX = 6;
  static const uint8_t PIN_E32_RX = 2;   // board receives  <- E32 TXD
  static const uint8_t PIN_E32_TX = 3;   // board transmits -> E32 RXD (DIVIDER)
  SoftwareSerial e32SoftSerial(PIN_E32_RX, PIN_E32_TX);
  #define E32_PORT e32SoftSerial
#endif

// What this project expects both modules to be set to. The interrogation prints
// what yours ACTUALLY holds, and flags anything that differs from these.
static const uint32_t E32_BAUD    = 9600;   // stored UART baud (normal mode)
static const uint8_t  E32_CHANNEL = 0x17;   // 410 + 0x17 = 433 MHz
static const uint8_t  E32_SPED    = 0x1A;   // 8N1 / 9600 UART / 2.4k air
static const uint8_t  E32_OPTION  = 0xC4;   // transparent, FEC on, max power

static const uint32_t CONFIG_BAUD = 9600;   // config mode is ALWAYS 9600 8N1
static const uint32_t HELLO_PERIOD_MS = 1000;
static const uint32_t QUIET_NAG_MS    = 5000;

// mode = (M1 << 1) | M0, per the EBYTE truth table
static const uint8_t E32_MODE_NORMAL = 0;
static const uint8_t E32_MODE_CONFIG = 3;

// ===========================================================================
//  MODE + AUX
//  The one rule: NEVER write to the UART unless AUX is HIGH. AUX LOW means the
//  module is busy; blind-writing corrupts its buffer and is the #1 cause of the
//  module "locking up".
// ===========================================================================
static bool e32WaitAux(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - start >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

// M0/M1 are 3.3 V INPUTS on the module, exactly like RXD -- the divider on RXD
// is not the only place 5 V can get in. A 5 V board must therefore never drive
// them HIGH.
//
// It does not have to. The datasheet lists both as "Input (weak pull-up)", so
// releasing the pin to high-Z lets the module's own pull-up take it to a clean
// 3.3 V. Driving LOW is safe on any board, because low is low. That gives a
// 5 V board open-drain behaviour with no extra parts and no dividers.
//
// Meter note: on an AVR a released pin reads ~3.3 V only because the MODULE is
// pulling it up. Reading 0 V there means the module is unpowered or that jumper
// is broken -- which makes this a free continuity check on M0/M1.
static void e32DriveModePin(int pin, bool high) {
#if defined(__AVR__)
  if (high) {
    pinMode(pin, INPUT);              // high-Z; module's weak pull-up -> 3.3 V
  } else {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
#else
  pinMode(pin, OUTPUT);               // 3.3 V board: driving HIGH is already safe
  digitalWrite(pin, high ? HIGH : LOW);
#endif
}

static void e32SetMode(uint8_t mode) {
  e32DriveModePin(PIN_M0, (mode & 0x01) != 0);
  e32DriveModePin(PIN_M1, (mode & 0x02) != 0);
  // The datasheet wants >2 ms after a mode change, and AUX pulses during the
  // switch. Settle, then wait for AUX to come back up.
  delay(50);
  e32WaitAux(1000);
  delay(10);
}

static void e32OpenPort(uint32_t baud) {
#if defined(ARDUINO_ARCH_ESP32)
  E32_PORT.begin(baud, SERIAL_8N1, PIN_E32_RX, PIN_E32_TX);
#elif defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
  E32_PORT.begin(baud);
#else
  E32_PORT.begin(baud);
#endif
  delay(10);
}

// ===========================================================================
//  INTERROGATION -- decode what the module actually holds
//  All of it compiles away when E32_INTERROGATE is 0.
//
//  NOTHING IN THIS BLOCK IS `static`, deliberately -- same family of gotcha the
//  E32_MODE_* constants dodge in E32ReceiverSolo. The IDE (and PlatformIO)
//  generate prototypes for sketch functions and inject them at the TOP of the
//  file, ABOVE this #if. Setting E32_INTERROGATE to 0 therefore leaves the
//  prototypes behind with no bodies, and a `static` prototype that is never
//  defined is a -Wall warning on every build. Non-static, it is silent.
// ===========================================================================
#if E32_INTERROGATE
const char* spedUartBaudName(uint8_t sped) {
  switch ((sped >> 3) & 0x07) {
    case 0: return "1200";   case 1: return "2400";
    case 2: return "4800";   case 3: return "9600";
    case 4: return "19200";  case 5: return "38400";
    case 6: return "57600";  default: return "115200";
  }
}

const char* spedAirRateName(uint8_t sped) {
  switch (sped & 0x07) {
    case 0: return "0.3k";  case 1: return "1.2k";
    case 2: return "2.4k";  case 3: return "4.8k";
    case 4: return "9.6k";  default: return "19.2k";
  }
}

const char* spedParityName(uint8_t sped) {
  switch ((sped >> 6) & 0x03) {
    case 0: return "8N1";  case 1: return "8O1";
    case 2: return "8E1";  default: return "8N1";
  }
}

// Reads N bytes with a deadline. Returns how many actually arrived, so the
// caller can tell "nothing at all" (0) from "a truncated reply".
uint8_t e32ReadReply(uint8_t* buf, uint8_t want, uint32_t timeoutMs) {
  uint32_t start = millis();
  uint8_t got = 0;
  while (got < want && millis() - start < timeoutMs) {
    if (E32_PORT.available()) buf[got++] = (uint8_t)E32_PORT.read();
    // Half a second of spinning trips the ESP8266 software watchdog if nothing
    // services it, and the reset would look like the module killing the board.
    // yield() is a no-op on AVR, so this costs the other targets nothing.
    yield();
  }
  return got;
}

// Returns true if the module answered a parameter read.
bool e32Interrogate() {
  Serial.println(F("--- interrogating the module (config mode, fixed 9600) ---"));

  e32SetMode(E32_MODE_CONFIG);
  e32OpenPort(CONFIG_BAUD);
  while (E32_PORT.available()) E32_PORT.read();   // drop mode-change garbage

  // C3 C3 C3 -> 4 bytes: C3, model, version, features
  uint8_t ver[4] = {0};
  E32_PORT.write((uint8_t)0xC3); E32_PORT.write((uint8_t)0xC3); E32_PORT.write((uint8_t)0xC3);
  uint8_t vgot = e32ReadReply(ver, 4, 500);
  if (vgot == 4 && ver[0] == 0xC3) {
    Serial.print(F("  version : model 0x")); Serial.print(ver[1], HEX);
    Serial.print(F("  ver 0x"));             Serial.print(ver[2], HEX);
    Serial.print(F("  features 0x"));        Serial.println(ver[3], HEX);
    if (ver[1] == 0x32) Serial.println(F("            (0x32 = 433 MHz part -- correct for this project)"));
  } else {
    Serial.print(F("  version : NO ANSWER (")); Serial.print(vgot); Serial.println(F(" bytes)"));
  }

  // C1 C1 C1 -> 6 bytes: C0/C2, ADDH, ADDL, SPED, CHAN, OPTION
  uint8_t p[6] = {0};
  E32_PORT.write((uint8_t)0xC1); E32_PORT.write((uint8_t)0xC1); E32_PORT.write((uint8_t)0xC1);
  uint8_t pgot = e32ReadReply(p, 6, 500);

  if (pgot != 6 || (p[0] != 0xC0 && p[0] != 0xC2)) {
    Serial.print(F("  params  : NO ANSWER (")); Serial.print(pgot); Serial.println(F(" bytes)"));
    Serial.println();
    Serial.println(F("  ****  THE MODULE IS NOT TALKING TO THIS BOARD.  ****"));
    Serial.println(F("  The fault is on this desk, not in the air. In order of likelihood:"));
    Serial.println(F("    1. E32 TXD and RXD swapped (TXD goes to the board's RX pin)"));
    Serial.println(F("    2. board TX -> E32 RXD not connected -- receive-only rigs never need"));
    Serial.println(F("       it, and this is the first test that writes to the module"));
    Serial.println(F("    3. M0/M1 not wired, so it never entered config mode"));
    Serial.println(F("    4. no/!bad 3.3 V, or GND not common with the board"));
    Serial.println(F("    5. module damaged by 5 V on RXD (needs the divider)"));
    Serial.println(F("  Do not change channels or antennas. None of that is involved yet."));
    return false;
  }

  const uint8_t sped = p[3], chan = p[4], opt = p[5];
  Serial.print(F("  address : 0x")); Serial.print(p[1], HEX); Serial.println(p[2], HEX);
  Serial.print(F("  channel : 0x"));  Serial.print(chan, HEX);
  Serial.print(F("  = "));            Serial.print(410 + chan); Serial.println(F(" MHz"));
  Serial.print(F("  uart    : "));    Serial.print(spedUartBaudName(sped));
  Serial.print(F(" "));               Serial.println(spedParityName(sped));
  Serial.print(F("  air rate: "));    Serial.println(spedAirRateName(sped));
  Serial.print(F("  option  : 0x"));  Serial.print(opt, HEX);
  Serial.print(F("  ("));             Serial.print((opt & 0x80) ? F("fixed") : F("transparent"));
  Serial.print(F(", FEC "));          Serial.print((opt & 0x04) ? F("on") : F("off"));
  Serial.println(F(")"));

  // Flag drift from what this project expects. This is the comparison that
  // matters -- but the REAL test is running this on both boards and diffing.
  bool ok = true;
  if (chan != E32_CHANNEL) {
    Serial.print(F("  !! channel is not the project's 0x")); Serial.println(E32_CHANNEL, HEX);
    ok = false;
  }
  if (sped != E32_SPED) {
    Serial.print(F("  !! sped is not the project's 0x")); Serial.println(E32_SPED, HEX);
    ok = false;
  }
  if ((opt & 0x80) != (E32_OPTION & 0x80)) {
    Serial.println(F("  !! transmission mode differs -- fixed vs transparent will not talk"));
    ok = false;
  }
  if (ok) Serial.println(F("  -> matches this project's expected settings"));
  Serial.println(F("  -> now run this on the OTHER board. Any differing line is your bug."));

  Serial.println();
  return true;
}
#endif  // E32_INTERROGATE

// ===========================================================================
#if E32_ROLE_TX
static uint32_t g_nextHello  = 0;
static uint32_t g_helloCount = 0;
#else
static uint32_t g_rxBytes = 0;
static uint32_t g_nextNag = 0;
#endif

void setup() {
  Serial.begin(115200);
  delay(300);            // let USB CDC come up on the S3

  Serial.println();
  Serial.println(F("=========================================="));
  Serial.print(F("  E32 HELLO -- role: "));
  Serial.println(E32_ROLE_TX ? F("TX (talker)") : F("RX (listener)"));
  Serial.println(F("=========================================="));

  // M0/M1 pin modes are managed per-write by e32DriveModePin -- on a 5 V board
  // "HIGH" means releasing the pin, so the direction is not fixed at OUTPUT.
  pinMode(PIN_AUX, INPUT_PULLUP);

  // An UNWIRED AUX also reads HIGH through the pull-up, so this can only catch a
  // genuinely stuck-LOW line -- it cannot tell "idle" from "not connected".
  if (digitalRead(PIN_AUX) == LOW) {
    Serial.println(F("  AUX is LOW at boot -- module busy, held in reset, or unpowered."));
  }

#if E32_INTERROGATE
  if (!e32Interrogate()) {
    Serial.println(F("  Halting. Fix the wiring above; nothing past this point can work."));
    while (true) delay(1000);
  }
#endif

  // Back to normal mode at the module's own baud, and start moving bytes.
  e32SetMode(E32_MODE_NORMAL);
  e32OpenPort(E32_BAUD);
  while (E32_PORT.available()) E32_PORT.read();

  Serial.print(F("  normal mode, UART "));
  Serial.print(E32_BAUD);
  Serial.println(E32_ROLE_TX ? F(" -- sending \"hello\" once a second")
                             : F(" -- listening"));
  Serial.println();

#if E32_ROLE_TX
  g_nextHello = millis();
#else
  g_nextNag   = millis() + QUIET_NAG_MS;
#endif
}

void loop() {
#if E32_ROLE_TX
  if ((int32_t)(millis() - g_nextHello) >= 0) {
    g_nextHello += HELLO_PERIOD_MS;

    // Never write blind: AUX LOW means the module is still chewing on the last
    // one. Skipping a beat is correct; corrupting its buffer is not.
    if (!e32WaitAux(1000)) {
      Serial.println(F("[skip] AUX stuck LOW -- module busy, nothing sent"));
      return;
    }
    E32_PORT.print(F("hello "));
    E32_PORT.println(g_helloCount);

    Serial.print(F("[tx] hello "));
    Serial.println(g_helloCount);
    g_helloCount++;
  }

  // A talker with a wired-back listener sees nothing here; that is fine. But if
  // you loop the two modules' own UARTs, this proves the port is bidirectional.
  while (E32_PORT.available()) E32_PORT.read();

#else
  while (E32_PORT.available()) {
    uint8_t b = (uint8_t)E32_PORT.read();
    g_rxBytes++;

  #if E32_SHOW_HEX
    Serial.print(F("<")); if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX); Serial.print(F("> "));
  #endif

    // Print text as text; anything else as hex so garbage is visibly garbage.
    // Solid readable "hello N" = the link works. Consistent mojibake = the two
    // modules disagree on air rate or UART baud, NOT on channel.
    if (b == '\n')                   Serial.println();
    else if (b == '\r')              { /* swallow */ }
    else if (b >= 0x20 && b < 0x7F)  Serial.write(b);
    else                             { Serial.print(F("[")); Serial.print(b, HEX); Serial.print(F("]")); }

    g_nextNag = millis() + QUIET_NAG_MS;
  }

  if ((int32_t)(millis() - g_nextNag) >= 0) {
    g_nextNag = millis() + QUIET_NAG_MS;
    Serial.print(F("[quiet] nothing for "));
    Serial.print(QUIET_NAG_MS / 1000);
    Serial.print(F("s (total bytes so far: "));
    Serial.print(g_rxBytes);
    Serial.println(F(")"));
  }
#endif
}
