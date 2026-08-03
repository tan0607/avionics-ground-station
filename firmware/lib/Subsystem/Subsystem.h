// Subsystem.h -- per-peripheral fault isolation for the flight computer.
//
// THE RULE THIS LIBRARY EXISTS TO ENFORCE:
//   No single peripheral may stop the vehicle from flying or from transmitting.
//
// A barometer that fails to init, an IMU that browns out, a GPS that stops
// answering -- each one degrades EXACTLY ONE field and EXACTLY ONE health bit.
// Boot always completes. The 4 Hz downlink always runs. The ground station is
// told *which* device died, by name, instead of being handed a blanket
// "AV FAILED" that tells the operator nothing.
//
// How it works
// ------------
//   begin(now)    calls every peripheral's init() ONCE. Failures are recorded,
//                 never fatal -- there is no `while (1)` in this file, ever.
//   service(now)  reads healthy peripherals; a read that fails
//                 SUBSYS_FAULT_THRESHOLD times in a row marks that peripheral
//                 dead. Dead peripherals are re-init'd on a slow cadence
//                 (SUBSYS_RETRY_MS), so a device that browns out and recovers
//                 comes back on its own -- mid-flight.
//   healthMask()  the HEALTH_* byte to drop straight into telem_body_t.health.
//
// YOUR CONTRACT (this is the part that actually prevents the cascade):
//   init() and read() MUST return within a few milliseconds and MUST NOT block
//   forever. This library bounds *how often* a broken device is touched; it
//   cannot bound a call that never returns. For I2C parts that means calling
//   Wire.setTimeOut() and using I2CRecover.h -- see that header. A read() that
//   spins on a wedged bus will stall the flight loop no matter what this
//   scheduler does.
//
// Deliberately free of <Arduino.h>: `now` is injected, so the whole policy is
// host-testable without hardware (test/subsystem_check.cpp proves it).
#pragma once
#include <stdint.h>
#include <stddef.h>

// Consecutive failed reads before a peripheral is declared dead. >1 so a single
// noisy sample doesn't drop a good sensor off the telemetry.
static const uint8_t SUBSYS_FAULT_THRESHOLD = 3;

// How often a dead peripheral is re-init'd. Long enough that a permanently dead
// device costs ~nothing per loop; short enough that a brownout recovers fast.
static const uint32_t SUBSYS_RETRY_MS = 2000;

// Retry stagger: peripheral i waits an extra i * SUBSYS_RETRY_STAGGER_MS. Keeps
// several dead devices from piling their retries into the same 250 ms window.
static const uint32_t SUBSYS_RETRY_STAGGER_MS = 137;

// Attempt to bring the device up. Return true only if it actually responded.
typedef bool (*subsys_init_fn)(void);
// Perform one bounded read into your own state. Return false on a fault.
typedef bool (*subsys_read_fn)(void);

struct Subsystem {
  // --- configuration (you fill these) ---
  const char*    name;   // "BARO" -- appears in the boot log and fault lines
  uint8_t        bit;    // HEALTH_* mask from TelemPacket.h
  subsys_init_fn init;
  subsys_read_fn read;   // may be nullptr for init-only devices (e.g. SD mount)

  // --- runtime state (managed here; do not poke) ---
  bool     healthy;
  bool     ever_healthy;      // false = never came up == INIT failure
  uint8_t  consecutive_faults;
  uint32_t fault_total;       // lifetime fault count, for the post-flight report
  uint32_t next_retry_ms;
};

// Convenience initialiser so a table entry reads as one clean line.
#define SUBSYS(nm, healthbit, initfn, readfn) \
  { nm, (healthbit), (initfn), (readfn), false, false, 0, 0, 0 }

class SubsystemSet {
 public:
  SubsystemSet(Subsystem* items, uint8_t count) : _items(items), _count(count) {}

  // Try every peripheral's init() exactly once. ALWAYS returns -- a total
  // failure of every device still leaves a flyable, transmitting vehicle.
  // Returns the health mask after the attempt.
  uint8_t begin(uint32_t now_ms);

  // One scheduler tick. Reads healthy peripherals, retries dead ones on cadence.
  // Returns the set of health bits that CHANGED this tick (0 = steady state),
  // so the caller can log transitions instead of spamming every cycle.
  uint8_t service(uint32_t now_ms);

  // Current health byte -- goes straight into telem_body_t.health.
  uint8_t healthMask() const;

  // Peripherals that never initialised (health bit clear AND never_healthy).
  // This is the "starting initiation failed" set, distinct from in-flight death.
  uint8_t initFailedMask() const;

  uint8_t count() const { return _count; }
  const Subsystem& at(uint8_t i) const { return _items[i]; }

 private:
  // Shared by begin() and the retry path: run init, update state, return healthy.
  bool tryInit(Subsystem& s, uint32_t now_ms);

  Subsystem* _items;
  uint8_t    _count;
};
