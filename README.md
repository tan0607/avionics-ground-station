# Rocket Avionics Ground Station

Receive SX1278 LoRa telemetry from a model rocket → a **live web dashboard** → CSV
export for a post-launch data report (PLDR). Downlink-only prototype; the whole
thing is **offline-first** (no CDNs, self-hosted fonts + map tiles) because the
launch site has no network.

```
 rocket ─LoRa→ ground station ─USB serial→ backend (FastAPI) ─WebSocket→ dashboard (React)
 (ESP32-S3)     (ESP32)                   │
                                          └─ flights/<session>/  (always on)
                                               └─ flight-NN_<name>/  (operator-declared)
```

- **`firmware/`** — the two Arduino sketches that fly: `MRCC_FlightComputer`
  (ESP32-S3 — sensors, filters, flight state, pyro, SD log, downlink) and
  `MRCC_GroundStation` (ESP32 — receives and prints to USB). Plus `SD_Doctor`
  and host-side filter tooling. **Read `firmware/README.md` before flashing.**
- **`backend/`** — reads bytes (serial in prod, a simulator in dev), logs raw
  bytes first, decodes them, writes the session CSVs, and broadcasts each
  decoded frame over `/ws`. Also serves the built dashboard.
- **`dashboard/`** — Vite + React + TS console: Live telemetry, offline Map,
  Log (raw frames + event log), Flights, and Settings.
- **`shared/protocol/`** — `mrcc.py` decodes the ASCII downlink the vehicle
  actually sends; `packet.py` is the internal `Telemetry` shape everything
  downstream consumes, and `PROTOCOL.md` documents it.
- **`shared/fake_telemetry.py`** — simulates a full flight (pad → boost → apogee
  → drogue → main → landed) so nothing needs hardware to develop against.
- **`flights/<session>/`** — per-run data (`raw.log`, `telemetry.csv`,
  `events.csv`, `mission.log`, `metadata.json`), recorded continuously.
- **`flights/<session>/flight-NN_<name>/`** — one operator-declared flight, the
  same five files for its span only. Cut with the REC button in the top bar.

### Two rockets fly

Each airframe gets its own radio channel — `VEHICLE_A` is 433.3 MHz, `VEHICLE_B`
is 434.1 MHz — and **a rocket and its ground station must be flashed from the
same setting.** LoRa does not pair: on a shared channel each ground station
decodes the *other* rocket's frames while the two transmitters collide on air.
`firmware/README.md` has the flashing table and the arithmetic.

### Two wire formats

The link carries **MRCC**, an ASCII `key=value` line (`MRCC,PKT=207,T=207.5,…`)
that `mrcc.py` decodes and maps onto `packet.Telemetry`. The 32-byte binary
frame in `PROTOCOL.md` is the format this project originally designed; only
`--fake` still produces it. `--format` defaults per source, so a live run needs
no flag.

See `HANDOFF.md` (architecture + decisions — note its staleness banner),
`DESIGN_SPECS.md` (dashboard UI), and `shared/protocol/PROTOCOL.md` for depth.

---

## Prerequisites

| Tool | Version | Install |
|---|---|---|
| Node.js + npm | 20+ | `brew install node` |
| Python | 3.12+ | (system / pyenv) |

One-time setup:

```bash
# backend deps (from repo root)
python3 -m venv backend/.venv
backend/.venv/bin/pip install fastapi uvicorn pyserial

# dashboard deps
cd dashboard && npm install
```

---

## Demo version (no hardware)

There are three ways to run without a rocket. A and B replay the same simulated
flight (the difference is whether a backend is in the loop); C replays a real
recorded one.

### A. Dashboard only — mock source (fastest)

The dashboard ships with a built-in flight simulator. No backend, no Python.

```bash
cd dashboard
npm run dev
```

Open the printed URL (e.g. `http://localhost:5173`). The **mock** source is the
default and loops a full flight forever — ideal for UI work and quick demos.
The top-bar source tag reads **◆ MOCK**.

> Force mock explicitly with `?source=mock` (handy if you've set a WS URL).

**What you get:** Live charts, Map, and the Log page all work off the mock
stream. The Settings → *Data export* card is disabled (no backend session), and
Connection shows `— (mock)`.

### B. Full stack — backend + fake telemetry

Runs the real pipeline (serial-shaped bytes → raw.log → CSV → WebSocket) against
the simulator, so **session files and export work**, exactly like a real flight.

```bash
# 1. build the dashboard so the backend can serve it
cd dashboard && npm run build && cd ..

# 2. start the backend with the fake source, looping for a continuous demo
backend/.venv/bin/python -m backend.app --fake --loop
```

Open **http://127.0.0.1:8000** — one origin serves both the UI and `/ws`.
The source tag reads **● LIVE**, Settings → Connection shows the session id and
live `/stats`, and Data export downloads the session's `telemetry.csv` /
`events.csv` / `raw.log`.

Useful `--fake` flags:

| Flag | Effect |
|---|---|
| `--loop` | replay the flight continuously (best for demos) |
| `--fast` | run with no realtime pacing (dump a flight instantly) |
| `--loss 0.1` | inject 10% packet loss to exercise the loss stats |
| `--host 0.0.0.0 --port 8000` | expose on the LAN |

#### Front-end dev against the live backend

To keep Vite's hot reload while talking to the backend, run them side by side:

```bash
# terminal 1
backend/.venv/bin/python -m backend.app --fake --loop
# terminal 2
cd dashboard && npm run dev
```

Then open `http://localhost:5173/?source=ws` (or set
`VITE_WS_URL=ws://localhost:8000/ws` in `dashboard/.env`). CORS is enabled for
any loopback origin, so `/stats` and exports work cross-origin in dev whichever
port Vite lands on (it moves to 5174, 5175... when 5173 is taken).

### C. Replay a recorded flight — the MRCC path without a radio

`--fake` emits **binary** PROTOCOL.md frames, but the airborne board downlinks
**ASCII MRCC**, which carries fields the binary frame has no slot for: the
accelerometer axes, RSSI/SNR, pressure, heading. So on `--fake` the Live accel
panel, the RSSI/SNR readouts and the aux row are all structurally empty — not
broken, just unfed. Replaying a recorded `raw.log` streams those real bytes back
through the whole pipeline instead:

```bash
backend/.venv/bin/python -m backend.app --replay flights/2026-08-19T05-54-40Z --loop
```

Point it at a session directory or the `raw.log` inside it. `--format` is read
from the session's `metadata.json`, so the right codec is picked automatically;
pass `--format` to override. `--loop` and `--fast` work as they do for `--fake`.

Records are replayed at the pace they arrived, with each inter-record gap capped
at 2 s: `raw.log` is opened in append mode per session, so one file can span days
of bench runs, and the real gaps between them are hours of dead air. A torn tail
(from a run that was killed mid-write) ends the replay cleanly — 'raw first'
means bytes reach the disk before anything validates them.

This is the only hardware-free way to exercise `shared/protocol/mrcc.py`: the
inferred health bits, `tilt_from_accel`, the `extra` passthrough, and the
null-vs-zero handling for the peripherals MRCC cannot see (SD/PYRO/VBAT read
`—`, not a fabricated failure).

---

## Live version (real hardware)

Wire up the ground station (see `firmware/README.md`), then point the backend at
the serial port instead of the simulator:

```bash
cd dashboard && npm run build && cd ..          # once, or after UI changes
backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX
```

Open **http://127.0.0.1:8000** on the operator laptop. Everything else is
identical to the full-stack demo — the only change is the byte source.

- Find the port: `ls /dev/tty.usbserial-*` (macOS) or `ls /dev/ttyUSB*` (Linux).
- Baud must match the firmware. `MRCC_GroundStation` opens USB at **115200**,
  which is now the default, so `--baud` is only needed if you change the firmware.
- Raw bytes are written to `flights/<session>/raw.log` **before** decoding, so a
  parser bug can never lose a flight — the log is replayable.

---

## Using the dashboard

The left rail switches four views:

| View | What it shows |
|---|---|
| **Live** | Altitude / vertical-speed / tilt charts, KPI strip, GO/NO-GO, flight-state timeline. The flight screen — one glance, no scroll. |
| **Map** | Offline PMTiles basemap with the live ground track + last-known fix (distance/bearing from the pad). |
| **Log** | **Left:** a live raw-telemetry table (newest frame first). **Right:** a mission event log — state transitions, pyro/deploy, link stale/restored, no-deploy alarm, session start/reset. |
| **Set** | Connection & session status, alarm/audio, display, and data export. |

### Settings

- **Connection & session** — current source, socket state, session id, frames
  decoded, CRC errors, loss. Switch **Mock ⇄ Live** (reloads the page, since the
  source is chosen at load). Shows whether the backend is reachable.
- **Alarms & audio** — master **buzzer** toggle, per-alarm **arming**
  (link-stale, no-deploy), and a **test beep**. Audio unlocks on your first
  click/keypress (browser autoplay policy); it's silent by default.
- **Display** — **high-contrast** (sunlight) mode, telemetry-table **density**,
  and **row cap**. Saved to `localStorage`, so they persist across reloads.
- **Data export** — download the active session's `telemetry.csv`, `events.csv`,
  `raw.log`, and a `stats.json` snapshot for the PLDR notebook.

### Source selection (how the dashboard picks its data)

Resolved once at page load, in this order:

1. `?source=mock` → always the built-in simulator.
2. `?source=ws` **or** `VITE_WS_URL` set → the WebSocket
   (`VITE_WS_URL`, else `ws://<host>:8000/ws`).
3. otherwise → **mock**.

---

## Data & endpoints

There are **two** nested records, because "one backend run" and "one flight" are
not the same span:

```
flights/2026-09-01T04-50-36Z/          session — one backend run, always recording
├── metadata.json · raw.log · telemetry.csv · events.csv · mission.log
└── flight-01_apex-1-1432/             flight  — cut by the operator (REC)
    └── metadata.json · raw.log · telemetry.csv · events.csv · mission.log
```

| File | Contents |
|---|---|
| `raw.log` | length-prefixed raw byte records (written before parsing) |
| `telemetry.csv` | one decoded row per frame (`packet.CSV_COLUMNS`) |
| `events.csv` | flight-state transitions + deploys |
| `mission.log` | human-readable ground log (states, pyro, link, health edges) |
| `metadata.json` | start/stop, duration, row count, source, packet/CSV contract |

A flight folder holds the same five files as its session, so it is a complete
record on its own — `--replay flights/<session>/flight-01` works, and its
`metadata.json` names the codec.

**Why the operator declares the boundary.** Neither auto-rule survives this
vehicle: the onboard clock jumps back on every transmitter reboot (394 times
inside one 21-hour bench session, all in a single `telemetry.csv`), and a board
that never leaves `PAD` never announces a liftoff. The session recorder
underneath never stops, so hitting REC late — or forgetting — costs a tidy
folder, never the data.

HTTP endpoints (same origin as the served dashboard):

| Endpoint | Purpose |
|---|---|
| `GET /ws` | live decoded-frame stream (WebSocket) |
| `GET /stats` | session id, active flight, frames decoded, CRC errors, loss % |
| `GET /session` | active session id + which files exist |
| `GET /session/{name}` | download an active-session file |
| `GET /flight` | recording state + completed flights this run |
| `POST /flight/start` | cut a flight folder (JSON body, optional `label`) |
| `POST /flight/stop` | close the flight being recorded |
| `GET /flight/files/{name}` | download from the flight being recorded |
| `GET /flights` | every recorded flight on disk, newest first |
| `GET /flights/{session}/{flight}` | one flight + bounded log/telemetry tails |
| `GET /flights/{session}/{flight}/files/{name}` | download an archived file |
| `DELETE /flights/{session}/{flight}` | permanently delete one flight folder |

`POST` requires an `application/json` content type — that is what forces a CORS
preflight, so another origin cannot stop a recording mid-flight. Downloads are
fixed-length snapshots (files are served while still being appended to).

Post-flight, point the PLDR notebook (`notebooks/`) at the session folder to
generate the flight report.

---

## Build & verify

```bash
cd dashboard && npm run build     # tsc typecheck + offline production bundle
```

The build must run with the network off — no CDNs, fonts and map tiles are
bundled. The backend serves the resulting `dashboard/dist/` at `/`.
