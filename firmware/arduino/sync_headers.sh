#!/usr/bin/env bash
# sync_headers.sh -- keep the Arduino sketches' copied sources identical to the
# PlatformIO libraries they were copied from.
#
# WHY COPIES EXIST AT ALL: the Arduino IDE compiles exactly one directory --
# the sketch folder. It cannot reach ../../lib/TelemPacket, and symlinks in
# sketch folders are unreliable across IDE versions. So these files are
# duplicated, and duplication means drift, and drift in a wire format means two
# boards that quietly disagree about what byte 22 is.
#
# This script is the guard. Run --check after ANY change under firmware/lib/.
#
#   ./sync_headers.sh --check    # exit 1 if any copy has drifted (no writes)
#   ./sync_headers.sh            # re-copy from lib/ and report what changed
#
# The chain of truth is:
#   shared/protocol/packet.py       <- source of truth for the wire format
#     -> firmware/lib/*             (proven by test/packet_check.cpp
#                                    and test/subsystem_check.cpp)
#       -> firmware/arduino/*/      (proven by this script)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib="$here/../lib"

# "<source under lib/>|<destination under arduino/>"
#
# E32Receiver only needs the packet format -- it has its own portable E32Link,
# because lib/E32 is ESP32-only and that sketch also runs on AVR. E32Transmitter
# is ESP32-only by design, so it takes lib/E32 verbatim instead of growing a
# second copy of the radio driver.
pairs=(
  "TelemPacket/TelemPacket.h|E32Receiver/TelemPacket.h"
  "TelemPacket/TelemPacket.h|E32Transmitter/TelemPacket.h"
  "E32/E32.h|E32Transmitter/E32.h"
  "E32/E32.cpp|E32Transmitter/E32.cpp"
  "Subsystem/Subsystem.h|E32Transmitter/Subsystem.h"
  "Subsystem/Subsystem.cpp|E32Transmitter/Subsystem.cpp"
  "Subsystem/I2CRecover.h|E32Transmitter/I2CRecover.h"
  "Subsystem/I2CRecover.cpp|E32Transmitter/I2CRecover.cpp"
)

check_only=0
[ "${1:-}" = "--check" ] && check_only=1

drifted=0
copied=0

for pair in "${pairs[@]}"; do
  rel_src="${pair%%|*}"
  rel_dst="${pair##*|}"
  src="$lib/$rel_src"
  dst="$here/$rel_dst"

  [ -f "$src" ] || { echo "missing source of truth: $src" >&2; exit 2; }

  cmp -s "$src" "$dst" && continue
  drifted=$((drifted + 1))

  if [ "$check_only" = 1 ]; then
    echo "DRIFT: $rel_dst differs from lib/$rel_src" >&2
    diff -u "$src" "$dst" || true
  else
    diff -u "$dst" "$src" || true
    cp "$src" "$dst"
    echo "updated $rel_dst from lib/$rel_src"
    copied=$((copied + 1))
  fi
done

if [ "$check_only" = 1 ]; then
  if [ "$drifted" = 0 ]; then
    echo "in sync: all ${#pairs[@]} Arduino copies match firmware/lib/"
    exit 0
  fi
  echo >&2
  echo "$drifted file(s) drifted -- the sketches and the PlatformIO targets no" >&2
  echo "longer agree. Fix: run $(basename "$0") with no arguments to re-copy." >&2
  exit 1
fi

if [ "$copied" = 0 ]; then
  echo "already in sync -- nothing to do (${#pairs[@]} files checked)"
else
  echo "re-copied $copied file(s) from lib/"
  echo "Rebuild and re-flash BOTH ends of the link before the next range test."
fi
