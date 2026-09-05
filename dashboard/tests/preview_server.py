"""Isolated UI fixture: no serial device, disk recordings, or production backend.

Build dashboard, then run with backend/.venv/bin/python dashboard/tests/preview_server.py.
Serves the production bundle at http://127.0.0.1:8765 with synthetic telemetry.
"""
import asyncio
import math
import time
from datetime import datetime, timezone
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

app = FastAPI()
state = {"channel": "A", "paused": False, "fail_stop": False, "fail_start": False,
         "actions": [], "kind": "serial", "status_failure": False}
record = {"session": "ui-test-fixture", "recording": False, "flight": None, "completed": []}
seqs = {"A": 10, "B": 10}


@app.get("/stats")
async def stats():
    return {"session": record["session"], "source": {"kind": state["kind"], "port": "TEST FIXTURE — no hardware"},
            "source_error": None, "frames_decoded": sum(seqs.values()), "crc_errors": 0,
            "unknown_states": [], "clients": 1, "loss_pct": 0}


@app.get("/gs")
async def gs():
    return {"channel": state["channel"], "channels": ["A", "B"], "supported": state["kind"] == "serial",
            "reason": None, "error": None}


@app.post("/gs/channel")
async def channel(request: Request):
    target = (await request.json())["channel"]
    state["actions"].append(f"channel:{target}")
    state["channel"] = target
    state["paused"] = True  # hold the empty chart until explicitly resumed
    return {"sent": target, "channel": target}


@app.get("/flight")
async def flight():
    if state["status_failure"]:
        return JSONResponse({"error": "fixture unavailable"}, status_code=500)
    return record


@app.post("/flight/start")
async def start(request: Request):
    if state["fail_start"]:
        return JSONResponse({"error": "Test disk unavailable"}, status_code=500)
    label = (await request.json()).get("label", "test")
    state["actions"].append(f"start:{label}")
    index = len(record["completed"]) + 1
    record["flight"] = {"flight": f"flight-{index:02d}_{label}", "index": index,
        "label": label, "started_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_s": 0, "rows": 0, "events": 0, "raw_bytes": 0,
        "recording": True, "stop_reason": None}
    record["recording"] = True
    return record


@app.post("/flight/stop")
async def stop():
    state["actions"].append("stop")
    if state["fail_stop"]:
        return JSONResponse({"error": "Test disk unavailable"}, status_code=500)
    if record["flight"]:
        record["flight"]["recording"] = False
        record["completed"].append(record["flight"])
    record["flight"] = None
    record["recording"] = False
    return record


@app.post("/_test/control")
async def control(request: Request):
    state.update(await request.json())
    return state


@app.get("/_test/state")
async def inspect():
    return {"state": state, "record": record}


@app.websocket("/ws")
async def ws(socket: WebSocket):
    await socket.accept()
    try:
        while True:
            if not state["paused"]:
                channel = state["channel"]
                seqs[channel] += 1
                seq = seqs[channel]
                await socket.send_json({"seq": seq, "flight_state": "PAD", "onboard_ms": seq * 500 + (30000 if channel == "B" else 0),
                    "baro_alt_m": (100 if channel == "A" else 3) + math.sin(seq / 5),
                    "vspeed_ms": math.cos(seq / 5), "tilt_deg": 2, "gps_sats": 10,
                    "gps_fix": 3, "host_time_ms": int(time.time() * 1000), "armed": False,
                    "pyro_fired": False, "sd_ok": True, "flags_known": 255, "health_known": 63,
                    "hw_imu": True, "hw_baro": True, "hw_sd": True, "hw_gps": True,
                    "extra": {"AX": 0.2, "AY": 0.3, "AZ": 9.8, "SDF": 1, "SDL": seq * 5, "SDE": 0}})
            await asyncio.sleep(0.5)
    except Exception:
        pass


app.mount("/", StaticFiles(directory=Path(__file__).resolve().parents[1] / "dist", html=True), name="dashboard")
if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=8765, log_level="warning")
