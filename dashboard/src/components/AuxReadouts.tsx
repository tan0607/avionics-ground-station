/**
 * AuxReadouts — a second instrument strip: radio quality, plus every decoded
 * field PROTOCOL.md has no slot for (pressure, GPS altitude, heading, course,
 * ground speed, horizontal accel/velocity, body rates).
 *
 * Why this exists: the downlink currently in use carries MORE than the protocol
 * defines. Those surplus fields were parsed and then dropped, which reads to an
 * operator as "the ground station is broken" — the data is visibly in the serial
 * stream and visibly not on the screen. Rather than widen the protocol for
 * fields that may not survive the next firmware revision, they ride along in
 * `frame.extra` and are listed here verbatim.
 *
 * Why a horizontal strip and not a card in the right rail: the rail already
 * carries three fixed-height panels and a fourth pushes the flight-state
 * timeline off the viewport. This is instrument data, so it gets the instrument
 * treatment — the same segmented-cluster language as KpiRow, in the full width.
 *
 * Cells are data-driven off the frame: a transmitter that adds a field gets a
 * cell for free, one that drops a field loses it, and neither is misreported.
 */
import { Card } from "@/components/ui/card"
import { fmtFixed } from "@/lib/format"
import type { TelemetryFrame } from "@/lib/protocol"

/**
 * Presentation for the keys we know about. Anything not listed still renders —
 * under its raw key — so an unrecognised field is visible rather than invisible.
 */
const KNOWN: Record<string, { label: string; unit: string; digits: number }> = {
  P: { label: "Press", unit: "Pa", digits: 0 },
  HDG: { label: "Hdg", unit: "°", digits: 0 },
  COURSE: { label: "Course", unit: "°", digits: 0 },
  GSPEED: { label: "Gnd Spd", unit: "m/s", digits: 2 },
  AX: { label: "Ax", unit: "m/s²", digits: 2 },
  AY: { label: "Ay", unit: "m/s²", digits: 2 },
  AZ: { label: "Az", unit: "m/s²", digits: 2 },
  // Body-rate gyro. The transmitter started sending these on 2026-08-19; before
  // this they rendered under their raw keys with no unit, which reads as a bare
  // number an operator has to guess the meaning of.
  GX: { label: "Gx", unit: "°/s", digits: 1 },
  GY: { label: "Gy", unit: "°/s", digits: 1 },
  GZ: { label: "Gz", unit: "°/s", digits: 1 },
  VX: { label: "Vx", unit: "m/s", digits: 2 },
  VY: { label: "Vy", unit: "m/s", digits: 2 },
  TEMP: { label: "Temp", unit: "°C", digits: 1 },
  // Apogee so far, as the VEHICLE computed it. Not the same number as the max
  // of baro_alt_m seen on the ground: a dropped packet never reaches this
  // console, and the vehicle's own peak does.
  MX: { label: "Max Alt", unit: "m", digits: 1 },
}

/**
 * Display order. GPSDATA is excluded on purpose: it is a health signal, already
 * consumed into the GPS peripheral row, and repeating it here as a bare 1 would
 * be a number with no meaning to the operator.
 *
 * SD/BA/IM and AR/FI are hidden for exactly that reason. They drive the
 * subsystem-health panel and the safety readouts respectively, where they are
 * rendered as state; a second copy here as a bare 0 or 1 is a number the
 * operator has to decode, sitting next to the panel that already says it.
 *
 * SDF/SDL/SDE — the vehicle's recorder — are hidden on the same grounds AND on
 * one of their own: they ride one packet in ten, so a cell here would sit empty
 * for nine frames out of ten and flash a number on the tenth. They are latched
 * and rendered as the SD row's state instead (useOnboardLog).
 */
const ORDER = ["P", "MX", "HDG", "COURSE", "GSPEED", "AZ", "AX", "AY", "GX", "GY", "GZ", "VX", "VY", "TEMP"]
const HIDDEN = new Set(["GPSDATA", "SD", "BA", "IM", "AR", "FI", "SDF", "SDL", "SDE"])

function orderedKeys(extra: Record<string, number>): string[] {
  const present = Object.keys(extra).filter((k) => !HIDDEN.has(k))
  const ranked = ORDER.filter((k) => present.includes(k))
  const rest = present.filter((k) => !ORDER.includes(k)).sort()
  return [...ranked, ...rest]
}

function Cell({ label, value, unit }: { label: string; value: string; unit: string }) {
  return (
    <div className="min-w-0 flex-1 px-2 py-1">
      <div className="truncate text-[0.5625rem] uppercase tracking-[0.12em] text-ink-mute">
        {label}
      </div>
      <div className="truncate tabular-nums text-[0.75rem] leading-tight text-ink">
        {value}
        {unit && <span className="ml-0.5 text-[0.5625rem] text-ink-mute">{unit}</span>}
      </div>
    </div>
  )
}

export function AuxReadouts({ frame }: { frame: TelemetryFrame | null }) {
  const extra = frame?.extra ?? {}
  const keys = orderedKeys(extra)
  const dash = "—"

  return (
    <div className="px-2 pt-2">
      <Card className="flex-row divide-x divide-hairline overflow-hidden">
        {/* Radio quality first: it describes the link, not the vehicle. */}
        <Cell
          label="RSSI"
          value={frame?.rssiDbm != null ? String(frame.rssiDbm) : dash}
          unit={frame?.rssiDbm != null ? "dBm" : ""}
        />
        <Cell
          label="SNR"
          value={frame?.snrDb != null ? fmtFixed(frame.snrDb, 1) : dash}
          unit={frame?.snrDb != null ? "dB" : ""}
        />
        {/*
          GPS altitude is a first-class protocol field (`gps_alt_m`), not an aux
          one — it is read off the frame, not `extra`. It lives here because the
          KPI strip has no room for it and no other view showed it at all, so it
          was being decoded and recorded and never once put in front of anyone.
        */}
        <Cell label="GPS Alt" value={frame ? fmtFixed(frame.gpsAltM, 0) : dash} unit="m" />
        {keys.map((k) => {
          const meta = KNOWN[k]
          return (
            <Cell
              key={k}
              label={meta?.label ?? k}
              value={fmtFixed(extra[k], meta?.digits ?? 2)}
              unit={meta?.unit ?? ""}
            />
          )
        })}
        {keys.length === 0 && <Cell label="Aux fields" value={dash} unit="" />}
      </Card>
    </div>
  )
}
