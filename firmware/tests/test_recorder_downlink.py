"""The recorder block on the downlink, checked against the transmitter source.

The flight computer prints its recording state to USB every 5 s; these fields
are the same state on the air, for the operator who is 19 km from the rocket
and has no USB cable. What matters here is not that they are sent, but the two
conditions under which sending them is safe:

  * NOT on every packet. The packet is 189-204 bytes measured and two copies
    already fill 87% of the 500 ms window. Three more fields at 2 Hz is byte
    budget and air time this link does not have.
  * NEVER at the cost of a flight field. The buffer truncates silently, and
    what is at the tail is the health block - so a recorder field that pushed
    the packet over the limit would delete the fields that say something is
    wrong, to report how many lines got written.

Run:  python3 firmware/tests/test_recorder_downlink.py
"""
from pathlib import Path
import re
import unittest


SRC = Path(__file__).resolve().parents[1] / "MRCC_FlightComputer" / "src"
RADIO = SRC / "Radio.cpp"
CONFIG = SRC / "Config.h"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0

    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace + 1 : index]

    raise AssertionError(f"Could not find the end of {signature}")


class RecorderDownlinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.radio = RADIO.read_text(encoding="utf-8")
        cls.config = CONFIG.read_text(encoding="utf-8")
        # The DEFINITION, not the forward declaration above it: the
        # declaration is followed by some other function's body.
        cls.builder = function_body(cls.radio, "buildTelemetryPacket() {")

    def test_recorder_reports_the_file_the_lines_and_the_write_errors(self) -> None:
        """SDF/SDL/SDE - which file, is it growing, is it erroring.

        The three questions `[SD]` answers that a single SD=1 health bit cannot.
        """
        keys = set(re.findall(r"([A-Z][A-Z0-9]*)=%", self.builder))

        for key in ("SDF", "SDL", "SDE"):
            self.assertIn(key, keys, f"{key} is not on the downlink")

    def test_recorder_fields_stay_inside_the_contract_window(self) -> None:
        """backend/tests.py reads the format strings from the top of the builder
        down to `txPacketLen = strlen`, and fails a key with nowhere to land. A
        block appended after that line would send fields the ground station
        silently drops - the exact failure that test exists to catch."""
        window = self.builder[: self.builder.index("txPacketLen = strlen")]

        for key in ("SDF=", "SDL=", "SDE="):
            self.assertIn(key, window, f"{key} is outside the checked window")

    def test_recorder_block_rides_the_status_cadence(self) -> None:
        """One packet in ten, not every packet."""
        self.assertIn("SD_BLOCK_EVERY", self.config,
                      "no cadence constant for the recorder block")
        self.assertIn("SD_BLOCK_EVERY", self.builder,
                      "the recorder block is not gated on the status cadence")

        # 2 Hz downlink, so 10 packets is the 5 s STATUS_INTERVAL the console
        # prints on. A block on every packet is the failure this guards.
        every = int(re.search(r"SD_BLOCK_EVERY\s*=?\s*(\d+)", self.config).group(1))
        self.assertGreaterEqual(every, 2, "the recorder block rides every packet")

    def test_recorder_block_is_dropped_rather_than_truncating_the_packet(self) -> None:
        """It is appended into what the flight fields left, or not at all."""
        self.assertIn("TX_PAYLOAD_MAX", self.config,
                      "no declared payload limit to append against")

        # The append has to be bounded by the room remaining, and it has to be
        # possible for it NOT to happen. Writing into the buffer unconditionally
        # is what silently eats the health block.
        tail = self.builder[self.builder.index("SDF="):]
        self.assertIn("TX_PAYLOAD_MAX", tail,
                      "the recorder block is not bounded by the payload limit")

    def test_the_flight_fields_are_still_built_first(self) -> None:
        """Ordering is the whole safety argument: state, altitude and pyro are
        written before anything optional is considered."""
        self.assertLess(self.builder.index("ST=%s"), self.builder.index("SDF="))
        self.assertLess(self.builder.index("AL=%.1f"), self.builder.index("SDF="))


if __name__ == "__main__":
    unittest.main()
