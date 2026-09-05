# Rocket Avionics Ground Station

Receive SX1278 LoRa telemetry from a model rocket, watch it live in a browser,
and keep every byte on disk for the post-launch data report (PLDR).

```
 rocket ─LoRa→ ground station ─USB serial→ backend (FastAPI) ─WebSocket→ dashboard (React)
 (ESP32-S3)     (ESP32)                   │
                                          └─ flights/<session>/  (always recording)
                                               └─ flight-NN_<name>/  (operator-declared)
```

Downlink-only prototype. The whole thing is **offline-first** — no CDNs,
self-hosted fonts, map tiles bundled into the repo — because the launch site
has no network.

**New here? Go to [Start here](#start-here). It is two installs and one
command, and it runs with no rocket attached.**

| | |
|---|---|
| [Start here](#start-here) | first run after cloning |
| [Ways to run it](#ways-to-run-it) | every `start.command` mode |
| [Running without hardware](#running-without-hardware) | mock, simulator, replay |
| [Running with real hardware](#running-with-real-hardware) | launch day |
| [Using the dashboard](#using-the-dashboard) | the five views |
| [Two rockets, two channels](#two-rockets-two-channels) | A/B radio channels |
| [What gets written to disk](#what-gets-written-to-disk) | sessions vs flights |
| [HTTP API](#http-api) | endpoints |
| [Wire formats](#wire-formats) | MRCC vs the binary frame |
| [Repo map](#repo-map) | what lives where |
| [Build, verify, report](#build-verify-report) | builds and the PLDR notebook |

---

## Start here

### 1. Install the two tools

| Tool | Version | Install (macOS) |
|---|---|---|
| Node.js + npm | 20+ | `brew install node` |
| Python | 3.12+ | `brew install python` (or system / pyenv) |

Nothing else. No hardware, no Arduino IDE, no rocket — those are only needed to
fly ([`firmware/README.md`](firmware/README.md) covers flashing).

### 2. Run it

```bash
./start.command --demo
```

Or **double-click `start.command` in Finder** — with `--demo` it needs a
terminal, but the bare double-click is the launch-day path.

The first run takes a couple of minutes: it creates the Python venv, installs
the backend deps, installs the dashboard deps, builds the dashboard, starts the
backend against the built-in flight simulator, and opens
**http://127.0.0.1:8000** once the server actually answers. Every later run
skips the steps already done and takes seconds.

Ctrl-C stops everything.

### 3. What you should see

A console with a left rail of five views. The top-bar source tag reads
**● LIVE** (it is a real backend — the *flight* is simulated, not the link), and
a full flight loops forever: pad → boost → apogee → drogue → main → landed.

Click through **Live**, **Map**, **Log**, **Flights**, **Set**. That demo run is
recording, so by the time you look at **Flights** there is a session in the
archive — your own, cut a minute ago.

### 4. What the clone does not include

Four things are deliberately not in git. You do not have to create any of them
by hand; `start.command` does, and this table is here so an empty directory
never reads as a broken clone.

| Missing after clone | What creates it | Why it is ignored |
|---|---|---|
| `backend/.venv/` | first `./start.command` | machine-specific |
| `dashboard/node_modules/` | first `./start.command` | large, lockfile-derived |
| `dashboard/dist/` | `./start.command` (rebuilds when sources are newer) | build output |
| `flights/` | every run — the recorder is always on | per-run data, gets large |

One thing that **is** committed and matters: `dashboard/public/basemap.pmtiles`,
the launch-site map. It is 3 MB of tiles kept in the repo on purpose, so the Map
view works with the network off.

Because `flights/` is empty on a fresh clone, anything in these docs that
replays a recording — `--replay flights/2026-08-19T05-54-40Z` and friends — has
nothing to point at until you have made a recording of your own. Run
`./start.command --demo` for a minute and you will have one.

### If it does not start

| Symptom | Fix |
|---|---|
| `python3 not found` | `brew install python` |
| `npm not found` | `brew install node` |
| `could not install backend deps (no network?)` | first run needs the network, once — do it before leaving for the range |
| `dashboard build failed` | `cd dashboard && npm install && npm run build` to see the real error |
| It waits, then asks about the receiver | expected with no hardware — use `--demo` |
| Browser opens on an old-looking console | stale `dashboard/dist`; touch a source file or `rm -rf dashboard/dist` and rerun |

---

## Ways to run it

`./start.command` is the whole launch-day procedure. Every mode below is the
same script with a different byte source.

| Command | What it does |
|---|---|
| `./start.command` | **live** — find the receiver on USB, serve, open the console |
| `./start.command --demo` | no hardware: the flight simulator, looping |
| `./start.command --replay flights/<session>` | no hardware: replay a real recording |
| `./start.command --dev` | Vite hot reload (`:5180`) in front of a live backend |
| `./start.command --serial /dev/cu.usbserial-0001` | skip USB autodetect |

Also `--no-browser`, `--wait SECONDS` (how long to wait for the receiver before
asking what to do; default 20), and anything `backend.app` takes — `--loop`,
`--fast`, `--loss`, `--no-reset`, `--port`.

**Two things it deliberately will not do.** It never falls back to the simulator
on its own: with no receiver on USB it waits, then asks — a screen full of
moving numbers that came from nowhere is this console's worst failure. And a
second run while one is already serving **opens that one** instead of dying on
"address already in use".

If two boards are plugged in (receiver + flight computer), it lists them and
asks which is which, because reading the wrong port is a blank screen with no
error on it.

Everything below is that same pipeline driven by hand — useful when you are
debugging a piece of it, and what `start.command` is doing underneath. The
one-time setup it performs for you is:

```bash
# backend deps (from repo root)
python3 -m venv backend/.venv
backend/.venv/bin/pip install fastapi uvicorn pyserial

# dashboard deps
cd dashboard && npm install
```

---

## Running without hardware

Three ways. **A** and **B** replay the same simulated flight — the difference is
whether a backend is in the loop. **C** replays a real recorded one.

### A. Dashboard only — mock source (fastest)

The dashboard ships with a built-in flight simulator. No backend, no Python.

```bash
cd dashboard
npm run dev
```

Open the printed URL (e.g. `http://localhost:5173`). The **mock** source is the
default and loops a full flight forever — ideal for UI work and quick demos. The
top-bar source tag reads **◆ MOCK**.

> Force mock explicitly with `?source=mock` (handy if you have set a WS URL).

**What you get:** Live charts, Map, and the Log page all work off the mock
stream. Settings → *Data export* is disabled (no backend session), and
Connection shows `— (mock)`.

### B. Full stack — backend + fake telemetry

This is what `./start.command --demo` runs. It drives the real pipeline
(serial-shaped bytes → `raw.log` → CSV → WebSocket) from the simulator, so
**session files and export work**, exactly like a real flight.

```bash
# 1. build the dashboard so the backend can serve it
cd dashboard && npm run build && cd ..

# 2. start the backend with the fake source, looping for a continuous demo
backend/.venv/bin/python -m backend.app --fake --loop
```

Open **http://127.0.0.1:8000** — one origin serves both the UI and `/ws`. The
source tag reads **● LIVE**, Settings → Connection shows the session id and live
`/stats`, and Data export downloads the session's `telemetry.csv` /
`events.csv` / `raw.log`.

| Flag | Effect |
|---|---|
| `--loop` | replay the flight continuously (best for demos) |
| `--fast` | no realtime pacing (dump a whole flight instantly) |
| `--loss 0.1` | inject 10% packet loss to exercise the loss stats |
| `--host 0.0.0.0 --port 8000` | expose on the LAN |

#### Front-end dev against the live backend

`./start.command --dev` does this in one terminal. By hand it is two:

```bash
# terminal 1
backend/.venv/bin/python -m backend.app --fake --loop
# terminal 2
cd dashboard && npm run dev
```

Then open `http://localhost:5173/?source=ws` (or set
`VITE_WS_URL=ws://localhost:8000/ws` in `dashboard/.env`). CORS is enabled for
any loopback origin, so `/stats` and exports work cross-origin in dev whichever
port Vite lands on (it moves to 5174, 5175… when 5173 is taken).

### C. Replay a recorded flight — the MRCC path without a radio

`--fake` emits **binary** `PROTOCOL.md` frames, but the airborne board downlinks
**ASCII MRCC**, which carries fields the binary frame has no slot for: the
accelerometer axes, RSSI/SNR, pressure, heading. So on `--fake` the Live accel
panel, the RSSI/SNR readouts and the aux row are all structurally empty — not
broken, just unfed. Replaying a recorded `raw.log` streams those real bytes back
through the whole pipeline instead:

```bash
backend/.venv/bin/python -m backend.app --replay flights/<session> --loop
```

**This needs a recording that came from the radio**, and `flights/` is not in
git — so on a fresh clone there is nothing to replay until someone hands you an
archived session or you capture one. (Replaying a `--demo` session works, but it
is the binary simulator again, with the same empty panels.)

Point it at a session directory or the `raw.log` inside it. `--format` is read
from the session's `metadata.json`, so the right codec is picked automatically;
pass `--format` to override. `--loop` and `--fast` work as they do for `--fake`.

Records are replayed at the pace they arrived, with each inter-record gap capped
at 2 s: `raw.log` is opened in append mode per session, so one file can span days
of bench runs, and the real gaps between them are hours of dead air. A torn tail
(from a run that was killed mid-write) ends the replay cleanly — "raw first"
means bytes reach the disk before anything validates them.

This is the only hardware-free way to exercise `shared/protocol/mrcc.py`: the
inferred health bits, `tilt_from_accel`, the `extra` passthrough, and the
null-vs-zero handling for the peripherals MRCC cannot see (SD/PYRO/VBAT read
`—`, not a fabricated failure).

---

## Running with real hardware

Wire up the ground station (see [`firmware/README.md`](firmware/README.md)) and
run **`./start.command`** — it does everything below for you. By hand:

```bash
cd dashboard && npm run build && cd ..          # once, or after UI changes
backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX
```

Open **http://127.0.0.1:8000** on the operator laptop. Everything else is
identical to the full-stack demo — the only change is the byte source.

- Find the port: `ls /dev/tty.usbserial-*` (macOS) or `ls /dev/ttyUSB*` (Linux).
- Baud must match the firmware. `MRCC_GroundStation` opens USB at **115200**,
  which is the default, so `--baud` is only needed if you change the firmware.
- Close the Arduino Serial Monitor first. Two readers on one port is a blank
  console with no error on it.
- Raw bytes are written to `flights/<session>/raw.log` **before** decoding, so a
  parser bug can never lose a flight — the log is replayable.

---

## Using the dashboard

The left rail switches five views:

| View | What it shows |
|---|---|
| **Live** | Altitude / vertical-speed / tilt charts, KPI strip, GO/NO-GO, flight-state timeline. The flight screen — one glance, no scroll. |
| **Map** | Offline PMTiles basemap with the live ground track + last-known fix (distance/bearing from the pad). |
| **Log** | **Left:** a live raw-telemetry table (newest frame first). **Right:** a mission event log — state transitions, pyro/deploy, link stale/restored, no-deploy alarm, session start/reset. **Clear** (press twice) empties both panes — your working copy only, never the recorded `telemetry.csv`. |
| **Flights** | The archive on disk: every session and every operator-declared flight, newest first, each labelled with its provenance (live / replay / sim). Open one for its log and telemetry tails, download its files, or delete it. |
| **Set** | Connection & session status, alarm/audio, display, and data export. |

### The top bar

- **Mission picker** — which vehicle you are listening to. It is the channel
  switch; see [Two rockets, two channels](#two-rockets-two-channels).
- **REC** — cut a flight folder out of the always-on session recording, with an
  optional name. Press it again to close the flight.
- **Source tag** — `● LIVE` (backend) or `◆ MOCK` (in-browser simulator).

### Settings

- **Connection & session** — current source, socket state, session id, frames
  decoded, CRC errors, loss. Switch **Mock ⇄ Live** (reloads the page, since the
  source is chosen at load). Shows whether the backend is reachable.
- **Alarms & audio** — master **buzzer** toggle, per-alarm **arming**
  (link-stale, no-deploy), and a **test beep**. Audio unlocks on your first
  click/keypress (browser autoplay policy); it is silent by default.
- **Display** — **high-contrast** (sunlight) mode, telemetry-table **density**,
  and **row cap**. Saved to `localStorage`, so they persist across reloads.
- **Data export** — download the active session's `telemetry.csv`, `events.csv`,
  `raw.log`, and a `stats.json` snapshot for the PLDR notebook.

### How the dashboard picks its data

Resolved once at page load, in this order:

1. `?source=mock` → always the built-in simulator.
2. `?source=ws` **or** `VITE_WS_URL` set → the WebSocket
   (`VITE_WS_URL`, else `ws://<host>:8000/ws`).
3. otherwise → **mock**.

---

## Two rockets, two channels

Each airframe gets its own radio channel — `VEHICLE_A` is 433.3 MHz, `VEHICLE_B`
is 434.1 MHz — and **the rocket is flashed for its channel; the ground station
is retuned to match.** LoRa does not pair: on a shared channel each ground
station decodes the *other* rocket's frames while the two transmitters collide
on air. [`firmware/README.md`](firmware/README.md) has the flashing table and
the arithmetic.

The console calls them by mission name — **A1R** flies on channel A, **A2R** on
channel B. The Mission picker in the top bar is the channel switch: choosing a
vehicle sends the box the same A/B key the serial monitor takes, so the name on
the header and the channel in the radio are one setting.

The picker shows what you asked for; beside it, the box's own answer, which is
the only thing that proves the receiver moved. Until the receiver has announced
itself the channel reads as unknown, never as "probably A".

Switching needs a `--serial` source — there is no receiver to retune on `--demo`
or `--replay`, and the control says so rather than failing silently.

---

## What gets written to disk

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

---

## HTTP API

Same origin as the served dashboard.

| Endpoint | Purpose |
|---|---|
| `GET /ws` | live decoded-frame stream (WebSocket) |
| `GET /stats` | session id, active flight, frames decoded, CRC errors, loss % |
| `GET /session` | active session id + which files exist |
| `GET /session/{name}` | download an active-session file |
| `GET /gs` | receiver's current channel, the channels available, and whether this session can switch |
| `POST /gs/channel` | retune the receiver (`{"channel": "A"}`); serial sources only |
| `GET /flight` | recording state + completed flights this run |
| `POST /flight/start` | cut a flight folder (JSON body, optional `label`) |
| `POST /flight/stop` | close the flight being recorded |
| `GET /flight/files/{name}` | download from the flight being recorded |
| `GET /flights` | every recorded session and flight on disk, newest first |
| `GET /flights/{session}/{flight}` | one recording + bounded log/telemetry tails |
| `GET /flights/{session}/{flight}/files/{name}` | download an archived file |
| `DELETE /flights/{session}/{flight}` | permanently delete one recording |

`POST` requires an `application/json` content type — that is what forces a CORS
preflight, so another origin cannot stop a recording mid-flight. Downloads are
fixed-length snapshots (files are served while still being appended to).

`POST /gs/channel` deliberately does not report the new channel back: the
receiver is the authority on where it is listening, and the console picks that
up from the receiver's own output a moment later.

---

## Wire formats

The link carries **MRCC**, an ASCII `key=value` line
(`MRCC,PKT=207,T=207.5,…`) that `shared/protocol/mrcc.py` decodes and maps onto
`packet.Telemetry`. The 32-byte binary frame in
[`shared/protocol/PROTOCOL.md`](shared/protocol/PROTOCOL.md) is the format this
project originally designed; only `--fake` still produces it. `--format`
defaults per source, so a live run needs no flag.

---

## Repo map

| Path | What it is |
|---|---|
| `start.command` | the launcher — deps, build, serial link, backend and console in one action |
| `backend/` | reads bytes (serial in prod, a simulator in dev), logs raw bytes first, decodes, writes the session CSVs, broadcasts each frame over `/ws`, and serves the built dashboard |
| `dashboard/` | Vite + React + TS console: Live, Map, Log, Flights, Settings |
| `firmware/` | vehicle sketches `MRCC_FlightComputer_A` / `MRCC_FlightComputer_B` (ESP32-S3: sensors, filters, flight state, pyro, SD log, downlink) and `MRCC_GroundStation` (ESP32: receives, prints to USB). Plus `SD_Doctor` and host-side filter tooling. **Read `firmware/README.md` before flashing.** |
| `shared/protocol/` | `mrcc.py` decodes the ASCII downlink the vehicle actually sends; `packet.py` is the internal `Telemetry` shape everything downstream consumes; `PROTOCOL.md` documents it |
| `shared/fake_telemetry.py` | simulates a full flight (pad → boost → apogee → drogue → main → landed) so nothing needs hardware to develop against |
| `flights/` | recorded sessions and flights (not in git) |
| `notebooks/` | the PLDR template that turns a recovered flight into the report |

---

## Build, verify, report

```bash
cd dashboard && npm run build     # tsc typecheck + offline production bundle
cd dashboard && npm run lint      # oxlint
backend/.venv/bin/python -m backend.tests
```

The build must run with the network off — no CDNs, fonts and map tiles are
bundled. The backend serves the resulting `dashboard/dist/` at `/`.

Post-flight, point the PLDR notebook at a session folder to generate the flight
report. It needs a few deps the backend venv does not carry:

```bash
backend/.venv/bin/pip install pandas matplotlib jupyter
backend/.venv/bin/python -m notebooks.make_fake_flight   # a flight to analyse, if you have none
backend/.venv/bin/jupyter notebook notebooks/flight_report.ipynb
```

See [`notebooks/README.md`](notebooks/README.md) for what it reports.

---

## Further reading

| Doc | Depth |
|---|---|
| [`firmware/README.md`](firmware/README.md) | flashing, channels, wiring — read before touching a board |
| [`HANDOFF.md`](HANDOFF.md) | architecture + decisions (note its staleness banner) |
| [`DESIGN_SPECS.md`](DESIGN_SPECS.md) | the dashboard UI spec |
| [`shared/protocol/PROTOCOL.md`](shared/protocol/PROTOCOL.md) | the binary frame |
| [`GROUND_STATION_PLAN.md`](GROUND_STATION_PLAN.md) | the original build plan |
| [`backend/README.md`](backend/README.md) · [`dashboard/README.md`](dashboard/README.md) | per-package detail |
