# Flights alignment

Scope: presentation only in `dashboard/src/components/FlightsView.tsx`; preserve archive data, selection, filtering, recording, download and deletion behavior.

- Give archive duration, row count and byte size stable columns; put child-flight count beside the folder identifier.
- Separate detail identity from download actions so long paths cannot crowd controls.
- Let timestamps wrap and allocate more width to the first metric.
- Right-align known numeric telemetry columns, including their headings.
- Follow-up from user screenshot: keep child indentation on title/identifier only; use identical outer padding for every statistics row and right-align row counts. Previous visual inspection missed the nested-row offset.

Verification: dashboard test suite and production build, then inspect the built Flights page in Chrome using existing archive data. No new tests for this reversible CSS-only adjustment; check layout visually. Do not restart backend or operate hardware.

Result: `npm test` passed all 20 tests (5 files); `npm run build` passed with the existing bundle-size advisory. Inspected the production page in Chrome at 1470px: archive statistics keep stable columns, download controls occupy their own row, the full first-frame timestamp is visible, and numeric telemetry headings/cells align right. Narrow-screen layout was not separately exercised. Existing firmware test edits belong to other work and were preserved.
