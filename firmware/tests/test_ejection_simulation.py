"""Execute each vehicle's actual C++ flight logic, without attached hardware."""
import json
import hashlib
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys


FIRMWARE = Path(__file__).resolve().parents[1]
HOST = Path(__file__).with_name("ejection_host")
REPORT_PATH = None
STRICT_SAFETY = "--strict-safety" in sys.argv
if STRICT_SAFETY:
    sys.argv.remove("--strict-safety")
GRAVITY = 9.80665
LIFTOFF_MS = 20000
APOGEE_SECONDS = 2 + 100 / GRAVITY


class Timeline:
    """Synthetic raw IMU + accepted-altitude inputs; no duplicated flight logic."""
    def __init__(self, latch=None):
        self.ms = 1500
        self.commands = ["BOOT 1500 " + (
            f'{latch["latch_state"]} {latch["latch_fired"]} {latch["latch_launch_ms"]}'
            if latch else "-1 0 0")]

    def step(self, ms, altitude=100, accel=1, gyro=0, imu=True, baro=True,
             fresh_imu=True, fresh_baro=True, reseed=False):
        self.ms = ms
        self.commands.append(
            f"STEP {ms} {altitude:.9f} {accel:.9f} {gyro:.9f} "
            f"{int(imu)} {int(baro)} {int(fresh_imu)} {int(fresh_baro)} {int(reseed)}")

    def hold(self, until, interval=10, **inputs):
        while self.ms < until:
            self.step(min(self.ms + interval, until), **inputs)
        return self

    def mark(self, label):
        self.commands.append(f"SNAP {label}")
        return self

    def fly(self, until=45000, interval=10, noise=0, baro=True,
            baro_loss_ms=None, imu=True, reseed_ms=None, freeze_ms=None):
        while self.ms < until:
            ms = min(self.ms + interval, until)
            trajectory_ms = min(ms, freeze_ms) if freeze_ms else ms
            t = (trajectory_ms - LIFTOFF_MS) / 1000
            if t <= 0:
                height = 0
            elif t <= 2:
                height = 25 * t * t
            else:
                coast = t - 2
                height = max(0, 100 + 100 * coast - 0.5 * GRAVITY * coast * coast)
            actual_t = (ms - LIFTOFF_MS) / 1000
            accel = (1 + 50 / GRAVITY) if 0 < actual_t <= 2 else (
                0 if actual_t > 2 and height > 0 else 1)
            self.step(ms, altitude=100 + height + noise * math.sin(ms * 0.013)
                      + (4000 if reseed_ms and ms >= reseed_ms else 0),
                      accel=accel, imu=imu,
                      baro=baro and (baro_loss_ms is None or ms < baro_loss_ms),
                      fresh_baro=(ms % 50 == 0),
                      reseed=(ms == reseed_ms))
        return self


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def marked(events, label):
    return next(e for e in events if e["label"] == label)


class EjectionSimulationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mrcc-ejection-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        cls.evidence = []
        files = [Path(__file__), *HOST.glob("*.*")]
        for vehicle in ("A", "B"):
            src = FIRMWARE / f"MRCC_FlightComputer_{vehicle}" / "src"
            files.extend(src / f"{module}.{extension}" for module in
                         ("State", "Filters", "Flight", "Pyro")
                         for extension in ("h", "cpp"))
            files.append(src / "Config.h")
        cls.source_hashes = {str(p.relative_to(FIRMWARE)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in sorted(files)}
        for vehicle in ("A", "B"):
            src = FIRMWARE / f"MRCC_FlightComputer_{vehicle}" / "src"
            binary = Path(cls.temp.name) / f"simulate_{vehicle}"
            result = subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-variable",  # disabled continuity leaves lastContRead unused
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(HOST), "-I", str(src), str(HOST / "main.cpp"),
                *[str(src / f"{module}.cpp") for module in
                  ("State", "Filters", "Flight", "Pyro")], "-o", str(binary),
            ], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(f"Host build {vehicle} failed:\n{result.stderr}")
            cls.binaries[vehicle] = binary

    @classmethod
    def tearDownClass(cls):
        for name, digest in cls.source_hashes.items():
            if hashlib.sha256((FIRMWARE / name).read_bytes()).hexdigest() != digest:
                raise AssertionError(f"Source changed during simulation: {name}; rerun required")

    @classmethod
    def write_report(cls, result):
        report = {
            "scope": "Host logic only; synthetic inputs, no hardware or driver validation",
            "outcome": ("TEST_RUN_INVALID" if result.errors or result.unexpectedSuccesses
                        else "UNRESOLVED_SAFETY_FINDINGS" if
                        result.expectedFailures or result.failures else "SELECTED_HOST_CHECKS_PASS"),
            "strict_safety": STRICT_SAFETY,
            "summary": {"tests": result.testsRun,
                        "failures": len(result.failures), "errors": len(result.errors),
                        "expected_failures": len(result.expectedFailures),
                        "unexpected_successes": len(result.unexpectedSuccesses)},
            "unmet_expectations": [str(test) for test, _ in
                                   result.failures + result.expectedFailures],
            "sha256": cls.source_hashes,
            "runs": cls.evidence,
        }
        REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
        REPORT_PATH.write_text(json.dumps(report, indent=2) + "\n")

    def simulate(self, vehicle, commands):
        if isinstance(commands, Timeline):
            commands = commands.commands
        stdin = "\n".join(commands) + "\n"
        result = subprocess.run([str(self.binaries[vehicle])],
                                input=stdin,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.evidence.append({"test": self._testMethodName, "vehicle": vehicle,
                              "input_sha256": hashlib.sha256(stdin.encode()).hexdigest(),
                              "commands": len(commands), "events": events})
        return events

    def test_cold_boot_is_safe(self):
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, ["BOOT 1500 -1 0 0", "SNAP end"])
                end = events[-1]
                self.assertEqual(end["state"], "PAD")
                self.assertFalse(end["armed"])
                self.assertFalse(end["fired"])
                self.assertEqual(end["gate"], 0)

    def test_auto_arm_waits_for_boot_timer_and_calibration(self):
        timeline = Timeline().hold(9990).mark("before")
        timeline.hold(LIFTOFF_MS).mark("after")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "before")["state"], "PAD")
                end = marked(events, "after")
                self.assertEqual(end["state"], "ARMED")
                self.assertTrue(end["gyro_cal"])
                self.assertTrue(end["armed"])
                self.assertEqual(end["rises"], 0)

    def test_two_hours_on_pad_never_start_backup(self):
        timeline = Timeline().hold(7200000, interval=50).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "ARMED")
                self.assertEqual(end["launch_ms"], 0)
                self.assertEqual(end["rises"], 0)

    def test_no_sensors_cannot_arm(self):
        timeline = Timeline().hold(60000, imu=False, baro=False).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "PAD")
                self.assertFalse(end["armed"])
                self.assertEqual(end["rises"], 0)

    def test_moving_gyro_cannot_auto_arm(self):
        timeline = Timeline().hold(60000, gyro=20).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertFalse(end["gyro_cal"])
                self.assertEqual(end["state"], "PAD")
                self.assertEqual(end["rises"], 0)

    def test_disarm_blocks_auto_arm_until_reboot(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.commands.append("DISARM")
        timeline.hold(60000).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "PAD")
                self.assertFalse(end["armed"])
                self.assertEqual(end["rises"], 0)

    def test_pressure_change_with_stationary_imu_does_not_launch(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.hold(25000, altitude=4100).hold(60000).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "ARMED")
                self.assertEqual(end["rises"], 0)

    def test_single_raw_acceleration_spike_is_rejected(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.step(20010, accel=10)
        timeline.hold(60000).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "ARMED")
                self.assertEqual(end["rises"], 0)

    def test_nominal_apogee_single_pulse_with_noise_and_cadence_variation(self):
        for vehicle in self.binaries:
            for interval, noise in ((10, 0), (20, 0), (50, 0), (100, 0), (10, 0.2)):
                with self.subTest(vehicle=vehicle, interval=interval, noise=noise):
                    timeline = Timeline().hold(LIFTOFF_MS)
                    timeline.fly(interval=interval, noise=noise).mark("end")
                    events = self.simulate(vehicle, timeline)
                    rises, falls = events_of(events, "rise"), events_of(events, "fall")
                    self.assertEqual(len(rises), 1)
                    self.assertEqual(len(falls), 1)
                    fire = rises[0]
                    self.assertEqual(fire["reason"], "APOGEE")
                    actual_apogee_ms = LIFTOFF_MS + APOGEE_SECONDS * 1000
                    self.assertGreaterEqual(fire["ms"], actual_apogee_ms)
                    # Exploratory 1.5 s envelope, NOT an airframe-approved limit.
                    self.assertLess(fire["ms"], actual_apogee_ms + 1500)
                    self.assertEqual(falls[0]["ms"] - fire["ms"], 400)
                    states = [e["state"] for e in events_of(events, "state")]
                    self.assertEqual(states[:5], ["ARMED", "BOOST", "COAST", "APOGEE", "DESCENT"])
                    self.assertTrue(events[-1]["latch_fired"])

    def test_baro_filter_holds_without_new_measurement(self):
        timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000)
        timeline.step(28050, altitude=500, accel=0)
        timeline.mark("accepted")
        timeline.hold(28950, accel=0, fresh_baro=False).mark("held")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                accepted, held = marked(events, "accepted"), marked(events, "held")
                self.assertEqual(held["baro_samples"], accepted["baro_samples"])
                self.assertEqual(held["alt"], accepted["alt"])
                self.assertEqual(held["vz"], accepted["vz"])
                self.assertEqual(held["rises"], 0)

    def test_baro_measurement_timing_is_independent_of_extra_loop_ticks(self):
        # Identical accepted 100 ms measurements, with/without 50 ms flight
        # services in between. Reusing data must not change velocity or fire.
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                results = []
                for loop_ms in (10, 100):
                    timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000)
                    # Align both the last accepted sample and flight service
                    # before varying cadence; fly() may end between services.
                    base_msl = 100 + 100 + 100 * 6 - 0.5 * GRAVITY * 6 * 6
                    timeline.step(28050, altitude=base_msl + 1, accel=0)
                    for ms in range(28050 + loop_ms, 31051, loop_ms):
                        timeline.step(ms, altitude=base_msl + (ms - 28000) * 0.02,
                                      accel=0, fresh_baro=((ms - 28050) % 100 == 0))
                    results.append(self.simulate(vehicle, timeline.mark("end"))[-1])
                self.assertEqual(results[0]["baro_samples"], results[1]["baro_samples"])
                self.assertAlmostEqual(results[0]["alt"], results[1]["alt"], places=4)
                self.assertAlmostEqual(results[0]["vz"], results[1]["vz"], places=4)
                self.assertEqual(results[0]["rises"], 0)
                self.assertEqual(results[1]["rises"], 0)

    def test_baro_descent_confirmations_expire_across_sample_gap(self):
        for vehicle in self.binaries:
            for keep_servicing in (False, True):
                with self.subTest(vehicle=vehicle, keep_servicing=keep_servicing):
                    timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000)
                    for ms, altitude in ((28050, 400), (28100, 390), (28150, 380)):
                        timeline.step(ms, altitude=altitude, accel=0)
                    timeline.mark("three")
                    if keep_servicing:
                        timeline.hold(29600, accel=0, fresh_baro=False)
                    timeline.step(29650, altitude=370, accel=0)
                    timeline.mark("resumed")
                    events = self.simulate(vehicle, timeline)
                    self.assertLess(marked(events, "three")["vz"], -2)
                    resumed = marked(events, "resumed")
                    self.assertEqual(resumed["state"], "COAST")
                    self.assertEqual(resumed["rises"], 0)
                    self.assertEqual(resumed["vz"], 0)

    def test_baro_stale_measurement_cannot_prime_or_correct_filter(self):
        timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000)
        # A sample arrives before the next flight tick, then the loop stalls.
        timeline.step(28010, altitude=100, accel=0)
        timeline.mark("before")
        timeline.step(29200, accel=0, fresh_baro=False)
        timeline.mark("after")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                before, after = marked(events, "before"), marked(events, "after")
                self.assertEqual(after["alt"], before["alt"])
                self.assertEqual(after["vz"], before["vz"])
                self.assertEqual(after["rises"], 0)

    def test_baro_four_fresh_equal_valued_samples_can_confirm_descent(self):
        # Equal values can be distinct readings. Three readings plus extra
        # service ticks must not fire; the fourth actual reading may confirm.
        timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000)
        for ms in (28050, 28100, 28150):
            timeline.step(ms, altitude=400, accel=0)
        timeline.hold(28300, accel=0, fresh_baro=False).mark("three")
        timeline.step(28350, altitude=400, accel=0)
        timeline.mark("four")
        timeline.hold(28800, accel=0, fresh_baro=False).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "three")["state"], "COAST")
                self.assertEqual(marked(events, "three")["rises"], 0)
                self.assertEqual(marked(events, "four")["state"], "APOGEE")
                self.assertEqual(events[-1]["rises"], 1)
                self.assertEqual(events[-1]["reason"], "APOGEE")

    def test_backup_with_missing_lost_or_frozen_barometer(self):
        for vehicle in self.binaries:
            for mode in ("missing_at_boot", "lost_in_coast", "frozen_healthy"):
                with self.subTest(vehicle=vehicle, mode=mode):
                    timeline = Timeline().hold(LIFTOFF_MS, baro=mode != "missing_at_boot")
                    timeline.fly(baro=mode != "missing_at_boot",
                                 baro_loss_ms=25000 if mode == "lost_in_coast" else None,
                                 freeze_ms=27000 if mode == "frozen_healthy" else None)
                    timeline.mark("end")
                    events = self.simulate(vehicle, timeline)
                    rises = events_of(events, "rise")
                    self.assertEqual(len(rises), 1)
                    fire = rises[0]
                    self.assertEqual(fire["reason"], "TIMER BACKUP")
                    self.assertGreaterEqual(fire["ms"] - fire["launch_ms"], 19000)
                    self.assertLessEqual(fire["ms"] - fire["launch_ms"], 19100)

    def test_low_altitude_blocks_baro_apogee_but_not_backup(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.hold(22000, altitude=105, accel=6)
        timeline.hold(23000, altitude=110, accel=0)
        timeline.hold(45000, altitude=100, accel=0).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                fire, = events_of(events, "rise")
                self.assertLess(fire["max_alt"], 30)
                self.assertEqual(fire["reason"], "TIMER BACKUP")

    def test_baro_only_launch_and_apogee_when_imu_unavailable(self):
        timeline = Timeline().hold(LIFTOFF_MS, imu=False).fly(imu=False).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                fire, = events_of(events, "rise")
                self.assertEqual(fire["reason"], "APOGEE")
                self.assertGreater(fire["launch_ms"], LIFTOFF_MS)
                self.assertGreaterEqual(fire["ms"], LIFTOFF_MS + APOGEE_SECONDS * 1000)

    def test_reseeded_altitude_step_does_not_cause_early_apogee(self):
        timeline = Timeline().hold(LIFTOFF_MS).fly(reseed_ms=26000).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                fire, = events_of(events, "rise")
                self.assertEqual(fire["reason"], "APOGEE")
                self.assertGreaterEqual(fire["ms"], LIFTOFF_MS + APOGEE_SECONDS * 1000)

    def test_unarmed_fire_and_second_fire_are_refused(self):
        cold = Timeline()
        cold.commands += ["TRY_FIRE", "SNAP end"]
        flight = Timeline().hold(LIFTOFF_MS).fly()
        flight.commands += ["TRY_FIRE", "SNAP end"]
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                self.assertEqual(self.simulate(vehicle, cold)[-1]["rises"], 0)
                end = self.simulate(vehicle, flight)[-1]
                self.assertEqual(end["rises"], 1)
                self.assertEqual(end["fire_count"], 1)

    def test_reset_after_completed_pulse_does_not_refire(self):
        first = Timeline().hold(LIFTOFF_MS).fly(until=34000).mark("reset")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                before = self.simulate(vehicle, first)[-1]
                self.assertTrue(before["fired"])
                self.assertFalse(before["firing"])
                second = Timeline(latch=before).hold(40000).mark("end")
                events = self.simulate(vehicle, second)
                self.assertEqual(events[0]["state"], "DESCENT")
                self.assertFalse(events[-1]["armed"])
                self.assertEqual(events[-1]["rises"], 0)

    def test_reset_in_coast_recovers_and_uses_live_baro(self):
        first = Timeline().hold(LIFTOFF_MS).fly(until=28000).mark("reset")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                before = self.simulate(vehicle, first)[-1]
                self.assertEqual(before["state"], "COAST")
                self.assertFalse(before["fired"])
                second = Timeline(latch=before)
                # Sensor recovery after reboot while the rocket is descending.
                while second.ms < 7000:
                    ms = second.ms + 10
                    second.step(ms, altitude=600 - (ms - 1500) * 0.02, accel=0)
                second.mark("end")
                events = self.simulate(vehicle, second)
                self.assertEqual(events[0]["state"], "COAST")
                self.assertTrue(events[0]["armed"])
                fire, = events_of(events, "rise")
                self.assertEqual(fire["reason"], "APOGEE")

    def test_without_launch_detection_backup_never_starts(self):
        # Pressure indicates a flight but healthy IMU insists on 1g throughout.
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.hold(22000, altitude=150).hold(25000, altitude=300)
        timeline.hold(30000, altitude=150).hold(65000).mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                end = self.simulate(vehicle, timeline)[-1]
                self.assertEqual(end["state"], "ARMED")
                self.assertEqual(end["launch_ms"], 0)
                self.assertEqual(end["rises"], 0)

    def test_launch_time_guard_blocks_early_apogee_after_reset(self):
        latch = {"latch_state": 3, "latch_fired": 0, "latch_launch_ms": 20000}
        timeline = Timeline(latch=latch)
        while timeline.ms < 6000:
            ms = timeline.ms + 10
            timeline.step(ms, altitude=1000 - (ms - 1500) * 0.02, accel=0)
        timeline.mark("end")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                fire, = events_of(self.simulate(vehicle, timeline), "rise")
                self.assertGreaterEqual(fire["ms"] - fire["launch_ms"], 1500)
                self.assertLess(fire["ms"] - fire["launch_ms"], 1700)

    def test_launch_held_sample_counts_once_until_another_sample_arrives(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.hold(20030, accel=6).hold(20100, fresh_imu=False)
        for ms in (20110, 20160, 20210):
            timeline.step(ms, accel=6)
        timeline.hold(20500, fresh_imu=False).mark("four_samples")
        timeline.step(20510, accel=6)
        timeline.mark("fifth_sample")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "four_samples")["state"], "ARMED")
                self.assertEqual(marked(events, "fifth_sample")["state"], "BOOST")

    def test_launch_equal_new_values_keep_original_confirmation_cadence(self):
        timeline = Timeline().hold(LIFTOFF_MS)
        timeline.hold(20190, accel=6).mark("too_soon")
        timeline.hold(20260, accel=6).mark("confirmed")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "too_soon")["state"], "ARMED")
                confirmed = marked(events, "confirmed")
                self.assertEqual(confirmed["state"], "BOOST")
                self.assertEqual(confirmed["launch_ms"], 20260)

    def test_launch_new_low_sample_or_imu_failure_clears_confirmation(self):
        for vehicle in self.binaries:
            for mode in ("low", "down"):
                with self.subTest(vehicle=vehicle, mode=mode):
                    timeline = Timeline().hold(LIFTOFF_MS).hold(20210, accel=6)
                    timeline.hold(20400, accel=1, imu=(mode == "low"))
                    timeline.hold(20550, accel=6).mark("not_enough_new")
                    timeline.hold(20700, accel=6).mark("confirmed")
                    events = self.simulate(vehicle, timeline)
                    self.assertEqual(marked(events, "not_enough_new")["state"], "ARMED")
                    self.assertEqual(marked(events, "confirmed")["state"], "BOOST")

    def test_launch_fresh_sample_after_timeout_cannot_finish_old_confirmation(self):
        timeline = Timeline().hold(LIFTOFF_MS).hold(20210, accel=6)
        # Four previous confirmations; loop and IMU pause > existing 2 s timeout.
        # A fresh sample is already present when service resumes, imuOK still true.
        timeline.step(22420, accel=6)
        timeline.mark("resumed")
        timeline.hold(22610, fresh_imu=False).mark("held")
        timeline.hold(22850, accel=6).mark("confirmed")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "resumed")["state"], "ARMED")
                self.assertEqual(marked(events, "held")["state"], "ARMED")
                self.assertEqual(marked(events, "confirmed")["state"], "BOOST")

    def test_launch_unconsumed_but_expired_sample_is_not_confirmation(self):
        timeline = Timeline().hold(LIFTOFF_MS).hold(20030, accel=6)
        timeline.step(22140, fresh_imu=False)
        timeline.hold(22340, accel=6).mark("four_new")
        timeline.hold(22390, accel=6).mark("five_new")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "four_new")["state"], "ARMED")
                self.assertEqual(marked(events, "five_new")["state"], "BOOST")

    def test_launch_baro_corroboration_requires_new_imu_evidence(self):
        timeline = Timeline().hold(LIFTOFF_MS).hold(20030, accel=6)
        timeline.hold(20500, altitude=140, fresh_imu=False).mark("held_imu")
        timeline.hold(20600, altitude=145, accel=2).mark("new_imu")
        for vehicle in self.binaries:
            with self.subTest(vehicle=vehicle):
                events = self.simulate(vehicle, timeline)
                self.assertEqual(marked(events, "held_imu")["state"], "ARMED")
                self.assertEqual(marked(events, "new_imu")["state"], "BOOST")


# Each unmet safety expectation is a separate test for each board. These start
# as ordinary failing assertions; only reproduced limitations are subsequently
# marked expectedFailure. They must not be mistaken for passing safety checks.
def full_observed_settle_window(self, vehicle):
    timeline = Timeline().hold(11490).mark("before_ten_seconds_of_samples")
    end = self.simulate(vehicle, timeline)[-1]
    self.assertEqual(end["state"], "PAD",
                     "Auto-arm counted boot time before the first observed still sample")


def fresh_samples_required_for_launch(self, vehicle):
    timeline = Timeline().hold(LIFTOFF_MS).mark("before")
    timeline.step(20010, accel=6)
    timeline.step(20020, accel=6)
    timeline.step(20030, accel=6)
    timeline.hold(20500, fresh_imu=False).mark("stale")
    timeline.hold(45000, imu=False, baro=False).mark("end")
    events = self.simulate(vehicle, timeline)
    self.assertEqual(marked(events, "stale")["state"], "ARMED",
                     "A held filtered acceleration was counted as multiple launch confirmations")
    self.assertEqual(events[-1]["rises"], 0)


def fresh_samples_required_for_apogee(self, vehicle):
    timeline = Timeline().hold(LIFTOFF_MS).fly(until=28000).mark("before")
    height_at_8s = 100 + 100 * 6 - 0.5 * GRAVITY * 6 * 6
    timeline.step(28050, altitude=100 + height_at_8s - 20, accel=0)
    timeline.hold(28950, accel=0, fresh_baro=False).mark("stale")
    events = self.simulate(vehicle, timeline)
    self.assertEqual(events[-1]["rises"], 0,
                     "One accepted pressure drop was reused until apogee fired during ascent")


def pulse_remains_bounded_during_loop_stall(self, vehicle):
    # First obtain the automatic firing instant, then reproduce exactly that
    # flight and stop servicing the loop for 900 ms while the gate is high.
    reference = Timeline().hold(LIFTOFF_MS).fly().mark("end")
    fire, = events_of(self.simulate(vehicle, reference), "rise")
    timeline = Timeline().hold(LIFTOFF_MS).fly(until=fire["ms"])
    timeline.step(fire["ms"] + 900, accel=0)
    timeline.mark("end")
    events = self.simulate(vehicle, timeline)
    rise, = events_of(events, "rise")
    fall, = events_of(events, "fall")
    self.assertLessEqual(fall["ms"] - rise["ms"], 450,
                         "400 ms software pulse was extended by the stalled loop")


def reset_preserves_original_backup_deadline(self, vehicle):
    first = Timeline().hold(LIFTOFF_MS).fly(until=30000, baro_loss_ms=25000).mark("reset")
    before = self.simulate(vehicle, first)[-1]
    self.assertEqual(before["state"], "COAST")
    second = Timeline(latch=before).hold(23000, baro=False, accel=0).mark("end")
    fire, = events_of(self.simulate(vehicle, second), "rise")
    total_since_launch = 30000 - before["launch_ms"] + fire["ms"]
    self.assertLessEqual(total_since_launch, 19100,
                         f"Reset restarted backup; fired {total_since_launch} ms after original launch")


def reset_during_pulse_does_not_silently_lose_remaining_output(self, vehicle):
    reference = Timeline().hold(LIFTOFF_MS).fly().mark("end")
    fire, = events_of(self.simulate(vehicle, reference), "rise")
    first = Timeline().hold(LIFTOFF_MS).fly(until=fire["ms"] + 10).mark("reset")
    before = self.simulate(vehicle, first)[-1]
    self.assertEqual(before["gate"], 1)
    self.assertTrue(before["latch_fired"])
    second = Timeline(latch=before).hold(40000).mark("end")
    events = self.simulate(vehicle, second)
    # Deliberately expose the tradeoff; this is not a prescription to refire.
    # No physical ignition/energy assumption is made from a 10 ms GPIO pulse.
    resumed_high_ms = sum(fall["ms"] - rise["ms"] for rise, fall in
                          zip(events_of(events, "rise"), events_of(events, "fall")))
    self.assertGreaterEqual(10 + resumed_high_ms, 400,
                            "Reset after 10 ms leaves fired latch set and never resumes output")


for _name, _check in (
    ("full_observed_settle_window", full_observed_settle_window),
    ("fresh_launch_samples", fresh_samples_required_for_launch),
    ("fresh_apogee_samples", fresh_samples_required_for_apogee),
    ("pulse_bounded_during_stall", pulse_remains_bounded_during_loop_stall),
    ("original_backup_deadline_after_reset", reset_preserves_original_backup_deadline),
    ("interrupted_pulse_not_lost", reset_during_pulse_does_not_silently_lose_remaining_output),
):
    for _vehicle in ("A", "B"):
        def _test(self, check=_check, vehicle=_vehicle):
            check(self, vehicle)
        if not STRICT_SAFETY and _name not in ("fresh_launch_samples", "fresh_apogee_samples"):
            _test = unittest.expectedFailure(_test)
        setattr(EjectionSimulationTest, f"test_safety_{_name}_{_vehicle}", _test)


if __name__ == "__main__":
    if "--report" in sys.argv:
        index = sys.argv.index("--report")
        REPORT_PATH = Path(sys.argv[index + 1]).resolve()
        del sys.argv[index:index + 2]
    program = unittest.main(exit=False)
    if REPORT_PATH and hasattr(EjectionSimulationTest, "source_hashes"):
        EjectionSimulationTest.write_report(program.result)
    if program.result.expectedFailures or program.result.failures:
        print("UNRESOLVED SAFETY FINDINGS: expected failures are not safety passes. "
              "Use --strict-safety for a failing safety gate.", file=sys.stderr)
    sys.exit(0 if program.result.wasSuccessful() else 1)
