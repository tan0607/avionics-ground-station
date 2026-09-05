# Ejection logic 地面模拟验证 — 2026-09-05

> 此为修复前的基线报告。F1 的后续修复及当前结果见
> [IMU launch confirmation 修复记录](2026-09-05-imu-launch-freshness.md)。

结论：正常合成轨迹下，A、B 都能自动识别 launch、apogee 并输出一次
400 ms GPIO pulse。故障注入复现了两项优先处理的误触发问题，以及四项
计时或 reset 限制。**当前结果不支持宣称 ejection 已验证安全或 flight-ready。**

本次仅增加 host tests、模拟适配层和报告，没有修改 A/B flight firmware，
没有连接串口、flash 或操作真实 firing circuit。

## 测试范围与方法

- 分别编译 A、B 的真实 `State.cpp`、`Filters.cpp`、`Flight.cpp`、`Pyro.cpp`
  和各自的 `Config.h`。不复制或重新实现 flight state machine。
- 输入为合成的 raw IMU、已被 barometer driver 接受的 altitude、health flags
  和模拟时间。`filterUpdate()` 实际执行；barometric alpha-beta filter 也实际执行。
- 使用无硬件连接的 Arduino shim，记录 GPIO rising/falling edges、state transitions、
  latch、sample counts、sensor sample age。GPIO edge 是逻辑输出，不代表电流或部署成功。
- 每次 reset 启动全新的进程；从前一次执行读取公开 latch 值，并通过真实
  `latchWrite()` 重建有效 latch。其他 C++ globals/statics 恢复启动初值。
  这验证恢复逻辑，**不验证 ESP32 在不同 reset 原因下的 RTC retention**。
- 模拟 boot delay 为 1.5 s；不模拟后续 SD/GPS/IMU 初始化等待。
- 编译使用 `-Wall -Wextra -Werror`、AddressSanitizer、UndefinedBehaviorSanitizer。
  仅关闭 unused-variable warning，因为当前关闭的 continuity 功能留下未使用变量。
- 文件 SHA-256 和 74 次进程执行的事件证据保存在
  [机器可读结果](2026-09-05-ejection-simulation.json)。运行前后检查被测源码未发生变化。

## 已通过的逻辑检查

新增 suite 有 30 个 test methods：18 个通过，12 个 expected failures，分别对应
下文六类未满足预期在 A、B 的复现。多个通过的 method 包含 A/B 和不同输入的 subtests；
74 次执行不是 74 种独立故障，也不能换算成可靠率。

| 情境 | 实际观察 |
|---|---|
| Cold boot | PAD、pyro 未 armed、GPIO LOW |
| 自动 arming | 完成 gyro calibration，并满足当前 boot/stillness 计时条件后 ARMED；真实观察时长的限制见 F3 |
| 两小时静置 | 保持 ARMED，无 launch，无输出；这是 2 h 的模拟时间 |
| 两个 sensor 都不可用／gyro 持续运动 | 不能 auto-arm |
| 主动 disarm 后继续等待 | 不会自动重新 arm |
| IMU 静止，仅 altitude 大幅变化 | 不触发 launch |
| 单个 raw acceleration spike 后恢复正常采样 | 实际 median/low-pass chain 阻止假 launch |
| 正常合成上升、apogee、下降 | ARMED → BOOST → COAST → APOGEE → DESCENT，仅一个 pulse |
| 不同 cadence、小幅合成 altitude noise | 所测 10/20/50/100 ms loop 间隔及 0.2 m 正弦 noise 下均在合成 apogee 后触发 |
| Barometer 启动即失效、途中失效、持续输出固定高度 | 通过 TIMER BACKUP 触发 |
| 已 launch 但最大高度不足 30 m | barometric apogee 被阻止，timer backup 仍可触发 |
| IMU 不可用、barometer 正常 | barometric fallback 可识别 launch，再自动触发 apogee |
| 已标记 reseed 的 +4000 m altitude offset | 不立即误判 apogee；这不验证 driver 的 spike rejection |
| 未 armed 的 fire 请求／已 fired 的第二次请求 | 拒绝输出／不再输出第二次 |
| 完整 pulse 后保留 latch 重启 | 恢复 DESCENT，无第二次 pulse |
| COAST 时保留 latch 重启、barometer 恢复下降数据 | 恢复 COAST 和 armed，再由 barometer 触发 |
| Reset 后过早出现下降数据 | 当前 1.5 s launch-time guard 生效 |
| 始终没有识别 launch | 19 s backup 根本不开始，不能补救漏检 launch |

正常轨迹是假设净上升加速度 50 m/s²、boost 2 s、之后只受 gravity 的合成轨迹，
并非最终火箭的 OpenRocket 导出数据。模拟 liftoff 在 boot 后 20 s，轨迹 apogee
在 liftoff 后 12.197 s。标准 10 ms loop、10 ms IMU、50 ms accepted barometer
输入下，检测 launch 在 boot 后 20.260 s，GPIO 在 32.910 s 拉高：比合成 apogee
晚约 0.713 s，pulse 为 400 ms。

不同 cadence 的观察到的 apogee 后延迟约 0.703–1.203 s。测试使用的 1.5 s
上界只是探索性 regression envelope，**不是已获认可的 deployment timing 要求**。
20 ms loop 情境的 barometer 更新实际为 100 ms，因为输入按 50 ms 边界采样；
所有计时以事件时间为准。传感器内部滤波和机体 pressure-port lag 未计入。

标准无 reset 的 backup 输出在 detected launch 后 19.050 s，包含下一次
`FS_APOGEE` service 的延迟，不是精确硬件 19.000 s timer。

## 未满足预期：A/B 均已复现

这些 probes 最初作为普通断言运行，实际得到 12 个失败后才标记为
`expectedFailure`。标记用于保留可运行的 regression suite，**不代表问题已解决**。
下列 F4–F6 也包含架构限制／既有设计取舍，不能全部视为同一种软件 bug。

| ID | 注入情境与结果 | 含义 |
|---|---|---|
| F1 — 优先 | ARMED 后只有 3 个新 6g raw IMU samples，随后停止更新。最后一笔在 20.030 s；20.260 s 进入 BOOST，当时 sample age 230 ms。之后注入 sensor-down 状态，39.310 s timer 发出输出。 | `launchSamples` 累计的是 flight-loop 检查次数，旧 filtered acceleration 也会被重复计算；短暂数据异常加停止更新可导致假 launch。 |
| F2 — 优先 | 上升期间 28.050 s 接受一笔比上一高度低 20 m 的值，然后暂停新 barometer samples。28.400 s 发出 APOGEE 输出；该值 age 350 ms，真实合成 apogee 应为 32.197 s。 | 重复用旧 altitude 更新 filter／累计下降确认，可比合成 apogee 提前约 3.797 s 输出。 |
| F3 | 第一笔 still IMU 在 1.510 s，10.010 s auto-arm，只实际观察了约 8.5 s。 | `stillSince` 初值为 0，会把首笔样本之前的 boot 时间算进 10 s stillness window。此处未出现 firing，但 arming 的观察时长不足。 |
| F4 | 在原始 launch 后约 9.740 s reset，保留 COAST latch，barometer 不可用。重启后到 20.560 s 才输出，合计原始 detected launch 后 30.300 s。 | backup 从 reboot 后重新计时，不保留原始 19 s deadline；真实 setup 等待还可能增加延迟。 |
| F5 | 自动 GPIO HIGH 后，让 loop 900 ms 不被调度，再恢复 service。 | HIGH 持续 900 ms，超出配置的 400 ms；`servicePyro()` 在 loop 顶部也不能在卡顿期间执行。未证明实际硬件存在这样的 stall，只验证其后果。 |
| F6 | 自动 GPIO HIGH 10 ms 后，模拟保留 latch 的 reset。 | latch 已标记 fired；重启保持安全状态，不恢复剩余 pulse。不能从 latch 判断负载是否已经成功动作，也不能保证完成原定 400 ms。 |

F1 的假 launch 发生在当前 `IMU_STALE=2000 ms` 之前。F2 的早触发发生在
`BARO_STALE=1000 ms` 之前，单靠现有 stale timeout 无法覆盖这两个输入情境。
F2 输入是在 driver 接受 altitude 的边界注入；20 m / 50 ms = 400 m/s，低于
当前 800 m/s rate threshold，且该高度对应压力在现有 absolute bounds 内。
因此它并非一定会被 rate gate 拦下的巨大 step；但本次没有执行实际 I2C driver。

F6 是防重复触发与中断输出之间的 assurance gap。测试中的 10 ms 是选定的逻辑
截断时间，不代表实际点火需要多久，也不是建议取消 latch 或 reset 后盲目重发。
物理 gate 在 reset 瞬间的波形、no-fire/all-fire 特性和真实供电保持能力必须实测。

对应的当前源码位置：

- `Flight.cpp`：`stillSince` 初始化、`FS_ARMED` 的 `launchSamples`、
  `updateAltitude()`、`FS_COAST` 的 `apogeeSamples`、`initFlight()` 的 `launchTime` 恢复。
- `Pyro.cpp`：`firePyro()` 在拉高 gate 前写 latch；`servicePyro()` 依赖 loop 调度结束 pulse。
- `Health.cpp`：sensor stale thresholds 的检查。此模块只做源码核对，未链接到模拟器。

## 如何重新运行

从 repository root 执行：

```sh
python3 firmware/tests/test_ejection_simulation.py -v --report docs/validation/2026-09-05-ejection-simulation.json
python3 -m unittest discover -s firmware/tests -v
```

本次全 firmware suite：44 个 tests，32 通过、12 expected failures，无其他失败或
sanitizer 错误。看到 `OK (expected failures=12)` 不能解释为 ejection safety gate 通过。

要让所有未满足预期作为实际失败返回 nonzero exit code：

```sh
python3 firmware/tests/test_ejection_simulation.py --strict-safety -k test_safety_ -v
```

当前预期：12 个 failures，exit code 1。未来修正后的 unexpected success 也会让默认
suite 非零退出，提醒移除对应标记、review 预期并更新报告。

## 尚未验证与下一步

本次不验证 BMP280/I2C 驱动、actual sensor failure detection/recovery、真实 main-loop
最坏执行时间、ESP32 timer rollover、真实 RTC checksum/retention 故障、GPIO 电气波形、
电流、电池压降、MOSFET、线束、机械释放、parachute inflation 或最终飞行 profile。
Host 的 `unsigned long` 宽度与 ESP32 不同，本 suite 不声称覆盖 rollover。

软件下一步应优先 review F1/F2 的 sample freshness 和 confirmation 计数方式，并在
保留这些失败输入的前提下验证改动。F3–F6 分别需要计时／reset 策略与硬件证据审查。
这次授权范围是建立与执行模拟，因此尚未修改这些 production behaviors。
完成软件处理后，仍需实际 A/B boards 的 dummy-load bench test 和完整 ground deployment。
