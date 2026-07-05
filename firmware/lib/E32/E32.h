// E32.h -- minimal EBYTE E32 (SX1278, 433 MHz) driver for ESP32.
//
// Shared by BOTH firmware targets (bridge + onboard TX). The whole point of this
// class is to enforce the one rule the E32 community keeps re-learning the hard way:
//
//     NEVER write to the module's UART unless AUX is HIGH.
//
// AUX LOW means the module is busy (transmitting, receiving, or waking from a mode
// change). Blind-writing during that window corrupts the buffer and is the #1 cause
// of the module "locking up" (see GROUND_STATION_PLAN.md §3). Every write in this
// driver goes through waitAux() first, with a *timeout* so a disconnected AUX can
// never hard-stall a flight loop.
//
// Mode pins (EBYTE truth table, mode value = (M1<<1)|M0):
//   0 NORMAL   M1=0 M0=0   transparent TX/RX  (flight use)
//   1 WAKEUP   M1=0 M0=1   adds wake-up preamble
//   2 POWERSAVE M1=1 M0=0  RX only, low power
//   3 CONFIG   M1=1 M0=1   sleep / AT-style parameter programming
//
// This driver is intentionally small: framing/CRC live in TelemPacket (onboard TX)
// and all parsing lives on the laptop (bridge stays thin).
#pragma once
#include <Arduino.h>

// EBYTE operating modes.
enum E32Mode : uint8_t {
  E32_NORMAL    = 0,
  E32_WAKEUP    = 1,
  E32_POWERSAVE = 2,
  E32_CONFIG    = 3,
};

// SPED byte for the C0 config command. Bit layout (EBYTE E32-433T20D):
//   [7:6] UART parity : 00 = 8N1
//   [5:3] UART baud   : 011 = 9600
//   [2:0] air rate    : 010 = 2.4k   <- lowest practical rate = longest range
// => 0b00'011'010 = 0x1A. This matches PROTOCOL.md ("air-rate 2.4k, 9600 UART").
static const uint8_t E32_SPED_9600_2K4 = 0x1A;

// OPTION byte (E32-433T20D): transparent transmission, FEC on, max TX power.
//   NOTE: power/option bits differ across E32 variants (T20D vs T30D vs T30S).
//   Verify this value against YOUR module's datasheet before a range test.
static const uint8_t E32_OPTION_DEFAULT = 0xC4;  // 0b1100'0100

class E32 {
 public:
  // `serial` is a HardwareSerial wired to the module (e.g. Serial2). The RX/TX
  // GPIOs are passed to begin() so this class owns the UART setup.
  E32(HardwareSerial& serial, int pinM0, int pinM1, int pinAux)
      : _s(serial), _m0(pinM0), _m1(pinM1), _aux(pinAux) {}

  // Configure control pins, open the UART at `uartBaud`, and enter NORMAL mode.
  void begin(int rxPin, int txPin, uint32_t uartBaud = 9600);

  // True once AUX reads HIGH; false if it never does within `timeoutMs`.
  // A false return means the radio is busy -- callers MUST NOT write.
  bool waitAux(uint32_t timeoutMs = 1000);

  bool auxHigh() const { return digitalRead(_aux) == HIGH; }

  // Drive M0/M1, then wait for AUX to settle (module pulses AUX on a mode change).
  void setMode(E32Mode mode);

  // Wait for AUX HIGH, then write `len` bytes. Returns false (writes nothing) if
  // AUX never goes HIGH within `timeoutMs` -- the anti-lock-up guarantee.
  bool writeFrame(const uint8_t* data, size_t len, uint32_t timeoutMs = 500);

  // One-shot: enter CONFIG mode, save air-rate/UART params (C0 command), verify the
  // module echoes them back, then return to NORMAL. Returns true on a verified match.
  // Call once at boot (or from a dedicated config sketch) -- not in the flight loop.
  bool configure(uint8_t addrHigh = 0x00, uint8_t addrLow = 0x00, uint8_t channel = 0x17,
                 uint8_t sped = E32_SPED_9600_2K4, uint8_t option = E32_OPTION_DEFAULT);

 private:
  HardwareSerial& _s;
  int _m0, _m1, _aux;
};
