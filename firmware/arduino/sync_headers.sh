#!/usr/bin/env bash
# sync_headers.sh -- keep the Arduino sketch's protocol header identical to the
# PlatformIO library it was copied from.
#
# WHY A COPY EXISTS AT ALL: the Arduino IDE compiles exactly one directory --
# the sketch folder. It cannot reach ../../lib/TelemPacket, and symlinks in
# sketch folders are unreliable across IDE versions. So the header is duplicated,
# and duplication means drift, and drift in a wire format means two boards that
# quietly disagree about what byte 22 is.
#
# This script is the guard. Run --check after ANY protocol change.
#
#   ./sync_headers.sh --check    # exit 1 if the copy has drifted (no writes)
#   ./sync_headers.sh            # re-copy from lib/ and report what changed
#
# The chain of truth is:
#   shared/protocol/packet.py            <- source of truth
#     -> firmware/lib/TelemPacket/TelemPacket.h   (proven by test/packet_check.cpp)
#       -> firmware/arduino/E32Receiver/TelemPacket.h   (proven by this script)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../lib/TelemPacket/TelemPacket.h"
dst="$here/E32Receiver/TelemPacket.h"

[ -f "$src" ] || { echo "missing source of truth: $src" >&2; exit 2; }

if [ "${1:-}" = "--check" ]; then
  if cmp -s "$src" "$dst"; then
    echo "in sync: $(basename "$dst") matches lib/TelemPacket/"
    exit 0
  fi
  echo "DRIFT: the Arduino copy differs from firmware/lib/TelemPacket/TelemPacket.h" >&2
  echo "       the sketch and the PlatformIO targets no longer agree on the wire format." >&2
  diff -u "$src" "$dst" || true
  echo >&2
  echo "Fix: run $(basename "$0") with no arguments to re-copy." >&2
  exit 1
fi

if cmp -s "$src" "$dst"; then
  echo "already in sync -- nothing to do"
else
  diff -u "$dst" "$src" || true
  cp "$src" "$dst"
  echo "updated $dst from lib/TelemPacket/"
  echo "Rebuild and re-flash BOTH ends of the link before the next range test."
fi
