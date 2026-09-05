from pathlib import Path
import unittest


SKETCH = Path(__file__).resolve().parents[1] / "TX_Doctor" / "TX_Doctor.ino"


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


class TxDoctorStartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SKETCH.read_text(encoding="utf-8")

    def test_radio_presence_is_not_run_automatically_during_setup(self) -> None:
        setup = function_body(self.source, "void setup()")

        self.assertNotIn(
            "testPresence();",
            setup,
            "Radio Test 1 must not block TX Doctor before menu commands are accepted",
        )

    def test_radio_presence_remains_available_on_key_one(self) -> None:
        loop = function_body(self.source, "void loop()")

        self.assertIn("if      (c == '1') testPresence();", loop)


if __name__ == "__main__":
    unittest.main()
