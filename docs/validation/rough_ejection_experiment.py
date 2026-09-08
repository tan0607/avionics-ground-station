"""Reproducible host-only stress experiment; no production firmware changes.

Run from repository root:
backend/.venv/bin/python docs/validation/rough_ejection_experiment.py --out PATH
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from firmware.tools.simulate_flight import (compile_host, source_hashes, trajectory,
                                           LIFTOFF_MS, GRAVITY, export_replay)

PROFILES = {
    "smooth": "Control: 10 ms loop, 50 ms barometer, no noise",
    "noise-1m": "Altitude white Gaussian sigma=1 m plus AR(1) rho=.92 stationary sigma=1 m",
    "noise-3m": "Altitude white Gaussian sigma=3 m plus AR(1) rho=.92 stationary sigma=3 m",
    "noise-10m": "Altitude white Gaussian sigma=10 m plus AR(1) rho=.92 stationary sigma=10 m",
    "spikes": "1 m noise plus 2% of accepted samples shifted by random +/-20 m",
    "gaps": "1 m noise, 20% barometer sample loss, no new barometer T+11.5 to 13 s",
    "lag": "1 m noise and 600 ms delay of prescribed altitude",
    "pad-rough": "No flight; 3 m noise and isolated 6g impulses at boot 190/195/205 s",
}


def inputs(profile, seed):
    rng = random.Random(seed)
    commands = ["BOOT 1500 -1 0 0", "MOUNT y"]
    ms, next_baro, correlated = 1500, 1510, 0.0
    sigma = {"noise-3m": 3, "noise-10m": 10, "pad-rough": 3}.get(profile, 1)
    accepted_errors, intervals, spikes, dropped = [], [], 0, 0
    previous_baro = None
    while ms < 230000:
        # Keep startup deterministic, then stress the armed pad and entire flight.
        rough = profile != "smooth" and ms >= 185000
        ms = min(230000, ms + (rng.choice((5, 10, 15, 20, 35)) if rough else 10))
        t = (ms - LIFTOFF_MS) / 1000
        height, accel = trajectory(ms, "pad-only" if profile == "pad-rough" else "nominal")
        observed_height = trajectory(ms - 600, "nominal")[0] if rough and profile == "lag" else height
        fresh = ms >= next_baro
        if fresh:
            next_baro = ms + (rng.choice((35, 50, 65, 80)) if rough else 50)
            if rough and profile == "gaps" and (11.5 <= t <= 13 or rng.random() < .2):
                fresh = False
                dropped += 1
        error = 0.0
        if rough and fresh:
            correlated = .92 * correlated + math.sqrt(1 - .92**2) * rng.gauss(0, sigma)
            error = rng.gauss(0, sigma) + correlated
            if profile == "spikes" and rng.random() < .02:
                error += rng.choice((-20, 20))
                spikes += 1
        if fresh:
            if rough:
                accepted_errors.append(observed_height + error - height)
                if previous_baro is not None:
                    intervals.append(ms - previous_baro)
            previous_baro = ms
        if rough:
            # Axial raw IMU jitter: 0.02g pad, 0.35g powered flight, 0.08g coast.
            accel += rng.gauss(0, .35 if 0 < t <= 2 and profile != "pad-rough" else
                               .08 if height > 0 else .02)
            if profile == "pad-rough" and any(ms - 35 < x <= ms for x in (190000,195000,205000)):
                accel = 6
        commands.append(f"STEP {ms} {100 + observed_height + error:.9f} {accel:.9f} 0 1 1 1 {int(fresh)} 0")
        if fresh or ms == 230000:
            commands.append("SNAP sample")
    stats = {"accepted_error_rms_m": math.sqrt(sum(x*x for x in accepted_errors)/len(accepted_errors)) if accepted_errors else 0,
             "max_abs_error_m": max(map(abs, accepted_errors), default=0),
             "max_baro_interval_ms": max(intervals, default=50),
             "spikes": spikes, "dropped_samples": dropped}
    return "\n".join(commands) + "\n", stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    rows, representatives = [], {}
    with tempfile.TemporaryDirectory(prefix="mrcc-rough-") as build:
        binaries = {v: compile_host(v, Path(build)) for v in ("A", "B")}
        for profile in PROFILES:
            for seed in (range(10) if profile != "smooth" else range(1)):
                stdin, stats = inputs(profile, seed)
                (args.out / f"{profile}-{seed}.input.txt").write_text(stdin)
                for vehicle, binary in binaries.items():
                    hashes = source_hashes(vehicle)
                    result = subprocess.run([str(binary)], input=stdin, capture_output=True,
                                            text=True, check=True, timeout=30)
                    if hashes != source_hashes(vehicle):
                        raise RuntimeError("Source changed during run")
                    events = [json.loads(line) for line in result.stdout.splitlines()]
                    rises = [e for e in events if e["kind"] == "rise"]
                    falls = [e for e in events if e["kind"] == "fall"]
                    launches = [e for e in events if e["kind"] == "state" and e["state"] == "BOOST"]
                    fire_t = (rises[0]["ms"] - LIFTOFF_MS)/1000 if rises else None
                    row = dict(vehicle=vehicle, profile=profile, seed=seed, **stats,
                               fire_t=fire_t, delay_after_apogee_s=fire_t-(2+100/GRAVITY) if rises else None,
                               rises=len(rises), falls=len(falls), launches=len(launches),
                               reason=rises[0]["reason"] if rises else None,
                               pulse_ms=falls[0]["ms"]-rises[0]["ms"] if rises and falls else None)
                    rows.append(row)
                    samples = [e for e in events if e["kind"] == "snapshot"]
                    for sample in samples:
                        sample["truth_alt"] = trajectory(sample["ms"], "pad-only" if profile == "pad-rough" else "nominal")[0]
                    run = dict(vehicle=vehicle, scenario=f"{profile}-{seed}", mount="+Y nose",
                               source_sha256=hashes, input_sha256=hashlib.sha256(stdin.encode()).hexdigest(),
                               assumptions={"description": PROFILES[profile], "seed": seed,
                                            "synthetic_liftoff_ms": LIFTOFF_MS,
                                            "scope": "accepted altitude; no Baro.cpp, Health.cpp or hardware",
                                            "noise_start_boot_ms": 185000},
                               events=[e for e in events if e["kind"] != "snapshot"], samples=samples)
                    export_replay(run, args.out / f"{vehicle}-{profile}-{seed}")
                    if vehicle == "A" and seed == 0:
                        representatives[profile] = run
            selected = [r for r in rows if r["profile"] == profile]
            delays = [r["delay_after_apogee_s"] for r in selected if r["fire_t"] is not None]
            print(profile, "runs", len(selected), "early", sum(d < 0 for d in delays),
                  "delay", (min(delays), max(delays)) if delays else None, flush=True)
    (args.out / "summary.json").write_text(json.dumps(rows, indent=2)+"\n")
    plot(representatives, args.out)


def plot(representatives, directory):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(4, 2, figsize=(14, 14), layout="constrained")
    for ax, (profile, run) in zip(axes.flat, representatives.items()):
        samples = [s for s in run["samples"] if 207000 <= s["ms"] <= 216000]
        t = [(s["ms"]-LIFTOFF_MS)/1000 for s in samples]
        ax.plot(t, [s["baro_input"]-100 for s in samples], lw=.7, alpha=.7, label="Accepted noisy altitude")
        ax.plot(t, [s["truth_alt"] for s in samples], color="black", lw=1.5, label="Prescribed truth")
        ax.plot(t, [s["alt"] for s in samples], lw=1, label="Firmware filtered altitude")
        if profile != "pad-rough":
            ax.axvline(2+100/GRAVITY, color="green", ls=":", label="Truth apogee")
        for e in run["events"]:
            if e["kind"] == "rise":
                ax.axvline((e["ms"]-LIFTOFF_MS)/1000, color="red", ls="--", label="GPIO HIGH")
        ax.set(title=f"{profile}, seed 0, A (B same input)", xlabel="Seconds after prescribed liftoff", ylabel="Altitude m")
        ax.legend(fontsize=7)
    fig.savefig(directory / "rough-inputs.png", dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    main()
