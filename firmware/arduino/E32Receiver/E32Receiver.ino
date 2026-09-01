/*
  ==========================================================
  E32 CONFIG READER
  Reads and decodes an E32's STORED parameters.
  ==========================================================
  WHY: two E32s only hear each other if their channel AND air rate match.
  Everything else can be perfect -- wiring, power, antennas, both modules in the
  same room -- and they will still be silent if these bytes differ. Nothing in
  normal operation reveals them, so they get assumed rather than checked.
  This asks the module directly.
  ----------------------------------------------------------
  IMPORTANT: PUT THE MODULE IN CONFIG MODE BY HAND
  ----------------------------------------------------------
  Move the M0 and M1 jumpers OFF GND and onto 3.3V:
      M0 -> 3.3V
      M1 -> 3.3V
  That is mode 3 (sleep/config). No Arduino pins and no extra resistors are
  needed for the mode pins -- you are wiring them to a rail, not driving them.
  In CONFIG mode the module's UART runs at 9600 8N1 NO MATTER what baud it is
  configured for, which is why this works even on a module set to 1200.
  Everything else stays as it is, including the divider on TX -> RXD. This
  sketch WRITES to the module, so that divider must be present.
  When you are done: put M0 and M1 back to GND for normal operation.
  ----------------------------------------------------------
  RUN IT ON BOTH MODULES AND COMPARE.
  Channel and air rate must match. If they differ, that is your answer.
  ----------------------------------------------------------
*/
#include <SoftwareSerial.h>
// ---- set these to match the board you are running on ---------------------
// Her ground-station rig:   RX = 2,  TX = 3
// His receiver rig:         RX = 10, TX = 11
const uint8_t E32_RX_PIN = 2;   // Arduino receives  <- E32 TXD
const uint8_t E32_TX_PIN = 3;   // Arduino transmits -> E32 RXD (through divider)
SoftwareSerial E32Serial(E32_RX_PIN, E32_TX_PIN);
static void printBaud(uint8_t code) {
  switch (code) {
    case 0: Serial.print(F("1200"));   break;
    case 1: Serial.print(F("2400"));   break;
    case 2: Serial.print(F("4800"));   break;
    case 3: Serial.print(F("9600"));   break;
    case 4: Serial.print(F("19200"));  break;
    case 5: Serial.print(F("38400"));  break;
    case 6: Serial.print(F("57600"));  break;
    default: Serial.print(F("115200"));break;
  }
}
static void printAirRate(uint8_t code) {
  switch (code) {
    case 0: Serial.print(F("0.3k")); break;
    case 1: Serial.print(F("1.2k")); break;
    case 2: Serial.print(F("2.4k")); break;
    case 3: Serial.print(F("4.8k")); break;
    case 4: Serial.print(F("9.6k")); break;
    default: Serial.print(F("19.2k"));break;
  }
}
static void printPower(uint8_t code) {
  switch (code) {
    case 0: Serial.print(F("20dBm")); break;
    case 1: Serial.print(F("17dBm")); break;
    case 2: Serial.print(F("14dBm")); break;
    default: Serial.print(F("10dBm"));break;
  }
}
void setup() {
  Serial.begin(115200);
  E32Serial.begin(9600);          // CONFIG mode is always 9600 8N1
  delay(1200);
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F("E32 CONFIG READER"));
  Serial.println(F("=========================================="));
  Serial.println(F("Module must be in CONFIG mode: M0 -> 3.3V, M1 -> 3.3V"));
  Serial.println();
  while (E32Serial.available()) E32Serial.read();   // flush stale bytes
  // C1 C1 C1 = "report your saved parameters".
  Serial.println(F("Sending C1 C1 C1 ..."));
  E32Serial.write(0xC1);
  E32Serial.write(0xC1);
  E32Serial.write(0xC1);
  // Reply is C0 ADDH ADDL SPED CHAN OPTION.
  uint8_t r[6];
  uint8_t got = 0;
  unsigned long start = millis();
  while (got < 6 && millis() - start < 2000) {
    if (E32Serial.available()) r[got++] = (uint8_t)E32Serial.read();
  }
  if (got < 6) {
    Serial.print(F("NO REPLY (got "));
    Serial.print(got);
    Serial.println(F(" of 6 bytes)."));
    Serial.println();
    Serial.println(F("Most likely causes, in order:"));
    Serial.println(F("  1. M0/M1 are not actually on 3.3V -- not in CONFIG mode."));
    Serial.println(F("  2. The TX -> RXD divider is broken, so the module never"));
    Serial.println(F("     received the command. (Same fault as 'transmitted: NO'.)"));
    Serial.println(F("  3. E32 TXD is not on this board's RX pin."));
    return;
  }
  Serial.print(F("Raw reply :"));
  for (uint8_t i = 0; i < 6; i++) {
    Serial.print(' ');
    if (r[i] < 0x10) Serial.print('0');
    Serial.print(r[i], HEX);
  }
  Serial.println();
  Serial.println();
  uint8_t sped = r[3], chan = r[4], opt = r[5];
  Serial.print(F("Address   : 0x"));
  if (r[1] < 0x10) Serial.print('0');
  Serial.print(r[1], HEX);
  if (r[2] < 0x10) Serial.print('0');
  Serial.println(r[2], HEX);
  Serial.print(F("UART baud : "));
  printBaud((sped >> 3) & 0x07);
  Serial.println(F(" 8N1"));
  Serial.print(F("AIR RATE  : "));
  printAirRate(sped & 0x07);
  Serial.println(F("   <-- MUST MATCH THE OTHER MODULE"));
  Serial.print(F("CHANNEL   : 0x"));
  if (chan < 0x10) Serial.print('0');
  Serial.print(chan, HEX);
  Serial.print(F("  ("));
  Serial.print(410 + chan);
  Serial.println(F(" MHz)   <-- MUST MATCH THE OTHER MODULE"));
  Serial.print(F("TX power  : "));
  printPower(opt & 0x03);
  Serial.println();
  Serial.print(F("FEC       : "));
  Serial.println((opt & 0x04) ? F("on") : F("off"));
  Serial.print(F("Mode      : "));
  Serial.println((opt & 0x80) ? F("FIXED (addressed)") : F("transparent"));
  Serial.println();
  Serial.println(F("This project expects: 9600 8N1, air rate 2.4k, channel 0x17."));
  Serial.print(F("SPED byte would be 0x1A -- this module reports 0x"));
  if (sped < 0x10) Serial.print('0');
  Serial.println(sped, HEX);
  Serial.println();
  Serial.println(F("Run this on BOTH modules. If air rate or channel differ,"));
  Serial.println(F("that is why they cannot hear each other."));
  Serial.println();
  Serial.println(F("Put M0/M1 back to GND when finished."));
}
void loop() {
}