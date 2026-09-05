"""Guard the vehicle split against wrong-channel builds and flight-code drift."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


FIRMWARE = Path(__file__).resolve().parents[1]


class VehicleSketchTest(unittest.TestCase):
    def test_each_sketch_builds_with_its_vehicle_pins_and_channel(self):
        for vehicle, sck, cs, freq in (("A", 12, 7, 433300000), ("B", 47, 6, 434100000)):
            with self.subTest(vehicle=vehicle):
                sketch = FIRMWARE / f"MRCC_FlightComputer_{vehicle}"
                self.assertTrue((sketch / f"{sketch.name}.ino").is_file())
                config = sketch / "src/Config.h"
                source = f'''#include <cstdint>
#include "{config}"
static_assert(LORA_SCK == {sck}, "LoRa clock pin");
static_assert(SD_CS == {cs}, "SD chip select");
static_assert(VEHICLE == VEHICLE_{vehicle}, "vehicle identity");
static_assert(VEHICLE_NAME[0] == '{vehicle}', "vehicle label");
static_assert(LORA_FREQ == {freq}, "radio channel");
static_assert(PYRO_CONT_ENABLED == 0, "continuity remains disabled");
'''
                with tempfile.TemporaryDirectory() as tmp:
                    src = Path(tmp) / "config.cpp"
                    src.write_text(source)
                    subprocess.run(["c++", "-std=c++17", "-fsyntax-only", str(src)], check=True)

    def test_only_the_three_vehicle_settings_differ(self):
        a = FIRMWARE / "MRCC_FlightComputer_A"
        b = FIRMWARE / "MRCC_FlightComputer_B"
        self.assertTrue(a.is_dir())
        self.assertTrue(b.is_dir())
        files_a = {p.relative_to(a) for p in (a / "src").rglob("*") if p.is_file()}
        files_b = {p.relative_to(b) for p in (b / "src").rglob("*") if p.is_file()}
        self.assertTrue(files_a)
        self.assertEqual(files_a, files_b)
        for path in files_a:
            left, right = (a / path).read_text(), (b / path).read_text()
            if path == Path("src/Config.h"):
                for key in ("LORA_SCK", "SD_CS", "VEHICLE"):
                    pattern = rf"(?m)^(#define\s+{key}\s+)\S+"
                    left = re.sub(pattern, r"\1<vehicle-setting>", left)
                    right = re.sub(pattern, r"\1<vehicle-setting>", right)
            self.assertEqual(left, right, str(path))
        ino_a = (a / f"{a.name}.ino").read_text()
        self.assertEqual(ino_a, (b / f"{b.name}.ino").read_text())
        self.assertIn('Serial.println(" MRCC FLIGHT COMPUTER " VEHICLE_NAME);', ino_a)
        self.assertFalse((FIRMWARE / "MRCC_FlightComputer").exists())


if __name__ == "__main__":
    unittest.main()
