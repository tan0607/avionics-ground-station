# Restore Live dashboard scrolling — 2026-09-08

Problem: on the current 1470 x 804 browser viewport, the Flight State card is compressed to 52.25 px and clips its list. The app has a fixed viewport shell with overflow hidden and no scrolling ancestor for Live content. Extra status/reminder rows consume the height available to the right column.

Scope: Live layout in dashboard/src/App.tsx and intrinsic minimum sizing in components/FlightTimeline.tsx. Preserve current colors, panels, telemetry, source, recording and all firmware work. Do not restart the backend or touch serial connections.

Plan:
1. Record failing real-browser visibility/scroll check before edits (no jsdom CSS assertions).
2. Give Live content one bounded vertical scroll container; let the grid/right-column/timeline retain intrinsic content height while charts continue filling taller windows.
3. Run dashboard tests, lint and production build. The existing backend serves dashboard/dist; refreshing the page loads the new build without backend restart.
4. Verify scroll reaches all eight flight states in the actual dashboard, including short and tall viewports; restore viewport afterward.

Boundary: frontend layout only, no state-machine or sensor change. Refresh clears browser-local chart history; backend recording/session files persist. Current UI indicates no active flight recording.

## Validation results

- Before edit: real browser check failed (`allStatesInsideCard=false`, no scroll ancestor); timeline card varied from 28.25–52.25 px as onboard-status rows appeared.
- After edit: original window scrollTop advanced from 0 to 115 through an actual scroll action. Timeline is 143 px and all eight phase labels are inside the card.
- 1280 x 600: Live scroll area 505 px, content 800 px, all labels retained; keyboard End advanced the scroll position.
- 1920 x 1080: content fits the 985 px Live region without vertical overflow; timeline retains all labels in the tall layout.
- Viewport override reset. Final original window 1470 x 780: timeline top 628.75 / bottom 771.75, fully visible after scrolling. UI reports current state LANDED; this is the received test telemetry, not independently verified motion.
- `npm run test`: 20/20 passed. `npm run build`: passed. `npm run lint`: passed with four existing only-export-components warnings in untouched useSettings.tsx/button.tsx. Build retains its existing large-chunk advisory.
- Existing dashboard tab refreshed to the rebuilt dist. No backend restart, recording command, channel change or serial action.
