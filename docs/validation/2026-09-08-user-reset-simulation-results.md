# A/B 用户指定 reset 情境：本轮 simulation 结果

2026-09-08。本轮只运行 host software simulation，未修改 production firmware、
未 flash、未操作 serial/GPIO 或真实负载。

## 使用的代码与证据

实际编译两块 `MRCC_FlightComputer_A/B/src` 的 `State.cpp`、`Filters.cpp`、
`Flight.cpp`、`Pyro.cpp`，包含当前 prelaunch session recovery、postlaunch RTC
deadline 和 GPTimer cutoff。Python 仅生成输入、调用 C++ 和检查结果，没有重写 FSM。
Host 编译启用 ASan/UBSan。

输入为合成 IMU 与已接受的 altitude。没有运行 Baro.cpp pressure/rate gate、
Sensors/Health drivers、实际 .ino setup/loop、Radio/Storage、硬件电气环境。
pressure 在 harness 中是固定参考值，不是 pressure simulation。

证据目录：`flights/simulations/20260908-123512-user-reset-matrix/`（git-ignored）。
保存 63 个源码文件的 SHA256 和完整 source-snapshot、working-tree patch、执行脚本、
full-suite.log/full-result.json、autoarm-inputs/events.json、strict-ejection.log/json、
user-cases 的逐次 input/jsonl/stderr 及 summary。测试前后 source hashes 无变化。
A/B State、Flight、Filters、Pyro、Console shared-source parity 通过。

## 用户情境与本轮结果

A 电池 14.8 V，B 电池 7.4 V，用户说明接法相同；本模型不模拟电池/降压器电压。
整机电源 switch 已确认。A/B 分别在首次 boot 100 s（PAD）和 190 s（ARMED）
生成基线，再注入 reset 或 power-on/lost-retention。共 18 次独立 C++ 执行，含
4 次基线、14 次故障/恢复情境；断言全部通过，所有这些 runs 无 GPIO HIGH。

| 用户情境 | 软件模型 | A/B 结果 |
|---|---|---|
| P1 主动关整机 switch 再上电 | POWERON，即使提供旧 retained bytes 也应清除 | 新 boot 179.990 s 仍 PAD；180.010 s ARMED；无输出 |
| P2 通电按 reset，原先只等了 100 s | 有效 retained warm reset，新 process 恢复实际 host RTC bytes | 原 session 180.010 s ARMED（新 boot 80.010 s），不重等完整180 s |
| P2 通电按 reset，原先190 s已ARMED | 同上，重新采集静止窗口/calibration | 启动回 PAD/unarmed；新 boot 11.510 s ARMED，无输出 |
| P3 线松造成断电/retention丢失 | 明确清空 retained bytes，即使非POWERON也不能继承进度 | 新 boot 180.010 s ARMED，无输出；真实掉电过程未模拟 |
| P4 红灯亮、绿灯灭、sensor疑似reset | 单独隔离“sensor失联但MCU不reset”分支：170–185 s令IMU/baro不可用，再恢复 | 185 s仍PAD；195.010 s ARMED，无输出 |

P4 若实际是 MCU warm reset，则参考 P2；若实际丢失 RTC，则参考 P3。
LED 现象本身没有被复现，不能据此判定 brownout/watchdog/EN/供电故障原因。
所有时间均为 host 模型结果：startup 固定1500 ms，10 ms输入步长；不是实机时间保证。

## 完整 regression 与 strict ejection

- 完整 firmware discovery：92 test methods，90 PASS、2 expected failures、0 errors。
- 全部 ejection tests 以 strict 模式重跑：50 methods，48 PASS、2 FAIL、0 errors，exit 1。
- 两组包含重叠测试，不能相加当作独立覆盖数量；每个 method 可含多个 A/B subcases。
- 两个 FAIL 均为 `test_safety_interrupted_pulse_not_lost_A/B`。模拟输出开始10 ms后
  reset，fire-attempt latch 保留，重启不补发，因此总输出不足400 ms。保留该失败，
  不把防重复触发解释成 deployment 已完成。
- 正常合成飞行、barometer丢失/冻结后的backup、重复reset的原始backup deadline、
  loop stall的400 ms timer cutoff、完整pulse后不重复fire等现有回归通过。
- 本轮平滑nominal A 的输出为 boot 212.910–213.310 s，400 ms；B对应测试同样通过。
- 人为disarm retained recovery、无效时钟/损坏record冷启动fallback、freshness与
  prelaunch重建窗口均在完整suite中运行。

独立 strict 命令（未加 `-k`，因此是全部50项，不是仅12项safety子集）：

```sh
python3 firmware/tests/test_ejection_simulation.py --strict-safety --report flights/simulations/20260908-123512-user-reset-matrix/strict-ejection.json -v
```

完整suite通过同目录 `run_validation.py` 调用 unittest discovery，保存auto-arm输入/
events；用户情境由 `run_user_cases.py` 调用同一正式C++模块，不更改原测试断言。

## 尚未被证明

- Prelaunch reset恢复到ready前仍不能把起飞输入当作已armed任务：对应regression
  通过意味着保持PAD，而不是保证这段空档可检测launch。
- 完全掉电会开始新的180 s等待；不能保证恢复旧flight history。
- Interrupted pulse仍未解决；GPIO-only恢复策略不自动再次fire。
- 人工rough-input实验的提前ejection敏感性没有被本轮消除；本轮未重跑该71输入
  stress矩阵，也没有调threshold。实际sensor logs尚未提供。
- 真实pressure path、RTC retention、供电/EN波形、负载端pulse、电流、radio、SD
  recording、机械deployment均不是本轮host结果。Multimeter不足以验证400 ms
  pulse或瞬时供电波形。
- 本轮未重新做Arduino目标编译，也未flash；先前compile报告只作为历史证据。

下一阶段：接收正常logs并检查sensor噪声；准备实际板上的输入注入方案及dummy-load
测量。参见 [用户情境清单](2026-09-08-pending-reset-simulation-matrix.md)。
