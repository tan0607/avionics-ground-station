# Actual A/B code simulation and backend replay — 2026-09-06

Both A/B were compiled as host binaries from current State.cpp, Filters.cpp,
Flight.cpp and Pyro.cpp, with ASan/UBSan. The runner drives the confirmed +Y nose
mount. Python supplies sensor inputs and formats outputs; it does not choose
flight states or firing times. Deployed Flight.cpp/Pyro.cpp/Config.h were not
changed. No serial hardware, flash, GPIO hardware or firing circuit was accessed.

## Open the result

- Graphs and exact event table: http://127.0.0.1:8001/simulation/
- Rocket A nominal dashboard: http://127.0.0.1:8001/?source=ws
- Rocket B nominal dashboard: http://127.0.0.1:8002/?source=ws
- Rocket B barometer-loss backup: http://127.0.0.1:8003/?source=ws

The three looped backends were left running. Their sources explicitly report
`replay`, with paths naming the simulated vehicle and scenario. They use
separate backend-session folders and do not replace the serial backend on 8000.
The report and six A/B scenario directories are under:
`flights/simulations/20260906-151605/` (runtime data, git-ignored).

Each scenario contains `run.json`, `trace.csv`, `events.csv`, `metadata.json`
and `raw.log`. Source hashes and input hash are included. Reports include
`flight-comparison.png`, its SVG, `pyro-detail.png` and browser screenshots.

## Synthetic input and timing convention

The first sensor sample is at boot 1.510 s. Prescribed liftoff is at boot 20 s;
all T+ below use that input liftoff as zero. Net acceleration is 50 m/s² for
2 s, followed by a prescribed ballistic altitude curve and stationary ground
input. Input apogee is T+12.197 s, about 610 m. This is not OpenRocket data and
does not predict either actual rocket. The trajectory does not respond to the
pyro output: no parachute deployment mechanics/aerodynamics are modeled.

Loop and IMU input interval are 10 ms; accepted barometer interval is 50 ms.
The backup scenario sets baroOK false at T+5 s. Pad-only remains stationary for
the whole 65 s boot-time run. Both A/B receive identical input histories.

Replay uses a 20 Hz analysis stream constructed from post-service C++ snapshots,
not the actual 2 Hz Radio.cpp packet generator. This makes the 50 ms APOGEE state
visible and carries `PG` as the simulated GPIO level. `FI` remains the actual
latched fired flag: FI stays 1 after the 400 ms GPIO pulse ends. GPIO event
timestamps, not browser wall time, are authoritative for pulse timing.

## Observed results (A and B identical)

| Event | Nominal boot s | Nominal T+ s | Barometer-loss T+ s |
|---|---:|---:|---:|
| PAD init | 1.500 | -18.500 | -18.500 |
| ARMED | 10.010 | -9.990 | -9.990 |
| BOOST / launch detected | 20.260 | 0.260 | 0.260 |
| COAST | 22.210 | 2.210 | 2.210 |
| APOGEE state | 32.860 | 12.860 | 19.260 |
| GPIO HIGH / firePyro | 32.910 | 12.910 | 19.310 |
| DESCENT | 32.910 | 12.910 | 19.310 |
| GPIO LOW | 33.310 | 13.310 | 19.710 |
| LANDED | 54.060 | 34.060 | 33.360 |

Nominal GPIO HIGH occurs 12.650 s after detected launch and about 0.713 s after
input apogee. Backup HIGH occurs 19.050 s after detected launch. Both pulses
are exactly 400 ms in these 10 ms loop runs. Pad-only auto-arms but never enters
BOOST and never raises the GPIO; the 19 s backup does not start from boot/arming.
Barometer-loss LANDED is only the firmware's declaration, not independent
touchdown evidence; its held altitude and velocity are stale.

## Every firmware state: current conditions

These apply to both A/B. Source: Flight.cpp `initFlight`, `armFlight`,
`tryAutoArm`, `serviceFlight`; constants in Config.h lines 338–437.

| State entered | Actual condition |
|---|---|
| PAD | Cold boot or reset with prelaunch retained state. Explicit disarm also returns to PAD and blocks auto-arm until reboot. |
| ARMED | Normal auto-arm: IMU healthy, gyro calibration done, stillness timer >=10000 ms. PAD resets that timer when acceleration magnitude is outside the strict 9.80665 ±0.5 m/s² band or gyro magnitude is >=5 deg/s. If IMU is down, healthy barometer plus >=10000 ms since PAD init is the fallback. `armPyro()` must succeed and no fired latch may block arming. Auto-arm retries are rate-limited to 5000 ms. |
| BOOST | From ARMED: filtered acceleration magnitude >3g for 5 fresh usable IMU confirmations at flight-service cadence. Missing samples do not add confirmations; stale/invalid samples clear them. Alternative: fresh accepted barometer, filtered altitude >15 m AGL and VZ >5 m/s, plus IMU unavailable or a new IMU sample corroborating acceleration >1.5g. |
| COAST | From BOOST: acceleration <1.5g or IMU unavailable, with burnout timer >=200 ms. A high-acceleration tick resets the timer. Forced COAST when detected-launch elapsed time >4000 ms. |
| APOGEE | From COAST, normal: detected-launch elapsed >=1500 ms, healthy barometer, max altitude reached >=30 m, and 4 fresh accepted samples with filtered VZ <-2 m/s. Stale/lost/reseeded barometer data clears descent confirmation. Alternative: elapsed >=19000 ms since detected launch; backup ignores height/velocity. Despite its name, MIN_COAST_TIME is measured from launch, not COAST entry. |
| DESCENT | On the next flight-service tick after APOGEE, call `firePyro()`, then enter DESCENT. `firePyro()` refuses if not armed or already fired; otherwise it latches fired, sets GPIO HIGH and starts the pulse. DESCENT itself does not prove current flow or successful deployment. |
| LANDED | From DESCENT: for 10000 ms no healthy barometer altitude change >2 m relative to the moving reference, and no healthy IMU acceleration deviation >3 m/s² from gravity. Movement resets the reference/time. Unavailable sensors are skipped, so this is not reliable touchdown evidence with missing sensors. The next LANDED service tick disarms pyro. |

Current ARM_SWITCH_ENABLED and PYRO_CONT_ENABLED are both 0. Software armed
and physical pyro supply are separate; the host does not model electrical power.

### Reset and display details

If reset retains an in-flight state and fired=false, firmware restores that
state, re-arms software and restarts launchTime from the reset clock. This shifts
the backup deadline. If fired=true it comes back in DESCENT with pyro safe.
The fired latch blocks a second firing. The existing interrupted-pulse issue is
not corrected or validated by these nominal runs.

Dashboard `DROGUE` is the legacy internal protocol mapping for MRCC `DESCENT`;
the firmware has no separate MAIN state or second deployment in this sequence.
The dashboard's large T+ is a browser wall clock anchored to the first received
launched frame. It can differ from input T+, particularly for late joins and
replay pacing. The exact plots use simulated onboard time relative to 20 s.

## Verification and remaining limits

- Fail-first MOUNT test rejected the new command on both old host binaries;
  after implementation A/B mounted-axis runs have the same transition/edge
  timestamps as the prior axis fixture, with AY populated and upright roll zero.
- Export integration tests pass: replay parses every sample, preserves states,
  mounted axes and FI transition timing; backup fires from the launch clock;
  pad-only never fires. Full firmware suite: **62 methods, 54 pass, 8 existing
  expected failures**. They remain failures of the stated safety expectations.
- `/stats` on all three replay backends: format mrcc, no source error, no CRC
  errors, no unknown states, no truncated input. `/gs` reports A/B correctly.
- Live WebSocket and browser observations captured FI=true and PG=1 at onboard
  32910 ms for both nominal streams, and 39310 ms for backup. Browser captures
  show REPLAY, correct A1R/A2R mission and PYRO FIRED. Report images loaded;
  no JavaScript page errors. Browser plugin had a stale service-module path;
  local Playwright was used for these checks instead.
- Existing expected failures: observed settle-window duration, reset-delayed
  backup deadline, loop-stall pulse extension and reset-interrupted pulse, each
  for A/B. In this run ARMED at boot 10.010 s follows only 8.500 s of supplied
  stationary samples, because stillSince starts at zero. Do not describe this
  result as proof of ten seconds of observed stillness.
- Host stubs do not execute ICM/BMP drivers, Baro.cpp pressure rejection, LoRa,
  SD stalls, ESP32 scheduling, current flow or deployment mechanics. Physical
  readiness and actual rocket deployment timing remain unverified.

Regenerate into a new directory:
`backend/.venv/bin/python -m firmware.tools.simulate_flight --out flights/simulations/NEW_RUN`

Serve one saved scenario:
`backend/.venv/bin/python -m firmware.tools.simulate_flight --out flights/simulations/20260906-151605 --serve --vehicle A --scenario nominal --port 8001`
