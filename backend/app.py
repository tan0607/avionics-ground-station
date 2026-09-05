"""FastAPI ground-station server: source -> raw.log -> telemetry.csv -> /ws.

One process serves both the built dashboard (static files at /) and the live
telemetry WebSocket (/ws), so the whole UI + link runs from http://localhost:8000.

Run from the repo root:
    backend/.venv/bin/python -m backend.app --fake
    backend/.venv/bin/python -m backend.app --serial /dev/tty.usbserial-XXXX
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import re
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from fastapi import FastAPI, Request, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from shared.protocol import mrcc, packet

from .loss import LossTracker
from .session import (
    CSV_COLUMNS_FOR,
    FlightFolderNotEmpty,
    delete_recorded_flight,
    file_snapshot,
    PACKET_DESC,
    RECORDED_FLIGHT_FILES,
    MissionDeriver,
    MissionEvent,
    SessionWriter,
    SESSION_SELF,
    recorded_archive,
    recorded_flight_detail,
    resolve_recorded_flight_file,
)
from .sources import ByteSource, FakeSource, ReplaySource, SerialSource
from .wire import telemetry_to_wire

REPO_ROOT = Path(__file__).resolve().parent.parent
FLIGHTS_ROOT = REPO_ROOT / "flights"
DIST_DIR = REPO_ROOT / "dashboard" / "dist"


# PACKET_DESC / CSV_COLUMNS_FOR describe the two wire formats the link can be
# carrying; they live in session.py because that is what writes them into
# metadata.json, and keeping them there lets backend/tests.py check the contract
# without importing FastAPI.


def make_parser(fmt: str):
    return mrcc.MrccParser() if fmt == "mrcc" else packet.PacketParser()


def split_frame(item) -> tuple[packet.Telemetry, mrcc.MrccFrame | None, int | None, int | None]:
    """Normalize what a parser yielded into (telemetry, mrcc_frame, health_known, flags_known).

    The second element is the whole MRCC frame (or None for binary), because the
    caller needs its radio metrics AND its `extra` fields, not just one of them.

    The binary parser yields a bare Telemetry: no radio metrics (the frame body
    has no room for them), a full health byte and a full flags byte, so both
    "known" masks are None = "everything reported". The MRCC parser yields a
    richer frame because the SX1278 does report RSSI/SNR per packet, and because
    both its health and its flags have to be read a bit at a time.

    The flags mask used to be hardcoded to 0 on the grounds that MRCC "carries no
    flags at all". It carries AR and FI, so ARMED and PYRO FIRED were being
    thrown away and rendered as unknown while the vehicle was reporting them.
    """
    if isinstance(item, packet.Telemetry):
        return item, None, None, None
    health, known = mrcc.health_from_fields(item)
    item.telemetry.health = health
    flags, fknown = mrcc.flags_from_fields(item)
    item.telemetry.flags = flags
    return item.telemetry, item, known, fknown


@dataclass
class Config:
    source: ByteSource
    fmt: str = "binary"
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
# --- ground-station channel, read off the receiver's own output --------------
# MRCC_GroundStation announces its channel three ways: at boot, on a switch, and
# when asked with '?'. Matching all three is what lets the console show the
# channel instead of assuming it -- and assuming is exactly the failure this
# whole two-channel scheme exists to prevent, since a receiver on the wrong
# channel is not weak or garbled, it is silent.
#
#   RX ready - vehicle B @ 434.100 MHz  (restored from last switch; ...)
#   ### GS CHANNEL=B FREQ=434.100MHz PREV_PKTS=1834 ###
#   ### GS STATUS channel=A freq=433.300MHz pkts=57 last_rssi=-53 ...
_GS_CHANNEL_RE = mrcc.GS_CHANNEL_RE


class ChannelWatcher:
    """Track the receiver's channel from the bytes it sends back.

    The console never gets to *decide* the channel -- it asks, and then reads
    what the box says it did. A command that is written but never acted on (port
    open, board wedged) therefore shows as the channel simply not changing,
    rather than as a UI that lies about where it is listening.

    Buffers a partial trailing line because a marker can straddle two chunk
    boundaries; the cap stops a source that never sends a newline from growing
    this without bound.
    """

    MAX_BUFFER = 4096

    def __init__(self) -> None:
        self.channel: str | None = None
        self._buf = ""

    def feed(self, chunk: bytes) -> str | None:
        """Absorb a chunk. Returns the new channel if this is a SWITCH, else None.

        First contact (None -> "A") is not a switch, it is discovery, and the
        caller must not treat it as one -- there is no previous link whose
        statistics have just been invalidated.
        """
        switched = None
        self._buf += chunk.decode("utf-8", errors="replace")
        lines = self._buf.split("\n")
        self._buf = lines.pop()[-self.MAX_BUFFER:]
        for line in lines:
            m = _GS_CHANNEL_RE.search(line)
            if m:
                new = m.group(1).upper()
                if self.channel is not None and new != self.channel:
                    switched = new
                self.channel = new
        return switched


class Runtime:
    def __init__(self, fmt: str = "binary") -> None:
        self.manager = ConnectionManager()
        self.parser = make_parser(fmt)
        self.link: mrcc.LinkQuality | None = None   # last frame's RSSI/SNR, if the source reports it
        self.loss = LossTracker()
        # Derives the ground log (state changes, pyro, link, health edges) that
        # mission.log records. It lives beside the session rather than inside it
        # because the LINK events depend on the wall clock, not on the frames.
        self.mission = MissionDeriver()
        self.session: SessionWriter | None = None
        self.last_frame: dict | None = None
        self.last_state: int | None = None
        self.frames = 0
        self.started_ms = int(time.time() * 1000)
        self.gs = ChannelWatcher()


async def ingest(rt: Runtime, source: ByteSource) -> None:
    """The core pipeline: for every chunk, log raw bytes FIRST, then parse, then
    persist / count loss / mark events / broadcast each decoded frame."""
    assert rt.session is not None
    async for chunk in source.chunks():
        host_ms = int(time.time() * 1000)
        rt.session.write_raw(host_ms, chunk)          # raw first — never lose data
        # A channel change means the LINK changed, and loss counts a link. The two
        # rockets number their packets independently, so carrying the old
        # counters across the switch produces a figure that is about neither
        # vehicle: a seq jump under LossTracker's RESET_GAP is booked as hundreds
        # of lost packets that were never sent, and one over it re-baselines but
        # leaves the earlier rocket's history in the denominator forever. Neither
        # is a number an operator should be reading during a flight.
        #
        # So the statistics restart with the link. The per-flight CSVs still
        # carry `seq`, so anything finer can be recomputed after the fact.
        switched = rt.gs.feed(chunk)
        if switched:
            rt.loss = LossTracker()
            rt.session.write_mission(MissionEvent(
                host_ms, "link", "info", f"GROUND STATION -> ROCKET {switched}",
                "loss counters restarted",
            ))
            print(f"[gs] channel now {switched} — loss counters reset", file=sys.stderr)
        for item in rt.parser.feed(chunk):            # resyncing framer, checksum/format-checked
            t, mf, known, fknown = split_frame(item)
            rt.frames += 1
            rt.link = mf.link if mf else rt.link
            rt.loss.observe(t.seq)
            # The radio metrics and the surplus fields go to the RECORD, not
            # just to the screen. They were reaching /ws and AuxReadouts and
            # stopping there, which made every one of them un-reviewable after
            # the flight.
            rt.session.write_row(t, host_ms, health_known=known, flags_known=fknown,
                                 aux=mrcc.aux_csv_row(mf) if mf else None)
            if t.flight_state != rt.last_state:        # flight-state transition -> event
                rt.last_state = t.flight_state
                rt.session.write_event(t, host_ms)
            # The same events the Log view derives in the browser, written to
            # disk. They used to exist only in localStorage, which meant the one
            # narrative record of a flight was the one thing never saved.
            for e in rt.mission.observe(t, host_ms, health_known=known, flags_known=fknown):
                rt.session.write_mission(e)
            frame = telemetry_to_wire(
                t, host_ms, health_known=known, flags_known=fknown,
                rssi_dbm=mf.link.rssi_dbm if mf else None,
                snr_db=mf.link.snr_db if mf else None,
                extra=mf.extra if mf else None,
            )
            rt.last_frame = frame
            await rt.manager.broadcast(frame)
    print("[ingest] source exhausted (stream ended)", file=sys.stderr)


async def link_watchdog(rt: Runtime, period: float = 0.5) -> None:
    """Write the mission events that a frame-driven pipeline can never emit.

    LINK STALE and LINK LOST are proven by the ABSENCE of frames, so nothing in
    ingest() ever runs to notice them -- the loop is simply not called. Without
    this task, mission.log would record a link that went quiet mid-flight as a
    file that just stops, which is indistinguishable from the ground station
    being shut down.
    """
    while True:
        await asyncio.sleep(period)
        if rt.session is None:
            continue
        for e in rt.mission.tick(int(time.time() * 1000)):
            rt.session.write_mission(e)


def create_app(config: Config) -> FastAPI:
    rt = Runtime(config.fmt)

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        config.flights_root.mkdir(parents=True, exist_ok=True)
        rt.session = SessionWriter(config.flights_root, config.source.describe(),
                                   PACKET_DESC[config.fmt],
                                   CSV_COLUMNS_FOR[config.fmt])
        print(f"[session] {rt.session.dir}", file=sys.stderr)
        task = asyncio.create_task(ingest(rt, config.source))
        watchdog = asyncio.create_task(link_watchdog(rt))
        try:
            yield
        finally:
            for t in (task, watchdog):
                t.cancel()
            for t in (task, watchdog):
                with contextlib.suppress(asyncio.CancelledError):
                    await t
            await config.source.close()
            if rt.session:
                rt.session.close()

    app = FastAPI(title="Rocket Ground Station", lifespan=lifespan)

    # The production build is served same-origin by this app, so CORS is only
    # needed for local `vite dev` reaching /stats + /session. Scope it to
    # loopback origins + GET — NOT "*", which would let any website the operator
    # happens to visit read the backend's telemetry/session files.
    #
    # Matched by regex rather than a literal port list: vite picks 5174, 5175...
    # whenever 5173 is taken, and a port miss here surfaces in the UI as
    # "BACKEND UNREACHABLE" with the socket still OPEN, which reads like a
    # backend fault rather than a CORS one. Any loopback port is still only
    # reachable from a page already running on this machine, so the threat this
    # list exists to stop — a public site the operator visits — is unaffected.
    app.add_middleware(
        CORSMiddleware,
        allow_origin_regex=r"http://(localhost|127\.0\.0\.1):\d+",
        # POST joins GET for /flight/start + /flight/stop. Those are the only
        # two mutating endpoints, they are still loopback-origin-only by the
        # regex above, and _json_body() below requires an application/json
        # content type -- which is what forces a browser preflight, so a page on
        # some other origin cannot fire one of them without being checked here
        # first.
        allow_methods=["GET", "POST", "DELETE"],
        allow_headers=["*"],
    )

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
            # Null when nothing is being cut into a flight folder. Carried here
            # as well as on /flight so the top bar's REC indicator rides the
            # poll the console already runs, instead of adding a second one.
            "flight": (rt.session.flight.status()
                       if rt.session and rt.session.flight else None),
            "source": config.source.describe(),
            "format": config.fmt,
            # Non-null when the byte source itself is broken (port busy,
            # unplugged, wrong name). Without this, "cannot open the radio" and
            # "the radio is quiet" are the same empty screen.
            "source_error": getattr(config.source, "last_error", None),
            "frames_decoded": rt.frames,
            # For the text link this counts lines that named MRCC but decoded to
            # nothing usable. Same job as a CRC failure: it separates "RF is
            # arriving but mangled" from "no RF at all", which look identical
            # from frames_decoded alone.
            "crc_errors": rt.parser.crc_errors,
            # Phase words the transmitter used that this build doesn't know. Any
            # entry here means flight_state is reading PAD for a real phase --
            # visible in /stats rather than only in a post-flight head-scratch.
            "unknown_states": sorted(getattr(rt.parser, "unknown_states", ())),
            # The SX1278 reports per-packet RSSI/SNR, which the E32 never could;
            # seq gaps are no longer the only link-quality signal we have.
            "link": asdict(rt.link) if rt.link else None,
            "last_seq": rt.loss.last_seq,
            "clients": rt.manager.count,
            "loss": asdict(ls),
            "loss_pct": round(ls.fraction * 100, 3),
        })

    # --- active-session file downloads (PLDR export) ---
    # Only these three names are servable; the whitelist check runs BEFORE any
    # path join, so an arbitrary `name` can never traverse out of the session dir.
    EXPORT_FILES = {
        "metadata.json": "application/json",
        "telemetry.csv": "text/csv",
        "events.csv": "text/csv",
        "mission.log": "text/plain; charset=utf-8",
        "raw.log": "application/octet-stream",
    }

    def download(path: Path, name: str, filename: str) -> StreamingResponse:
        """Serve one export file as a fixed-length snapshot.

        Not FileResponse: these files are being written while they are served
        (see session.file_snapshot). Content-Length is the size at request time
        and the body is truncated to match, so an active telemetry.csv downloads
        as a clean prefix instead of failing on its last byte.
        """
        size, chunks = file_snapshot(path)
        return StreamingResponse(
            chunks,
            media_type=EXPORT_FILES[name],
            headers={
                "Content-Disposition": f'attachment; filename="{filename}"',
                "Content-Length": str(size),
            },
        )

    @app.get("/session")
    async def session_info() -> JSONResponse:
        if rt.session is None:
            return JSONResponse({"session": None, "files": []}, status_code=503)
        present = [name for name in EXPORT_FILES if (rt.session.dir / name).is_file()]
        return JSONResponse({"session": rt.session.session_id, "files": present})

    @app.get("/session/{name}")
    async def session_file(name: str):
        if name not in EXPORT_FILES:
            return JSONResponse(
                {"error": "unknown file", "available": list(EXPORT_FILES)}, status_code=404
            )
        if rt.session is None:
            return JSONResponse({"error": "no active session"}, status_code=503)
        path = rt.session.dir / name
        if not path.is_file():
            return JSONResponse({"error": "not written yet"}, status_code=404)
        return download(path, name, f"{rt.session.session_id}_{name}")

    # --- operator-declared flight recording ---
    # A flight boundary cannot be inferred here. The onboard clock jumps back on
    # every transmitter reboot (394 times in one 21-hour bench session), and a
    # vehicle that never leaves PAD never announces a liftoff -- so both
    # candidate auto-rules produce either a folder per reboot or no folder at
    # all. The operator says when, and the always-on session recorder underneath
    # means saying it late (or not at all) still loses nothing.

    async def _json_body(request: Request) -> dict:
        """Parse a JSON body, REQUIRING the JSON content type.

        The header check is not ceremony: it is what forces a CORS preflight.
        A form-encoded or text/plain POST is a "simple request" that any page
        the operator happens to have open can fire at 127.0.0.1:8000 without the
        browser ever asking permission -- and one of these two endpoints STOPS a
        recording mid-flight. Requiring application/json means a cross-origin
        caller has to clear the loopback-only allow_origin_regex above before
        the request is delivered at all.
        """
        ctype = request.headers.get("content-type", "")
        if not ctype.startswith("application/json"):
            raise ValueError("send application/json")
        raw = await request.body()
        if not raw:
            return {}
        try:
            body = json.loads(raw)
        except json.JSONDecodeError:
            raise ValueError("malformed JSON body")
        return body if isinstance(body, dict) else {}

    @app.get("/gs")
    async def gs_state() -> JSONResponse:
        """What the ground-station receiver is listening to, and whether the
        console may change it.

        `channel` is null until the receiver says which one it is on -- it
        announces at boot, on a switch, and when asked. Null means "not heard
        from yet", never "probably A": guessing here would put a channel on
        screen that nobody verified.
        """
        src = config.source
        supported = isinstance(src, SerialSource)
        return JSONResponse({
            "channel": rt.gs.channel,
            "channels": ["A", "B"],
            "supported": supported,
            "reason": None if supported else
                      "channel switching needs a --serial source; this session "
                      f"is {src.describe().get('kind', 'not serial')}",
            "error": getattr(src, "last_error", None),
        })

    @app.post("/gs/channel")
    async def gs_set_channel(request: Request) -> JSONResponse:
        """Retune the receiver by sending it the same key an operator would type.

        Deliberately does NOT report the new channel back. The receiver is the
        authority on where it is listening, and it says so on its own output;
        the console picks that up through ChannelWatcher a moment later. Echoing
        the requested channel here would show a switch that may not have
        happened.
        """
        src = config.source
        if not isinstance(src, SerialSource):
            return JSONResponse(
                {"error": "channel switching needs a --serial source"},
                status_code=503)
        try:
            body = await _json_body(request)
        except ValueError as exc:
            return JSONResponse({"error": str(exc)}, status_code=415)

        channel = str(body.get("channel") or "").strip().upper()
        if channel not in ("A", "B"):
            return JSONResponse({"error": "channel must be 'A' or 'B'"},
                                status_code=422)
        try:
            # Off the event loop: send() is a blocking syscall, and running it
            # here directly wedged every other request behind it.
            await asyncio.get_running_loop().run_in_executor(None, src.send, channel)
        except RuntimeError as exc:
            # 503, not 500: the port being shut is a state the operator can fix
            # (replug, close the Serial Monitor), not a bug in the server.
            return JSONResponse({"error": str(exc)}, status_code=503)

        print(f"[gs] sent channel {channel} to {src.port}", file=sys.stderr)
        return JSONResponse({"sent": channel, "channel": rt.gs.channel},
                            status_code=202)

    @app.get("/flight")
    async def flight_state() -> JSONResponse:
        if rt.session is None:
            return JSONResponse({"session": None, "recording": False, "flight": None,
                                 "completed": []}, status_code=503)
        return JSONResponse(rt.session.flight_status())

    @app.post("/flight/start")
    async def flight_start(request: Request) -> JSONResponse:
        if rt.session is None:
            return JSONResponse({"error": "no active session"}, status_code=503)
        try:
            body = await _json_body(request)
        except ValueError as exc:
            return JSONResponse({"error": str(exc)}, status_code=415)
        if rt.session.flight is not None:
            # 409, not a silent restart: the one thing this must never do is
            # close a flight that is already recording because a second browser
            # tab (or a double-click) sent start again.
            return JSONResponse(
                {"error": "already recording", "flight": rt.session.flight.status()},
                status_code=409,
            )
        label = str(body.get("label") or "")[:80]
        flight = rt.session.start_flight(label)
        # Written after the folder exists, so the first line of the flight's own
        # mission.log is the moment it started.
        rt.session.write_mission(MissionEvent(
            int(time.time() * 1000), "record", "info", "RECORDING STARTED",
            flight.name,
        ))
        print(f"[flight] recording {flight.dir}", file=sys.stderr)
        return JSONResponse(rt.session.flight_status(), status_code=201)

    @app.post("/flight/stop")
    async def flight_stop(request: Request) -> JSONResponse:
        if rt.session is None:
            return JSONResponse({"error": "no active session"}, status_code=503)
        try:
            await _json_body(request)
        except ValueError as exc:
            return JSONResponse({"error": str(exc)}, status_code=415)
        if rt.session.flight is None:
            return JSONResponse({"error": "not recording"}, status_code=409)
        name = rt.session.flight.name
        # Logged BEFORE the close, so the line lands in the flight's own file
        # rather than only in the session's.
        rt.session.write_mission(MissionEvent(
            int(time.time() * 1000), "record", "info", "RECORDING STOPPED", name,
        ))
        summary = rt.session.stop_flight()
        print(f"[flight] closed {name}: {summary['rows']} rows, "
              f"{summary['elapsed_s']:.0f} s", file=sys.stderr)
        return JSONResponse({**rt.session.flight_status(), "closed": summary})

    @app.get("/flight/files/{name}")
    async def flight_file(name: str):
        """Download a file from the flight being recorded right now.

        Same whitelist-before-join as /session/{name}: `name` is checked against
        EXPORT_FILES before it is ever joined to a path, so it cannot traverse.
        """
        if name not in EXPORT_FILES:
            return JSONResponse(
                {"error": "unknown file", "available": list(EXPORT_FILES)}, status_code=404
            )
        if rt.session is None or rt.session.flight is None:
            return JSONResponse({"error": "not recording"}, status_code=409)
        flight = rt.session.flight
        path = flight.dir / name
        if not path.is_file():
            return JSONResponse({"error": "not written yet"}, status_code=404)
        return download(path, name, f"{rt.session.session_id}_{flight.name}_{name}")

    # --- persistent recorded-flight archive ---
    # Unlike GET /flight, these endpoints are reconstructed from the configured
    # flights root on every request. A backend restart therefore changes no
    # portal history, and a stopped flight stays downloadable indefinitely.
    @app.get("/flights")
    async def flights_index() -> JSONResponse:
        """The whole archive: every session, and every flight declared inside one.

        `flights` keeps its original meaning and shape. `sessions` is the half
        the console could not see before -- a session with no flight folder was
        unreachable from the UI while holding everything it captured.
        """
        sessions, flights = recorded_archive(config.flights_root)
        # Disk cannot tell which session a running backend still holds open, and
        # this is the only caller that knows. Marking it keeps the index from
        # presenting the session being written right now as a finished one.
        live = rt.session.session_id if rt.session else None
        if live is not None:
            for row in sessions:
                if row["session"] == live:
                    row["recording"] = True
        return JSONResponse({
            "root": str(config.flights_root.resolve()),
            "count": len(flights),
            "session_count": len(sessions),
            "flights": flights,
            "sessions": sessions,
        })

    @app.get("/flights/{session_id}/{flight_name}")
    async def flight_archive_detail(session_id: str, flight_name: str) -> JSONResponse:
        detail = recorded_flight_detail(config.flights_root, session_id, flight_name)
        if detail is None:
            return JSONResponse({"error": "recorded flight not found"}, status_code=404)
        return JSONResponse(detail)

    @app.get("/flights/{session_id}/{flight_name}/files/{name}")
    async def flight_archive_file(session_id: str, flight_name: str, name: str):
        # Keep the filename whitelist separate from directory validation. Both
        # must pass before a user-controlled segment is joined to the root.
        if name not in RECORDED_FLIGHT_FILES or name not in EXPORT_FILES:
            return JSONResponse(
                {"error": "unknown file", "available": list(RECORDED_FLIGHT_FILES)},
                status_code=404,
            )
        path = resolve_recorded_flight_file(
            config.flights_root, session_id, flight_name, name
        )
        if path is None:
            return JSONResponse({"error": "file not present"}, status_code=404)
        # The archive can name the flight that is recording RIGHT NOW, so this
        # goes through the same snapshot path as the live endpoints.
        return download(path, name, f"{session_id}_{flight_name}_{name}")

    @app.delete("/flights/{session_id}/{flight_name}")
    async def flight_archive_delete(session_id: str, flight_name: str) -> JSONResponse:
        """Permanently delete one recorded flight folder.

        Guarded twice. The live flight cannot be deleted from under the writer
        that still has its files open, and the delete itself refuses any folder
        holding something this backend did not write (session.py explains why).
        """
        live = rt.session.flight if rt.session else None
        if (live is not None and rt.session is not None
                and session_id == rt.session.session_id and flight_name == live.name):
            return JSONResponse(
                {"error": "that flight is recording right now — stop it first"},
                status_code=409,
            )
        # The session this backend is writing into has all five files open. It
        # is never deletable from here, flight folder or not.
        if (flight_name == SESSION_SELF and rt.session is not None
                and session_id == rt.session.session_id):
            return JSONResponse(
                {"error": "this backend is recording into that session — "
                          "stop the server first"},
                status_code=409,
            )
        try:
            removed = delete_recorded_flight(config.flights_root, session_id, flight_name)
        except FlightFolderNotEmpty as exc:
            return JSONResponse({"error": str(exc)}, status_code=409)
        except OSError as exc:
            return JSONResponse({"error": f"could not delete: {exc}"}, status_code=500)
        if removed is None:
            return JSONResponse({"error": "recorded flight not found"}, status_code=404)
        print(f"[flight] deleted {session_id}/{flight_name} "
              f"({removed['freed_bytes']} bytes)", file=sys.stderr)
        return JSONResponse(removed)

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
        return SerialSource(args.serial, args.baud, reset=not args.no_reset)
    if args.replay:
        return ReplaySource(args.replay, loop=args.loop, fast=args.fast)
    extra: list[str] = []
    if args.loss:
        extra += ["--loss", str(args.loss)]
    if args.fast:
        extra += ["--fast"]
    return FakeSource(extra, loop=args.loop)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Rocket ground-station backend.")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--fake", action="store_true",
                     help="dev source: spawn `python -m shared.fake_telemetry`")
    src.add_argument("--serial", metavar="PORT",
                     help="prod source: read the LoRa bridge on this serial port")
    src.add_argument("--replay", metavar="PATH",
                     help="dev source: replay a recorded flights/<session>/raw.log "
                          "(or the session dir) at the pace it was received")
    # 115200 is what firmware/MRCC_GroundStation opens its USB port at. This used
    # to default to 9600 -- the radio's old UART baud, which was never the baud of
    # THIS link -- and reading a 115200 stream at 9600 is undecodable garbage that
    # looks exactly like a dead radio. Change it only if you change that sketch.
    ap.add_argument("--baud", type=int, default=115200,
                    help="serial baud (default 115200 -- must match MRCC_GroundStation)")
    # Which codec reads the byte stream. There is no project-wide default any
    # more, because the format is a property of whatever is producing the bytes:
    # --serial is a real vehicle and real vehicles downlink MRCC; --fake emits
    # binary frames; --replay knows from its own metadata. See the resolution
    # below.
    # Getting this wrong is silent, not loud: the wrong parser finds no frames
    # and the dashboard simply stays blank, so /stats reports the format back.
    # Opening the port without pulsing the ESP32's reset line can leave the board
    # silent -- a connect that reports no errors and delivers zero bytes, which
    # reads exactly like a dead radio. Reset-on-connect is therefore the default;
    # this opts out for a board that must not be rebooted (e.g. one already
    # mid-capture, or a non-ESP32 receiver with no auto-reset circuit).
    ap.add_argument("--no-reset", action="store_true",
                    help="don't pulse the board's reset line when opening the serial port")
    ap.add_argument("--format", dest="fmt", choices=("binary", "mrcc"), default=None,
                    help="wire format on the link. Default follows the source: "
                         "mrcc for --serial (what the vehicle flies), binary for "
                         "--fake, and for --replay the codec the session recorded.")
    ap.add_argument("--loss", type=float, default=0.0,
                    help="(--fake only) inject packet loss fraction to test loss stats")
    ap.add_argument("--fast", action="store_true",
                    help="(--fake/--replay) run with no realtime pacing")
    ap.add_argument("--loop", action="store_true",
                    help="(--fake/--replay) replay continuously for UI/demo work")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args(argv)

    if not args.fake and not args.serial and not args.replay:
        args.fake = True  # default to the dev source so `python -m backend.app` just works

    if args.fake and args.fmt == "mrcc":
        # fake_telemetry emits binary frames, so this pairing decodes nothing and
        # looks exactly like a dead link. Fail loudly instead.
        ap.error("--format mrcc needs a real --serial or --replay source; "
                 "--fake emits binary frames")

    try:
        source = build_source(args)
    except (OSError, ValueError) as exc:
        # An unreadable/empty replay log: fail at the CLI, not with a server up
        # and a dashboard that never fills in.
        ap.error(str(exc))

    # A replay knows its own codec from the session metadata. Only fall back to
    # asking when the recording didn't say -- picking the wrong parser here is
    # the silent failure --format's help warns about, and the log itself is the
    # one source that can't be wrong about how it was written.
    fmt = args.fmt
    if fmt is None and isinstance(source, ReplaySource):
        fmt = source.detect_format()
        if fmt:
            print(f"[replay] format={fmt} (from the session's metadata.json)",
                  file=sys.stderr)
    if fmt is None:
        # Per-source default. "binary" used to be the blanket fallback, from when
        # this repo also held the firmware that spoke it -- that firmware is gone
        # (firmware/README.md) and the only binary producer left is --fake. A
        # real serial link is an MRCC link, so defaulting it to binary meant every
        # live run needed --format mrcc or the dashboard silently stayed blank.
        #
        # A replay that reaches here recorded no codec, which dates it to before
        # metadata carried one -- i.e. before MRCC existed. Those logs are binary,
        # so replay keeps the old fallback rather than inheriting serial's.
        fmt = "mrcc" if args.serial else "binary"
        print(f"[serve] format={fmt} (default for this source; override with --format)",
              file=sys.stderr)

    import uvicorn
    app = create_app(Config(source=source, fmt=fmt))
    print(f"[serve] http://{args.host}:{args.port}  (dashboard + /ws)", file=sys.stderr)
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
