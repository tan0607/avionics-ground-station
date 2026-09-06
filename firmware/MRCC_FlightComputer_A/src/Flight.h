#pragma once
#include <Arduino.h>

// =====================================================
// FLIGHT STATE MACHINE - single deployment at apogee
//
//   PAD -> ARMED -> BOOST -> COAST -> APOGEE(fire)
//       -> DESCENT -> LANDED
//
// Transitions LATCH. The machine never runs backwards
// once it has left the pad, so a noisy sample cannot
// walk it back into a state where it fires twice.
//
// Every fire path is guarded by three independent
// conditions - state, time and altitude - and each one
// alone is enough to block it.
// =====================================================

#define FS_PAD      0
#define FS_ARMED    1
#define FS_BOOST    2
#define FS_COAST    3
#define FS_APOGEE   4
#define FS_DESCENT  5
#define FS_LANDED   6

extern uint8_t flightState;

extern float groundAlt;     // m MSL, the pad
extern float altAGL;        // m above the pad, raw
extern float altFiltered;   // m above the pad, alpha-beta
extern float vertVel;       // m/s, positive up
extern float maxAlt;        // m AGL, highest seen
extern float apogeeAlt;     // m AGL, at the moment of fire

extern float accelMag;      // m/s2, vector magnitude
extern float gyroMag;       // deg/s, vector magnitude

extern unsigned long launchTime;
extern unsigned long apogeeTime;

extern bool timerBackupUsed;

const char* stateName(uint8_t s);

void initFlight(bool verbose);
void serviceFlight();
void printArmReadiness(); // read-only PAD timer and last rejected sample

// AW telemetry bitmask. Zero means gates satisfied, NOT an arming acknowledgement.
enum ArmWait : uint8_t {
  ARM_WAIT_DELAY = 1, ARM_WAIT_STILL = 2, ARM_WAIT_CAL = 4,
  ARM_WAIT_IMU = 8, ARM_WAIT_BLOCKED = 16, ARM_WAIT_DISABLED = 32,
  ARM_WAIT_FIRED = 64, ARM_WAIT_INTERLOCK = 128
};
struct ArmReadiness {
  uint8_t wait;
  unsigned long delayRemainingMs;
  unsigned long stillRemainingMs;
};
ArmReadiness armReadiness();

bool armFlight();
void disarmFlight();
