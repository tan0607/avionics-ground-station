# 手动上下移动测试 — HAND TEST ONLY

这是独立的 ESP32-S3 测试 firmware：使用真实 IMU / BMP280、原版 Filters.cpp / Flight.cpp，尝试用手举高再放低触发 flight state。**pyro GPIO 在此测试版只会被写为 LOW，firing 是模拟状态。禁止用于飞行。**

原有 `firmware/MRCC_FlightComputer_A` 和 `B` 没有被本次工作修改。这里复制的是创建时的实际工作目录，包括当时尚未 commit 的 reset recovery 改动；SHA256 在 `source-snapshot.json`。复制版不会自动跟随原版更新。

## 选择正确的 sketch

| 板子 | 打开的 Arduino sketch | LoRa SCK | SD CS | Radio |
|---|---|---:|---:|---|
| A | `MRCC_HandMotion_A/MRCC_HandMotion_A.ino` | 12 | 7 | 433.3 MHz |
| B | `MRCC_HandMotion_B/MRCC_HandMotion_B.ino` | 47 | 6 | 434.1 MHz |

必须先确认是哪一块板子，以及点火器／火药已与 pyro 输出完全断开，再 upload 或重启板子。软件 LOW 无法控制 firmware 启动前的悬空 pin，也不能证明实际 MOSFET 接线正确。此版本不使用 pyro pin 点亮 LED，直接从 Serial Monitor 查看 `WOULD_FIRE`。

## 实测步骤

1. 打开对应 sketch，Board 选择实际 ESP32-S3 型号／已有正确设置，确认 USB port 后 upload。使用原先适合该板子的 USB CDC 设置。
2. 打开 **115200 baud** Serial Monitor。确认 boot banner 包含 `MRCC HAND TEST`、`PROFILE=HAND` 和 `PYRO GPIO PERMANENTLY LOW`。若看不到这些字，不要按此说明操作。
3. 检查 `IM=1`、`BA=1`，sample age 持续刷新。在低处握稳板子，保持静止，等待 gyro calibration 完成和 `[FLIGHT] -> ARMED`。默认至少经过上电 15 s，同时需要最近连续 10 s 的有效静止 IMU 数据；初始化延迟、手抖或 sensor 问题可使等待更久。`S` 查看 arming blockers。
4. 保持板子方向大致不变（安装方向 +Y 朝上），握牢，做一次短而明确的向上加速，然后在舒服的范围内举高约 1 m。不要抛板、猛烈甩动或拉扯 USB／接线。观察 `g` 是否超过 1.25、状态是否进入 `BOOST`，随后进入 `COAST`。缓慢匀速举起可能仍只有约 1 g，不触发 launch 是合理结果。
5. 在高处稍作停留，让从 `BOOST` 起至少经过 1.5 s，再向下移动约 1 m。观察 `MAX >= 0.5`、`VZ < -0.3`。BMP280 的噪声、气流和 filter 延迟可能让这一步失败或产生误报；不要为了强行触发而甩板。
6. 出现 `WOULD_FIRE=1 REASON=APOGEE`，表示测试 threshold 下的气压下降判据通过。出现 `REASON=TIMER BACKUP`，表示 launch 后约 19 s 的备用计时路径触发，**并未证明它检测到了你的向下动作**。`GPIO=LOW(commanded)` 是软件命令，不是电压测量。
7. 放稳后，状态通常会继续到 `LANDED`。`X` 可 disarm，并阻止该 power session 自动重新 arm。要重新跑一轮，在点火输出已物理断开的前提下完全断电再上电（USB 和外部供电都要断开）；普通 warm reset 会保留 flight latch。

## 两个 threshold profile

每个 sketch 的 `src/Config.h` 顶部有 `HAND_TEST_ORIGINAL_THRESHOLDS`：默认 `0` 为手动测试；设为 `1` 后重编译可使用原版 threshold。**两个 profile 都永久禁止实际 pyro HIGH，没有 console command 可以启用。**

| 条件 | 默认 HAND (`0`) | ORIGINAL_THRESHOLDS (`1`) |
|---|---:|---:|
| 上电等待 | 15 s | 180 s |
| 连续静止与 gyro calibration | 10 s + calibration | 相同 |
| IMU launch magnitude | >1.25 g，2 个 fresh flight samples | >3 g，5 个 fresh flight samples |
| 气压 apogee 最低最高点 `MAX` | 0.5 m | 30 m |
| 下降速度 | <-0.3 m/s | <-2 m/s |
| 气压确认 | 4 个 fresh barometer samples | 相同 |
| 最早 apogee 判断 | launch 后 1.5 s | 相同 |
| Backup | confirmed launch 后约 19 s | 相同 |

Barometric launch fallback 保留原版 >15 m / >5 m/s 等条件；HAND 依靠 IMU launch。其余滤波、sensor driver、freshness、state transition、RTC recovery 逻辑均保留。`T` 然后 `Y` 也只是模拟 pulse，不会提高 GPIO。继承的 SD／radio commands 仍存在；手动测试只需要 `S` 和 `X`。

## 查看与保存结果

- USB 每约 250 ms 输出 `[HAND TEST]`，包含 state、g、ALT、MAX、VZ、sensor health/age、WOULD_FIRE、REASON。实际输出节奏受 loop 与 USB 状态影响。
- Radio 保留 A/B channel，在每个 MRCC packet 开头加 `HT=1`。连接原来的 ground station 并选择匹配 channel，可尝试查看原有 dashboard；**dashboard 尚未增加 HAND TEST 警示**，其中 AR/FI 表示模拟状态。以 USB 的 reason 为判断依据，并把此次 recording 命名为 HAND TEST。RF 实际收发尚未验证。
- SD 测试日志使用 `/HAND001.CSV` 等独立名称，原有 `/FLIGHTxxx.CSV` 保留。CSV 的 `ARM/FIR` 也是模拟状态。

## Build 与 verification

在 repository root 执行：

```bash
python3 -m unittest discover -s firmware/HandMotionTest/tests -p 'test_*.py' -v
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/HandMotionTest/MRCC_HandMotion_A
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/HandMotionTest/MRCC_HandMotion_B
```

测试复用 repository 的 `firmware/tests/ejection_host` 和 packet harness；Arduino sketches 本身各自完整。无需更改正式 firmware 或 dashboard。

这些 host tests 输入的是 synthetic IMU / accepted barometer altitude 数据；没有模拟真实 I2C driver、气压噪声或完整手部运动学。通过只证明对应输入下的测试逻辑／packet formatting；build 不代表已经 flash，更不代表传感器、无线电、电气输出或真实飞行通过验证。

**测试版成功，只说明放宽 threshold 后的实物流程可用。不能据此宣称原版 launch / apogee 条件已验证。** ORIGINAL_THRESHOLDS profile 保留原判据，但普通手举通常不会触发它。
