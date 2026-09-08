#include "Pyro.h"
#include "Config.h"
#include "State.h"
#include "driver/gptimer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

bool pyroArmed  = false;
bool pyroFiring = false;
bool pyroFired  = false;

unsigned long fireCount     = 0;
unsigned long fireStartTime = 0;
const char*   lastFireReason = "-";

float contVolts = 0.0;
bool  contOK    = false;

static unsigned long lastContRead = 0;


static gptimer_handle_t pulseTimer = nullptr;
static bool pulseTimerReady = false;
static bool pulseTimerRunning = false;
static volatile bool pulseCutoffDone = false;
static portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR pulseAlarm(gptimer_handle_t,
                                const gptimer_alarm_event_data_t*, void*) {
  portENTER_CRITICAL_ISR(&pulseMux);
  gpio_ll_set_level(&GPIO, PYRO_GATE_PIN, LOW);
  pulseCutoffDone = true;
  portEXIT_CRITICAL_ISR(&pulseMux);
  return false;
}

static bool initPulseTimer() {
  if (pulseTimerReady) return true;
  gptimer_config_t config = {};
  config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  config.direction = GPTIMER_COUNT_UP;
  config.resolution_hz = 1000000;
  gptimer_event_callbacks_t callbacks = {};
  callbacks.on_alarm = pulseAlarm;
  if (gptimer_new_timer(&config, &pulseTimer) != ESP_OK) return false;
  if (gptimer_register_event_callbacks(pulseTimer, &callbacks, nullptr) != ESP_OK ||
      gptimer_enable(pulseTimer) != ESP_OK) {
    gptimer_del_timer(pulseTimer);
    pulseTimer = nullptr;
    return false;
  }
  pulseTimerReady = true;
  return true;
}

static void stopPulseTimer() {
  if (pulseTimerRunning) {
    if (gptimer_stop(pulseTimer) != ESP_OK) pulseTimerReady = false;
    pulseTimerRunning = false;
  }
}

static bool preparePulse() {
  if (!pulseTimerReady || pyroFiring) return false;
  stopPulseTimer();
  if (!pulseTimerReady) return false;
  gptimer_alarm_config_t alarm = {};
  alarm.alarm_count = FIRE_DURATION * 1000ULL;
  if (gptimer_set_raw_count(pulseTimer, 0) != ESP_OK ||
      gptimer_set_alarm_action(pulseTimer, &alarm) != ESP_OK) return false;
  return true;
}

static bool startPulse() {
  // Gate remains LOW unless the independent timer starts successfully.
  // Serialize HIGH with the ISR's LOW, so preemption cannot leave a HIGH
  // written after an already-expired alarm. No Serial or latch writes here.
  portENTER_CRITICAL(&pulseMux);
  pulseCutoffDone = false;
  const bool started = gptimer_start(pulseTimer) == ESP_OK;
  if (started) {
    pulseTimerRunning = true;
    pyroFiring = true;
    fireStartTime = millis();
    digitalWrite(PYRO_GATE_PIN, HIGH);
  }
  portEXIT_CRITICAL(&pulseMux);
  return started;
}

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

  latchBootInit();
  pyroArmed  = false;
  pyroFiring = false;
  pyroFired  = latchFired();
  pulseTimerReady = initPulseTimer();

  if (!verbose) return;

  Serial.println("[PYRO] Gate LOW, channel SAFE");

  if (pyroFired) {
    Serial.println("[PYRO] *** LATCH SAYS ALREADY FIRED ***");
    Serial.println("[PYRO] A fire request was latched; deployment is NOT confirmed.");
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
  if (!pulseTimerReady || pyroFiring) {
    Serial.println("[PYRO] ARM REFUSED - pulse timer unavailable or output active");
    return false;
  }

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
  stopPulseTimer();

  pyroArmed  = false;
  pyroFiring = false;

  Serial.println("[PYRO] DISARMED - gate LOW");
}


// =====================================================
// FIRE
//
// Starts an independent GPTimer cutoff, raises the gate, and returns.
// servicePyro() cleans up status and provides a secondary LOW fallback.
//
// The latch is written BEFORE the gate goes high. If
// the resulting current surge browns the board out
// mid-pulse, the latch is already in retained RTC RAM.
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

  if (!preparePulse()) {
    Serial.println("[PYRO] FIRE REFUSED - pulse timer unavailable or output active");
    return false;
  }

  pyroFired      = true;
  lastFireReason = reason;

  latchWrite(latchState(), true, latchLaunchTime());

  if (!startPulse()) {
    // Keep the conservative anti-refire latch even on an uncertain start.
    Serial.println("[PYRO] FIRE FAILED - timer start; latch remains set, gate LOW");
    return false;
  }
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

  if (!preparePulse() || !startPulse()) {
    Serial.println("[PYRO] TEST REFUSED - pulse timer unavailable or output active");
    return false;
  }

  Serial.print("[PYRO] TEST PULSE ");
  Serial.print(FIRE_DURATION);
  Serial.println(" ms - gate HIGH");

  return true;
}


// =====================================================
// SERVICE
//
// Called at the top of every loop to reconcile ISR completion and stop
// the counter. The ISR lowers the gate even if this service is delayed.
// Interrupt masking, MCU reset and electrical failures are separate limits.
//
// When not firing it re-asserts LOW every pass. Costs
// nothing, and covers a pin that was disturbed by
// something else.
// =====================================================

void servicePyro() {
  if (pyroFiring) {
    if (pulseCutoffDone || millis() - fireStartTime >= FIRE_DURATION) {
      // Independent ISR already lowered the gate normally. This LOW is also
      // a best-effort fallback if interrupts have been delayed or disabled.
      digitalWrite(PYRO_GATE_PIN, LOW);
      stopPulseTimer();
      pyroFiring = false;

      Serial.print("[PYRO] Pulse cutoff serviced at elapsed ");
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
