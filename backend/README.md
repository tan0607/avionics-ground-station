# backend — serial → raw.log → telemetry.csv → WebSocket

FastAPI ground-station server. One process reads the telemetry byte stream, logs
it durably, and serves both the built dashboard and a live WebSocket from
`http://localhost:8000`. It imports the packet codec from
`shared/protocol/packet.py` — **the struct is never redefined here.**

## Setup (once)

```bash
# from the repo root
python3 -m venv backend/.venv
backend/.venv/bin/pip install fastapi "uvicorn[standard]" pyserial
```

## Run (from the repo root, so `shared.*` imports resolve)

```bash
# dev — spawn the flight simulator as the byte source (no hardware)
backend/.venv/bin/python -m backend.app --fake

# dev — replay the flight forever so the dashboard always has live data
backend/.venv/bin/python -m backend.app --fake --loop

# dev — inject packet loss to exercise the loss stats
backend/.venv/bin/python -m backend.app --fake --loss 0.05

# prod — read the E32 bridge over USB serial
backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX --baud 9600
```

Then open **http://localhost:8000/?source=ws** (the dashboard's `defaultWsUrl()`
points at `:8000/ws`). Or, in vite dev, set `VITE_WS_URL=ws://localhost:8000/ws`.

Endpoints: `/` dashboard (static `dashboard/dist`) · `/ws` telemetry stream ·
`/stats` link/loss JSON.

## Pipeline (`app.py: ingest`)

```
source bytes ─► raw.log (host_ms + raw, BEFORE parse)  ─► PacketParser (resync + CRC)
                                                            ├─► telemetry.csv (flush/line)
                                                            ├─► events.csv (state changes)
                                                            ├─► LossTracker (seq gaps)
                                                            └─► /ws broadcast (WireFrame JSON)
```

**Raw first, then parse** (GROUND_STATION_PLAN.md §4): every chunk is appended to
`raw.log` with a host timestamp before the parser touches it, so a parser bug can
never lose data. `raw.log` is length-prefixed binary
(`<host_epoch_ms u64><len u32><bytes>`) and is independently replayable.

## Session output — `flights/<UTC-timestamp>/`

| file | contents |
|------|----------|
| `metadata.json` | start time, source, packet/CSV contract, raw.log format |
| `raw.log` | verbatim byte records with host timestamps (replayable) |
| `telemetry.csv` | one decoded row per frame (`packet.CSV_COLUMNS`), flushed per line |
| `events.csv` | flight-state transitions (pad/liftoff/apogee/deploys/landed) |
| `mission.log` | ground log: states, pyro, link stale/lost, per-peripheral health |

`flights/` is git-ignored (runtime data).

## Flight output — `flights/<session>/flight-NN_<name>/`

The session above records continuously; a **flight** is the span the operator
declares with REC (`POST /flight/start` … `/flight/stop`). It holds the same
five files for that span only, so it replays and analyses on its own.

The boundary is not inferred, and that is deliberate: the onboard clock jumping
back means a transmitter reboot (394 of them in one 21-hour bench session, not
394 flights), and a vehicle sitting in `PAD` never reports a liftoff. Both
candidate auto-rules therefore produce either a folder per reboot or no folder
at all. `session.py::MissionDeriver` writes `mission.log`; `link_watchdog` in
`app.py` supplies the events proven by the *absence* of frames (LINK STALE /
LOST), which a frame-driven loop can never emit.

`session.py::delete_recorded_flight` is the only code here that destroys data:
it unlinks exactly the five files it wrote and removes the empty directory —
never a recursive tree walk — and refuses a folder holding anything else.

## Loss stats — the one policy knob

`loss.py::LossTracker` infers loss from `seq` gaps (the E32 has no RSSI). `seq` is
a uint16 that wraps, so gap math is `(seq - last) & 0xFFFF`. `RESET_GAP` (default
1000) decides when a large forward jump is treated as a **stream restart**
(re-baseline) rather than a genuine burst loss — tune it to your longest plausible
real dropout. See `stats()` / `GET /stats`.

## Tests

```bash
backend/.venv/bin/python -m backend.tests   # loss wraparound, wire contract, raw framing
```

## Notes

- `telemetry.csv host_time` is ISO-8601 UTC (PLDR/pandas friendly); the WebSocket
  `host_time` is epoch **ms** (what the dashboard normalizer expects).
- The WS frame is exactly the dashboard `WireFrame` — loss is *not* stuffed into
  the frame; read it from `/stats`.
```
