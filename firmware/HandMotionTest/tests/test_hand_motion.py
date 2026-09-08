"""Host evidence only: synthetic sensor inputs; no USB, sensors or GPIO hardware."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT.parent
REPO = FIRMWARE.parent
HOST = FIRMWARE / "tests/ejection_host"
sys.path.insert(0, str(FIRMWARE / "tests"))
from test_ejection_simulation import Timeline, events_of


def hand_trace():
    t = Timeline().hold(20000)
    # Idealized accepted barometer trace; real hardware may be too noisy.
    while t.ms < 24000:
        ms = t.ms + 10
        seconds = (ms - 20000) / 1000
        height = min(1.3, max(0, seconds * 1.3)) if seconds < 2 else max(0, 1.3 - (seconds - 2) * 1.3)
        t.step(ms, altitude=100 + height, accel=1.6 if seconds <= 0.3 else 1,
               fresh_baro=ms % 50 == 0)
    return t.hold(34000).mark("end")


class HandMotionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mrcc-hand-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for vehicle in "AB":
            src = ROOT / f"MRCC_HandMotion_{vehicle}/src"
            for original in (0, 1):
                binary = Path(cls.temp.name) / f"hand_{vehicle}_{original}"
                result = subprocess.run([
                    "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-g",
                    f"-DHAND_TEST_ORIGINAL_THRESHOLDS={original}",
                    "-I", str(HOST), "-I", str(src), str(HOST / "main.cpp"),
                    *[str(src / f"{m}.cpp") for m in ("State", "Filters", "Flight", "Pyro")],
                    "-o", str(binary)], capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(result.stderr)
                cls.binaries[vehicle, original] = binary

    def simulate(self, vehicle, timeline, original=0):
        commands = timeline.commands if isinstance(timeline, Timeline) else timeline
        result = subprocess.run([str(self.binaries[vehicle, original])],
                                input="\n".join(commands) + "\n", capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]

    def test_hand_lift_lower_uses_baro_and_keeps_gate_low(self):
        for v in "AB":
            with self.subTest(vehicle=v):
                events = self.simulate(v, hand_trace())
                end = events[-1]
                self.assertTrue(end["fired"])
                self.assertEqual(end["reason"], "APOGEE")
                self.assertEqual(end["fire_count"], 1)
                self.assertEqual(end["rises"], 0)
                self.assertEqual(end["gate"], 0)
                self.assertEqual(end["state"], "LANDED")

    def test_stationary_never_launches_or_fires(self):
        for v in "AB":
            end = self.simulate(v, Timeline().hold(50000).mark("end"))[-1]
            self.assertEqual(end["state"], "ARMED")
            self.assertFalse(end["fired"])
            self.assertEqual(end["rises"], 0)

    def test_original_profile_keeps_180_second_wait_and_rejects_hand_motion(self):
        for v in "AB":
            end = self.simulate(v, hand_trace(), original=1)[-1]
            self.assertEqual(end["state"], "PAD")
            t = Timeline().hold(200000).hold(200500, accel=1.6).hold(225000).mark("end")
            end = self.simulate(v, t, original=1)[-1]
            self.assertEqual(end["state"], "ARMED")
            self.assertFalse(end["fired"])

    def test_backup_is_distinct_and_output_remains_low(self):
        t = Timeline().hold(20000).hold(20500, accel=2).hold(45000, baro=False).mark("end")
        for v in "AB":
            end = self.simulate(v, t)[-1]
            self.assertTrue(end["fired"])
            self.assertEqual(end["reason"], "TIMER BACKUP")
            self.assertEqual(end["fire_count"], 1)
            self.assertEqual(end["rises"], 0)

    def test_console_test_fire_and_repeat_never_raise_gate(self):
        commands = ["BOOT 1500 -1 0 0", "TRY_FIRE", "TEST_FIRE", "WAIT 1700",
                    "TEST_FIRE", "WAIT 2400", "SNAP end"]
        for v in "AB":
            for original in (0, 1):
                end = self.simulate(v, commands, original)[-1]
                self.assertEqual(end["rises"], 0)
                self.assertEqual(end["gate"], 0)

    def test_original_flight_profile_simulates_fire_without_output(self):
        for v in "AB":
            end = self.simulate(v, Timeline().hold(200000).fly().mark("end"), 1)[-1]
            self.assertTrue(end["fired"])
            self.assertEqual(end["reason"], "APOGEE")
            self.assertEqual(end["rises"], 0)

    def test_recovery_and_refire_do_not_raise_gate(self):
        for v in "AB":
            before = self.simulate(v, hand_trace())[-1]
            t = Timeline(latch=before).hold(30000)
            t.commands += ["TRY_FIRE", "TEST_FIRE", "WAIT 31000", "SNAP end"]
            end = self.simulate(v, t)[-1]
            self.assertEqual(end["rises"], 0)
            self.assertEqual(end["gate"], 0)

    def test_held_high_sample_cannot_confirm_launch(self):
        t = Timeline().hold(20000).hold(20030, accel=2)
        # Do not inject new raw samples: their filtered tail is new evidence.
        t.hold(45000, fresh_imu=False).mark("end")
        for v in "AB":
            end = self.simulate(v, t)[-1]
            self.assertEqual(end["state"], "ARMED")
            self.assertFalse(end["fired"])

    def test_disarm_blocks_hand_launch(self):
        t = Timeline().hold(20000)
        t.commands += ["DISARM"]
        t.hold(20500, accel=2).hold(45000).mark("end")
        for v in "AB":
            end = self.simulate(v, t)[-1]
            self.assertEqual(end["state"], "PAD")
            self.assertFalse(end["fired"])

    def test_original_sources_unchanged_and_core_is_exact_copy(self):
        manifest = json.loads((ROOT / "source-snapshot.json").read_text())
        for name, digest in manifest.items():
            self.assertEqual(hashlib.sha256((REPO / name).read_bytes()).hexdigest(), digest, name)
        for v in "AB":
            for name in ("Flight.cpp", "Filters.cpp", "Sensors.cpp", "Baro.cpp", "State.cpp"):
                self.assertEqual((ROOT / f"MRCC_HandMotion_{v}/src/{name}").read_bytes(),
                                 (FIRMWARE / f"MRCC_FlightComputer_{v}/src/{name}").read_bytes())


if __name__ == "__main__":
    unittest.main()
