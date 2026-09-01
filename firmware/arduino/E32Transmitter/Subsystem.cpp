#include "Subsystem.h"

// Wrap-safe deadline compare: true once `now` has reached `deadline`, correct
// across the millis() rollover at 2^32 ms (~49.7 days). Same idiom the TX
// cadence in onboard_tx.cpp uses.
static inline bool due(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

// Stagger dead-peripheral retries so N failed devices don't all re-init in the
// same loop pass and blow the 250 ms TX budget together.
static inline uint32_t retryAt(uint32_t now, uint8_t index) {
  return now + SUBSYS_RETRY_MS + (uint32_t)index * SUBSYS_RETRY_STAGGER_MS;
}

bool SubsystemSet::tryInit(Subsystem& s, uint32_t now_ms) {
  (void)now_ms;
  // A null init() means "nothing to bring up" (e.g. a plain ADC) -- treated as
  // present. A null read() means "init-only" and is handled in service().
  const bool ok = s.init ? s.init() : true;
  if (ok) {
    s.healthy = true;
    s.ever_healthy = true;
    s.consecutive_faults = 0;
  } else {
    s.healthy = false;
    s.fault_total++;
  }
  return ok;
}

uint8_t SubsystemSet::begin(uint32_t now_ms) {
  for (uint8_t i = 0; i < _count; i++) {
    Subsystem& s = _items[i];
    // No early return and no abort: every peripheral gets its shot, and a
    // failure here is recorded rather than fatal. This is the line that stops
    // one dead sensor from taking the whole flight computer with it.
    if (!tryInit(s, now_ms)) {
      s.next_retry_ms = retryAt(now_ms, i);
    }
  }
  return healthMask();
}

uint8_t SubsystemSet::service(uint32_t now_ms) {
  uint8_t changed = 0;

  for (uint8_t i = 0; i < _count; i++) {
    Subsystem& s = _items[i];

    if (s.healthy) {
      if (s.read == nullptr) continue;   // init-only device: nothing to poll
      if (s.read()) {
        s.consecutive_faults = 0;        // a good read clears the streak
        continue;
      }
      // Faulted read. Tolerate a short streak before dropping the device --
      // one bad sample on a noisy bus is not a dead sensor.
      s.fault_total++;
      if (s.consecutive_faults < 0xFF) s.consecutive_faults++;
      if (s.consecutive_faults >= SUBSYS_FAULT_THRESHOLD) {
        s.healthy = false;
        s.next_retry_ms = retryAt(now_ms, i);
        changed |= s.bit;                // 1 -> 0: died (log it once)
      }
      continue;
    }

    // Dead. Re-init on a slow, staggered cadence so it can come back by itself
    // -- including mid-flight, after a brownout clears.
    if (!due(now_ms, s.next_retry_ms)) continue;
    if (tryInit(s, now_ms)) {
      changed |= s.bit;                  // 0 -> 1: recovered
    } else {
      s.next_retry_ms = retryAt(now_ms, i);
    }
  }

  return changed;
}

uint8_t SubsystemSet::healthMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < _count; i++) {
    if (_items[i].healthy) mask |= _items[i].bit;
  }
  return mask;
}

uint8_t SubsystemSet::initFailedMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < _count; i++) {
    // Never came up at all -- "starting initiation failed", as opposed to a
    // device that worked and then died (ever_healthy would be true).
    if (!_items[i].healthy && !_items[i].ever_healthy) mask |= _items[i].bit;
  }
  return mask;
}
