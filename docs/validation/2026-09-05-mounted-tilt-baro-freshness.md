# 安装方向与 barometer freshness 修复 — 2026-09-05

代码修复已通过 host 验证及 A/B ESP32-S3 编译，未 flash、未重启 live backend、
未发送 serial 指令、未操作 firing circuit。不能据此宣称 flight-ready。

## 本次修复

- A/B `Flight.cpp`：按 `lastBaroUpdate` 消费新的 accepted sample；旧值不再重复
  修正 altitude filter、不再累计 apogee confirmation，也不能重复支持 baro launch
  fallback。保留原有 IMU freshness 修复。
- Altitude filter 的 dt 改为实际 accepted sample 间隔。缺少新样本时保持输出；
  health failure、超过已有 `BARO_STALE=1000 ms`、跨越该时长的采样中断或 reseed
  会清除下降确认，恢复时重新初始化 filter。使用现有门槛，没有新增参数。
- 高度、速度、确认次数、19 s backup、400 ms pulse、GPIO 与 reset policy 不变。
- `shared/protocol/mrcc.py`：receiver 宣布 Channel B 时，tilt 按已确认的 +Y nose
  安装方向计算。A 和无 channel 上下文的输入保留原有 +Z。用户尚未确认 A 的方向。
  Parser 逐行处理 channel marker，支持跨 chunk marker 和同一 chunk 内切换。
- 与 `backend/app.py` 共用 receiver marker pattern，避免两处对 channel 的解释不同。
  AX/AY/AZ 原值、firmware IMU axes、Kalman roll/pitch、heading 均未改动。

这里的 tilt 是静止时的 gravity-reference angle，不是 boost/free-fall 期间可靠的
attitude estimate。旧 CSV 不会重写；raw replay 会按当前 mounting mapping 重新计算，
所以安装方向不同的旧 B 日志不应直接沿用当前 +Y 的解释。单独 decode_line 默认仍为 +Z。

## Fail-first 与结果

修改前，原 F2 A/B probes 都观察到单笔异常 altitude 后停止更新仍产生 GPIO rise。
新增 held-filter、stale sample、interrupted descent、measurement cadence regressions
共观察到 10 个失败断言。测试脚本最初的 STEP chaining 错误已先修正再确认行为失败。
Cadence fixture 对齐 service 后，再修正不连续的合成 MSL 起点；最终 fixture 在临时
HEAD baseline 上 A/B 都失败，在修复代码上通过。补充 equal-valued fresh sample 测试
同样在临时 baseline 上复现提前触发，在修复代码上通过。

Tilt 测试在修复前复现 87° 而非 3°、B channel 未切换安装轴等失败；修复后通过。

| 检查 | 结果 |
|---|---|
| Simulation suite | 41 methods：33 pass，8 expected failures；102 次进程执行 |
| 完整 firmware suite | 55 methods：47 pass，8 expected failures |
| A/B host sanitizers | AddressSanitizer / UndefinedBehaviorSanitizer 无报告错误 |
| Backend tests | 48 pass |
| MRCC protocol self-test | pass |
| Dashboard | 12 tests pass，TypeScript / Vite build pass；保留既有 bundle size warning |
| Mounted tilt tests | 5 pass，覆盖 upright / side / inverted / 45° / zero vector / 实测样本 / chunk split / channel switch / wire 与 CSV |
| ESP32-S3 compile A | 441214 bytes flash，26064 bytes RAM |
| ESP32-S3 compile B | 441218 bytes flash，26064 bytes RAM |
| A/B Flight.cpp | byte-identical；结果 JSON 的所有 source hashes 与当前文件匹配 |

标准合成轨迹的 launch/fire 仍为 boot 后 20.260 / 32.910 s，pulse 400 ms。
10/20/50/100 ms loop 与 noise cases 全部在合成 apogee 后触发。
20 ms loop 情境中 accepted barometer 实际每 100 ms 更新，修复后 fire 从
33.040 s 变为 33.400 s（延后 360 ms）；其他已测 nominal case 的 fire 时刻不变。
这是去除重复数据和使用真实 dt 后的时序变化；所用 1.5 s regression envelope
不是最终 airframe 的 deployment timing acceptance requirement。

Source-hashed 事件记录：[2026-09-05-baro-freshness.json](2026-09-05-baro-freshness.json)。

## Live evidence 与未完成的硬件确认

读取的是现有 Ground Station backend 的 serial 原始日志，没有抢占串口。
`/gs` 返回 B；原始日志显示最后一次 channel switch 为 UTC 14:27:04.889 → B。
UTC 14:41:52、14:42:04、14:44:40、14:45:12、14:51:26、14:51:47、14:54:05
均出现 onboard clock 回跳，期间没有新的 receiver channel switch marker。
这支持发射端重新启动的现象，但不足以区分手动 reset、power loss、brownout、
watchdog 或其他原因。是否人为操作的问题尚未收到用户答复。

`initFlight()` 明确在 retained pre-launch state <= ARMED 时回 PAD；这次未改成
重启后强行 ARMED。真实 reset reason 已由 Health.cpp 输出到 flight computer
本机 serial 和 SD header，但当前无线 packet 不包含该信息。需要该证据才能继续
定因；此次不编造或修改供电修复。

离线重读既有 raw.log，UTC 14:44:00 的 B 样本由原 +Z 90° 得到 +Y 1°；
用户确认姿态时的代表样本 (-0.16, 9.63, 0.48) 得到 3°，原算法为 87°。
运行中的 backend 仍加载旧代码，因此 live UI 不会因本次文件修改自动改变。

尚存 F3-F6 各 A/B 两个 expected failures：真实静止观察时长不足、reset 延后
backup deadline、loop stall 延长 pulse、reset 截断 pulse。8 expected failures
不是 8 项通过。原报告继续保留，实际 GPIO、电流、供电和机械部署仍未验证。

## 复验

```sh
python3 firmware/tests/test_ejection_simulation.py -v --report docs/validation/2026-09-05-baro-freshness.json
python3 -m unittest discover -s firmware/tests
backend/.venv/bin/python -m backend.tests
backend/.venv/bin/python -m shared.protocol.mrcc
backend/.venv/bin/python -m unittest shared.protocol.test_mounted_tilt -v
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/MRCC_FlightComputer_B
```
