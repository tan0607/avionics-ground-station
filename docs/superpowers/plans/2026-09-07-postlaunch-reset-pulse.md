# Post-launch reset deadline and independent pulse cutoff

## Authorized scope
Preserve PAD/arming rules and deployment thresholds. On a retained post-launch
reset, preserve the original backup deadline with a reset-persistent clock rather
than restarting 19 seconds. End output independently of the application loop.
Keep startup LOW and the anti-refire latch; do not add automatic interrupted-pulse
refiring. Full power loss and interrupted deployment remain explicit limitations.
No flashing, serial control or real pyro operation.

## Implementation (test first)
1. Inspect installed ESP32-S3 timer/RTC APIs and existing latch/host harness.
2. Extend host clock/reset and timer models without duplicating flight logic;
   reproduce deadline and stalled-pulse failures before production changes.
3. Add retained launch clock validation; recover elapsed launch time only for
   an established flight with valid retained timing. Preserve a separate reboot
   sensor-settle guard. Define invalid-time behavior explicitly in evidence.
4. Add a timer-based output cutoff independent of loop servicing, initialization
   failure refusal, cancellation on disarm, and repeat-test/rearm protection.
5. Apply identical shared changes to A/B. Test cold/prelaunch boots, repeated
   resets, elapsed downtime, expired/invalid retained time, nominal flight,
   stalled loop, output refusal and anti-refire behavior.

## Validation
Run focused red/green tests, all firmware tests (ASan/UBSan host builds), strict
safety probes, backend protocol tests, A/B parity and ESP32-S3 compile. Document
exact remaining failures; host timers do not prove electrical or interrupt timing.
Actual board dummy-load/logic-analyzer validation remains required separately.

## Files
Both flight computers: State.{h,cpp}, Flight.cpp, Pyro.{h,cpp}, sketch comments;
host stubs/harness and firmware tests; validation/firmware documentation.

## Completed 2026-09-08
- Implemented A/B retained launch-clock recovery and independent GPTimer cutoff.
- Corrected RTC latch storage to noinit, with version/checksum validation, cold
  boot clearing, and verified invalidation/commit memory ordering in ESP32 code.
- Prelaunch 180 s policy and anti-refire behavior remain; interrupted pulse is
  explicitly unresolved. No hardware accessed or flashed.
- Final firmware suite: 79 pass, 2 expected failures (81 methods); strict safety
  10 pass, 2 fail. Backend 48 pass; A/B compile and parity pass.
- Evidence and limitations: `docs/validation/2026-09-08-postlaunch-reset-pulse.md`.
