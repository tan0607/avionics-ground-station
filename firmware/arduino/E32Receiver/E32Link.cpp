#include "E32Link.h"

// After any mode change the module re-initialises and pulses AUX; the datasheet
// asks for a short settle window once AUX is HIGH again before trusting the UART.
static const uint32_t AUX_SETTLE_MS = 3;

void E32Link::begin(uint32_t uartBaud, int rxPin, int txPin) {
  pinMode(_m0, OUTPUT);
  pinMode(_m1, OUTPUT);
  // AUX is an open-drain-ish status line; the community fix for lock-ups is a
  // pull-up (external 4.7k preferred, INPUT_PULLUP as a fallback).
  pinMode(_aux, INPUT_PULLUP);

#if defined(ARDUINO_ARCH_ESP32)
  // Only the ESP32 can remap a UART onto arbitrary GPIOs.
  _s.begin(uartBaud, SERIAL_8N1, rxPin, txPin);
#else
  // Uno/Nano: SoftwareSerial took its pins in the constructor.
  // Mega: Serial1 is fixed to pins 19 (RX) / 18 (TX).
  (void)rxPin; (void)txPin;
  _s.begin(uartBaud);
#endif

  setMode(E32_NORMAL);
}

bool E32Link::waitAux(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(_aux) == LOW) {
    if (millis() - start >= timeoutMs) return false;  // busy -> caller must not write
    delay(1);
  }
  return true;
}

void E32Link::setMode(E32Mode mode) {
  // mode value = (M1<<1)|M0  (EBYTE truth table)
  digitalWrite(_m0, (mode & 0x01) ? HIGH : LOW);
  digitalWrite(_m1, (mode & 0x02) ? HIGH : LOW);
  // Give the module a moment to notice the pin change, then wait for it to idle.
  delay(AUX_SETTLE_MS);
  waitAux(1000);
  delay(AUX_SETTLE_MS);
}

bool E32Link::writeFrame(const uint8_t* data, size_t len, uint32_t timeoutMs) {
  if (!waitAux(timeoutMs)) return false;  // <-- never blind-write
  _s.write(data, len);
  return true;
}

bool E32Link::configure(uint8_t addrHigh, uint8_t addrLow, uint8_t channel,
                        uint8_t sped, uint8_t option) {
  setMode(E32_CONFIG);               // M0=1 M1=1: params only writable in mode 3
  while (_s.available()) _s.read();  // flush any stale bytes

  // C0 = "set parameters and save through power-down" (C2 = volatile).
  const uint8_t cmd[6] = {0xC0, addrHigh, addrLow, sped, channel, option};
  if (!waitAux(1000)) { setMode(E32_NORMAL); return false; }
  _s.write(cmd, sizeof(cmd));
  _s.flush();
  waitAux(1000);

  // The module echoes the saved parameters back as C0 + the same 5 bytes.
  uint8_t resp[6] = {0};
  uint32_t start = millis();
  size_t got = 0;
  while (got < sizeof(resp) && millis() - start < 1000) {
    if (_s.available()) resp[got++] = (uint8_t)_s.read();
  }

  setMode(E32_NORMAL);               // back to transparent RX
  // Verify the three parameters we care about round-tripped. resp[0] is C0 or C1.
  return (got == sizeof(resp)) &&
         (resp[3] == sped) && (resp[4] == channel) && (resp[5] == option);
}
