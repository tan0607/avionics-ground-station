# Filter tools — before / after figures for the report

Everything here is host-side. None of it is compiled into the
flight build; the Arduino IDE only recurses into `src/`.

## The point

`replay` **links `src/Filters.cpp` directly**. The graphs are
produced by the same C++ that flies, not by a Python copy of it.
Change a cutoff or a Kalman gain in `Config.h`, re-run, and the
figures move with the firmware. There is no second implementation
to drift out of step.

## One command

```sh
./make_report_figures.sh                 # synthetic flight, works today
./make_report_figures.sh FLIGHT001.CSV   # a real log off the SD card
```

Both paths write to `figures/` and print the numbers to the
terminal for pasting into the report.

## The figures

| file | what it shows |
|---|---|
| `01_axial_accel.png` | AZ raw vs filtered, whole flight and a zoom on the burn |
| `02_gyro.png` | pitch rate raw vs filtered, plus the zero-rate bias on the pad |
| `03_attitude.png` | pitch and roll: accelerometer-only vs Kalman, against truth |
| `04_stages.png` | all four stages on one axis — the cumulative story |
| `05_error.png` | RMS attitude error per method (truth runs only) |
| `06_spectrum.png` | spectrum of AZ during the burn, before vs after |
| `07_heading.png` | heading, raw `atan2` vs tilt compensated |
| `08_noise_table.png` | σ before and after per channel, measured on the pad |

`05_error.png` needs the true attitude, so it only appears on the
synthetic run. A real flight has no truth to compare against — that
is exactly why the synthetic path exists.

## Why a synthetic flight is honest here

It is not used to claim a result. It is used to show what the filters
do to a signal whose true attitude is known, so the error numbers
mean something. The noise model is stated in `make_demo_log.py`:
gyro bias 0.85/-1.30/0.42 deg/s, 32 Hz airframe vibration under
thrust, single-sample I2C spikes, an ejection transient at apogee.

Once you have a real `FLIGHTnnn.CSV`, plot that instead. The card
log already carries the raw and filtered columns side by side, so
the same script reads it with no conversion.

## One caveat about a real card log

The IMU runs at ~100 Hz but the card is written at 10 Hz
(`LOG_INTERVAL`). The filtered columns are fine — they were
computed at the full rate and only sampled at 10 Hz. The **raw**
columns are decimated with no anti-aliasing, so on a real log the
32 Hz vibration folds down and `06_spectrum.png` cannot show
anything above 5 Hz.

The spectrum figure is therefore only meaningful on the replay
path, which runs at the real 100 Hz. If you want it from an actual
flight, drop `LOG_INTERVAL` to 20 ms for one characterisation
flight (~20 KB/s to the card) and put it back afterwards.

Every other figure reads correctly off a 10 Hz card log.

## Files

| file | |
|---|---|
| `make_demo_log.py` | synthesises a raw flight (raw channels + true attitude) |
| `replay.cpp` | drives `src/Filters.cpp` over a CSV |
| `arduino_shim/Arduino.h` | just enough Arduino to build that on a laptop |
| `plot_filters.py` | the figures; reads either a replay output or a card log |
| `make_report_figures.sh` | the whole pipeline |
