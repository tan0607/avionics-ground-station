# Backup timeout set to 16 seconds — 2026-09-09

User requested reducing backup from 19 s to 16 s, reporting simulated apogee about 14 s. Formal A/B and independent HandMotionTest A/B now set `APOGEE_TIMEOUT = 16000` ms after confirmed launch. The barometric apogee path can still trigger earlier. Valid retained launch timing across warm resets remains unchanged.

Scope audit: after removing comments/whitespace, Config.h differs from HEAD only by that constant in all four sketches; Flight.cpp changes are comments only. All other conditions and pulse behavior are unchanged. The hand-test GPIO remains inhibited.

Focused tests failed against the original 19 s setting, including missing/lost/frozen barometer, repeated resets, reset downtime crossing the new deadline, and hand-test backup activation. Legacy invalid-clock fallback expectations were also updated from 19 s to 16 s relative to its boot-local anchor.

HandMotionTest: 13/13 tests passed. Updated its source baseline for the four intentionally edited formal Config.h/Flight.cpp files; all other original hashes verified unchanged. Preserved the initial baseline in `source-snapshot-2026-09-08.json` and refreshed `tested-source-sha256.json`.

All four ESP32-S3 Arduino builds passed (`esp32:esp32:esp32s3`, installed Arduino-ESP32 3.3.11):

| Sketch | Program bytes | Global RAM bytes |
|---|---:|---:|
| MRCC_FlightComputer_A | 449226 | 26144 |
| MRCC_FlightComputer_B | 449230 | 26144 |
| MRCC_HandMotion_A | 450086 | 26144 |
| MRCC_HandMotion_B | 450090 | 26144 |

No upload, reset, serial command, bench test or flight occurred. The requested 16 s setting is not a validation of real-flight timing margin. Existing reset-during-pulse limitations are outside this timer-only change.

Final formal ejection regression: **50 tests run; 48 passed, 2 existing expected failures**, no unexpected failures/errors (46.158 s). The two existing findings are `test_safety_interrupted_pulse_not_lost_A/B`: reset during a pulse does not restore the remaining pulse. Backup deadline and reset-timing tests passed. This is host evidence only.
