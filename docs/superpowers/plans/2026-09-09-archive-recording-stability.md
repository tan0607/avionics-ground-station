# Archive recording stability

Preserve existing FlightsView edits and console styling. No backend, firmware, hardware or archive-file mutations.

1. Add failing UI/hook tests for active empty sessions, persistent selection on insertion, provenance while recording, and background detail refresh.
2. Align archive rows on shared columns with a reserved hierarchy gutter and recording slot.
3. Anchor the visible row during insertions and offer a new-recordings jump while browsing history.
4. Serialize archive/detail reads, retain detail during background refresh, and refresh selected detail when its index summary changes (including stop).
5. Run dashboard tests/build/lint and inspect desktop/narrow browser layouts with mocked archive data.

Files: dashboard/src/components/FlightsView.tsx, dashboard/src/hooks/useRecordedFlights.ts, dashboard/tests/archive.test.tsx.
