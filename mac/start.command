#!/usr/bin/env bash
#
# Start the whole ground station with one action: double-click this file in
# Finder, or run ./mac/start.command from the repository root.
#
# It does, in order, everything README's "Live version" section asks for by
# hand -- Python venv, dashboard build, serial port, backend, browser -- and
# skips each step that is already done, so a second run on the range costs a
# couple of seconds, not a rebuild.
#
#   ./mac/start.command                 live: find the ground station on USB
#   ./mac/start.command --demo          no hardware: simulated flight, looping
#   ./mac/start.command --replay flights/2026-08-19T05-54-40Z
#   ./mac/start.command --dev           Vite hot reload in front of a live backend
#   ./mac/start.command --serial /dev/cu.usbserial-0001 --no-browser
#
# Anything else is forwarded to backend/launch.py, and from there to
# backend.app: --loop, --fast, --loss, --no-reset, --port all still work.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
PY="$ROOT/backend/.venv/bin/python"

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31merror:\033[0m %s\n\n' "$*" >&2; exit 1; }

# Kill a job and everything under it. `npm run dev` is a chain -- subshell, npm,
# then the node process actually holding the port -- and killing only the job we
# started leaves the last one alive, still bound to 5180, invisible until the
# next --dev run fails on the port.
kill_tree() {
  local pid=$1 kid
  for kid in $(pgrep -P "$pid" 2>/dev/null); do kill_tree "$kid"; done
  kill "$pid" 2>/dev/null || true
}

DEV=0
args=()
for a in "$@"; do
  case "$a" in
    --dev) DEV=1 ;;
    *) args+=("$a") ;;
  esac
done
# bash 3.2 (what macOS ships) treats "${args[@]}" on an empty array as unset
# under `set -u`, hence the ${args[@]+...} guard on every expansion below.

# --- 1. Python side --------------------------------------------------------
if [ ! -x "$PY" ]; then
  command -v python3 >/dev/null 2>&1 || die "python3 not found. brew install python"
  step "Creating backend/.venv (first run only)"
  python3 -m venv backend/.venv
fi
# Cheap every-run check: the venv can exist with its packages half-installed
# (an interrupted first run), and that surfaces as an ImportError traceback
# rather than anything an operator can act on.
if ! "$PY" -c 'import fastapi, uvicorn, serial, websockets' >/dev/null 2>&1; then
  step "Installing backend dependencies"
  "$PY" -m pip install --quiet --disable-pip-version-check fastapi uvicorn pyserial websockets \
    || die "could not install backend deps (no network?)"
fi

# --- 2. Dashboard ----------------------------------------------------------
# The backend serves dashboard/dist, so a stale bundle means the console you
# see is not the console in the repo. Rebuild only when a source file is newer
# than the build -- npm is not needed at all on a launch day with no UI edits,
# which matters because the pad has no network.
needs_build=0
if [ "$DEV" -eq 0 ]; then
  if [ ! -f dashboard/dist/index.html ]; then
    needs_build=1
  elif [ -n "$(find dashboard/src dashboard/public dashboard/index.html \
                    dashboard/package.json dashboard/vite.config.ts \
                    -newer dashboard/dist/index.html -print -quit 2>/dev/null)" ]; then
    needs_build=1
  fi
fi

if [ "$needs_build" -eq 1 ] || [ "$DEV" -eq 1 ]; then
  command -v npm >/dev/null 2>&1 || die "npm not found. brew install node"
  if [ ! -d dashboard/node_modules ]; then
    step "Installing dashboard dependencies (first run only)"
    (cd dashboard && npm install) || die "npm install failed"
  fi
fi

if [ "$needs_build" -eq 1 ]; then
  step "Building the dashboard"
  (cd dashboard && npm run build) || die "dashboard build failed"
fi

# --- 3. Dev mode: Vite in front of the backend -----------------------------
# The two-terminal workflow from README, in one terminal. Vite serves the UI
# with hot reload; ?source=ws points it at the backend started below.
#
# On 5180, not Vite's default 5173: 5173 is whatever project you started last
# (this laptop had another one holding it during testing, and Vite quietly
# bound localhost-only instead of failing). 5180 is what .claude/launch.json
# already pins for this dashboard.
DEV_PORT=5180
if [ "$DEV" -eq 1 ]; then
  step "Starting Vite (hot reload) on :$DEV_PORT"
  (cd dashboard && npm run dev -- --port "$DEV_PORT" --strictPort) &
  VITE_PID=$!
  args+=(--browser-url "http://localhost:$DEV_PORT/?source=ws")
fi

# --- 4. Link + server + console -------------------------------------------
step "Starting the ground station"
if [ "$DEV" -eq 1 ]; then
  # Backgrounded, not foreground: bash defers a trap until the foreground
  # command returns, and the backend only returns on Ctrl-C -- so killing this
  # shell any other way (closing the window, `kill`) left Vite and the backend
  # running with nothing on screen to show for them.
  #
  # `<&0` because bash points a background job's stdin at /dev/null unless told
  # otherwise, and backend/launch.py has to be able to ask which board is the
  # ground station when two are plugged in.
  "$PY" -m backend.launch ${args[@]+"${args[@]}"} <&0 &
  LAUNCH_PID=$!
  trap 'kill_tree "$VITE_PID"; kill "$LAUNCH_PID" 2>/dev/null || true' EXIT INT TERM
  wait "$LAUNCH_PID"
else
  exec "$PY" -m backend.launch ${args[@]+"${args[@]}"}
fi
