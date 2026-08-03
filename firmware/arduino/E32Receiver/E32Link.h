// E32Link.h -- portable EBYTE E32 (SX1278, 433 MHz) control for the Arduino IDE.
//
// This is the Arduino-IDE sibling of firmware/lib/E32. It exists because that
// driver is ESP32-only: its begin() calls
//     _s.begin(baud, SERIAL_8N1, rxPin, txPin)
// and the 4-argument HardwareSerial::begin() is an ESP32 extension that does not
// exist on AVR. This header keeps the identical AUX discipline but selects the
// UART at compile time so ONE sketch runs on the Uno bench setup today and the
// ESP32 later, with no edits beyond the pin map.
//
// The one rule, unchanged from lib/E32:
//
//     NEVER write to the module's UART unless AUX is HIGH.
//
// AUX LOW means the module is busy (transmitting, receiving, or waking from a
// mode change). Blind-writing during that window corrupts its buffer and is the
// #1 cause of the module "locking up" (GROUND_STATION_PLAN.md §3). A pure
// receiver only writes during configure(), but the guarantee is kept there too.
//
// Mode pins (EBYTE truth table, mode value = (M1<<1)|M0):
//   0 NORMAL    M1=0 M0=0   transparent TX/RX   <- what a receiver sits in
//   1 WAKEUP    M1=0 M0=1
//   2 POWERSAVE M1=1 M0=0   RX only, low power
//   3 CONFIG    M1=1 M0=1   parameter programming
#pragma once
#include <Arduino.h>

// ---- UART selection -------------------------------------------------------
// Uno/Nano have exactly one hardware UART and it is wired to USB, so the E32
// gets a SoftwareSerial port. Boards with a spare hardware UART use it: software
// serial is half-duplex and cannot receive while it transmits.
#if defined(ARDUINO_ARCH_ESP32)
  #define E32_USES_SOFTSERIAL 0
  typedef HardwareSerial E32Serial;
#elif defined(ARDUINO_ARCH_AVR) && (defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__))
  #define E32_USES_SOFTSERIAL 0
  typedef HardwareSerial E32Serial;
#else
  #define E32_USES_SOFTSERIAL 1
  #include <SoftwareSerial.h>
  typedef SoftwareSerial E32Serial;
#endif

enum E32Mode : uint8_t {
  E32_NORMAL    = 0,
  E32_WAKEUP    = 1,
  E32_POWERSAVE = 2,
  E32_CONFIG    = 3,
};

// SPED byte for the C0 config command (E32-433T20D):
//   [7:6] UART parity : 00 = 8N1
//   [5:3] UART baud   : 011 = 9600
//   [2:0] air rate    : 010 = 2.4k   <- lowest practical rate = longest range
// => 0b00'011'010 = 0x1A. Matches PROTOCOL.md and lib/E32's E32_SPED_9600_2K4.
static const uint8_t E32_SPED_9600_2K4 = 0x1A;

// OPTION byte: transparent transmission, FEC on, max TX power.
// NOTE: power/option bits differ across E32 variants (T20D vs T30D vs T30S).
// Verify against YOUR module's datasheet before a range test.
static const uint8_t E32_OPTION_DEFAULT = 0xC4;  // 0b1100'0100

// Both radios must agree on these or they will never hear each other.
static const uint8_t E32_CHANNEL_DEFAULT = 0x17;

class E32Link {
 public:
  E32Link(E32Serial& serial, int pinM0, int pinM1, int pinAux)
      : _s(serial), _m0(pinM0), _m1(pinM1), _aux(pinAux) {}

  // Configure control pins, open the UART, and enter NORMAL mode.
  //
  // `rxPin`/`txPin` are used ONLY on the ESP32, whose HardwareSerial::begin()
  // can remap a UART onto arbitrary GPIOs. They are ignored on AVR, where the
  // pins are fixed by the hardware (Mega Serial1) or were already fixed by the
  // SoftwareSerial constructor (Uno/Nano). They are arguments rather than
  // #defines because the Arduino IDE compiles this .cpp as its own translation
  // unit -- it never sees macros defined in the .ino.
  void begin(uint32_t uartBaud = 9600, int rxPin = -1, int txPin = -1);

  // True once AUX reads HIGH; false if it never does within `timeoutMs`.
  // A false return means the radio is busy -- callers MUST NOT write.
  bool waitAux(uint32_t timeoutMs = 1000);

  bool auxHigh() const { return digitalRead(_aux) == HIGH; }

  // Drive M0/M1, then wait for AUX to settle (the module pulses AUX on a mode
  // change).
  void setMode(E32Mode mode);

  // Wait for AUX HIGH, then write `len` bytes. Returns false (writes NOTHING)
  // if AUX never goes HIGH within `timeoutMs` -- the anti-lock-up guarantee.
  bool writeFrame(const uint8_t* data, size_t len, uint32_t timeoutMs = 500);

  // One-shot: enter CONFIG mode, save air-rate/UART params (C0), verify the
  // module echoes them back, then return to NORMAL. True on a verified match.
  // Run once per module -- both ends of the link must share these parameters.
  bool configure(uint8_t addrHigh = 0x00, uint8_t addrLow = 0x00,
                 uint8_t channel = E32_CHANNEL_DEFAULT,
                 uint8_t sped = E32_SPED_9600_2K4,
                 uint8_t option = E32_OPTION_DEFAULT);

  int available() { return _s.available(); }
  int read()      { return _s.read(); }

 private:
  E32Serial& _s;
  int _m0, _m1, _aux;
};
