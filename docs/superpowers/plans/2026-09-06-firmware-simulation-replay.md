# Actual firmware simulation, backend replay and trigger graphs

Run current A/B State, Filters, Flight and Pyro C++ with synthetic +Y-nose
sensor inputs. Produce high-resolution trace, exact GPIO edges and source
hashes, then export MRCC replay records for the existing backend/dashboard.
Do not change deployed flight/pyro behavior or access serial hardware.

- Extend the existing host-only harness with optional mounted-axis input and
  trace snapshots; keep old test inputs compatible. Add focused fail-first tests.
- Add a reusable host runner/exporter under firmware/tools, producing nominal,
  barometer-loss backup and pad-only scenarios for both vehicles. Full traces
  and GPIO events are authoritative; MRCC replay is a sampled visualization.
- Use explicit synthetic input assumptions until actual trajectory data is
  supplied. State machine response does not prove physical parachute deployment.
- Export event/trace CSV, source-hashed JSON, matplotlib graphs and backend
  replay folders to a distinct simulation directory. Preserve live backend.
- Launch separate local replay backends on free ports; verify /stats and /ws,
  open dashboards and inspect graph output. No mock flight-state substitution.
- Explain every state transition from current Flight.cpp/Config.h, including
  normal/backup apogee, next-service-tick fire, landing and reset paths.

Validation: focused runner/harness tests; full firmware suite; backend replay
integration checks; actual backend WebSocket observations and rendered plots.

Completed: six actual-code A/B runs generated with hashes, CSV/JSON, MRCC
replays and plots. Replays running on 8001 (A nominal), 8002 (B nominal), 8003
(B baro-loss). Browser/WS observed correct fired flags and GPIO levels/times.
Full suite: 62 methods, 54 pass, 8 pre-existing expected failures. Detailed
conditions, times and limits: docs/validation/2026-09-06-firmware-simulation-replay.md.
