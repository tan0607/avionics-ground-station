"""Ground-station backend: serial/fake source -> raw.log -> telemetry.csv -> WebSocket.

Run from the repo root so `from shared.protocol import packet` resolves:

    backend/.venv/bin/python -m backend.app --fake
"""
