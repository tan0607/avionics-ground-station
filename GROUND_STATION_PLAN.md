# Ground Station Plan

> Rocket avionics ground station — 接收 E32 LoRa telemetry、实时可视化、CSV export 给 Python 做 PLDR (post-launch data report)。
> Drafted 2026-07-05.

## 1. 总架构

```
[火箭]                          [Ground Station]
ESP32-S3 ──► E32 (TX)  ~~~RF~~~  E32 (RX) ──► bridge MCU ──USB──► Laptop
   │                                                                │
   └─► SD card (全速率 master log)                     Python backend (serial → WebSocket)
                                                        ├─► raw log + telemetry.csv (实时落盘)
                                                        └─► Browser dashboard (实时可视化)
                                                                     │
                                              飞行结束 → Python/pandas → PLDR 图表和报告
```

核心概念：**LoRa telemetry 不是主数据源**。E32 带宽很小，空中只能发每秒几个降采样 packet。全速率数据（IMU 50Hz 等）在机上 SD card。

- **SD card** = master record，飞完拆出来，PLDR 主要用它
- **GS telemetry** = 实时监控 + 找火箭（GPS 落点）+ 火箭丢失/摔烂时的 backup 数据

## 2. 接收端硬件（bridge）

- **建议用普通 ESP32 devkit**（最便宜那种即可），不用 Arduino Uno/Nano：
  - E32 是 3.3V 器件，5V Arduino 的 TX 直接接会伤模块（要分压/level shifter）
  - Phase 2 要加 uplink，ESP32 直接就绪；toolchain 和机上代码还能复用
- Bridge 工作保持"薄"：管 M0/M1/AUX、把 E32 字节流原样转发 USB serial、LED 指示收包。**校验和解析放 laptop 端。**
- 其他：三脚架/桅杆架天线、laptop 供电（power bank）。

## 3. Telemetry 协议

### E32 限制
- Transparent mode 半双工模块；air rate 2.4k/9.6k/19.2 kbps，**越低距离越远**，保守用 2.4k
- 2.4 kbps 实际安全预算按 **~150 B/s** 规划
- 单 packet ≤ 58 bytes（E32 按 58-byte sub-packet 发射，避免拆包）
- **E32 无 RSSI 输出**（E22/E220 才有）→ 链路质量靠 seq counter 算丢包率
- **AUX pin 必须接且必须处理**：社区大量报告不接 AUX（或不加 4.7k pull-up）会死机/丢数据。发送前等 AUX HIGH，机上和 bridge 固件都要遵守，不准盲写 UART

### Packet 格式（binary，固定 ~32 bytes，@4Hz = 128 B/s）

| 字段 | 类型 | 备注 |
|---|---|---|
| sync header | 2B (`0xAA55`) | 找包头 |
| seq counter | uint16 | 算丢包率 |
| flight state | uint8 | PAD / ASCENT / DESCENT / LANDED... |
| onboard millis | uint32 | 机上时间轴 |
| baro altitude | int16 | AGL |
| vertical speed | int16 (dm/s) | |
| GPS lat / lon | int32 × 2 (deg×1e7) | |
| GPS alt | int16 | |
| GPS sats + fix | uint8 | |
| tilt | int8~int16 | 发倾角就够，不发整个 quaternion |
| Vbat | uint8 (×0.1V) | |
| flags | uint8 | continuity / pyro fired / SD OK 等 bit |
| CRC16 | uint16 | |

用 binary 不用 CSV text（同样信息 text 要 80-100B，rate 直接砍半）。

**纪律**：机上 SD log 用同一个 struct 定义（更高频率写同格式/超集）→ Python 一个 parser 同时解 SD 和 GS 数据。

## 4. GS 数据落盘

黄金法则：**先存 raw，再解析**。每段字节流先带 host timestamp 原样 append 进 `raw.log`，然后才找包头/CRC/解码。parser 有 bug 也不丢数据。

每次飞行一个 session folder：

```
flights/2026-07-05_flight-01/
├── metadata.json      # 日期、air rate、packet 版本、备注
├── raw.log            # 原始字节流 + host 时间戳
├── telemetry.csv      # 实时解码逐行 append（每行 flush，防 crash）
└── events.csv         # liftoff / apogee / pyro / landed 时间点
```

CSV 每行带三个时间戳：host time、onboard millis、GPS time。

## 5. Web Dashboard 架构

**Local Python backend + browser 前端**（不用纯 Web Serial API）：
- 浏览器 tab 崩了不能丢数据 → logging 必须在 backend
- PLDR 反正要 Python → parser 只写一次共用
- Launch site 没网 → 一切本地跑

技术栈：
- **Backend**: Python + `pyserial` + FastAPI（静态文件 + WebSocket 推数据），单文件百来行量级
- **Frontend**: 静态 HTML/JS；图表 uPlot 或 Chart.js；地图 **Leaflet + Protomaps PMTiles**（单文件离线矢量地图，`go-pmtiles` 裁发射场区域，**不要**批量抓 OSM tiles——违反其 usage policy）
- 数据流：serial → parse → (a) append CSV (b) WebSocket broadcast

### Panels（按重要性）
1. Altitude vs time 实时曲线 + 当前值大字 + max altitude
2. GPS 地图：ground track + **最后已知坐标大字**（回收靠这个）+ 距离/方位
3. Flight state 状态牌 + event log
4. 链路健康：收包数、丢包率%、**距上一包多少秒**（>3s 变红）
5. Vbat、continuity 状态灯（go/no-go）
6. Vertical speed、tilt
7. Raw console（可折叠）

## 6. Export → PLDR pipeline

- `telemetry.csv` 本身就是 export，不需要额外步骤
- PLDR 用 **Jupyter notebook template**：输入 flight folder → 自动出：
  - Altitude / velocity / acceleration vs time（SD 全速率）
  - Apogee 检测、max velocity、descent rate
  - GPS 轨迹、落点距离
  - Event timeline 表
  - 链路统计（丢包率 vs 飞行阶段 = 天线性能报告）
- 之后每次飞行的 PLDR = 跑一遍 notebook

## 7. Phase 2: Uplink / C2 link

E32 是双向半双工 transceiver，uplink 完全可行（见 plan 讨论记录）。现在预留：
- Packet 留 `flags`/`msg type` 字节区分 telemetry / command / ack
- **Time-slot** 方案：火箭每 250ms 发一包，gap 里 GS 发 command，下一包带 ack
- 首批 command：arm/disarm pyro、trigger buzzer（找火箭）、**manual ejection（备用开伞）**
- Pyro 类 command 要安全设计：CRC + 专用 command code + arming 序列 + timeout

### Manual ejection（备用开伞）的定位
- **它不是真 redundancy**：和 auto ejection 共享 MCU/MOSFET/e-match/电池，只能救"硬件正常但决策逻辑错了"（apogee 检测 bug、state machine 卡死）这一类失败
- 备份优先级：① 机上 failsafe（下降 + 低于 backup 高度 + 未点火 → 强制点火，0 延迟不依赖链路）② 才是 ground manual command
- **PCB 建议（趁未打板）**：加第二路 pyro channel（AO3400 + 电阻 + 2P connector + 2 GPIO），独立 backup charge 才是真 redundancy
- **比赛规则注意（IREC DTEG 原文）**："A COTS flight computer shall fire either the primary or redundant energetic system" —— IREC 系比赛要求两条开伞链路里**至少一条由商业高度计**（StratoLogger/RRC3 类）点火，SRAD 自制板只能做另一条；官方 apogee 成绩也只认 COTS 气压高度计的记录。查你们比赛是否继承这条，是的话要预算一颗 COTS altimeter
- Manual FIRE 安全门：PAD 状态拒绝执行（state gating）、ARM→FIRE 两步 + timeout 自动 disarm、GS 连续重发直到 ack（下坠 tumble 时丢包率高）、UI 确认式按钮
- Dashboard 自动报警："过 apogee + 下降速度超阈值 + pyro 未 fired" → 闪红 + 蜂鸣，人只负责按按钮
- 查比赛/靶场规则：部分要求全自主 recovery、限制 RF 遥控点火

## 8. 自制天线（433MHz，λ≈70cm）

| 天线 | 增益 | 特点 |
|---|---|---|
| ¼-wave ground plane（~16.4cm 振子 + 4 根斜下地网） | ~2 dBi 全向 | 一小时焊完，GS 打底 |
| Moxon | ~5-6 dBi 定向 | 紧凑好做 |
| 3 单元 Yagi | ~6-7 dBi 定向 | 三脚架 + 人手指向追踪，火箭 GS 经典 |

- **两根都做**：ground plane 主力全向，Yagi 增强（专人指向）
- 天线**架高**最重要——落地后火箭在地上，GS 天线 3-4m vs 1m 可能就是收得到 vs 收不到
- 馈线短 RG58 + SMA
- 调试用**丢包率 vs 距离**当指标（range test）；能借 NanoVNA 调 SWR 更好
- 火箭端：**垂直 dipole 优于单根 whip**（RocketTalk 项目试了几十种天线的结论，PCB dipole 成本 ~$2）；¼-wave whip (~16.4cm) 是保底。**不能在 carbon fiber 段内**（屏蔽 RF）
- **RFI 警告（PCB 层面）**：大功率 433MHz TX 会干扰同板的 GPS 和传感器（RocketTalk 1W 版被迫全面屏蔽+磁珠+地平面）。E32 和 NEO-M8N 天线尽量拉开距离，**上电测试"发射时 GPS 是否掉 fix"**，掉就要加屏蔽/降 duty cycle
- 功率注意 MCMC 433MHz ISM 限制（尤其 T30D 1W）

## 9. 开发顺序（milestones）

1. **Bridge + 裸链路**：两块 E32 桌上对发，laptop 看到字节流
2. **协议**：packet struct（机上/GS 共用 header），机上发假数据，GS Python 解码 + CRC + 落 CSV（← 到这里 PLDR 数据通路已通）
3. **Backend + logging**：session folder、raw log、丢包统计
4. **Dashboard**：WebSocket + 前端，先做 altitude 曲线和地图
5. **PLDR notebook**：假数据先跑通 template
6. **天线 + range test**：ground plane 先上，测丢包率 vs 距离
7. **全系统 dry run**：车载测试，走完"开机 → 收数据 → 找回 → 出 PLDR"

## 风险 Top 2

1. **RF 链路** — range test 趁早做
2. **Offline 地图** — 很容易到现场才发现没缓存
