"""The binary downlink codec, checked end to end and against its own duplicates.

test_arming_downlink.py already round-trips the real encoder through the real
decoder and asserts on the flight fields. What it cannot catch is the class of
failure this file exists for: the wire format is written down in TWO places, and
the two ends agree with each other in a host test even when both have drifted
away from what is actually flashed on the other board.

  * The TLM_* constants live in MRCC_FlightComputer_A/src/Config.h and again in
    MRCC_GroundStation.ino, because Arduino builds each sketch on its own and
    there is no header to share. The frequencies have the same problem and the
    same rule. A test compiling both regions against ONE Config.h would never
    see them diverge - so they are compared as TEXT here.

  * The state table is a third copy: Flight.cpp names the states, the ground
    station has to name them identically, and mrcc.py matches on the word. A
    name that drifts does not produce a wrong label, it produces an armed
    vehicle reporting PAD.

And the value checks below are about the two things a scaled-integer wire format
gets wrong quietly: a non-finite input cast into an integer, and a finite one
out of range wrapping through it. Both produce a NUMBER on the ground - in
range, plausible, and false - where the ASCII packet produced `nan` or a
visibly pinned value.

Run:  python3 -m unittest firmware.tests.test_downlink_codec
"""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from firmware.tests.test_arm_switch import function_body
from shared.protocol import mrcc

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "firmware/tests/ejection_host"
GROUND = ROOT / "firmware/MRCC_GroundStation/MRCC_GroundStation.ino"
SRC_A = ROOT / "firmware/MRCC_FlightComputer_A/src"


def region(source: str, tag: str) -> str:
    begin = source.index(f"---- {tag} BEGIN ----")
    end = source.index(f"---- {tag} END ----")
    return source[source.index("\n", begin) + 1 : source.rindex("//", begin, end)]


def tlm_constants(source: str) -> dict[str, str]:
    """Every TLM_* constant, however it happens to be spelled.

    Config.h declares the lengths as `const int` and the rest as `#define`; the
    ground station uses `#define` throughout. That difference is not drift - it
    is one file being a C++ header and the other an .ino - so the comparison is
    on names and values, not on the declaration.
    """
    found = {}
    for pattern in (r"#define\s+(TLM_[A-Z0-9_]+)\s+(.+)",
                    r"const\s+int\s+(TLM_[A-Z0-9_]+)\s*=\s*([^;]+);"):
        for name, value in re.findall(pattern, source):
            found[name] = value.split("//")[0].strip()
    return found


class WireFormatDuplicationTest(unittest.TestCase):
    def test_the_two_copies_of_the_wire_format_agree(self) -> None:
        flight = tlm_constants((SRC_A / "Config.h").read_text())
        ground = tlm_constants(GROUND.read_text())

        self.assertTrue(flight, "no TLM_* constants in Config.h")
        self.assertEqual(
            sorted(flight), sorted(ground),
            "the flight computer and the ground station declare different "
            "wire-format constants",
        )
        for name in flight:
            self.assertEqual(flight[name], ground[name], f"{name} disagrees")

    def test_both_vehicles_ship_the_same_wire_format(self) -> None:
        """A and B fly the same channel-day and the same ground station."""
        a = tlm_constants((SRC_A / "Config.h").read_text())
        b = tlm_constants((ROOT / "firmware/MRCC_FlightComputer_B/src/Config.h").read_text())
        self.assertEqual(a, b)

    def test_the_ground_station_names_the_states_flight_cpp_names(self) -> None:
        flight = dict(re.findall(r"case FS_(\w+):\s*return \"(\w+)\";",
                                 (SRC_A / "Flight.cpp").read_text()))
        ground = dict(re.findall(r"case (\d+): return \"(\w+)\";",
                                 region(GROUND.read_text(), "TLM DECODE")))

        self.assertEqual(len(flight), 7, "Flight.cpp's state table changed shape")
        self.assertEqual(sorted(flight.values()), sorted(ground.values()),
                         "the ground station and Flight.cpp disagree on the state names")

        # And in the right ORDER - the packet carries the index, not the word.
        defines = dict(re.findall(r"#define FS_(\w+)\s+(\d+)", (SRC_A / "Flight.h").read_text()))
        for name, word in flight.items():
            self.assertEqual(ground[defines[name]], word,
                             f"FS_{name} decodes as {ground[defines[name]]}, not {word}")


class CodecRoundTripTest(unittest.TestCase):
    """Drive known values through the real encoder and the real decoder."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-codec-")
        cls.addClassCleanup(cls.tmp.cleanup)
        radio = (SRC_A / "Radio.cpp").read_text()
        unit = Path(cls.tmp.name) / "codec.cpp"
        unit.write_text(f'''#include <cstring>
#include <cstdio>
#include <cmath>
#define main flightHostMain
#include "{HOST / 'main.cpp'}"
#undef main
uint8_t txPacket[256];
int txPacketLen = 0;
int logFileIndex = 3;
unsigned long logLineCount = 12345, sdErrorCount = 7;
{region(radio, "TLM CODEC")}
static void buildTelemetryPacket() {{ {function_body(radio, "static void buildTelemetryPacket()")} }}
{region(GROUND.read_text(), "TLM DECODE")}

int main(int argc, char** argv) {{
  const std::string mode = argc > 1 ? argv[1] : "nominal";
  packetNumber = 42;
  flightState = FS_COAST;          // no PAD block, so the base size is visible
  imuOK = baroOK = sdOK = true; gpsData = gpsFix = true; satellites = 11;
  altFiltered = 1234.5f; maxAlt = 1240.0f; vertVel = -12.3f;
  latitude = 4.09860; longitude = 100.95050;
  gpsAltitude = 45.6f; gpsSpeed = 7.8f; gpsCourse = 123.0f;
  ax = 0.10f; ay = -0.20f; az = 9.79f; gx = 1; gy = -2; gz = 3; heading = 103;
  txFiltered = false;

  if (mode == "nan") {{
    altFiltered = NAN; vertVel = NAN; latitude = NAN; gpsSpeed = NAN;
    ax = NAN; gz = NAN; heading = NAN;
  }} else if (mode == "huge") {{
    // Past every scaled field's range, in both directions.
    vertVel = -1e6f; gpsSpeed = 1e6f; ax = 1e6f; ay = -1e6f; gz = 1e9f;
    heading = 725.0f;                // modular, so this wraps rather than pins
    altFiltered = 31000.0f;          // f32: no ceiling to hit
  }} else if (mode == "sdblock") {{
    packetNumber = SD_BLOCK_EVERY;   // exercises the optional recorder block
  }} else if (mode == "pad") {{
    flightState = FS_PAD;            // exercises the optional arming block
    packetNumber = SD_BLOCK_EVERY;   // ... and both blocks at once
  }}

  buildTelemetryPacket();
  char line[320];
  int n = tlmDecode(txPacket, txPacketLen, line, sizeof(line));
  std::cout << "AIRLEN " << txPacketLen << '\\n';
  std::cout << "DECODED " << (n < 0 ? "REJECTED" : line) << '\\n';
  return 0;
}}
''')
        cls.binary = Path(cls.tmp.name) / "codec"
        subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                        "-I", str(HOST), "-I", str(SRC_A), str(unit),
                        *[str(SRC_A / f"{m}.cpp") for m in ("State", "Filters", "Flight", "Pyro")],
                        "-o", str(cls.binary)], check=True, capture_output=True, text=True)

    def run_mode(self, mode: str) -> tuple[int, str]:
        result = subprocess.run([str(self.binary), mode], input="BOOT\n",
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = {k: v for k, v in (s.split(" ", 1) for s in result.stdout.splitlines()
                                 if s.startswith(("AIRLEN ", "DECODED ")))}
        return int(out["AIRLEN"]), out["DECODED"]

    def fields(self, line: str) -> dict[str, str]:
        return dict(s.split("=", 1) for s in line.split(",")[1:])

    def test_the_packet_is_the_size_the_wire_format_says(self) -> None:
        """52-67 bytes. This is the whole reason 10 Hz is reachable, so it is
        worth failing a build over: at SF7/BW250 the old 185-237 byte ASCII
        packet was 149-187 ms of air and one copy did not fit in a 100 ms
        window."""
        base, _ = self.run_mode("nominal")
        self.assertEqual(base, 52)

        with_sd, _ = self.run_mode("sdblock")
        self.assertEqual(with_sd, 62)

        both, _ = self.run_mode("pad")
        self.assertEqual(both, 67, "worst case is the base plus both blocks")

    def test_every_field_survives_the_round_trip(self) -> None:
        _, line = self.run_mode("nominal")
        f = self.fields(line)
        self.assertEqual(f["PKT"], "42")
        self.assertEqual(f["ST"], "COAST")
        self.assertEqual(f["AL"], "1234.5")
        self.assertEqual(f["MX"], "1240.0")
        self.assertEqual(f["VZ"], "-12.3")
        self.assertEqual(f["LAT"], "4.09860")
        self.assertEqual(f["LON"], "100.95050")
        self.assertEqual(f["GA"], "45.6")
        self.assertEqual(f["GS"], "7.8")
        self.assertEqual(f["CRS"], "123")
        self.assertEqual((f["AX"], f["AY"], f["AZ"]), ("0.10", "-0.20", "9.79"))
        self.assertEqual((f["GX"], f["GY"], f["GZ"]), ("1", "-2", "3"))
        self.assertEqual(f["HDG"], "103")
        self.assertEqual(f["SAT"], "11")
        self.assertEqual((f["SD"], f["BA"], f["IM"]), ("1", "1", "1"))
        self.assertEqual((f["AR"], f["FI"]), ("0", "0"))

    def test_the_line_is_still_the_line_mrcc_py_reads(self) -> None:
        """The binary exists only between the radios. Everything downstream -
        loss tracking, telemetry.csv, the WebSocket, the dashboard - still gets
        the shape it always got, and this is where that claim is tested."""
        _, line = self.run_mode("nominal")
        frame, = mrcc.MrccParser().feed(
            (f"len={len(line)} RSSI=-53 SNR=10.2 | {line}\n").encode())
        # Whole metres, and that is packet.Telemetry's int16 field rather than
        # anything this codec did - the line still carries AL=1234.5, and the
        # decimal survives into `extra` and the CSV. It is asserted at the value
        # the mapping actually produces so that a future change to the CODEC
        # fails here instead of being absorbed by a rounding tolerance.
        self.assertEqual(frame.telemetry.baro_alt_m, 1234)
        self.assertAlmostEqual(frame.telemetry.lat_deg, 4.09860, places=5)
        self.assertEqual(frame.telemetry.gps_sats, 11)
        self.assertEqual(frame.state_name, "COAST")

    def test_the_recorder_block_reports_the_file_lines_and_errors(self) -> None:
        _, line = self.run_mode("sdblock")
        f = self.fields(line)
        self.assertEqual((f["SDF"], f["SDL"], f["SDE"]), ("3", "12345", "7"))

    def test_a_dead_sensor_arrives_as_nan_not_as_a_plausible_number(self) -> None:
        """The sentinel exists so that a non-finite value stays non-finite. Cast
        straight into an int16 it would arrive as a number in range."""
        _, line = self.run_mode("nan")
        f = self.fields(line)
        for key in ("AL", "VZ", "LAT", "GS", "AX", "GZ", "HDG"):
            self.assertEqual(f[key], "nan", f"{key} lost its non-finiteness")
        # And the fields around them are untouched.
        self.assertEqual(f["AZ"], "9.79")
        self.assertEqual(f["ST"], "COAST")

    def test_an_out_of_range_value_pins_rather_than_wrapping(self) -> None:
        """Saturation is visible; a wrap is a plausible reading with the sign
        reversed. An accelerometer glitch must not arrive as a real number
        pointing the other way."""
        _, line = self.run_mode("huge")
        f = self.fields(line)
        self.assertEqual(f["VZ"], "-3276.7")
        self.assertEqual(f["GS"], "3276.7")
        self.assertEqual(f["AX"], "327.67")
        self.assertEqual(f["AY"], "-327.67")
        self.assertEqual(f["GZ"], "32767")
        # Heading is modular: 725 is 5, not a pinned 359.
        self.assertEqual(f["HDG"], "5")
        # Altitude is f32 precisely so that it has no ceiling to pin against.
        self.assertEqual(f["AL"], "31000.0")

    def test_the_over_air_size_is_reported_since_len_can_no_longer_carry_it(self) -> None:
        """mrcc.py compares len= against the ASCII it received, as a splice
        guard, so len= describes the line and not the frame. AIR= is what is
        left saying what the packet actually cost on the air."""
        air, line = self.run_mode("pad")
        self.assertEqual(self.fields(line)["AIR"], str(air))


if __name__ == "__main__":
    unittest.main()
