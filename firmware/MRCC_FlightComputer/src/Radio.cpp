#include "Radio.h"
#include "Config.h"
#include "State.h"
#include "Sensors.h"
#include "Flight.h"
#include "Pyro.h"
#include "Filters.h"
#include <SPI.h>
#include <LoRa.h>

int  txPower    = TX_POWER_DEFAULT;
int  txCopies   = TX_COPIES_DEFAULT;
bool showPacket = true;

unsigned long lastAirTime     = 0;
unsigned long txBusyCount     = 0;
unsigned long txTimeoutCount  = 0;
unsigned long txFallbackCount = 0;

char txPacket[250];
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
  // Worst case measured at 231 bytes. The buffer is 250
  // and LoRa's own hard limit is 255 - with VX/VY still
  // in, worst case was 251 and the last fields would
  // have been silently truncated on the ground.
  snprintf(
    txPacket, sizeof(txPacket),
    "MRCC,PKT=%lu,T=%.1f,ST=%s,AL=%.1f,VZ=%.1f,MX=%.1f,AR=%d,FI=%d,"
    "GD=%d,GF=%d,SAT=%d,"
    "LAT=%.5f,LON=%.5f,GA=%.1f,GS=%.1f,CRS=%.0f,"
    "AX=%.2f,AY=%.2f,AZ=%.2f,GX=%.0f,GY=%.0f,GZ=%.0f,"
    "HDG=%.0f,SD=%d,BA=%d",
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
    sdOK ? 1 : 0, baroOK ? 1 : 0
  );

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
