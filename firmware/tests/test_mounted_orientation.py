"""Actual A/B attitude filters with the confirmed +Y nose installation."""
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
FIELDS = ("roll_raw pitch_raw roll_lpf pitch_lpf roll_kal pitch_kal roll_comp "
          "pitch_comp heading heading_filt norm_raw norm_filt ax ay az fax fay faz").split()
G = 9.80665


class MountedOrientationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mrcc-orientation-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for vehicle in ("A", "B"):
            for enabled in (0, 1):
                # Build a private copy to exercise the existing compile-time switch.
                src = Path(cls.temp.name) / f"{vehicle}_{enabled}"
                shutil.copytree(ROOT / f"MRCC_FlightComputer_{vehicle}/src", src)
                config = src / "Config.h"
                config.write_text(config.read_text().replace(
                    "#define FILTER_ENABLED 1", f"#define FILTER_ENABLED {enabled}"))
                binary = src / "orientation"
                subprocess.run([
                    "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "tests/ejection_host"), "-I", str(src),
                    str(ROOT / "tests/orientation_host.cpp"),
                    str(src / "State.cpp"), str(src / "Filters.cpp"), "-o", str(binary),
                ], check=True, capture_output=True, text=True)
                cls.binaries[vehicle, enabled] = binary

    def run_samples(self, key, rows):
        result = subprocess.run([str(self.binaries[key])], input="\n".join(
            " ".join(map(str, row)) for row in rows) + "\n",
            capture_output=True, text=True, check=True, timeout=10)
        return [dict(zip(FIELDS, map(float, line.split())))
                for line in result.stdout.splitlines()]

    def test_upright_and_signed_tilts_keep_sensor_vectors(self):
        for key in self.binaries:
            for roll, pitch in ((0, 0), (30, 0), (-30, 0), (0, 30), (0, -30)):
                with self.subTest(build=key, roll=roll, pitch=pitch):
                    r, p = map(math.radians, (roll, pitch))
                    # Gravity rotated into the installed sensor frame.
                    axes = (-G * math.sin(p), G * math.cos(r) * math.cos(p),
                            -G * math.sin(r) * math.cos(p))
                    out, = self.run_samples(key, [(3000, *axes, 0, 0, 0, 1, 1, 1)])
                    for suffix in ("raw", "lpf", "kal", "comp"):
                        self.assertAlmostEqual(out[f"roll_{suffix}"], roll, delta=0.1)
                        self.assertAlmostEqual(out[f"pitch_{suffix}"], pitch, delta=0.1)
                    for name, value in zip(("ax", "ay", "az"), axes):
                        self.assertAlmostEqual(out[name], value, places=5)
                        self.assertAlmostEqual(out["f" + name], value, places=5)
                    self.assertAlmostEqual(out["norm_raw"], G, places=5)
                    self.assertAlmostEqual(out["norm_filt"], G, places=5)

    def test_gyro_uses_mounted_axes_when_acceleration_is_untrusted(self):
        for vehicle in ("A", "B"):
            for rates, expected in (((10, 0, 0), (10, 0)),
                                    ((0, 0, 10), (0, -10)),
                                    ((0, 10, 0), (0, 0))):
                with self.subTest(vehicle=vehicle, rates=rates):
                    before, after = self.run_samples((vehicle, 1), [
                        (400, 0, G, 0, 0, 0, 0, 1, 1, 1),
                        (100, 0, 6 * G, 0, *rates, 1, 1, 1),
                    ])
                    for suffix in ("kal", "comp"):
                        for axis, angle in zip(("roll", "pitch"), expected):
                            name = f"{axis}_{suffix}"
                            self.assertAlmostEqual(after[name] - before[name], angle, delta=0.3)

    def test_heading_uses_same_mount_for_raw_and_compensated_modes(self):
        for key in self.binaries:
            for degrees in (0, 37, 90, 180, 270):
                with self.subTest(build=key, heading=degrees):
                    h = math.radians(degrees)
                    # AK09916 field for a level mounted frame: bx=my, by=mz, bz=mx.
                    out, = self.run_samples(key, [(3000, 0, G, 0, 0, 0, 0,
                                                  0.6, math.cos(h), math.sin(h))])
                    for name in ("heading", "heading_filt"):
                        error = (out[name] - degrees + 180) % 360 - 180
                        self.assertAlmostEqual(error, 0, delta=0.1)

    def test_heading_compensates_mounted_roll_and_pitch(self):
        h, dip = math.radians(37), 0.6
        for vehicle in ("A", "B"):
            for roll, pitch in ((30, 0), (-30, 0), (0, 30), (0, -30)):
                with self.subTest(vehicle=vehicle, roll=roll, pitch=pitch):
                    r, p = map(math.radians, (roll, pitch))
                    if roll:
                        bx = math.cos(h)
                        by = math.sin(h) * math.cos(r) + dip * math.sin(r)
                        bz = -math.sin(h) * math.sin(r) + dip * math.cos(r)
                    else:
                        bx = math.cos(h) * math.cos(p) - dip * math.sin(p)
                        by = math.sin(h)
                        bz = math.cos(h) * math.sin(p) + dip * math.cos(p)
                    out, = self.run_samples((vehicle, 1), [(3000,
                        -G * math.sin(p), G * math.cos(r) * math.cos(p),
                        -G * math.sin(r) * math.cos(p), 0, 0, 0, bz, bx, by)])
                    self.assertAlmostEqual(out["heading_filt"], 37, delta=0.1)


if __name__ == "__main__":
    unittest.main()
