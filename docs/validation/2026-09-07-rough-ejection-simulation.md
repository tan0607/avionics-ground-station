# Ejection simulation 总结与粗糙输入实验 — 2026-09-07

## 结论

用户指出之前轨迹太平滑，本次补跑 71 条不同输入历史 × A/B = 142 次 host
执行。真实 A/B State/Filters/Flight/Pyro C++ 在强组合 noise 和部分 spikes
输入下提前输出 GPIO HIGH。不能把之前平滑轨迹通过理解为不规则数据下也通过。
本次没有修改 production firmware、thresholds，没有 flash 或操作串口/真实 pyro。

## 之前实际做过的模拟

| 类别 | 情境与结果 |
|---|---|
| 启动、arming、pad | Cold boot GPIO LOW；自动 arming；2 h 模拟静置不 launch/fire；无 sensor、gyro 持续运动不能 arm；主动 disarm 后不自动重 arm。最新版本要求 boot >=180 s、最近连续10 s stable IMU、gyro calibration 完成；IMU-down 不再允许 baro-only prelaunch arming。 |
| Launch 抗干扰 | 静止 IMU 加 altitude 变化不 launch；单个 raw acceleration spike 被 filter 阻止；3 笔高加速度后停更曾复现假 launch，freshness 修复后通过；相同数值的新样本、低值打断、丢失/过期 IMU、barometric corroboration 都有 regression。 |
| 正常 ejection | 合成 boost 2 s、净加速度50 m/s²、随后 ballistic；apogee T+12.197 s，约610 m；A/B GPIO T+12.910 s，即晚0.713 s；nominal pulse 400 ms。 |
| Cadence/小 noise | 10/20/50/100 ms loop 和仅0.2 m正弦 noise；所测输入在apogee后触发。它们并未覆盖广泛随机、相关噪声。 |
| Barometer 异常 | 启动缺失、飞行中丢失、冻结高度走timer backup；最大高度<30 m阻止barometric apogee，但不阻止backup；IMU在已armed后失效，barometer可fallback识别launch。 |
| Barometer freshness | 单笔下降20 m后停更曾提前fire，修复后通过；hold filter、新旧样本、实际dt、stale/gap/recovery、相同值的新样本和reseed +4000 m均有检查。+4000 m测试是已标记reseed的输入，未验证Baro.cpp拒绝spike。 |
| Output guards | 未armed fire被拒绝；第二次fire被拒绝；完整pulse后保留latch重启不重复fire。 |
| Reset/backup | COAST reset后恢复live baro可fire；reset后1.5 s guard；没有检测到launch则19 s backup不会开始。另有下列3项未解决限制。 |
| Replay | 9月6日及9月7日各有A/B nominal、baro-loss、pad-only；9月7日追加A/B arming preview。Preview裁剪同一C++输出，不是独立飞行模型。验证过backend/browser状态和FI/PG，不能把UI播放算作新的独立flight测试。 |

历史证据：
[初始基线](2026-09-05-ejection-simulation.md)、
[IMU freshness](2026-09-05-imu-launch-freshness.md)、
[barometer freshness](2026-09-05-mounted-tilt-baro-freshness.md)、
[9月6日 replay](2026-09-06-firmware-simulation-replay.md)、
[当前180 s arming与replay](2026-09-07-auto-arm-countdown.md)。
旧报告的10 s arming和8/10/12 expected failures均属于旧源码阶段。

## 本次 current suite 复跑

- `python3 -m unittest discover -s firmware/tests -v`：73 methods，67 pass，6 expected failures，exit 0。
- `python3 firmware/tests/test_ejection_simulation.py --strict-safety -k test_safety_ -v --report flights/simulations/20260907-strict-safety.json`：12 methods，6 pass，6 FAIL，exit 1。
- 6 FAIL对应3类问题各A/B复现：reset使backup输出落在原始launch后30.300 s；900 ms loop stall使400 ms pulse持续900 ms；pulse开始10 ms后reset不恢复剩余输出。
- IMU/Baro freshness和完整observed stillness probes现在通过。

## 新输入的具体定义

生成器：[rough_ejection_experiment.py](rough_ejection_experiment.py)。
每组seed 0–9；smooth仅1条对照。A/B每个seed收到完全相同输入，不能当作两个
独立随机样本，也不能将提前数转换成真实飞行故障率。

保留原ballistic truth；boot 200 s liftoff；boot 185 s起加入扰动，startup保持
原稳定输入。粗糙组loop间隔随机5/10/15/20/35 ms；barometer下一采样间隔随机
35/50/65/80 ms并在下一loop提供。IMU轴向Gaussian noise：pad sigma .02g、boost
.35g、coast .08g；gyro输入仍为0，未模拟真实3轴旋转振动。

“1/3/10 m级”各自是两项叠加：white Gaussian sigma=N m与AR(1) rho=.92、
stationary sigma=N m的相关noise。不是总noise限制±N m；测得总体RMS分别约
1.30–1.53 / 3.90–4.60 / 13.00–15.33 m。相关项从0启动。
这些是人为stress级别，未用实际BMP280或机体pressure-port日志标定。

| 输入 | A/B每组提前的seed数 | GPIO相对truth apogee时间范围 |
|---|---:|---:|
| Smooth control | 0/1 | +0.713 s |
| 1 m级组合noise + jitter | 0/10 | +0.693 至 +1.183 s |
| 3 m级组合noise + jitter | 0/10 | +0.573 至 +1.823 s |
| 10 m级组合noise + jitter | 8/10 | **−3.142 至 +1.278 s** |
| 1 m级noise + 2% accepted samples加±20 m spikes | 2/10 | **−0.882 至 +1.198 s** |
| 1 m级noise + 20%漏样 + T+11.5–13 s停更 | 0/10 | +1.253 至 +1.793 s |
| 1 m级noise + altitude延迟600 ms | 0/10 | +1.058 至 +1.823 s |
| Noisy pad + boot190/195/205 s短促6g impulses | 0/10假launch | 无GPIO输出 |

所有122次有飞行输入的run各一次launch、一次rise和一次fall，触发reason为APOGEE；
未出现漏fire或重复fire。20次pad run无launch/fire。粗糙组pulse为400–430 ms，
体现结束pulse需要等下一loop；nominal仍400 ms。A/B对应seed的summary完全一致。
这些计数仅描述本次样本；没有预先获认可的最大部署延迟要求，不能把正延迟全称为合格。

### 提前3.142 s的具体例子

noise-10m、seed 2，A/B均在T+9.055 s输出，truth apogee为T+12.197 s。
此时truth高度约561.45 m、仍以约30.81 m/s上升，但firmware VZ约−14.90 m/s。
T+8.8–9.0 s附近一段连续偏低的accepted altitude使filter把上升解读成下降。
这与“旧sample重复计数”不同：持续的新异常样本仍能形成错误下降证据。

## 证据与边界

结果目录：`flights/simulations/20260907-rough-inputs/`（git-ignored runtime data）。
含71个精确input command文件、142个A/B run目录，每目录有source/input hashes、
run.json、trace.csv、events.csv、metadata.json、raw.log；另有summary.json和
rough-inputs.png。图展示每组seed 0，不是每组最坏seed。

本次链接真实filter/state/pyro逻辑并启用ASan/UBSan，但注入点仍是**已接受的altitude**。
Baro.cpp压力/rate gate、Health.cpp实际sensor检测、I2C、SD/ESP32调度、RTC电气保持、
输出电流与机械deployment均未运行。不能据此断言真实driver会接受所有异常值。
同样不能用可能存在的driver保护否定已复现的flight-logic输入敏感性。
Noise/jitter组合实验不能单独量化每个因素的因果贡献。

下一步应以真实raw pressure/altitude时间序列标定noise，并加入实际Baro.cpp边界的
replay，检查异常到底会否进入flight logic；本次未擅自调整ejection条件。

复跑（使用新的输出目录以保留原证据）：

```sh
backend/.venv/bin/python docs/validation/rough_ejection_experiment.py --out flights/simulations/NEW_ROUGH_RUN
```
