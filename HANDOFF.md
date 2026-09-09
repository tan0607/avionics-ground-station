# HANDOFF — read this first (context for any Claude Code window)

> This is the shared context for the rocket avionics **ground station**. Any new Claude Code
> window opened on this project should read **this file first**, then the two design docs it points to.
> It captures every decision, the current status, the per-window job split, and copy-paste opening prompts.
> Last updated: 2026-07-05.
>
> ⚠️ **This file predates the switch to the MRCC link and is stale in two ways.**
> The radio is a bare SX1278 on SPI, not an EBYTE E32 — every mention of
> M0/M1/AUX below is history. And the downlink is a 52-67 byte MRCC binary frame
> at 10 Hz, expanded by the ground station into the ASCII MRCC text that
> `shared/protocol/mrcc.py` parses — not the 32-byte binary frame described in
> §5, and not the 2 Hz ASCII downlink that replaced it. `mrcc.py` decodes it
> and maps it onto `packet.Telemetry`, which survives as the project's internal
> data shape rather than as a wire format. For what actually flies and how to
> flash it, read **`firmware/README.md`** — it is current.

## 0. TL;DR

- We're building a **ground station**: receive E32 LoRa telemetry from a rocket → live web dashboard → CSV export for a post-launch data report (PLDR). Downlink-only prototype; uplink is phase 2.
- Full plans live in **`GROUND_STATION_PLAN.md`** (hardware/protocol/architecture) and **`DESIGN_SPECS.md`** (dashboard UI — shadcn admin-dashboard 骨架 + mission-control 纪律). Read both.
- Work is split across **multiple Claude Code windows**, one scoped job each. This file is the coordination hub.
- **The 32-byte packet is the shared contract.** It lives in `shared/protocol/packet.py` (source of truth). Never redefine the struct in another window — import it.

## 1. Environment (already set up — don't redo)

| Thing | State |
|---|---|
| Node.js / npm | **v26.4.0 / 11.17.0** (installed via Homebrew) |
| Python | **3.14.6** |
| git | initialized; commit per milestone |
| Design skills | `impeccable` + `avoid-ai-design` installed **globally** in `~/.claude/skills/` |
| impeccable hook | project `.claude/settings.local.json` runs impeccable's detector after Edit/Write on UI files |

## 1b. New computer? Run this setup ONCE before pasting any window prompt

`git pull` brings the code + docs, but **NOT** the toolchain or the design skills (those live
outside the repo). On a fresh machine, do this once:

```bash
# 1. Node 20+ (needed for the frontend + impeccable). On macOS:
brew install node

# 2. Design skills — global, not in the repo:
git clone https://github.com/funboy322/avoid-ai-design.git ~/.claude/skills/avoid-ai-design
npx impeccable install     # pick claude; also re-creates the impeccable edit hook for this machine

# 3. Per-window deps, when you reach that window:
#    backend/   -> python3 -m venv .venv && pip install fastapi uvicorn pyserial
#    dashboard/ -> npm install
#    notebooks/ -> pip install pandas matplotlib jupyter
```

Until step 2 is done, the frontend window's `impeccable` / `avoid-ai-design` skills won't exist.
Everything else (HANDOFF.md, the prompts, PROTOCOL.md, packet.py, all built code) travels with git.

## 2. Design-skill policy (frontend windows)

- Use **`impeccable`** in **product register** (`audit` / `polish` / `quieter`; go easy on `bolder`) to generate/refine UI.
- Finish UI work with an **`avoid-ai-design`** audit.
- Run **`/impeccable init`** as the **first step in the frontend window** (feed it `DESIGN_SPECS.md §4` — shadcn dashboard 骨架 + mission-control 纪律：all-black, no AI slop). It is NOT a global prerequisite; it only matters where UI is written.
- **Do NOT use the official `frontend-design` skill.** (User preference; impeccable + avoid-ai-design is the chosen pair — stacking more design skills makes them fight.)

## 3. Repo layout (one dir per window ⇒ near-zero merge conflicts)

```
shared/protocol/packet.py     32-byte codec + CRC + stream parser   ← CONTRACT (source of truth)
shared/protocol/PROTOCOL.md   human spec of the wire format         ← ✅ done
shared/fake_telemetry.py      full-flight simulator                 ← ✅ done
backend/                      FastAPI serial→CSV→WebSocket           ← Window 1
dashboard/                    Vite/React/TS + uPlot + shadcn         ← Window 2 (+ map from W3)
notebooks/                    PLDR Jupyter template                  ← Window 4
firmware/                     flight computer + ground station sketches
flights/                      runtime session data (raw.log, csv)
```

## 4. Current status

- ✅ Setup done (§1). `git log`: `docs` → `.gitignore` → `shared codec + handoff`.
- ✅ **`shared/protocol/packet.py`** written and **passes its self-test** (`python3 shared/protocol/packet.py`). It is the authoritative 32-byte packet codec (encode/decode/CRC + a `PacketParser` stream framer + `Telemetry` dataclass + `CSV_COLUMNS`).
- ✅ **Contract complete**: `shared/protocol/PROTOCOL.md` + `shared/fake_telemetry.py` written and verified — self-test passes, a full simulated flight (pad→landed, ~1390 m apogee) round-trips through the codec, and loss stats work.
- ⏳ Everything else pending per the window map (§6). **Next up: Frontend + Firmware can start in parallel, then Backend, then Map + PLDR.**

## 5. The packet (summary — `packet.py` is the source of truth)

Wire frame, **little-endian, 32 bytes**:

```
[0:2]   SYNC   0xAA 0x55            (literal bytes on the wire)
[2:30]  BODY   28 bytes            (fields below)
[30:32] CRC16  CRC-16/CCITT-FALSE over BODY (poly 0x1021, init 0xFFFF)
```

BODY fields (offsets relative to frame start):
`msg_type u8@2 · seq u16@3 · flight_state u8@5 · onboard_ms u32@6 · baro_alt_m i16@10 · vspeed_dms i16@12 · gps_lat i32@14 (deg×1e7) · gps_lon i32@18 · gps_alt_m i16@22 · gps_sats u8@24 · gps_fix u8@25 · tilt_deg u8@26 · vbat_dv u8@27 (×0.1V) · flags u8@28 · reserved u8@29`

Enums: `FlightState` = PAD/BOOST/COAST/APOGEE/DROGUE/MAIN/LANDED (0–6); `GpsFix` = 0/2/3.
Flags bits: `CONTINUITY(0) · PYRO_FIRED(1) · SD_OK(2) · ARMED(3)`.
**Firmware must match this byte-for-byte** (same struct, same CRC). SD-card log should use the same struct (superset OK) so one Python parser handles SD + GS.

Import it anywhere (from repo root):
```python
from shared.protocol import packet
t = packet.decode(frame)                 # -> Telemetry | None (checks sync + CRC)
p = packet.PacketParser(); p.feed(chunk) # -> yields Telemetry, resyncs on noise
```

## 6. Window map + copy-paste opening prompts

Open each in a **fresh Claude Code window in this project folder**, paste the prompt as the first message.

**Order:** Contract window first (finishes the shared contract) → then Frontend + Firmware can start in parallel → then Backend → then Map + PLDR.

---

### Contract window — ✅ DONE (PROTOCOL.md + fake_telemetry.py built & verified; prompt kept for reference)
```
You're finishing the SHARED CONTRACT for this rocket ground station. Read HANDOFF.md and GROUND_STATION_PLAN.md §3.
shared/protocol/packet.py ALREADY EXISTS and passes its self-test (run: python3 shared/protocol/packet.py) — it is the source
of truth for the 32-byte packet; do NOT rewrite its layout or CRC. Your job:
1. Write shared/protocol/PROTOCOL.md — the human spec matching packet.py EXACTLY: framing (SYNC 0xAA 0x55 + 28-byte body +
   CRC16), a field table with byte offsets/types/units/scaling, the FlightState + GpsFix enums, the flags bitfield, the
   CRC-16/CCITT-FALSE definition WITH a C reference implementation AND a packed C struct so firmware matches byte-for-byte,
   note little-endianness, and the shared CSV column contract (packet.CSV_COLUMNS).
2. Write shared/fake_telemetry.py — simulate a full flight (pad→boost→coast→apogee→drogue→main→landed) emitting framed 32-byte
   packets at 4 Hz. Import the codec: `from shared.protocol import packet`. Default = realtime binary frames to stdout (a fake
   serial stream); flags: --fast (dump instantly), --csv (decoded rows), --loss P (randomly drop fraction P to test loss stats).
   Launch site 3.2437N,101.7061E; add downrange wind drift, tilt growth on descent, slow vbat droop, seq increment, set the
   PYRO_FIRED flag at deploys; print event markers (liftoff/burnout/apogee/deploys/landed) to stderr.
3. Verify: python3 shared/protocol/packet.py passes; python3 -m shared.fake_telemetry --csv --fast prints a sane altitude arc.
   Commit: "feat(protocol): PROTOCOL.md + fake telemetry generator".
```

### Window 1 — Backend
```
You're the BACKEND window. Read HANDOFF.md, GROUND_STATION_PLAN.md §3–4, and shared/protocol/PROTOCOL.md.
Build a FastAPI backend in backend/ that: (1) imports the decoder via `from shared.protocol import packet` — do NOT redefine the
struct; use packet.PacketParser; (2) reads bytes from a source (pyserial in prod; in dev run `python3 -m shared.fake_telemetry`
as the source) → append raw bytes + host timestamp to flights/<session>/raw.log FIRST, THEN parse; (3) write each decoded row to
flights/<session>/telemetry.csv using packet.CSV_COLUMNS, flush every line; (4) track seq-counter loss %; (5) broadcast decoded
packets over a native WebSocket (no socket.io) and serve the built frontend as static files. Create the session folder structure
from §4 (metadata.json/raw.log/telemetry.csv/events.csv). Set up .venv: python3 -m venv .venv && pip install fastapi uvicorn
pyserial. "Raw first, then parse." Commit when fake data flows serial→CSV→WebSocket end to end.
```

### Window 2 — Frontend (design-skill window)

> M1 已建（commit `dfa50e9`：单屏 ops console，status bar + altitude chart + readouts，跑 mock）。
> 现在的任务是 **把它改成 shadcn admin-dashboard shell**（方向见下 + DESIGN_SPECS §3/§4），数据层完全不动。

```
You're the FRONTEND window. FIRST run /impeccable init (answer with DESIGN_SPECS.md §4: shadcn admin-dashboard 骨架 + mission-control
纪律 — all-black, monospace data, semantic color only, no AI slop). Then read HANDOFF.md, DESIGN_SPECS.md (esp. §3 layout, §4 aesthetic),
GROUND_STATION_PLAN.md §5. dashboard/ already exists (Vite+React+TS+Tailwind4+shadcn+uPlot, mock源可跑).

REWORK M1 into a shadcn admin-dashboard layout WITHOUT touching the data layer:
- KEEP UNCHANGED: src/hooks/useTelemetry.ts (ref+rev 架构,已验证,不是性能bug), src/lib/{mock,protocol,wsClient}.ts,
  UPlotChart.tsx 的 imperative + setData(rev) 模式.
- BUILD the shell: 左侧细导航栏 (Live 有效; Map/Log/Settings 占位, lucide 图标) + 顶栏 (mission名 + T+时钟 + link状态pill +
  loss% + source).
- KPI 卡片行 (shadcn Card): ALT / APOGEE / V-SPEED / TILT / VBAT / GPS(sats·fix). 每张 = 小号大写标签 + 大号 JetBrains Mono 数字
  + 单位/状态行.
- 主区: 大高度图表卡 + 右侧 GO/NO-GO (continuity/pyro/sd/armed 状态点) + 飞行状态时间线.
- AltitudeChart.tsx 的 series 加 paths: uPlot.paths.spline() 平滑曲线 (数据是4Hz,直线插值看着顿); apogee marker 保留.

DESIGN RULES (硬性,做完跑 impeccable polish/quieter + avoid-ai-design 终审):
near-black 底 (oklch(0.14 0 0) 一档,卡片surface再亮一档), 1px hairline, 单一青色数据强调色, 语义状态色(绿/琥珀/红),
数字全 mono + tabular-nums. 禁止: 紫/靛渐变, glassmorphism, 发光, rounded-2xl 悬浮重阴影糖果卡, emoji 图标. 卡片扁平统一圆角边框1px阴影极弱.
Do NOT use the frontend-design skill. Fully offline — no CDNs, self-host fonts.
验收: npm run dev, mock 源跑完整飞行, 一屏不滚动, 曲线顺滑, KPI 实时更新, GO/NO-GO 在 apogee/deploy 变色. Commit the rework.
```

### Window 3 — Map
```
You're the MAP window. Read HANDOFF.md, DESIGN_SPECS.md §2 (map row), GROUND_STATION_PLAN.md §5. Build an offline map panel for
dashboard/ using Leaflet + protomaps-leaflet + a PMTiles file. Use go-pmtiles to extract ONLY the launch-site region from a Protomaps
daily build — do NOT bulk-scrape OSM raster tiles (violates their policy). Deliver a self-contained React map component: offline
basemap, live ground track, last-known-coordinate shown BIG + distance/bearing from GS. Verify it works with the network fully
disabled. Coordinates come from shared/protocol/PROTOCOL.md GPS fields; until wired, drive from a scripted track. Commit when offline
basemap + track + last-fix readout work with WiFi off.
```

### Window 4 — PLDR notebook
```
You're the PLDR window. Read HANDOFF.md, GROUND_STATION_PLAN.md §6, shared/protocol/PROTOCOL.md. Build a Jupyter notebook template in
notebooks/ that takes a flight folder (flights/<session>/) and produces: altitude/velocity/acceleration vs time, apogee detection +
max velocity + descent rate, GPS track + landing distance, an event timeline table, and link stats (loss % vs flight phase). Reuse
the SAME parser via `from shared.protocol import packet` (one parser for SD + GS logs). Test against a telemetry.csv from
`python3 -m shared.fake_telemetry`. pip install pandas matplotlib jupyter. Commit when it runs end-to-end on a fake flight.
```

### Window 5 — Firmware (independent; start now, test when E32 arrives)
```
You're the FIRMWARE window (independent hardware track). Read HANDOFF.md, GROUND_STATION_PLAN.md §2, §3 (E32 limits + AUX discipline),
§7, shared/protocol/PROTOCOL.md. Two targets in firmware/: (1) bridge — a THIN ESP32 devkit sketch: manage E32 M0/M1/AUX, WAIT for AUX
HIGH before every UART write (never blind-write — #1 lock-up cause), forward the raw E32 byte stream to USB serial, LED on packet RX;
checksum/parse stays on the laptop. (2) onboard TX — pack the shared 28-byte body + SYNC + CRC (byte-identical to PROTOCOL.md /
packet.py) and transmit at 4 Hz, air-rate 2.4k, 9600 UART. Flag the RFI risk (§8): plan a test for whether E32 TX drops the NEO-M8N GPS
fix. Commit per target.
```

## 7. Coordination rules

- **One packet definition.** Only `shared/protocol/packet.py` defines the struct/CRC. Everyone imports it. Firmware mirrors PROTOCOL.md byte-for-byte.
- **Commit per milestone**, scoped to your window's directory (dir separation keeps windows from colliding).
- **Offline-first** everywhere: no CDNs, self-host fonts, offline map tiles. `npm run build` output must run with the network off.
- **Aesthetic** = shadcn admin-dashboard 骨架 + mission-control 纪律 (DESIGN_SPECS §4): 结构像 SaaS dashboard，气质像 mission console —— all-black, mono, semantic color, no AI slop.
- **Raw first, then parse** on the backend (log bytes to raw.log before decoding).
