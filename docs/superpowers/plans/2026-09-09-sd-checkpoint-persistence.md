# SD checkpoint persistence

## Evidence and scope

- User-provided `Downloads/FLIGHT003.CSV` is 496 bytes: 204-byte header,
  39-byte boot marker, and one complete row (`PKT=1,T=2.17`). The matching
  receiver run reported `SDF=3,SDL=295,SDE=0` at onboard 31.7 s.
- User confirmed full data survives power-off after console S/D, but not
  ordinary unattended logging. D flushes, closes, and reopens the file.
- Root hardware/library cause is not proven. Implement the observed successful
  close/reopen boundary periodically, with readback validation and honest counts.
- Scope: production A/B and HandMotionTest A/B Storage, a small checkpoint
  helper, serial checkpoint timing, focused host tests, and bench instructions.
  Preserve filenames, CSV schema, radio packet layout, flight/pyro logic, and
  unrelated dashboard/archive changes. No flashing, reset, or serial commands.

## Implementation

1. Add fail-first host tests for real checkpoint code with a filesystem fake
   that accepts writes but persists only on close, plus silent-loss, corrupted
   readback, truncated newline, and reopen-failure injection.
2. At the existing one-second flush interval, close the append handle, reopen
   read-only, verify exact size and checksum of the pending batch, then reopen
   append. No per-row close or full-file readback.
3. Publish SDL/logLineCount only for verified batches. On write/checkpoint
   failure, increment SDE, mark SD down, and leave prior verified count intact.
   Existing recovery remains responsible for a fresh file.
4. Measure last/max checkpoint duration in serial status; route console/status
   flushes through the same checkpoint. Keep data row scheduling unchanged.

## Verification

- Focused C++ host tests with sanitizers, driven by Python unittest.
- Existing firmware suite and HandMotionTest suite.
- Compile all four ESP32-S3 sketches with arduino-cli; do not upload.
- Inspect diff for identical storage behavior across A/B and retained HAND vs
  FLIGHT naming.
- Hardware acceptance remains outstanding: same board/card/power supply,
  no S/D, run >=60 s, power off, read card on computer, compare persisted rows
  against last verified SDL. Repeat across a sector boundary and multiple
  checkpoints. Record checkpoint duration and sensor/flight-loop cadence;
  synchronous SD I/O can block and host/compile checks cannot establish timing
  or power-loss guarantees. The current pending batch may be lost at power-off.

## Results

### Hardware feedback follow-up

Second report: `/FLIGHT011.CSV base=243 expected=500 actual=243 pending=1`.
The very first 257-byte row was accepted but the reopened file still exposes
only startup bytes. Close/reopen alone has not fixed hardware persistence.
Next implementation: use a small move-only POSIX file adapter for log files on
the existing `/sd` VFS mount. Check direct write/fsync/close return values and
preserve errno, removing the unobservable C stdio buffering layer. Retain the
checkpoint size/checksum guard and all rates/pins/flight logic. Test real POSIX
files plus injected write/sync/close faults before integration; compile all four
sketches and repeat required host suites. No upload/hardware mutation by agent.

User's first uploaded run reports `readback size | verified lines=0`. The first
checkpoint implementation has therefore NOT passed hardware acceptance. Existing
error text omits expected/actual sizes. Before changing storage behavior again,
add fail-first checks for diagnostic state and print filename, committed bytes,
pending row count, expected bytes and actual bytes when known. Keep the strict
size/checksum guard: a new read handle already supplies the size, and a stale
metadata hypothesis is unproven. Re-test and compile all four sketches. Do not
upload or send serial commands automatically.

Diagnostic follow-up implemented and verified: focused 13/13, firmware 105
tests with the same 2 expected failures, HandMotionTest 13/13, and four Arduino
compiles passed. IDE cached dependencies confirm production A uses the inspected
3.3.11 FS/SD libraries and the checkpoint helper. Await expanded first-failure
output from hardware; underlying persistence failure remains unresolved.

- Implemented for all four sketches. Existing one-second timer now performs
  close/readback/checksum/reopen; SDL counts verified batches, errors mark SD
  down, and serial status reports last/max checkpoint duration.
- Fail-first helper tests failed when implementation was absent. Real Storage
  integration failed on all four original paths because pending writes were
  advertised as saved. Both passed after implementation. A further timing
  regression failed first and passed after skipping empty checkpoints.
- Focused suite: 13 passed. Full firmware: 105 tests, OK with 2 existing expected
  failures (reset-interrupted pulse A/B); HandMotionTest: 13 passed.
- Four ESP32-S3 compiles passed. Diff whitespace and four-way storage consistency
  checks passed. Existing HandMotionTest production baseline was archived before
  refreshing only the six modified files and two new helper entries.
- No upload/hardware actions. Actual SD power-off persistence and main-loop
  latency remain unverified. Evidence and acceptance procedure:
  `docs/validation/2026-09-09-sd-checkpoint-persistence.md`.

### Checked descriptor follow-up implementation

Implemented the `/sd` POSIX adapter in all four sketches; startup and data
logging use checked write/fsync/close. Checkpoints retain size/checksum checks
and preserve errno in failure output. Real-file adapter tests and injected
sync/close tests are covered; the latter failed on the prior helper before
checks were added. Focused 24 tests, HandMotionTest 13 tests, four Arduino
compiles and cross-profile consistency checks passed. No upload/reset/serial
commands were issued. Hardware remains unvalidated; see the validation record.

Full firmware regression completed: 116 tests in 118.960 s, OK with the same
two existing reset-interrupted-pulse expected failures. No new failures.
