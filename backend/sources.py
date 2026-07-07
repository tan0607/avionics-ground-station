"""Pluggable byte sources for the ingest loop.

Both sources expose the same shape — an async iterator of raw `bytes` chunks —
so app.py's ingest loop is identical whether the bytes come from a real E32 over
a serial port (prod) or from the fake_telemetry simulator's stdout (dev). The
ingest loop treats these bytes as opaque: it logs them raw, THEN parses.

    prod:  ByteSource = SerialSource("/dev/tty.usbserial-XXXX", 9600)
    dev:   ByteSource = FakeSource()   # spawns `python3 -m shared.fake_telemetry`
"""
from __future__ import annotations

import asyncio
import sys
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
    """Read the E32 bridge's USB serial stream. pyserial is blocking, so each read
    runs in a thread via run_in_executor; a short port timeout keeps reads responsive
    and cancellable. Requires `pip install pyserial` (already in the venv).
    """

    def __init__(self, port: str, baud: int = 9600, read_size: int = 256) -> None:
        self.port = port
        self.baud = baud
        self.read_size = read_size
        self._serial = None  # lazily opened in chunks()

    async def chunks(self) -> AsyncIterator[bytes]:
        import serial  # imported here so --fake never needs pyserial present

        self._serial = serial.Serial(self.port, self.baud, timeout=0.2)
        loop = asyncio.get_running_loop()
        while True:
            # read() returns up to read_size bytes, or fewer after the timeout.
            chunk = await loop.run_in_executor(None, self._serial.read, self.read_size)
            if chunk:
                yield chunk

    async def close(self) -> None:
        if self._serial is not None and self._serial.is_open:
            self._serial.close()

    def describe(self) -> dict:
        return {"kind": "serial", "port": self.port, "baud": self.baud}
