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
#include <math.h>
#include <string.h>

int  txPower    = TX_POWER_DEFAULT;
int  txCopies   = TX_COPIES_DEFAULT;
bool showPacket = true;

unsigned long lastAirTime     = 0;
unsigned long txBusyCount     = 0;
unsigned long txTimeoutCount  = 0;
unsigned long txFallbackCount = 0;

// 255 is LoRa's hard payload limit. The binary packet is 52-67 bytes, so this
// is nowhere near full - it stays sized to the limit because the buffer is what
// every optional block is bounds-checked against, and a buffer sized to the
// measured packet would have to be re-checked every time a field is added.
//
// BYTES, not a string. There is no NUL terminator and there cannot be one: the
// payload is binary and 0x00 is a legal value in the middle of it. Anything
// that treats this as a C string reads a truncated packet - which is why the
// transmit path below uses LoRa.write() with an explicit length and the console
// dumps hex rather than printing it.
uint8_t txPacket[256];
int     txPacketLen = 0;

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
static int  tlmReportCountdown = 0;
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
// PACKET CODEC
//
// The wire format is specified in Config.h. Every offset
// and scale factor below has its rationale there, not
// here - this is the encoder for it.
//
// The region between the CODEC markers is compiled
// VERBATIM by firmware/tests/test_downlink_codec.py,
// alongside the decoder lifted out of the ground station
// sketch, so the round trip is tested against the real
// code on both sides rather than against a Python model
// of it. Keep the markers.
// =====================================================

// ---- TLM CODEC BEGIN ----

// Little-endian, byte at a time. Both ends are ESP32s and both are
// little-endian, so a memcpy of a packed struct would work today - and would
// make the wire format depend on a property of the compiler that nothing
// states, checks or would notice changing.
static inline void tlmPutU8(uint8_t *b, int &i, uint8_t v) {
  b[i++] = v;
}
static inline void tlmPutU16(uint8_t *b, int &i, uint16_t v) {
  b[i++] = (uint8_t)(v & 0xFF);
  b[i++] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void tlmPutU32(uint8_t *b, int &i, uint32_t v) {
  for (int k = 0; k < 4; k++) b[i++] = (uint8_t)((v >> (8 * k)) & 0xFF);
}
static inline void tlmPutI16(uint8_t *b, int &i, int16_t v) {
  tlmPutU16(b, i, (uint16_t) v);
}
static inline void tlmPutI32(uint8_t *b, int &i, int32_t v) {
  tlmPutU32(b, i, (uint32_t) v);
}
static inline void tlmPutF32(uint8_t *b, int &i, float v) {
  uint32_t bits;
  memcpy(&bits, &v, 4);          // the only portable float -> bits move
  tlmPutU32(b, i, bits);
}

// Scaled integer fields. Two things have to be true of every one of these and
// neither is automatic:
//
//   A non-finite input must not be cast. (int16_t) NAN is undefined behaviour,
//   and what it produces in practice is a number - so a dead sensor would
//   arrive as a plausible reading instead of as nan. The sentinel is what keeps
//   `nan` on the ground station's line, which is what the ASCII packet did.
//
//   A finite input out of range must SATURATE, not wrap. An accelerometer
//   glitch at 400 m/s2 wrapping through int16 is a large NEGATIVE acceleration,
//   which is a plausible reading pointing the wrong way. Saturation is
//   obviously pinned; a wrap is not obviously anything.
static int16_t tlmQ16(float v, float scale) {
  if (!isfinite(v)) return TLM_NAN_I16;
  const float s = v * scale;
  if (s >=  32767.0f) return  32767;
  if (s <= -32767.0f) return -32767;
  return (int16_t) lroundf(s);
}

static int32_t tlmQ32(double v, double scale) {
  if (!isfinite(v)) return TLM_NAN_I32;
  const double s = v * scale;
  if (s >=  2147483647.0) return  2147483647;
  if (s <= -2147483647.0) return -2147483647;
  return (int32_t) llround(s);
}

// Angles, wrapped into 0..359 rather than saturated - a heading is modular, so
// 361 degrees is 1 degree and clamping it to 359 would be a real error where
// wrapping is none.
static uint16_t tlmQDeg(float v) {
  if (!isfinite(v)) return TLM_NAN_U16;
  float w = fmodf(v, 360.0f);
  if (w < 0.0f) w += 360.0f;
  return (uint16_t)(((uint16_t) lroundf(w)) % 360);
}

// ---- TLM CODEC END ----


static void buildTelemetryPacket() {
  // Flight fields go FIRST, in the fixed-length base block. The ordering
  // argument that put them first in the ASCII packet was about truncation, and
  // truncation is no longer the risk it was - the base block is fixed at
  // TLM_BASE_LEN and either arrives whole or fails the radio's CRC and never
  // arrives at all. What survives from that argument is the part that still
  // holds: the OPTIONAL blocks come last, so a block that does not fit is
  // dropped without touching a flight field.
  //
  // VX/VY are still absent - they are computed FROM gpsSpeed and gpsCourse,
  // which are both here, so on the air they were always redundancy. The card
  // still logs them at full precision.
  //
  // SD / BA / IM are the subsystem health bits, in the flags byte. There is
  // deliberately no LORA bit: serviceTelemetry() returns early when the radio
  // is down, so the field could only ever be 1 in a packet that arrived.
  // Silence is the honest signal there, and the ground station reads it.
  int i = 0;

  tlmPutU8 (txPacket, i, TLM_MAGIC);
  tlmPutU8 (txPacket, i, TLM_VERSION);
  tlmPutU32(txPacket, i, (uint32_t) packetNumber);
  tlmPutU32(txPacket, i, (uint32_t) millis());
  tlmPutU8 (txPacket, i, flightState);

  uint8_t flags = 0;
  if (pyroArmed) flags |= TLM_FLAG_ARMED;
  if (pyroFired) flags |= TLM_FLAG_FIRED;
  if (gpsData)   flags |= TLM_FLAG_GPS_DATA;
  if (gpsFix)    flags |= TLM_FLAG_GPS_FIX;
  if (sdOK)      flags |= TLM_FLAG_SD_OK;
  if (baroOK)    flags |= TLM_FLAG_BARO_OK;
  if (imuOK)     flags |= TLM_FLAG_IMU_OK;
  tlmPutU8(txPacket, i, flags);

  tlmPutU8(txPacket, i, (uint8_t)(satellites < 0   ? 0
                                : satellites > 255 ? 255
                                : satellites));

  // Which optional blocks follow. Written as a placeholder and patched once
  // both appends have had their say - the alternative is deciding twice, in
  // two places, whether a block fits, which is how the two disagree.
  const int blocksAt = i;
  tlmPutU8(txPacket, i, 0);

  // f32, not scaled. See the WIRE FORMAT note in Config.h: these two have no
  // provable ceiling and a wrapped altitude is an apogee reported as a hole in
  // the ground.
  tlmPutF32(txPacket, i, altFiltered);
  tlmPutF32(txPacket, i, maxAlt);

  tlmPutI16(txPacket, i, tlmQ16(vertVel, 10.0f));
  tlmPutI32(txPacket, i, tlmQ32(latitude,  100000.0));
  tlmPutI32(txPacket, i, tlmQ32(longitude, 100000.0));
  tlmPutI16(txPacket, i, tlmQ16(gpsAltitude, 10.0f));
  tlmPutI16(txPacket, i, tlmQ16(gpsSpeed,    10.0f));
  tlmPutU16(txPacket, i, tlmQDeg(gpsCourse));

  // R on the console swaps these six between raw and filtered. Same fields,
  // same length - the card is still logging both either way.
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? fax : ax, 100.0f));
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? fay : ay, 100.0f));
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? faz : az, 100.0f));
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? fgx : gx, 1.0f));
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? fgy : gy, 1.0f));
  tlmPutI16(txPacket, i, tlmQ16(txFiltered ? fgz : gz, 1.0f));
  tlmPutU16(txPacket, i, tlmQDeg(txFiltered ? headingFilt : heading));

  // The base block is a fixed size and Config.h says what it is. If those two
  // ever disagree the decoder reads every field at the wrong offset and
  // reports confident nonsense, so make it a build error instead.
  static_assert(TLM_BASE_LEN == 52, "TLM_BASE_LEN disagrees with the encoder");

  uint8_t blocks = 0;

  // ---- prelaunch readiness, PAD only ----
  //
  // AW=blocker bits, AD=boot-delay seconds, AS=observed-stillness seconds.
  // Ceil remaining time; AW=0 is NOT an arming acknowledgement (ST/AR are).
  // All-or-none: appended whole or not at all.
  if (flightState == FS_PAD && i + TLM_ARM_LEN <= TX_PAYLOAD_MAX) {
    const ArmReadiness ready = armReadiness();
    const unsigned long delaySec = (ready.delayRemainingMs + 999) / 1000;
    const unsigned long stillSec = (ready.stillRemainingMs + 999) / 1000;

    tlmPutU8 (txPacket, i, ready.wait);
    tlmPutU16(txPacket, i, (uint16_t)(delaySec > 65535 ? 65535 : delaySec));
    tlmPutU16(txPacket, i, (uint16_t)(stillSec > 65535 ? 65535 : stillSec));
    blocks |= TLM_BLOCK_ARM;
  }

  // ---- recorder block, one packet in SD_BLOCK_EVERY ----
  //
  // What printStatus's [SD] line says, minus the parts the ground can work out
  // for itself: which file is open (SDF), how many lines are in it (SDL), and
  // how many writes failed (SDE). The write RATE is not sent - the ground
  // station differences SDL between reports exactly as printStatus differences
  // it for logHz.
  //
  // The SD bit in the flags byte says the card is MOUNTED. It does not say the
  // flight is being recorded: a card that mounts, opens a file and then stops
  // accepting writes reports SD=1 for the whole flight. A line count that stops
  // moving is what shows that, and without this block it exists only on a
  // serial port nobody can reach once the rocket is on the pad.
  if (packetNumber % SD_BLOCK_EVERY == 0 && i + TLM_SD_LEN <= TX_PAYLOAD_MAX) {
    tlmPutI16(txPacket, i, (int16_t) logFileIndex);
    tlmPutU32(txPacket, i, (uint32_t) logLineCount);
    tlmPutU32(txPacket, i, (uint32_t) sdErrorCount);
    blocks |= TLM_BLOCK_SD;
  }

  txPacket[blocksAt] = blocks;
  txPacketLen = i;
}


static bool startLoRaCopy(int copyNumber) {
  if (LoRa.beginPacket() == 0) {
    txBusyCount++;
    return false;
  }

  txDoneFlag = false;

  // write(), not print(). print() would stop at the first 0x00, and a binary
  // packet is full of them - a zero altitude, a zero gyro rate, the high byte
  // of almost every small number. The length is explicit for the same reason.
  LoRa.write(txPacket, (size_t) txPacketLen);
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
  // THROTTLED. At 2 Hz a line per packet was two lines a second; at 10 Hz it is
  // ten, and a console scrolling that fast is a console nobody reads - which
  // costs you the [BARO] and [PYRO] lines that actually need looking at. One
  // line per TX_REPORT_EVERY packets, and `air=` is still measured on every
  // packet whether or not this prints it.
  if (--tlmReportCountdown > 0) return;
  tlmReportCountdown = (int) TX_REPORT_EVERY;

  Serial.print("[TX] air=");
  Serial.print(lastAirTime);
  Serial.print("ms pwr=");
  Serial.print(txPower);
  Serial.print("dBm x");
  Serial.print(txCopies);
  Serial.print(" len=");
  Serial.print(txPacketLen);

  // Hex, because the packet is bytes now. The readable form of this data is on
  // the ground station's console, where the decoder puts it back into the
  // MRCC,PKT=... line - this end has printStatus() for the same numbers, so
  // what is wanted here is the literal thing that went on the air.
  if (showPacket) {
    Serial.print(" | ");
    for (int k = 0; k < txPacketLen; k++) {
      if (txPacket[k] < 0x10) Serial.print('0');
      Serial.print(txPacket[k], HEX);
    }
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
