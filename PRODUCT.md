# Product

## Register

product

## Users

A **single rocketry operator** running the ground station at a launch site — no
office, no network, often bright outdoor light, frequently no free hand during a
flight. During a live flight they are watching a rocket climb and descend in
seconds and simultaneously deciding go/no-go and (phase 2) whether to fire a
backup recovery charge. After the flight they use the same data to walk to the
rocket's landing coordinates and to produce a post-launch data report (PLDR).

Context that shapes every screen:
- **Glanceable under stress.** Altitude, flight state, link health and last-known
  coordinate must be readable in ~1 second without hunting or navigating.
- **Zero navigation during flight.** One screen holds every critical panel; there
  is nobody free to click menus mid-flight.
- **The data is the interface.** Numbers, states and a live curve — not marketing
  surface. This serves a task; it is not a thing to admire.

## Product Purpose

A **rocket avionics ground station dashboard**. It receives 4 Hz LoRa telemetry
(decoded from the shared 32-byte packet, see `shared/protocol/PROTOCOL.md`) over a
native WebSocket from the Python backend and turns it into a live mission-control
view: streaming altitude/vertical-speed charts, big-number readouts, flight-state
and link-health indicators, an event log, and an offline GPS map for recovery.

Success = the operator can, at a glance, (1) trust the link is live, (2) read the
current flight state and altitude, (3) be alarmed automatically when something is
wrong (e.g. past apogee with no chute), and (4) after landing, find the rocket and
export clean data. It must run **fully offline** — a `npm run build` bundle works
with the network disabled; no CDN fonts, tiles, or scripts.

## Brand Personality

Mission control, not marketing. Three words: **instrument, legible, calm.**
The interface should feel like an oscilloscope or a NASA/SpaceX telemetry console —
dense, precise, quiet until something demands attention, then unambiguous. It earns
trust by being boring and correct. No personality flourishes compete with the data.

## Anti-references

Hard rejects (from `DESIGN_SPECS.md` §4 — violating these bounces the work):
- **SaaS landing-page / marketing-dashboard aesthetics.** Generous whitespace,
  hero metrics, friendly rounded card grids. This is an ops console; density wins.
- **Purple/indigo gradients, glassmorphism, gradient text, decorative shadows.**
- **Default shadcn look** — rounded floating shadow-card sea, default radii.
- **Inter / Roboto / default geometric sans for data.** Data is monospace.
- **Decorative color.** Color is semantic only; no color that doesn't mean something.

Reference gestalt to move toward: NASA Open MCT, SpaceX webcast telemetry strip,
Grafana dark — information density and hierarchy, not their exact styling.

## Design Principles

1. **The data is the hero.** Every pixel of chrome that isn't a value, a label, or a
   1px divider is suspect. Panels are separated by hairlines, not floated on shadows.
2. **Color carries meaning, nothing else.** green = nominal, amber = caution,
   red = alarm, cyan/white = data. If a color isn't reporting state, it doesn't ship.
3. **Glanceable beats complete.** The one-second-scan values (altitude, state, link,
   coordinate) get size and position; everything else is secondary and quieter.
4. **Dual-encode critical state.** Never rely on color alone — pair it with shape,
   label, position, or motion so it survives sunlight and color-blindness.
5. **Calm until it matters.** No idle animation competing with the 4 Hz stream. Motion
   is reserved for genuine alarms (link stale, no-deploy), where it must be impossible
   to miss.
6. **Offline is a correctness requirement, not a nicety.** Self-host everything.

## Accessibility & Inclusion

- **Outdoor legibility:** high contrast throughout; body/data text ≥ 4.5:1, large
  readouts well above that. No light-gray-on-dark "for elegance."
- **Color-blind safe:** every state that uses color also uses a distinct shape/label/
  position (dual-encoding), so red/amber/green are never the sole signal.
- **Reduced motion:** alarm motion (flashing bar, pulsing indicators) must respect
  `prefers-reduced-motion` with a non-animated but still unmistakable alternative
  (solid alarm fill + text), never a silently-dropped signal.
- Target **WCAG 2.1 AA** for contrast and non-text indicators.
