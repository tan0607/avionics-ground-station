#include <SPI.h>
#include <FS.h>
#include <SD.h>

// =====================================================
// SD DOCTOR
// Standalone SD card fault finder for ESP32-S3
//
// No LoRa, no GPS, no IMU. Only the SD card.
// Upload this, open Serial Monitor at 115200,
// then press the number keys.
//
//   1 = Find the module   (scan every GPIO for pullups)
//   2 = Loopback test     (proves the ESP32 side works)
//   3 = Card handshake    (hardware SPI, your pins)
//   4 = Bit-bang handshake(no SPI peripheral at all)
//   5 = Permutation scan  (ignores your pin labels)
//   6 = Mount filesystem
//   7 = Format card
//   P = print pins
// =====================================================


// =====================================================
// YOUR PINS - edit to match your wiring
// =====================================================

int PIN_CS   = 7;
int PIN_SCK  = 14;
int PIN_MOSI = 15;
int PIN_MISO = 16;


#ifndef HSPI
#define HSPI 1
#endif

SPIClass sdSPI(HSPI);


// =====================================================
// GPIOs that are safe to poke on an ESP32-S3
// Excluded: 0,19,20,26-32,43,44,45,46 and 33-37
// (USB, flash, UART, strapping, octal PSRAM)
// =====================================================

const int SAFE_PINS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
  11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 47, 48
};

const int SAFE_PIN_COUNT = sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]);


void printPins();
void testPullups();
void testLoopback();
bool testHandshakeSPI(bool verbose);
bool testHandshakeBitBang(bool verbose);
void permutationScan();
void testMount();
void formatCard();
void printMenu();


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SD DOCTOR");
  Serial.println(" ESP32-S3 SD card fault finder");
  Serial.println("========================================");

  printPins();
  printMenu();

  // Run the important ones automatically at boot
  Serial.println();
  Serial.println("### AUTO TEST 1: FIND THE MODULE ###");
  testPullups();

  Serial.println();
  Serial.println("### AUTO TEST 2: CARD HANDSHAKE ###");
  testHandshakeSPI(true);

  Serial.println();
  Serial.println("Press a number key for more tests.");
  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '1') testPullups();
    else if (c == '2') testLoopback();
    else if (c == '3') testHandshakeSPI(true);
    else if (c == '4') testHandshakeBitBang(true);
    else if (c == '5') permutationScan();
    else if (c == '6') testMount();
    else if (c == '7') formatCard();
    else if (c == 'p' || c == 'P') printPins();
    else if (c == 'm' || c == 'M') printMenu();
  }
}


// =====================================================
// MENU
// =====================================================

void printMenu() {
  Serial.println();
  Serial.println("---------------- TESTS ----------------");
  Serial.println(" 1 = Find the module (GPIO pullup scan)");
  Serial.println(" 2 = Loopback test (jumper MOSI to MISO)");
  Serial.println(" 3 = Card handshake, hardware SPI");
  Serial.println(" 4 = Card handshake, bit-banged");
  Serial.println(" 5 = Permutation scan (finds swapped wires)");
  Serial.println(" 6 = Mount filesystem");
  Serial.println(" 7 = Format card");
  Serial.println(" P = print pins   M = this menu");
  Serial.println("---------------------------------------");
}


void printPins() {
  Serial.println();
  Serial.print("[PINS] CS=GPIO");
  Serial.print(PIN_CS);
  Serial.print("  SCK=GPIO");
  Serial.print(PIN_SCK);
  Serial.print("  MOSI=GPIO");
  Serial.print(PIN_MOSI);
  Serial.print("  MISO=GPIO");
  Serial.println(PIN_MISO);
}


// =====================================================
// TEST 1 - FIND THE MODULE
//
// An SD module holds MISO high through a pullup.
// Force every GPIO low with the internal pulldown and
// see which ones fight back. Any pin that stays HIGH
// has something external pulling it up.
//
// This finds your module even if you wired it to
// completely different pins than you think.
// =====================================================

void testPullups() {
  Serial.println();
  Serial.println("--- GPIO PULLUP SCAN ---");
  Serial.println("Looking for pins held HIGH by external hardware...");

  int found = 0;

  for (int i = 0; i < SAFE_PIN_COUNT; i++) {
    int p = SAFE_PINS[i];

    pinMode(p, INPUT_PULLDOWN);
    delay(3);
    int withPulldown = digitalRead(p);

    pinMode(p, INPUT_PULLUP);
    delay(3);
    int withPullup = digitalRead(p);

    pinMode(p, INPUT);

    if (withPulldown == 1) {
      Serial.print("  GPIO");
      Serial.print(p);
      Serial.println("  HELD HIGH  <-- external pullup here");
      found++;
    }
    else if (withPullup == 0) {
      Serial.print("  GPIO");
      Serial.print(p);
      Serial.println("  HELD LOW   <-- shorted to GND");
      found++;
    }
  }

  Serial.println("------------------------");

  if (found == 0) {
    Serial.println(">>> NOTHING FOUND ON ANY PIN <<<");
    Serial.println("No powered device is connected to any GPIO.");
    Serial.println("This means one of:");
    Serial.println("  a) The module has NO POWER.");
    Serial.println("     Measure VCC to GND on the module itself.");
    Serial.println("     Should read 4.5-5.2V (regulator module)");
    Serial.println("     or 3.2-3.4V (bare adapter).");
    Serial.println("  b) GND is not connected between board and module.");
    Serial.println("  c) The module is dead.");
    Serial.println("  d) Breadboard rail not making contact.");
  }
  else {
    Serial.print("Found ");
    Serial.print(found);
    Serial.println(" pin(s) with external hardware attached.");
    Serial.print("Your MISO pin should be one of them. You set MISO=GPIO");
    Serial.println(PIN_MISO);
  }
}


// =====================================================
// TEST 2 - LOOPBACK
//
// Disconnect the SD module. Put a single jumper wire
// straight from the MOSI pin to the MISO pin.
// Whatever we send must come straight back.
//
// Passes  -> the ESP32, its pins and the SPI bus are fine,
//            the fault is the module or the card.
// Fails   -> the fault is the board or those GPIOs.
// =====================================================

void testLoopback() {
  Serial.println();
  Serial.println("--- SPI LOOPBACK TEST ---");
  Serial.print("Put ONE jumper wire from GPIO");
  Serial.print(PIN_MOSI);
  Serial.print(" (MOSI) to GPIO");
  Serial.print(PIN_MISO);
  Serial.println(" (MISO).");
  Serial.println("Unplug the SD module first.");
  Serial.println("Starting in 5 seconds...");

  delay(5000);

  sdSPI.end();
  delay(50);
  sdSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);

  sdSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  uint8_t patterns[] = { 0xA5, 0x5A, 0xFF, 0x00, 0x0F, 0xF0 };
  int pass = 0;

  for (int i = 0; i < 6; i++) {
    uint8_t got = sdSPI.transfer(patterns[i]);

    Serial.print("  sent 0x");
    if (patterns[i] < 0x10) Serial.print("0");
    Serial.print(patterns[i], HEX);
    Serial.print("  got 0x");
    if (got < 0x10) Serial.print("0");
    Serial.print(got, HEX);

    if (got == patterns[i]) {
      Serial.println("  OK");
      pass++;
    }
    else {
      Serial.println("  MISMATCH");
    }
  }

  sdSPI.endTransaction();

  Serial.println("-------------------------");

  if (pass == 6) {
    Serial.println(">>> LOOPBACK PASSED <<<");
    Serial.println("The ESP32, these GPIOs and the SPI bus all work.");
    Serial.println("The fault is the SD MODULE, the CARD, or their wiring.");
    Serial.println("Reconnect the module and press 3.");
  }
  else {
    Serial.println(">>> LOOPBACK FAILED <<<");
    Serial.println("Either the jumper is not fitted, or one of");
    Serial.print("GPIO");
    Serial.print(PIN_MOSI);
    Serial.print(" / GPIO");
    Serial.print(PIN_MISO);
    Serial.println(" is damaged or unusable on this board.");
    Serial.println("Try different GPIOs - edit PIN_MOSI / PIN_MISO.");
  }
}


// =====================================================
// RAW SD COMMAND OVER HARDWARE SPI
// =====================================================

uint8_t spiCmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  sdSPI.transfer(0xFF);

  sdSPI.transfer(0x40 | cmd);
  sdSPI.transfer((uint8_t)(arg >> 24));
  sdSPI.transfer((uint8_t)(arg >> 16));
  sdSPI.transfer((uint8_t)(arg >> 8));
  sdSPI.transfer((uint8_t)(arg));
  sdSPI.transfer(crc);

  for (int i = 0; i < 10; i++) {
    uint8_t r = sdSPI.transfer(0xFF);
    if ((r & 0x80) == 0) return r;
  }

  return 0xFF;
}


// =====================================================
// TEST 3 - CARD HANDSHAKE (HARDWARE SPI)
// =====================================================

bool testHandshakeSPI(bool verbose) {
  if (verbose) {
    Serial.println();
    Serial.println("--- CARD HANDSHAKE (hardware SPI) ---");
    printPins();
  }

  sdSPI.end();
  delay(50);
  sdSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);

  sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  // Wake up: >74 clocks with CS high
  for (int i = 0; i < 12; i++) {
    sdSPI.transfer(0xFF);
  }

  uint8_t r1 = 0xFF;

  for (int attempt = 0; attempt < 8; attempt++) {
    digitalWrite(PIN_CS, LOW);
    r1 = spiCmd(0, 0, 0x95);
    digitalWrite(PIN_CS, HIGH);
    sdSPI.transfer(0xFF);

    if (r1 == 0x01) break;
    delay(20);
  }

  sdSPI.endTransaction();

  if (verbose) {
    Serial.print("CMD0 response: 0x");
    if (r1 < 0x10) Serial.print("0");
    Serial.println(r1, HEX);
  }

  if (r1 == 0x01) {
    if (verbose) {
      Serial.println(">>> CARD RESPONDED - HARDWARE IS GOOD <<<");
      Serial.println("Press 6 to mount, or 7 to format.");
    }
    return true;
  }

  if (verbose) {
    if (r1 == 0xFF) {
      Serial.println(">>> NO RESPONSE (all ones) <<<");
      Serial.println("MISO is idle high but nothing is talking.");
      Serial.println("Most likely, in order:");
      Serial.println("  1. MOSI and MISO swapped  -> press 5");
      Serial.println("  2. SCK on the wrong pin   -> press 5");
      Serial.println("  3. Card not fully seated  -> push until it clicks");
      Serial.println("  4. Module needs 5V, is on 3.3V");
      Serial.println("  5. Card is dead - try another card");
      Serial.println("Also try 4 (bit-bang) and 2 (loopback).");
    }
    else if (r1 == 0x00) {
      Serial.println(">>> ALL ZEROS <<<");
      Serial.println("MISO is stuck low. Module unpowered,");
      Serial.println("MISO shorted to GND, or MISO on the wrong pin.");
    }
    else {
      Serial.println(">>> CARD ANSWERED BUT NOT IDLE <<<");
      Serial.println("Wiring is right. Card may be faulty or");
      Serial.println("the supply is sagging. Add a 100uF cap.");
    }
  }

  return false;
}


// =====================================================
// BIT-BANGED SPI
// Pure digitalWrite / digitalRead, SPI mode 0.
// Bypasses the SPI peripheral entirely, so it rules
// out any bus, pin-matrix or library problem.
// =====================================================

uint8_t bbTransfer(uint8_t out) {
  uint8_t in = 0;

  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (out >> i) & 0x01);
    delayMicroseconds(4);

    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(4);

    in <<= 1;
    if (digitalRead(PIN_MISO)) {
      in |= 0x01;
    }

    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(4);
  }

  return in;
}


uint8_t bbCmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  bbTransfer(0xFF);

  bbTransfer(0x40 | cmd);
  bbTransfer((uint8_t)(arg >> 24));
  bbTransfer((uint8_t)(arg >> 16));
  bbTransfer((uint8_t)(arg >> 8));
  bbTransfer((uint8_t)(arg));
  bbTransfer(crc);

  for (int i = 0; i < 10; i++) {
    uint8_t r = bbTransfer(0xFF);
    if ((r & 0x80) == 0) return r;
  }

  return 0xFF;
}


// =====================================================
// TEST 4 - BIT-BANG HANDSHAKE
// =====================================================

bool testHandshakeBitBang(bool verbose) {
  if (verbose) {
    Serial.println();
    Serial.println("--- CARD HANDSHAKE (bit-banged) ---");
    Serial.println("No SPI peripheral involved. Slow but honest.");
  }

  sdSPI.end();
  delay(50);

  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_MISO, INPUT_PULLUP);

  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(PIN_MOSI, HIGH);

  delay(10);

  for (int i = 0; i < 12; i++) {
    bbTransfer(0xFF);
  }

  uint8_t r1 = 0xFF;

  for (int attempt = 0; attempt < 8; attempt++) {
    digitalWrite(PIN_CS, LOW);
    r1 = bbCmd(0, 0, 0x95);
    digitalWrite(PIN_CS, HIGH);
    bbTransfer(0xFF);

    if (r1 == 0x01) break;
    delay(20);
  }

  if (verbose) {
    Serial.print("CMD0 response: 0x");
    if (r1 < 0x10) Serial.print("0");
    Serial.println(r1, HEX);

    if (r1 == 0x01) {
      Serial.println(">>> CARD RESPONDED TO BIT-BANG <<<");
      Serial.println("The wiring and card are fine.");
      Serial.println("If test 3 fails but this passes, the SPI");
      Serial.println("bus config is the problem, not your wiring.");
    }
    else {
      Serial.println(">>> NO RESPONSE TO BIT-BANG EITHER <<<");
      Serial.println("This is real hardware. It is not a software");
      Serial.println("or SPI bus problem. Check power and wiring,");
      Serial.println("then press 5 to scan for swapped wires.");
    }
  }

  return (r1 == 0x01);
}


// =====================================================
// TEST 5 - PERMUTATION SCAN
//
// Takes the four GPIOs you are using and tries every
// possible assignment of CS / SCK / MOSI / MISO.
// If any one of the 24 gets a CMD0 response, your
// wires are simply in the wrong order - and this
// tells you the correct order.
// =====================================================

void permutationScan() {
  Serial.println();
  Serial.println("--- PERMUTATION SCAN ---");
  Serial.println("Trying all 24 orderings of your four pins.");
  Serial.println("This finds swapped wires without rewiring.");

  int pins[4] = { PIN_CS, PIN_SCK, PIN_MOSI, PIN_MISO };

  int savedCS   = PIN_CS;
  int savedSCK  = PIN_SCK;
  int savedMOSI = PIN_MOSI;
  int savedMISO = PIN_MISO;

  bool foundAny = false;

  for (int a = 0; a < 4; a++) {
    for (int b = 0; b < 4; b++) {
      if (b == a) continue;

      for (int c = 0; c < 4; c++) {
        if (c == a || c == b) continue;

        for (int d = 0; d < 4; d++) {
          if (d == a || d == b || d == c) continue;

          PIN_CS   = pins[a];
          PIN_SCK  = pins[b];
          PIN_MOSI = pins[c];
          PIN_MISO = pins[d];

          Serial.print("  CS=");
          Serial.print(PIN_CS);
          Serial.print(" SCK=");
          Serial.print(PIN_SCK);
          Serial.print(" MOSI=");
          Serial.print(PIN_MOSI);
          Serial.print(" MISO=");
          Serial.print(PIN_MISO);
          Serial.print("  ... ");

          bool ok = testHandshakeSPI(false);

          if (ok) {
            Serial.println("*** CARD RESPONDED ***");
            Serial.println();
            Serial.println(">>> FOUND THE CORRECT PIN ORDER <<<");
            Serial.println("Put these in your flight sketch:");
            Serial.print("  #define SD_CS    ");
            Serial.println(PIN_CS);
            Serial.print("  #define SD_SCK   ");
            Serial.println(PIN_SCK);
            Serial.print("  #define SD_MOSI  ");
            Serial.println(PIN_MOSI);
            Serial.print("  #define SD_MISO  ");
            Serial.println(PIN_MISO);
            Serial.println();
            foundAny = true;
            return;
          }

          Serial.println("no");
        }
      }
    }
  }

  PIN_CS   = savedCS;
  PIN_SCK  = savedSCK;
  PIN_MOSI = savedMOSI;
  PIN_MISO = savedMISO;

  if (!foundAny) {
    Serial.println();
    Serial.println(">>> NO ORDERING WORKED <<<");
    Serial.println("The problem is not a swapped wire.");
    Serial.println("It is power, a broken wire, a dead card,");
    Serial.println("or a dead module.");
    Serial.println();
    Serial.println("Do this, in order:");
    Serial.println(" 1. Measure VCC to GND ON THE MODULE with a meter.");
    Serial.println(" 2. Press 1 - if nothing is found, it has no power.");
    Serial.println(" 3. Press 2 - loopback proves the ESP32 side.");
    Serial.println(" 4. Swap in a different micro SD card.");
    Serial.println(" 5. Swap in a different module.");
  }
}


// =====================================================
// TEST 6 - MOUNT
// =====================================================

void testMount() {
  Serial.println();
  Serial.println("--- MOUNT FILESYSTEM ---");

  SD.end();
  delay(50);

  sdSPI.end();
  delay(50);
  sdSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  bool ok = SD.begin(PIN_CS, sdSPI, 10000000);

  if (!ok) {
    Serial.println("10 MHz failed, trying 4 MHz");
    ok = SD.begin(PIN_CS, sdSPI, 4000000);
  }

  if (!ok) {
    Serial.println("4 MHz failed, trying 1 MHz");
    ok = SD.begin(PIN_CS, sdSPI, 1000000);
  }

  if (!ok) {
    Serial.println(">>> MOUNT FAILED <<<");
    Serial.println("If test 3 passed, the card is alive but has");
    Serial.println("no readable filesystem. Press 7 to format.");
    return;
  }

  Serial.println(">>> MOUNTED <<<");

  Serial.print("Card size: ");
  Serial.print((unsigned long)(SD.cardSize() / (1024ULL * 1024ULL)));
  Serial.println(" MB");

  Serial.print("Used: ");
  Serial.print((unsigned long)(SD.usedBytes() / (1024ULL * 1024ULL)));
  Serial.print(" MB of ");
  Serial.print((unsigned long)(SD.totalBytes() / (1024ULL * 1024ULL)));
  Serial.println(" MB");

  // Write and read back
  File f = SD.open("/doctor.txt", FILE_WRITE);

  if (!f) {
    Serial.println("Could not create /doctor.txt");
    return;
  }

  f.println("SD DOCTOR WRITE TEST OK");
  f.flush();
  f.close();

  File r = SD.open("/doctor.txt", FILE_READ);

  if (!r) {
    Serial.println("Could not read /doctor.txt back");
    return;
  }

  String line = r.readStringUntil('\n');
  r.close();

  Serial.print("Read back: \"");
  Serial.print(line);
  Serial.println("\"");

  if (line.startsWith("SD DOCTOR")) {
    Serial.println(">>> WRITE AND READ VERIFIED - CARD FULLY WORKING <<<");
  }
  else {
    Serial.println(">>> READ BACK DID NOT MATCH <<<");
  }
}


// =====================================================
// TEST 7 - FORMAT
// =====================================================

void formatCard() {
  Serial.println();
  Serial.println("--- FORMAT CARD ---");
  Serial.println("THIS ERASES EVERYTHING. Send Y within 10 seconds.");

  unsigned long t0 = millis();
  bool confirmed = false;

  while (millis() - t0 < 10000) {
    if (Serial.available() > 0) {
      char c = Serial.read();

      if (c == 'y' || c == 'Y') {
        confirmed = true;
        break;
      }

      if (c != '\n' && c != '\r') break;
    }

    delay(10);
  }

  if (!confirmed) {
    Serial.println("Cancelled.");
    return;
  }

  Serial.println("Formatting, do not unplug...");

  SD.end();
  delay(200);

  sdSPI.end();
  delay(50);
  sdSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  bool ok = SD.begin(PIN_CS, sdSPI, 4000000, "/sd", 5, true);

  if (ok) {
    Serial.println(">>> FORMAT OK <<<");
    Serial.println("Press 6 to verify.");
  }
  else {
    Serial.println(">>> FORMAT FAILED <<<");
    Serial.println("Use a 32 GB or smaller card.");
  }
}
