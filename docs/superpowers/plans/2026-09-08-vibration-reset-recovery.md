# Vibration 接线中断：prelaunch 等待恢复与 interrupted pulse

## 状态与目标

2026-09-08：用户已要求开始实施第 2 节 firmware 恢复逻辑与对应测试。
不 flash、不操作输出；接线修复和第 3 节独立输出硬件仍待实物确认。
目标：有效 warm reset 不重复整段 prelaunch 180 s 等待；明确解决输出被 reset
打断所需的硬件与验证工作，不能把软件测试通过等同于电气 deployment 成功。

用户提供的实际情况：7.4 V LiPo，MP1584EN 输出 5 V 给 ESP32；ejection module
正极从降压器 IN+ 的电池端分支，负极直接回电池负极。用户观察到 vibration
使接线偶尔松动并 reset；目前没有观察到 ejection 导致 reset。将其作为本次
问题背景，不再假定 firing current 是故障原因。具体松脱点、掉电时长与供电波形
尚未测量，ejection module 型号与内部功能尚未确认。

## 当前基线

- 本地 HEAD `cd37c60` 加已有未提交 postlaunch reset / GPTimer 修改。
- 本轮之前的实际重跑：strict ejection 50 methods，48 PASS / 2 FAIL；auto-arm
  8/8 PASS。证据：`flights/simulations/20260908-071721-reset-rerun/`（git-ignored）。
- `initFlight()` 对 PAD/ARMED latch 回 PAD，重新从本次 boot 等 180 s。
- flight latch 在 `.rtc_noinit`；POWERON 清除。checksum 验证记录完整性，不能证明
  RTC 电源连续，也不能证明多字段写入是原子的。
- pulse 前先保存 fire-attempt latch；重启保持 LOW 且不补发。两个 strict FAIL
  就是 A/B 在 pulse 开始 10 ms 后 reset，剩余输出没有恢复。
- GPTimer 已覆盖主 loop stall，不覆盖 MCU reset 或整个供电断开。

## 1. 先消除已经观察到的接线问题

1. 断电检查实际松动的连接点；记录它位于电池公共线、降压器输入/输出、ESP32
   电源/地线，还是 signal/EN。不把所有 reset 都解释成同一种掉电。
2. 根据实物采用可靠的接线固定、锁紧连接与 strain relief；固定模块，避免线束
   拉力直接作用于焊点/插针。具体连接件待实物确认，不先指定型号或电容值。
3. 后续 bench 仅用非爆炸性 dummy load，在测量设备下验证装配振动时的
   ESP32 电源、EN、ejection module 供电和输出；不靠松动真实带载接头制造故障。
4. 将“MCU reset 但输出模块仍有电”和“共同电源中断”分开记录。后者在无独立
   能量来源时不可能靠 firmware 继续输出。

这是故障原因的修复；下面的恢复逻辑是对剩余异常的容错，不能替代可靠接线。

## 2. Prelaunch：保存同次供电 session 的原始等待期限

建议规则：保留最初 180 s 等待的进度，而不是每次 boot 新建一个 180 s deadline。
这也覆盖尚未等满就 reset 的情况；不只是保留“已经等完”的标记。

| 情况 | 计划中的行为 |
|---|---|
| 第一次上电 | 从首次 boot 的时间基准等待 180 s，保留当前准备时间 |
| 等了 170 s 后 valid warm reset | 保留原 deadline；原 session 满 180 s 后，仍需满足本次 boot 的 sensor/静止/calibration 条件 |
| 已超过 180 s 后 valid warm reset | 不重等 180 s；回 PAD，重新检查本次 boot 的全部 arming 条件 |
| 连续 valid warm resets | 不移动原 deadline，不把反复重启误算成新的完整准备时间 |
| 人为 disarm 后 warm reset | 保留 disarm 阻止状态，不能因恢复等待进度而自动重新 armed |
| POWERON / 无有效 session / RTC 时钟无效 | 使用完整 180 s 冷启动流程，不能继承旧任务的 armed 状态 |
| 已确认升空 | 继续已有 postlaunch recovery，不重新执行 PAD 静止校准规则 |

实施细节：

- 在版本化 retained record 中增加 prelaunch session 时间基准、clock calibration
  与 disarm 标记；和 launch clock 分开，不能覆盖已有原始 launch deadline。
- 明确首次 boot uptime 与 RTC 时间的换算，使首次 arming 不早于现有 180 s；
  校验时钟倒退、零 calibration、溢出、旧版本、checksum 和 interrupted writes。
- 扩展每次 latch 更新路径以保留 session 信息；不能在进入 PAD/ARMED 时误清除。
- 每次 prelaunch reboot 都重新开始 10 s fresh IMU 静止窗口与 gyro calibration；
  不复用 reboot 前 held samples、旧 calibration 或旧 ARMED 来立即放行。
- 沿用现有 sensor/interlock/fire-attempt 检查与阈值；恢复等待进度不等于已经 armed。
- 日志说明“等待进度已恢复 / 冷启动重新等待 / disarm 保留”；现有 readiness 的
  remaining time 必须和实际 arming gate 一致。无需新增 dashboard 或协议字段。
- V1 不将 armed/session 写入 Flash/NVS 后跨任意断电自动恢复。没有独立 session
  依据时，接线断电和下一次人为重新上电不能可靠区分。

边界：如果实际松线使 RTC 丢失，这个 firmware 改进仍会要求重新等 180 s。
若还要求完全掉电后无等待恢复，则需要另行定义独立的 mission/arming 授权与
时间来源；不能仅凭旧 NVS 记录认为仍在同一任务中。此项不包含在 V1。

## 3. Pulse：把“开始过”和“已完成”分清，并覆盖 MCU reset

目标是：合法触发后，在输出模块供电保持的条件下，MCU reset 不截断规定输出，
同时输出必须独立按时截止、不得因 reboot/repeated command 重启或延长。

推荐实施路线是先核对现有 ejection module 能力，再选择有独立有界输出控制的
模块/硬件方案。先定义下面的接口要求，具体电路、器件与参数需模块资料和测量后
再确定，不把一段自动 re-fire firmware 作为问题已经解决的依据。

- 未合法触发时，MCU 上电、断电、reset、signal 浮空均不得启动输出。
- 已开始的输出，在 MCU reset 且模块供电保持时应按既定时限完成并截止。
- 重复触发、MCU reboot 和接口恢复不得刷新 deadline 或产生额外输出。
- 明确 hardware inhibit/disarm 的优先级与行为，保留人工停止能力。
- 整个输出模块供电被切断单独测试、单独报告；断电恢复不能自行触发。
- 保留现有 anti-refire 与 startup LOW。完成指示最多证明电气输出状态，不能
  单靠 GPIO、continuity 或 fired latch 宣称机械 deployment 已完成。

软件自动补发仅作为未选用的备选：它存在 reboot 空档、首次结果未知与反复触发
风险；当前不采用，也不将 fired latch 推迟到 pulse 结束才写入。
不依据 Config.h 中通用的点火时间注释，宣称当前实际负载在 10 ms 已成功动作。

若暂时只能改 firmware：可以完成第 2 节，但必须保留第 3 节未解决的标记；
不能声称两个问题都修好了。独立输出部分需获得模块型号/资料与实物验证。

## 4. TDD 与验证顺序

1. 为 prelaunch 原 deadline、已完成等待、连续 reset、disarm retention 先写
   failing tests；确认失败原因对应当前行为，再做最小实现，A/B 同步修改。
2. Host harness 支持明确的 retained reset、lost-record/power-on、invalid clock
   和 record 损坏场景；区分测试注入的假设和硬件实际 retention，不能硬编码所有
   reset 都保留 RTC。增加 reset 后立刻出现起飞输入的测试，报告未 ready 窗口
   的现有覆盖边界，不能通过合成 ARMED/launch 状态隐藏它。
3. 验证首次 boot 180 s 边界、静止窗口重建、校准中断重来、无 IMU 拒绝 arming、
   人为 disarm 与冷启动 reset；确认 readiness 显示和 gate 使用同一时间规则。
4. 回归 postlaunch 单次/重复 reset 的原 19 s deadline、baro recovery、正常
   single pulse、timer failure refusal 与 stalled-loop cutoff。
5. 保留两个 interrupted-pulse strict FAIL 作为当前硬件路径的未解项；只有实际
   硬件/接口改进后才能增加对应模型测试，不能修改断言或添加假想外部 timer
   让旧的 GPIO-only firmware 看起来通过。
6. 后续硬件用 dummy load + scope/logic analyzer，分别注入 MCU reset、MCU
   电源中断和模块电源中断；记录真实电源、输出起止与重复触发情况。先验证
   reset 前未触发保持关闭，再验证输出中断场景。操作范围不包含真实点火负载。

拟改文件：A/B `src/State.{h,cpp}`、`src/Flight.cpp`，必要的 Config/README 文案；
`firmware/tests/test_auto_arm.py`、`test_ejection_simulation.py`、`ejection_host/`。
Pyro/硬件接口的修改在模块能力确认后另行细化。`Health.cpp` 只在需要验证诊断时
调整相关文案：reset reason 和旧 boot counters 本身不能证明具体松线位置或完整失电。

验证命令（2026-09-08 firmware 部分已执行，结果见文末）：

```sh
python3 -m unittest firmware.tests.test_auto_arm -v
python3 firmware/tests/test_ejection_simulation.py --strict-safety --report /tmp/reset-recovery.json -v
python3 -m unittest discover -s firmware/tests -p 'test_*.py' -v
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
git diff --check
```

同时核对 A/B shared-source parity，保存 source hashes、命令、exit codes 与
host events。保持已有未提交修改与日志；不将 compile/host pass 称为 flight ready。

## 官方参考与本计划的推断边界

- [Espressif Memory Types](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-guides/memory-types.html)：
  RTC_NOINIT 的存储/初始化语义。实际目标 SDK 和断电 retention 仍须分别核对。
- [Espressif ESP32-S3 Schematic Checklist](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)：
  电源与 CHIP_PU/EN 的启动/reset 条件。本文的“松线可能造成不同 reset/掉电类型”
  是结合用户接线情况的工程判断，并非已经测得的电压波形或根因定位报告。

## Firmware 实施完成（2026-09-08）

- 第 2 节与对应 host 测试已实现：原 session deadline、重新静止/calibration、
  warm-reset disarm retention、无效时钟/record 的冷启动等待；保留 fire-attempt。
- Test-first：旧代码 19 methods / 24 失败 subcases；实现后 auto-arm 19/19 PASS。
- 完整 firmware 92 methods：90 PASS、2 expected failures；strict safety 10 PASS、
  2 FAIL，均为未解决的 interrupted pulse。A/B ESP32-S3 compile 与 parity 通过。
- 第 1 节接线修复与第 3 节独立输出硬件仍待实物工作；未 flash、未操作真实输出。
- 证据与实测边界：[validation](../../validation/2026-09-08-prelaunch-reset-recovery.md)。
