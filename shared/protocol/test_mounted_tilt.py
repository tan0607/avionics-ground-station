"""Mounted sensor axes, receiver channel framing, and recorded telemetry tilt."""
import tempfile
import unittest
from pathlib import Path

from shared.protocol import mrcc
from backend.session import PACKET_DESC, SessionWriter
from backend.app import split_frame
from backend.wire import telemetry_to_wire


class MountedTiltTest(unittest.TestCase):
    def test_positive_y_mount_geometry_and_live_pad_sample(self):
        for axes, expected in (((0, 9.81, 0), 0), ((9.81, 0, 0), 90),
                               ((0, 0, 9.81), 90), ((0, -9.81, 0), 180),
                               ((6.94, 6.94, 0), 45), ((0, 6.94, 6.94), 45),
                               ((-0.16, 9.63, 0.48), 3)):
            with self.subTest(axes=axes):
                self.assertEqual(mrcc.tilt_from_accel(*axes, nose_axis="y"), expected)
        self.assertIsNone(mrcc.tilt_from_accel(0, 0, 0, nose_axis="y"))

    def test_unconfirmed_mount_keeps_existing_z_convention(self):
        self.assertEqual(mrcc.tilt_from_accel(0, 0, 9.81), 0)
        self.assertEqual(mrcc.tilt_from_accel(0, 9.81, 0), 90)

    def test_each_receiver_announcement_selects_b_mount(self):
        packet = b"MRCC,PKT=1,AX=-0.16,AY=9.63,AZ=0.48,ST=PAD\n"
        for marker in (b"RX ready - vehicle B @ 434.100 MHz\n",
                       b"### GS CHANNEL=B FREQ=434.100MHz PREV_PKTS=12 ###\n",
                       b"### GS STATUS channel=B freq=434.100MHz pkts=12\n"):
            with self.subTest(marker=marker):
                parser = mrcc.MrccParser()
                data = marker + packet
                # All split points, including within the channel name/packet.
                for split in range(len(data) + 1):
                    parser = mrcc.MrccParser()
                    frames = list(parser.feed(data[:split])) + list(parser.feed(data[split:]))
                    self.assertEqual(len(frames), 1)
                    self.assertEqual(frames[0].telemetry.tilt_deg, 3)
                    self.assertEqual(frames[0].extra["AY"], 9.63)

    def test_channel_switch_applies_in_line_order_within_chunk(self):
        packet = b"MRCC,PKT=1,AX=0,AY=9.81,AZ=0,ST=PAD\n"
        frames = list(mrcc.MrccParser().feed(
            packet + b"### GS CHANNEL=B\n" + packet +
            b"### GS CHANNEL=A\n" + packet))
        self.assertEqual([f.telemetry.tilt_deg for f in frames], [90, 0, 90])

    def test_b_tilt_reaches_dashboard_and_csv_without_remapping_raw_axes(self):
        frame, = mrcc.MrccParser().feed(
            b"### GS CHANNEL=B\nMRCC,PKT=1,AX=-0.16,AY=9.63,AZ=0.48,ST=PAD\n")
        t, mf, known, fknown = split_frame(frame)
        wire = telemetry_to_wire(t, 1700000000000, health_known=known,
                                 flags_known=fknown, extra=mf.extra)
        self.assertEqual(wire["tilt_deg"], 3)
        self.assertEqual(wire["extra"]["AY"], 9.63)
        with tempfile.TemporaryDirectory() as tmp:
            sw = SessionWriter(Path(tmp), {"kind": "test"}, PACKET_DESC["mrcc"],
                               mrcc.CSV_COLUMNS)
            sw.write_row(t, 1700000000000, health_known=known, flags_known=fknown,
                         aux=mrcc.aux_csv_row(mf))
            sw.close()
            import csv
            with (sw.dir / "telemetry.csv").open() as stream:
                row, = csv.DictReader(stream)
            self.assertEqual(int(row["tilt_deg"]), 3)
            self.assertEqual(float(row["aux_ay"]), 9.63)


if __name__ == "__main__":
    unittest.main()
