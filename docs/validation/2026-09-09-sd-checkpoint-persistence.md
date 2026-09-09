# SD persistence checkpoints — 2026-09-09

Status: hardware persistence is **not yet fixed/validated**. The user's second
report was `file=/FLIGHT011.CSV | base=243 expected=500 actual=243 pending=1`:
the first 257-byte data row did not appear in fresh readback. The current follow-up
uses checked POSIX writes on the existing SD VFS mount. It has not been uploaded
by the agent. Physical power-off persistence and loop timing remain outstanding.

## The card, not the code: EBADF traced to the shipped binary

After the dead-descriptor fix was flashed, the same line appeared again on
`/FLIGHT015.CSV`. The board was confirmed to be running the fix: the IDE's build
of 22:36 contains `append handle` and `cannot size the file`, strings that exist
only in the new sources.

The failure was then traced instruction by instruction in that exact `.elf`:

- `vfs_fat_write` sees the descriptor's stored `O_APPEND` bit and calls
  `f_lseek(file, f_size(file))` **before** `f_write` on every write.
- `f_lseek` calls FatFs `validate()`, whose last check is
  `ff_disk_status(pdrv) & STA_NOINIT`. Disassembly of `validate` at `42023b6c`
  shows that path returning 9.
- `fresult_to_errno` maps FRESULT 9 (`FR_INVALID_OBJECT`) to errno 9, confirmed
  by the `movi.n a2, 9 / beq` pair at `4202627b`.
- `write()` therefore returns -1 with `EBADF`, which the checkpoint reports as
  `short write | io_errno=9`.

So the driver is reporting that **the card is no longer initialised** at the
moment of the write. It was alive moments earlier: the 243-byte header write,
`fsync`, `close`, reopen for read, `fstat`, read, `close` and reopen for append
all succeeded on the same file, and `f_open` validates the volume too. The card
therefore stops answering somewhere between the end of `initSD()` and the first
`loop()` iteration - a window containing `initGPS`, `initIMU`, `filterInit`,
`initBaro` and `initRadio`.

SD and LoRa are on separate buses with no shared pins (SD 14/16/15/7, LoRa
12/13/11/10/9/8), so bus contention is excluded; supply and signal integrity are
not. The user reports the module is fed 5 V and that file creation succeeds while
writes intermittently fail, which matches a rail that sags on the card's write
and erase current bursts rather than a logic fault.

`SD.cardType()` was evaluated as a runtime probe and rejected: in this core it
returns the cached `card->type` and only reports `CARD_NONE` after an explicit
unmount, so a browned-out card still reads as SDHC. A live probe in the failure
path was also rejected - a card re-init can block for about a second, and nothing
in `loop()` may block.

## Mount clock and failure diagnostics

- The mount ladder is now 4 MHz then 1 MHz; 10 MHz is gone. A marginal rail or
  long wiring mounts happily at 10 MHz and then loses the card under sustained
  writes. The achieved frequency is recorded in `sdMountHz`, printed at startup.
- The failure line now carries `(EBADF - card stopped answering)` when the errno
  is EBADF, plus `mount=<n>MHz` and `t=<millis>ms` so a failure can be correlated
  with what the vehicle was doing.
- The first SD auto-recovery is now verbose, printing the full MISO/handshake/card
  probe. `initSD()` runs that probe either way, so this costs no time.
- Two source-level tests pin the ladder and the failure-line fields, since no host
  fake can exercise a mount clock.

Verification: focused suite 32 tests OK, full firmware 123 tests with the same two
expected `test_safety_interrupted_pulse_not_lost_A/B` failures and the pre-existing
`fastapi` environment error, HandMotionTest 13/13. All four ESP32-S3 builds pass
with no warning in any changed file: A=452510, B=452514, HandA=453370, HandB=453374
program bytes, 26184 bytes global RAM each. This is a diagnostic and mount-clock
change; it does not by itself repair a supply that cannot hold the card up.

## Dead-descriptor root cause and fix

The hardware report was
`short write | verified lines=0 | file=/FLIGHT013.CSV | base=243 expected=243 actual=unknown
pending=0 | io_errno=9`: the 243-byte header was written and verified, then the first data
row never reached the card. `io_errno=9` is `EBADF`, and a genuinely short write reports
`EIO`, so the descriptor was already invalid before that row - not a failing card.

Three defects in the preceding revision produced and then misattributed it.

- `operator bool` tested only `fd_ >= 0`, while `write()` short-circuits on the sticky
  error and returns 0 without touching the card. A handle that could no longer write
  still passed `LogCheckpoint::append`'s guard, so a write that never happened was
  reported as a short one, carrying an errno left by an earlier, unrelated call.
- `size()` was `const` but recorded that sticky error. Its only caller on the writer is
  the serial status line, so a failed size report - pure diagnostics - permanently
  disabled flight logging. This call predates the POSIX rewrite, when it was
  `File::size()` and could not fail.
- `initSD()` ran `SD.end()` with the log file still open. Unregistering the `/sd` VFS
  invalidates every descriptor open across it; `startNewLogFile()` would then flush and
  close that dangling descriptor. `initSD()` is reachable at runtime from console `I` and
  from the 30 s SD auto-recovery, and `storageFailure()`'s conditional close leaked the
  descriptor in exactly the case where it had just failed.

A host reproduction using the real headers, a fake `Ops` whose `write()` always succeeds,
and a failing `fstat` reproduced the reported line field-for-field. `vfs_fat_fstat` is
present in `esp32s3-libs/3.3.11/lib/libfatfs.a`, so `fstat` on a live FAT descriptor is
implemented; the size/checksum scheme was left unchanged and still uses it.

Changes, applied identically to all four sketches:

- `operator bool` now means usable for I/O (`fd_ >= 0 && error_ == 0`); the new `isOpen()`
  means owns a descriptor, and cleanup and diagnostics use it. `size()` is replaced by
  `querySize(size_t&)` plus `statErrorNumber()`, which never record the sticky error.
- `LogCheckpoint::append` reports `append handle` with the real errno for a handle that
  cannot write, and counts bytes the filesystem did accept toward `expected` so a partial
  row is not hidden from the diagnostic.
- `closeLogBeforeUnmount()` commits the pending batch and closes the writer before any
  remount; `initSD()` and `formatCard()` call it first. `storageFailure()` now closes
  unconditionally.
- The status line prints `bytes=?` with `io_errno=` instead of a size it could not read,
  and no longer stops the log by asking.

Verification: focused checkpoint suite 30 tests OK (was 24), including a fail-first check
that the descriptor-leak test fails against the previous conditional close. Full firmware
suite 121 tests with the same two expected `test_safety_interrupted_pulse_not_lost_A/B`
failures, plus one pre-existing environment error (`test_simulation_export` needs
`fastapi`, not installed here); this is not a clean safety pass. HandMotionTest 13/13.
The four-way Storage/Health/helper identity holds (Storage differs only in the
FLIGHT/HAND prefix) and `git diff --check` is clean. The HandMotionTest baseline was
refreshed for the eight intentionally changed production files; two stale `.DS_Store`
entries for gitignored files that no longer exist were dropped, which had been failing
that guard before this change. Archived snapshots are untouched.

Board builds pass. `ICM_20948.h` was missing from this machine and was installed
(SparkFun 9DoF IMU Breakout - ICM 20948, 1.3.2); it was the only absent dependency, and
all ten sketches in `firmware/` now compile. The four ESP32-S3 builds used Arduino-ESP32
3.3.11 and FQBN `esp32:esp32:esp32s3`, with no warning in any changed file:

  | Sketch | Program bytes | Global RAM bytes |
  | --- | ---: | ---: |
  | MRCC_FlightComputer_A | 452278 | 26176 |
  | MRCC_FlightComputer_B | 452282 | 26176 |
  | MRCC_HandMotion_A | 453154 | 26176 |
  | MRCC_HandMotion_B | 453158 | 26176 |

The changed logging code is additionally compiled for all four sketches by the host suites
under `-Wall -Wextra -Werror` with ASan/UBSan; the `Health.cpp` status block, which the
host suites do not compile, was type-checked separately against the real header.

**All physical acceptance below remains outstanding.** No firmware upload, serial command,
or hardware action was performed. A compile is not evidence that the descriptor survives a
real remount on the card.

## Checked POSIX follow-up

- Startup/header and data writes now use a move-only descriptor wrapper around
  `open/write/fsync/close` on the existing `/sd` mount. This removes the Arduino
  File wrapper's unobservable C stdio buffering while retaining the same FatFs,
  card, pins, mount frequencies, and checkpoint size/checksum verification.
- A short/failed write, failed sync, or failed close is an error even if some
  bytes can be read back. `io_errno` preserves the first underlying error;
  `fsync`, `close`, and readback stages identify where the checkpoint stopped.
  A size/checksum discrepancy can still report errno 0 when syscalls succeed.
- Reopening append does not create a missing file. This avoids silently
  starting a replacement file without the existing header/data.
- This is a diagnostic and persistence-path change, not evidence of a defective
  card or a confirmed repair. Direct writes may change blocking behavior;
  measure board timing during the same unattended test below.
- Focused suite: 24 tests passed, including real temporary POSIX files,
  injected write/sync/close errors, adapter-plus-checkpoint integration,
  startup failure handling, and the actual periodic Storage functions for
  all four sketches. Sync/close checkpoint tests failed on the preceding
  implementation before the checks were added.
- Full firmware: 116 tests in 118.960 s, OK with the same two existing
  reset-interrupted-pulse expected failures (A/B), not a hardware safety pass.
  Log: `/tmp/mrcc-sd-posix-firmware-tests.log`.
- HandMotionTest: 13 tests passed (11.123 s). Four Arduino ESP32-S3 compiles
  passed: A=451902, B=451906, HandA=452782, HandB=452786 program bytes;
  global RAM=26176 bytes each. Logs: `/tmp/mrcc-sd-posix-compile-*.log`
  and `/tmp/mrcc-sd-posix-hand-tests.log`.
- Four-way Storage/Health/helper consistency and whitespace checks passed.
  The active production snapshot was refreshed for changed Storage files and
  the two new descriptor headers; the pre-checkpoint archive is preserved.

## First hardware failure and diagnostic follow-up

- User reported `[SD] CHECKPOINT/WRITE FAILED: readback size | verified lines=0`.
  The first pending batch did not pass the fresh read handle's size check.
  This does not, by itself, prove either a physical write failure or a false
  metadata reading; the first message did not contain the compared sizes.
- Read-only inspection found Arduino IDE on production A at
  `/dev/cu.usbmodem5C4E0029251`; its cached build includes the checkpoint helper.
  No Serial Monitor was opened and no reset/upload/serial command was issued.
- Failure output now includes `file`, `base`, `expected`, `actual`, and `pending`.
  Sizes are bytes; `actual=unknown` is used when a read handle could not supply
  a size. Appending clears the previous readback-size validity so a write error
  cannot report a stale observation from an earlier batch.
- The size/checksum guard and storage behavior are unchanged by this diagnostic
  follow-up. Tests assert expected/actual/base sizes and pending rows on silent
  loss, distinguish unmeasured sizes, and reject stale diagnostic state.
- At this revision, the requested next evidence was full startup `[SD]` output and the expanded
  first failure line, followed by the same file read from the card. In
  particular, `actual=243` versus a larger `expected` would show the filesystem
  still exposes only the startup bytes; it would not establish why.
- Diagnostic follow-up verification: focused 13/13; full firmware 105 tests,
  OK with the same 2 expected failures (105.866 s); HandMotionTest 13/13
  (10.077 s). All four compiles passed: program bytes A=450858, B=450862,
  HandA=451742, HandB=451746; global RAM 26184 bytes each. The build table below
  records the first checkpoint revision. Follow-up logs are
  `/tmp/mrcc-sd-diagnostic-*-tests.log` and
  `/tmp/mrcc-sd-diagnostic-compile-{A,B,HandA,HandB}.log`.

## Incident evidence

The supplied `FLIGHT003.CSV` contains exactly 496 bytes with three CRLF-terminated
lines: the 204-byte CSV header, a 39-byte power-on marker, and one 253-byte data
row (`PKT=1,T=2.17`). All 55 columns are present. No hidden rows, NUL bytes, or
newline/display issue was found. At onboard 31.7 s the corresponding receiver
record reports `SDF=3,SDL=295,SDE=0` (2026-09-09 18:16:42 Malaysia time).

The user confirmed that running console S/D produced complete data that then
survived power-off/card removal. D flushes, closes, and reopens the file. This
supports testing a periodic close boundary; it does **not** establish the
underlying card, power, or library defect. The prior code already flushed every
second and during automatic five-second status reports.

## Change

- Both production sketches and both HandMotionTest sketches now close/reopen
  at the existing one-second flush cadence. Each checkpoint reads only bytes
  appended since the previous checkpoint, checking exact file size and a
  streaming FNV-1a checksum before reopening in append mode. Memory is bounded
  to a 64-byte read buffer plus checkpoint state, not a full-file buffer.
- `logLineCount` / `SDL` advances only after successful readback and append
  reopen. Writes accepted by the filesystem but not yet checkpointed remain
  uncounted. Rates calculated over short intervals can reflect one-second
  batches rather than individual 100 ms rows.
- Short writes (including partial CRLF), failed reopen, wrong size, failed read
  or seek, and checksum mismatch mark SD down, increase SDE, close the active
  writer, and report the stage over serial. Previously verified count is kept
  until a new file is initialized by existing recovery. SDE remains per-file.
- `checkpoint=...ms max=...ms` in serial SD status measures the last/max actual
  checkpoint for this file. Empty checkpoints do not erase the measurement.
- S, D, list-files and new-log paths use the same checked checkpoint. CSV
  columns, FLIGHT/HAND filename prefixes, radio fields and timing constants,
  sensor code, and flight/pyro logic are unchanged.
- The startup banner now says HEADER VERIFIED rather than implying it has
  verified subsequent data rows.

The initial Arduino File version could only detect observable failures through
readback because flush/close return void. The current descriptor adapter also
checks syscall results and reports errno. Checksums detect accidental corruption,
not all possible media failures. A card/filesystem may still cache or corrupt
data across power loss.

## Initial checkpoint host/build evidence (historical)

- Fail-first integration tests compiled the original Storage functions and
  actual .ino timer branch against a filesystem fake that accepts writes but
  persists only on close. All four sketches failed because SDL counted pending
  rows. The same tests pass with checkpoints.
- 13 focused tests exercise power loss without console calls, repeated
  checkpoints spanning sector boundaries, bounded incremental readback, silent
  persistence loss, same-size corruption, short newline writes, read/seek/open
  errors, preserving the prior verified count, and timing preservation.
- Full firmware suite: 105 tests, OK with **2 existing expected failures**:
  `test_safety_interrupted_pulse_not_lost_A/B`. These remain unresolved safety
  findings; this is not a clean safety pass.
- HandMotionTest's snapshot guard initially failed for the six intentionally
  changed production Storage/Health files. No other baseline mismatch existed.
  The prior manifest is preserved as
  `firmware/HandMotionTest/source-snapshot-2026-09-09-pre-sd-checkpoint.json`;
  only these files and the two new helpers were refreshed in the active guard.
- HandMotionTest suite after updating the reviewed baseline: **13/13 passed**
  (10.872 s), including both threshold profiles and the LOW-only output checks.
- All four Arduino ESP32-S3 builds passed with Arduino-ESP32 3.3.11 and FQBN
  `esp32:esp32:esp32s3`:

  | Sketch | Program bytes | Global RAM bytes |
  | --- | ---: | ---: |
  | MRCC_FlightComputer_A | 450674 | 26184 |
  | MRCC_FlightComputer_B | 450678 | 26184 |
  | MRCC_HandMotion_A | 451546 | 26184 |
  | MRCC_HandMotion_B | 451550 | 26184 |

- `git diff --check` passed. All four Storage/Health/helper paths are identical
  after normalizing the intentional HAND filename prefix. No flight/pyro,
  sensor, pin, frequency, or sampling-timer source was changed.

Commands:

```sh
python3 -m unittest firmware.tests.test_storage_checkpoint -v
python3 -m unittest discover -s firmware/tests -p 'test_*.py' -v
python3 -m unittest discover -s firmware/HandMotionTest/tests -p 'test_*.py' -v
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/HandMotionTest/MRCC_HandMotion_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/HandMotionTest/MRCC_HandMotion_B
```

Builds used separate temporary directories under `/tmp/mrcc-sd-checkpoint-*`.
Full test/compile logs from this run are `/tmp/mrcc-sd-firmware-tests.log` and
`/tmp/mrcc-sd-compile-{A,B,HandA,HandB}.log`; temporary files are not permanent
evidence and commands above reproduce the software checks.

## Required physical acceptance

1. On an isolated bench with energetic loads disconnected, identify the correct
   A/B board and sketch. Record card, power supply and firmware build; maintain
   the same supply and USB configuration across comparisons. Do not compare a
   USB-powered test with a battery-powered run and attribute all differences to
   console commands.
2. Run at least 60 s without S/D. Observe automatic status or receiver SDL/SDE;
   record the final filename and verified count. SD=1 and count alone are not
   physical persistence proof.
3. Power off, then remove the card. Read that exact file on a separate computer.
   It must contain complete 55-column rows extending beyond the first sample
   and at least as many as the latest confirmed SDL for that file. Header/boot
   lines are not data rows. The most recent uncheckpointed batch may be lost;
   SD latency means the window is not guaranteed to be exactly one second.
4. Repeat at several power-off times, including during a checkpoint and across
   sector/cluster growth. Preserve the original failing CSV and test cards'
   data; do not format as a prerequisite.
5. In a separate automatic-serial observation run, measure checkpoint last/max,
   loop rate, IMU rate and barometer freshness. Synchronous close/read/reopen
   can delay the shared loop; host tests and compiles provide no hardware timing
   bound. Investigate gaps against the existing 10 ms IMU / 50 ms barometer and
   flight-service schedules before treating this as flight-ready. Card write
   stalls, power loss and electrical behavior are not simulated by the fake.

No firmware upload, serial command, process restart, or hardware action was
performed during implementation.
