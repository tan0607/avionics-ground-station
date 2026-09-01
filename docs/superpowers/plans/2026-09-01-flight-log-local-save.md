# Per-flight local recording completion plan

## Goal

Complete the interrupted operator-declared flight recorder so each explicit
REC/STOP span is saved under its own local `flights/<session>/flight-NN/`
folder, including raw bytes, decoded telemetry, state events, metadata, and the
human-readable mission log.

## Scope

- Preserve the always-on session recording as the recovery copy.
- Finish the dashboard integration for the existing recorder hook and REC
  control.
- Verify start/stop API behavior and exact fan-out boundaries for every saved
  flight file.
- Preserve all unrelated worktree changes.

## Expected files

- `dashboard/src/App.tsx`
- `dashboard/src/hooks/useFlightRecorder.ts`
- `dashboard/src/components/TopBar.tsx`
- `backend/session.py`
- `backend/app.py`
- `backend/tests.py`

Only files that need a tested correction will be changed.

## Verification

- `backend/.venv/bin/python -m backend.tests`
- `npm run build` from `dashboard/`
- `npm run lint` from `dashboard/`
- A temporary-root API smoke test that starts a flight, ingests data, stops it,
  and inspects the resulting files without touching the repository's real
  `flights/` data.
