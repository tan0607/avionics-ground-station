# Backup timeout 19 s -> 16 s

User requests only a backup-time change, citing simulated apogee about 14 s. Apply 16000 ms after confirmed launch to formal A/B and isolated HandMotionTest A/B. Do not change launch, barometric apogee, arming, recovery or output duration/guards. No upload or device actions.

1. Update focused backup-deadline host expectations to 16000..16100 ms, including multiple resets. Place expired-reset fixture before the new deadline with reset downtime crossing it. Confirm failure against 19 s source.
2. Change only APOGEE_TIMEOUT in four Config.h files; update nearby comments/current README references. Retain historical validation reports.
3. Synchronize hand-test source-baseline hashes for intentionally updated Config.h/Flight.cpp files; preserve the old snapshot as provenance and record this explicit refresh. Verify other files against the old hashes.
4. Run ejection tests and hand tests; compile all four default sketches. Record passed checks and any existing unresolved tests.

16 s is the requested setting, not a conclusion that the supplied height comparison validates timing margin. Existing barometric apogee can still fire earlier; backup uses confirmed-launch time including valid retained elapsed time after reset.

Completed: timer-only executable diff verified for all four sketches; all four Arduino builds passed. Formal host suite 48 passed + 2 existing interrupted-pulse expected failures; HandMotionTest 13 passed. Evidence: `docs/validation/2026-09-09-backup-timeout-16s.md`. No flash/device action.
