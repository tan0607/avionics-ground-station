"""Compile the real isolated Radio.cpp builder and decode its test marker."""
import importlib.util
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from firmware.tests.test_ejection_simulation import Timeline
from shared.protocol import mrcc
# Reuse the production packet harness without modifying its module globals.
spec = importlib.util.spec_from_file_location("hand_packet_harness", ROOT / "firmware/tests/test_arming_downlink.py")
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

class RedirectRoot:
    def __truediv__(self, relative):
        return ROOT / relative.replace("firmware/MRCC_FlightComputer_", "firmware/HandMotionTest/MRCC_HandMotion_")
harness.ROOT = RedirectRoot()

class HandPacketTest(harness.ArmingDownlinkTest):
    def test_actual_pad_packet_transports_countdown_and_records_it(self):
        for v, line, sample in self.packets(Timeline().hold(5000).mark("end")):
            self.assertIn("MRCC,HT=1,", line)
            frame, = mrcc.MrccParser().feed((line + "\n").encode())
            self.assertEqual(frame.extra["HT"], 1)
            self.assertEqual(frame.extra["AD"], 10)
            self.assertIn("BA=1,IM=1", line)
            self.assertLessEqual(len(line), 255)

    def test_armed_packet_uses_real_state_and_flag_not_a_zero_countdown(self):
        for v, line, sample in self.packets(Timeline().hold(20000).mark("end")):
            self.assertIn("MRCC,HT=1,", line)
            self.assertIn("ST=ARMED", line)
            self.assertIn("AR=1,FI=0", line)

if __name__ == "__main__":
    unittest.main()
