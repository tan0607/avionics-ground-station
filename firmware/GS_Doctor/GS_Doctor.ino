#include <SPI.h>
#include <Preferences.h>

// =====================================================
// GS DOCTOR
// Standalone LoRa link fault finder for the ground
// station (classic ESP32 + SX1278)
//
// No backend, no Preferences-driven channel logic, no
// MRCC parsing. Only the radio.
//
// THE FAILURE THIS EXISTS FOR: on this link almost
// every fault looks identical from the operator's seat.
// Wrong frequency, wrong spreading factor, wrong
// bandwidth, wrong coding rate, wrong sync word, CRC
// off at one end, DIO0 in the wrong hole, a rocket that
// is not switched on - all of them produce exactly zero
// packets and not one error message. MRCC_GroundStation
// says "RX ready" and then says nothing for the rest of
// the day.
//
// So this sketch's job is to turn that one silence into
// distinguishable answers.
//
// Upload it, open Serial Monitor at 115200, press the
// number keys.
//
//   1 = Radio present?     (SPI handshake, both directions)
//   2 = Link parameters    (what the modem IS vs what it MUST be)
//   3 = Listen             (polled RX: good / CRC-bad / silent)
//   4 = DIO0 wiring        (is the interrupt line real?)
//   5 = Band scan          (RSSI sweep - find the rocket)
//   6 = A/B traffic check  (which channel has a rocket on it)
//   7 = TX beacon          (make THIS box transmit)
//   8 = SPI pin scan       (one wrong wire, found)
//   P = print pins         N = stored channel (NVS)
//
// WHY NO LoRa LIBRARY: SD_Doctor carries a bit-bang
// handshake so a fault can be found without trusting
// the SPI peripheral. Same idea here. Everything below
// talks to the SX1278 through raw register reads and
// writes, so a diagnosis never depends on the library
// version, on its init sequence, or on its opinion of
// what your pins are. If this sketch sees the radio and
// MRCC_GroundStation does not, the fault is in the
// sketch or the library, not in the wiring.
//
// Board: classic ESP32 DevKit. This is the RECEIVER's
// board - the flight computer is the S3 and has its own
// doctor in SD_Doctor.
// =====================================================


// =====================================================
// YOUR PINS - must match MRCC_GroundStation.ino
// =====================================================

int PIN_SCK  = 18;
int PIN_MISO = 19;
int PIN_MOSI = 22;
int PIN_SS   =  5;
int PIN_RST  = 14;
int PIN_DIO0 = 26;


// =====================================================
// THE CONTRACT
//
// Every one of these must equal the TRANSMITTER's
// setting in MRCC_FlightComputer/src/Radio.cpp,
// initRadio(). Test 2 reads the chip back and compares
// against this block, so if you change the link, change
// it here too or the doctor will report a fault that
// is really just a stale expectation.
// =====================================================

const long  WANT_FREQ_A  = 433300000L;   // rocket A
const long  WANT_FREQ_B  = 434100000L;   // rocket B
const int   WANT_SF      = 7;
const long  WANT_BW      = 250000L;
const int   WANT_CR      = 5;            // 4/5
const int   WANT_PREAMB  = 8;
const uint8_t WANT_SYNC  = 0x12;
const bool  WANT_CRC     = true;

// Which one this sketch tunes to on boot and in every
// test that needs a single channel. Tests 5 and 6 sweep
// regardless of this.
long gFreq = WANT_FREQ_A;


// =====================================================
// SX1278 REGISTERS
//
// Named rather than numbered at the call sites, because
// a doctor whose output you cannot check against the
// datasheet is not a diagnosis, it is another opinion.
// Numbers are from the SX1276/77/78/79 datasheet rev 7,
// LoRa mode register table.
// =====================================================

#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_RSSI_VALUE           0x1B
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42
#define REG_PA_DAC               0x4D

#define MODE_LONG_RANGE          0x80
#define MODE_SLEEP               0x00
#define MODE_STDBY               0x01
#define MODE_TX                  0x03
#define MODE_RX_CONTINUOUS       0x05

#define IRQ_TX_DONE              0x08
#define IRQ_PAYLOAD_CRC_ERROR    0x20
#define IRQ_RX_DONE              0x40
#define IRQ_VALID_HEADER         0x10

// The SX1278 answers 0x12 here and nothing else does.
// This one byte is the whole "is the radio there"
// question, which is why test 1 is built around it.
#define SX1278_VERSION           0x12

// 32 MHz crystal, 19-bit fractional PLL. freq = frf * 32e6 / 2^19
const long XTAL_HZ = 32000000L;

SPIClass  &radioSPI = SPI;
SPISettings radioSettings(8000000, MSBFIRST, SPI_MODE0);

static Preferences gPrefs;
static const char *NVS_NAMESPACE = "mrccgs";
static const char *NVS_KEY_CH    = "ch";


// =====================================================
// GPIOs that are safe to poke on a classic ESP32
//
// Excluded: 0/2/12/15 (strapping), 1/3 (UART0 - the
// serial monitor you are reading this on), 6-11 (SPI
// flash; touching them reboots the board), 20/24/28-31
// (do not exist on the module).
//
// 34-39 are INPUT ONLY. They are listed separately
// because a MISO can live there and a SCK/MOSI/SS
// cannot - a scan that ignores that would report a
// working pinout you can never wire.
// =====================================================

const int SAFE_PINS[] = {
  4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
};
const int SAFE_PIN_COUNT = sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]);

const int INPUT_ONLY_PINS[] = { 34, 35, 36, 39 };
const int INPUT_ONLY_COUNT = sizeof(INPUT_ONLY_PINS) / sizeof(INPUT_ONLY_PINS[0]);


void printPins();
void printMenu();
void testPresence();
void testParameters();
void testListen();
void testDio0();
void bandScan();
void channelCheck();
void txBeacon();
void spiPinScan();
void showNvs();


// =====================================================
// RAW REGISTER ACCESS
//
// One transaction per access, SS driven here. Nothing
// else in this sketch owns the bus, so there is no ISR
// to race - which is itself a diagnostic property: a
// register read that fails here cannot be blamed on
// concurrency.
// =====================================================

static uint8_t regRead(uint8_t addr) {
  radioSPI.beginTransaction(radioSettings);
  digitalWrite(PIN_SS, LOW);
  radioSPI.transfer(addr & 0x7F);          // bit 7 clear = read
  uint8_t v = radioSPI.transfer(0x00);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.endTransaction();
  return v;
}

static void regWrite(uint8_t addr, uint8_t value) {
  radioSPI.beginTransaction(radioSettings);
  digitalWrite(PIN_SS, LOW);
  radioSPI.transfer(addr | 0x80);          // bit 7 set = write
  radioSPI.transfer(value);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.endTransaction();
}

static void hardReset() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(10);
  digitalWrite(PIN_RST, HIGH);
  delay(20);       // datasheet asks for 5 ms; 20 is free and forgiving
}

static void setMode(uint8_t mode) {
  regWrite(REG_OP_MODE, MODE_LONG_RANGE | mode);
}

static void setFrequency(long hz) {
  // LoRa mode is only enterable from SLEEP, and the PLL
  // only latches FRF on the next mode transition. Both
  // of those are silent if you skip them: the registers
  // read back correct and the radio listens elsewhere.
  uint64_t frf = ((uint64_t)hz << 19) / XTAL_HZ;
  regWrite(REG_FRF_MSB, (uint8_t)(frf >> 16));
  regWrite(REG_FRF_MID, (uint8_t)(frf >> 8));
  regWrite(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

static long getFrequency() {
  uint32_t frf = ((uint32_t)regRead(REG_FRF_MSB) << 16) |
                 ((uint32_t)regRead(REG_FRF_MID) << 8) |
                 ((uint32_t)regRead(REG_FRF_LSB));
  return (long)(((uint64_t)frf * XTAL_HZ) >> 19);
}


// The bandwidth field is an INDEX, not a number. Kept
// as a table so test 2 can print "250 kHz" instead of
// "8", which is the difference between an operator
// spotting the fault and copying it into a bug report.
static const long BW_TABLE[] = {
  7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000
};

static long getBandwidth() {
  uint8_t idx = regRead(REG_MODEM_CONFIG_1) >> 4;
  if (idx > 9) return -1;
  return BW_TABLE[idx];
}

static int  getCodingRate()  { return ((regRead(REG_MODEM_CONFIG_1) >> 1) & 0x07) + 4; }
static int  getSpreadFactor(){ return regRead(REG_MODEM_CONFIG_2) >> 4; }
static bool getCrcOn()       { return (regRead(REG_MODEM_CONFIG_2) & 0x04) != 0; }
static int  getPreamble()    { return (regRead(REG_PREAMBLE_MSB) << 8) | regRead(REG_PREAMBLE_LSB); }


// =====================================================
// APPLY THE CONTRACT
//
// Brings the modem up on `hz` with the exact settings
// the transmitter uses. Called by every test that needs
// to actually hear a rocket.
// =====================================================

static bool radioBegin(long hz) {
  hardReset();

  if (regRead(REG_VERSION) != SX1278_VERSION) return false;

  setMode(MODE_SLEEP);                   // LoRa mode only from sleep
  delay(10);
  setFrequency(hz);

  regWrite(REG_FIFO_TX_BASE_ADDR, 0);
  regWrite(REG_FIFO_RX_BASE_ADDR, 0);

  // LNA: max gain, boost on. Not part of the contract -
  // it is a receiver-side choice - but a doctor that
  // hunts for weak signals should not do it deaf.
  regWrite(0x0C, regRead(0x0C) | 0x03);

  // MODEM_CONFIG_1: BW index 8 (250k) << 4 | CR (4/5 -> 1) << 1 | explicit header
  uint8_t bwIdx = 8;
  for (int i = 0; i < 10; i++) if (BW_TABLE[i] == WANT_BW) bwIdx = i;
  regWrite(REG_MODEM_CONFIG_1, (bwIdx << 4) | ((WANT_CR - 4) << 1) | 0x00);

  // MODEM_CONFIG_2: SF << 4 | CRC on
  regWrite(REG_MODEM_CONFIG_2, (WANT_SF << 4) | (WANT_CRC ? 0x04 : 0x00));

  // MODEM_CONFIG_3: LowDataRateOptimize off at SF7/250k, AGC on
  regWrite(REG_MODEM_CONFIG_3, 0x04);

  regWrite(REG_PREAMBLE_MSB, WANT_PREAMB >> 8);
  regWrite(REG_PREAMBLE_LSB, WANT_PREAMB & 0xFF);
  regWrite(REG_SYNC_WORD, WANT_SYNC);

  regWrite(REG_DIO_MAPPING_1, 0x00);     // DIO0 = RxDone
  setMode(MODE_STDBY);
  return true;
}

static void radioReceive() {
  regWrite(REG_IRQ_FLAGS, 0xFF);         // clear stale flags
  setMode(MODE_RX_CONTINUOUS);
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" GS DOCTOR");
  Serial.println(" ESP32 + SX1278 LoRa link fault finder");
  Serial.println("========================================");

  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  pinMode(PIN_DIO0, INPUT);
  radioSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);

  printPins();
  printMenu();

  // The two that answer "is this box broken" before you
  // start blaming the rocket. Run them without being
  // asked, exactly as SD_Doctor does.
  Serial.println();
  Serial.println("### AUTO TEST 1: RADIO PRESENT? ###");
  testPresence();

  Serial.println();
  Serial.println("### AUTO TEST 2: LINK PARAMETERS ###");
  testParameters();

  Serial.println();
  Serial.println("Press a number key for more tests.");
  Serial.println("Press 3 to listen for a rocket.");
  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();

    if      (c == '1') testPresence();
    else if (c == '2') testParameters();
    else if (c == '3') testListen();
    else if (c == '4') testDio0();
    else if (c == '5') bandScan();
    else if (c == '6') channelCheck();
    else if (c == '7') txBeacon();
    else if (c == '8') spiPinScan();
    else if (c == 'p' || c == 'P') printPins();
    else if (c == 'n' || c == 'N') showNvs();
    else if (c == 'm' || c == 'M') printMenu();
  }
}


void printPins() {
  Serial.println();
  Serial.println("--- PINS (edit at the top of this file) ---");
  Serial.printf("  SCK  = %d\n", PIN_SCK);
  Serial.printf("  MISO = %d\n", PIN_MISO);
  Serial.printf("  MOSI = %d\n", PIN_MOSI);
  Serial.printf("  SS   = %d\n", PIN_SS);
  Serial.printf("  RST  = %d\n", PIN_RST);
  Serial.printf("  DIO0 = %d\n", PIN_DIO0);
  Serial.println("  These must match MRCC_GroundStation.ino.");
}


void printMenu() {
  Serial.println();
  Serial.println("--- TESTS ---");
  Serial.println("  1 = Radio present?    SPI handshake, both directions");
  Serial.println("  2 = Link parameters   modem readback vs the contract");
  Serial.println("  3 = Listen            polled RX: good / CRC-bad / silent");
  Serial.println("  4 = DIO0 wiring       is the interrupt line real?");
  Serial.println("  5 = Band scan         RSSI sweep - find the rocket");
  Serial.println("  6 = A/B check         which channel has traffic");
  Serial.println("  7 = TX beacon         make THIS box transmit");
  Serial.println("  8 = SPI pin scan      one wrong wire, found");
  Serial.println("  P = pins   N = stored channel   M = this menu");
}


// =====================================================
// TEST 1 - RADIO PRESENT?
//
// Two questions, not one, and they fail differently:
//
//   Can we READ?   REG_VERSION must be 0x12. This
//                  exercises SCK, MISO and SS.
//   Can we WRITE?  Write a scratch register and read it
//                  back. This is the only thing that
//                  exercises MOSI, and a dead MOSI
//                  passes the read test perfectly - the
//                  module answers, you conclude the
//                  wiring is fine, and then nothing you
//                  configure ever takes.
// =====================================================

void testPresence() {
  Serial.println("Resetting the module...");
  hardReset();

  uint8_t v = regRead(REG_VERSION);
  Serial.printf("  REG_VERSION (0x42) = 0x%02X   (expect 0x%02X)\n", v, SX1278_VERSION);

  if (v == 0x00) {
    Serial.println("  FAIL - reads 0x00. MISO is stuck low, or the module");
    Serial.println("         has no power. Check 3V3 and GND first: a");
    Serial.println("         SX1278 on 5V is usually a dead SX1278.");
    return;
  }
  if (v == 0xFF) {
    Serial.println("  FAIL - reads 0xFF. MISO is floating: nothing is");
    Serial.println("         driving the line. Wrong MISO pin, a broken");
    Serial.println("         wire, or SS never going low (wrong SS pin).");
    return;
  }
  if (v != SX1278_VERSION) {
    Serial.println("  FAIL - something answers, but it is not an SX1278.");
    Serial.println("         Wrong module, or SCK/MISO swapped so the");
    Serial.println("         bits are arriving shifted.");
    return;
  }
  Serial.println("  PASS - read path works (SCK + MISO + SS).");

  // ---- write path ----
  // FIFO_ADDR_PTR is a plain read/write pointer with no
  // side effects while the modem is in standby, which
  // makes it the safest scratch register on the chip.
  setMode(MODE_SLEEP);
  delay(5);
  setMode(MODE_STDBY);

  bool wrote = true;
  for (uint8_t probe = 0x2A; probe <= 0x2C; probe++) {
    regWrite(REG_FIFO_ADDR_PTR, probe);
    uint8_t back = regRead(REG_FIFO_ADDR_PTR);
    Serial.printf("  write 0x%02X -> read 0x%02X\n", probe, back);
    if (back != probe) wrote = false;
  }
  regWrite(REG_FIFO_ADDR_PTR, 0);

  if (!wrote) {
    Serial.println("  FAIL - the module answers but will not accept writes.");
    Serial.println("         MOSI is the only line this test adds, so that");
    Serial.println("         is where to look. Everything else in this");
    Serial.println("         sketch will 'work' and configure nothing.");
    return;
  }
  Serial.println("  PASS - write path works (MOSI).");
  Serial.println("  RADIO IS PRESENT AND ADDRESSABLE.");
}


// =====================================================
// TEST 2 - LINK PARAMETERS
//
// Every setting below produces ZERO packets and NO error
// when it disagrees with the transmitter, so the only
// way to catch one is to read it back off the chip.
//
// WHAT THIS DOES AND DOES NOT PROVE. Read this before
// trusting the output.
//
//   It DOES prove the modem accepts and holds every
//   value in the contract block. A setting the chip
//   silently refuses - an illegal bandwidth index, a
//   write that landed in the wrong mode, a dead MOSI -
//   shows up here as a value that stays at its RESET
//   DEFAULT after being written, which is why both
//   columns are printed.
//
//   It does NOT prove MRCC_GroundStation uses the same
//   values, because this sketch writes its own. That
//   part is a source comparison and only you can make
//   it: the WANT_ block at the top of this file, the
//   LORA_ defines in MRCC_GroundStation.ino, and
//   initRadio() in MRCC_FlightComputer/src/Radio.cpp
//   are three copies of one contract with no compiler
//   checking any of them against the others.
//
// So: read the "want" column against those three files
// by eye. That comparison is the actual test.
// =====================================================

static void checkLine(const char *name, long dflt, long got, long want, const char *unit) {
  Serial.printf("  %-12s %8ld -> %8ld %-4s  want %8ld   %s\n",
                name, dflt, got, unit, want,
                got == want ? "ok" : "<<< MISMATCH");
}

void testParameters() {
  // Reset and enter LoRa mode WITHOUT applying anything,
  // so the first column is what the chip powers up with.
  hardReset();
  if (regRead(REG_VERSION) != SX1278_VERSION) {
    Serial.println("  Cannot configure - run test 1 first, the radio is not");
    Serial.println("  answering. Every line below would be a guess.");
    return;
  }
  setMode(MODE_SLEEP);
  delay(10);
  setMode(MODE_STDBY);

  long dSF = getSpreadFactor(), dBW = getBandwidth(), dCR = getCodingRate();
  long dPRE = getPreamble(), dFRQ = getFrequency();
  uint8_t dSYNC = regRead(REG_SYNC_WORD);
  bool dCRC = getCrcOn();

  if (!radioBegin(gFreq)) {
    Serial.println("  Radio stopped answering mid-configure.");
    return;
  }

  Serial.printf("Modem readback (tuned to %.3f MHz):\n", gFreq / 1e6);
  Serial.println("               reset      after");
  Serial.println("              default  configure");

  long f = getFrequency();
  // The PLL is a 19-bit fraction of 32 MHz, so an exact
  // hertz match is not always representable. 1 kHz is
  // far tighter than the 250 kHz channel and far looser
  // than the rounding.
  bool fOK = labs(f - gFreq) < 1000;
  Serial.printf("  %-12s %8.3f -> %8.3f MHz   want %8.3f   %s\n",
                "frequency", dFRQ / 1e6, f / 1e6, gFreq / 1e6,
                fOK ? "ok" : "<<< MISMATCH");

  checkLine("spread fact", dSF,  getSpreadFactor(), WANT_SF,     "SF");
  checkLine("bandwidth",   dBW,  getBandwidth(),    WANT_BW,     "Hz");
  checkLine("coding rate", dCR,  getCodingRate(),   WANT_CR,     "4/x");
  checkLine("preamble",    dPRE, getPreamble(),     WANT_PREAMB, "sym");

  uint8_t sync = regRead(REG_SYNC_WORD);
  Serial.printf("  %-12s     0x%02X ->     0x%02X        want     0x%02X   %s\n",
                "sync word", dSYNC, sync, WANT_SYNC,
                sync == WANT_SYNC ? "ok" : "<<< MISMATCH");

  bool crc = getCrcOn();
  Serial.printf("  %-12s %8s -> %8s        want %8s   %s\n",
                "CRC", dCRC ? "on" : "off", crc ? "on" : "off",
                WANT_CRC ? "on" : "off", crc == WANT_CRC ? "ok" : "<<< MISMATCH");

  uint8_t op = regRead(REG_OP_MODE);
  Serial.printf("  op mode      0x%02X  (LoRa bit %s)\n",
                op, (op & MODE_LONG_RANGE) ? "SET" : "CLEAR <<< still in FSK mode");

  Serial.println();
  Serial.println("  A value that did NOT move from its reset default is a");
  Serial.println("  write the chip refused - look at MOSI (test 1) before");
  Serial.println("  anything else.");
  Serial.println("  Any MISMATCH is a rocket you will never hear, with no");
  Serial.println("  error printed anywhere. And this only checks the chip");
  Serial.println("  against THIS file - compare the want column against");
  Serial.println("  Radio.cpp on the rocket by eye.");
}


// =====================================================
// TEST 3 - LISTEN
//
// POLLED, not interrupt driven. That is deliberate: it
// works with DIO0 unwired, so it separates "the radio
// is not hearing anything" from "the radio hears fine
// and the interrupt never reaches the ESP32". Those two
// look identical in MRCC_GroundStation, which is
// interrupt driven throughout.
//
// CRC errors are counted SEPARATELY from good packets
// and that number is the useful one: a rising CRC count
// means RF is arriving and is being mangled (too far,
// too weak, interference), which is a completely
// different problem from a count that stays at zero
// while nothing arrives at all.
// =====================================================

void testListen() {
  if (!radioBegin(gFreq)) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }
  radioReceive();

  Serial.printf("Listening on %.3f MHz. Press any key to stop.\n", gFreq / 1e6);
  Serial.println("(polled - DIO0 is not used, so this works unwired)");

  unsigned long good = 0, bad = 0, hdr = 0;
  unsigned long lastReport = millis();
  int floorRssi = 0;

  while (!Serial.available()) {
    uint8_t flags = regRead(REG_IRQ_FLAGS);

    if (flags & IRQ_RX_DONE) {
      regWrite(REG_IRQ_FLAGS, IRQ_RX_DONE | IRQ_PAYLOAD_CRC_ERROR | IRQ_VALID_HEADER);

      if (flags & IRQ_PAYLOAD_CRC_ERROR) {
        bad++;
        Serial.printf("  [CRC FAIL] rssi=%d  - RF IS arriving, and is damaged\n",
                      regRead(REG_PKT_RSSI_VALUE) - 157);
      }
      else {
        good++;
        int len = regRead(REG_RX_NB_BYTES);
        int rssi = regRead(REG_PKT_RSSI_VALUE) - 157;
        float snr = (int8_t)regRead(REG_PKT_SNR_VALUE) * 0.25;

        regWrite(REG_FIFO_ADDR_PTR, regRead(REG_FIFO_RX_CURRENT_ADDR));

        // First 48 bytes only. Enough to see "MRCC,PKT="
        // and the state word; the full frame is
        // MRCC_GroundStation's job, not the doctor's.
        char preview[49];
        int n = len < 48 ? len : 48;
        for (int i = 0; i < n; i++) {
          char ch = (char)regRead(REG_FIFO);
          preview[i] = (ch >= 32 && ch < 127) ? ch : '.';
        }
        preview[n] = '\0';
        for (int i = n; i < len; i++) regRead(REG_FIFO);   // drain the rest

        Serial.printf("  [OK] len=%d rssi=%d snr=%.1f | %s%s\n",
                      len, rssi, snr, preview, len > 48 ? "..." : "");
      }
    }
    else if (flags & IRQ_VALID_HEADER) {
      hdr++;
      regWrite(REG_IRQ_FLAGS, IRQ_VALID_HEADER);
    }

    // The noise floor, every 3 s. A number around -100
    // to -120 dBm is a quiet band. Anything much higher
    // with no packets means something else is on the
    // channel and you are listening to it.
    if (millis() - lastReport > 3000) {
      lastReport = millis();
      floorRssi = regRead(REG_RSSI_VALUE) - 157;
      Serial.printf("  ... good=%lu crc_fail=%lu headers=%lu  noise floor=%d dBm\n",
                    good, bad, hdr, floorRssi);
    }
  }
  while (Serial.available()) Serial.read();

  Serial.println();
  Serial.printf("STOPPED. good=%lu  crc_fail=%lu  headers_only=%lu\n", good, bad, hdr);

  if (good == 0 && bad == 0 && hdr == 0) {
    Serial.println("  NOTHING AT ALL. In order of likelihood:");
    Serial.println("   - the rocket is off, or its radio failed to init");
    Serial.println("   - wrong channel      -> run test 6, then test 5");
    Serial.println("   - a link parameter   -> run test 2");
    Serial.println("   - antenna missing at one end (check BOTH)");
  }
  else if (good == 0 && (bad > 0 || hdr > 0)) {
    Serial.println("  RF IS ARRIVING BUT NOT DECODING. The channel and the");
    Serial.println("  modem settings are RIGHT - a wrong SF or sync word");
    Serial.println("  would give you nothing at all, not damaged frames.");
    Serial.println("  This is range, antenna, or interference.");
  }
  else {
    Serial.println("  LINK IS GOOD. If MRCC_GroundStation still prints");
    Serial.println("  nothing with this working, the fault is DIO0 - the");
    Serial.println("  real sketch is interrupt driven. Run test 4.");
  }
}


// =====================================================
// TEST 4 - DIO0 WIRING
//
// The one failure test 3 cannot see, because test 3
// deliberately does not use the pin.
//
// MRCC_GroundStation only ever prints a frame from
// inside onRx(), which the LoRa library calls from the
// DIO0 rising-edge interrupt. So a receiver with a
// perfect link and a DIO0 in the wrong hole is exactly
// as silent as one with no antenna - and the fix is
// completely different.
//
// Needs the rocket powered and transmitting.
// =====================================================

void testDio0() {
  if (!radioBegin(gFreq)) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }
  regWrite(REG_DIO_MAPPING_1, 0x00);      // DIO0 = RxDone
  radioReceive();

  Serial.printf("Watching GPIO%d for the RxDone interrupt line.\n", PIN_DIO0);
  Serial.println("The ROCKET MUST BE TRANSMITTING for this to mean anything.");
  Serial.println("Press any key to stop.");

  unsigned long packets = 0, edges = 0;
  bool prev = digitalRead(PIN_DIO0);

  while (!Serial.available()) {
    bool now = digitalRead(PIN_DIO0);
    if (now && !prev) edges++;
    prev = now;

    uint8_t flags = regRead(REG_IRQ_FLAGS);
    if (flags & IRQ_RX_DONE) {
      packets++;
      // Read the pin BEFORE clearing the flag: clearing
      // RxDone is what drops DIO0 again, so checking
      // afterwards would always read low and report a
      // dead line on a perfectly good one.
      bool high = digitalRead(PIN_DIO0);
      regWrite(REG_IRQ_FLAGS, 0xFF);
      Serial.printf("  packet %lu: IRQ set, GPIO%d reads %s\n",
                    packets, PIN_DIO0, high ? "HIGH (good)" : "LOW  (<<< suspect)");
    }
  }
  while (Serial.available()) Serial.read();

  Serial.println();
  Serial.printf("STOPPED. packets=%lu  rising edges on GPIO%d=%lu\n",
                packets, PIN_DIO0, edges);

  if (packets == 0) {
    Serial.println("  No packets arrived, so this test proved nothing about");
    Serial.println("  DIO0. Get test 3 receiving first.");
  }
  else if (edges == 0) {
    Serial.println("  PACKETS ARRIVE, DIO0 NEVER MOVES.");
    Serial.printf("  The module is not driving GPIO%d. Either the wire is\n", PIN_DIO0);
    Serial.println("  in the wrong hole, or it is not connected at all.");
    Serial.println("  This is the fault that makes a GOOD link look dead:");
    Serial.println("  MRCC_GroundStation prints only from the interrupt.");
    Serial.println("  Run test 8 to find which pin it is actually on.");
  }
  else {
    Serial.println("  DIO0 IS WIRED AND FIRING. The interrupt path is good.");
  }
}


// =====================================================
// TEST 5 - BAND SCAN
//
// Sweeps the 433 MHz ISM allocation and prints the RSSI
// at each step as a bar. A transmitting rocket shows up
// as a hump 250 kHz wide, and you can read its centre
// straight off the scale - which answers "what is that
// board ACTUALLY flashed to" without reflashing it.
//
// Also finds the other thing that produces zero good
// packets: somebody else's transmitter sitting on your
// channel.
// =====================================================

void bandScan() {
  if (!radioBegin(WANT_FREQ_A)) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }

  const long START = 433000000L;
  const long STOP  = 434800000L;
  const long STEP  =    100000L;

  Serial.println("Scanning 433.0 - 434.8 MHz (Malaysia ISM: 433.05-434.79).");
  Serial.println("Leave the rocket TRANSMITTING while this runs.");
  Serial.println();
  Serial.println("   MHz     dBm  |");

  for (long f = START; f <= STOP; f += STEP) {
    setMode(MODE_STDBY);
    setFrequency(f);
    radioReceive();
    delay(30);                       // let the AGC settle before reading

    // Peak-hold over a short window. A LoRa frame at
    // 2 Hz is not continuous, so a single sample lands
    // in the gap between packets more often than not
    // and the rocket disappears from its own scan.
    int peak = -200;
    unsigned long until = millis() + 120;
    while (millis() < until) {
      int r = regRead(REG_RSSI_VALUE) - 157;
      if (r > peak) peak = r;
    }

    // -120 dBm floor, one character per 2 dB.
    int bars = (peak + 120) / 2;
    if (bars < 0) bars = 0;
    if (bars > 50) bars = 50;

    Serial.printf("  %7.3f  %4d  |", f / 1e6, peak);
    for (int i = 0; i < bars; i++) Serial.print('#');

    if (f == WANT_FREQ_A) Serial.print("   <- rocket A");
    if (f == WANT_FREQ_B) Serial.print("   <- rocket B");
    Serial.println();
  }

  Serial.println();
  Serial.println("A hump ~250 kHz wide is a LoRa transmitter. If it is not");
  Serial.println("centred on A or B, that board is flashed to the wrong");
  Serial.println("VEHICLE - fix Config.h on the ROCKET, not here.");
  Serial.println("A raised floor everywhere is interference, not a rocket.");
}


// =====================================================
// TEST 6 - A/B TRAFFIC CHECK
//
// The fastest answer to the most common launch-day
// question: is this box on the wrong channel?
//
// Listens on A, then on B, and reports what each heard.
// Ten seconds per channel is 20 frames at the 2 Hz
// downlink, which is plenty to tell traffic from
// silence without anyone standing around.
// =====================================================

static unsigned long listenFor(long hz, unsigned long ms, int *bestRssi) {
  radioBegin(hz);
  radioReceive();

  unsigned long good = 0;
  *bestRssi = -200;
  unsigned long until = millis() + ms;

  while (millis() < until) {
    uint8_t flags = regRead(REG_IRQ_FLAGS);
    if (flags & IRQ_RX_DONE) {
      if (!(flags & IRQ_PAYLOAD_CRC_ERROR)) {
        good++;
        int r = regRead(REG_PKT_RSSI_VALUE) - 157;
        if (r > *bestRssi) *bestRssi = r;
      }
      regWrite(REG_IRQ_FLAGS, 0xFF);
    }
  }
  return good;
}

void channelCheck() {
  if (!radioBegin(WANT_FREQ_A)) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }

  Serial.println("Checking both channels, 10 s each. Rocket must be ON.");

  int rssiA = 0, rssiB = 0;
  Serial.printf("  listening on A (%.3f MHz)...\n", WANT_FREQ_A / 1e6);
  unsigned long a = listenFor(WANT_FREQ_A, 10000, &rssiA);
  Serial.printf("  listening on B (%.3f MHz)...\n", WANT_FREQ_B / 1e6);
  unsigned long b = listenFor(WANT_FREQ_B, 10000, &rssiB);

  Serial.println();
  Serial.printf("  A: %lu packets", a);
  if (a) Serial.printf("  best rssi %d dBm", rssiA);
  Serial.println();
  Serial.printf("  B: %lu packets", b);
  if (b) Serial.printf("  best rssi %d dBm", rssiB);
  Serial.println();
  Serial.println();

  if (a == 0 && b == 0) {
    Serial.println("  NEITHER channel has traffic. The rocket is off, its");
    Serial.println("  radio failed to init, or it is on a third frequency -");
    Serial.println("  run test 5 to look at the whole band.");
  }
  else if (a && b) {
    Serial.println("  BOTH channels have traffic. Either both rockets are");
    Serial.println("  powered, or something else is on the band. Do not fly");
    Serial.println("  until you know which - the loss counter cannot tell");
    Serial.println("  two transmitters apart.");
  }
  else {
    const char *live = a ? "A" : "B";
    Serial.printf("  ROCKET IS ON CHANNEL %s.\n", live);
    Serial.printf("  Set MRCC_GroundStation to %s (press %s on its serial\n", live, live);
    Serial.println("  monitor - no reflash needed; the choice survives a reboot).");
  }
}


// =====================================================
// TEST 7 - TX BEACON
//
// Turns the receiver into a transmitter, so a second
// ground station - or this one after you swap the
// suspect module into it - can be tested without
// waiting for a rocket.
//
// The payload deliberately contains NO "MRCC" substring.
// mrcc.py keys on that word, so a beacon that carried it
// would be half-parsed into the flight record as a
// corrupt frame. Without it the line is ignored as
// chatter, which is the correct outcome.
// =====================================================

void txBeacon() {
  if (!radioBegin(gFreq)) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }

  Serial.println();
  Serial.println("  *** THIS TRANSMITS. It will sit on the channel and");
  Serial.printf("  *** collide with a rocket flying on %.3f MHz.\n", gFreq / 1e6);
  Serial.println("  *** Bench use only. Press any key to stop.");
  Serial.println();

  // ~17 dBm on PA_BOOST, which is where these modules
  // wire the antenna. RFO would transmit into nothing.
  regWrite(REG_PA_CONFIG, 0x80 | 0x0F);
  regWrite(REG_PA_DAC, 0x84);

  unsigned long n = 0;
  while (!Serial.available()) {
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "GSDOCTOR,BEACON=%lu,T=%lu", n, millis() / 1000);

    setMode(MODE_STDBY);
    regWrite(REG_IRQ_FLAGS, 0xFF);
    regWrite(REG_FIFO_ADDR_PTR, 0);
    for (int i = 0; i < len; i++) regWrite(REG_FIFO, msg[i]);
    regWrite(REG_PAYLOAD_LENGTH, len);
    setMode(MODE_TX);

    // Bounded wait. A TxDone that never arrives is
    // itself the diagnosis - on a module with no
    // antenna the PA can trip and the flag never sets.
    unsigned long until = millis() + 2000;
    bool done = false;
    while (millis() < until) {
      if (regRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) { done = true; break; }
    }
    regWrite(REG_IRQ_FLAGS, 0xFF);

    Serial.printf("  sent #%lu (%d bytes) %s\n", n, len,
                  done ? "TxDone" : "<<< NO TxDone - check the antenna");
    n++;

    unsigned long gap = millis() + 1000;
    while (millis() < gap && !Serial.available()) { }
  }
  while (Serial.available()) Serial.read();

  setMode(MODE_STDBY);
  Serial.printf("STOPPED after %lu beacons. Radio back in standby.\n", n);
}


// =====================================================
// TEST 8 - SPI PIN SCAN
//
// SD_Doctor permutes every pin against every other. That
// is 4 pins from 20 candidates - about 116,000 orderings
// - which is fine when you are looking for a module you
// cannot find at all.
//
// This scan is deliberately narrower, because the fault
// it is built for is different: the wiring is mostly
// right and ONE wire is in the wrong hole. So it holds
// three pins at your configured values and sweeps the
// fourth, four times over. 4 x ~20 tries finishes in
// seconds and finds a single misplaced jumper, which is
// what actually happens on a bench.
//
// If all four sweeps come up empty, the module is not
// answering on ANY single-pin change: it is unpowered,
// dead, or two wires are wrong. Fix the power first.
// =====================================================

static bool probeAt(int sck, int miso, int mosi, int ss) {
  radioSPI.end();
  pinMode(ss, OUTPUT);
  digitalWrite(ss, HIGH);
  radioSPI.begin(sck, miso, mosi, ss);

  int savedSS = PIN_SS;
  PIN_SS = ss;                       // regRead() drives PIN_SS
  hardReset();
  uint8_t v = regRead(REG_VERSION);
  PIN_SS = savedSS;

  return v == SX1278_VERSION;
}

// A candidate that is already doing another job in this
// pinout is not a candidate. Trying SCK on the RST pin
// makes hardReset() and the clock fight over one line,
// and whatever comes back is noise, not evidence.
static bool pinIsFree(int p, int which) {
  if (p == PIN_RST || p == PIN_DIO0) return false;
  if (which != 0 && p == PIN_SCK)  return false;
  if (which != 1 && p == PIN_MISO) return false;
  if (which != 2 && p == PIN_MOSI) return false;
  if (which != 3 && p == PIN_SS)   return false;
  return true;
}

static void sweepOne(const char *label, int which) {
  int sck = PIN_SCK, miso = PIN_MISO, mosi = PIN_MOSI, ss = PIN_SS;

  Serial.printf("  sweeping %s ...\n", label);

  // MISO may live on an input-only pin; the other three
  // may not. Scanning them there would report a pinout
  // you cannot wire.
  for (int i = 0; i < SAFE_PIN_COUNT; i++) {
    int p = SAFE_PINS[i];
    if (!pinIsFree(p, which)) continue;

    if (which == 0) sck = p; else if (which == 1) miso = p;
    else if (which == 2) mosi = p; else ss = p;

    if (probeAt(sck, miso, mosi, ss)) {
      Serial.printf("    FOUND: %s = %d  (you have %d)\n", label, p,
                    which == 0 ? PIN_SCK : which == 1 ? PIN_MISO :
                    which == 2 ? PIN_MOSI : PIN_SS);
    }
  }

  if (which == 1) {
    sck = PIN_SCK; mosi = PIN_MOSI; ss = PIN_SS;
    for (int i = 0; i < INPUT_ONLY_COUNT; i++) {
      miso = INPUT_ONLY_PINS[i];
      if (probeAt(sck, miso, mosi, ss)) {
        Serial.printf("    FOUND: MISO = %d  (input-only pin, legal for MISO)\n", miso);
      }
    }
  }
}

void spiPinScan() {
  Serial.println();
  Serial.println("Sweeping one pin at a time, the other three held at your");
  Serial.println("configured values. Looking for REG_VERSION == 0x12.");
  Serial.println();

  sweepOne("SCK",  0);
  sweepOne("MISO", 1);
  sweepOne("MOSI", 2);
  sweepOne("SS",   3);

  // Put the bus back the way the rest of the sketch
  // expects it, or every later test runs on whatever
  // the last probe happened to leave configured.
  radioSPI.end();
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);

  Serial.println();
  Serial.println("Nothing found above means no SINGLE pin change makes the");
  Serial.println("module answer. Check 3V3 and GND, then look for TWO wrong");
  Serial.println("wires - this scan cannot see those.");
  Serial.println("Note RST and DIO0 are NOT scanned here: RST is exercised by");
  Serial.println("every probe, and DIO0 has its own test (4).");
}


// =====================================================
// N - STORED CHANNEL
//
// MRCC_GroundStation remembers the operator's last A/B
// choice in NVS, on purpose: attaching the backend
// resets this board, and without persistence every
// reconnect would silently drag it back to the
// compile-time default.
//
// The cost of that is a box that can boot onto a channel
// nobody in the room chose. This prints what is stored
// so the question is answerable without reading flash
// by hand.
// =====================================================

void showNvs() {
  gPrefs.begin(NVS_NAMESPACE, true);        // read-only
  int saved = gPrefs.getInt(NVS_KEY_CH, -1);
  gPrefs.end();

  Serial.println();
  if (saved < 0) {
    Serial.println("  NVS: nothing stored. MRCC_GroundStation will boot on its");
    Serial.println("  compile-time VEHICLE default.");
  }
  else {
    Serial.printf("  NVS: stored channel = %d (%s, %.3f MHz)\n", saved,
                  saved == 0 ? "A" : "B",
                  (saved == 0 ? WANT_FREQ_A : WANT_FREQ_B) / 1e6);
    Serial.println("  MRCC_GroundStation will boot on THIS, not on the");
    Serial.println("  #define. If that is a surprise, it is the answer to");
    Serial.println("  'why is this box on the wrong rocket'.");
  }
  Serial.println("  This sketch never writes it - use A/B on the real sketch.");
}
