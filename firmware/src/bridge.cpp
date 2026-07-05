// bridge.cpp -- GROUND-STATION bridge (Target 1).
//
// Role: sit between the receiving E32 and the laptop and be as DUMB as possible.
//   E32 UART  --(raw bytes)-->  ESP32  --(USB serial)-->  laptop (Python backend)
//
// It does exactly three things:
//   1. Hold the E32 in NORMAL mode and respect AUX (via the E32 driver).
//   2. Forward every byte the E32 emits straight to USB serial, untouched.
//   3. Blink an LED when a packet SYNC header (0xAA 0x55) goes by.
//
// It deliberately does NOT checksum, parse, or reframe. CRC + struct decoding live
// on the laptop (shared/protocol/packet.py) so there is ONE parser and the bridge
// can never silently drop a byte the backend's raw.log should have seen.
// "Raw first, then parse" starts here: the bytes leave this board unmodified.
#include <Arduino.h>
#include "E32.h"

// ---- pin map (ESP32 DevKitC / WROOM) --------------------------------------
static const int PIN_E32_M0  = 25;
static const int PIN_E32_M1  = 26;
static const int PIN_E32_AUX = 27;
static const int PIN_E32_RX  = 16;  // ESP32 RX2  <- E32 TXD
static const int PIN_E32_TX  = 17;  // ESP32 TX2  -> E32 RXD (level-safe: E32 is 3.3V)
static const int PIN_LED     = 2;   // onboard LED on most DevKitC boards

// ---- link parameters ------------------------------------------------------
static const uint32_t E32_UART_BAUD  = 9600;    // must match the E32's configured UART baud
static const uint32_t BRIDGE_USB_BAUD = 115200; // laptop opens the port at THIS baud

// Set to 1, flash once to program the E32 to 9600 UART / 2.4k air rate, then set
// back to 0. Both E32s (this one + the airborne one) must share these params.
#define E32_RUN_CONFIG 0

E32 radio(Serial2, PIN_E32_M0, PIN_E32_M1, PIN_E32_AUX);

// Non-blocking LED pulse: lit for LED_PULSE_MS after each SYNC match.
static const uint32_t LED_PULSE_MS = 40;
static uint32_t led_off_at = 0;
static uint8_t  prev_byte  = 0;

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Serial.begin(BRIDGE_USB_BAUD);           // USB link to the laptop
  radio.begin(PIN_E32_RX, PIN_E32_TX, E32_UART_BAUD);  // opens Serial2, enters NORMAL

#if E32_RUN_CONFIG
  bool ok = radio.configure();             // 9600 UART, 2.4k air rate (see E32.h)
  // A single visible heartbeat tells us whether the config verified, without needing
  // the USB console (handy in the field). 3 blinks = OK, fast flutter = failed.
  for (int i = 0; i < (ok ? 3 : 12); i++) {
    digitalWrite(PIN_LED, HIGH); delay(ok ? 150 : 60);
    digitalWrite(PIN_LED, LOW);  delay(ok ? 150 : 60);
  }
#endif
}

void loop() {
  // Drain the E32 -> USB as fast as bytes arrive. Forward first, inspect second, so
  // forwarding is never delayed by our LED bookkeeping.
  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    Serial.write(b);                       // raw passthrough -- do not touch the byte

    if (prev_byte == 0xAA && b == 0x55) {  // SYNC header seen -> a frame is passing
      digitalWrite(PIN_LED, HIGH);
      led_off_at = millis() + LED_PULSE_MS;
    }
    prev_byte = b;
  }

  if (led_off_at && (int32_t)(millis() - led_off_at) >= 0) {
    digitalWrite(PIN_LED, LOW);
    led_off_at = 0;
  }

  // Phase-2 uplink seam: when C2 command frames are added, read Serial (USB) here and
  // radio.writeFrame(...) them to the E32 -- writeFrame() already enforces AUX HIGH.
}
