#pragma once
// Host test include path only. No serial device or GPIO is accessed.
#include <cmath>
#include <cstdint>

#define PI 3.1415926535897932384626433832795
#define RAD_TO_DEG 57.2957795130823208768
#define DEG_TO_RAD 0.01745329251994329577
#define LOW 0
#define HIGH 1
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2

extern unsigned long hostNowMs;
inline unsigned long millis() { return hostNowMs; }
inline unsigned long micros() { return hostNowMs * 1000UL; }
void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);
int analogReadMilliVolts(int pin);

// Structured state/edge events, rather than serial messages, are the oracle.
struct HostSerial {
  template <typename T> void print(T) {}
  template <typename T> void print(T, int) {}
  void println() {}
  template <typename T> void println(T) {}
  template <typename T> void println(T, int) {}
};
extern HostSerial Serial;
