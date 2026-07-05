# Design

> Visual system for the rocket ground-station dashboard. Seeded from
> `DESIGN_SPECS.md` §4 before implementation; re-run `/impeccable document`
> against the real tokens once `dashboard/` has code, to capture exact values.

## Theme

**Near-black mission-control console.** Dark by requirement, not fashion: the
operator reads this on a laptop in bright outdoor light at a launch site, watching
fast-moving telemetry. A dark instrument panel keeps bright semantic colors (a red
alarm, a green nominal light) maximally legible and keeps the screen from glaring.
The surface is a blue-tinted near-black — an oscilloscope / Grafana-dark / SpaceX
telemetry-strip register, dense and quiet. Panels are divided by 1px hairlines; no
floating shadow cards, no elevation theater.

Color strategy: **restrained** — a near-black tinted-neutral field, with saturated
color admitted only to report state (nominal / caution / alarm / data).

## Colors

OKLCH throughout. Backgrounds are a cool blue-tinted near-black ramp; text is a
near-white cool gray ramp; the four semantic hues are the only saturated colors and
each means exactly one thing.

### Surfaces (blue-tinted near-black ramp)
- `--bg`          `oklch(0.17 0.012 250)`  — app background (≈ `#0a0e12`)
- `--surface`     `oklch(0.21 0.014 250)`  — panel field
- `--surface-2`   `oklch(0.25 0.016 248)`  — inset / raised row, sparingly
- `--border`      `oklch(0.30 0.018 245)`  — 1px hairline dividers (the main structure)
- `--border-strong` `oklch(0.37 0.022 243)` — emphasized division / focus ring base

### Ink (cool near-white ramp)
- `--ink`         `oklch(0.94 0.008 240)`  — primary data & headings (≈ `#e6edf3`)
- `--ink-dim`     `oklch(0.72 0.018 245)`  — secondary values, active labels (≥4.5:1 on `--bg`)
- `--ink-mute`    `oklch(0.60 0.020 245)`  — quiet labels/units; never body text

### Semantic (the only saturated colors — each carries one meaning)
- `--nominal`     `oklch(0.78 0.16 158)`   — green: go / healthy / in-range
- `--caution`     `oklch(0.80 0.13 85)`    — amber: caution / degraded / near-limit
- `--alarm`       `oklch(0.64 0.21 25)`    — red: alarm / fault / out-of-range
- `--data`        `oklch(0.77 0.11 205)`   — cyan: live data accent (chart line, active fix)

Each semantic color also has a low-alpha fill sibling (e.g. `--alarm-bg`) for status
rows / the alarm bar, derived as the same hue at low opacity — no new hues.

**Dual-encoding rule:** color never rides alone. Every stateful indicator pairs its
color with a glyph/shape (●/▲/■), a text label, or position, so it survives sunlight
and color-blindness. Decorative color budget = 0.

## Typography

Contrast axis = **weight + case within one monospace family**, not two similar sans.

- **Data / numbers / tabular:** `JetBrains Mono`, self-hosted (woff2 in the bundle,
  no CDN). Used for every value, timer, coordinate, and the event log. Enable
  `font-variant-numeric: tabular-nums` so digits don't jitter as they update at 4 Hz.
- **Labels:** same family, **small, uppercase, letter-spacing ~0.08em**, in `--ink-mute`
  — the classic instrument-panel label (`ALT`, `VSPEED`, `LINK`).
- **Readout values:** large weight-500/600, `--ink`, tabular. The big-number column and
  the status bar values are the size hierarchy; labels stay small beside them.
- No display/hero type. Largest element is a telemetry readout, not a headline.
- IBM Plex Mono is an acceptable fallback family if JetBrains Mono is unavailable.

## Spacing & Density

NASA/Open-MCT density, not marketing whitespace. Tight, consistent gutters (a 4px
base step: 4 / 8 / 12 / 16), hairline dividers doing the separating. One screen holds
every panel at 1080p with zero navigation. Rhythm comes from grouping and dividers,
not large empty margins.

## Components

- **Status bar (top):** full-width strip — flight state · T+ timer · link age · loss %.
  Goes solid `--alarm` and flashes on a genuine alarm (past-apogee-no-deploy, link stale).
- **Streaming chart:** uPlot, `--data` cyan trace on `--bg`, hairline axes in `--border`,
  `--ink-mute` tick labels. Apogee marker. No gridline clutter; no chart chrome.
- **Big-number readouts:** label (small uppercase) + value (large tabular) + optional
  state glyph. Separated by hairlines in a column, not boxed as cards.
- **Status lights:** glyph + label + semantic color (go/no-go, ARMED, continuity).
- **Event log:** monospace rows, timestamp + event, newest at top.
- shadcn/ui is used only for un-styled primitives (dialog, button, badge shells);
  its default visual language is overridden by these tokens.

## Motion

Calm by default — nothing idle animates while the 4 Hz stream runs (it would compete
for frames and attention). Motion is reserved for alarms and must be unmissable:
the status bar flash, a pulsing stale-link indicator. Ease-out only. Every animation
has a `prefers-reduced-motion: reduce` alternative that keeps the signal (solid alarm
fill + enlarged text) without the animation — a dropped alarm is a safety bug.
