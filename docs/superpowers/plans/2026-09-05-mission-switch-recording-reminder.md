# Mission switch confirmation and recording reminder

## Scope

- Both mission pickers request confirmation before changing A1R/A2R.
- Cancel preserves the current mission and live history. Confirm clears the live
  telemetry and sensor history, then starts a fresh display for the target.
- Keep receiver confirmation authoritative; do not append the old vehicle's
  telemetry while a requested radio switch is still pending.
- Preserve saved session/flight logs and the browser's accumulated log.
- Asked the operator about reminder and active-recording switch preferences.
  With no further preference received, stated the recommended defaults before
  implementing: persistent serial-session reminder with a start button; save/stop
  the active flight on confirmed switch, then remind to start the next flight.
- Use the existing monochrome console styles; no firmware changes. Preserve the
  unrelated untracked ejection simulation work.

## Files and verification

Mission/telemetry hooks, App, confirmation/reminder components, recorder control,
and focused dashboard tests. Add runnable React tests, show the pre-change
failures, implement, then run tests, TypeScript/Vite build, lint, backend tests,
and browser checks for cancel, confirm, source switching, and recording failures.
Verify with an isolated test server; do not send channel commands to live hardware.

## Verification results

- 12 dashboard tests pass: confirmation/cancel, switch order, failed stop, receiver
  gate, complete chart reset without reconnect, keyboard focus, reminder states,
  and recording POST/poll races. Each behavior test failed before its change.
- 48 backend tests pass. TypeScript and Vite production build pass.
- Lint reports only four pre-existing Fast Refresh export warnings in unchanged
  useSettings.tsx and ui/button.tsx. Build retains its existing bundle-size warning.
- Isolated browser fixture confirms the amber banner, both confirmation entry
  points, cancel preserving history, all four graphs empty after a switch, new B
  data at B's altitude, recording success/failure, and a failed stop blocking retune.
- No firmware changes, real serial commands, or real recording changes performed.
