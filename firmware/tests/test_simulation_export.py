"""Simulation data must come from C++ outputs and survive backend replay."""
import asyncio
import json
from pathlib import Path
import tempfile
import unittest

from backend.sources import ReplaySource
from backend.app import split_frame
from shared.protocol import mrcc, packet
from firmware.tools.simulate_flight import compile_host, simulate, export_replay


class SimulationExportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-export-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        cls.binary = compile_host("B", cls.root / "build")

    def test_nominal_replay_matches_host_states_and_pyro_edges(self):
        run = simulate(self.binary, "B", "nominal")
        directory = self.root / "nominal"
        export_replay(run, directory)
        replay = ReplaySource(directory, fast=True)
        self.assertEqual(replay.detect_format(), "mrcc")

        async def decode():
            parser = mrcc.MrccParser()
            frames = []
            async for chunk in replay.chunks():
                frames.extend(parser.feed(chunk))
            return frames

        frames = asyncio.run(decode())
        self.assertEqual(len(frames), len(run["samples"]))
        for frame, sample in zip(frames, run["samples"]):
            self.assertEqual(frame.state_name, sample["state"])
            self.assertAlmostEqual(frame.extra["AY"], sample["fay"], places=3)
            if sample["state"] == "PAD":
                self.assertEqual(frame.extra["AW"], sample["arm_wait"])
                self.assertEqual(frame.extra["AD"], (sample["arm_delay_ms"] + 999) // 1000)
        self.assertEqual(frames[20].telemetry.tilt_deg, 0)
        states = list(dict.fromkeys(s["state"] for s in run["samples"]))
        self.assertEqual(states, ["PAD", "ARMED", "BOOST", "COAST", "APOGEE", "DESCENT", "LANDED"])
        arm = next(e for e in run["events"] if e["kind"] == "state" and e["state"] == "ARMED")
        self.assertGreaterEqual(arm["ms"], 180000)
        rises = [e for e in run["events"] if e["kind"] == "rise"]
        falls = [e for e in run["events"] if e["kind"] == "fall"]
        self.assertEqual(len(rises), 1)
        self.assertEqual(falls[0]["ms"] - rises[0]["ms"], 400)
        fired = next(f for f in frames if split_frame(f)[0].flags & packet.FLAG_PYRO_FIRED)
        self.assertGreaterEqual(fired.telemetry.onboard_ms, rises[0]["ms"])
        self.assertLess(fired.telemetry.onboard_ms - rises[0]["ms"], 50)
        metadata = json.loads((directory / "metadata.json").read_text())
        self.assertEqual(metadata["source"]["kind"], "firmware-simulation")
        self.assertEqual(run["mount"], "+Y nose")
        self.assertTrue(run["source_sha256"])

    def test_backup_is_clock_driven_and_pad_never_fires(self):
        run = simulate(self.binary, "B", "baro-loss")
        boost = next(e for e in run["events"] if e["kind"] == "state" and e["state"] == "BOOST")
        fire = next(e for e in run["events"] if e["kind"] == "rise")
        self.assertEqual(fire["reason"], "TIMER BACKUP")
        self.assertEqual(fire["ms"] - boost["ms"], 19050)
        pad = simulate(self.binary, "B", "pad-only")
        self.assertFalse(any(e["kind"] == "rise" for e in pad["events"]))
        self.assertNotIn("BOOST", {s["state"] for s in pad["samples"]})


if __name__ == "__main__":
    unittest.main()
