from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "firmware" / "MRCC_FlightComputer_A" / "src"


def function_body(source: str, signature: str) -> str:
    for match in re.finditer(re.escape(signature), source):
        rest = source[match.end():].lstrip()
        if not rest.startswith("{"):
            continue

        open_brace = source.index("{", match.end())
        depth = 0
        for index in range(open_brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[open_brace + 1:index]

    raise AssertionError(f"Could not find definition of {signature}")


class NoArmSwitchSenseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = (FW / "Config.h").read_text(encoding="utf-8")
        cls.pyro = (FW / "Pyro.cpp").read_text(encoding="utf-8")
        cls.console = (FW / "Console.cpp").read_text(encoding="utf-8")
        cls.health = (FW / "Health.cpp").read_text(encoding="utf-8")
        cls.radio = (FW / "Radio.cpp").read_text(encoding="utf-8")
        cls.flight = (FW / "Flight.cpp").read_text(encoding="utf-8")
        cls.doctor = (ROOT / "firmware" / "Pyro_Doctor" / "Pyro_Doctor.ino").read_text(
            encoding="utf-8"
        )

    def test_original_optional_switch_configuration_is_restored(self) -> None:
        self.assertRegex(self.config, r"#define ARM_SWITCH_ENABLED\s+0")
        self.assertNotIn("ARM_SWITCH_MODE", self.config)
        self.assertNotIn("ARM_SWITCH_ADC_PIN", self.config)

    def test_no_unused_switch_monitor_remains_in_the_flight_build(self) -> None:
        self.assertNotIn("armSwitchName", self.pyro)
        self.assertNotIn("serviceArmSwitch", self.pyro)
        self.assertNotIn("ARM_SW_UNKNOWN", self.pyro)
        self.assertNotIn("SWITCH=", self.console)
        self.assertNotIn('Serial.print(" | sw=")', self.health)

    def test_disabled_sense_line_does_not_change_the_existing_arm_flow(self) -> None:
        switch = function_body(self.pyro, "bool armSwitchClosed()")
        self.assertIn("#if ARM_SWITCH_ENABLED", switch)
        self.assertIn("return true;", switch)

        arm = function_body(self.pyro, "bool armPyro()")
        self.assertIn("armSwitchClosed()", arm)
        self.assertIn("pyroArmed = true", arm)

        self.assertRegex(self.config, r"#define AUTO_ARM_ENABLED\s+1")
        auto = function_body(self.flight, "static void tryAutoArm()")
        self.assertIn("armFlight()", auto)

    def test_mrcc_packet_has_original_precision_and_no_switch_field(self) -> None:
        packet = function_body(self.radio, "static void buildTelemetryPacket()")
        self.assertNotIn("SW=%", packet)
        self.assertNotIn("armSwitch", packet)
        self.assertIn("GA=%.1f", packet)
        self.assertIn("AX=%.2f,AY=%.2f,AZ=%.2f", packet)

    def test_pyro_doctor_only_tests_the_mosfet_output(self) -> None:
        self.assertNotIn("ARM_SW_", self.doctor)
        self.assertNotIn("arm switch", self.doctor.lower())
        self.assertNotIn("STAGE 4", self.doctor)


if __name__ == "__main__":
    unittest.main()
