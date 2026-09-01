// bridge.cpp -- GROUND-STATION bridge (Target 1).
//
// Role: sit between the receiving SX1278 and the laptop and be as DUMB as possible.
//   SX1278  --(SPI)-->  ESP32  --(USB serial)-->  laptop (Python backend)
//
// It does exactly two things:
//   1. Bring the radio up on the settings in lib/LoRaLink.
//   2. Hand every received packet's bytes straight to USB serial, untouched,
//      and blink an LED per packet.
//
// It deliberately does NOT checksum, parse, or reframe. CRC + struct decoding live
// on the laptop (shared/protocol/packet.py) so there is ONE parser and the bridge
// can never silently drop a byte the backend's raw.log should have seen.
// "Raw first, then parse" starts here: the bytes leave this board unmodified.
//
// WHAT THE SX1278 REMOVED, versus the EBYTE E32 this used to talk to: LoRa hands
// us whole packets with their boundaries already known, so there is no byte
// stream to resynchronise and nothing to sniff for a SYNC header -- the LED now
// pulses on a real packet rather than on a byte pair that merely looked like one.
// Gone with it: M0/M1/AUX, the AUX-before-every-write rule, CONFIG mode, the C0/C1
// parameter dance, and the second baud rate. This file is the whole radio path.
//
// USB BAUD IS 115200, and the backend must be told so:
//   backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX
// (That is now the backend's default. Opening a 115200 stream at 9600 yields
// pure garbage, which reads exactly like a dead radio.)
#include <Arduino.h>
#include "LoRaLink.h"

// ---- pin map (ESP32 DevKitC / WROOM + Ra-02) -------------------------------
// The VSPI defaults, so this is the wiring every Ra-02/ESP32 guide already shows.
static const int PIN_LORA_NSS  = 5;
static const int PIN_LORA_RST  = 14;
static const int PIN_LORA_DIO0 = 26;
static const int PIN_LORA_SCK  = 18;
static const int PIN_LORA_MISO = 19;
static const int PIN_LORA_MOSI = 23;
static const int PIN_LED       = 2;   // onboard LED on most DevKitC boards

static const uint32_t BRIDGE_USB_BAUD = 115200;  // laptop opens the port at THIS baud

// Non-blocking LED pulse: lit for LED_PULSE_MS after each received packet.
static const uint32_t LED_PULSE_MS = 40;
static uint32_t led_off_at = 0;

// Set once at boot. The bridge emits ONLY radio bytes -- a diagnostic banner on
// this port would land in raw.log and pollute the one file that is supposed to be
// a faithful record of what came off the air. So a dead radio is reported the only
// way that costs nothing: the LED flutters instead of pulsing. Fluttering at boot
// means the SX1278 did not answer over SPI -- wiring or power, never RF.
static bool radio_up = false;

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Serial.begin(BRIDGE_USB_BAUD);           // USB link to the laptop

  radio_up = loraLinkBegin(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0,
                           PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
}

void loop() {
  if (!radio_up) {                         // fast flutter = SPI wiring / power
    digitalWrite(PIN_LED, HIGH); delay(60);
    digitalWrite(PIN_LED, LOW);  delay(60);
    return;
  }

  // parsePacket() returns the payload length of one complete received packet, or
  // 0 if none is waiting. The radio already found the packet boundary for us.
  int len = LoRa.parsePacket();
  if (len > 0) {
    while (LoRa.available()) {
      Serial.write((uint8_t)LoRa.read());  // raw passthrough -- do not touch the byte
    }
    digitalWrite(PIN_LED, HIGH);
    led_off_at = millis() + LED_PULSE_MS;
  }

  if (led_off_at && (int32_t)(millis() - led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    led_off_at = 0;
  }

  // Two seams left open on purpose, both cheap now and neither worth adding blind:
  //   RSSI/SNR -- LoRa.packetRssi() / packetSnr() are valid right after the read
  //   above. The E32 could never report them (GROUND_STATION_PLAN.md §3 says as
  //   much). Surfacing them needs a protocol slot + backend + dashboard, so it is
  //   a feature, not part of this swap.
  //   Uplink -- LoRa.beginPacket()/write()/endPacket() when C2 command frames land.
}
