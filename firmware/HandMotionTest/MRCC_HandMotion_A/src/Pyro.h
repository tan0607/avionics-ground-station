// HAND TEST BENCH OUTPUT: real 400 ms pulse; multimeter/dummy load only.
// Original API documentation below describes the production module.
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
//      prelaunch readiness decision (or retained in-flight recovery).
//   3. A GPTimer ISR bounds the pulse independently of the main loop.
//      This does not guarantee cutoff during interrupt masking or reset.
//   4. Once fired, the RTC latch prevents a second
//      fire for the rest of the flight - INCLUDING
//      after a brownout reset.
//
// This module requires a working GPTimer, but not SD, radio or GPS.
// The fired latch records an attempt, not confirmed deployment.
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
