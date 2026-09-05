#pragma once
#include <Arduino.h>

// =====================================================
// PYRO - the ejection channel
//
// Drives a low-side N-MOSFET module (HW-517 class).
// GPIO -> (PWM)+, e-match across OUT+ / OUT-.
//
// SAFETY RULES THIS MODULE ENFORCES:
//
//   1. The gate is driven LOW before anything else in
//      the whole program runs.
//   2. Firing requires ARMED, and arming is a
//      deliberate act that cannot happen in flight.
//   3. The pulse is timed and non-blocking. The FET is
//      never left latched on.
//   4. Once fired, the RTC latch prevents a second
//      fire for the rest of the flight - INCLUDING
//      after a brownout reset.
//
// This module depends on nothing. It does not care
// whether the SD card, radio or GPS are alive.
// =====================================================

extern bool pyroArmed;
extern bool pyroFiring;
extern bool pyroFired;

extern unsigned long fireCount;
extern unsigned long fireStartTime;
extern const char*   lastFireReason;

extern float contVolts;
extern bool  contOK;

// Call this FIRST in setup(), before Serial, before
// any delay. The pin floats until it runs.
void pyroSafeInit();

void initPyro(bool verbose);
void servicePyro();

bool armPyro();
void disarmPyro();

// Flight-critical fire path. Requires ARMED and not
// already fired. Returns false if it refused.
bool firePyro(const char* reason);

// Bench only. Requires DISARMED. Never call in flight.
bool testFirePyro();

bool  armSwitchClosed();
void  readContinuity();
