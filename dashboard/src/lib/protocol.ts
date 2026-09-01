/**
 * TypeScript mirror of shared/protocol/PROTOCOL.md (source of truth:
 * shared/protocol/packet.py). The dashboard consumes DECODED telemetry as JSON
 * over the WebSocket — the backend runs packet.PacketParser and broadcasts each
 * decoded frame. This file defines that decoded contract + the normalizer that
 * coerces the backend's wire JSON into the shape the UI uses.
 *
 * Keep enums/flags in sync with packet.py. Do NOT re-implement the byte codec
 * here — the frontend never sees raw bytes.
 */

/**
 * flight_state — matches packet.FlightState (0..6). A `const` object + union
 * (not a TS `enum`) so the file stays fully erasable (Vite's erasableSyntaxOnly
 * / Node type-stripping): `FlightState.PAD` is a value, `FlightState` is a type.
 */
export const FlightState = {
  PAD: 0,
  BOOST: 1,
  COAST: 2,
  APOGEE: 3,
  DROGUE: 4,
  MAIN: 5,
  LANDED: 6,
} as const
export type FlightState = (typeof FlightState)[keyof typeof FlightState]

export const FLIGHT_STATE_NAME: Record<FlightState, string> = {
  [FlightState.PAD]: "PAD",
  [FlightState.BOOST]: "BOOST",
  [FlightState.COAST]: "COAST",
  [FlightState.APOGEE]: "APOGEE",
  [FlightState.DROGUE]: "DROGUE",
  [FlightState.MAIN]: "MAIN",
  [FlightState.LANDED]: "LANDED",
}

/**
 * flags bitfield (packet.py FLAG_*). Used to say WHICH flags a frame reports —
 * see `flagsKnown`. A downlink that carries none of them must not render as
 * "continuity open, SD failed"; those are alarms nobody raised.
 */
export const Flag = {
  CONTINUITY: 1 << 0,
  PYRO_FIRED: 1 << 1,
  SD_OK: 1 << 2,
  ARMED: 1 << 3,
} as const
export type Flag = (typeof Flag)[keyof typeof Flag]

/** Every flag a binary frame reports (packet.FLAGS_ALL). */
export const FLAGS_ALL = Flag.CONTINUITY | Flag.PYRO_FIRED | Flag.SD_OK | Flag.ARMED

/** gps_fix — 0 none / 2 = 2D / 3 = 3D. Same erasable const-object pattern. */
export const GpsFix = {
  NONE: 0,
  FIX_2D: 2,
  FIX_3D: 3,
} as const
export type GpsFix = (typeof GpsFix)[keyof typeof GpsFix]

/**
 * Per-peripheral health (packet.py HEALTH_*). Bit set = that device initialised
 * and is currently responding. The vehicle keeps flying and transmitting with
 * any of these clear — a dead peripheral greys out ONE row, it is never a
 * blanket "AV FAILED". See shared/protocol/PROTOCOL.md.
 */
export const Health = {
  BARO: 1 << 0,
  IMU: 1 << 1,
  GPS: 1 << 2,
  SD: 1 << 3,
  PYRO: 1 << 4,
  VBAT: 1 << 5,
} as const
export type Health = (typeof Health)[keyof typeof Health]

/** All peripherals nominal — what `health` reads on a clean boot. */
export const HEALTH_ALL_OK =
  Health.BARO | Health.IMU | Health.GPS | Health.SD | Health.PYRO | Health.VBAT

/**
 * Display roster, in panel order. `key` matches the backend's `hw_*` wire field
 * and `field` names the telemetry the peripheral feeds — the UI greys that
 * readout out when the peripheral is down.
 */
export const SUBSYSTEMS = [
  { mask: Health.BARO, name: "BARO", key: "hw_baro", feeds: "Altitude" },
  { mask: Health.IMU, name: "IMU", key: "hw_imu", feeds: "Tilt" },
  { mask: Health.GPS, name: "GPS", key: "hw_gps", feeds: "Position" },
  { mask: Health.SD, name: "SD", key: "hw_sd", feeds: "Onboard log" },
  { mask: Health.PYRO, name: "PYRO", key: "hw_pyro", feeds: "Continuity" },
  { mask: Health.VBAT, name: "VBAT", key: "hw_vbat", feeds: "Battery" },
] as const

/**
 * DESIGNED rate of the binary protocol — 4 Hz (PROTOCOL.md). This drives the
 * MOCK SIM ONLY. It is not what the live radio does, and the two must not be
 * conflated: this file used to describe 4 Hz as "the" telemetry rate, which made
 * the console's own documentation disagree with its own link.
 */
export const PACKET_HZ = 4
export const PACKET_INTERVAL_MS = 1000 / PACKET_HZ

/**
 * MEASURED rate of the live MRCC downlink — 2 Hz of new telemetry, each packet
 * transmitted TWICE (~205 ms apart), so the receiver prints ~4 lines/s.
 *
 * Both numbers are here because mistaking one for the other is exactly what
 * makes the console look broken: a serial monitor scrolling at 4 lines/s next
 * to readouts that change twice a second reads as a dashboard falling behind
 * its radio, when in fact it has already drawn every frame it was sent — half
 * of them were byte-identical repeats. `LINK_HZ` is the rate at which the
 * numbers on screen CAN change; `LINE_HZ` is the rate the monitor shows.
 *
 * Source: backend session.PACKET_DESC["mrcc"], measured off /stats and the
 * onboard timestamps in flights/2026-08-19T05-54-40Z. Verify against the RATE
 * readout in the top bar rather than trusting this constant — it is a
 * transmitter setting and this transmitter changes.
 */
export const LINK_HZ = 2
export const LINK_INTERVAL_MS = 1000 / LINK_HZ
export const LINE_HZ = 4

/**
 * Link is considered stale after this many ms with no packet (GS plan §5,
 * DESIGN §3) — six frame intervals at the measured 2 Hz, so an ordinary
 * dropout never flickers the whole top bar into alarm.
 */
export const LINK_STALE_MS = 3000

/**
 * Launch site / ground-station reference — MRCC 2026 Zon Tengah.
 *
 * The competition base is ILD UiTM (Jalan Tanjung Tualang, Kg. Gajah) at
 * 4.2488, 101.0361, but launches happen ~19 km SW of it (a ~30 min drive) in
 * the FELCRA Seberang Perak paddy scheme — this constant is the PAD, not the
 * base, because the map centres on it and the recovery range/bearing readout
 * measures from it.
 *
 * PLACEHOLDER PRECISION: this is the scheme's reference point, good to ~1 km.
 * Replace it with the surveyed pad coordinate on site — every range/bearing the
 * recovery team walks is measured from here. Keep it inside the offline basemap
 * extract (see EXTRACT_BOUNDS in FlightMap.tsx).
 */
export const LAUNCH_SITE = { lat: 4.0986, lon: 100.9505 } as const

/**
 * Decoded telemetry frame in the units the UI reasons about (engineering units,
 * camelCase). One of these is produced per received packet, real or mocked.
 */
export interface TelemetryFrame {
  /** laptop receive time, epoch ms (backend host_time). */
  hostTime: number
  seq: number
  flightState: FlightState
  onboardMs: number
  baroAltM: number
  /** vertical speed, m/s (packet ships dm/s). */
  vspeedMs: number
  gpsLat: number
  gpsLon: number
  gpsAltM: number
  gpsSats: number
  gpsFix: GpsFix
  tiltDeg: number
  /**
   * Battery volts (packet ships ×10), or null when the downlink carries no
   * battery reading at all. Null and 0.0 are NOT the same thing — one is "we
   * don't know", the other is "the pack is flat".
   */
  vbatV: number | null
  continuity: boolean
  pyroFired: boolean
  sdOk: boolean
  armed: boolean
  /** Which of the four flags above the frame actually reports — see `Flag`. */
  flagsKnown: number
  /** Raw HEALTH_* bitfield — one bit per peripheral. */
  health: number
  /**
   * Which health bits the frame actually speaks to — same bit layout as
   * `health`. Zero when it carried no health information at all (pre-health
   * firmware, or an old raw.log replay); a partial mask when the source can
   * only see some peripherals (the MRCC text downlink infers BARO/IMU/GPS and
   * has nothing to say about SD/PYRO/VBAT). Rows outside the mask render as
   * unknown, because inventing six green lights and inventing six red alarms
   * are equally wrong.
   */
  healthKnown: number
  /** Per-packet radio quality (SX1278 reports these), null if unmeasured. */
  rssiDbm: number | null
  snrDb: number | null
  /**
   * Decoded fields the protocol has no slot for — pressure, heading, ground
   * speed, course, horizontal accel/velocity. Keys are whatever the source
   * produced. Shown verbatim so a downlink that carries more than PROTOCOL.md
   * defines doesn't have to throw the surplus away.
   */
  extra: Record<string, number>
}

/**
 * Wire shape as broadcast by the backend — mirrors packet.CSV_COLUMNS
 * (snake_case, engineering units). Fields are permissive because the backend
 * window isn't built yet: flight_state may arrive as an int or its name,
 * booleans as bool or 0/1, host_time as epoch ms or an ISO string. The
 * normalizer below tolerates all of these so we don't hard-couple to a shape
 * that isn't finalized.
 */
export interface WireFrame {
  host_time?: number | string
  seq: number
  flight_state: number | string
  onboard_ms: number
  baro_alt_m: number
  vspeed_ms: number
  gps_lat: number
  gps_lon: number
  gps_alt_m: number
  gps_sats: number
  gps_fix: number
  tilt_deg: number
  /** null when the source carries no battery reading. */
  vbat_v: number | null
  continuity: boolean | number
  pyro_fired: boolean | number
  sd_ok: boolean | number
  armed: boolean | number
  /**
   * Mask of the flags this frame reports. Absent means "all four", so an older
   * backend that doesn't send the field keeps behaving exactly as before.
   */
  flags_known?: number
  /**
   * Peripheral health byte. Optional: a pre-health firmware build (or a replay
   * of an old raw.log) simply omits it. Absent is treated as "unknown", NOT as
   * "everything failed" — see normalizeFrame.
   */
  health?: number
  /**
   * Mask of the bits in `health` that the source actually reports. Absent means
   * "all of them", which is what a binary frame always carries — so an older
   * backend that doesn't send this field keeps behaving exactly as before.
   */
  health_known?: number
  rssi_dbm?: number | null
  snr_db?: number | null
  extra?: Record<string, number>
}

const NAME_TO_STATE: Record<string, FlightState> = Object.fromEntries(
  Object.entries(FLIGHT_STATE_NAME).map(([value, name]) => [name, Number(value) as FlightState]),
)

function coerceState(raw: number | string): FlightState {
  if (typeof raw === "number") return raw as FlightState
  const byName = NAME_TO_STATE[raw.toUpperCase()]
  return byName ?? FlightState.PAD
}

function coerceBool(raw: boolean | number | undefined): boolean {
  return raw === true || raw === 1
}

function coerceHostTime(raw: number | string | undefined): number {
  if (typeof raw === "number") return raw
  if (typeof raw === "string") {
    const parsed = Date.parse(raw)
    if (!Number.isNaN(parsed)) return parsed
  }
  return Date.now()
}

/** Coerce a backend wire frame into the normalized UI frame. */
export function normalizeFrame(w: WireFrame): TelemetryFrame {
  return {
    hostTime: coerceHostTime(w.host_time),
    seq: w.seq,
    flightState: coerceState(w.flight_state),
    onboardMs: w.onboard_ms,
    baroAltM: w.baro_alt_m,
    vspeedMs: w.vspeed_ms,
    gpsLat: w.gps_lat,
    gpsLon: w.gps_lon,
    gpsAltM: w.gps_alt_m,
    gpsSats: w.gps_sats,
    gpsFix: w.gps_fix as GpsFix,
    tiltDeg: w.tilt_deg,
    vbatV: typeof w.vbat_v === "number" ? w.vbat_v : null,
    continuity: coerceBool(w.continuity),
    pyroFired: coerceBool(w.pyro_fired),
    sdOk: coerceBool(w.sd_ok),
    armed: coerceBool(w.armed),
    flagsKnown: typeof w.flags_known === "number" ? w.flags_known : FLAGS_ALL,
    health: typeof w.health === "number" ? w.health : HEALTH_ALL_OK,
    // No health byte -> nothing is known. A health byte with no accompanying
    // mask -> all six are known, which is the binary protocol's guarantee.
    healthKnown:
      typeof w.health !== "number"
        ? 0
        : typeof w.health_known === "number"
          ? w.health_known
          : HEALTH_ALL_OK,
    rssiDbm: typeof w.rssi_dbm === "number" ? w.rssi_dbm : null,
    snrDb: typeof w.snr_db === "number" ? w.snr_db : null,
    extra: w.extra ?? {},
  }
}

/** True if the peripheral is initialised and responding. */
export function isHealthy(frame: TelemetryFrame, mask: number): boolean {
  return (frame.health & mask) === mask
}

/** Whether this frame says anything at all about the given peripheral. */
export function isKnown(frame: TelemetryFrame | null, mask: number): boolean {
  return !!frame && (frame.healthKnown & mask) === mask
}

/** Whether this frame reports the given flag at all (vs. simply not carrying it). */
export function isFlagKnown(frame: TelemetryFrame | null, mask: number): boolean {
  return !!frame && (frame.flagsKnown & mask) === mask
}

/**
 * Names of every peripheral currently down — the thing to show the operator
 * instead of a single failure boolean. Empty array = all nominal.
 *
 * A peripheral the frame says nothing about is NOT down; it is unknown, and
 * naming it here would put a fabricated failure in front of the operator.
 */
export function failedSubsystems(frame: TelemetryFrame | null): string[] {
  if (!frame) return []
  return SUBSYSTEMS.filter((s) => isKnown(frame, s.mask) && !(frame.health & s.mask))
    .map((s) => s.name)
}

/** True once the vehicle has left the pad (T+ clock runs from the first BOOST). */
export function hasLaunched(state: FlightState): boolean {
  return state !== FlightState.PAD
}

/** Descent phases — used by the no-deploy alarm and tilt emphasis. */
export function isDescending(state: FlightState): boolean {
  return state === FlightState.DROGUE || state === FlightState.MAIN
}
