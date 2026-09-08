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
        cls.evidence = []

    def run_vehicle(self, vehicle, timeline):
        result = subprocess.run([str(self.binaries[vehicle])],
                                input="\n".join(timeline.commands)+"\n",
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.evidence.append({"test": self._testMethodName, "vehicle": vehicle,
                              "events": events})
        return events

    def run_trace(self, timeline):
        for vehicle in self.binaries:
            yield vehicle, self.run_vehicle(vehicle, timeline)

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

    def test_disarm_remains_blocked_and_missing_session_restarts_delay(self):
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

    def test_warm_reset_preserves_unfinished_wait_and_readiness(self):
        for v, first in self.run_trace(Timeline().hold(100000).mark("reset")):
            with self.subTest(vehicle=v):
                t = Timeline(latch=first[-1]).hold(79990).mark("before")
                t.hold(80100).mark("after")
                events = self.run_vehicle(v, t)
                self.assertEqual(events[0]["arm_delay_ms"], 78500)
                self.assertEqual(marked(events, "before")["arm_delay_ms"], 10)
                self.assertEqual(marked(events, "before")["state"], "PAD")
                self.assertEqual(marked(events, "after")["state"], "ARMED")
                self.assertEqual(events[-1]["rises"], 0)

    def test_warm_reset_after_wait_rebuilds_stillness_and_calibration(self):
        for v, first in self.run_trace(Timeline().hold(190000).mark("reset")):
            with self.subTest(vehicle=v):
                t = Timeline(latch=first[-1]).hold(11490).mark("before")
                t.hold(11560).mark("after")
                events = self.run_vehicle(v, t)
                boot = events[0]
                self.assertEqual(boot["state"], "PAD")
                self.assertFalse(boot["armed"])
                self.assertFalse(boot["gyro_cal"])
                self.assertEqual(boot["arm_delay_ms"], 0)
                self.assertEqual(boot["arm_still_ms"], 10000)
                self.assertEqual(marked(events, "before")["state"], "PAD")
                self.assertEqual(marked(events, "after")["state"], "ARMED")

    def test_repeated_warm_reset_keeps_original_session_clock(self):
        for v, first in self.run_trace(Timeline().hold(60000).mark("reset")):
            with self.subTest(vehicle=v):
                t = Timeline(latch=first[-1]).hold(60000).mark("reset")
                # Boot-time recalibration must not reinterpret the saved clock.
                t.commands.insert(0, f"RTC_CAL {2 << 19}")
                second = self.run_vehicle(v, t)[-1]
                t = Timeline(latch=second).hold(59990).mark("before")
                t.commands.insert(0, f"RTC_CAL {3 << 19}")
                t.hold(60060).mark("after")
                events = self.run_vehicle(v, t)
                self.assertEqual(events[0]["arm_delay_ms"], 58500)
                self.assertEqual(marked(events, "before")["arm_delay_ms"], 10)
                self.assertEqual(marked(events, "before")["state"], "PAD")
                self.assertEqual(marked(events, "after")["state"], "ARMED")

    def test_reset_near_deadline_does_not_reuse_previous_still_window(self):
        for v, first in self.run_trace(Timeline().hold(170000).mark("reset")):
            with self.subTest(vehicle=v):
                t = Timeline(latch=first[-1]).hold(10060).mark("deadline")
                t.hold(11560).mark("ready")
                events = self.run_vehicle(v, t)
                deadline = marked(events, "deadline")
                self.assertEqual(deadline["arm_delay_ms"], 0)
                self.assertGreater(deadline["arm_still_ms"], 0)
                self.assertEqual(deadline["state"], "PAD")
                self.assertEqual(events[-1]["state"], "ARMED")

    def test_disarm_survives_repeated_warm_resets_but_power_on_clears_it(self):
        for disarm_at in (60000, 190000):
            first = Timeline().hold(disarm_at)
            first.commands += ["DISARM", "SNAP reset"]
            for v, before in self.run_trace(first):
                with self.subTest(vehicle=v, disarm_at=disarm_at):
                    saved = before[-1]
                    for _ in range(2):
                        events = self.run_vehicle(v, Timeline(latch=saved)
                                                  .hold(200000).mark("reset"))
                        self.assertTrue(events[0]["arm_wait"] & 16)
                        self.assertEqual(events[-1]["state"], "PAD")
                        self.assertFalse(events[-1]["armed"])
                        self.assertEqual(events[-1]["rises"], 0)
                        saved = events[-1]
                    t = Timeline(latch=saved).hold(179990).mark("before")
                    t.commands.insert(0, "RESET_REASON 1")
                    t.hold(180060).mark("after")
                    events = self.run_vehicle(v, t)
                    self.assertEqual(events[0]["arm_delay_ms"], 178500)
                    self.assertFalse(events[0]["arm_wait"] & 16)
                    self.assertEqual(marked(events, "before")["state"], "PAD")
                    self.assertEqual(events[-1]["state"], "ARMED")

    def test_invalid_retained_clock_restarts_wait_without_clearing_disarm(self):
        for blocked in (False, True):
            t = Timeline().hold(190000)
            if blocked:
                t.commands.append("DISARM")
            t.mark("reset")
            for v, before in self.run_trace(t):
                for rtc_ticks in (-1499500, (1 << 32) * 1000, 1 << 63):
                    with self.subTest(vehicle=v, blocked=blocked, rtc_ticks=rtc_ticks):
                        saved = dict(before[-1], rtc_ticks=rtc_ticks)
                        t = Timeline(latch=saved).hold(179990).mark("before")
                        t.hold(180060).mark("after")
                        events = self.run_vehicle(v, t)
                        self.assertEqual(events[0]["arm_delay_ms"], 178500)
                        self.assertEqual(bool(events[0]["arm_wait"] & 16), blocked)
                        self.assertEqual(marked(events, "before")["state"], "PAD")
                        self.assertEqual(events[-1]["state"], "PAD" if blocked else "ARMED")

    def test_zero_saved_calibration_cannot_resume_wait(self):
        t = Timeline().hold(190000).mark("reset")
        t.commands.insert(0, "RTC_CAL 0")
        for v, first in self.run_trace(t):
            with self.subTest(vehicle=v):
                events = self.run_vehicle(v, Timeline(latch=first[-1]).hold(12000))
                self.assertEqual(events[0]["arm_delay_ms"], 178500)
                self.assertEqual(events[-1]["state"], "PAD")

    def test_corrupt_lost_or_old_record_cannot_skip_cold_wait(self):
        for v, before in self.run_trace(Timeline().hold(190000).mark("reset")):
            image = bytearray.fromhex(before[-1]["rtc_image"])
            nonzero = [i for i, b in enumerate(image) if b]
            bad_marker, bad_payload = image.copy(), image.copy()
            bad_marker[nonzero[0]] ^= 0x80
            bad_payload[nonzero[-1]] ^= 0x80
            old = bytes(image).replace(bytes.fromhex("4543524d"), bytes.fromhex("4443524d"))
            for kind, corrupted in (("lost", bytes(len(image))), ("marker", bad_marker),
                                    ("payload", bad_payload), ("old", old)):
                with self.subTest(vehicle=v, kind=kind):
                    saved = dict(before[-1], rtc_image=corrupted.hex())
                    t = Timeline(latch=saved).hold(179990).mark("before")
                    t.hold(180060).mark("after")
                    events = self.run_vehicle(v, t)
                    self.assertEqual(events[0]["arm_delay_ms"], 178500)
                    self.assertEqual(marked(events, "before")["state"], "PAD")
                    self.assertEqual(events[-1]["state"], "ARMED")

    def test_recovered_wait_does_not_bypass_missing_imu_or_stale_samples(self):
        for v, before in self.run_trace(Timeline().hold(190000).mark("reset")):
            for missing in (False, True):
                with self.subTest(vehicle=v, missing=missing):
                    t = Timeline(latch=before[-1]).hold(6000)
                    t.hold(12000, imu=not missing, fresh_imu=False).mark("gap")
                    t.hold(21990).mark("before").hold(22100).mark("after")
                    events = self.run_vehicle(v, t)
                    self.assertEqual(marked(events, "gap")["state"], "PAD")
                    self.assertEqual(marked(events, "gap")["arm_delay_ms"], 0)
                    self.assertEqual(marked(events, "before")["state"], "PAD")
                    self.assertEqual(events[-1]["state"], "ARMED")

    def test_launch_during_prelaunch_recovery_is_not_fabricated_as_armed(self):
        for v, before in self.run_trace(Timeline().hold(190000).mark("reset")):
            with self.subTest(vehicle=v):
                t = Timeline(latch=before[-1]).hold(3500, accel=6, altitude=200)
                t.hold(25000, accel=0, altitude=500).mark("end")
                events = self.run_vehicle(v, t)
                self.assertEqual(events[0]["arm_delay_ms"], 0)
                self.assertEqual(events[-1]["state"], "PAD")
                self.assertEqual(events[-1]["launch_ms"], 0)
                self.assertEqual(events[-1]["rises"], 0)

    def test_prelaunch_recovery_cannot_clear_a_fire_attempt_latch(self):
        t = Timeline(latch={"latch_state": 1, "latch_fired": 1, "latch_launch_ms": 0})
        t.hold(200000).mark("reset")
        for v, first in self.run_trace(t):
            with self.subTest(vehicle=v):
                self.assertTrue(first[-1]["latch_fired"])
                events = self.run_vehicle(v, Timeline(latch=first[-1]).hold(200000))
                self.assertTrue(events[-1]["fired"])
                self.assertFalse(events[-1]["armed"])
                self.assertEqual(events[-1]["rises"], 0)

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
