#include <SPI.h>
#include <Wire.h>
#include "esp_system.h"

// =====================================================
// TX DOCTOR
// Standalone fault finder for the FLIGHT COMPUTER
// (ESP32-S3: SX1278 transmitter, ICM-20948, BMP280, GPS)
//
// The third doctor in this repo:
//
//   SD_Doctor  - the card, on this same board
//   GS_Doctor  - the radio, on the RECEIVER board
//   TX_Doctor  - the radio and the sensors, on THIS one
//
// WHY THE TRANSMITTER NEEDS ITS OWN. GS_Doctor can only
// tell you what does or does not arrive. Every fault on
// this end that it cannot see lives here:
//
//   - the modem is configured wrong, so it transmits
//     perfectly on settings nothing is listening for
//   - DIO0 is not wired, so Radio.cpp never gets TxDone
//     and silently falls back to its air-time timeout.
//     Telemetry still flows. Nothing reports it.
//   - the supply collapses mid-transmission, which is a
//     BROWNOUT and looks like a crash
//   - the IMU or the barometer never came up, and the
//     one line saying so scrolled past 400 lines ago
//
// The headline test is 3. Air time is a FINGERPRINT of
// the modem settings, so it verifies them with no
// receiver, no second person, and no antenna range:
//
//     PL=237  SF7  187 ms      <- the contract
//             SF8  328 ms
//             SF9  584 ms      <- longer than SEND_INTERVAL
//
// A board transmitting on the wrong spreading factor
// cannot hide from a stopwatch.
//
// Upload it, open Serial Monitor at 115200, press the
// number keys.
//
//   1 = Radio present?     (SPI handshake, both directions)
//   2 = Link parameters    (modem readback vs the contract)
//   3 = Transmit           (air time, duty cycle, TxDone)
//   4 = DIO0 TxDone        (the interrupt Radio.cpp needs)
//   5 = Power sweep        (find the brownout)
//   6 = Listen             (hear GS_Doctor's beacon)
//   7 = I2C bus scan       (who is actually on the bus)
//   8 = Sensors            (IMU rate + WHICH AXIS IS UP, baro)
//   9 = GPS                (raw NMEA, with baud hunt)
//   S = SPI pin scan       (one wrong wire, found)
//   P = pins   R = last reset reason   M = menu
//
// PYRO IS NEVER TOUCHED. The gate pin is driven LOW at
// boot and left there - see setup(). This sketch has no
// fire path at all, deliberately: a diagnostic you have
// to think twice about before flashing is one you will
// not flash on the pad.
//
// Board: ESP32-S3. Same board as SD_Doctor, and the SD
// bus is a different set of pins, so neither sketch can
// disturb the other's wiring.
// =====================================================


// =====================================================
// YOUR PINS - must match MRCC_FlightComputer/src/Config.h
// =====================================================

int PIN_SCK  = 12;
int PIN_MISO = 13;
int PIN_MOSI = 11;
int PIN_SS   = 10;
int PIN_RST  =  9;
int PIN_DIO0 =  8;

#define PIN_I2C_SDA   4
#define PIN_I2C_SCL   5

#define PIN_GPS_RX   18        // ESP32 <- GPS TX
#define PIN_GPS_TX   17

// Driven LOW at boot and never again. Active HIGH fires
// the charge (Pyro.cpp), so LOW is safe, and an output
// held low cannot be pulled up by a floating gate.
#define PIN_PYRO_GATE 2


// =====================================================
// THE CONTRACT
//
// Must equal MRCC_FlightComputer/src/Radio.cpp's
// initRadio() AND MRCC_GroundStation.ino's LORA_ block.
// Three copies, no compiler checking any against the
// others - see test 2.
// =====================================================

const long  WANT_FREQ_A  = 433300000L;   // vehicle A
const long  WANT_FREQ_B  = 434100000L;   // vehicle B
const int   WANT_SF      = 7;
const long  WANT_BW      = 250000L;
const int   WANT_CR      = 5;            // 4/5
const int   WANT_PREAMB  = 8;
const uint8_t WANT_SYNC  = 0x12;
const bool  WANT_CRC     = true;
const int   WANT_TXPOWER = 17;           // TX_POWER_DEFAULT

// From Config.h. Test 3 checks the real air time against
// these, because two copies that no longer fit inside
// SEND_INTERVAL is a link that eats itself.
const unsigned long SEND_INTERVAL = 500;
const unsigned long COPY_GAP      =  60;
const unsigned long TX_MAX_AIR    = 300;

// Worst-case packet from buildTelemetryPacket() with the
// health block on the end. Test 3 sends this length so
// the measured air time is the flight's air time.
const int TEST_PAYLOAD = 237;

long gFreq = WANT_FREQ_A;


// =====================================================
// SX1278 REGISTERS (datasheet rev 7, LoRa mode table)
// =====================================================

#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_OCP                  0x0B
#define REG_LNA                  0x0C
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
#define IRQ_VALID_HEADER         0x10
#define IRQ_PAYLOAD_CRC_ERROR    0x20
#define IRQ_RX_DONE              0x40

#define SX1278_VERSION           0x12
const long XTAL_HZ = 32000000L;

SPIClass  &radioSPI = SPI;

// 8 MHz is what Radio.cpp and the LoRa library use, so the doctor
// starts there - a fault that only appears at the flight speed is a
// fault worth reproducing. testPresence() drops it when that fails,
// because a module that answers at 500 kHz and not at 8 MHz is not a
// wiring fault, it is a signal-integrity one, and the fix is
// different (shorter leads, a ground return next to the clock).
long        gSpiHz = 8000000;
SPISettings radioSettings(8000000, MSBFIRST, SPI_MODE0);

static void setSpiHz(long hz) {
  gSpiHz = hz;
  radioSettings = SPISettings(hz, MSBFIRST, SPI_MODE0);
}


// =====================================================
// I2C DEVICES
// =====================================================

#define ICM_ADDR_AD0_LOW   0x68     // AD0_VAL 0 in Config.h
#define ICM_ADDR_AD0_HIGH  0x69
#define ICM_REG_WHOAMI     0x00
#define ICM_REG_PWR_MGMT_1 0x06
#define ICM_REG_INT_STATUS 0x1A
#define ICM_REG_ACCEL_XOUT 0x2D
#define ICM_WHOAMI_VALUE   0xEA

#define BMP_ADDR_PRIMARY   0x76
#define BMP_ADDR_FALLBACK  0x77
#define BMP_REG_ID         0xD0
#define BMP_REG_CTRL_MEAS  0xF4
#define BMP_REG_PRESS_MSB  0xF7
#define BMP_REG_CALIB      0x88


// =====================================================
// BROWNOUT TRAP
//
// The power sweep cannot measure supply sag - the VBAT
// divider is not wired (VBAT_ENABLED 0). But it does not
// need to, because the SYMPTOM of a collapsing supply on
// an ESP32 is the board resetting, and the reset reason
// survives the reset in RTC memory.
//
// So: stamp the power level into RTC memory before each
// transmission, and read it back at boot. If the board
// comes up with ESP_RST_BROWNOUT and a level stamped,
// that level is where the supply gave out. This is the
// known failure on this airframe - Health.cpp counts
// brownouts for exactly this reason - and it is the one
// test here that diagnoses itself by crashing.
// =====================================================

RTC_DATA_ATTR int  rtcSweepLevel = -1;
RTC_DATA_ATTR bool rtcSweeping   = false;


void printPins();
void printMenu();
void printResetReason();
void testPresence();
void testParameters();
void testTransmit();
void testTxDone();
void powerSweep();
void testListen();
void i2cScan();
void testSensors();
void testGps();
void spiPinScan();


// =====================================================
// RAW REGISTER ACCESS
// =====================================================

static uint8_t regRead(uint8_t addr) {
  radioSPI.beginTransaction(radioSettings);
  digitalWrite(PIN_SS, LOW);
  radioSPI.transfer(addr & 0x7F);
  uint8_t v = radioSPI.transfer(0x00);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.endTransaction();
  return v;
}

static void regWrite(uint8_t addr, uint8_t value) {
  radioSPI.beginTransaction(radioSettings);
  digitalWrite(PIN_SS, LOW);
  radioSPI.transfer(addr | 0x80);
  radioSPI.transfer(value);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.endTransaction();
}

// =====================================================
// BIT-BANGED REGISTER READ
//
// No SPI peripheral, no GPIO matrix routing, ~125 kHz.
// SD_Doctor carries the same idea for the same reason: if
// this works and the hardware SPI does not, the wiring is
// fine and the fault is the peripheral, the pin mapping
// or the clock rate. If NEITHER works, it is the wiring
// and no amount of driver fiddling will help.
// =====================================================

static uint8_t bitBangRead(uint8_t addr) {
  pinMode(PIN_SCK,  OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_SS,   OUTPUT);

  digitalWrite(PIN_SCK, LOW);          // SPI mode 0 idles low
  digitalWrite(PIN_SS,  HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SS,  LOW);
  delayMicroseconds(5);

  uint8_t a = addr & 0x7F;             // bit 7 clear = read
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (a >> i) & 1);   // MOSI changes while SCK low
    delayMicroseconds(4);
    digitalWrite(PIN_SCK, HIGH);            // slave samples on the rise
    delayMicroseconds(4);
    digitalWrite(PIN_SCK, LOW);
  }

  uint8_t v = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(4);
    v = (v << 1) | (digitalRead(PIN_MISO) & 1);   // master samples on the rise
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(4);
  }

  digitalWrite(PIN_SS, HIGH);
  return v;
}


static void hardReset() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(10);
  digitalWrite(PIN_RST, HIGH);
  delay(20);
}

static void setMode(uint8_t mode) { regWrite(REG_OP_MODE, MODE_LONG_RANGE | mode); }

static void setFrequency(long hz) {
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

static const long BW_TABLE[] = {
  7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000
};

static long getBandwidth() {
  uint8_t idx = regRead(REG_MODEM_CONFIG_1) >> 4;
  return idx > 9 ? -1 : BW_TABLE[idx];
}
static int  getCodingRate()   { return ((regRead(REG_MODEM_CONFIG_1) >> 1) & 0x07) + 4; }
static int  getSpreadFactor() { return regRead(REG_MODEM_CONFIG_2) >> 4; }
static bool getCrcOn()        { return (regRead(REG_MODEM_CONFIG_2) & 0x04) != 0; }
static int  getPreamble()     { return (regRead(REG_PREAMBLE_MSB) << 8) | regRead(REG_PREAMBLE_LSB); }


// =====================================================
// TX POWER
//
// PA_BOOST only. These modules wire the antenna to that
// pin, so transmitting on RFO puts the energy nowhere -
// which reads on the ground as a range problem and is
// actually a register.
// =====================================================

static void setTxPower(int level) {
  if (level < 2)  level = 2;
  if (level > 20) level = 20;

  if (level == 20) {
    // +20 dBm needs the PA_DAC boost AND the overcurrent
    // limit lifted, or the PA trips and TxDone never
    // comes. That trip is a diagnosis, not a bug - see
    // the power sweep.
    regWrite(REG_PA_DAC, 0x87);
    regWrite(REG_OCP, 0x20 | 17);        // ~140 mA
    regWrite(REG_PA_CONFIG, 0x80 | 15);
  }
  else {
    regWrite(REG_PA_DAC, 0x84);
    regWrite(REG_OCP, 0x20 | 11);        // ~100 mA
    regWrite(REG_PA_CONFIG, 0x80 | (level - 2));
  }
}

static bool radioBegin(long hz) {
  hardReset();
  if (regRead(REG_VERSION) != SX1278_VERSION) return false;

  setMode(MODE_SLEEP);
  delay(10);
  setFrequency(hz);

  regWrite(REG_FIFO_TX_BASE_ADDR, 0);
  regWrite(REG_FIFO_RX_BASE_ADDR, 0);
  regWrite(REG_LNA, regRead(REG_LNA) | 0x03);

  uint8_t bwIdx = 8;
  for (int i = 0; i < 10; i++) if (BW_TABLE[i] == WANT_BW) bwIdx = i;
  regWrite(REG_MODEM_CONFIG_1, (bwIdx << 4) | ((WANT_CR - 4) << 1));
  regWrite(REG_MODEM_CONFIG_2, (WANT_SF << 4) | (WANT_CRC ? 0x04 : 0x00));
  regWrite(REG_MODEM_CONFIG_3, 0x04);

  regWrite(REG_PREAMBLE_MSB, WANT_PREAMB >> 8);
  regWrite(REG_PREAMBLE_LSB, WANT_PREAMB & 0xFF);
  regWrite(REG_SYNC_WORD, WANT_SYNC);

  setTxPower(WANT_TXPOWER);
  setMode(MODE_STDBY);
  return true;
}


// =====================================================
// AIR TIME, FROM THE DATASHEET
//
// Semtech's formula, so the doctor has an expectation to
// hold the stopwatch against rather than just a number.
// A measurement with nothing to compare it to cannot
// tell you the modem is misconfigured; this can.
// =====================================================

static float airTimeMs(int payload, int sf, long bw, int crDenom,
                       int preamble, bool crc) {
  float tsym = (float)(1L << sf) * 1000.0 / (float)bw;      // ms
  float tpre = (preamble + 4.25) * tsym;

  // LowDataRateOptimize is mandatory when a symbol is
  // longer than 16 ms. Off at SF7/250k, but the term has
  // to be here or the SF11/SF12 estimate is wrong.
  int de = (tsym > 16.0) ? 1 : 0;

  int num = 8 * payload - 4 * sf + 28 + (crc ? 16 : 0);
  int den = 4 * (sf - 2 * de);
  int n = (num + den - 1) / den;                 // ceil
  if (n < 0) n = 0;
  int symbols = 8 + n * crDenom;

  return tpre + symbols * tsym;
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  // FIRST, before anything else can float it. Pyro.cpp
  // fires on HIGH; this sketch never raises it.
  pinMode(PIN_PYRO_GATE, OUTPUT);
  digitalWrite(PIN_PYRO_GATE, LOW);

  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TX DOCTOR");
  Serial.println(" ESP32-S3 flight computer fault finder");
  Serial.println("========================================");
  Serial.println(" PYRO GATE HELD LOW. No fire path here.");

  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  pinMode(PIN_DIO0, INPUT);
  radioSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  printResetReason();
  printPins();
  printMenu();

  Serial.println();
  Serial.println("### AUTO TEST 1: RADIO PRESENT? ###");
  testPresence();

  Serial.println();
  Serial.println("### AUTO TEST 7: I2C BUS SCAN ###");
  i2cScan();

  Serial.println();
  Serial.println("Press a number key. 3 is the one that catches a");
  Serial.println("misconfigured modem without a receiver.");
  Serial.println();
}


void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if      (c == '1') testPresence();
    else if (c == '2') testParameters();
    else if (c == '3') testTransmit();
    else if (c == '4') testTxDone();
    else if (c == '5') powerSweep();
    else if (c == '6') testListen();
    else if (c == '7') i2cScan();
    else if (c == '8') testSensors();
    else if (c == '9') testGps();
    else if (c == 's' || c == 'S') spiPinScan();
    else if (c == 'p' || c == 'P') printPins();
    else if (c == 'r' || c == 'R') printResetReason();
    else if (c == 'm' || c == 'M') printMenu();
  }
}


void printPins() {
  Serial.println();
  Serial.println("--- PINS (edit at the top of this file) ---");
  Serial.printf("  LoRa  SCK=%d MISO=%d MOSI=%d SS=%d RST=%d DIO0=%d\n",
                PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS, PIN_RST, PIN_DIO0);
  Serial.printf("  I2C   SDA=%d SCL=%d\n", PIN_I2C_SDA, PIN_I2C_SCL);
  Serial.printf("  GPS   RX=%d TX=%d @ 9600\n", PIN_GPS_RX, PIN_GPS_TX);
  Serial.printf("  PYRO  gate=%d  (held LOW, never raised)\n", PIN_PYRO_GATE);
  Serial.println("  These must match MRCC_FlightComputer/src/Config.h.");
  Serial.println("  SD is on 14/15/16/7 - a different bus. See SD_Doctor.");
}


void printMenu() {
  Serial.println();
  Serial.println("--- TESTS ---");
  Serial.println("  1 = Radio present?   SPI handshake, both directions");
  Serial.println("  2 = Link parameters  modem readback vs the contract");
  Serial.println("  3 = Transmit         AIR TIME, duty cycle, TxDone");
  Serial.println("  4 = DIO0 TxDone      the interrupt Radio.cpp needs");
  Serial.println("  5 = Power sweep      find the brownout");
  Serial.println("  6 = Listen           hear GS_Doctor's beacon (test 7 there)");
  Serial.println("  7 = I2C bus scan     who is actually on the bus");
  Serial.println("  8 = Sensors          IMU rate + WHICH AXIS IS UP, baro");
  Serial.println("  9 = GPS              raw NMEA, with baud hunt");
  Serial.println("  S = SPI pin scan     one wrong wire, found");
  Serial.println("  P = pins   R = reset reason   M = this menu");
}


// =====================================================
// R - RESET REASON
//
// Mirrors Health.cpp's reportResetReason(), and exists
// here for the same reason: on this airframe a reset is
// usually the SUPPLY, not the code, and the two are
// indistinguishable unless you ask.
// =====================================================

void printResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *name = "UNKNOWN";
  switch (r) {
    case ESP_RST_POWERON:  name = "POWER ON (clean start)";   break;
    case ESP_RST_EXT:      name = "EXTERNAL RESET BUTTON";    break;
    case ESP_RST_SW:       name = "SOFTWARE RESET";           break;
    case ESP_RST_PANIC:    name = "CRASH / PANIC";            break;
    case ESP_RST_INT_WDT:  name = "INTERRUPT WATCHDOG";       break;
    case ESP_RST_TASK_WDT: name = "TASK WATCHDOG";            break;
    case ESP_RST_WDT:      name = "WATCHDOG";                 break;
    case ESP_RST_BROWNOUT: name = "BROWNOUT - SUPPLY DIPPED"; break;
    default: break;
  }
  Serial.println();
  Serial.printf("  Last reset: %s\n", name);

  if (r == ESP_RST_BROWNOUT && rtcSweeping) {
    Serial.println();
    Serial.println("  *** THE POWER SWEEP BROWNED THIS BOARD OUT ***");
    Serial.printf("  *** It died transmitting at %d dBm.\n", rtcSweepLevel);
    Serial.println("  *** That is your usable ceiling on this supply.");
    Serial.println("  *** Under vibration it will be LOWER than this.");
    Serial.println("  *** This is a power connection, not the radio.");
    rtcSweeping = false;
  }
  else if (r == ESP_RST_BROWNOUT) {
    Serial.println("  The supply collapsed. On this airframe that is a");
    Serial.println("  power connection breaking contact, not an SD or");
    Serial.println("  code fault. Run test 5.");
  }
}


// =====================================================
// TEST 1 - RADIO PRESENT?
//
// Read path and write path separately: a dead MOSI
// passes a read test perfectly, so the module answers,
// you conclude the wiring is fine, and then nothing you
// configure ever takes.
// =====================================================

// Names the chip behind a REG_VERSION byte. 0x12 is the
// SX1276/77/78/79 family and the RFM95/96/98 modules built
// on them; 0x22 is an SX1272, which is a DIFFERENT part on
// a different band and will never hear a 433 MHz rocket.
static const char *versionMeaning(uint8_t v) {
  if (v == 0x12) return "SX1276/77/78/79 or RFM9x - correct family";
  if (v == 0x22) return "SX1272 <<< WRONG PART. 868/915 MHz, not 433.";
  return NULL;
}

// Before clocking anything, look at MISO as a plain wire.
// A line that reads the same with SS high and SS low is not
// being driven by anything, and no amount of SPI will make
// it talk. This separates "nothing is connected" from "the
// transfer is going wrong", which the version byte alone
// cannot do.
// Is a line FLOATING, or is something HOLDING it?
//
// The internal pull is ~45 kOhm, so anything actually
// driving the pin - a short to a rail, or a powered chip -
// wins against it, and a disconnected pin follows it. That
// one distinction is the difference between "the wire is
// not there" and "the wire is there and tied to ground",
// which read identically to every SPI test in this sketch
// and need completely different fixes.
//
//   pull-up 1, pull-down 0  ->  FLOATING: nothing connected
//   pull-up 0, pull-down 0  ->  HELD LOW by something
//   pull-up 1, pull-down 1  ->  HELD HIGH by something
static const char *lineState(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(500);
  int up = digitalRead(pin);

  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(500);
  int down = digitalRead(pin);

  pinMode(pin, INPUT);

  if (up && !down) return "floating  (nothing is connected to this pin)";
  if (!up && !down) return "HELD LOW  (shorted to GND, or an unpowered chip)";
  if (up && down)   return "HELD HIGH (shorted to 3V3)";
  return "inverted?? (pull-up reads 0, pull-down reads 1 - impossible)";
}

static void reportStaticLines() {
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_SS, OUTPUT);

  digitalWrite(PIN_SS, HIGH);
  delayMicroseconds(50);
  int misoIdle = digitalRead(PIN_MISO);

  digitalWrite(PIN_SS, LOW);
  delayMicroseconds(50);
  int misoSel = digitalRead(PIN_MISO);
  digitalWrite(PIN_SS, HIGH);

  Serial.printf("  MISO with SS high = %d, with SS low = %d\n", misoIdle, misoSel);
  if (misoIdle == misoSel) {
    Serial.println("  (MISO did not respond to chip select)");
  }

  // The pull test, which is what actually locates it. MISO
  // is the line that carries the answer, so it is the one
  // that matters - but SCK/MOSI are printed too because a
  // short between two of them shows up here as a pair.
  Serial.println();
  Serial.println("  Line states (internal pull-up vs pull-down):");
  Serial.printf("    MISO GPIO%-2d  %s\n", PIN_MISO, lineState(PIN_MISO));
  Serial.printf("    SCK  GPIO%-2d  %s\n", PIN_SCK,  lineState(PIN_SCK));
  Serial.printf("    MOSI GPIO%-2d  %s\n", PIN_MOSI, lineState(PIN_MOSI));
  Serial.printf("    DIO0 GPIO%-2d  %s\n", PIN_DIO0, lineState(PIN_DIO0));
  Serial.println();
  Serial.println("  A module that is POWERED and connected holds MISO one way");
  Serial.println("  or the other. FLOATING means the wire is not arriving.");
  Serial.println("  HELD LOW with no power at the chip is the same picture an");
  Serial.println("  unpowered SX1278 makes: its protection diodes clamp the");
  Serial.println("  line. Check the module's own 3V3 pad before its MISO pad.");

  // Restore what the SPI bus expects.
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
}

void testPresence() {
  Serial.println("Resetting the module...");
  setSpiHz(8000000);
  hardReset();

  reportStaticLines();

  uint8_t v = regRead(REG_VERSION);
  Serial.printf("  REG_VERSION (0x42) = 0x%02X   (expect 0x%02X) @ %ld kHz\n",
                v, SX1278_VERSION, gSpiHz / 1000);

  const char *known = versionMeaning(v);
  if (known) Serial.printf("  -> %s\n", known);

  if (v != SX1278_VERSION) {
    // ---- is it the clock rate? ----
    // Long jumper leads and a breadboard do not carry 8 MHz.
    // The module is fine, the wiring is fine for the flight
    // build's own purposes, and only this speed is wrong -
    // a distinction worth two seconds of sweeping.
    Serial.println();
    Serial.println("  Retrying slower, in case this is signal integrity...");
    const long speeds[] = { 4000000, 2000000, 1000000, 500000, 100000 };
    uint8_t got = 0;
    long worked = 0;

    for (int i = 0; i < 5; i++) {
      setSpiHz(speeds[i]);
      hardReset();
      got = regRead(REG_VERSION);
      Serial.printf("    %4ld kHz -> 0x%02X\n", speeds[i] / 1000, got);
      if (got == SX1278_VERSION) { worked = speeds[i]; break; }
    }

    if (worked) {
      Serial.println();
      Serial.printf("  *** ANSWERS AT %ld kHz, NOT AT 8 MHz. ***\n", worked / 1000);
      Serial.println("  The module and the wiring are fine. The BUS is not");
      Serial.println("  carrying 8 MHz - long jumpers, a breadboard, or no");
      Serial.println("  ground return alongside the clock. Radio.cpp and the");
      Serial.println("  LoRa library both run 8 MHz, so the flight build will");
      Serial.println("  fail where this just succeeded. Shorten the leads.");
      return;
    }

    // ---- is it the SPI peripheral? ----
    Serial.println();
    Serial.println("  Retrying with the SPI peripheral bypassed entirely");
    Serial.println("  (bit-banged, ~125 kHz)...");
    radioSPI.end();
    hardReset();
    uint8_t bb = bitBangRead(REG_VERSION);
    Serial.printf("    bit-bang -> 0x%02X\n", bb);
    radioSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
    setSpiHz(8000000);

    if (bb == SX1278_VERSION) {
      Serial.println();
      Serial.println("  *** THE WIRING IS GOOD. The SPI peripheral is not. ***");
      Serial.println("  Bit-banging the same pins reads the module correctly,");
      Serial.println("  so every wire is where this sketch thinks it is.");
      Serial.println("  Suspect a pin that cannot be routed, or another");
      Serial.println("  driver holding the bus. This one is a software fault.");
      return;
    }

    // ---- neither. It is the wiring or the part. ----
    Serial.println();
    if (v == 0x00 && bb == 0x00) {
      Serial.println("  FAIL - 0x00 every way. MISO is held LOW by something,");
      Serial.println("         or the module has no power AT THE CHIP.");
      Serial.println("         A multimeter on the header pin proves the wire");
      Serial.println("         has voltage, NOT that the regulator on the");
      Serial.println("         module is passing it. Measure between the");
      Serial.println("         module's own 3V3 and GND pads.");
      Serial.println("         Then check GND is actually shared with the S3.");
    }
    else if (v == 0xFF && bb == 0xFF) {
      Serial.println("  FAIL - 0xFF every way. MISO is floating: nothing is");
      Serial.println("         driving it at all. In order of likelihood:");
      Serial.printf("           - MISO is not on GPIO%d\n", PIN_MISO);
      Serial.printf("           - SS is not on GPIO%d, so the module is never\n", PIN_SS);
      Serial.println("             selected and never answers");
      Serial.println("           - a broken or unseated jumper");
      Serial.println("         Run the pin scan (S) - it finds one wrong hole.");
    }
    else {
      Serial.printf("  FAIL - reads 0x%02X (bit-bang 0x%02X). Not a value any\n", v, bb);
      Serial.println("         SX127x returns. Something is answering out of");
      Serial.println("         step: SCK and MOSI swapped shifts every bit,");
      Serial.println("         and a shared bus with a second chip selected");
      Serial.println("         does the same. Run the pin scan (S).");
    }
    Serial.println();
    Serial.println("  Whatever the cause, it is BEFORE the radio: nothing");
    Serial.println("  else in this sketch can mean anything until this reads");
    Serial.printf("  0x%02X.\n", SX1278_VERSION);
    return;
  }

  Serial.println("  PASS - read path works (SCK + MISO + SS).");

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
    Serial.println("  FAIL - answers but refuses writes. MOSI is the only");
    Serial.println("         line this adds. Everything else in this sketch");
    Serial.println("         will 'work' and configure nothing.");
    return;
  }
  Serial.println("  PASS - write path works (MOSI).");
  Serial.println("  RADIO IS PRESENT AND ADDRESSABLE.");
}


// =====================================================
// TEST 2 - LINK PARAMETERS
//
// Same caveat as GS_Doctor's test 2, and it matters more
// here: this proves the CHIP accepts and holds the
// contract, not that Radio.cpp writes the same values.
// The WANT_ block above, initRadio() in Radio.cpp, and
// the LORA_ defines in MRCC_GroundStation.ino are three
// copies of one contract with nothing checking them
// against each other. Read the want column against those
// two files by eye - that comparison is the real test.
//
// The reset-default column is what makes a REFUSED write
// visible: a value that never moved is not a value that
// happens to match.
// =====================================================

static void checkLine(const char *name, long dflt, long got, long want, const char *unit) {
  Serial.printf("  %-12s %8ld -> %8ld %-4s  want %8ld   %s\n",
                name, dflt, got, unit, want, got == want ? "ok" : "<<< MISMATCH");
}

void testParameters() {
  hardReset();
  if (regRead(REG_VERSION) != SX1278_VERSION) {
    Serial.println("  Radio not answering - run test 1.");
    return;
  }
  setMode(MODE_SLEEP);
  delay(10);
  setMode(MODE_STDBY);

  long dSF = getSpreadFactor(), dBW = getBandwidth(), dCR = getCodingRate();
  long dPRE = getPreamble(), dFRQ = getFrequency();
  uint8_t dSYNC = regRead(REG_SYNC_WORD);
  bool dCRC = getCrcOn();

  if (!radioBegin(gFreq)) { Serial.println("  Stopped answering mid-configure."); return; }

  Serial.printf("Modem readback (tuned to %.3f MHz, vehicle %s):\n",
                gFreq / 1e6, gFreq == WANT_FREQ_A ? "A" : "B");
  Serial.println("               reset      after");
  Serial.println("              default  configure");

  long f = getFrequency();
  bool fOK = labs(f - gFreq) < 1000;
  Serial.printf("  %-12s %8.3f -> %8.3f MHz   want %8.3f   %s\n",
                "frequency", dFRQ / 1e6, f / 1e6, gFreq / 1e6, fOK ? "ok" : "<<< MISMATCH");

  checkLine("spread fact", dSF,  getSpreadFactor(), WANT_SF,     "SF");
  checkLine("bandwidth",   dBW,  getBandwidth(),    WANT_BW,     "Hz");
  checkLine("coding rate", dCR,  getCodingRate(),   WANT_CR,     "4/x");
  checkLine("preamble",    dPRE, getPreamble(),     WANT_PREAMB, "sym");

  uint8_t sync = regRead(REG_SYNC_WORD);
  Serial.printf("  %-12s     0x%02X ->     0x%02X        want     0x%02X   %s\n",
                "sync word", dSYNC, sync, WANT_SYNC, sync == WANT_SYNC ? "ok" : "<<< MISMATCH");

  bool crc = getCrcOn();
  Serial.printf("  %-12s %8s -> %8s        want %8s   %s\n",
                "CRC", dCRC ? "on" : "off", crc ? "on" : "off",
                WANT_CRC ? "on" : "off", crc == WANT_CRC ? "ok" : "<<< MISMATCH");

  uint8_t pa = regRead(REG_PA_CONFIG);
  Serial.printf("  PA output    %s  power=%d dBm\n",
                (pa & 0x80) ? "PA_BOOST (correct)" : "RFO <<< ANTENNA IS NOT THERE",
                (pa & 0x0F) + 2);

  Serial.println();
  Serial.println("  A value that did NOT move from its reset default is a");
  Serial.println("  write the chip refused - look at MOSI (test 1) first.");
  Serial.println("  Then run test 3: air time re-checks SF and BW against");
  Serial.println("  a stopwatch, which catches a register that reads back");
  Serial.println("  correct and is not what the modem is actually doing.");
}


// =====================================================
// TEST 3 - TRANSMIT: AIR TIME AND DUTY CYCLE
//
// The test worth flashing this sketch for.
//
// Air time is a FINGERPRINT of the modem settings. At
// the contract's SF7/BW250/CR4-5, a 237-byte packet
// takes 187 ms. At SF8 it takes 328, at SF9 584. So a
// stopwatch verifies the two settings that matter most,
// with no receiver, no second person and no antenna
// range - and it catches the case test 2 cannot: a
// register that reads back correctly while the modem
// does something else.
//
// It also checks the thing nobody re-checks after the
// packet grows. Radio.cpp sends TWO copies per
// SEND_INTERVAL with COPY_GAP between them. At 237 bytes
// that is 2x187 + 60 = 434 ms inside a 500 ms window -
// 87% of the air, and 66 ms of margin. The comment in
// MRCC_GroundStation still says ~73%, which was true
// when the packet was shorter.
//
// And TX_MAX_AIR is 300 ms. At SF7 that is comfortable.
// At SF8 every single transmission would exceed it, the
// air-time fallback would fire every time, and
// txFallbackCount would climb with telemetry still
// flowing - which is exactly the kind of fault that
// never gets noticed.
// =====================================================

void testTransmit() {
  if (!radioBegin(gFreq)) { Serial.println("  Radio not answering - run test 1."); return; }

  Serial.println();
  Serial.printf("  *** TRANSMITTING on %.3f MHz at %d dBm.\n", gFreq / 1e6, WANT_TXPOWER);
  Serial.println("  *** Do not run this while a rocket is flying on this");
  Serial.println("  *** channel. Antenna MUST be connected.");
  Serial.println();

  float expect = airTimeMs(TEST_PAYLOAD, WANT_SF, WANT_BW, WANT_CR, WANT_PREAMB, WANT_CRC);
  Serial.printf("  payload      %d bytes (worst-case flight packet)\n", TEST_PAYLOAD);
  Serial.printf("  expected     %.1f ms at SF%d / %ld kHz / 4-%d\n",
                expect, WANT_SF, WANT_BW / 1000, WANT_CR);
  Serial.println();

  float sum = 0;
  int   n = 0, fallbacks = 0;

  for (int i = 0; i < 5; i++) {
    setMode(MODE_STDBY);
    regWrite(REG_IRQ_FLAGS, 0xFF);
    regWrite(REG_FIFO_ADDR_PTR, 0);
    for (int b = 0; b < TEST_PAYLOAD; b++) regWrite(REG_FIFO, 'A' + (b % 26));
    regWrite(REG_PAYLOAD_LENGTH, TEST_PAYLOAD);

    unsigned long t0 = micros();
    setMode(MODE_TX);

    bool done = false;
    while (micros() - t0 < 3000000UL) {
      if (regRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) { done = true; break; }
    }
    float ms = (micros() - t0) / 1000.0;
    regWrite(REG_IRQ_FLAGS, 0xFF);

    if (done) { sum += ms; n++; }
    else      { fallbacks++; }

    Serial.printf("  packet %d: %7.1f ms  %s\n", i + 1, ms,
                  done ? "TxDone" : "<<< NO TxDone in 3 s");
  }

  if (n == 0) {
    Serial.println();
    Serial.println("  NOTHING COMPLETED. The PA is tripping or the modem");
    Serial.println("  never entered TX. Check the antenna first - a bare");
    Serial.println("  PA_BOOST pin can trip the overcurrent limit - then");
    Serial.println("  the supply, then test 1.");
    return;
  }

  float avg = sum / n;
  float err = (avg - expect) / expect * 100.0;
  Serial.println();
  Serial.printf("  measured     %.1f ms   (expected %.1f, %+.1f%%)\n", avg, expect, err);

  if (fabs(err) > 15.0) {
    Serial.println();
    Serial.println("  *** AIR TIME DOES NOT MATCH THE CONTRACT ***");
    Serial.println("  The modem is not running the settings it reports.");
    Serial.println("  For reference, at this payload length:");
    for (int sf = 6; sf <= 12; sf++) {
      Serial.printf("     SF%-2d %7.1f ms%s\n", sf,
                    airTimeMs(TEST_PAYLOAD, sf, WANT_BW, WANT_CR, WANT_PREAMB, WANT_CRC),
                    sf == WANT_SF ? "   <- the contract" : "");
    }
    Serial.println("  A doubled time is one SF too high; a halved one is");
    Serial.println("  double the bandwidth. Match it to the row above.");
  }
  else {
    Serial.println("  SF and BW confirmed against the clock, not just the");
    Serial.println("  register. This is the strongest evidence available");
    Serial.println("  without a second radio.");
  }

  // ---- what this costs on the air ----
  float twoCopies = 2 * avg + COPY_GAP;
  float duty = twoCopies / SEND_INTERVAL * 100.0;
  Serial.println();
  Serial.printf("  Radio.cpp sends TWO copies + %lu ms gap per %lu ms:\n",
                COPY_GAP, SEND_INTERVAL);
  Serial.printf("    on air   %.0f ms of %lu   = %.0f%% duty\n",
                twoCopies, SEND_INTERVAL, duty);
  Serial.printf("    margin   %.0f ms\n", SEND_INTERVAL - twoCopies);

  if (twoCopies > SEND_INTERVAL) {
    Serial.println("    *** OVER BUDGET. The second copy cannot finish");
    Serial.println("    *** before the next packet is due. Shorten the");
    Serial.println("    *** packet, drop to one copy, or slow SEND_INTERVAL.");
  }
  else if (duty > 85.0) {
    Serial.println("    Tight. Any further growth in the packet eats the");
    Serial.println("    margin - check this again after adding a field.");
  }

  if (avg > TX_MAX_AIR) {
    Serial.printf("    *** air time %.0f ms EXCEEDS TX_MAX_AIR (%lu ms).\n",
                  avg, TX_MAX_AIR);
    Serial.println("    *** Radio.cpp's fallback would fire on EVERY packet");
    Serial.println("    *** and txFallbackCount would climb while telemetry");
    Serial.println("    *** kept flowing. Silent degradation.");
  }

  setMode(MODE_STDBY);
}


// =====================================================
// TEST 4 - DIO0 TxDone
//
// Radio.cpp transmits asynchronously: endPacket(true)
// returns immediately and the state machine waits for
// onLoRaTxDone(), which is the DIO0 rising edge.
//
// If that line is not wired, telemetry DOES NOT STOP. It
// falls through to the TX_MAX_AIR timeout instead, which
// works - and costs up to 300 ms per packet against a
// 500 ms budget, silently. txFallbackCount is the only
// evidence and nobody reads it.
//
// That makes this the most easily-missed fault on the
// transmitter, and it is invisible from the ground.
// =====================================================

void testTxDone() {
  if (!radioBegin(gFreq)) { Serial.println("  Radio not answering - run test 1."); return; }

  regWrite(REG_DIO_MAPPING_1, 0x40);      // DIO0 = TxDone in TX mode
  Serial.printf("Watching GPIO%d during transmission. Antenna required.\n", PIN_DIO0);

  int edges = 0, sent = 0;

  for (int i = 0; i < 5; i++) {
    setMode(MODE_STDBY);
    regWrite(REG_IRQ_FLAGS, 0xFF);
    regWrite(REG_FIFO_ADDR_PTR, 0);
    for (int b = 0; b < 32; b++) regWrite(REG_FIFO, 'T');
    regWrite(REG_PAYLOAD_LENGTH, 32);

    bool prev = digitalRead(PIN_DIO0);
    bool sawEdge = false;

    setMode(MODE_TX);
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
      bool now = digitalRead(PIN_DIO0);
      if (now && !prev) sawEdge = true;
      prev = now;
      if (regRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) break;
    }
    // The pin is read BEFORE the flag is cleared: clearing
    // TxDone is what drops DIO0, so checking afterwards
    // would report a dead line on a perfectly good one.
    bool high = digitalRead(PIN_DIO0);
    regWrite(REG_IRQ_FLAGS, 0xFF);

    sent++;
    if (sawEdge || high) edges++;
    Serial.printf("  tx %d: DIO0 %s\n", i + 1,
                  (sawEdge || high) ? "went HIGH (good)" : "never moved  <<<");
  }

  setMode(MODE_STDBY);
  Serial.println();
  Serial.printf("STOPPED. %d transmissions, %d with a DIO0 edge.\n", sent, edges);

  if (edges == 0) {
    Serial.println("  DIO0 IS NOT REACHING THE ESP32.");
    Serial.printf("  The wire is not on GPIO%d, or not connected.\n", PIN_DIO0);
    Serial.println("  Telemetry will still work - Radio.cpp falls back to");
    Serial.println("  its TX_MAX_AIR timeout - but every packet pays up to");
    Serial.printf("  %lu ms of a %lu ms budget for nothing, and the only\n",
                  TX_MAX_AIR, SEND_INTERVAL);
    Serial.println("  symptom is txFallbackCount rising on the [TX] line.");
  }
  else if (edges < sent) {
    Serial.println("  INTERMITTENT. A loose DIO0 wire, or a solder joint");
    Serial.println("  that opens warm. Reseat it before flying.");
  }
  else {
    Serial.println("  DIO0 IS WIRED AND FIRING. Radio.cpp's async TX path");
    Serial.println("  will work as designed.");
  }
}


// =====================================================
// TEST 5 - POWER SWEEP
//
// Transmits at rising power and finds where the supply
// gives out.
//
// It cannot measure the sag - the VBAT divider is not
// wired (VBAT_ENABLED 0 in Config.h) - and it does not
// need to. The symptom of a collapsing supply is the
// board RESETTING, and esp_reset_reason() survives that.
// So the level is stamped into RTC memory before each
// burst and read back at boot.
//
// If this test reboots the board, that IS the result.
// Come back, read the banner, and it names the level.
//
// Brownouts on this airframe happen under VIBRATION,
// where a connection breaks contact for a moment. A
// bench ceiling found here is the OPTIMISTIC number.
// =====================================================

void powerSweep() {
  if (!radioBegin(gFreq)) { Serial.println("  Radio not answering - run test 1."); return; }

  Serial.println();
  Serial.println("  *** TRANSMITS at rising power, up to 20 dBm.");
  Serial.println("  *** Antenna MUST be connected - a bare PA_BOOST pin");
  Serial.println("  *** trips the overcurrent limit and this test will");
  Serial.println("  *** blame your supply for a missing antenna.");
  Serial.println("  *** If the board REBOOTS, that is the answer. Come");
  Serial.println("  *** back and read the boot banner.");
  Serial.println();

  const int levels[] = { 2, 5, 8, 11, 14, 17, 20 };
  const int nLevels = sizeof(levels) / sizeof(levels[0]);

  rtcSweeping = true;

  for (int i = 0; i < nLevels; i++) {
    rtcSweepLevel = levels[i];
    setTxPower(levels[i]);

    int ok = 0;
    for (int b = 0; b < 3; b++) {
      setMode(MODE_STDBY);
      regWrite(REG_IRQ_FLAGS, 0xFF);
      regWrite(REG_FIFO_ADDR_PTR, 0);
      for (int k = 0; k < 64; k++) regWrite(REG_FIFO, 'P');
      regWrite(REG_PAYLOAD_LENGTH, 64);

      setMode(MODE_TX);
      unsigned long t0 = millis();
      while (millis() - t0 < 2000) {
        if (regRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) { ok++; break; }
      }
      regWrite(REG_IRQ_FLAGS, 0xFF);
    }

    Serial.printf("  %2d dBm: %d/3 completed %s\n", levels[i], ok,
                  ok == 3 ? "" : "<<< PA tripping or supply sagging");
  }

  rtcSweeping = false;
  setTxPower(WANT_TXPOWER);
  setMode(MODE_STDBY);

  Serial.println();
  Serial.println("  Survived the whole sweep - the supply holds 20 dBm on");
  Serial.println("  the bench. That is NOT a promise about the pad: this");
  Serial.println("  test cannot shake the wiring, and vibration is what");
  Serial.printf("  breaks it. Flight setting is %d dBm.\n", WANT_TXPOWER);
  Serial.println("  A level completing 0/3 without a reboot is the PA");
  Serial.println("  tripping, not a brownout - check the antenna.");
}


// =====================================================
// TEST 6 - LISTEN
//
// The other half of GS_Doctor's test 7. Run the beacon
// there, this here, and the pair proves the link in both
// directions with nobody standing in a field.
//
// Also useful alone: it hears the OTHER rocket. If both
// airframes are powered on the same channel, this is
// where you find out - and neither link survives that.
// =====================================================

void testListen() {
  if (!radioBegin(gFreq)) { Serial.println("  Radio not answering - run test 1."); return; }

  regWrite(REG_IRQ_FLAGS, 0xFF);
  setMode(MODE_RX_CONTINUOUS);

  Serial.printf("Listening on %.3f MHz. Press any key to stop.\n", gFreq / 1e6);
  Serial.println("Run GS_Doctor's test 7 on the ground station to feed this.");

  unsigned long good = 0, bad = 0;
  unsigned long lastReport = millis();

  while (!Serial.available()) {
    uint8_t flags = regRead(REG_IRQ_FLAGS);

    if (flags & IRQ_RX_DONE) {
      regWrite(REG_IRQ_FLAGS, 0xFF);

      if (flags & IRQ_PAYLOAD_CRC_ERROR) {
        bad++;
        Serial.printf("  [CRC FAIL] rssi=%d\n", regRead(REG_PKT_RSSI_VALUE) - 157);
      }
      else {
        good++;
        int len = regRead(REG_RX_NB_BYTES);
        int rssi = regRead(REG_PKT_RSSI_VALUE) - 157;
        float snr = (int8_t)regRead(REG_PKT_SNR_VALUE) * 0.25;
        regWrite(REG_FIFO_ADDR_PTR, regRead(REG_FIFO_RX_CURRENT_ADDR));

        char preview[49];
        int n = len < 48 ? len : 48;
        for (int i = 0; i < n; i++) {
          char c = (char)regRead(REG_FIFO);
          preview[i] = (c >= 32 && c < 127) ? c : '.';
        }
        preview[n] = '\0';
        for (int i = n; i < len; i++) regRead(REG_FIFO);

        Serial.printf("  [OK] len=%d rssi=%d snr=%.1f | %s\n", len, rssi, snr, preview);
      }
    }

    if (millis() - lastReport > 3000) {
      lastReport = millis();
      Serial.printf("  ... good=%lu crc_fail=%lu  noise floor=%d dBm\n",
                    good, bad, regRead(REG_RSSI_VALUE) - 157);
    }
  }
  while (Serial.available()) Serial.read();

  setMode(MODE_STDBY);
  Serial.printf("STOPPED. good=%lu crc_fail=%lu\n", good, bad);
  if (good == 0 && bad == 0) {
    Serial.println("  Nothing heard. If GS_Doctor's beacon is running, the");
    Serial.println("  two boxes disagree on a link parameter - run test 2");
    Serial.println("  on BOTH and compare the want columns.");
  }
}


// =====================================================
// TEST 7 - I2C BUS SCAN
//
// Before asking why a sensor reads wrong, ask whether it
// is there. Both sensors share one bus, so one device
// holding SDA low takes out the other as well - and the
// symptom is "the barometer died" when the fault is the
// IMU.
// =====================================================

static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((int)addr, 1) != 1) return 0xFF;
  return Wire.read();
}

static void i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void i2cScan() {
  Serial.printf("Scanning I2C on SDA=%d SCL=%d ...\n", PIN_I2C_SDA, PIN_I2C_SCL);

  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cPresent(a)) continue;
    found++;
    const char *who = "";
    if (a == ICM_ADDR_AD0_LOW)  who = "  <- ICM-20948 (AD0 low, matches AD0_VAL 0)";
    if (a == ICM_ADDR_AD0_HIGH) who = "  <- ICM-20948 (AD0 HIGH - Config.h says 0!)";
    if (a == BMP_ADDR_PRIMARY)  who = "  <- BMP280 (primary, 0x76)";
    if (a == BMP_ADDR_FALLBACK) who = "  <- BMP280 (fallback, 0x77)";
    Serial.printf("  0x%02X%s\n", a, who);
  }

  if (found == 0) {
    Serial.println("  NOTHING ON THE BUS.");
    Serial.println("  Both sensors gone at once is almost never both sensors:");
    Serial.println("  it is the bus. Check SDA/SCL are not swapped, that the");
    Serial.println("  pullups are present, and that 3V3 reaches the breakout.");
    return;
  }

  bool icm = i2cPresent(ICM_ADDR_AD0_LOW) || i2cPresent(ICM_ADDR_AD0_HIGH);
  bool bmp = i2cPresent(BMP_ADDR_PRIMARY) || i2cPresent(BMP_ADDR_FALLBACK);
  Serial.println();
  Serial.printf("  IMU  %s\n", icm ? "present" : "MISSING - tilt and launch detect are blind");
  Serial.printf("  BARO %s\n", bmp ? "present" : "MISSING - apogee would be TIMER ONLY");
}


// =====================================================
// TEST 8 - SENSORS
//
// Two questions the boot banner cannot answer.
//
// FIRST: how fast is the IMU actually producing samples?
// "The chip answers" and "the chip is delivering data"
// are different, and the firmware conflated them until
// 2026-09-04: a dead data path with a live I2C interface
// flapped DOWN/RECOVERED every 5 s and reported OK the
// whole time. This measures the rate directly, so the
// answer is a number instead of a flag.
//
// SECOND, and this is the one with an open question
// behind it: WHICH AXIS IS UP. The IMU is mounted
// rotated, so body Z is not vertical, which makes the
// onboard tilt and attitude wrong. Fixing it needs one
// measurement - which axis reads +/-1 g with the rocket
// standing upright - and that measurement is this test.
// Stand the airframe up, press 8, read the answer.
// =====================================================

void testSensors() {
  uint8_t icmAddr = i2cPresent(ICM_ADDR_AD0_LOW) ? ICM_ADDR_AD0_LOW :
                    i2cPresent(ICM_ADDR_AD0_HIGH) ? ICM_ADDR_AD0_HIGH : 0;

  Serial.println();
  Serial.println("--- IMU (ICM-20948) ---");

  if (!icmAddr) {
    Serial.println("  Not on the bus. Run test 7.");
  }
  else {
    // Bank 0 explicitly. Every register this test touches
    // lives there, and the ICM keeps its bank selection
    // across a soft reset of the ESP32 - so a firmware
    // that left it in bank 2 would make WHO_AM_I read as
    // whatever bank 2 has at 0x00.
    i2cWriteReg(icmAddr, 0x7F, 0x00);

    uint8_t who = i2cReadReg(icmAddr, ICM_REG_WHOAMI);
    Serial.printf("  addr 0x%02X  WHO_AM_I=0x%02X  (expect 0x%02X)  %s\n",
                  icmAddr, who, ICM_WHOAMI_VALUE,
                  who == ICM_WHOAMI_VALUE ? "ok" : "<<< not an ICM-20948");

    // Wake it: reset default is asleep, and an asleep IMU
    // answers every register and produces nothing - which
    // is precisely the failure this test exists to name.
    i2cWriteReg(icmAddr, ICM_REG_PWR_MGMT_1, 0x01);
    delay(50);

    // Count data-ready for a second. At the reset sample
    // rate this is ~1125 Hz; the firmware divides it to
    // ~102. Either way, ZERO is the finding.
    //
    // INT_STATUS_1 is READ-TO-CLEAR, so every read that
    // finds the bit set is one or more new samples since
    // the last read - no edge detection, which would
    // halve the count against a flag that clears itself.
    // The rate is therefore a floor, not an exact figure;
    // the question here is zero or not zero.
    //
    // 0xFF is i2cReadReg's failure return and its bit 0
    // is set, so it has to be excluded explicitly or a
    // bus that died mid-test would report the highest
    // rate this loop can turn over. A false pass on the
    // one test whose whole job is catching a silent
    // sensor would be worse than no test.
    unsigned long samples = 0, reads = 0, failed = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 1000) {
      uint8_t st = i2cReadReg(icmAddr, ICM_REG_INT_STATUS);
      reads++;
      if (st == 0xFF) failed++;
      else if (st & 0x01) samples++;
    }

    if (failed > reads / 2) {
      Serial.printf("  data ready: BUS FAILED (%lu of %lu reads)\n", failed, reads);
      Serial.println("  The IMU stopped acknowledging mid-test. That is the");
      Serial.println("  bus or the wiring, not the sensor's data path.");
      samples = 0;
    }
    else {
      Serial.printf("  data ready: >= %lu samples/s (over %lu polls)\n", samples, reads);
    }

    if (samples == 0) {
      Serial.println("  *** THE CHIP ANSWERS AND PRODUCES NO DATA. ***");
      Serial.println("  This is the exact failure that used to flap the");
      Serial.println("  firmware between DOWN and RECOVERED forever while");
      Serial.println("  reporting IMU=OK. Power-cycle it; if it persists,");
      Serial.println("  the sensor is dead however well it talks.");
    }
    else {
      // ---- which axis is up ----
      Wire.beginTransmission(icmAddr);
      Wire.write(ICM_REG_ACCEL_XOUT);
      Wire.endTransmission(false);
      Wire.requestFrom((int)icmAddr, 6);

      int16_t raw[3];
      for (int i = 0; i < 3; i++) {
        uint8_t h = Wire.read(), l = Wire.read();
        raw[i] = (int16_t)((h << 8) | l);
      }
      // Reset default full scale is +/-2 g -> 16384 LSB/g.
      // The flight build runs +/-16 g; this is the doctor's
      // own configuration, so the constant is the doctor's.
      float g[3] = { raw[0] / 16384.0f, raw[1] / 16384.0f, raw[2] / 16384.0f };
      const char *names[3] = { "X", "Y", "Z" };

      Serial.printf("  accel: X=%+.2f g  Y=%+.2f g  Z=%+.2f g\n", g[0], g[1], g[2]);

      int up = 0;
      for (int i = 1; i < 3; i++) if (fabs(g[i]) > fabs(g[up])) up = i;

      Serial.println();
      Serial.println("  *** WHICH AXIS IS UP ***");
      Serial.println("  Only meaningful with the airframe STANDING UPRIGHT.");
      if (fabs(g[up]) < 0.8 || fabs(g[up]) > 1.2) {
        Serial.printf("  No axis reads close to 1 g (largest is %s at %+.2f).\n",
                      names[up], g[up]);
        Serial.println("  The board is not upright, or it is moving. Stand it");
        Serial.println("  up, hold it still, and press 8 again.");
      }
      else {
        Serial.printf("  Gravity is on %s (%+.2f g).\n", names[up], g[up]);
        if (up == 2) {
          Serial.println("  Body Z is vertical - the mounting is what the");
          Serial.println("  firmware's tilt and attitude math assumes.");
        }
        else {
          Serial.printf("  Body Z is NOT vertical: %s is. The firmware's tilt\n", names[up]);
          Serial.println("  and onboard attitude are computed against Z, so");
          Serial.println("  they are wrong by that rotation. This is the");
          Serial.printf("  measurement that unblocks the fix: UP = %s, sign %s.\n",
                        names[up], g[up] > 0 ? "positive" : "negative");
        }
      }
    }
  }

  // ---- barometer ----
  Serial.println();
  Serial.println("--- BARO (BMP280) ---");

  uint8_t bmpAddr = i2cPresent(BMP_ADDR_PRIMARY) ? BMP_ADDR_PRIMARY :
                    i2cPresent(BMP_ADDR_FALLBACK) ? BMP_ADDR_FALLBACK : 0;
  if (!bmpAddr) {
    Serial.println("  Not on the bus. Run test 7.");
    Serial.println("  Without it apogee would be TIMER ONLY. Do not fly.");
    return;
  }

  uint8_t id = i2cReadReg(bmpAddr, BMP_REG_ID);
  Serial.printf("  addr 0x%02X  chip id=0x%02X  %s\n", bmpAddr, id,
                id == 0x58 ? "(BMP280)" :
                id == 0x60 ? "(BME280 - works, has humidity too)" : "<<< unknown");

  // Calibration, then one compensated reading. The full
  // arithmetic is here rather than a raw ADC dump because
  // "101325 Pa" is checkable against a weather app and
  // "raw 415236" is not.
  uint8_t cal[24];
  Wire.beginTransmission(bmpAddr);
  Wire.write(BMP_REG_CALIB);
  Wire.endTransmission(false);
  Wire.requestFrom((int)bmpAddr, 24);
  for (int i = 0; i < 24; i++) cal[i] = Wire.read();

  uint16_t T1 = cal[0] | (cal[1] << 8);
  int16_t  T2 = cal[2] | (cal[3] << 8);
  int16_t  T3 = cal[4] | (cal[5] << 8);
  uint16_t P1 = cal[6] | (cal[7] << 8);
  int16_t  P2 = cal[8] | (cal[9] << 8);
  int16_t  P3 = cal[10] | (cal[11] << 8);
  int16_t  P4 = cal[12] | (cal[13] << 8);
  int16_t  P5 = cal[14] | (cal[15] << 8);
  int16_t  P6 = cal[16] | (cal[17] << 8);
  int16_t  P7 = cal[18] | (cal[19] << 8);
  int16_t  P8 = cal[20] | (cal[21] << 8);
  int16_t  P9 = cal[22] | (cal[23] << 8);

  if (T1 == 0 || P1 == 0) {
    Serial.println("  Calibration reads as zeros - the sensor answers but");
    Serial.println("  its OTP is not readable. Treat every number below as");
    Serial.println("  meaningless.");
    return;
  }

  i2cWriteReg(bmpAddr, BMP_REG_CTRL_MEAS, (1 << 5) | (4 << 2) | 3);   // x1 T, x8 P, normal
  delay(100);

  Wire.beginTransmission(bmpAddr);
  Wire.write(BMP_REG_PRESS_MSB);
  Wire.endTransmission(false);
  Wire.requestFrom((int)bmpAddr, 6);
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();

  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);

  double v1 = (adcT / 16384.0 - T1 / 1024.0) * T2;
  double v2 = ((adcT / 131072.0 - T1 / 8192.0) * (adcT / 131072.0 - T1 / 8192.0)) * T3;
  double tFine = v1 + v2;
  double tempC = tFine / 5120.0;

  v1 = tFine / 2.0 - 64000.0;
  v2 = v1 * v1 * P6 / 32768.0;
  v2 = v2 + v1 * P5 * 2.0;
  v2 = v2 / 4.0 + P4 * 65536.0;
  v1 = (P3 * v1 * v1 / 524288.0 + P2 * v1) / 524288.0;
  v1 = (1.0 + v1 / 32768.0) * P1;

  double pa = 0;
  if (v1 != 0.0) {
    pa = 1048576.0 - adcP;
    pa = (pa - v2 / 4096.0) * 6250.0 / v1;
    v1 = P9 * pa * pa / 2147483648.0;
    v2 = pa * P8 / 32768.0;
    pa = pa + (v1 + v2 + P7) / 16.0;
  }

  Serial.printf("  temperature  %.1f C\n", tempC);
  Serial.printf("  pressure     %.1f hPa\n", pa / 100.0);

  if (pa < 30000 || pa > 110000) {
    Serial.println("  *** OUT OF RANGE. Sea level is ~1013 hPa and the");
    Serial.println("  *** highest place on earth is ~340. A reading outside");
    Serial.println("  *** that is not weather, it is the sensor.");
  }
  else {
    double alt = 44330.0 * (1.0 - pow(pa / 101325.0, 0.1903));
    Serial.printf("  altitude MSL %.1f m  (at SEA_LEVEL_HPA 1013.25)\n", alt);
    Serial.println("  This is what groundAlt will be primed to on the pad,");
    Serial.println("  and what the downlink's AL= is measured FROM. It is");
    Serial.println("  supposed to be your site elevation for today's");
    Serial.println("  weather, not zero.");
  }
}


// =====================================================
// TEST 9 - GPS
//
// Prints raw NMEA. Nothing is parsed here on purpose:
// the flight build has TinyGPS++ for that, and when the
// question is "is the GPS talking", a parser between you
// and the bytes is one more thing to be wrong.
//
// The baud hunt matters because a module reflashed to
// 38400 looks exactly like a module that is not wired.
// =====================================================

void testGps() {
  Serial.printf("Opening GPS UART on RX=%d (module TX -> here) at 9600.\n", PIN_GPS_RX);
  Serial1.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  unsigned long bytes = 0, sentences = 0;
  unsigned long t0 = millis();
  String line;

  while (millis() - t0 < 5000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      bytes++;
      if (c == '\n') {
        if (line.startsWith("$")) {
          sentences++;
          if (sentences <= 6) Serial.printf("  %s\n", line.c_str());
        }
        line = "";
      }
      else if (c != '\r' && line.length() < 120) {
        line += c;
      }
    }
  }

  Serial.println();
  Serial.printf("  5 s: %lu bytes, %lu NMEA sentences\n", bytes, sentences);

  if (bytes == 0) {
    Serial.println("  SILENT. Hunting for another baud rate...");
    const long bauds[] = { 4800, 19200, 38400, 57600, 115200 };
    bool found = false;

    for (int i = 0; i < 5; i++) {
      Serial1.begin(bauds[i], SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
      delay(50);
      while (Serial1.available()) Serial1.read();

      unsigned long n = 0, tt = millis();
      while (millis() - tt < 1500) if (Serial1.available()) { Serial1.read(); n++; }

      Serial.printf("    %6ld baud: %lu bytes\n", bauds[i], n);
      if (n > 50) {
        Serial.printf("    *** THE MODULE IS AT %ld BAUD, not 9600.\n", bauds[i]);
        Serial.println("    *** Set GPS_BAUD in Config.h to match.");
        found = true;
      }
    }
    if (!found) {
      Serial.println("  Nothing at any baud. This is wiring, not configuration:");
      Serial.printf("  the module's TX must reach GPIO%d, and it needs 3V3\n", PIN_GPS_RX);
      Serial.println("  and GND. TX->RX is the pair people cross.");
    }
  }
  else if (sentences == 0) {
    Serial.println("  BYTES BUT NO SENTENCES. Something is transmitting and");
    Serial.println("  it is not NMEA at this baud - wrong rate, or the module");
    Serial.println("  is in a binary mode (UBX). The bytes are the proof the");
    Serial.println("  wiring is fine; only the rate or the mode is wrong.");
  }
  else {
    Serial.println("  GPS IS TALKING. A fix needs sky - indoors you will see");
    Serial.println("  valid sentences with empty position fields for a long");
    Serial.println("  time, and that is normal, not a fault.");
    Serial.println("  gpsOK in the firmware means TALKING, not FIXED.");
  }

  Serial1.end();
}


// =====================================================
// S - SPI PIN SCAN
//
// GS_Doctor's test 8, ported. Holds three pins at your
// configured values and sweeps the fourth, four times
// over, looking for REG_VERSION == 0x12.
//
// Narrow on purpose. The full permutation is four pins
// from ~24 candidates - about 300,000 orderings - and it
// answers a question you only have when the module is
// missing entirely. The fault that actually happens on a
// bench is ONE jumper in the wrong hole, and that is 4 x
// 24 tries.
//
// THE PYRO GATE IS NEVER SWEPT. GPIO2 fires the charge,
// and a scan that clocked SPI into it would be driving
// the one pin this whole sketch refuses to touch. It is
// excluded by pinIsFree() below, unconditionally.
//
// The other assigned pins - I2C, GPS, SD - are excluded
// too, but only because a pin already doing another job
// is not a candidate for this one. Disconnect the module
// from them if you genuinely suspect a swap there.
// =====================================================

// ESP32-S3 GPIOs that are safe to drive. Same list SD_Doctor
// uses. Excluded: 0/45/46 (strapping), 19/20 (native USB),
// 26-32 (SPI flash), 33-37 (octal PSRAM), 43/44 (UART0 - the
// serial monitor you are reading this on).
static const int S3_SAFE_PINS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 47, 48
};
static const int S3_SAFE_COUNT = sizeof(S3_SAFE_PINS) / sizeof(S3_SAFE_PINS[0]);

static bool pinIsFree(int p, int which) {
  if (p == PIN_PYRO_GATE) return false;            // safety, non-negotiable
  if (p == PIN_RST || p == PIN_DIO0) return false;
  if (p == PIN_I2C_SDA || p == PIN_I2C_SCL) return false;
  if (p == PIN_GPS_RX  || p == PIN_GPS_TX)  return false;
  if (p == 7 || p == 14 || p == 15 || p == 16) return false;   // SD bus
  if (which != 0 && p == PIN_SCK)  return false;
  if (which != 1 && p == PIN_MISO) return false;
  if (which != 2 && p == PIN_MOSI) return false;
  if (which != 3 && p == PIN_SS)   return false;
  return true;
}

static bool probeAt(int sck, int miso, int mosi, int ss) {
  radioSPI.end();
  pinMode(ss, OUTPUT);
  digitalWrite(ss, HIGH);
  radioSPI.begin(sck, miso, mosi, ss);

  int saved = PIN_SS;
  PIN_SS = ss;                     // regRead() drives PIN_SS
  hardReset();
  uint8_t v = regRead(REG_VERSION);
  PIN_SS = saved;

  return v == SX1278_VERSION;
}

static void sweepOne(const char *label, int which) {
  int sck = PIN_SCK, miso = PIN_MISO, mosi = PIN_MOSI, ss = PIN_SS;
  int have = which == 0 ? PIN_SCK : which == 1 ? PIN_MISO :
             which == 2 ? PIN_MOSI : PIN_SS;

  Serial.printf("  sweeping %-4s (you have %d) ...\n", label, have);

  for (int i = 0; i < S3_SAFE_COUNT; i++) {
    int p = S3_SAFE_PINS[i];
    if (!pinIsFree(p, which)) continue;

    if      (which == 0) sck  = p;
    else if (which == 1) miso = p;
    else if (which == 2) mosi = p;
    else                 ss   = p;

    if (probeAt(sck, miso, mosi, ss)) {
      Serial.printf("    *** FOUND: %s = %d   (Config.h says %d)\n", label, p, have);
    }
  }
}

void spiPinScan() {
  Serial.println();
  Serial.println("Sweeping one pin at a time, the other three held at your");
  Serial.println("configured values. Looking for REG_VERSION == 0x12.");
  Serial.printf("GPIO%d (pyro gate) is never touched.\n", PIN_PYRO_GATE);
  Serial.println();

  setSpiHz(1000000);        // slow: this is a hunt, not a benchmark
  sweepOne("SCK",  0);
  sweepOne("MISO", 1);
  sweepOne("MOSI", 2);
  sweepOne("SS",   3);

  // Put the bus back, or every later test runs on whatever
  // the last probe happened to leave configured.
  radioSPI.end();
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);
  radioSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  setSpiHz(8000000);

  Serial.println();
  Serial.println("Nothing found means no SINGLE pin change makes the module");
  Serial.println("answer. That leaves: no power at the chip, GND not shared,");
  Serial.println("a dead module, or TWO wrong wires - none of which this");
  Serial.println("scan can see. Measure 3V3 to GND on the module's own pads");
  Serial.println("before going further.");
  Serial.println();
  Serial.println("RST and DIO0 are not scanned: RST is exercised by every");
  Serial.println("probe here, and DIO0 has its own test (4).");
}
