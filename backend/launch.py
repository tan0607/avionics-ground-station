"""One command that brings the whole ground station up.

`mac/start.command` or `window/start.ps1` runs this after making sure the venv and `dashboard/dist` are
in place. What is left is the part the operator used to do by hand every time,
from README's "Live version" section: find which `/dev/cu.*` the receiver landed
on, start `backend.app` against it, then open the console in a browser.

    ./mac/start.command                 # find the ground station, serve, open the UI
    ./mac/start.command --demo          # no hardware: the built-in flight simulator
    ./mac/start.command --replay flights/2026-08-19T05-54-40Z

Anything this does not recognise is forwarded to `backend.app` untouched, so
`--fast`, `--loss`, `--no-reset` and friends still work through the launcher.

Two deliberate refusals:

- **No silent demo fallback.** If the hardware is missing, this waits and then
  asks. It never quietly starts the simulator, because a page full of moving
  numbers that came from nowhere is the worst failure this console can have --
  the same reason `chooseSource()` in the dashboard refuses to default a
  backend-served build to mock.
- **No second server.** If something is already serving on the port, this opens
  that instead of dying on "address already in use". Double-clicking the app
  twice should show you the ground station, not a traceback.
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser

from serial.tools import list_ports

# The USB-serial bridges an ESP32 dev board shows up behind. Only used to RANK
# candidates (a board on an unlisted bridge still gets offered), and to print a
# name the operator can recognise when more than one board is plugged in.
UART_BRIDGE_VIDS = {
    0x10C4: "CP210x",
    0x1A86: "CH340/CH9102",
    0x0403: "FTDI",
}
# Espressif's own VID: the chip's native USB, no bridge chip in the path. It is
# listed to be NAMED, and ranked BELOW the bridges on purpose. MRCC_GroundStation
# runs on a plain ESP32, which has no native USB and therefore always arrives
# through a bridge; the flight computer is the S3, which is exactly what shows up
# here. So when both boards are on the laptop, 0x303A is the one we do NOT want.
NATIVE_USB_VIDS = {0x303A: "Espressif USB"}
BRIDGE_VIDS = {**UART_BRIDGE_VIDS, **NATIVE_USB_VIDS}
# Fallbacks for a port pyserial reports no VID for. macOS names the callout
# device after the bridge driver; Linux uses ttyUSB/ttyACM.
NAME_HINTS = ("usbserial", "usbmodem", "wchusbserial", "slab_usbtouart",
              "ttyusb", "ttyacm")
# Always-present ports that are never a radio: the Bluetooth serial profile and
# the Mac's own debug console both look like serial devices forever.
IGNORE_HINTS = ("bluetooth", "debug-console")


def _rank(port) -> int:
    """Best guess first. Only decides the DEFAULT -- anything ambiguous is still
    put to the operator, because a wrong port is a blank screen with no error."""
    if port.vid in UART_BRIDGE_VIDS:
        return 0
    if port.vid is not None and port.vid not in NATIVE_USB_VIDS:
        return 1
    if port.vid in NATIVE_USB_VIDS:
        return 2      # probably the flight computer, not the receiver
    return 3          # no VID at all: a bridge pyserial couldn't identify


def candidates() -> list:
    """Ports that could plausibly be the ground station, best guess first."""
    found = []
    for p in list_ports.comports():
        name = (p.device or "").lower()
        if any(h in name for h in IGNORE_HINTS):
            continue
        if p.vid is None and not any(h in name for h in NAME_HINTS):
            continue
        found.append(p)
    return sorted(found, key=_rank)


def describe(port) -> str:
    bits = [BRIDGE_VIDS.get(port.vid, "")]
    if port.description and port.description != "n/a":
        bits.append(port.description)
    label = " · ".join(b for b in bits if b)
    return f"{port.device}  ({label})" if label else str(port.device)


def wait_for_port(seconds: float, poll: float = 1.0) -> list:
    """Poll for the receiver appearing on USB. Returns the candidates found.

    A cable plugged in after the app was opened is the normal case, not an edge
    one -- laptop first, hardware second is how a range table gets set up.
    """
    deadline = time.monotonic() + seconds
    announced = False
    while True:
        ports = candidates()
        if ports:
            return ports
        if time.monotonic() >= deadline:
            return []
        if not announced:
            print("Waiting for the ground station on USB — plug it in "
                  "(Ctrl-C to quit)...", file=sys.stderr)
            announced = True
        time.sleep(poll)


def choose(ports: list) -> str:
    """Pick the port to read. Asks only when the answer is genuinely ambiguous.

    Both boards in this project are USB serial devices, so an operator with the
    flight computer also plugged into the laptop has two plausible ports and we
    must not guess between them: reading the wrong one is an empty screen with
    no error on it.
    """
    if len(ports) == 1:
        # One candidate is not the same as the right candidate. If the only board
        # on the laptop is a native-USB ESP32, it is far more likely the flight
        # computer than the receiver -- and connecting pulses its reset line, so
        # say what is about to happen before it happens.
        if ports[0].vid in NATIVE_USB_VIDS:
            print(f"[launch] note: {describe(ports[0])} looks like the flight "
                  f"computer, not the receiver. Reading it anyway (it will "
                  f"reboot on connect) — Ctrl-C if that's wrong.", file=sys.stderr)
        return ports[0].device
    print("\nMore than one board is plugged in:", file=sys.stderr)
    for i, p in enumerate(ports, 1):
        print(f"  {i}. {describe(p)}", file=sys.stderr)
    if not sys.stdin.isatty():
        print(f"Not a terminal — using {ports[0].device}. "
              f"Pass --serial PORT to be explicit.", file=sys.stderr)
        return ports[0].device
    while True:
        raw = input(f"Which one is the ground station? [1-{len(ports)}, "
                    f"Enter for 1] ").strip()
        if not raw:
            return ports[0].device
        if raw.isdigit() and 1 <= int(raw) <= len(ports):
            return ports[int(raw) - 1].device


def prompt_no_hardware() -> str:
    """Nothing on USB after the wait. Returns 'wait', 'demo' or 'quit'."""
    print("\nNo ground station found on USB.", file=sys.stderr)
    print("  Check the cable, and that the receiver is powered.", file=sys.stderr)
    if not sys.stdin.isatty():
        return "quit"
    while True:
        raw = input("  [Enter] look again   d = demo mode (simulated flight)   "
                    "q = quit  ").strip().lower()
        if raw in ("", "r"):
            return "wait"
        if raw.startswith("d"):
            return "demo"
        if raw.startswith("q"):
            return "quit"


def resolve_port(wait_s: float) -> str | None:
    """The serial port to read, or None if the operator asked for demo mode."""
    while True:
        ports = wait_for_port(wait_s)
        if ports:
            return choose(ports)
        answer = prompt_no_hardware()
        if answer == "demo":
            return None
        if answer == "quit":
            raise SystemExit(1)


# ---------------------------------------------------------------------------
# server liveness / browser
# ---------------------------------------------------------------------------
def probe(host: str, port: int, timeout: float = 1.0) -> dict | None:
    """One /stats poll. None if nothing is listening (or it isn't our backend)."""
    url = f"http://{_browser_host(host)}:{port}/stats"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.load(r)
    except (urllib.error.URLError, OSError, ValueError):
        return None


def _browser_host(host: str) -> str:
    # --host 0.0.0.0 is a bind address, not somewhere a browser can go.
    return "127.0.0.1" if host in ("0.0.0.0", "::", "") else host


def open_when_ready(url: str, host: str, port: int, timeout: float = 30.0) -> None:
    """Open the console once the backend actually answers.

    Opening immediately races uvicorn's bind and lands the operator on a
    connection-refused page, which reads exactly like a broken install.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if probe(host, port, timeout=0.5) is not None:
            webbrowser.open(url)
            return
        time.sleep(0.25)
    print(f"[launch] server didn't answer in {timeout:.0f}s — open {url} "
          f"yourself once it does", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Start the ground station: backend + link + console.",
        epilog="Unrecognised flags are passed straight through to backend.app.",
    )
    ap.add_argument("--demo", action="store_true",
                    help="no hardware: run the flight simulator on a loop")
    ap.add_argument("--replay", metavar="PATH",
                    help="no hardware: replay a recorded session or raw.log")
    ap.add_argument("--serial", metavar="PORT",
                    help="skip autodetect and read this port")
    ap.add_argument("--baud", type=int, default=115200,
                    help="serial baud (default 115200 — matches MRCC_GroundStation)")
    ap.add_argument("--wait", type=float, default=20.0, metavar="SECONDS",
                    help="how long to wait for the receiver to appear on USB "
                         "before asking what to do (default 20)")
    ap.add_argument("--no-browser", action="store_true",
                    help="don't open the console automatically")
    ap.add_argument("--browser-url", metavar="URL",
                    help="open this instead of the backend's own URL "
                         "(mac/start.command --dev points it at Vite)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    args, passthrough = ap.parse_known_args(argv)

    url = args.browser_url or f"http://{_browser_host(args.host)}:{args.port}"

    # Already serving? Show the operator that one rather than failing to bind a
    # second copy on top of it.
    running = probe(args.host, args.port)
    if running is not None:
        src = (running.get("source") or {}).get("kind", "?")
        print(f"[launch] a ground station backend is already serving on "
              f"{args.host}:{args.port} (source={src}, "
              f"session={running.get('session')}) — opening it.", file=sys.stderr)
        if not args.no_browser:
            webbrowser.open(url)
        return 0

    forward: list[str] = []
    if args.demo:
        forward = ["--fake", "--loop"]
    elif args.replay:
        forward = ["--replay", args.replay]
    elif args.serial:
        forward = ["--serial", args.serial, "--baud", str(args.baud)]
    else:
        chosen = resolve_port(args.wait)
        if chosen is None:
            print("[launch] demo mode — this telemetry is SIMULATED.",
                  file=sys.stderr)
            forward = ["--fake", "--loop"]
        else:
            print(f"[launch] ground station on {chosen}", file=sys.stderr)
            forward = ["--serial", chosen, "--baud", str(args.baud)]

    forward += ["--host", args.host, "--port", str(args.port), *passthrough]

    if not args.no_browser:
        # Daemon: backend.app.main blocks in uvicorn until Ctrl-C, and a browser
        # that never opened must not keep the process alive after it.
        threading.Thread(target=open_when_ready, args=(url, args.host, args.port),
                         daemon=True).start()

    from .app import main as serve
    return serve(forward)


if __name__ == "__main__":
    raise SystemExit(main())
