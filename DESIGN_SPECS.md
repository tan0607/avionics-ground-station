# GS Dashboard — Design Specs

> Frontend/UI 设计规格。配合 `GROUND_STATION_PLAN.md` 第 5 节。
> Researched & drafted 2026-07-05.

## 1. 设计目标与硬约束

| 约束 | 含义 |
|---|---|
| **Offline-first** | 发射场没网。**禁止一切 CDN**（fonts/tiles/js 全部本地打包），断网状态下 `npm run build` 出来的东西必须完整能跑 |
| 户外可读 | 阳光下看屏幕：高对比度、大号数字、状态用颜色+形状双编码 |
| Glanceable | 关键信息（altitude、state、link、坐标）1 秒内扫到，不用找 |
| 4Hz 实时流 | WebSocket 推送，图表流式追加不重绘整图 |
| 单人操作 | 飞行时没人有空点菜单，一屏放下所有关键面板，零导航 |

## 2. 技术栈（定案）

| 层 | 选择 | 理由 |
|---|---|---|
| 构建 | **Vite + React + TypeScript** | 你熟 shadcn = 熟 React；Vite 打包天然 offline-friendly |
| UI 组件 | **Tailwind + shadcn/ui** | 你已经会；只用它做 shell/按钮/badge/dialog，**不用它的默认审美**（见 §4） |
| 实时图表 | **uPlot** | ~50KB 开源，流式数据王者，帧级性能远超需求；Recharts/shadcn charts 留给 post-flight 静态图。SciChart 更快但 commercial，不考虑 |
| 地图 | **Leaflet + Protomaps PMTiles**（`protomaps-leaflet`） | 单文件离线矢量地图：用 `go-pmtiles` 从 Protomaps 每日全球构建裁出发射场区域，本地 serve，免费无授权问题。**不要批量抓 OSM raster tiles**——违反 OSM tile usage policy |
| 通信 | **原生 WebSocket** + 简单重连逻辑 | FastAPI 原生支持，不引 socket.io 减依赖 |
| 状态 | React state / zustand（如果需要） | 数据流简单，别上 Redux |

## 3. 布局（单屏，1080p laptop）

```
┌────────────────────────────────────────────────────────────────┐
│ FLIGHT STATE: ASCENT   T+ 00:12.4   ● LINK 0.3s  loss 2.1%     │ ← 顶栏：状态+计时+链路
├──────────────────────────────────┬─────────────────────────────┤
│                                  │  ALT      1 247 m   ▲       │
│   Altitude vs Time (uPlot)       │  VSPEED   +142 m/s          │
│   [实时曲线 + apogee 标记]        │  MAX ALT  1 247 m           │ ← 大数字读出区
│                                  │  VBAT     7.9 V  ●          │   (mono 字体)
│                                  │  TILT     4°                │
├──────────────────────────────────┤  CONT     ● ARMED           │
│   Vertical Speed (uPlot)         ├─────────────────────────────┤
│                                  │  MAP (Leaflet offline)      │
├──────────────────────────────────┤  轨迹 + 落点大字坐标         │
│ EVENT LOG                        │  3.2437°N 101.7061°E        │
│ 12:01:03 LIFTOFF  12:01:15 ...   │  ↖ 320m NW from GS          │
└──────────────────────────────────┴─────────────────────────────┘
```

- 报警态（如 "apogee 已过 + 未开伞"）：顶栏整条变红闪烁 + 蜂鸣（Web Audio）
- Link 超 3s 无包：LINK indicator 红 + "LAST PACKET 8.2s AGO" 放大

## 4. 视觉方向：Mission Control，不是 SaaS landing page

**Anti-AI-slop 规则**（违反即打回）：
- ❌ 紫色渐变、玻璃拟态、默认 shadcn 圆角卡片海、Inter/Roboto 默认字体
- ✅ **深色底**（近黑 `#0a0e12` 类），面板用 1px 边线分区而不是浮起阴影
- ✅ 数据一律 **monospace**（JetBrains Mono / IBM Plex Mono，self-host）；标签用小号大写字母 + letter-spacing
- ✅ 颜色只承载语义：绿=nominal、琥珀=caution、红=alarm、青/白=数据。装饰性颜色 = 0
- ✅ 密度向 NASA/SpaceX console 靠，不是营销 dashboard 的大留白
- 参考气质：Open MCT、SpaceX webcast telemetry 条、Grafana dark

## 5. Claude Code 设计类 skills（GitHub 上真实存在，已验证）

| Skill / 资源 | 干嘛的 |
|---|---|
| [pbakaus/impeccable](https://github.com/pbakaus/impeccable) | **首选**。Paul Bakaus（jQuery UI 作者）出品，35k+ stars。给 AI 装一套 design vocabulary：23 个单词命令（`polish` / `audit` / `critique` / `distill` / `bolder` / `quieter`…），每个命令挂 7 个维度的 reference（typography/color/motion/spatial/interaction/responsive/UX writing），还分 brand vs product 语境。思路是从源头改变生成分布，slop 根本不产生。免费，装完自动生效 |
| [leonxlnx/taste-skill](https://github.com/leonxlnx/taste-skill) | 第三方 design skill 里 star 最多的。11 个 design 变体 + 3 个 image gen skill 的套件，两个旋钮：`DESIGN_VARIANCE` 1-10（居中规整 → 不对称激进）、`MOTION_INTENSITY` 1-10。装：`npx skills add https://github.com/Leonxlnx/taste-skill` |
| [anthropics/claude-code → plugins/frontend-design](https://github.com/anthropics/claude-code/blob/main/plugins/frontend-design/README.md) | **官方** frontend 设计 skill：先定 aesthetic direction 再写码，明确反 generic AI 风。通过 `/plugin` marketplace 装 |
| [funboy322/avoid-ai-design](https://github.com/funboy322/avoid-ai-design) | 审计**已有**代码，找出并重写 AI-slop 模式（紫渐变/Inter/默认 shadcn）。生成用上面的，修复用这个 |
| [h3nryprod01/design-taste](https://github.com/h3nryprod01/design-taste) | 把 impeccable + taste-skill + emilkowalski/skill 三家合成一个的 synthesis 版，不想装三个就装这个 |
| [Koomook/claude-frontend-skills](https://github.com/Koomook/claude-frontend-skills) | 社区 frontend skills 合集 plugin |
| [wilwaldon/Claude-Code-Frontend-Design-Toolkit](https://github.com/wilwaldon/Claude-Code-Frontend-Design-Toolkit) | Skills + plugins + MCP + CLAUDE.md 技巧汇总 |
| [travisvn/awesome-claude-skills](https://github.com/travisvn/awesome-claude-skills) | Skills 大列表，找其他领域的入口 |
| [Anthropic frontend aesthetics cookbook](https://platform.claude.com/cookbook/coding-prompting-for-frontend-aesthetics) | 官方 prompt 指南，§4 的规则很多源自这里的思路 |

### 开发工具：shadcn MCP（官方）

写 frontend 时装，让 Claude Code 直接浏览/搜索/安装 registry 组件（拿最新源码，不靠训练数据里的旧版）。Project root 建 `.mcp.json`：

```json
{
  "mcpServers": {
    "shadcn": { "command": "npx", "args": ["shadcn@latest", "mcp"] }
  }
}
```

或 `npx shadcn@latest mcp init --client claude`，重启后 `/mcp` 确认。文档：https://ui.shadcn.com/docs/mcp

**在这个项目里怎么用（重要）**：这些 skill 的默认倾向是"做出令人惊艳的页面"，但我们做的是 **ops console，不是 portfolio piece**——§4 的 mission-control 方向优先于 skill 的品味。具体设定：
- impeccable 用 **product register**（不是 brand），主力用 `audit` / `polish` / `quieter`，慎用 `bolder`
- taste-skill 如果用：`DESIGN_VARIANCE` ≤ 3、`MOTION_INTENSITY` 1-2 —— 飞行中动画是干扰，还跟 4Hz 图表流抢帧
- 推荐组合：**impeccable（写+改）+ avoid-ai-design（终检）**，够了；skill 之间会互相打架，别全装

## 6. 参考项目（同类 ground station，抄架构不抄代码）

| 项目 | 参考什么 |
|---|---|
| [RMIT-Hive-Rocketry/GCS (SOTERIA)](https://github.com/RMIT-Hive-Rocketry/GCS) | **最接近你们**：学生火箭队 LoRa GCS，web 前端 + WebSocket，IREC 2026 实战部署。看它的多视图划分（main control / pre-flight / GSE）和 device emulator 思路（假数据测试！） |
| [sgoudelis/ground-station](https://github.com/sgoudelis/ground-station) | 卫星向但栈一致：React + Vite + FastAPI + Socket.IO + Leaflet/MapLibre，验证了我们的选型 |
| [rocketproplab/Base11-GUI](https://github.com/rocketproplab/Base11-GUI) | UCSD 火箭队 telemetry dashboard，规模较小好读 |
| [NASA Open MCT](https://nasa.github.io/openmct/) | 不建议直接用（学习曲线陡），但 UI 密度和信息层级抄它 |
| [Serial Studio](https://github.com/Serial-Studio/Serial-Studio) | Plan B：如果 web dashboard 进度崩了，JSON 配置即得 dashboard + CSV，保底方案 |

## 7. 开发时的实践顺序

1. 先写 **fake telemetry generator**（Python 脚本按 packet 格式模拟一次完整飞行：pad→boost→apogee→descent→landed）——前端全程不需要硬件就能开发
2. 前端第一个 milestone：顶栏 + altitude uPlot + 大数字区，接 fake 数据的 WebSocket
3. 地图单独一个 milestone：`go-pmtiles extract` 裁发射场区域 PMTiles + `protomaps-leaflet` 渲染，全程离线验证
4. 报警逻辑（no-deploy 检测、link stale）最后但**必须在 dry run 前完成**
