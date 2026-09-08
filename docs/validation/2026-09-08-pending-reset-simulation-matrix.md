# 待执行：用户指定的四种 reset / 电源情境

记录日期：2026-09-08。状态：待确认细节、待执行；本文件不是测试通过报告。

## 用户要求与尚未确认的信息

用户准备更新板上 flight firmware，然后运行以下四种 simulation / bench 情境。
已确认两块 A/B 都测；switch 切断整个 flight computer 的电源，并非软件 disarm。
A 电池 14.8 V，B 电池 7.4 V；用户说明其余接法相同。电池正极经 switch 分到
MP1584EN 和 ejection module，负极共同回电池；module 的 PWM 标记输入接 GPIO2，
GND 共地。降压器实际输出、module 型号与电气时序仍待核对。
用户目前只有 multimeter，板上为旧 firmware（用户认为尚未包含 180 s arming）。
正常 logs 稍后提供；不要求提供尚未发生过的真实飞行或 ejection 记录。

| ID | 用户情境 | 要观察的结果 | 证据边界 |
|---|---|---|---|
| P1 | PAD / ARMED 阶段主动关闭整机电源 switch，之后恢复 | 记录关断前状态；确认实际是否完全断电；恢复后的 PAD、arming 等待、sensor calibration、静止检查与输出状态 | 实际 POWERON 不应当作 retained warm reset |
| P2 | 已通电后按 reset | 分别在 PAD 等待中与已经 ARMED 时记录；检查等待进度、重新进行的 sensor/静止条件，以及有无意外输出 | 按钮 reset 与软件 reset 的实际 reset reason、RTC retention 不可直接互换 |
| P3 | 线松造成断电，随后恢复 | 记录受影响供电支路、失电持续时间、reset reason、恢复状态、日志连续性和输出 | 区分 MCU 支路掉电和整个系统掉电；完全失电不能由 firmware 保证保留历史或继续输出 |
| P4 | 线松后红灯仍亮、绿灯灭，并疑似 reset | 保存用户观察；以 boot banner、uptime、reset reason、telemetry、供电/EN 波形判断是否真 reset，再检查相应恢复行为 | LED 现象不能证明 MCU rail 连续稳定，也不能直接归因为 brownout/watchdog；原因未知 |

## 执行顺序

1. 确认 A/B、switch 的作用、实际接线；真实 igniter/charge 断开，仅使用非爆炸性 dummy load。
2. 固定待测源码快照，记录 source hashes、board、SDK、build/upload 信息。当前
   working tree 存在未提交修改与新增 prelaunch-recovery 工作，旧报告不自动适用于新版本。
3. 先完成该快照的 host regression 与 A/B compile，再核对实际刷入版本。
   Flash 正常 flight firmware 不会自行产生模拟升空；板上输入注入入口需另行准备和验证。
4. Host simulation 先覆盖冷启动、retained warm reset、无效/丢失 retained state。
   P3/P4 的真实电气原因不能由一个 BOOT/reset stub 证明。
5. 板上先做 PAD/ARMED 情境，再用确认过的模拟飞行输入检查 postlaunch reset、
   原始 backup deadline、pulse 中途 reset 与完整 pulse 后 reset；保留 anti-refire 检查。
6. 每次保存 onboard SD CSV、ground raw.log/telemetry.csv、boot/reset 输出及可用波形。
   记录 state、arming 时间、实际输出起止/次数、telemetry 恢复、SD 是否继续写入。
   GPIO 波形与负载端输出分别记载；无测量时标记未验证。

实机故障注入需先确定可控方式，不通过松动带电接头随意制造故障。

## 需保留的判定规则与未解项

- Prelaunch 恢复策略以最终审核和测试的版本为准；新增计划提出保留 valid warm
  reset 前原 180 s deadline，同时重新满足本次 boot 的 sensor/calibration/静止条件。
  这不是直接恢复 ARMED，也不能把计划写成已经实现或通过。
- 保留人为 disarm 与 fire-attempt latch 的测试；恢复等待进度不得绕过它们。
- Valid postlaunch retained clock 应保持原 backup deadline，单独检查 startup downtime。
- Loop stall 下的 timer cutoff 与 MCU reset 下的 pulse interruption 是不同测试。
- Interrupted pulse 当前仍是未解项，不以重启自动再 fire 或放宽断言掩盖；
  FI 仅表示 fire attempt 已锁存，不表示机械 deployment 成功。
- 用户稍后提供正常 sensor logs，用于检查地面 noise / drift / gaps，不能当作
  真实飞行环境的证明。模拟飞行用于验证逻辑与输出，不证明飞行气流/振动下的 sensor 行为。

## 关联记录

- [Reset / pulse 已有软件验证](2026-09-08-postlaunch-reset-pulse.md)
- [Prelaunch recovery 新工作计划](../superpowers/plans/2026-09-08-vibration-reset-recovery.md)

## 执行结果

实机 P1、P2、P3、P4：全部待执行。对应 host 软件模型已运行，见
[本轮结果](2026-09-08-user-reset-simulation-results.md)：18次情境/基线执行通过；
完整92项中90 pass、2 expected failures；strict ejection 50项中48 pass、2 fail。
不将 host reset/retention 注入当作真实电源故障复现。尚未 flash、操作串口或硬件输出。
