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

`build` runs the TypeScript check first, so a type error fails the build rather
than shipping. Output lands in `dist/`, which the backend serves.

## Conventions

Colors, spacing and typography come from the design tokens in `DESIGN.md` at the
repo root. Charts read CSS variables where they can; `FlightMap` cannot (canvas
does not resolve CSS vars) and mirrors the token values in `lib/`, so those two
have to be changed together.
