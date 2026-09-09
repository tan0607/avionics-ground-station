"""The recorder block on the downlink, checked against the transmitter source.

The flight computer prints its recording state to USB every 5 s; these fields
are the same state on the air, for the operator who is 19 km from the rocket
and has no USB cable. What matters here is not that they are sent, but the
conditions under which sending them is safe:

  * NOT on every packet, and the cadence is counted in PACKETS while the thing
    it mirrors is counted in SECONDS. That was harmless while the link ran at
    2 Hz and SD_BLOCK_EVERY was 10; at 10 Hz the same 10 would be one second,
    and the constant would still have READ like a deliberate choice while
    quietly reporting five times faster than the console it copies. So the
    cadence is checked against SEND_INTERVAL here, not against a literal.

  * NEVER at the cost of a flight field. The optional blocks are appended after
    the fixed base block, all-or-nothing, bounded by the buffer - so a block
    that does not fit is dropped rather than written over something.

Byte pressure is no longer the argument it was: the block is 10 bytes against a
255-byte limit, where in ASCII it was ~25 bytes against a packet already at 237.
What survives is air time on a link at ~60% duty, and that reporting a line
count faster than the console that produces it buys nothing.

Value-level checks - that SDF/SDL/SDE actually arrive with the right numbers -
live in test_downlink_codec.py, which compiles the encoder and the decoder and
round-trips them. This file is about the RULES around the block.

Run:  python3 -m unittest firmware.tests.test_recorder_downlink
"""
from pathlib import Path
import re
import unittest


SRC = Path(__file__).resolve().parents[1] / "MRCC_FlightComputer_A" / "src"
RADIO = SRC / "Radio.cpp"
CONFIG = SRC / "Config.h"
GROUND = (Path(__file__).resolve().parents[1]
          / "MRCC_GroundStation" / "MRCC_GroundStation.ino")


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


def const(source: str, name: str) -> int:
    match = re.search(rf"{name}\s*=\s*(\d+)", source)
    assert match, f"{name} is not declared"
    return int(match.group(1))


class RecorderDownlinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.radio = RADIO.read_text(encoding="utf-8")
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.ground = GROUND.read_text(encoding="utf-8")
        # The DEFINITION, not the forward declaration above it: the
        # declaration is followed by some other function's body.
        cls.builder = function_body(cls.radio, "buildTelemetryPacket() {")

    def test_recorder_reports_the_file_the_lines_and_the_write_errors(self) -> None:
        """SDF/SDL/SDE - which file, is it growing, is it erroring.

        The three questions `[SD]` answers that a single SD=1 health bit cannot.
        The names are on the RECEIVING end now, because the transmitter sends
        bytes; what the transmitter has to contain is the block that carries
        them.
        """
        for key in ("SDF=", "SDL=", "SDE="):
            self.assertIn(key, self.ground, f"{key} is not on the downlink")

        for source in ("logFileIndex", "logLineCount", "sdErrorCount"):
            self.assertIn(source, self.builder,
                          f"{source} is not read into the packet")

    def test_recorder_block_rides_the_status_cadence(self) -> None:
        """One packet in SD_BLOCK_EVERY, and that has to still mean 5 s.

        This is the check the old literal `>= 2` could not make. SD_BLOCK_EVERY
        counts packets; STATUS_INTERVAL counts milliseconds; SEND_INTERVAL is
        the only thing relating them, and it changed by 5x.
        """
        self.assertIn("SD_BLOCK_EVERY", self.builder,
                      "the recorder block is not gated on the status cadence")

        every = const(self.config, "SD_BLOCK_EVERY")
        send = const(self.config, "SEND_INTERVAL")
        status = const(self.config, "STATUS_INTERVAL")

        self.assertGreater(every, 1, "the recorder block rides every packet")
        self.assertEqual(
            every * send, status,
            f"SD_BLOCK_EVERY ({every}) x SEND_INTERVAL ({send} ms) is "
            f"{every * send} ms, but the console it mirrors prints every "
            f"{status} ms — the cadence stopped tracking the link rate",
        )

    def test_recorder_block_is_dropped_rather_than_truncating_the_packet(self) -> None:
        """It is appended into what the base block left, or not at all."""
        self.assertIn("TX_PAYLOAD_MAX", self.config,
                      "no declared payload limit to append against")

        tail = self.builder[self.builder.index("SD_BLOCK_EVERY"):]
        self.assertIn("TX_PAYLOAD_MAX", tail,
                      "the recorder block is not bounded by the payload limit")

        # The presence flag must be set INSIDE the guarded block. Setting it
        # outside would announce a block the encoder then declined to write,
        # and the decoder rejects the whole frame on that mismatch - so the
        # failure would not be a missing line count, it would be silence.
        guard = tail[tail.index("{"):]
        self.assertIn("TLM_BLOCK_SD", guard[:guard.index("}")],
                      "the SD block flag is set outside the fit check")

    def test_the_flight_fields_are_still_built_first(self) -> None:
        """Ordering is the whole safety argument: state, altitude and pyro are
        written into the fixed base block before anything optional is
        considered."""
        self.assertLess(self.builder.index("flightState"),
                        self.builder.index("SD_BLOCK_EVERY"))
        self.assertLess(self.builder.index("altFiltered"),
                        self.builder.index("SD_BLOCK_EVERY"))
        self.assertLess(self.builder.index("TLM_FLAG_FIRED"),
                        self.builder.index("SD_BLOCK_EVERY"))


if __name__ == "__main__":
    unittest.main()
