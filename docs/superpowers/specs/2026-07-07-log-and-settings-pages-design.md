# Log & Settings Pages — Design

> Ground-station dashboard: wire up the two remaining SideNav views (Log, Set).
> Brainstormed & approved 2026-07-07. Aesthetic contract = DESIGN_SPECS §4
> (near-black, 1px hairline, JetBrains Mono data, semantic color only, no AI slop,
> one viewport). The frozen data layer stays frozen.

## Goal

Turn the two disabled SideNav placeholders (`Log`, `Set`) into working views:

- **Log** — a live, in-memory view with two parts: a **raw telemetry table**
  (shadcn `Table`) and an **event log** (state transitions, deploys, link
  events, alarms). Derived from the running stream; no history/backend browse.
- **Set(tings)** — connection & session status, alarm/audio prefs, display
  prefs, and data export.

## Hard constraints (carried from HANDOFF / DESIGN_SPECS)

- **Frozen data layer** — `useTelemetry.ts`, `lib/{mock,protocol,wsClient}.ts`
  are KEEP-UNCHANGED. New work *piggybacks* on the telemetry snapshot (the same
  way `useSensorSeries` does); it never edits the source.
- **Offline-first** — no CDNs, no new runtime deps. shadcn `Table` is a styled
  `<table>` (radix already present), so no dependency is added.
- **One viewport, no page scroll** — panels scroll internally.
- **Metric only** — imperial units deferred (decided 2026-07-07); the widest
  ripple, low value for a metric-thinking team.

## Architecture (isolated units, one job each)

| Unit | File | Job | Depends on |
|---|---|---|---|
| View wiring | `App.tsx`, `SideNav.tsx` | 4-view switch; un-disable Log/Set | — |
| Settings store | `hooks/useSettings.tsx` | Context + localStorage prefs | localStorage |
| Log derivation | `hooks/useTelemetryLog.ts` | frames→rows + derived events | telemetry snapshot |
| Alarm audio | `hooks/useAlarmSound.ts` | Web Audio buzzer, gated by prefs | telemetry + settings |
| Log page | `components/LogView.tsx` | table + event-log presentation | useTelemetryLog, settings |
| Settings page | `components/SettingsView.tsx` | 4 setting cards | settings, telemetry, /stats |
| Table primitive | `components/ui/table.tsx` | shadcn table styled to the theme | cn |
| Export routes | `backend/app.py` | serve active-session files | rt.session (additive) |

### 1. View wiring

- `ViewId = "live" | "map" | "log" | "settings"`.
- SideNav: `Log`/`Set` items get real ids, `disabled` removed.
- `App.tsx`: refactor the `live ? … : map` ternary into a per-view render map.
  `TopBar` renders above every view; `KpiRow` stays live-only.
- `SettingsProvider` wraps `<App/>` in `main.tsx`; `useAlarmSound` + the
  high-contrast root class mount in `App` (they need both telemetry + settings).

### 2. Settings store — `hooks/useSettings.tsx`

React Context + `localStorage` key `apex.settings.v1`. Read-with-defaults-merge
on init (unknown/old blobs fall back to defaults → forward compatible).
Write-through on every change.

```ts
interface Settings {
  highContrast: boolean
  table: { density: "comfortable" | "compact"; rowCap: number }
  buzzer: boolean
  alarms: { linkStale: boolean; noDeploy: boolean }
}
```

`highContrast` toggles a `data-contrast="high"` (or `.contrast-high`) class on
the root that bumps a few existing CSS vars — no new palette.

### 3. Log derivation — `hooks/useTelemetryLog.ts`

Piggybacks on the telemetry state (arg, like `useSensorSeries`). Per new frame
(deduped by `seq`):

- push a **row** into a capped ring buffer (`rowCap`, newest-first for display);
- derive **events** by diffing successive snapshots:
  - session start (first frame),
  - flight-state transition (`flightState` changed) → `info`,
  - pyro rising edge (`!prev.pyroFired && frame.pyroFired`) → `caution`,
  - link transition (`live↔stale↔down` from `telemetry.link`) → caution/alarm/nominal,
  - no-deploy alarm rising edge (`telemetry.alarms.noDeploy`) → `alarm`,
  - onboard-clock jump-back (`frame.onboardMs + RESET < prev.onboardMs`) →
    "session reset": push a marker **and clear rows** (matches the live view's
    per-flight scoping).

Returns `{ rows, events, rev }`; `rev` bumps so consumers diff cheaply without
comparing arrays (mirrors the chart-series `rev` convention).

Event shape: `{ id, tPlusSec | onboardMs, kind, severity, message }`.

### 4. Alarm audio — `hooks/useAlarmSound.ts`

Web Audio `OscillatorGain` beep. Fires on an armed alarm's rising edge when
`settings.buzzer` is on and that alarm is armed. `AudioContext` is created
lazily on first user gesture (autoplay policy); the Settings "test buzzer"
button doubles as the unlock gesture. Silent by default.

### 5. Log page — `components/LogView.tsx`

One viewport, two panels:

- **Main**: telemetry `Table` — newest-first, sticky header, internal scroll,
  columns `seq · T+ · state · alt(m) · vspd(m/s) · tilt(°) · sats/fix · vbat(V) ·
  flags`. Density + row-cap from settings. `tabular-nums`, mono.
- **Side**: event log — newest-first, severity-colored rows (icon + T+ +
  message), internal scroll.

### 6. Settings page — `components/SettingsView.tsx`

Four flat hairline cards (reuse `Card`):

1. **Connection & session** — read-only: source, socket status, session id,
   frames decoded, CRC errors, loss % (polls `/stats` ~2 s). Mock/Live switch =
   `?source=mock` / `?source=ws` reload links. Effective WS URL shown.
2. **Alarms & audio** — buzzer toggle, per-alarm arm toggles, "test buzzer".
3. **Display** — high-contrast toggle, table density, row-cap.
4. **Data export** — download `telemetry.csv` / `events.csv` / `raw.log` /
   `stats.json` for the active session (links to the backend origin).

### 7. Backend export routes — `backend/app.py` (additive only)

Registered **before** the static catch-all mount, serving the active session
(`rt.session.dir`) via `FileResponse`:

- `GET /session` → `{ id, dir, files:[…] }` (also lets the frontend detect a
  live backend vs mock).
- `GET /session/telemetry.csv`, `/session/events.csv`, `/session/raw.log`.
- `/stats` already exists (JSON export).

Guard when `rt.session is None` (503/JSON note). No edits to
`sources.py`/`session.py`/`loss.py`/`wire.py`.

## Error / edge handling

- **Mock source** (no backend): `/stats` + export fetches fail → Connection card
  shows "backend not reachable (mock source)"; export buttons disabled with a
  hint. The dashboard still fully works on mock.
- **No frames yet**: Log table + event log show an "awaiting link" empty state.
- **localStorage unavailable / corrupt**: `useSettings` catches and falls back to
  in-memory defaults.
- **AudioContext blocked**: buzzer no-ops until the first gesture; never throws.
- **Session reset** while on Log: rows clear, a reset marker is logged.

## Testing / acceptance

- `npm run build` clean (typecheck passes), fully offline.
- Mock flight: Log table streams newest-first and caps; event log shows PAD→…→
  LANDED transitions, drogue/main pyro, and any link/no-deploy events; loop →
  session-reset marker + rows clear.
- Settings: contrast toggle visibly changes contrast and persists across reload;
  density/row-cap affect the table; buzzer test beeps; arming gates the buzzer.
- Against the real backend (`backend.app --fake`): Connection card shows live
  session/stats; export buttons download the session files.
- Data layer files unchanged (git diff touches none of the frozen set).

## Out of scope (YAGNI)

- Historical session browser / replay.
- Imperial units.
- Uplink / command send.
