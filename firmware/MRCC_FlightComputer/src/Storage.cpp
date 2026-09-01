#include "Storage.h"
#include "Flight.h"
#include "Pyro.h"
#include "Config.h"
#include "State.h"

#ifndef HSPI
#define HSPI 1
#endif

SPIClass sdSPI(HSPI);
File     logFile;

char          logFileName[32] = "";
unsigned long logLineCount    = 0;
unsigned long sdErrorCount    = 0;

uint8_t probeResult = PROBE_NO_MODULE;

static unsigned long lastLog   = 0;
static bool cardIsHighCapacity = false;

static void logOneLine();
static bool testMisoLine(bool verbose);
static bool testCardHandshake(bool verbose);
static void verifyWrite();


// =====================================================
// STEP 1 - IS THE MODULE THERE?
//
// Force the pin low with an internal pulldown. A powered
// module's pullup wins and it still reads HIGH.
// =====================================================

static bool testMisoLine(bool verbose) {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(20);

  int idlePulldown = 0;
  int idlePullup   = 0;
  int selPulldown  = 0;

  // Retry - a regulator on a marginal rail needs time
  for (int attempt = 0; attempt < 3; attempt++) {
    pinMode(SD_MISO, INPUT_PULLDOWN);
    delay(5);
    idlePulldown = digitalRead(SD_MISO);

    pinMode(SD_MISO, INPUT_PULLUP);
    delay(5);
    idlePullup = digitalRead(SD_MISO);

    digitalWrite(SD_CS, LOW);
    delay(5);
    pinMode(SD_MISO, INPUT_PULLDOWN);
    delay(5);
    selPulldown = digitalRead(SD_MISO);

    digitalWrite(SD_CS, HIGH);
    pinMode(SD_MISO, INPUT);
    delay(5);

    if (idlePulldown == 1 || selPulldown == 1) break;

    if (attempt < 2) delay(300);
  }

  if (verbose) {
    Serial.print("[SD] MISO test  idle(pd/pu)=");
    Serial.print(idlePulldown);
    Serial.print("/");
    Serial.print(idlePullup);
    Serial.print("  selected(pd)=");
    Serial.println(selPulldown);
  }

  if (idlePullup == 0) {
    if (verbose) {
      Serial.println("[SD] >>> MISO STUCK LOW - POWER FAULT <<<");
      Serial.println("[SD] Reads LOW even with the pullup on, so this");
      Serial.println("[SD] is not a loose data line or a wrong pin.");
      Serial.println("[SD] Measure VCC to GND on the module itself.");
    }
    probeResult = PROBE_MISO_LOW;
    return false;
  }

  if (idlePulldown == 1 || selPulldown == 1) {
    if (verbose) Serial.println("[SD] MODULE DETECTED");
    return true;
  }

  if (verbose) {
    Serial.println("[SD] >>> NOTHING ON MISO <<<");
    Serial.println("[SD] Module not connected, not powered,");
    Serial.println("[SD] or the MISO wire is on the wrong pin.");
  }
  probeResult = PROBE_NO_MODULE;
  return false;
}


// =====================================================
// RAW SD COMMAND
// =====================================================

static uint8_t sdCommand(uint8_t cmd, uint32_t arg, uint8_t crc) {
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
// STEP 2 - IS A LIVE CARD IN THE SOCKET?
//
// CMD0 proves the card exists and every SPI line is
// correct, with no filesystem involved.
// =====================================================

static bool testCardHandshake(bool verbose) {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  for (int i = 0; i < 12; i++) sdSPI.transfer(0xFF);

  uint8_t r1 = 0xFF;

  for (int attempt = 0; attempt < 8; attempt++) {
    digitalWrite(SD_CS, LOW);
    r1 = sdCommand(0, 0, 0x95);
    digitalWrite(SD_CS, HIGH);
    sdSPI.transfer(0xFF);

    if (r1 == 0x01) break;
    delay(20);
  }

  if (verbose) {
    Serial.print("[SD] CMD0 response: 0x");
    if (r1 < 0x10) Serial.print("0");
    Serial.println(r1, HEX);
  }

  if (r1 != 0x01) {
    sdSPI.endTransaction();

    if (verbose) {
      if (r1 == 0xFF) {
        Serial.println("[SD] >>> NO CARD RESPONSE <<<");
        Serial.println("[SD] Module wired, but no card answered.");
        Serial.println("[SD] Not inserted, not seated, or dead.");
      }
      else if (r1 == 0x00) {
        Serial.println("[SD] >>> MISO READS ALL ZEROS <<<");
      }
      else {
        Serial.println("[SD] >>> CARD REPLIED BUT NOT IDLE <<<");
      }
    }

    probeResult = PROBE_NO_CARD;
    return false;
  }

  // CMD8 - v1 or v2
  digitalWrite(SD_CS, LOW);
  uint8_t r8 = sdCommand(8, 0x000001AA, 0x87);

  bool isV2 = false;

  if (r8 == 0x01) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = sdSPI.transfer(0xFF);
    isV2 = (b[3] == 0xAA);
  }

  digitalWrite(SD_CS, HIGH);
  sdSPI.transfer(0xFF);

  // ACMD41 - finish init
  uint8_t rInit  = 0xFF;
  unsigned long t0 = millis();

  while (millis() - t0 < 1500) {
    digitalWrite(SD_CS, LOW);
    sdCommand(55, 0, 0x65);
    rInit = sdCommand(41, isV2 ? 0x40000000UL : 0, 0x77);
    digitalWrite(SD_CS, HIGH);
    sdSPI.transfer(0xFF);

    if (rInit == 0x00) break;
    delay(10);
  }

  if (rInit != 0x00) {
    if (verbose) {
      Serial.println("[SD] Card found but failed ACMD41 init.");
      Serial.println("[SD] Worn out card, or a weak 3.3V supply.");
    }
    sdSPI.endTransaction();
    probeResult = PROBE_NO_CARD;
    return false;
  }

  // CMD58 - capacity class
  digitalWrite(SD_CS, LOW);
  uint8_t r58 = sdCommand(58, 0, 0xFD);

  if (r58 == 0x00) {
    uint8_t ocr0 = sdSPI.transfer(0xFF);
    sdSPI.transfer(0xFF);
    sdSPI.transfer(0xFF);
    sdSPI.transfer(0xFF);
    cardIsHighCapacity = (ocr0 & 0x40) != 0;
  }

  digitalWrite(SD_CS, HIGH);
  sdSPI.transfer(0xFF);
  sdSPI.endTransaction();

  if (verbose) {
    Serial.print("[SD] Card class: ");
    Serial.println(cardIsHighCapacity ? "SDHC / SDXC" : "SDSC");
    Serial.println("[SD] *** HARDWARE OK ***");
  }

  probeResult = PROBE_CARD_OK;
  return true;
}


// =====================================================
// INIT
// =====================================================

bool initSD(bool verbose) {
  if (verbose) {
    Serial.println();
    Serial.println("--------- SD DIAGNOSTIC ---------");
    Serial.print("[SD] Pins  CS=");
    Serial.print(SD_CS);
    Serial.print("  SCK=");
    Serial.print(SD_SCK);
    Serial.print("  MOSI=");
    Serial.print(SD_MOSI);
    Serial.print("  MISO=");
    Serial.println(SD_MISO);
  }

  sdOK = false;

  if (!testMisoLine(verbose))      { if (verbose) printSdHelp(); return false; }
  if (!testCardHandshake(verbose)) { if (verbose) printSdHelp(); return false; }

  if (verbose) Serial.println("[SD] Mounting filesystem...");

  SD.end();
  delay(50);

  sdOK = SD.begin(SD_CS, sdSPI, 10000000);
  if (!sdOK) sdOK = SD.begin(SD_CS, sdSPI, 4000000);
  if (!sdOK) sdOK = SD.begin(SD_CS, sdSPI, 1000000);

  if (!sdOK) {
    if (verbose) {
      Serial.println("[SD] >>> CARD ALIVE BUT NO USABLE FILESYSTEM <<<");
      Serial.println("[SD] It is exFAT, blank or corrupted.");
      Serial.println("[SD] Send  F  to format it here - no reader needed.");
    }
    return false;
  }

  if (verbose) {
    uint8_t cardType = SD.cardType();

    Serial.print("[SD] Card type: ");
    if (cardType == CARD_MMC)       Serial.println("MMC");
    else if (cardType == CARD_SD)   Serial.println("SDSC");
    else if (cardType == CARD_SDHC) Serial.println("SDHC");
    else                            Serial.println("UNKNOWN");

    Serial.print("[SD] Card size: ");
    Serial.print((unsigned long)(SD.cardSize() / (1024ULL * 1024ULL)));
    Serial.println(" MB");
  }

  startNewLogFile();
  return sdOK;
}


// =====================================================
// LOG FILE
// =====================================================

void startNewLogFile() {
  if (!sdOK) return;

  if (logFile) {
    logFile.flush();
    logFile.close();
  }

  for (int i = 1; i < 1000; i++) {
    snprintf(logFileName, sizeof(logFileName), "/FLIGHT%03d.CSV", i);
    if (!SD.exists(logFileName)) break;
  }

  Serial.print("[SD] Log file: ");
  Serial.println(logFileName);

  logFile = SD.open(logFileName, FILE_WRITE);

  if (!logFile) {
    Serial.println("[SD] ERROR - cannot create file");
    sdOK = false;
    return;
  }

  // ST is the numeric flight state (see Flight.h):
  // 0 PAD 1 ARMED 2 BOOST 3 COAST 4 APOGEE 5 DESCENT 6 LANDED
  logFile.println(
    "PKT,T,GD,GF,SAT,LAT,LON,GA,GS,CRS,"
    "AX,AY,AZ,GX,GY,GZ,MX,MY,MZ,VX,VY,HDG,VB,BOOT,"
    "ST,ALT,VZ,MAXALT,PRS,BTEMP,ARM,FIR,"
    // Filtered twins. Every column from here on has a raw
    // counterpart above it, so one flight gives you both
    // the before and the after with nothing to line up.
    "FAX,FAY,FAZ,FGX,FGY,FGZ,FMX,FMY,FMZ,"
    "ANRM,FANRM,RA,PA,RL,PL,RC,PC,RK,PK,FHDG,ATR,IHZ"
  );

  // Marker so you can see across files whether the
  // board restarted, and why
  logFile.print("# BOOT=");
  logFile.print(bootCount);
  logFile.print(" RESET=");
  logFile.println(resetReasonName);

  logFile.flush();
  logFile.close();

  logLineCount = 0;
  sdErrorCount = 0;

  verifyWrite();

  logFile = SD.open(logFileName, FILE_APPEND);

  if (!logFile) {
    Serial.println("[SD] ERROR - cannot reopen for append");
    sdOK = false;
  }
}


static void verifyWrite() {
  File f = SD.open(logFileName, FILE_READ);

  if (!f) {
    Serial.println("[SD] VERIFY FAILED - cannot reopen");
    sdOK = false;
    return;
  }

  String firstLine        = f.readStringUntil('\n');
  unsigned long sizeOnCard = f.size();
  f.close();

  Serial.print("[SD] Read back ");
  Serial.print(sizeOnCard);
  Serial.println(" bytes from the card");

  if (firstLine.startsWith("PKT,T,GD")) {
    Serial.println("[SD] *** WRITE VERIFIED ***");
  }
  else {
    Serial.println("[SD] VERIFY FAILED - data did not match");
    sdOK = false;
  }
}


// =====================================================
// LOGGING - fixed 10 Hz, drift free
// =====================================================

void serviceLogging() {
  if (millis() - lastLog < LOG_INTERVAL) return;

  lastLog += LOG_INTERVAL;

  // More than 4 intervals behind - stop trying to catch up
  if (millis() - lastLog >= LOG_INTERVAL * 4) {
    lastLog = millis();
  }

  logOneLine();
}


static void logOneLine() {
  if (!sdOK || !logFile) return;

  char line[512];

  int len = snprintf(
    line, sizeof(line),
    "%lu,%.2f,%d,%d,%d,%.6f,%.6f,%.1f,%.2f,%.0f,"
    "%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.0f,%.2f,%lu,"
    "%d,%.2f,%.2f,%.2f,%.2f,%.1f,%d,%d,"
    "%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
    "%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%d,%.0f",
    packetNumber, millis() / 1000.0,
    gpsData ? 1 : 0, gpsFix ? 1 : 0, satellites,
    latitude, longitude, gpsAltitude, gpsSpeed, gpsCourse,
    ax, ay, az, gx, gy, gz, mx, my, mz,
    vx, vy, heading, vbat, bootCount,
    flightState, altFiltered, vertVel, maxAlt, pressure, baroTemp,
    pyroArmed ? 1 : 0, pyroFired ? 1 : 0,
    fax, fay, faz, fgx, fgy, fgz, fmx, fmy, fmz,
    accelNormRaw, accelNormFilt,
    rollAcc, pitchAcc, rollLpf, pitchLpf,
    rollComp, pitchComp, rollKal, pitchKal,
    headingFilt, accelTrusted ? 1 : 0, imuHz
  );

  if (len <= 0 || len >= (int)sizeof(line)) {
    sdErrorCount++;
    return;
  }

  size_t written = logFile.println(line);

  if (written < (size_t)len) {
    sdErrorCount++;
    return;
  }

  logLineCount++;
}


void flushSD() {
  if (!sdOK || !logFile) return;
  logFile.flush();
}


// =====================================================
// DUMP - read the card back over serial.
// This is how you recover data with no card reader.
// =====================================================

void dumpLogFile() {
  if (!sdOK) {
    Serial.println("[SD] OFFLINE - nothing to dump");
    return;
  }

  if (logFile) {
    logFile.flush();
    logFile.close();
  }

  File f = SD.open(logFileName, FILE_READ);

  if (!f) {
    Serial.println("[SD] ERROR - cannot open for reading");
    return;
  }

  Serial.println();
  Serial.println("=========== BEGIN FILE DUMP ===========");
  Serial.print("FILE: ");
  Serial.println(logFileName);
  Serial.print("SIZE: ");
  Serial.print(f.size());
  Serial.println(" bytes");
  Serial.println("---------------------------------------");

  uint8_t buf[256];

  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    Serial.write(buf, n);
  }

  f.close();

  Serial.println();
  Serial.println("============ END FILE DUMP ============");
  Serial.println();

  logFile = SD.open(logFileName, FILE_APPEND);
  if (!logFile) sdOK = false;
}


void listFiles() {
  if (!sdOK) {
    Serial.println("[SD] OFFLINE");
    return;
  }

  if (logFile) logFile.flush();

  File root = SD.open("/");

  if (!root) {
    Serial.println("[SD] ERROR - cannot open root");
    return;
  }

  Serial.println();
  Serial.println("---------- FILES ON CARD ----------");

  File entry = root.openNextFile();

  while (entry) {
    Serial.print(entry.isDirectory() ? "[DIR ] " : "[FILE] ");
    Serial.print(entry.name());
    Serial.print("  ");
    Serial.print(entry.size());
    Serial.println(" bytes");

    entry.close();
    entry = root.openNextFile();
  }

  root.close();
  Serial.println("-----------------------------------");
  Serial.println();
}


// =====================================================
// FORMAT ON BOARD - for when you have no card reader
// =====================================================

void formatCard() {
  Serial.println();
  Serial.println("[SD] ===== FORMAT CARD =====");

  if (probeResult != PROBE_CARD_OK) {
    Serial.println("[SD] No live card. Fix the hardware first.");
    Serial.println("[SD] Send  I  to re-test.");
    return;
  }

  Serial.println("[SD] THIS ERASES EVERYTHING.");
  Serial.println("[SD] Send  Y  within 10 seconds to confirm.");

  unsigned long t0 = millis();
  bool confirmed   = false;

  while (millis() - t0 < 10000) {
    if (Serial.available() > 0) {
      char c = Serial.read();
      if (c == 'y' || c == 'Y') { confirmed = true; break; }
      if (c != '\n' && c != '\r') break;
    }
    delay(10);
  }

  if (!confirmed) {
    Serial.println("[SD] Format cancelled.");
    return;
  }

  Serial.println("[SD] Formatting... do not unplug.");

  if (logFile) logFile.close();

  SD.end();
  delay(200);

  sdOK = SD.begin(SD_CS, sdSPI, 4000000, "/sd", 5, true);

  if (!sdOK) {
    Serial.println("[SD] FORMAT FAILED. Try a 32 GB or smaller card.");
    return;
  }

  Serial.println("[SD] *** FORMAT SUCCESSFUL ***");
  startNewLogFile();
}


void printSdHelp() {
  Serial.println("[SD] ---- CHECK THIS ----");
  Serial.println("[SD] 1. POWER: modules with a regulator chip need");
  Serial.println("[SD]    5V on VCC. Bare adapters need 3.3V.");
  Serial.println("[SD] 2. Wiring:");
  Serial.print  ("[SD]      CS   -> GPIO"); Serial.println(SD_CS);
  Serial.print  ("[SD]      SCK  -> GPIO"); Serial.println(SD_SCK);
  Serial.print  ("[SD]      MOSI -> GPIO"); Serial.println(SD_MOSI);
  Serial.print  ("[SD]      MISO -> GPIO"); Serial.println(SD_MISO);
  Serial.println("[SD]      GND  -> GND (must share GND)");
  Serial.println("[SD] 3. MISO and MOSI are the pair people swap most.");
  Serial.println("[SD] 4. Push the card until it clicks.");
  Serial.println("[SD] Fix it, then send  I  to re-test. No reflash.");
  Serial.println("[SD] FLIGHT CONTINUES WITHOUT LOGGING.");
  Serial.println("---------------------------------");
}
