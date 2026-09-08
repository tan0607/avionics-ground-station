# A/B post-launch reset 与 pulse cutoff

## 范围

按 [implementation plan](../superpowers/plans/2026-09-07-postlaunch-reset-pulse.md)
实现两个可独立验证的修复：post-launch warm reset 保留原始 backup deadline；
GPTimer ISR 独立截止输出。没有 flash、串口命令、真实 GPIO 或 pyro 操作。
Prelaunch reset 仍回 PAD，重新满足 180 s boot uptime、连续 10 s stable IMU
和 gyro calibration。没有缩短等待，也没有加入 interrupted-pulse 自动 re-fire。

## 实现

- `State.cpp` 的 flight latch 改用 `RTC_NOINIT_ATTR`，保存原始 launch RTC
  counter 与当时 slow-clock calibration。记录包含版本、字段范围与 checksum
  校验；power-on reset 清除旧记录。每次状态改变及 reboot recovery 都保留原始
  counter，不能把 reboot 时间覆盖成 launch 时间。
- Reset 后计算已经过去的 flight time，再以当前 `millis()` 为本次 boot 的
  增量时间基准。因此 restart downtime 也计入 backup。保留 barometric inference
  原有 1.5 s post-initialization guard；已过期的 timer backup 不额外等待该 guard。
- `Pyro.cpp` 使用 checked GPTimer API，在 ISR 中通过 GPIO LL 写 LOW。主 loop
  负责清理 timer/status，并保留 LOW fallback。开始输出与 ISR cutoff 用同一
  critical-section lock 排序；ISR 不执行 Serial、SD 或 latch 写入。
- Timer 初始化、callback registration、enable、counter/alarm setup、start 失败
  的测试均无 HIGH。重复 bench command 不延长现有 pulse；disarm 取消输出，下一次
  bench pulse 使用自己的 deadline。Timer start 失败后仍保留已写入的 anti-refire
  latch，避免把不确定状态自动重试成第二次输出。
- `FI` 仍是 fire-attempt latch。Serial 文案不再把它称为已完成 deployment，延迟
  loop service 的日志也不再把 service 时间误称为实际 pulse 长度。

## Red → green 与实际编译检查

修改 production code 前：新 interrupts-only stall test 在 A/B 均看到 GPIO 保持
HIGH；timer failure probes 共 12 个 subcases 均仍输出 HIGH。原 strict single-reset
probe 均为原始 detected launch 后 30.300 s；新增 repeated-reset probe 为 32.300 s。

修改后（真实 A/B State/Filters/Flight/Pyro C++，ASan/UBSan，host peripherals）：

| 检查 | A/B 结果 |
|---|---|
| Reset 后原始 backup deadline | detected launch 后 19.050 s 输出 |
| 连续两次 reset | 输出仍在原始 deadline 的既有 100 ms test envelope 内 |
| Restart downtime 已跨过 deadline | 完成 host startup 后在 flight-service cadence 输出 |
| 主 loop 停顿 900 ms | ISR 在 400 ms 截止；无需再调用 servicePyro |
| Timer 六类失败 | 全部无 HIGH |
| 重复 bench command | 不延长 400 ms pulse |
| Disarm 再开始 bench pulse | 前次提前 LOW，后次拥有独立 400 ms deadline |
| Power-on 携带看似有效的旧 flight latch | 清除后 PAD、unarmed、无输出 |
| Prelaunch reset | 保留重新等待 180 s 的既有测试行为 |

编译后的实际目标文件检查发现：初版普通变量的 invalidation store 被优化掉。
将 commit marker 改为 volatile 并添加 memory barriers 后，A 的 disassembly
明确包含 payload 更新前的 zero store 与 `memw`，以及 checksum 后、valid marker
前的 `memw`。这是编译产物检查，不是物理断电测试，也不使多字记录写入原子化。

ELF 检查确认 latch 位于 `.rtc_noinit`，pulse callback 位于 `.iram0.text`，
ISR lock/flag 位于 DRAM。使用本机 Arduino-ESP32 3.3.11 的 GPTimer、
`soc/rtc.h` 与 `esp_private/esp_clk.h`；升级 SDK 后需重新核对这些接口。

## 验证结果

- 完整 firmware suite：81 methods，79 pass、2 expected failures。
- Strict safety：12 methods，10 pass、2 FAIL，exit 1 保留。
- Backend：48 tests pass；A/B source parity、`git diff --check` pass。
- ESP32-S3 A compile：448502 bytes flash，26136 bytes static RAM。
- ESP32-S3 B compile：448506 bytes flash，26136 bytes static RAM。
- Orientation host 初次完整运行因 RTC stub globals 无定义而链接失败；已让 stubs
  共享 inline globals，4 个 orientation tests 与完整 suite 均恢复通过。

运行命令：

```sh
python3 -m unittest discover -s firmware/tests -p 'test_*.py' -v
python3 firmware/tests/test_ejection_simulation.py --strict-safety -k test_safety_ -v
backend/.venv/bin/python -m backend.tests
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
```

## 剩余边界

1. 两个 strict FAIL 仍是 A/B pulse 开始 10 ms 后 reset、输出中断且不补发。
   Anti-refire 行为保留；没有宣称该输出足以完成 deployment。
2. RTC counter 倒退、calibration 无效或时间不能表示时，明确 Serial warning 后
   保留旧的 restarted-backup fallback，因此这一 degraded case 仍可能延迟。
   Invalid latch、旧版本 latch、完全失电不能恢复 flight history。
3. RTC domain 是否跨实际 brownout/watchdog reset 保持、clock drift、启动耗时与
   power integrity 尚未实机验证。Boot 中 sensor/SD 初始化仍可延迟 flight service。
   Legacy boot/brownout counters 未重构，不是证明 latch 保留的依据。
4. Host timer 是按 deadline 执行 callback 的模型，不模拟 interrupt masking、
   cache/driver delays、真实 GPIO 时序或 current。ISR cutoff 不是独立于 MCU 的
   electrical one-shot，MCU reset/失电仍能中断 pulse。
5. 之前 rough-input experiments 的提前 ejection 问题未在这次调整；apogee
   thresholds 未修改。机械 deployment 与 flight readiness 也未被本次验证。

Runtime evidence 保存在 `flights/simulations/20260908-postlaunch-reset-pulse/`，
包含 full/strict logs、strict source hashes 与实际 host events。该目录为 git-ignored。
