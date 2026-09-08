# Sync backup-time test contract

## Goal

Finish the requested 19 s to 16 s backup-time change by updating the one
remaining full-suite simulation assertion that still expects the old deadline.

## Scope

- Update `firmware/tests/test_simulation_export.py` from 19,050 ms to the
  current 16,050 ms backup observation.
- Preserve historical validation documents that intentionally describe the
  former 19 s behavior.

## Verification

- Run the focused simulation-export test and the full firmware unittest suite.
- Re-run the Dashboard test, lint, and production build, HandMotionTest suite,
  and all four ESP32-S3 compile checks before commit.
- Confirm `git diff --check`, local/remote commit parity, and a clean worktree
  after push.

## Constraint

This synchronizes a test contract only; it does not change launch detection,
barometric apogee logic, arming, reset recovery, pulse duration, or hardware
behavior.
