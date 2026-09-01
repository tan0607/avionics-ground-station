#!/bin/sh
# One command, from nothing to a folder of figures.
#
#   ./make_report_figures.sh                 # synthetic flight
#   ./make_report_figures.sh FLIGHT001.CSV   # a real card log
set -e
cd "$(dirname "$0")"

SRC=../MRCC_FlightComputer/src

echo "==> building replay against the flight firmware"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I arduino_shim -I "$SRC" \
    replay.cpp "$SRC/Filters.cpp" -o replay

# Read the accel cutoff straight out of Config.h so the line
# drawn on the spectrum can never disagree with the firmware.
FC=$(sed -n 's/^const float LPF_FC_ACCEL *= *\([0-9.]*\).*/\1/p' "$SRC/Config.h")
FC=${FC:-12.0}

# Same idea for the calibration length, so the noise window
# always starts after the gyro bias has been applied.
CAL=$(sed -n 's/^const int  *GYRO_CAL_SAMPLES *= *\([0-9]*\).*/\1/p' "$SRC/Config.h")
CAL=${CAL:-300}

if [ -n "$1" ]; then
    echo "==> plotting the real log $1  (cutoff ${FC} Hz)"
    python3 plot_filters.py --input "$1" --out figures --fc "$FC" --cal-samples "$CAL"
else
    echo "==> no log given, synthesising a flight"
    python3 make_demo_log.py --out demo_raw.csv

    echo "==> running it through the FLIGHT filter code"
    ./replay < demo_raw.csv > demo_filtered.csv

    echo "==> plotting  (cutoff ${FC} Hz)"
    python3 plot_filters.py --input demo_filtered.csv --truth demo_raw.csv \
            --out figures --fc "$FC" --cal-samples "$CAL"
fi

echo
echo "figures/ is ready."
