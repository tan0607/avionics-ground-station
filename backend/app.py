"""FastAPI ground-station server: source -> raw.log -> telemetry.csv -> /ws.

One process serves both the built dashboard (static files at /) and the live
telemetry WebSocket (/ws), so the whole UI + link runs from http://localhost:8000.

Run from the repo root:
    backend/.venv/bin/python -m backend.app --fake
    backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX --baud 9600
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from fastapi import FastAPI, WebSocket
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from shared.protocol import packet

from .loss import LossTracker
from .session import SessionWriter
from .sources import ByteSource, FakeSource, SerialSource
from .wire import telemetry_to_wire

REPO_ROOT = Path(__file__).resolve().parent.parent
FLIGHTS_ROOT = REPO_ROOT / "flights"
DIST_DIR = REPO_ROOT / "dashboard" / "dist"


@dataclass
class Config:
    source: ByteSource
    flights_root: Path = FLIGHTS_ROOT


# ----------------------------------------------------------------------------
# WebSocket fan-out
# ----------------------------------------------------------------------------
class ConnectionManager:
    """Tracks live /ws clients and broadcasts each decoded frame to all of them."""

    def __init__(self) -> None:
        self._clients: set[WebSocket] = set()

    async def connect(self, ws: WebSocket, last_frame: dict | None) -> None:
        await ws.accept()
        self._clients.add(ws)
        # Late joiners aren't blank: replay the most recent frame immediately.
        if last_frame is not None:
            with contextlib.suppress(Exception):
                await ws.send_text(json.dumps(last_frame))

    def disconnect(self, ws: WebSocket) -> None:
        self._clients.discard(ws)

    async def broadcast(self, frame: dict) -> None:
        if not self._clients:
            return
        text = json.dumps(frame)
        dead: list[WebSocket] = []
        for ws in self._clients:
            try:
                await ws.send_text(text)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)

    @property
    def count(self) -> int:
        return len(self._clients)


# ----------------------------------------------------------------------------
# Runtime state shared between the ingest loop and the HTTP/WS handlers
# ----------------------------------------------------------------------------
class Runtime:
    def __init__(self) -> None:
        self.manager = ConnectionManager()
        self.parser = packet.PacketParser()
        self.loss = LossTracker()
        self.session: SessionWriter | None = None
        self.last_frame: dict | None = None
        self.last_state: int | None = None
        self.frames = 0
        self.started_ms = int(time.time() * 1000)


async def ingest(rt: Runtime, source: ByteSource) -> None:
    """The core pipeline: for every chunk, log raw bytes FIRST, then parse, then
    persist / count loss / mark events / broadcast each decoded frame."""
    assert rt.session is not None
    async for chunk in source.chunks():
        host_ms = int(time.time() * 1000)
        rt.session.write_raw(host_ms, chunk)          # raw first — never lose data
        for t in rt.parser.feed(chunk):               # resyncing framer, CRC-checked
            rt.frames += 1
            rt.loss.observe(t.seq)
            rt.session.write_row(t, host_ms)
            if t.flight_state != rt.last_state:        # flight-state transition -> event
                rt.last_state = t.flight_state
                rt.session.write_event(t, host_ms)
            frame = telemetry_to_wire(t, host_ms)
            rt.last_frame = frame
            await rt.manager.broadcast(frame)
    print("[ingest] source exhausted (stream ended)", file=sys.stderr)


def create_app(config: Config) -> FastAPI:
    rt = Runtime()

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        config.flights_root.mkdir(parents=True, exist_ok=True)
        rt.session = SessionWriter(config.flights_root, config.source.describe())
        print(f"[session] {rt.session.dir}", file=sys.stderr)
        task = asyncio.create_task(ingest(rt, config.source))
        try:
            yield
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
            await config.source.close()
            if rt.session:
                rt.session.close()

    app = FastAPI(title="Rocket Ground Station", lifespan=lifespan)

    # --- WebSocket + stats registered BEFORE the catch-all static mount ---
    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket) -> None:
        await rt.manager.connect(ws, rt.last_frame)
        try:
            while True:
                msg = await ws.receive()  # we don't expect client input; detect close
                if msg.get("type") == "websocket.disconnect":
                    break
        except Exception:
            pass
        finally:
            rt.manager.disconnect(ws)

    @app.get("/stats")
    async def stats() -> JSONResponse:
        ls = rt.loss.stats()
        return JSONResponse({
            "session": rt.session.session_id if rt.session else None,
            "source": config.source.describe(),
            "frames_decoded": rt.frames,
            "crc_errors": rt.parser.crc_errors,
            "last_seq": rt.loss.last_seq,
            "clients": rt.manager.count,
            "loss": asdict(ls),
            "loss_pct": round(ls.fraction * 100, 3),
        })

    # --- static dashboard last: Mount("/") matches everything ---
    if DIST_DIR.is_dir():
        app.mount("/", StaticFiles(directory=str(DIST_DIR), html=True), name="dashboard")
    else:
        @app.get("/")
        async def no_dist() -> JSONResponse:
            return JSONResponse(
                {"note": "dashboard/dist not built. Run `npm run build` in dashboard/, "
                         "or use vite dev with VITE_WS_URL=ws://localhost:8000/ws.",
                 "ws": "/ws", "stats": "/stats"},
            )

    return app


def build_source(args: argparse.Namespace) -> ByteSource:
    if args.serial:
        return SerialSource(args.serial, args.baud)
    extra: list[str] = []
    if args.loss:
        extra += ["--loss", str(args.loss)]
    if args.fast:
        extra += ["--fast"]
    return FakeSource(extra)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Rocket ground-station backend.")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--fake", action="store_true",
                     help="dev source: spawn `python -m shared.fake_telemetry`")
    src.add_argument("--serial", metavar="PORT",
                     help="prod source: read the E32 bridge on this serial port")
    ap.add_argument("--baud", type=int, default=9600, help="serial baud (default 9600)")
    ap.add_argument("--loss", type=float, default=0.0,
                    help="(--fake only) inject packet loss fraction to test loss stats")
    ap.add_argument("--fast", action="store_true",
                    help="(--fake only) run the sim with no realtime pacing")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args(argv)

    if not args.fake and not args.serial:
        args.fake = True  # default to the dev source so `python -m backend.app` just works

    import uvicorn
    app = create_app(Config(source=build_source(args)))
    print(f"[serve] http://{args.host}:{args.port}  (dashboard + /ws)", file=sys.stderr)
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
