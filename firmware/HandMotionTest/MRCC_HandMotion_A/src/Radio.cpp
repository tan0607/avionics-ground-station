#include "Radio.h"
#include "Config.h"
#include "State.h"
#include "Sensors.h"
#include "Flight.h"
#include "Pyro.h"
#include "Filters.h"
#include "Storage.h"   // the recorder block: file index, lines, write errors
#include <SPI.h>
#include <LoRa.h>

int  txPower    = TX_POWER_DEFAULT;
int  txCopies   = TX_COPIES_DEFAULT;
bool showPacket = true;

unsigned long lastAirTime     = 0;
unsigned long txBusyCount     = 0;
unsigned long txTimeoutCount  = 0;
unsigned long txFallbackCount = 0;

// 255 is LoRa's hard payload limit; +1 for snprintf's NUL. Sized to the limit
// rather than to the measured packet because the failure mode is silent: an
// over-long packet is truncated at the buffer, the transmitter reports the
// truncated length, and the ground station sees a well-formed frame that
// simply stops early. The tail is the health block, so the fields that vanish
// first are exactly the ones that say something is wrong.
char txPacket[256];
int  txPacketLen = 0;

#define TX_IDLE  0
#define TX_COPY1 1
#define TX_GAP   2
#define TX_COPY2 3

static uint8_t txState = TX_IDLE;

static volatile bool txDoneFlag = false;

static unsigned long txStartTime = 0;
static unsigned long txGapStart  = 0;
static unsigned long lastSend    = 0;

static void buildTelemetryPacket();
static bool startLoRaCopy(int copyNumber);
static void reportPacketSent();
static bool txComplete();


// =====================================================
// TX DONE INTERRUPT
//
// Fired by the LoRa library on the DIO0 rising edge.
// One flag only - no serial, no SPI in here.
// =====================================================

void IRAM_ATTR onLoRaTxDone() {
  txDoneFlag = true;
}


// =====================================================
// INIT
//
// Returns false if the radio is absent. The caller marks
// it down and carries on - logging continues either way.
// =====================================================

bool initRadio(bool verbose) {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    if (verbose) {
      Serial.println("[LORA] FAILED - no radio detected");
      Serial.println("[LORA] Continuing WITHOUT telemetry.");
      Serial.println("[LORA] SD logging is unaffected.");
      Serial.println("[LORA] Will retry automatically every 5 s.");
    }
    return false;
  }

  // The receiver must use these same settings.
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(250E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(0x12);
  LoRa.setTxPower(txPower);
  LoRa.enableCrc();

  // TxDone interrupt on DIO0 - this is what makes the
  // radio non-blocking without touching the library.
  LoRa.onTxDone(onLoRaTxDone);

  txState = TX_IDLE;

  // Say WHICH vehicle, not just "SUCCESS". VEHICLE in
  // Config.h is the only thing separating this airframe
  // from the other one, and a board flashed from the
  // wrong setting behaves perfectly until both are
  // powered at once - which is the pad, not the bench.
  // This line is where that mistake is catchable.
  if (verbose) {
    Serial.print("[LORA] SUCCESS - vehicle ");
    Serial.print(VEHICLE_NAME);
    Serial.print(" @ ");
    Serial.print(LORA_FREQ / 1e6, 3);
    Serial.println(" MHz  (ground station must match)");
  }
  return true;
}


void applyTxPower() {
  if (radioOK) LoRa.setTxPower(txPower);
}


// =====================================================
// PACKET
// =====================================================

static void buildTelemetryPacket() {
  // Flight fields go FIRST. If the packet is ever
  // truncated by a marginal link, the state, altitude
  // and whether the charge has gone are the fields you
  // cannot afford to lose.
  //
  // Gyro dropped to whole deg/s to buy back the bytes,
  // and VX/VY dropped entirely - they are computed FROM
  // GS and CRS, which are already in this packet, so on
  // the air they were pure redundancy. The card still
  // logs both at full precision.
  //
  // Typical packet is ~185 bytes; 231 was the worst
  // measured, 237 since IM was added. Feeding every field
  // its format-width maximum at once gives 252 - not a
  // flight this rocket will have (it needs a 68-year
  // packet count AND a 32 km altitude AND 16 g at the same
  // instant) but it is above the old 250-byte buffer, so
  // the buffer is now 256. With VX/VY still in, the same
  // arithmetic gave 258 and the tail really would have
  // gone.
  //
  // SD / BA / IM are the subsystem health bits. IM is here
  // because the ground station had no way to see the IMU
  // at all: it was guessing from AX/AY/AZ being non-zero,
  // and a dead IMU used to downlink its last good sample
  // forever, so the guess read OK through the failure.
  // There is deliberately no LORA bit - serviceTelemetry()
  // returns early when the radio is down, so the field
  // could only ever be 1 in a packet that arrived. Silence
  // is the honest signal there, and the ground station
  // already reads it.
  snprintf(
    txPacket, sizeof(txPacket),
    "MRCC,HT=1,PKT=%lu,T=%.1f,ST=%s,AL=%.1f,VZ=%.1f,MX=%.1f,AR=%d,FI=%d,"
    "GD=%d,GF=%d,SAT=%d,"
    "LAT=%.5f,LON=%.5f,GA=%.1f,GS=%.1f,CRS=%.0f,"
    "AX=%.2f,AY=%.2f,AZ=%.2f,GX=%.0f,GY=%.0f,GZ=%.0f,"
    "HDG=%.0f,SD=%d,BA=%d,IM=%d",
    packetNumber, millis() / 1000.0,
    stateName(flightState), altFiltered, vertVel, maxAlt,
    pyroArmed ? 1 : 0, pyroFired ? 1 : 0,
    gpsData ? 1 : 0, gpsFix ? 1 : 0, satellites,
    latitude, longitude, gpsAltitude, gpsSpeed, gpsCourse,
    // R on the console swaps these six between raw and
    // filtered. Same field names, same packet length -
    // the card is still logging both either way.
    txFiltered ? fax : ax, txFiltered ? fay : ay, txFiltered ? faz : az,
    txFiltered ? fgx : gx, txFiltered ? fgy : gy, txFiltered ? fgz : gz,
    txFiltered ? headingFilt : heading,
    sdOK ? 1 : 0, baroOK ? 1 : 0, imuOK ? 1 : 0
  );

  // Prelaunch readiness, computed by the same gates that decide arming.
  // AW=blocker bits, AD=boot-delay seconds, AS=observed-stillness seconds.
  // Ceil remaining time; AW=0 is NOT an arming acknowledgement (ST/AR are).
  // All-or-none append preserves flight/health fields at the LoRa byte limit.
  // Give this block priority over the sparse recorder details while in PAD.
  if (flightState == FS_PAD) {
    const ArmReadiness ready = armReadiness();
    char block[40];
    int n = snprintf(block, sizeof(block), ",AW=%u,AD=%lu,AS=%lu",
                     (unsigned int) ready.wait,
                     (ready.delayRemainingMs + 999) / 1000,
                     (ready.stillRemainingMs + 999) / 1000);
    size_t used = strlen(txPacket);
    if (n > 0 && n < (int) sizeof(block) && used + n <= TX_PAYLOAD_MAX) {
      strcpy(txPacket + used, block);
    }
  }

  // ---- recorder block, one packet in ten (SD_BLOCK_EVERY) ----
  //
  // What printStatus's [SD] line says, minus the parts the ground can work out
  // for itself: which file is open (SDF), how many lines are in it (SDL), and
  // how many writes failed (SDE). The write RATE is not sent -- the ground
  // station differences SDL between reports exactly as printStatus differences
  // it for logHz -- and neither is the byte count, which tracks the line count
  // and is the field worth least per byte on a link with ~20 to spare.
  //
  // The SD bit already on every packet says the card is MOUNTED. It does not
  // say the flight is being recorded: a card that mounts, opens a file and then
  // stops accepting writes reports SD=1 for the whole flight. A line count that
  // stops moving is what shows that, and until now it existed only on a serial
  // port nobody can reach once the rocket is on the pad.
  //
  // Built into a scratch buffer and copied only IF IT FITS. Formatting straight
  // into the tail would let a long packet -- a five-digit altitude over a
  // full-width GPS fix -- push past the buffer, and what sits at the tail is the
  // health block. Losing SDL for one tick costs nothing. Losing SD/BA/IM costs
  // the operator the fields that say something is wrong, in order to report how
  // many lines got written.
  if (packetNumber % SD_BLOCK_EVERY == 0) {
    char block[40];
    int  n = snprintf(block, sizeof(block), ",SDF=%d,SDL=%lu,SDE=%lu",
                      logFileIndex, logLineCount, sdErrorCount);

    size_t used = strlen(txPacket);

    // n < sizeof(block) rejects a block snprintf itself had to truncate, which
    // would otherwise be appended as a half-written field.
    if (n > 0 && n < (int) sizeof(block) && used + n <= TX_PAYLOAD_MAX) {
      strcpy(txPacket + used, block);
    }
  }

  txPacketLen = strlen(txPacket);
}


static bool startLoRaCopy(int copyNumber) {
  if (LoRa.beginPacket() == 0) {
    txBusyCount++;
    return false;
  }

  txDoneFlag = false;

  LoRa.print(txPacket);
  LoRa.endPacket(true);   // async - returns immediately

  txStartTime = millis();
  return true;
}


// =====================================================
// HAS THE TRANSMISSION FINISHED?
//
// Normally the interrupt answers. The air-time fallback
// guarantees the state machine can never wedge.
// =====================================================

static bool txComplete() {
  if (txDoneFlag) return true;

  if (millis() - txStartTime >= TX_MAX_AIR) {
    txFallbackCount++;
    return true;
  }

  return false;
}


// =====================================================
// REPORT - the packet contents plus how it was sent
// =====================================================

static void reportPacketSent() {
  Serial.print("[TX] air=");
  Serial.print(lastAirTime);
  Serial.print("ms pwr=");
  Serial.print(txPower);
  Serial.print("dBm x");
  Serial.print(txCopies);
  Serial.print(" len=");
  Serial.print(txPacketLen);

  if (showPacket) {
    Serial.print(" | ");
    Serial.print(txPacket);
  }

  Serial.println();
}


// =====================================================
// SERVICE - one step per loop, never waits
//
//   IDLE -> COPY1 -> GAP -> COPY2 -> IDLE
// =====================================================

void serviceTelemetry() {
  if (!radioOK) return;

  // A transmission that never completes must not freeze
  // telemetry for the rest of the flight.
  if (txState == TX_COPY1 || txState == TX_COPY2) {
    if (millis() - txStartTime > TX_MAX_AIR * 4) {
      Serial.println("[LORA] TX TIMEOUT - resetting state");
      txTimeoutCount++;
      txState = TX_IDLE;
      return;
    }
  }

  switch (txState) {

    case TX_IDLE:
      if (millis() - lastSend >= SEND_INTERVAL) {
        lastSend += SEND_INTERVAL;

        if (millis() - lastSend >= SEND_INTERVAL) {
          lastSend = millis();
        }

        packetNumber++;

        // The GPS snapshot used to be refreshed HERE,
        // which meant the 10 Hz log only ever saw 2 Hz
        // data - and saw nothing at all once the radio
        // went down. It now runs in loop(), where it
        // belongs.
        buildTelemetryPacket();

        if (startLoRaCopy(1)) txState = TX_COPY1;
      }
      break;

    case TX_COPY1:
      if (txComplete()) {
        lastAirTime = millis() - txStartTime;

        if (txCopies < 2) {
          txState = TX_IDLE;
          reportPacketSent();
        }
        else {
          txGapStart = millis();
          txState    = TX_GAP;
        }
      }
      break;

    case TX_GAP:
      if (millis() - txGapStart >= COPY_GAP) {
        txState = startLoRaCopy(2) ? TX_COPY2 : TX_IDLE;
      }
      break;

    case TX_COPY2:
      if (txComplete()) {
        txState = TX_IDLE;
        reportPacketSent();
      }
      break;
  }
}
