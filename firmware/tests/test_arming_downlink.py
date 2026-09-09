"""Compile the actual Radio.cpp packet builder AND the actual ground-station
decoder, and check what comes out the far end.

The downlink is binary now, so "what the packet says" is no longer something
you can read off the transmitter alone - it is whatever the two ends AGREE it
says. This test is therefore a round trip: the encoder region and
buildTelemetryPacket() are lifted out of Radio.cpp, the decoder region is lifted
out of MRCC_GroundStation.ino, they are compiled together against the real
Flight modules, and the ASCII line that falls out is fed to mrcc.py exactly as
the laptop would receive it.

That is why every assertion below still reads in ASCII while nothing on the air
is ASCII any more: the ground station re-expands the binary into the identical
`MRCC,PKT=...` line, and these assertions are the check that it does. A field
that the encoder writes and the decoder reads at a different offset fails here,
which is the failure the old single-ended test could not have.

SPI/LoRa transport is outside this host check. Neither extracted region is a
second implementation; source changes on both sides are compiled every run.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from firmware.tests.test_arm_switch import function_body
from firmware.tests.test_ejection_simulation import Timeline
from shared.protocol import mrcc
from backend.wire import telemetry_to_wire

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "firmware/tests/ejection_host"
GROUND = ROOT / "firmware/MRCC_GroundStation/MRCC_GroundStation.ino"


def region(source: str, tag: str) -> str:
    """The source between `---- <tag> BEGIN ----` and `---- <tag> END ----`.

    Extracting by marker rather than by function signature because both regions
    are several functions plus their reasoning, and the reasoning is the part
    most likely to be edited without the code. Copying either into this file
    would defeat the entire point of the test.
    """
    begin = source.index(f"---- {tag} BEGIN ----")
    end = source.index(f"---- {tag} END ----")
    if end < begin:
        raise AssertionError(f"{tag} markers are out of order")
    return source[source.index("\n", begin) + 1 : source.rindex("//", begin, end)]


class ArmingDownlinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-arm-packet-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        for vehicle in "AB":
            src = ROOT / f"firmware/MRCC_FlightComputer_{vehicle}/src"
            radio = (src / "Radio.cpp").read_text()
            body = function_body(radio, "static void buildTelemetryPacket()")
            codec = region(radio, "TLM CODEC")
            decoder = region(GROUND.read_text(), "TLM DECODE")
            unit = Path(cls.tmp.name) / f"packet_{vehicle}.cpp"
            unit.write_text(f'''#include <cstring>
#include <cstdio>
#include <cmath>
#define main flightHostMain
#include "{HOST / 'main.cpp'}"
#undef main
uint8_t txPacket[256];
int txPacketLen = 0;
int logFileIndex = 1;
unsigned long logLineCount = 300, sdErrorCount = 0;
{codec}
static void buildTelemetryPacket() {{ {body} }}
{decoder}
int main(int argc, char**) {{
  int result = flightHostMain();
  if (result) return result;
  packetNumber = 10; // exercises SD optional block as well
  if (argc > 1) {{
    packetNumber = 4294967290UL;
    latitude = -89.99999; longitude = -179.99999;
    gpsAltitude = -9999.9; gpsSpeed = 999.9; gpsCourse = 359;
    satellites = 99; gx = gy = gz = fgx = fgy = fgz = -999;
    ax = ay = az = fax = fay = faz = -99.99;
    altFiltered = maxAlt = 9999.9; vertVel = -999.9;
    headingFilt = heading = 359;
  }}
  buildTelemetryPacket();
  char line[320];
  int n = tlmDecode(txPacket, txPacketLen, line, sizeof(line));
  if (n < 0) {{ std::cerr << "decode rejected a packet the encoder built\\n"; return 2; }}
  std::cout << "PACKET " << line << '\\n';
  return 0;
}}
''')
            binary = Path(cls.tmp.name) / f"packet_{vehicle}"
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                            "-I", str(HOST), "-I", str(src), str(unit),
                            *[str(src / f"{m}.cpp") for m in ("State", "Filters", "Flight", "Pyro")],
                            "-o", str(binary)], check=True, capture_output=True, text=True)
            cls.binaries[vehicle] = binary

    def packets(self, trace, wide=False):
        for vehicle, binary in self.binaries.items():
            result = subprocess.run([str(binary), *(["wide"] if wide else [])],
                                    input="\n".join(trace.commands)+"\n", text=True,
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            line = next(s.removeprefix("PACKET ") for s in result.stdout.splitlines() if s.startswith("PACKET "))
            sample = json.loads(next(t for t in reversed(result.stdout.splitlines())
                                     if t.startswith("{")))
            yield vehicle, line, sample

    def test_actual_pad_packet_transports_countdown_and_records_it(self):
        for v, line, sample in self.packets(Timeline().hold(60000).mark("end")):
            with self.subTest(vehicle=v):
                frame, = mrcc.MrccParser().feed((line + "\n").encode())
                self.assertEqual(frame.extra["AW"], sample["arm_wait"])
                self.assertEqual(frame.extra["AD"], 120)
                self.assertEqual(frame.extra["AS"], 0)
                for field in ("AW", "AD", "AS"):
                    self.assertIn(field, mrcc.AUX_FIELDS)
                row = mrcc.aux_csv_row(frame)
                self.assertEqual(row["aux_ad"], 120)
                self.assertEqual(row["aux_aw"], sample["arm_wait"])
                wire = telemetry_to_wire(frame.telemetry, 1788652800000, extra=frame.extra)
                self.assertEqual(wire["extra"]["AD"], 120)
                self.assertLessEqual(len(line.encode()), 255)
                self.assertIn("IM=1", line)

    def test_optional_arming_block_never_truncates_health_or_partially_appends(self):
        for v, line, _ in self.packets(Timeline().hold(60000).mark("end"), wide=True):
            with self.subTest(vehicle=v):
                self.assertLessEqual(len(line.encode()), 255)
                self.assertIn("BA=1,IM=1", line)
                fields = dict(s.split("=", 1) for s in line.split(",")[1:])
                self.assertEqual(len({"AW", "AD", "AS"} & fields.keys()) in (0, 3), True)

    def test_armed_packet_uses_real_state_and_flag_not_a_zero_countdown(self):
        for v, line, _ in self.packets(Timeline().hold(180100).mark("end")):
            with self.subTest(vehicle=v):
                self.assertIn("ST=ARMED", line)
                self.assertIn("AR=1,FI=0", line)
                self.assertNotIn("AD=", line)


if __name__ == "__main__":
    unittest.main()
