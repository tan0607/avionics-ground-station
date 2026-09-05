# IMU launch confirmation 修复记录 — 2026-09-05

本次仅修复 F1：旧 IMU sample 被重复累计导致假 launch。A/B 使用相同改动，
production 修改限于两份 `Flight.cpp`。其余 F2–F6 未修改，不能据此宣称可安全飞行。

## 实际改动

- 用已有 `lastImuUpdate` 与上次消费的 timestamp 比较。每笔数据最多作为一次
  IMU launch confirmation；新读数的数值可以与上一笔完全相同。
- 仍由现有 50 ms flight service 判断，每轮最多累计一次。保留 >3g、5 次确认、
  baro fallback 的门槛，以及 19 s backup 和 400 ms pulse 等所有配置。
- 没有新数据且未超时：保持计数、不增加。新的 filtered acceleration 不满足门槛、
  IMU 被标记失效或超过现有 2 s `IMU_STALE`：清零。
- 即使 loop 长时间暂停后马上收到新数据，也检查与上一笔消费数据之间的间隔，
  防止把中断前的四次确认与刚恢复的一次拼成 launch。
- Arming 时记录当前 timestamp，排除已在 PAD 观察过的样本。
- 健康 IMU 为 barometric launch fallback 提供 corroboration 时，同样要求本轮
  有新且未过期的 IMU 数据。IMU 明确不可用时，保留现有 baro-only fallback。
  不把 timeout 自行解释为可以只靠 pressure 触发 launch。

## 验证证据

按 plan-first-tdd 先保存计划并运行失败测试，再修改 production code。
修改前，launch-focused suite 的 12 个 methods 中出现 10 个断言失败（包括 A/B
subtests）；修改后，同一组 12 个 methods 全部通过。

额外覆盖：重复读旧值、相同数值的新样本、fresh low acceleration、health loss、
新样本恢复前的长间隔、尚未消费但已经过期的样本，以及 fallback 的 IMU corroboration。
原始 F1 A/B probes 已转为必须通过的普通测试。

| 检查 | 结果 |
|---|---|
| 原始 F1：短暂高 acceleration 后停止更新 | A/B 均保持 ARMED，无假 launch、无 timer 输出 |
| 相同数值的连续新高 acceleration | A/B 正常识别 launch，标准输入仍在 boot 后 20.260 s |
| 所有原有正常轨迹／cadence／noise 情境 | 修复前后完整 state 与 GPIO event 序列一致 |
| Simulation suite | 36 methods：26 通过，10 expected failures；88 次进程执行 |
| 完整 firmware tests | 50 methods：40 通过，10 expected failures，无其他失败 |
| Host 检查 | A/B 均启用 AddressSanitizer 和 UndefinedBehaviorSanitizer，无报告错误 |
| ESP32-S3 编译 | A/B 均通过，Arduino ESP32 core 3.3.11；A 441110 bytes、B 441114 bytes，RAM 各 26056 bytes |

剩余 10 expected failures 对应 F2–F6 × A/B：barometric apogee freshness、
arming 静止时长、reset 后 backup deadline、loop stall pulse 时长、reset 截断 pulse。
这些问题仍然存在。默认 suite 的 `OK (expected failures=10)` 不等于 safety gate 通过。

新事件记录和被测 source hashes：
[2026-09-05-imu-launch-freshness.json](2026-09-05-imu-launch-freshness.json)。
修复前证据保留在 [原始报告](2026-09-05-ejection-simulation.md) 和对应 JSON。

## 重新运行

```sh
python3 firmware/tests/test_ejection_simulation.py -k launch -v
python3 firmware/tests/test_ejection_simulation.py -v --report docs/validation/2026-09-05-imu-launch-freshness.json
python3 -m unittest discover -s firmware/tests
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
```

测试仅覆盖真实 C++ logic 在合成输入下的行为。未 flash、未操作串口、未测量
真实 GPIO、电流或 ejection 机构；BMP280 driver、实际 health detection latency、
硬件 reset/RTC retention 和 timer rollover 的既有限制保持不变。
