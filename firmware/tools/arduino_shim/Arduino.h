#pragma once
// =====================================================
// MINIMAL ARDUINO SHIM - HOST BUILDS ONLY
//
// Just enough of Arduino.h to compile src/Filters.cpp
// on a laptop. This is NOT used by the flight build -
// the ESP32 toolchain has the real header.
//
// Its whole purpose is that the before/after graphs in
// the report come out of the SAME C++ that flies, not
// out of a Python re-implementation that might quietly
// disagree with it.
//
// millis() and micros() are driven by the replayed log
// timestamps, not by the wall clock.
// =====================================================

#include <cstdint>
#include <cstdio>
#include <cmath>

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

#define RAD_TO_DEG 57.295779513082320876798154814105
#define DEG_TO_RAD 0.017453292519943295769236907684886

extern unsigned long g_replayMicros;

inline unsigned long micros() { return g_replayMicros; }
inline unsigned long millis() { return g_replayMicros / 1000UL; }

// Everything the firmware prints goes to stderr so that
// stdout stays a clean CSV that can be piped.
struct HostSerial {
  void print(const char* s)            { fprintf(stderr, "%s", s); }
  void print(char c)                   { fprintf(stderr, "%c", c); }
  void print(int v)                    { fprintf(stderr, "%d", v); }
  void print(unsigned int v)           { fprintf(stderr, "%u", v); }
  void print(long v)                   { fprintf(stderr, "%ld", v); }
  void print(unsigned long v)          { fprintf(stderr, "%lu", v); }
  void print(double v, int digits = 2) { fprintf(stderr, "%.*f", digits, v); }

  void println()                       { fprintf(stderr, "\n"); }
  void println(const char* s)          { fprintf(stderr, "%s\n", s); }
  void println(int v)                  { fprintf(stderr, "%d\n", v); }
  void println(unsigned long v)        { fprintf(stderr, "%lu\n", v); }
  void println(double v, int d = 2)    { fprintf(stderr, "%.*f\n", d, v); }
};

extern HostSerial Serial;
