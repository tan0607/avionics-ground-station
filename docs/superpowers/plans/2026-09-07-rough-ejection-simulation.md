# Rough-input ejection experiment

- Audit prior simulation reports and rerun current firmware tests.
- Add a standalone validation experiment using existing compiled A/B host logic;
  leave production firmware and thresholds unchanged.
- Exercise seeded white/correlated altitude noise, IMU vibration, spikes,
  irregular scheduling, missing samples, pressure lag and noisy pad-only input.
- Save exact input commands, source hashes, event traces, summary and plots.
- Report early firing, missing firing, duplicate pulses and timing ranges honestly;
  these synthetic stress levels are not measured sensor specifications.
