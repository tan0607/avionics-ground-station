# Live dashboard right-panel scrolling — 2026-09-09

Goal: keep the top bars, readouts and charts stationary; scroll only the right-hand GO / NO-GO, PERIPHERALS and FLIGHT STATE column.

Scope: dashboard/src/App.tsx layout only. Preserve existing unrelated changes and all telemetry/firmware behavior.

1. Run a real-browser regression check on the current build at a short desktop viewport: right column must scroll while top readouts, charts and document stay fixed. Confirm failure before changing code.
2. Bound the Live grid to the remaining viewport height and move vertical overflow to a keyboard-focusable named right-hand region. Keep card content accessible.
3. Run dashboard tests, lint and build. Repeat browser wheel/keyboard and geometry checks at 1280×600, 1470×804 and 1920×1080; inspect a screenshot.

Use a separate browser page in mock mode with backend requests intercepted. No backend restart or recording/channel commands.

## Validation

- Before: 1280×600 browser check failed because the right column had `overflow-y: visible`; the 529 px grid extended below the viewport.
- After: wheel and keyboard scrolling passed at 1280×600, 1470×804 and 1920×1080. At 1280×600 the right region is 351 px high and scrolls 178 px; all eight Flight State labels are reachable. Top readout/chart rectangles and document scroll position remain unchanged. Larger viewports fit the content without unnecessary scrolling.
- Inspected the rendered 1280×600 screenshot; charts and top rows remain visible with the right region scrolled to the bottom.
- Build passed. Lint passed with four existing Fast Refresh warnings; build retains its bundle-size advisory.
- The first full test run had 20 passing tests and two failing Archive tests in concurrently modified files. Those tests do not import App.tsx.
- Final full test rerun after the concurrent Archive changes: 22/22 passed.
