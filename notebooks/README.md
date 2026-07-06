# PLDR notebooks

**Post-Launch Data Report** — turn a recovered flight into charts + a report. Every flight's
PLDR is one pass of the template notebook.

## Files

| File | What it is |
|------|------------|
| `flight_report.ipynb` | The PLDR template. Set `FLIGHT_DIR`, **Run All**. |
| `make_fake_flight.py`  | Generates a fake `flights/<session>/` so the notebook can be run with no hardware. |

## One parser, two logs

Both the E32 downlink and the onboard SD card carry the same 32-byte `body_t` struct, so a
single codec — [`shared/protocol/packet.py`](../shared/protocol/packet.py) — decodes both.
The notebook imports it directly (never a copy):

- **SD master log** (`sd_log.bin`) → `packet.PacketParser` → full-rate, complete record → drives **kinematics**.
- **GS downlink** (`telemetry.csv`, columns = `packet.CSV_COLUMNS`) → lossy 4 Hz view → drives **link stats**.

`packet.Telemetry.to_csv_row()` defines the column schema, so the binary-decode path and the
CSV path land in the *same* table shape.

## Quick start (from repo root)

```bash
pip install pandas matplotlib jupyter

# 1. make a fake flight to analyse (writes flights/2026-07-06_sim-01/)
python3 -m notebooks.make_fake_flight

# 2a. open it interactively …
jupyter notebook notebooks/flight_report.ipynb

# 2b. … or run it headless
jupyter nbconvert --to notebook --execute --inplace notebooks/flight_report.ipynb
```

Point the notebook at a different flight without editing it:

```bash
PLDR_FLIGHT_DIR=flights/2026-07-05_flight-01 \
  jupyter nbconvert --to notebook --execute --inplace notebooks/flight_report.ipynb
```

## What it reports

1. Altitude / velocity / acceleration vs time (SD full-rate)
2. Apogee + max velocity + descent rates (drogue / main / touchdown)
3. GPS ground track + landing distance & bearing
4. Event timeline (pad → boost → … → landed, with `T+` times)
5. **Link stats: packet loss % vs flight phase** — the antenna-performance report

## Session folder layout

The notebook reads the layout from `GROUND_STATION_PLAN.md` §4:

```
flights/<session>/
├── metadata.json     flight metadata
├── sd_log.bin        onboard SD master (complete, binary frames)   ← kinematics
├── raw.log           GS raw capture (host-timestamped hex)
├── telemetry.csv     GS decoded downlink (packet.CSV_COLUMNS)      ← link stats
└── events.csv        flight-state transitions + pyro edge
```

`sd_log.bin` is optional — with only `telemetry.csv` the notebook falls back to the downlink
for kinematics (set `PREFER=csv` to force it).
