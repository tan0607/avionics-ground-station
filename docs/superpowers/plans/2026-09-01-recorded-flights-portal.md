# Recorded flights portal plan

## Goal

Add a persistent Flights archive to the ground-station portal so an operator
can find every saved operator-declared flight after it has stopped, inspect its
summary and bounded mission/telemetry previews, and download each durable file.

## Scope

- Discover recorded flight folders from `flights/<session>/flight-NN*/` on
  disk, including flights created by earlier backend processes.
- Expose read-only archive list, detail, and whitelisted file-download routes.
- Add a `Flights` view to the existing mission-control navigation with clear
  loading, unavailable, empty, selected, and error states.
- Preserve the always-on active-session export and all existing flight data.
- Keep previews bounded so a large `telemetry.csv` is never loaded wholesale
  just to render the portal.

## Expected files

- `backend/session.py`
- `backend/app.py`
- `backend/tests.py`
- `dashboard/src/App.tsx`
- `dashboard/src/components/SideNav.tsx`
- `dashboard/src/components/FlightsView.tsx`
- `dashboard/src/hooks/useRecordedFlights.ts`

## Verification

- Add fail-first archive discovery, preview-bound, malformed metadata, and safe
  path-resolution tests before backend implementation.
- Run `backend/.venv/bin/python -m backend.tests`.
- Run `npm run build` and `npm run lint` from `dashboard/`.
- Exercise list/detail/download against a temporary flights root.
- Render the final view in the local browser at desktop and narrow widths and
  inspect its loading, populated, selected, and empty/error behavior.
