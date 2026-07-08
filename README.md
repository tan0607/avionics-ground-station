# Rocket Avionics Ground Station

Receive E32 LoRa telemetry from a model rocket → a **live web dashboard** → CSV
export for a post-launch data report (PLDR). Downlink-only prototype; the whole
thing is **offline-first** (no CDNs, self-hosted fonts + map tiles) because the
launch site has no network.

```
 rocket ─LoRa→ E32 bridge ─USB serial→ backend (FastAPI) ─WebSocket→ dashboard (React)
                                         │
                                         └─ flights/<session>/  (raw.log · telemetry.csv · events.csv)
```

- **`backend/`** — reads bytes (serial in prod, a simulator in dev), logs raw
  bytes first, decodes the 32-byte packet, writes the session CSVs, and
  broadcasts each decoded frame over `/ws`. Also serves the built dashboard.
- **`dashboard/`** — Vite + React + TS console: Live telemetry, offline Map,
  Log (raw frames + event log), and Settings.
- **`shared/protocol/`** — the 32-byte packet codec + `PROTOCOL.md` (the one
  source of truth; firmware mirrors it byte-for-byte).
- **`shared/fake_telemetry.py`** — simulates a full flight (pad → boost → apogee
  → drogue → main → landed) so nothing needs hardware to develop against.
- **`flights/<session>/`** — per-run data (`raw.log`, `telemetry.csv`,
  `events.csv`, `metadata.json`).

See `HANDOFF.md` (architecture + decisions), `DESIGN_SPECS.md` (dashboard UI),
and `shared/protocol/PROTOCOL.md` (wire format) for depth.

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

There are two ways to run without a rocket. Both replay the same simulated
flight; the difference is whether a backend is in the loop.

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
`VITE_WS_URL=ws://localhost:8000/ws` in `dashboard/.env`). CORS is enabled on the
backend so `/stats` and exports work cross-origin in dev.

---

## Live version (real hardware)

Wire up the E32 bridge (see `firmware/` + `PROTOCOL.md`), then point the backend
at the serial port instead of the simulator:

```bash
cd dashboard && npm run build && cd ..          # once, or after UI changes
backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX --baud 9600
```

Open **http://127.0.0.1:8000** on the operator laptop. Everything else is
identical to the full-stack demo — the only change is the byte source.

- Find the port: `ls /dev/tty.usbserial-*` (macOS) or `ls /dev/ttyUSB*` (Linux).
- Baud must match the firmware (E32 default here is **9600**).
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

Each backend run creates `flights/<session-id>/` with:

| File | Contents |
|---|---|
| `raw.log` | length-prefixed raw byte records (written before parsing) |
| `telemetry.csv` | one decoded row per frame (`packet.CSV_COLUMNS`) |
| `events.csv` | flight-state transitions + deploys |
| `metadata.json` | start time, source, packet/CSV contract |

HTTP endpoints (same origin as the served dashboard):

| Endpoint | Purpose |
|---|---|
| `GET /ws` | live decoded-frame stream (WebSocket) |
| `GET /stats` | session id, frames decoded, CRC errors, loss % |
| `GET /session` | active session id + which files exist |
| `GET /session/{telemetry.csv\|events.csv\|raw.log}` | download a session file |

Post-flight, point the PLDR notebook (`notebooks/`) at the session folder to
generate the flight report.

---

## Build & verify

```bash
cd dashboard && npm run build     # tsc typecheck + offline production bundle
```

The build must run with the network off — no CDNs, fonts and map tiles are
bundled. The backend serves the resulting `dashboard/dist/` at `/`.
