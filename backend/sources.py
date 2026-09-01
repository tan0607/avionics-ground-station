"""Pluggable byte sources for the ingest loop.

Both sources expose the same shape — an async iterator of raw `bytes` chunks —
so app.py's ingest loop is identical whether the bytes come from the real LoRa
bridge over a serial port (prod) or from the fake_telemetry simulator's stdout
(dev). The
ingest loop treats these bytes as opaque: it logs them raw, THEN parses.

    prod:   ByteSource = SerialSource("/dev/tty.usbserial-XXXX", 115200)
    dev:    ByteSource = FakeSource()   # spawns `python3 -m shared.fake_telemetry`
    replay: ByteSource = ReplaySource("flights/<session>")  # a recorded raw.log
"""
from __future__ import annotations

import asyncio
import json
import struct
import sys
import time
from pathlib import Path
from typing import AsyncIterator, Protocol

REPO_ROOT = Path(__file__).resolve().parent.parent


class ByteSource(Protocol):
    def chunks(self) -> AsyncIterator[bytes]: ...
    async def close(self) -> None: ...
    def describe(self) -> dict: ...


class FakeSource:
    """Spawn the flight simulator and stream its stdout — a stand-in for the serial
    port. fake_telemetry writes realtime binary 32-byte frames to stdout (4 Hz) and
    human event markers to stderr; we forward stdout bytes and surface stderr lines.
    """

    def __init__(self, extra_args: list[str] | None = None, loop: bool = False) -> None:
        self._proc: asyncio.subprocess.Process | None = None
        self._extra = extra_args or []
        self._loop = loop  # replay the flight forever (UI/demo) instead of once

    async def _spawn(self) -> asyncio.subprocess.Process:
        # Run from repo root so `-m shared.fake_telemetry` resolves; same interpreter.
        return await asyncio.create_subprocess_exec(
            sys.executable, "-u", "-m", "shared.fake_telemetry", *self._extra,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=str(REPO_ROOT),
        )

    async def chunks(self) -> AsyncIterator[bytes]:
        while True:
            self._proc = await self._spawn()
            assert self._proc.stdout is not None
            # Drain stderr event markers in the background so the pipe never blocks.
            asyncio.create_task(self._drain_stderr(self._proc))
            while True:
                chunk = await self._proc.stdout.read(4096)
                if not chunk:
                    break  # simulator finished (flight landed) or pipe closed
                yield chunk
            await self._proc.wait()
            if not self._loop:
                break
            # seq restarts at 1 -> LossTracker's RESET_GAP re-baselines (no fake loss).
            print("[fake_telemetry] flight ended — replaying (--loop)", file=sys.stderr)

    async def _drain_stderr(self, proc: asyncio.subprocess.Process) -> None:
        if proc.stderr is None:
            return
        async for line in proc.stderr:
            text = line.decode(errors="replace").rstrip()
            if text:
                print(f"[fake_telemetry] {text}", file=sys.stderr)

    async def close(self) -> None:
        if self._proc and self._proc.returncode is None:
            self._proc.terminate()
            try:
                await asyncio.wait_for(self._proc.wait(), timeout=2)
            except asyncio.TimeoutError:
                self._proc.kill()

    def describe(self) -> dict:
        return {"kind": "fake", "loop": self._loop,
                "cmd": f"python -m shared.fake_telemetry {' '.join(self._extra)}".strip()}


class SerialSource:
    """Read the ground station's USB serial stream at the baud MRCC_GroundStation
    opens (115200 -- NOT the radio's own rate; the SX1278 has no UART at all).
    pyserial is blocking, so each read
    runs in a thread via run_in_executor; a short port timeout keeps reads responsive
    and cancellable. Requires `pip install pyserial` (already in the venv).

    Bytes are handed on the moment they arrive (see `_read_available`), because
    the console is a live instrument: a frame held back to fill a read buffer is
    a frame the operator is reading late. `read_size` caps how much one drain
    takes, not how long anything waits.
    """

    def __init__(self, port: str, baud: int = 115200, read_size: int = 256,
                 reset: bool = True) -> None:
        self.port = port
        self.baud = baud
        self.read_size = read_size
        self.reset = reset
        self._serial = None  # lazily opened in chunks()
        # Last open/read failure, or None while the port is healthy. Surfaced by
        # /stats so an unopenable port is visible to the operator, not just to
        # whoever happens to be reading the server's stderr.
        self.last_error: str | None = None

    @staticmethod
    def _reset_board(ser) -> None:
        """Pulse the ESP32's auto-reset circuit so the board is in a known state
        when we start reading.

        The sequence is esptool's, minus the download-mode step: assert EN low
        with GPIO0 held HIGH so the chip comes up running the sketch, not the
        bootloader.

        WHAT THIS IS AND ISN'T FOR. It makes the board's state deterministic at
        connect rather than inheriting whatever the previous port owner left on
        the control lines, and the boot banner it produces is cheap positive
        proof that the receiver is alive — which is worth having, because a
        receiver only prints when a packet arrives, so a healthy board and a dead
        one both look like an empty port.

        It is NOT the fix for an empty raw.log. That symptom was chased here
        once and the actual cause was the port being held by another program
        (see `last_error` in chunks()), not the control lines. Reach for
        /stats.source_error first.

        Cost is one reboot of the GROUND-side receiver, which holds no state —
        it is a passive listener. The airborne board is never touched. Its boot
        banner does land in raw.log, which is correct: raw.log is a faithful
        record of the port, and the parsers ignore non-telemetry lines.
        """
        ser.dtr = False
        ser.rts = True      # EN low — hold in reset
        time.sleep(0.1)
        ser.rts = False     # release; GPIO0 stayed high -> normal boot

        # Deliberately NOT flushing the input buffer here. The board's boot
        # banner is the only positive evidence that this pulse worked, and it is
        # what separates "the receiver is wedged" from "the receiver is fine and
        # nothing is being transmitted" — two faults that otherwise present
        # identically as an empty raw.log. It belongs in raw.log for the same
        # reason: that file is a faithful record of the port, and every parser
        # already ignores non-telemetry lines.

    # Seconds between reopen attempts once the port is unavailable.
    RETRY_S = 2.0

    def _read_available(self) -> bytes:
        """Block for the first byte, then take everything already buffered behind it.

        THIS IS A LATENCY FIX, not a throughput one. `read(read_size)` returns
        when it has read_size bytes OR when the port timeout expires, whichever
        comes first -- and a ~200-byte MRCC line is SMALLER than the 256-byte
        read, so a complete line would sit in the buffer waiting for bytes that
        only the next line could supply. Every frame paid up to the full 0.2 s
        timeout before the parser ever saw it.

        Measured in flights/2026-08-19T05-54-40Z/raw.log: 1639 of 1904 reads
        returned on the 200 ms boundary carrying exactly one whole line. On a
        link whose frames are 250 ms apart that is most of a frame interval of
        pure stall, which is why the console visibly trailed the serial monitor.

        read(1) still blocks up to the port timeout, so an idle port costs
        nothing; `in_waiting` is then drained without waiting at all. The
        read_size cap only bounds one drain -- the loop comes straight back and
        the next read(1) returns immediately while bytes remain.
        """
        ser = self._serial
        first = ser.read(1)
        if not first:
            return b""
        waiting = ser.in_waiting
        if not waiting:
            return first
        return first + ser.read(min(waiting, self.read_size))

    async def chunks(self) -> AsyncIterator[bytes]:
        import serial  # imported here so --fake never needs pyserial present

        loop = asyncio.get_running_loop()
        while True:
            # OPENING THE PORT MUST NEVER FAIL SILENTLY. An unopenable port used
            # to raise out of the ingest task, where the traceback went to stderr
            # and nothing else noticed: /stats went on reporting 0 frames and 0
            # errors, and the dashboard showed "NO LINK". So "another program has
            # the port" (usually the Arduino Serial Monitor), "wrong port name"
            # and "the radio is quiet" all looked identical to the operator —
            # the exact failure this whole file is supposed to make obvious.
            # Now it is recorded, named, and retried.
            try:
                self._serial = serial.Serial(self.port, self.baud, timeout=0.2)
                if self.reset:
                    self._reset_board(self._serial)
                if self.last_error:
                    print(f"[serial] {self.port} reopened", file=sys.stderr)
                self.last_error = None
            except Exception as exc:  # SerialException, OSError, permissions...
                self.last_error = str(exc)
                print(f"[serial] cannot open {self.port}: {exc}", file=sys.stderr)
                if "busy" in str(exc).lower():
                    print("[serial] something else holds this port — close the "
                          "Arduino IDE's Serial Monitor (or any `screen`/`pio "
                          "device monitor`) and it will reconnect automatically.",
                          file=sys.stderr)
                await asyncio.sleep(self.RETRY_S)
                continue

            try:
                while True:
                    chunk = await loop.run_in_executor(None, self._read_available)
                    if chunk:
                        yield chunk
            except Exception as exc:
                # Unplugged mid-session: report it and go back to retrying, so a
                # reconnected cable resumes the session instead of ending it.
                self.last_error = str(exc)
                print(f"[serial] {self.port} read failed: {exc}", file=sys.stderr)
                await self.close()
                await asyncio.sleep(self.RETRY_S)

    def send(self, text: str) -> None:
        """Write an operator command to the receiver (see MRCC_GroundStation).

        Used by POST /gs/channel to retune the ground station from the console
        instead of the serial monitor. The sketch takes one key per press --
        'A'/'B' pick a channel, '?' reports -- so this stays a raw byte write
        rather than a line protocol.

        CALL THIS OFF THE EVENT LOOP -- the caller runs it in an executor. It is
        a blocking syscall, and an earlier version that awaited nothing wedged
        the whole server: one POST and every other request, including /ws, hung
        behind it. Caught on a pty in testing, which is a harsher writer than
        real hardware and therefore the right place to find it.

        Deliberately NO flush(). pyserial's posix flush() is termios.tcdrain(),
        which waits for the UART to physically drain -- microseconds on real
        hardware, but unbounded against a reader that has stopped reading, and
        it buys nothing here. write() already loops until every byte is handed
        to the OS, and one command byte is on its way the moment it is.

        THREAD SAFETY: the reader blocks in `_read_available` on its own
        executor thread while this runs. That is safe by construction, not by
        luck -- pyserial's posix write() uses its own os.write on the fd and its
        own abort pipe (pipe_abort_write_r), sharing no mutable Python state
        with read(). The kernel serialises the two directions.

        Raises RuntimeError if the port is not open -- unplugged, still
        retrying, or never opened. The caller turns that into a 503 rather than
        letting the console believe a command landed.
        """
        ser = self._serial
        if ser is None or not ser.is_open:
            raise RuntimeError(self.last_error or f"{self.port} is not open")
        try:
            ser.write(text.encode("ascii"))
        except Exception as exc:                 # SerialTimeoutException, OSError...
            raise RuntimeError(f"write to {self.port} failed: {exc}") from exc

    async def close(self) -> None:
        if self._serial is not None and self._serial.is_open:
            self._serial.close()

    def describe(self) -> dict:
        return {"kind": "serial", "port": self.port, "baud": self.baud,
                "reset_on_connect": self.reset, "error": self.last_error}


class ReplaySource:
    """Replay a recorded `raw.log` back through the ingest loop, at the pace it
    was received.

    This is the third source because the first two cannot exercise the format we
    actually fly. `--fake` emits binary PROTOCOL.md frames; the airborne board
    downlinks ASCII MRCC, which carries fields the binary frame has no slot for
    (the accelerometer axes, RSSI/SNR, pressure). So every MRCC-only path --
    mrcc.py's parser, the inferred health bits, tilt_from_accel, the `extra`
    passthrough and the Live accel panel that reads it -- was previously only
    reachable with a radio plugged in. A recorded log is the same bytes, so it
    reaches all of it at a desk.

    Records are `<host_epoch_ms u64 LE><len u32 LE><raw bytes>` (session.py
    writes them; RAW_LOG_FORMAT in metadata.json states it). Bytes are yielded
    verbatim -- chunk boundaries included, since where a chunk splits is exactly
    what stresses a resyncing parser, and a replay that silently re-chunked
    would test a stream the radio never produces.
    """

    # Inter-record waits are clamped to this. raw.log is opened in append mode
    # per session directory, so one file can span days of bench runs: the
    # 2026-08-19 log holds ~83 min of telemetry stretched over ~99 h of
    # wall-clock, with a single 94-hour gap between runs. Honouring those gaps
    # literally would leave the dashboard frozen for days; capping them keeps
    # real pacing intact (that log's p99 gap is 0.42 s) and collapses only the
    # dead air between sessions.
    MAX_GAP_S = 2.0

    _HEADER = struct.Struct("<QI")
    # A record header claiming more than this is a desync or a corrupt file, not
    # a real chunk -- serial reads are capped at a few hundred bytes.
    _MAX_RECORD = 1 << 20

    def __init__(self, path: str | Path, loop: bool = False, fast: bool = False,
                 max_gap: float = MAX_GAP_S) -> None:
        self.path = self._resolve(Path(path))
        self._loop = loop
        self._fast = fast
        self._max_gap = max_gap
        # Counted once up front so a truncated or non-raw.log file fails at
        # startup with a clear message, rather than after the server is up and
        # the operator is staring at an empty dashboard.
        self.records, self.bytes, self.duration_s, self.truncated = self._scan()
        if not self.records:
            raise ValueError(f"{self.path} contains no raw.log records")

    @staticmethod
    def _resolve(path: Path) -> Path:
        """Accept either a session directory or the raw.log inside it."""
        if path.is_dir():
            return path / "raw.log"
        return path

    def detect_format(self) -> str | None:
        """The codec this log was recorded with, from the session's metadata.json.

        Replaying MRCC bytes through the binary parser (or the reverse) decodes
        nothing and looks exactly like a dead link -- the same silent failure
        --format already warns about. The recording wrote down which codec it
        used, so the CLI can just read it instead of asking the operator to
        remember. None when there's no metadata beside the log.
        """
        meta = self.path.parent / "metadata.json"
        if not meta.is_file():
            return None
        try:
            codec = json.loads(meta.read_text()).get("packet", {}).get("codec")
        except (json.JSONDecodeError, OSError):
            return None
        return codec if codec in ("binary", "mrcc") else None

    def _scan(self) -> tuple[int, int, float, bool]:
        """Walk the records once: (count, payload bytes, span seconds, truncated)."""
        count = payload = 0
        first_ms = last_ms = None
        truncated = False
        for host_ms, chunk in self._read_records():
            if chunk is None:      # sentinel: ran out mid-record
                truncated = True
                break
            count += 1
            payload += len(chunk)
            if first_ms is None:
                first_ms = host_ms
            last_ms = host_ms
        span = (last_ms - first_ms) / 1000.0 if first_ms is not None else 0.0
        return count, payload, span, truncated

    def _read_records(self):
        """Yield (host_epoch_ms, chunk) per record; (ms, None) once if the file
        ends mid-record.

        A log from a run that was killed (or is still being written) has a torn
        tail. That's expected -- 'raw first' means bytes hit the disk before
        anything validates them -- so it ends the replay instead of raising.
        """
        with self.path.open("rb") as fh:
            while True:
                header = fh.read(self._HEADER.size)
                if len(header) < self._HEADER.size:
                    if header:
                        yield 0, None
                    return
                host_ms, length = self._HEADER.unpack(header)
                if length > self._MAX_RECORD:
                    yield host_ms, None
                    return
                chunk = fh.read(length)
                if len(chunk) < length:
                    yield host_ms, None
                    return
                yield host_ms, chunk

    async def chunks(self) -> AsyncIterator[bytes]:
        while True:
            prev_ms: int | None = None
            for host_ms, chunk in self._read_records():
                if chunk is None:
                    print(f"[replay] {self.path.name}: truncated tail — stopping "
                          f"at the last whole record", file=sys.stderr)
                    break
                if not self._fast and prev_ms is not None:
                    delay = min((host_ms - prev_ms) / 1000.0, self._max_gap)
                    if delay > 0:
                        await asyncio.sleep(delay)
                prev_ms = host_ms
                yield chunk
            if not self._loop:
                break
            # Same reasoning as FakeSource: the sequence counter restarts, and
            # LossTracker's RESET_GAP re-baselines rather than booking a jump
            # backwards as a few thousand lost packets.
            print(f"[replay] {self.path.name} exhausted — replaying (--loop)",
                  file=sys.stderr)

    async def close(self) -> None:
        return None

    def describe(self) -> dict:
        return {"kind": "replay", "path": str(self.path), "records": self.records,
                "bytes": self.bytes, "duration_s": round(self.duration_s, 1),
                "loop": self._loop, "fast": self._fast,
                "truncated": self.truncated}
