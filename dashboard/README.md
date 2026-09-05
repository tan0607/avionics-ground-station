# Dashboard

The operator console: a Vite + React + TypeScript app that the backend serves at
`/` and feeds over a WebSocket. Run it from the repo root — see the main
[README](../README.md) for the three ways to start it (mock, fake backend, replay).

**Offline-first is a hard requirement, not a preference.** The launch site has no
network. No CDNs, no runtime font fetches, no remote tiles: fonts are self-hosted
and the basemap is a bundled `.pmtiles` cut. `npm run build` must succeed with the
network off, and a dependency that phones home at runtime is a bug.

## Views

| View | What it is for |
|---|---|
| **Live** | flight state, KPIs, altitude + sensor charts, subsystem health, go/no-go |
| **Map** | offline ground track against the bundled basemap |
| **Log** | raw frames and the event log, side by side |
| **Flights** | recorded flights cut with the REC button, replayable |
| **Settings** | source selection, units, thresholds |

## Switching vehicles and recording

Selecting A1R (A) or A2R (B), from either the header or Settings, opens a
confirmation. Cancel keeps the current mission and graphs. Confirm clears the
live graphs/readouts and waits for the receiver to report the selected channel
before accepting new telemetry. Saved logs stay intact.

If a flight is recording, confirmation saves and stops it before retuning. A
failed stop leaves the current mission selected and shows the error. Start a
new flight recording after the switch; REC stays disabled while the receiver's
channel is unconfirmed.

In a serial session, a persistent amber reminder appears on every view whenever
no flight is recording. Its **Start recording A1R/A2R** button uses a vehicle/time
label. The top-bar name field and REC button remain available for custom names.
The reminder clears only after the backend confirms recording; unavailable
recorder status is shown explicitly. Mock, fake and replay sessions do not show
the launch reminder. The backend's continuous session log remains separate from
these operator-declared flight folders.

## Where the data comes from

`useTelemetry` opens the WebSocket and owns the connection state; the charts read
from `useSensorSeries`, which keeps fixed-length ring buffers so a long session
cannot grow unbounded. `lib/protocol.ts` mirrors the backend's frame shape — it is
the one place the wire contract is written down on this side, so change it with
`shared/protocol/` in view, never independently.

`lib/mock.ts` drives the UI with no backend at all, which is the fastest loop for
visual work.

## Commands

```bash
npm run dev
```

```bash
npm run build
```

```bash
npm test
npm run lint
```

For isolated browser checks, build first, then run from the repo root:
`backend/.venv/bin/python dashboard/tests/preview_server.py`. Open
`http://127.0.0.1:8765`; its API and telemetry are synthetic and never access a
serial device. A switch pauses the fixture stream so the empty charts can be
inspected; POST `{"paused":false}` to `/_test/control` to resume.

`build` runs the TypeScript check first, so a type error fails the build rather
than shipping. Output lands in `dist/`, which the backend serves.

## Conventions

Colors, spacing and typography come from the design tokens in `DESIGN.md` at the
repo root. Charts read CSS variables where they can; `FlightMap` cannot (canvas
does not resolve CSS vars) and mirrors the token values in `lib/`, so those two
have to be changed together.
