#include "Pyro.h"
#include "Config.h"
#include "State.h"

bool pyroArmed  = false;
bool pyroFiring = false;
bool pyroFired  = false;

unsigned long fireCount     = 0;
unsigned long fireStartTime = 0;
const char*   lastFireReason = "-";

float contVolts = 0.0;
bool  contOK    = false;

static unsigned long lastContRead = 0;


// =====================================================
// SAFE INIT
//
// The very first thing the program does. Between reset
// and this line the gate is floating, which is why the
// 10k hardware pulldown is not optional.
//
// No Serial in here - it has not been started yet.
// =====================================================

void pyroSafeInit() {
  pinMode(PYRO_GATE_PIN, OUTPUT);
  digitalWrite(PYRO_GATE_PIN, LOW);
}


// =====================================================
// INIT
//
// Reads the RTC latch. If the charge already went in
// this flight, we come up permanently safe.
// =====================================================

void initPyro(bool verbose) {
  digitalWrite(PYRO_GATE_PIN, LOW);

#if ARM_SWITCH_ENABLED
  pinMode(ARM_SWITCH_PIN, INPUT_PULLUP);
#endif

#if PYRO_CONT_ENABLED
  pinMode(PYRO_CONT_PIN, INPUT);
#endif

  pyroArmed  = false;
  pyroFiring = false;
  pyroFired  = latchFired();

  if (!verbose) return;

  Serial.println("[PYRO] Gate LOW, channel SAFE");

  if (pyroFired) {
    Serial.println("[PYRO] *** LATCH SAYS ALREADY FIRED ***");
    Serial.println("[PYRO] This board reset AFTER deployment.");
    Serial.println("[PYRO] Arming is blocked. Power cycle to clear.");
  }

#if !ARM_SWITCH_ENABLED
  Serial.println("[PYRO] NOTE - arm switch sense disabled in Config.h");
#endif

#if PYRO_CONT_ENABLED
  readContinuity();
  Serial.print("[PYRO] Continuity ");
  Serial.print(contOK ? "OK" : "OPEN");
  Serial.print(" (");
  Serial.print(contVolts, 2);
  Serial.println(" V)");
#else
  Serial.println("[PYRO] NOTE - continuity sense disabled in Config.h");
#endif
}


// =====================================================
// ARM SWITCH
//
// Closed = LOW (input pullup). When the sense line is
// not wired we report closed, because the switch still
// physically breaks the pyro battery line - the GPIO
// was only ever a report, never the interlock.
// =====================================================

bool armSwitchClosed() {
#if ARM_SWITCH_ENABLED
  return digitalRead(ARM_SWITCH_PIN) == LOW;
#else
  return true;
#endif
}


// =====================================================
// CONTINUITY
//
// Divider across the match. Microamps only - nowhere
// near the no-fire current. Never read while firing.
// =====================================================

void readContinuity() {
#if PYRO_CONT_ENABLED
  if (pyroFiring) return;

  contVolts = analogReadMilliVolts(PYRO_CONT_PIN) * PYRO_CONT_DIVIDER / 1000.0;
  contOK    = (contVolts >= PYRO_CONT_MIN_VOLTS);
#else
  contVolts = 0.0;
  contOK    = false;
#endif
}


// =====================================================
// ARM
// =====================================================

bool armPyro() {
  if (pyroFired) {
    Serial.println("[PYRO] ARM REFUSED - already fired this flight");
    return false;
  }

  if (!armSwitchClosed()) {
    Serial.println("[PYRO] ARM REFUSED - arm switch is open");
    return false;
  }

  readContinuity();

#if PYRO_CONT_ENABLED
  if (!contOK) {
    Serial.println("[PYRO] ARM REFUSED - no continuity, check the match");
    return false;
  }
#endif

  pyroArmed = true;

  Serial.println("[PYRO] *** ARMED ***");
  return true;
}


void disarmPyro() {
  digitalWrite(PYRO_GATE_PIN, LOW);

  pyroArmed  = false;
  pyroFiring = false;

  Serial.println("[PYRO] DISARMED - gate LOW");
}


// =====================================================
// FIRE
//
// Sets the gate high and returns immediately. The pulse
// is ended by servicePyro(). Nothing here waits.
//
// The latch is written BEFORE the gate goes high. If
// the resulting current surge browns the board out
// mid-pulse, the latch is already on the card.
// =====================================================

bool firePyro(const char* reason) {
  if (!pyroArmed) {
    Serial.println("[PYRO] FIRE REFUSED - not armed");
    return false;
  }

  if (pyroFired) {
    Serial.println("[PYRO] FIRE REFUSED - already fired");
    return false;
  }

  pyroFired      = true;
  lastFireReason = reason;

  latchWrite(latchState(), true, latchLaunchTime());

  digitalWrite(PYRO_GATE_PIN, HIGH);

  pyroFiring    = true;
  fireStartTime = millis();
  fireCount++;

  Serial.println();
  Serial.println("########################################");
  Serial.print  ("# FIRE  t=");
  Serial.print(millis() / 1000.0, 2);
  Serial.print  ("s  reason=");
  Serial.println(reason);
  Serial.println("########################################");
  Serial.println();

  return true;
}


// =====================================================
// BENCH TEST FIRE
//
// Deliberately refuses while armed, so the flight path
// and the test path can never be confused.
// =====================================================

bool testFirePyro() {
  if (pyroArmed) {
    Serial.println("[PYRO] TEST REFUSED - disarm first");
    return false;
  }

  digitalWrite(PYRO_GATE_PIN, HIGH);

  pyroFiring    = true;
  fireStartTime = millis();

  Serial.print("[PYRO] TEST PULSE ");
  Serial.print(FIRE_DURATION);
  Serial.println(" ms - gate HIGH");

  return true;
}


// =====================================================
// SERVICE
//
// Called at the top of every loop, before anything that
// could be slow. Ending the pulse on time is the only
// job here and it must never be delayed.
//
// When not firing it re-asserts LOW every pass. Costs
// nothing, and covers a pin that was disturbed by
// something else.
// =====================================================

void servicePyro() {
  if (pyroFiring) {
    if (millis() - fireStartTime >= FIRE_DURATION) {
      digitalWrite(PYRO_GATE_PIN, LOW);
      pyroFiring = false;

      Serial.print("[PYRO] Pulse ended after ");
      Serial.print(millis() - fireStartTime);
      Serial.println(" ms - gate LOW");
    }
    return;
  }

  digitalWrite(PYRO_GATE_PIN, LOW);

#if PYRO_CONT_ENABLED
  if (millis() - lastContRead >= 1000) {
    lastContRead = millis();
    readContinuity();
  }
#endif
}
