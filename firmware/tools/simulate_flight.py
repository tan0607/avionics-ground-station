"""Run actual A/B C++ flight logic, plot exact events, and serve backend replays.

Generate: backend/.venv/bin/python -m firmware.tools.simulate_flight --out PATH
Serve:    backend/.venv/bin/python -m firmware.tools.simulate_flight --out PATH \
              --serve --vehicle A --scenario nominal --port 8001
No serial device is opened. Inputs are synthetic; no flight decisions are
reimplemented in Python. Baro.cpp, radio, SD and physical deployment are outside
this host simulation's scope.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import html
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware"
HOST = FIRMWARE / "tests/ejection_host"
MODULES = ("State", "Filters", "Flight", "Pyro")
SCENARIOS = ("nominal", "baro-loss", "pad-only")
GRAVITY = 9.80665
LIFTOFF_MS = 200000  # after the real 180 s auto-arm gate; never override Config.h
END_MS = 245000
STATES = ("PAD", "ARMED", "BOOST", "COAST", "APOGEE", "DESCENT", "LANDED")


def compile_host(vehicle: str, directory: Path) -> Path:
    if vehicle not in ("A", "B"):
        raise ValueError("vehicle must be A or B")
    directory.mkdir(parents=True, exist_ok=True)
    source = FIRMWARE / f"MRCC_FlightComputer_{vehicle}/src"
    binary = directory / f"simulate_{vehicle}"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-variable", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(HOST), "-I", str(source),
        str(HOST / "main.cpp"),
        *[str(source / f"{module}.cpp") for module in MODULES],
        "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    return binary


def source_hashes(vehicle: str) -> dict[str, str]:
    source = FIRMWARE / f"MRCC_FlightComputer_{vehicle}/src"
    paths = [Path(__file__), *HOST.rglob("*.*"), source / "Config.h"]
    paths += [source / f"{m}.{ext}" for m in MODULES for ext in ("cpp", "h")]
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(paths)}


def trajectory(ms: int, scenario: str) -> tuple[float, float]:
    """Prescribed ballistic profile, not a coupled parachute/airframe model."""
    t = (ms - LIFTOFF_MS) / 1000
    if scenario == "pad-only" or t <= 0:
        return 0.0, 1.0
    if t <= 2:
        return 25 * t * t, 1 + 50 / GRAVITY
    coast = t - 2
    height = max(0.0, 100 + 100 * coast - 0.5 * GRAVITY * coast * coast)
    return height, 0.0 if height > 0 else 1.0


def simulate(binary: Path, vehicle: str, scenario: str) -> dict:
    if scenario not in SCENARIOS:
        raise ValueError(f"Unknown scenario: {scenario}")
    hashes = source_hashes(vehicle)
    commands = ["BOOT 1500 -1 0 0", "MOUNT y"]
    for ms in range(1510, END_MS + 1, 10):
        height, accel_g = trajectory(ms, scenario)
        baro = not (scenario == "baro-loss" and ms >= LIFTOFF_MS + 5000)
        commands.append(f"STEP {ms} {100 + height:.9f} {accel_g:.9f} 0 1 "
                        f"{int(baro)} 1 {int(ms % 50 == 10)} 0")
        # Flight service ticks are 1510, 1560, ...; sample after serviceFlight.
        if ms % 50 == 10:
            commands.append("SNAP sample")
    commands.append("SNAP end")
    stdin = "\n".join(commands) + "\n"
    result = subprocess.run([str(binary)], input=stdin, capture_output=True,
                            text=True, check=True, timeout=30)
    events = [json.loads(line) for line in result.stdout.splitlines()]
    if any(e["vehicle"] != vehicle for e in events):
        raise ValueError("Wrong vehicle binary")
    if hashes != source_hashes(vehicle):
        raise RuntimeError("Firmware changed during simulation; rerun")
    samples = [e for e in events if e["kind"] == "snapshot" and e["label"] == "sample"]
    for s in samples:
        s["truth_alt"], _ = trajectory(s["ms"], scenario)
    return {
        "vehicle": vehicle, "scenario": scenario, "mount": "+Y nose",
        "source_sha256": hashes,
        "input_sha256": hashlib.sha256(stdin.encode()).hexdigest(),
        "assumptions": {"synthetic_liftoff_ms": LIFTOFF_MS,
                        "net_boost_acceleration_ms2": 50, "burn_seconds": 2,
                        "loop_ms": 10, "imu_ms": 10, "baro_ms": 50,
                        "replay_sample_ms": 50, "end_ms": END_MS,
                        "profile": "prescribed ballistic descent; no parachute dynamics",
                        "input_apogee_s_after_liftoff": 2 + 100 / GRAVITY},
        "events": [e for e in events if e["kind"] != "snapshot"],
        "samples": samples,
    }


def export_replay(run: dict, directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=False)
    epoch = int(datetime.now(timezone.utc).timestamp() * 1000)
    metadata = {"packet": {"codec": "mrcc"},
                "source": {"kind": "firmware-simulation", "vehicle": run["vehicle"],
                           "scenario": run["scenario"], "mount": run["mount"]},
                "assumptions": run["assumptions"], "source_sha256": run["source_sha256"],
                "note": "20 Hz analysis replay, not Radio.cpp/LoRa output. GPIO edges in events.csv."}
    (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with (directory / "raw.log").open("xb") as raw:
        def record(ms, payload):
            raw.write(struct.pack("<QI", epoch + ms, len(payload)) + payload)
        record(run["samples"][0]["ms"], f"### GS CHANNEL={run['vehicle']}\n".encode())
        for seq, s in enumerate(run["samples"]):
            arming = (f",AW={s['arm_wait']},AD={(s['arm_delay_ms']+999)//1000},"
                      f"AS={(s['arm_still_ms']+999)//1000}") if s['state'] == 'PAD' else ""
            payload = (f"MRCC,PKT={seq},T={s['ms']/1000:.3f},ST={s['state']},"
                       f"AL={s['alt']:.3f},VZ={s['vz']:.3f},MX={s['max_alt']:.3f},"
                       f"AR={s['armed']},FI={s['fired']},"
                       f"AX={s['fax']:.4f},AY={s['fay']:.4f},AZ={s['faz']:.4f},"
                       f"BA={s['baro_ok']},IM={s['imu_ok']},PG={s['gate']}{arming}\n").encode()
            record(s["ms"], payload)
    for name, rows in (("trace", run["samples"]), ("events", run["events"])):
        with (directory / f"{name}.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    (directory / "run.json").write_text(json.dumps(run, indent=2) + "\n")


def plot_runs(runs: list[dict], directory: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # A/B overlap intentionally: identical logic and synthetic inputs.
    fig, axes = plt.subplots(4, 2, figsize=(14, 11), layout="constrained",
                             gridspec_kw={"height_ratios": [2, 1.4, 1.3, 1]})
    for col, scenario in enumerate(("nominal", "baro-loss")):
        selected = [r for r in runs if r["scenario"] == scenario]
        first = selected[0]
        truth_t = [(s["ms"] - LIFTOFF_MS) / 1000 for s in first["samples"]]
        axes[0, col].plot(truth_t, [s["truth_alt"] for s in first["samples"]],
                          color="#999999", lw=1, label="Prescribed input height")
        for run in selected:
            t = [(s["ms"] - LIFTOFF_MS) / 1000 for s in run["samples"]]
            color, line = ("#1268ad", "-") if run["vehicle"] == "A" else ("#df6b18", "--")
            style = dict(color=color, ls=line, lw=1.6, label=f"Rocket {run['vehicle']}")
            axes[0, col].plot(t, [s["alt"] for s in run["samples"]], **style)
            axes[1, col].plot(t, [s["vz"] for s in run["samples"]], **style)
            axes[2, col].step(t, [STATES.index(s["state"]) for s in run["samples"]],
                              where="post", **style)
            edges = [e for e in run["events"] if e["kind"] in ("rise", "fall")]
            edge_t = [t[0], *[(e["ms"] - LIFTOFF_MS) / 1000 for e in edges], t[-1]]
            axes[3, col].step(edge_t, [0, *[e["gate"] for e in edges], 0], where="post", **style)
        fire = next(e for e in first["events"] if e["kind"] == "rise")
        boost = next(e for e in first["events"] if e["kind"] == "state" and e["state"] == "BOOST")
        ft = (fire["ms"] - LIFTOFF_MS) / 1000
        for row in range(4):
            ax = axes[row, col]
            ax.axvline(ft, color="#bb2525", lw=1, alpha=0.8)
            ax.axvline(0, color="#777777", lw=0.7, ls=":")
            ax.grid(alpha=0.16)
            ax.set_xlim(-12, 36)
        axes[0, col].set_title(
            f"{'Normal barometer' if col == 0 else 'Barometer lost at T+5 s'}\n"
            f"Pyro gate HIGH: T+{ft:.3f} s ({fire['reason']})", loc="left", fontsize=12)
        axes[0, col].legend(fontsize=8, loc="upper right")
        axes[1, col].axhline(-2, color="#555555", ls=":", lw=0.8)
        axes[1, col].text(0.02, 0.05, "Apogee velocity gate: < -2 m/s",
                          transform=axes[1, col].transAxes, fontsize=8)
        axes[2, col].set_yticks(range(len(STATES)), STATES, fontsize=8)
        axes[3, col].set_yticks([0, 1], ["LOW", "HIGH"])
        axes[3, col].set_ylim(-0.15, 1.5)
        axes[3, col].annotate(
            f"400 ms pulse\n{(fire['ms']-boost['ms'])/1000:.3f} s after detected launch",
            xy=(ft + 0.2, 1), xytext=(ft - 9, 1.2), fontsize=8,
            arrowprops={"arrowstyle": "->", "color": "#555555"})
        axes[3, col].set_xlabel("Time from synthetic liftoff (s); T=0 is input liftoff")
        axes[0, col].set_ylabel("Altitude AGL (m)")
        axes[1, col].set_ylabel("Filtered vertical velocity (m/s)")
        axes[2, col].set_ylabel("Firmware state")
        axes[3, col].set_ylabel("Simulated GPIO")
    fig.suptitle("Actual A/B firmware host simulation | +Y nose | A/B traces overlap", fontsize=15)
    fig.savefig(directory / "flight-comparison.png", dpi=160)
    fig.savefig(directory / "flight-comparison.svg")
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(12, 3.3), layout="constrained")
    for ax, scenario in zip(axes, ("nominal", "baro-loss")):
        run = next(r for r in runs if r["vehicle"] == "A" and r["scenario"] == scenario)
        edges = [e for e in run["events"] if e["kind"] in ("rise", "fall")]
        fire_t, end_t = [(e["ms"] - LIFTOFF_MS) / 1000 for e in edges]
        apogee = next(e for e in run["events"] if e["kind"] == "state" and e["state"] == "APOGEE")
        apo_t = (apogee["ms"] - LIFTOFF_MS) / 1000
        ax.step([fire_t-.12, fire_t, end_t, end_t+.12], [0, 1, 0, 0], where="post", color="#bb2525")
        ax.axvline(apo_t, color="#1268ad", ls="--", lw=1)
        ax.set_title(f"{scenario}: A and B identical", loc="left")
        ax.set_ylim(-.15, 1.7)
        ax.set_yticks([0, 1], ["LOW", "HIGH"])
        ax.set_xlabel("Seconds after synthetic liftoff")
        ax.set_ylabel("Pyro GPIO")
        ax.text(apo_t, 1.52, f"APOGEE\n{apo_t:.3f}s", ha="center", fontsize=9)
        ax.text(fire_t+.04, 1.10, f"HIGH {fire_t:.3f}s", fontsize=9)
        ax.text(end_t, .4, f"LOW\n{end_t:.3f}s", ha="center", fontsize=9)
        ax.grid(alpha=.15)
    fig.savefig(directory / "pyro-detail.png", dpi=170)
    plt.close(fig)


def write_report(runs: list[dict], directory: Path) -> None:
    rows = []
    for run in runs:
        for e in run["events"]:
            if e["kind"] in ("boot", "state", "rise", "fall"):
                name = e["state"] if e["kind"] in ("boot", "state") else f"GPIO {e['kind']}"
                rows.append(f"<tr><td>{run['vehicle']}</td><td>{run['scenario']}</td>"
                            f"<td>{html.escape(name)}</td><td>{e['ms']/1000:.3f}</td>"
                            f"<td>{(e['ms']-LIFTOFF_MS)/1000:.3f}</td>"
                            f"<td>{e['alt']:.2f}</td><td>{e['vz']:.2f}</td></tr>")
    document = '''<!doctype html><html lang="zh"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>A/B firmware simulation</title><style>
body{font:16px/1.5 system-ui,sans-serif;margin:24px auto;max-width:1280px;padding:0 18px;color:#18212c;background:#fff}
img{width:100%;height:auto}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:8px;border-bottom:1px solid #ddd}a{color:#1268ad}.scroll{overflow:auto}
</style><h1>A/B 实际 firmware simulation</h1>
<p>两套 C++ Flight / Filters / Pyro 实际执行；安装方向 +Y 朝鼻端。这里是 host simulation，未连接硬件。</p>
<p>输入假设：T=0 离架；净加速度 50 m/s² 持续 2 s，之后使用规定的弹道高度。
没有模拟降落伞展开效果，也不是实际火箭的 OpenRocket 预测。灰线是输入，彩线是 firmware 输出。</p>
<p>backend 使用 20 Hz 分析回放，保留短暂的 APOGEE 状态；并非真实 2 Hz 无线链路。
精确触发时刻来自 events.csv 的 GPIO 边沿。A/B 同条件的结果重合。</p>
<p><a href="/?source=ws">打开本端口的 dashboard（REPLAY）</a> ·
<a href="flight-comparison.svg">下载 SVG 图</a> · <a href="summary.json">完整结果索引</a></p>
<img src="flight-comparison.png" alt="Normal and barometer-loss altitude, velocity, states and pyro gate">
<img src="pyro-detail.png" alt="APOGEE state precedes the pyro pulse by one 50 millisecond service tick">
<h2>精确事件</h2><p>Boot 时间是 millis()；T+ 时间以合成离架时刻为零。Pad-only 的 T+20 只是对齐参考。</p>
<div class="scroll"><table><thead><tr><th>Rocket</th><th>Scenario</th><th>Event</th><th>Boot s</th><th>T+ s</th><th>Altitude m</th><th>VZ m/s</th></tr></thead><tbody>'''
    document += "".join(rows) + "</tbody></table></div></html>"
    (directory / "index.html").write_text(document)
    summary = [{k: v for k, v in r.items() if k != "samples"} for r in runs]
    (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


def serve(directory: Path, vehicle: str, scenario: str, port: int) -> None:
    import uvicorn
    from starlette.routing import Mount
    from starlette.staticfiles import StaticFiles
    from backend.app import Config, create_app
    from backend.sources import ReplaySource
    replay = ReplaySource(directory / f"{vehicle}-{scenario}", loop=True)
    app = create_app(Config(source=replay, fmt="mrcc",
                           flights_root=directory / "backend-sessions" / f"{vehicle}-{scenario}"))
    app.router.routes.insert(0, Mount("/simulation", app=StaticFiles(directory=directory, html=True)))
    print(f"Simulation replay {vehicle}/{scenario}: http://127.0.0.1:{port}/?source=ws", flush=True)
    print(f"Graphs: http://127.0.0.1:{port}/simulation/", flush=True)
    uvicorn.run(app, host="127.0.0.1", port=port, log_level="warning")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--vehicle", choices=("A", "B"), default="A")
    parser.add_argument("--scenario", choices=SCENARIOS, default="nominal")
    parser.add_argument("--port", type=int, default=8001)
    args = parser.parse_args()
    directory = args.out.resolve()
    if args.serve:
        serve(directory, args.vehicle, args.scenario, args.port)
        return
    directory.mkdir(parents=True, exist_ok=False)
    runs = []
    with tempfile.TemporaryDirectory(prefix="mrcc-simulation-") as build:
        for vehicle in ("A", "B"):
            binary = compile_host(vehicle, Path(build))
            for scenario in SCENARIOS:
                run = simulate(binary, vehicle, scenario)
                export_replay(run, directory / f"{vehicle}-{scenario}")
                runs.append(run)
                edges = [(e["kind"], e["ms"], e["reason"]) for e in run["events"]
                         if e["kind"] in ("rise", "fall")]
                print(vehicle, scenario, edges, flush=True)
    plot_runs(runs, directory)
    write_report(runs, directory)
    print(directory, flush=True)


if __name__ == "__main__":
    main()
