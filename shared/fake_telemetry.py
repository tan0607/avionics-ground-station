"""
fake_telemetry.py -- simulate a full rocket flight and emit framed telemetry packets.

Lets the whole pipeline (backend, dashboard, PLDR) be built with NO hardware. The default
mode (realtime binary frames to stdout) is a stand-in for the serial port: the backend can
read this process's stdout exactly like it will read pyserial later.

Usage (from repo root):
    python3 -m shared.fake_telemetry             # realtime binary framed stream -> stdout
    python3 -m shared.fake_telemetry --fast      # dump the whole flight instantly
    python3 -m shared.fake_telemetry --csv       # decoded human-readable rows -> stdout
    python3 -m shared.fake_telemetry --loss 0.03 # randomly drop ~3% of packets (test loss stats)
"""
from __future__ import annotations

import argparse
import math
import random
import sys
import time

try:
    from shared.protocol import packet as pk
except ImportError:  # allow: python3 shared/fake_telemetry.py
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "protocol"))
    import packet as pk  # type: ignore

HZ = 4
DT = 1.0 / HZ
G = 9.81

# Launch site -- MRCC 2026 Zon Tengah pad, in the FELCRA Seberang Perak paddy
# scheme (Perak Tengah, MY). Keep in step with LAUNCH_SITE in
# dashboard/src/lib/protocol.ts; the map centres on that one.
LAT0, LON0 = 4.0986, 100.9505
WIND_E, WIND_N = 4.0, 1.5            # steady wind drift, m/s (east, north)

# Flight-profile knobs (these give a ~1390 m apogee, ~109 s flight).
PAD_T = 5.0                          # seconds sitting on the pad
BOOST_T = 2.1                        # burn duration
BOOST_A = 8 * G                      # net upward accel during burn (thrust - weight - drag, lumped)
DROGUE_V = -25.0                     # terminal velocity under drogue, m/s
MAIN_ALT = 250.0                     # main-chute deploy altitude, m AGL
MAIN_V = -6.0                        # terminal velocity under main, m/s


def simulate_flight():
    """Yield Telemetry at 4 Hz for one full flight:
    pad -> boost -> coast -> apogee -> drogue -> main -> landed.
    Simple kinematic integration -- plausible numbers, not a real trajectory."""
    t = 0.0
    alt = 0.0            # m AGL
    v = 0.0             # m/s, up positive
    east = north = 0.0  # m downrange
    vbat = 8.0
    seq = 0
    fired = False
    apogee_reached = False
    main_deployed = False
    landed_hold = 0

    while True:
        # --- pick acceleration by phase ---
        if t < PAD_T:
            state, a = pk.FlightState.PAD, 0.0
        elif t < PAD_T + BOOST_T:
            state, a = pk.FlightState.BOOST, BOOST_A - G
        elif not apogee_reached:
            if v > 0:
                state, a = pk.FlightState.COAST, -G
            else:
                apogee_reached = True
                state, a = pk.FlightState.APOGEE, -G
        elif alt > MAIN_ALT and not main_deployed:
            state, a = pk.FlightState.DROGUE, (DROGUE_V - v) * 2.0   # relax to drogue terminal v
            fired = True
        else:
            main_deployed = True
            state, a = pk.FlightState.MAIN, (MAIN_V - v) * 2.0
            fired = True

        # --- integrate vertical motion ---
        v += a * DT
        alt += v * DT
        if alt <= 0 and t > PAD_T:
            alt, v = 0.0, 0.0
            state = pk.FlightState.LANDED

        # horizontal wind drift once off the pad
        if state != pk.FlightState.PAD:
            east += WIND_E * DT
            north += WIND_N * DT

        # tilt: near-vertical going up, tumbling under drogue, calmer under main
        if state in (pk.FlightState.PAD, pk.FlightState.BOOST,
                     pk.FlightState.COAST, pk.FlightState.APOGEE):
            tilt = 2 + 2 * math.sin(t)
        elif state == pk.FlightState.DROGUE:
            tilt = 35 + 20 * math.sin(t * 3)
        elif state == pk.FlightState.MAIN:
            tilt = 10 + 5 * math.sin(t * 2)
        else:
            tilt = 3
        tilt = max(0, min(180, int(tilt)))

        vbat = max(7.6, vbat - 0.0006)   # slow droop under load

        lat = LAT0 + north / 111_320.0
        lon = LON0 + east / (111_320.0 * math.cos(math.radians(LAT0)))

        flags = pk.FLAG_CONTINUITY | pk.FLAG_ARMED | pk.FLAG_SD_OK
        if fired:
            flags |= pk.FLAG_PYRO_FIRED

        seq += 1
        yield pk.Telemetry(
            seq=seq & 0xFFFF,
            flight_state=state,
            onboard_ms=int(t * 1000),
            baro_alt_m=int(round(alt)),
            vspeed_dms=int(round(v * 10)),
            gps_lat=int(round(lat * 1e7)),
            gps_lon=int(round(lon * 1e7)),
            gps_alt_m=int(round(alt + 40)),    # site sits ~40 m MSL
            gps_sats=11,
            gps_fix=pk.GpsFix.FIX_3D,
            tilt_deg=tilt,
            vbat_dv=int(round(vbat * 10)),
            flags=flags,
            # Healthy vehicle. Clear a bit (e.g. `& ~pk.HEALTH_BARO`) to rehearse
            # a peripheral loss against the ground station before flight.
            health=pk.HEALTH_ALL_OK,
        )

        if state == pk.FlightState.LANDED:
            landed_hold += 1
            if landed_hold > HZ * 3:   # 3 s of landed packets, then stop
                return
        t += DT


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Fake rocket telemetry generator.")
    ap.add_argument("--fast", action="store_true", help="dump instantly (no realtime pacing)")
    ap.add_argument("--csv", action="store_true", help="emit decoded rows instead of binary frames")
    ap.add_argument("--loss", type=float, default=0.0, metavar="P",
                    help="probability [0..1] of dropping each packet (seq still advances)")
    ap.add_argument("--seed", type=int, default=None, help="seed the RNG used by --loss")
    args = ap.parse_args(argv)
    if args.seed is not None:
        random.seed(args.seed)

    out = sys.stdout.buffer
    last_state = None
    for tm in simulate_flight():
        if tm.flight_state != last_state:   # event markers to stderr
            print(f"[event] t={tm.onboard_ms / 1000:6.2f}s  "
                  f"{pk.FlightState(tm.flight_state).name:7s}  alt={tm.baro_alt_m:5d}m  "
                  f"v={tm.vspeed_ms:+7.1f} m/s", file=sys.stderr)
            last_state = tm.flight_state

        if args.loss and random.random() < args.loss:
            continue   # "lost in the air" -- the receiver never sees this one

        if args.csv:
            print(f"{tm.onboard_ms:7d}ms  seq={tm.seq:5d}  "
                  f"{pk.FlightState(tm.flight_state).name:7s}  alt={tm.baro_alt_m:5d}m  "
                  f"v={tm.vspeed_ms:+7.1f}  tilt={tm.tilt_deg:3d}  "
                  f"{tm.lat_deg:.5f},{tm.lon_deg:.5f}  vbat={tm.vbat_v:.1f}V")
        else:
            out.write(pk.encode(tm))
            out.flush()

        if not args.fast:
            time.sleep(DT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
