# Archive recording stability

Preserve existing FlightsView edits and console styling. No backend, firmware, hardware or archive-file mutations.

1. Add failing UI/hook tests for active empty sessions, persistent selection on insertion, provenance while recording, and background detail refresh.
2. Align archive rows on shared columns with a reserved hierarchy gutter and recording slot.
3. Anchor the visible row during insertions and offer a new-recordings jump while browsing history.
4. Serialize archive/detail reads, retain detail during background refresh, and refresh selected detail when its index summary changes (including stop).
5. Run dashboard tests/build/lint and inspect desktop/narrow browser layouts; use mocked requests for insertion and refresh scenarios.

Files: dashboard/src/components/FlightsView.tsx, dashboard/src/hooks/useRecordedFlights.ts, dashboard/tests/archive.test.tsx.

Verification completed:
- 24 dashboard tests pass, including active empty recording visibility/provenance, persistent selection, simulated scroll geometry on insertion and explicit jump, background detail updates on stop, and deletion versus an older poll response.
- Production build passes; existing bundle-size advisory remains. Lint reports only the four existing Fast Refresh warnings.
- Inspected the latest production bundle against the existing archive in Chrome at the default desktop viewport and 390 x 844. Row columns and nested title/ID alignment are consistent, REC and LIVE coexist, and download controls wrap on narrow screens. Restored the viewport afterward.
- Insertion/stop/race behavior is verified through mocked tests, not a new hardware recording. No serial operations, backend restart, or archive deletion was performed.
