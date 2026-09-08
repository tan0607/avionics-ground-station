# A/B prelaunch warm-reset recovery

## 已实现范围

按 [implementation plan](../superpowers/plans/2026-09-08-vibration-reset-recovery.md)
实现 prelaunch 180 s 等待进度和人为 disarm 的 retained recovery。
用户观察到 vibration 松线导致 reset；没有把 ejection 造成供电下跌当成已确认原因。
本次未 flash、未接触串口/真实 GPIO、未进行硬件改装或 deployment 测试。

- 首次上电仍按原 boot uptime 等 180 s。记录保存首次 session 的 RTC anchor、
  对应 uptime 和当时 calibration；后续有效 warm reset 不移动原始 deadline。
- Prelaunch reboot 回 PAD，重新获得 fresh IMU 数据、连续 10 s 静止与 gyro
  calibration，不能直接恢复旧 ARMED。等待进度和 AD/AW readiness 使用同一逻辑。
- 人为 `X` disarm 在有效 warm reset 中保留；有效记录的时钟无效时，disarm
  仍保留，同时重新走完整 boot wait。POWERON 启动新 session 并清除旧记录。
- 记录丢失、旧版本、损坏或 RTC timing 无效时，不继承旧等待进度。使用升级后的
  magic/version、字段范围、checksum、volatile invalidation/commit marker 和 barriers。
- session clock 与 launch clock 分开。原 postlaunch 19 s deadline、launch/apogee
  阈值、GPTimer cutoff 和 anti-refire 路径保留。
- Prelaunch 恢复也不再清除有效的 fire-attempt latch；异常 PAD/ARMED + fired
  记录经过多次 reboot 后仍禁止输出，不能因为回 PAD 丢掉 fire-attempt 信息。

## Test-first 证据

修改 production behavior 前，扩展 host harness，将真实 `RTC_NOINIT_ATTR` 字段
放入专用 host section，并在全新 process 中恢复相同 binary 的原始 retained bytes。
不再只凭 state/fired 字段重建一个看似有效的恢复记录。

首次 harness 实验读取到了 ASan 的 global redzones；最终显式跳过这些非 payload
保护区，没有禁用 ASan/UBSan。Host byte layout 不是 ESP32 RTC 的物理布局；这种
传递只用于同一 host binary 的 reset 模型，不用于跨版本固件记录迁移。

修正 harness 后，19 个 auto-arm methods 在旧 firmware 上出现 24 个失败 subcases、
0 errors，包括等待进度恢复、disarm retention 和 fire-attempt 保留。典型证据：
reset 前运行 100 s，重启后原代码报告还要等 178500 ms，目标是 78500 ms。
实现后 19/19 methods 通过。

## 本次实测 host 结果

以下 A/B 结果相同；host 假设启动初始化完成时 boot uptime=1500 ms，后续 loop
每 10 ms、flight service 每 50 ms。实际 sensor/SD/radio 初始化耗时不在这个模型内。

| 场景 | 结果 |
|---|---|
| 100 s 时 warm reset | boot 时剩余 78.500 s；原 session 179.990 s 仍 PAD；180.010 s ARMED |
| 190 s、已经 ARMED 后 warm reset | boot 回 PAD、未 armed、未 calibration、静止观察剩余 10 s；boot 11.510 s 才 ARMED |
| 连续 reset，同时改变本次 boot 的 calibration stub | 原 session 180.010 s ARMED，原时间基准没有移动 |
| 170 s 时 warm reset | 原 session 180 s 等待已满时仍 PAD；新的静止观察完成后才 ARMED |
| PAD 或 ARMED 时 X disarm，再连续两次 reset | 保持 blocked，0 次 HIGH；POWERON 后重新走正常 180 s 准备 |
| RTC 倒退、elapsed 超范围、乘法溢出、零 saved calibration | 不恢复等待进度；有效 disarm 标记不被 timing failure 清掉 |
| 丢失/损坏/旧版本 record | 不能跳过冷启动等待 |
| 恢复后 IMU 丢失或 sample gap | 不绕过新静止窗口 |
| reset 后未重新 ready 就出现起飞输入 | 仍 PAD、没有 launch detection/backup；明确保留这个覆盖边界 |
| 异常 prelaunch fire-attempt latch | 多次 reboot 后仍 fired、未 armed、0 次 HIGH |

## Regression 与真实目标编译

- 完整 firmware suite：92 methods，90 PASS、2 expected failures、0 errors；exit 0。
- 独立 strict safety：12 methods，10 PASS、2 FAIL、0 errors；exit 1。
- 这两个 FAIL 仍是 A/B pulse 开始 10 ms 后 reset、不补发；没有将其改为 PASS。
- A/B `State`、`Flight`、`Console`、`Pyro`、`Filters` shared-source parity 通过；
  Config 仍仅保留原有三处车辆配置差异。此次未改变已有 Pyro/Filters 内容。
- ESP32-S3 A compile：449222 bytes flash，26144 bytes static RAM。
- ESP32-S3 B compile：449226 bytes flash，26144 bytes static RAM。
- 完整 suite 前后的源码 SHA256 一致；`git diff --check` 通过。

A 的真实 ELF 显示 session/disarm 字段位于 `.rtc_noinit`，pulse callback 仍位于
`.iram0.text`。Disassembly 确认 session/disarm 更新前仍写 zero marker + `memw`，
checksum 保存后经 `memw` 再写 valid marker。它证明生成的代码保留了写入顺序，
不证明多字段写入原子化或实际断电 retention。

运行命令：

```sh
python3 -m unittest firmware.tests.test_auto_arm -v
python3 -m unittest discover -s firmware/tests -p 'test_*.py' -v
python3 firmware/tests/test_ejection_simulation.py --strict-safety -k test_safety_ -v
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
git diff --check
```

实际运行使用独立 build paths、JSON report 参数和 unittest wrapper 保存 host events
及 source hashes。证据目录（git-ignored）：
`flights/simulations/20260908-prelaunch-reset-recovery/`。
其中 `autoarm-red.log`、`autoarm-green.log` 保存 red/green；`full-suite.log`、
`full-result.json`、`autoarm-events.json`、`full-ejection.json` 保存完整验证结果。
`full-ejection.json` 的 summary 是整个 92-method suite，runs 是其中 ejection class
的 events；`strict-safety.json` 是独立 strict suite。另有 A/B compile logs 与 A ELF
symbol/disassembly 检查。`before/` 是本轮开始前的源码快照，用于区分已有未提交修改。

## 尚未解决与实机边界

1. 松线可能造成 RTC 丢失或 POWERON；这版仍会重新等待 180 s。没有引入 NVS
   跨任意断电恢复 ARMED，也没有把 brownout/boot counters 当作 retention 证明。
2. Prelaunch 恢复期间不能直接发射；本次重建静止/calibration 之前仍无 launch
   detection。Host 的 10.010 s 新观察窗口不是实际硬件恢复时间保证。
3. Pulse 中途 MCU reset 仍可中断输出，anti-refire 保留，不补发；独立输出硬件
   与供电连续性尚未实现/验证，整个输出模块掉电时软件无法维持输出。
4. 可靠接线、vibration 下供电/EN 稳定性、真实 reset 的 RTC retention、clock drift、
   dummy-load 输出波形和机械 deployment 都仍需实物验证。
