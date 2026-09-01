// LoRaLink.h -- the radio settings both ends of the link must agree on.
//
// Hardware: a BARE SX1278 module (Ra-01 / Ra-02 / RFM95-style), driven over SPI
// with sandeepmistry/LoRa. Not an EBYTE E32 -- there is no wrapper MCU, no UART,
// no M0/M1/AUX, and no CONFIG mode. The old lib/E32 driver existed entirely to
// tame that wrapper; none of it is needed here.
//
// WHY THIS FILE EXISTS: two LoRa radios only hear each other if frequency,
// spreading factor, bandwidth, coding rate and sync word all match. That is the
// same class of bug the E32's channel/air-rate mismatch was, and the fix is to
// give it exactly one home. bridge.cpp and onboard_tx.cpp both call
// loraLinkBegin() and neither one names a parameter itself, so they cannot drift.
//
// AIR TIME, which is the constraint that picked these numbers:
//   32-byte payload, SF7 / BW 125 kHz / CR 4-5, explicit header, 8-sym preamble
//   -> 58 payload symbols x 1.024 ms + 12.5 ms preamble = ~72 ms on air.
// The downlink runs at 4 Hz (250 ms), so that is ~29% duty -- comfortable. Going
// to SF9 for range costs ~247 ms per frame and would not fit the window at all.
// At 17 dBm into ~-123 dBm sensitivity there is still ~40 dB of margin at 5 km,
// so SF7 is not the limiting factor for a model rocket.
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

// --- per-vehicle channel ---------------------------------------------------
// TWO ROCKETS ON ONE FIELD SHARE THE AIR. LoRa does not pair; a receiver decodes
// every packet whose frequency/SF/BW/CR/syncword match, no matter which vehicle
// sent it. Two rockets on one channel therefore give you both failures at once:
// each ground station decodes the OTHER rocket's frames (PKT jumps, so loss rate
// becomes noise and the map hops between vehicles), and the two transmitters
// collide on air so neither link survives. An MRCC frame is ~190 bytes, which at
// SF7/BW125/CR4-5 is ~400 ms on air -- one vehicle already fills the channel.
//
// So the channel comes from the BUILD, not from an edit here: platformio.ini
// defines one env per vehicle and the env is the vehicle's identity. Two boards
// that differ only by a hand-edited constant are the easiest thing in this
// project to flash wrong, and a wrong flash is invisible until both rockets are
// powered at the same time -- which is the pad, not the bench.
//
//   pio run -e onboard_tx_a -t upload      # rocket A, 433.3 MHz
//   pio run -e onboard_tx_b -t upload      # rocket B, 434.1 MHz
//
// The default below is rocket A's channel, so an unflagged build is still legal
// RF rather than 0 Hz -- but ALWAYS name the env.
#ifndef LORA_CHANNEL_HZ
#  define LORA_CHANNEL_HZ 433300000
#endif

// Both ends. Change here, reflash BOTH boards, never one.
static const long    LORA_FREQ_HZ    = (long)(LORA_CHANNEL_HZ);
static const int     LORA_SF         = 7;       // 7..12; higher = more range, more air time
static const long    LORA_BW_HZ      = 125E3;
static const int     LORA_CR_DENOM   = 5;       // coding rate 4/5
static const uint8_t LORA_SYNCWORD   = 0x12;    // private-network default; not LoRaWAN's 0x34
static const int     LORA_TX_DBM     = 17;      // PA_BOOST pin, 2..17

// Bring up the radio on the given pins and apply the settings above.
// Returns false if the SX1278 does not answer over SPI -- which on this module
// means wiring or power, never RF.
//
// SPI.begin() is called with explicit pins FIRST: LoRa.begin() internally calls
// SPI.begin() with no arguments, and arduino-esp32's SPIClass::begin() returns
// early once the bus is already up. So claiming the pins here is what makes a
// non-default pin map stick.
inline bool loraLinkBegin(int nss, int rst, int dio0, int sck, int miso, int mosi) {
  SPI.begin(sck, miso, mosi, nss);
  LoRa.setPins(nss, rst, dio0);
  if (!LoRa.begin(LORA_FREQ_HZ)) return false;

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW_HZ);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.setTxPower(LORA_TX_DBM, PA_OUTPUT_PA_BOOST_PIN);

  // Hardware CRC stays OFF, deliberately. The payload already carries its own
  // CRC-16/CCITT (PROTOCOL.md bytes 30-31) and the ground station counts CRC
  // failures to tell "RF arriving but corrupt" apart from "no RF at all".
  // LoRa's hardware CRC drops a bad packet inside parsePacket(), so enabling it
  // would erase exactly that evidence before raw.log ever sees it -- and leave a
  // marginal link looking identical to a dead one. One CRC, checked on the laptop.
  LoRa.disableCrc();
  return true;
}
