"""Power loss and failed-write/readback tests of the actual C++ checkpoint helper."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re

ROOT = Path(__file__).resolve().parents[2]
SKETCHES = [ROOT / "firmware" / name for name in (
    "MRCC_FlightComputer_A", "MRCC_FlightComputer_B",
    "HandMotionTest/MRCC_HandMotion_A", "HandMotionTest/MRCC_HandMotion_B",
)]


class StorageCheckpointTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-storage-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = []
        for sketch in SKETCHES:
            binary = Path(cls.tmp.name) / sketch.name
            result = subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(sketch / "src"),
                str(ROOT / "firmware/tests/storage_checkpoint_host.cpp"),
                "-o", str(binary),
            ], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries.append(binary)

    def scenario(self, name):
        for binary in self.binaries:
            with self.subTest(sketch=binary.name):
                result = subprocess.run([str(binary), name], text=True,
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_fake_reproduces_accepted_but_volatile_data(self): self.scenario("volatile")
    def test_periodic_checkpoint_survives_power_loss_without_console(self): self.scenario("persist")
    def test_multiple_batches_cross_sectors_and_read_only_new_bytes(self): self.scenario("batches")
    def test_silent_close_failure_does_not_count_data(self): self.scenario("silent_loss")
    def test_same_size_corruption_is_detected(self): self.scenario("corrupt")
    def test_read_open_failure(self): self.scenario("read_open")
    def test_append_reopen_failure(self): self.scenario("append_open")
    def test_short_read_failure(self): self.scenario("read_failure")
    def test_seek_failure(self): self.scenario("seek_failure")
    def test_sync_failure_is_not_committed(self): self.scenario("sync_failure")
    def test_checked_close_failure_is_not_committed(self): self.scenario("close_failure")
    def test_partial_newline_is_not_a_successful_row(self): self.scenario("partial_newline")
    def test_failed_size_report_does_not_stop_logging(self): self.scenario("status_stat")
    def test_open_but_unwritable_handle_is_reported_as_such(self): self.scenario("dead_handle")


class CheckedLogFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-posix-log-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = []
        for sketch in SKETCHES:
            binary = Path(cls.tmp.name) / sketch.name
            result = subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(sketch / "src"),
                str(ROOT / "firmware/tests/checked_log_file_host.cpp"),
                "-o", str(binary),
            ], text=True, capture_output=True)
            if result.returncode: raise AssertionError(result.stderr)
            cls.binaries.append(binary)

    def scenario(self, name):
        for binary in self.binaries:
            with self.subTest(sketch=binary.name), tempfile.TemporaryDirectory() as mount:
                result = subprocess.run([str(binary), name, mount], text=True,
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_real_adapter_checkpoint(self): self.scenario("checkpoint_ok")
    def test_real_adapter_checkpoint_sync_error(self): self.scenario("checkpoint_sync")
    def test_real_adapter_checkpoint_close_error(self): self.scenario("checkpoint_close")
    def test_real_file_append_sync_close_readback_and_ownership(self): self.scenario("normal")
    def test_write_failure_preserves_enospc(self): self.scenario("write_error")
    def test_fsync_failure_preserves_eio(self): self.scenario("sync_error")
    def test_close_failure_is_reported_and_releases_ownership(self): self.scenario("close_error")
    def test_real_stat_failure_does_not_poison_the_writer(self): self.scenario("stat_error")
    def test_real_dead_handle_is_not_reported_as_a_short_write(self): self.scenario("dead_handle")


class SdMountLadderTest(unittest.TestCase):
    """The mount clock is a hardware setting no host fake can exercise, so it is
    pinned here. 10 MHz mounts on marginal wiring and then loses the card during
    sustained writes, which the driver reports as an invalid descriptor."""

    def test_all_sketches_mount_at_4mhz_then_1mhz(self):
        for sketch in SKETCHES:
            with self.subTest(sketch=sketch.name):
                source = (sketch / "src" / "Storage.cpp").read_text()
                self.assertEqual(re.findall(r"sdMountHz = (\d+);", source),
                                 ["0", "4000000", "1000000", "0"], "mount ladder changed")
                self.assertNotIn("10000000", source, "10 MHz mount reintroduced")
                self.assertEqual(source.count("SD.begin(SD_CS, sdSPI, sdMountHz)"), 2,
                                 "a mount bypasses the recorded frequency")

    def test_failure_line_reports_mount_and_errno_meaning(self):
        for sketch in SKETCHES:
            with self.subTest(sketch=sketch.name):
                source = (sketch / "src" / "Storage.cpp").read_text()
                failure = source[source.index("static void storageFailure"):]
                failure = failure[:failure.index("\nvoid serviceLogging")]
                for field in ("io_errno=", "EBADF - card stopped answering",
                              "mount=", "t="):
                    self.assertIn(field, failure, field)


def definition(source, signature):
    match = re.search(re.escape(signature) + r"\s*\{", source)
    if not match:
        raise AssertionError(f"missing function {signature}")
    start = source.index("{", match.start())
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{": depth += 1
        if source[i] == "}": depth -= 1
        if depth == 0: return source[match.start():i + 1]
    raise AssertionError(f"unclosed function {signature}")


class StorageIntegrationTest(unittest.TestCase):
    """Compile actual Storage functions and the actual .ino checkpoint timer.

    Sensors/flight state come from the existing host harness; no serial device
    is opened. A fake SD accepts data but does not persist it until close.
    """
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mrcc-storage-integration-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = []
        host = ROOT / "firmware/tests/ejection_host"
        for sketch in SKETCHES:
            src = sketch / "src"
            storage = (src / "Storage.cpp").read_text()
            globals_ = storage[storage.index("SPIClass sdSPI"):storage.index("static void logOneLine();")]
            logging = storage[storage.index("// LOGGING -"):storage.index("// DUMP -")]
            # Compile the existing periodic branch verbatim, not a replacement
            # scheduler that might pass even if the sketch no longer calls it.
            ino = (sketch / f"{sketch.name}.ino").read_text()
            timer = definition(ino, "if (millis() - lastFlush >= FLUSH_INTERVAL)")
            unit = Path(cls.tmp.name) / f"{sketch.name}.cpp"
            unit.write_text(f'''#define main checkpointFakeMain
#include "{ROOT / 'firmware/tests/storage_checkpoint_host.cpp'}"
#undef main
#define main flightHostMain
#include "{host / 'main.cpp'}"
#undef main
using File = FakeFile;
using CheckedLogFile = FakeFile;
using LogFileSystem = FakeFS;
using String = FakeFile::Text;
struct SPIClass {{ explicit SPIClass(int) {{}} }};
#define HSPI 1
#define PROBE_NO_MODULE 0
#define FILE_WRITE "w"
#define FILE_READ "r"
#define FILE_APPEND "a"
FakeFS SD;
{globals_}
static void verifyWrite();
static void storageFailure(const char* reason, int ioError = 0);
static void logOneLine();
void flushSD();
{definition(storage, 'void startNewLogFile()')}
{definition(storage, 'static void verifyWrite()')}
{logging}
static unsigned long lastFlush = 0;
static void tick() {{ serviceLogging(); {timer} }}
int main(int argc, char** argv) {{
  try {{
    require(argc == 2, "scenario required");
    const std::string scenario = argv[1];
    sdOK = true; bootCount = 1; resetReasonName = "POWER ON (clean start)";
    if (scenario == "header_sync") LogFiles.disk.failSync = true;
    if (scenario == "header_close") LogFiles.disk.failClose = true;
    startNewLogFile();
    if (scenario == "header_sync" || scenario == "header_close") {{
      require(!sdOK && sdErrorCount == 1 && !logFile, "header failure not reported/stopped");
      require(logLineCount == 0, "failed header counted data");
      return 0;
    }}
    require(sdOK, "startup failed");
    const std::string header = LogFiles.disk.bytes;
    require(header.size() == 243, "CSV initialization changed");
    for (hostNowMs = 100; hostNowMs < 1000; hostNowMs += 100) tick();
    require(logLineCount == 0, "SDL counts unverified writes before checkpoint");
    require(LogFiles.disk.bytes == header, "fake must retain only startup bytes before checkpoint");
    hostNowMs = 1000; tick();
    require(logLineCount == 10, "one-second checkpoint did not confirm ten rows");
    require(sdOK && sdErrorCount == 0, "successful checkpoint reported failure");
    const auto saved = LogFiles.disk.bytes;
    if (scenario == "stranded_handle") {{
      // What SD.end() does to a descriptor that is still open. The row must be
      // reported as the dead handle it is, and the fd must not be left behind
      // for the next remount to strand.
      logFile.ioError = EBADF;
      hostNowMs = 1100; tick();
      require(!sdOK && sdErrorCount == 1, "a dead descriptor was not reported");
      require(!logFile.isOpen(), "failed logger leaked its descriptor");
      require(logLineCount == 10, "a row that was never written was counted");
      require(LogFiles.disk.bytes == saved, "failure changed the durable batch");
      return 0;
    }}
    if (scenario == "remount_teardown") {{
      for (hostNowMs = 1100; hostNowMs <= 1400; hostNowMs += 100) tick();
      require(logLineCount == 10, "pending rows counted before their checkpoint");
      const unsigned closesBefore = LogFiles.disk.closes;
      closeLogBeforeUnmount(); // what initSD()/formatCard() do before SD.end()
      require(logLineCount == 14, "pending rows were not committed before the unmount");
      require(!logFile.isOpen(), "a live descriptor was left open across the unmount");
      require(LogFiles.disk.closes > closesBefore, "file was never closed before the unmount");
      require(sdOK && sdErrorCount == 0, "a clean teardown reported an error");
      const auto durable = LogFiles.disk.bytes;
      require(std::count(durable.begin(), durable.end(), '\\n') == 16,
              "not exactly header, boot, and fourteen complete rows");
      return 0;
    }}
    if (scenario == "idle_timing") {{
      sdCheckpointLastUs = 1234; sdCheckpointMaxUs = 2000;
      flushSD();
      require(sdCheckpointLastUs == 1234 && sdCheckpointMaxUs == 2000,
              "empty status checkpoint erased last real duration");
      return 0;
    }}
    if (scenario == "failure") LogFiles.disk.loseOnClose = true;
    for (hostNowMs = 1100; hostNowMs <= 2000; hostNowMs += 100) tick();
    if (scenario == "failure") {{
      require(!sdOK && sdErrorCount == 1, "silent loss not surfaced as SD down/error");
      require(logLineCount == 10, "failed batch advanced verified counter");
      require(!logFile, "failed logger still writable");
      for (hostNowMs = 2100; hostNowMs <= 4000; hostNowMs += 100) tick();
      require(sdErrorCount == 1, "offline logger repeats errors every loop");
      require(LogFiles.disk.bytes == saved, "failure changed prior durable batch");
    }} else {{
      require(logLineCount == 20, "second checkpoint count incorrect");
      const auto durable = LogFiles.disk.bytes;
      hostNowMs = 2100; tick();
      logFile.powerLoss(); // no S/D or final flush
      require(LogFiles.disk.bytes == durable, "unattended checkpoint lost on power-off");
      require(std::count(durable.begin(), durable.end(), '\\n') == 22,
              "not exactly header, boot, and twenty complete rows");
      require(durable.rfind(header, 0) == 0, "append overwrote header");
    }}
    return 0;
  }} catch (const std::exception& e) {{ std::cerr << e.what() << '\\n'; return 1; }}
}}
''')
            binary = Path(cls.tmp.name) / sketch.name
            result = subprocess.run([
                "c++", "-std=c++17", "-fsanitize=address,undefined",
                "-I", str(host), "-I", str(src), str(unit),
                *[str(src / f"{m}.cpp") for m in ("State", "Filters", "Flight", "Pyro")],
                "-o", str(binary),
            ], text=True, capture_output=True)
            if result.returncode: raise AssertionError(result.stderr)
            cls.binaries.append(binary)

    def scenario(self, name):
        for binary in self.binaries:
            with self.subTest(sketch=binary.name):
                result = subprocess.run([str(binary), name], text=True,
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_remount_teardown_commits_and_closes_before_unmount(self): self.scenario("remount_teardown")
    def test_dead_descriptor_is_reported_and_released(self): self.scenario("stranded_handle")
    def test_header_sync_failure_stops_logging(self): self.scenario("header_sync")
    def test_header_close_failure_stops_logging(self): self.scenario("header_close")
    def test_real_periodic_logger_persists_without_console(self): self.scenario("persist")
    def test_real_logger_surfaces_loss_and_preserves_verified_count(self): self.scenario("failure")
    def test_status_without_pending_rows_keeps_last_measured_duration(self): self.scenario("idle_timing")


if __name__ == "__main__": unittest.main()
