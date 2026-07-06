"""
make_fake_flight.py -- materialise a fake flight *session folder* for the PLDR notebook.

This stands in for a real recovered flight so the whole PLDR pipeline can be built and
tested with no hardware. It reuses the two shared contract modules -- there is no second
copy of the flight model or the packet codec:

    shared.fake_telemetry.simulate_flight()   # the trajectory (pad->boost->...->landed)
    shared.protocol.packet                     # the 32-byte wire codec + CSV schema

It writes the session-folder layout from GROUND_STATION_PLAN.md §4:

    flights/<session>/
      ├── metadata.json     flight metadata (marked simulated: true)
      ├── sd_log.bin        onboard SD master: EVERY packet, no loss  (binary frames)
      ├── raw.log           GS raw capture: host-timestamped hex of RECEIVED frames
      ├── telemetry.csv     GS decoded downlink (packet.CSV_COLUMNS), WITH packet loss
      └── events.csv        flight-state transitions + pyro-fired edge

Key discipline mirrored here:
  * SD log is the full-rate master record and is COMPLETE.
  * The downlink (raw.log / telemetry.csv) DROPS packets -- more when the airframe is
    tumbling under drogue -- so the notebook's "loss % vs flight phase" analysis has
    something real to report (the E32 gives no RSSI; seq-loss is the only link metric).

Usage (from repo root):
    python3 -m notebooks.make_fake_flight                       # default sim session
    python3 -m notebooks.make_fake_flight --session my-flight   # custom name
    python3 notebooks/make_fake_flight.py --loss 0.06 --seed 7  # heavier loss
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import random
import sys

# Import the two shared modules. Works as `python3 -m notebooks.make_fake_flight`
# (repo root on sys.path) and as a bare `python3 notebooks/make_fake_flight.py`.
try:
    from shared.protocol import packet as pk
    from shared.fake_telemetry import simulate_flight
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from shared.protocol import packet as pk
    from shared.fake_telemetry import simulate_flight

# Downlink loss is not uniform: a tumbling airframe swings the antenna through nulls.
# These per-phase multipliers scale the base --loss probability so the PLDR link-stats
# table shows a realistic phase-dependent pattern.
PHASE_LOSS_MULT = {
    pk.FlightState.PAD: 0.2,      # on the pad, LOS, near the GS antenna -> very clean
    pk.FlightState.BOOST: 1.0,
    pk.FlightState.COAST: 1.0,
    pk.FlightState.APOGEE: 1.2,
    pk.FlightState.DROGUE: 3.0,   # tumbling -> antenna nulls -> worst link
    pk.FlightState.MAIN: 1.5,     # calmer descent, still swinging
    pk.FlightState.LANDED: 2.0,   # on the ground, blocked by terrain
}

# Session wall-clock origin. Arbitrary but fixed for reproducible host_time stamps.
HOST_EPOCH = dt.datetime(2026, 7, 6, 9, 0, 0, tzinfo=dt.timezone.utc)
DT = 1.0 / 4  # matches shared.fake_telemetry.HZ (4 Hz)


def _iso(seconds_from_epoch: float) -> str:
    """Host/GPS timestamp as ISO-8601 UTC, milliseconds resolution."""
    ts = HOST_EPOCH + dt.timedelta(seconds=seconds_from_epoch)
    return ts.isoformat(timespec="milliseconds")


def generate(session: str, out_root: str, loss: float, seed: int) -> str:
    rng = random.Random(seed)
    flight_dir = os.path.join(out_root, session)
    os.makedirs(flight_dir, exist_ok=True)

    sd_path = os.path.join(flight_dir, "sd_log.bin")
    raw_path = os.path.join(flight_dir, "raw.log")
    csv_path = os.path.join(flight_dir, "telemetry.csv")
    events_path = os.path.join(flight_dir, "events.csv")
    meta_path = os.path.join(flight_dir, "metadata.json")

    n_total = 0
    n_received = 0
    last_state = None
    prev_pyro = False

    with (
        open(sd_path, "wb") as sd_f,
        open(raw_path, "w", encoding="utf-8") as raw_f,
        open(csv_path, "w", newline="", encoding="utf-8") as csv_f,
        open(events_path, "w", newline="", encoding="utf-8") as ev_f,
    ):
        csv_w = csv.DictWriter(csv_f, fieldnames=pk.CSV_COLUMNS)
        csv_w.writeheader()

        ev_w = csv.writer(ev_f)
        ev_w.writerow(["event", "host_time", "onboard_ms", "seq",
                       "flight_state", "baro_alt_m", "vspeed_ms"])

        for i, t in enumerate(simulate_flight()):
            n_total += 1
            frame = pk.encode(t)
            host_s = i * DT
            host_time = _iso(host_s)
            gps_time = host_time  # sim: treat GPS UTC == host clock

            # (1) SD master: always write -- this is the complete onboard record.
            sd_f.write(frame)

            # (2) event markers: flight-state transitions + first pyro-fired edge.
            state = pk.FlightState(t.flight_state)
            if state != last_state:
                ev_w.writerow([f"state:{state.name}", host_time, t.onboard_ms,
                               t.seq, state.name, t.baro_alt_m, f"{t.vspeed_ms:.1f}"])
                last_state = state
            pyro = t.flag(pk.FLAG_PYRO_FIRED)
            if pyro and not prev_pyro:
                ev_w.writerow(["pyro_fired", host_time, t.onboard_ms,
                               t.seq, state.name, t.baro_alt_m, f"{t.vspeed_ms:.1f}"])
            prev_pyro = pyro

            # (3) downlink: apply phase-weighted loss. Dropped packets never reach the GS,
            #     but seq still advances (that gap is exactly what loss stats detect).
            p_drop = min(1.0, loss * PHASE_LOSS_MULT.get(state, 1.0))
            if rng.random() < p_drop:
                continue

            n_received += 1
            # raw-first: log received bytes with a host timestamp BEFORE any decode.
            raw_f.write(f"{host_time} {frame.hex()}\n")
            # then decode via the SAME codec and append the engineering-unit CSV row.
            decoded = pk.decode(frame)
            assert decoded is not None, "self-encoded frame failed CRC -- codec bug"
            csv_w.writerow(decoded.to_csv_row(host_time=host_time, gps_time=gps_time))

    overall_loss = 1.0 - (n_received / n_total) if n_total else 0.0
    metadata = {
        "session": session,
        "simulated": True,
        "generated_by": "notebooks/make_fake_flight.py",
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "date": HOST_EPOCH.date().isoformat(),
        "packet_version": {
            "packet_size": pk.PACKET_SIZE,
            "body_size": pk.BODY_SIZE,
            "struct": pk._BODY.format,
        },
        "air_rate_bps": 2400,
        "downlink_hz": 4,
        "sd_hz": 4,
        "loss": {"base_prob": loss, "seed": seed, "phase_multipliers":
                 {s.name: m for s, m in PHASE_LOSS_MULT.items()}},
        "counts": {"packets_total": n_total, "packets_received": n_received,
                   "overall_loss_pct": round(overall_loss * 100, 2)},
        "notes": "Fake flight for PLDR template testing. SD log is complete; "
                 "telemetry.csv/raw.log carry phase-weighted downlink loss.",
    }
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)

    return flight_dir


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", default="2026-07-06_sim-01",
                    help="session folder name under the flights root")
    ap.add_argument("--out", default="flights", help="flights root directory")
    ap.add_argument("--loss", type=float, default=0.04,
                    help="base per-packet downlink drop probability [0..1]")
    ap.add_argument("--seed", type=int, default=42, help="RNG seed (reproducible loss)")
    args = ap.parse_args(argv)

    flight_dir = generate(args.session, args.out, args.loss, args.seed)
    meta = json.load(open(os.path.join(flight_dir, "metadata.json")))
    c = meta["counts"]
    print(f"wrote {flight_dir}/")
    print(f"  packets: {c['packets_received']}/{c['packets_total']} received "
          f"({c['overall_loss_pct']}% overall downlink loss)")
    print("  files: metadata.json  sd_log.bin  raw.log  telemetry.csv  events.csv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
