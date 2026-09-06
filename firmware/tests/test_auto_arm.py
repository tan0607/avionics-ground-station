"""New auto-arm policy against actual A/B production modules, not a Python FSM."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from firmware.tools.simulate_flight import compile_host
from firmware.tests.test_ejection_simulation import Timeline, marked, events_of


class AutoArmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-autoarm-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {v: compile_host(v, Path(cls.tmp.name)) for v in "AB"}

    def run_trace(self, timeline):
        for vehicle, binary in self.binaries.items():
            result = subprocess.run([str(binary)], input="\n".join(timeline.commands)+"\n",
                                    text=True, capture_output=True, check=True, timeout=30)
            yield vehicle, [json.loads(line) for line in result.stdout.splitlines()]

    def test_delay_and_actual_armed_confirmation(self):
        trace = Timeline().hold(179990).mark("before").hold(180060).mark("after")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertEqual(marked(events, "before")["state"], "PAD")
                end = marked(events, "after")
                self.assertEqual(end["state"], "ARMED")
                self.assertTrue(end["armed"])
                self.assertEqual(end["rises"], 0)

    def test_movement_near_deadline_requires_a_new_ten_second_window(self):
        trace = Timeline().hold(170000).hold(179000, accel=1.3, gyro=20)
        trace.hold(185000).mark("waiting").hold(190000).mark("armed")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertEqual(marked(events, "waiting")["state"], "PAD")
                self.assertEqual(marked(events, "armed")["state"], "ARMED")
                self.assertEqual(events[-1]["rises"], 0)

    def test_imu_missing_never_auto_arms_even_with_healthy_baro(self):
        for v, events in self.run_trace(Timeline().hold(220000, imu=False).mark("end")):
            with self.subTest(vehicle=v):
                self.assertEqual(events[-1]["state"], "PAD")
                self.assertFalse(events[-1]["armed"])

    def test_held_imu_and_resumed_loop_cannot_complete_old_stillness(self):
        for stall in (False, True):
            trace = Timeline().hold(178000)
            if not stall:
                trace.hold(185000, fresh_imu=False).mark("frozen")
            trace.step(185010)
            trace.hold(194900).mark("waiting").hold(195200).mark("armed")
            for v, events in self.run_trace(trace):
                with self.subTest(vehicle=v, stall=stall):
                    self.assertEqual(marked(events, "waiting")["state"], "PAD")
                    self.assertEqual(marked(events, "armed")["state"], "ARMED")
                    if not stall:
                        self.assertEqual(marked(events, "frozen")["state"], "PAD")

    def test_installation_motion_then_calibration_retries_without_console(self):
        trace = Timeline().hold(120000, gyro=20).mark("moving")
        trace.hold(179990).mark("ready").hold(180100).mark("armed")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertFalse(marked(events, "moving")["gyro_cal"])
                self.assertTrue(marked(events, "ready")["gyro_cal"])
                self.assertEqual(marked(events, "ready")["state"], "PAD")
                self.assertEqual(marked(events, "armed")["state"], "ARMED")

    def test_disarm_remains_blocked_and_prelaunch_reset_restarts_delay(self):
        trace = Timeline().hold(180100)
        trace.commands.append("DISARM")
        trace.hold(220000).mark("blocked")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertEqual(events[-1]["state"], "PAD")
                self.assertFalse(events[-1]["armed"])
        trace = Timeline(latch={"latch_state": 1, "latch_fired": 0, "latch_launch_ms": 0})
        trace.hold(179990).mark("before").hold(180100).mark("after")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertEqual(marked(events, "before")["state"], "PAD")
                self.assertEqual(marked(events, "after")["state"], "ARMED")

    def test_calibration_gap_discards_partial_batch(self):
        trace = Timeline().hold(3000)
        trace.step(12000)
        trace.hold(14000).mark("partial").hold(15200).mark("complete")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                self.assertFalse(marked(events, "partial")["gyro_cal"])
                self.assertTrue(marked(events, "complete")["gyro_cal"])

    def test_readiness_reports_blockers_and_no_countdown_is_arm_confirmation(self):
        trace = Timeline().hold(60000).mark("delay")
        trace.hold(179000, gyro=20).hold(180100).mark("still")
        trace.hold(190000).mark("armed")
        for v, events in self.run_trace(trace):
            with self.subTest(vehicle=v):
                delay = marked(events, "delay")
                self.assertEqual(delay["arm_delay_ms"], 120000)
                self.assertEqual(delay["arm_still_ms"], 0)
                self.assertEqual(delay["arm_wait"], 1)
                self.assertGreater(marked(events, "still")["arm_still_ms"], 0)
                self.assertEqual(marked(events, "armed")["arm_wait"], 0)


if __name__ == "__main__":
    unittest.main()
