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
 * blanket "AV FAILED". See firmware/lib/Subsystem.
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

/** Nominal telemetry rate — 4 Hz (see PROTOCOL.md). Drives the mock + link math. */
export const PACKET_HZ = 4
export const PACKET_INTERVAL_MS = 1000 / PACKET_HZ

/** Link is considered stale after this many ms with no packet (GS plan §5, DESIGN §3). */
export const LINK_STALE_MS = 3000

/** Launch site (fake_telemetry.py / DESIGN_SPECS): Selangor, MY. */
export const LAUNCH_SITE = { lat: 3.2437, lon: 101.7061 } as const

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
  /** battery volts (packet ships ×10). */
  vbatV: number
  continuity: boolean
  pyroFired: boolean
  sdOk: boolean
  armed: boolean
  /** Raw HEALTH_* bitfield — one bit per peripheral. */
  health: number
  /**
   * False when the frame carried no health byte at all (pre-health firmware, or
   * an old raw.log replay). The panel then shows every row as unknown rather
   * than inventing six green lights or six red alarms.
   */
  healthKnown: boolean
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
  vbat_v: number
  continuity: boolean | number
  pyro_fired: boolean | number
  sd_ok: boolean | number
  armed: boolean | number
  /**
   * Peripheral health byte. Optional: a pre-health firmware build (or a replay
   * of an old raw.log) simply omits it. Absent is treated as "unknown", NOT as
   * "everything failed" — see coerceHealth.
   */
  health?: number
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
    vbatV: w.vbat_v,
    continuity: coerceBool(w.continuity),
    pyroFired: coerceBool(w.pyro_fired),
    sdOk: coerceBool(w.sd_ok),
    armed: coerceBool(w.armed),
    health: typeof w.health === "number" ? w.health : HEALTH_ALL_OK,
    healthKnown: typeof w.health === "number",
  }
}

/** True if the peripheral is initialised and responding. */
export function isHealthy(frame: TelemetryFrame, mask: number): boolean {
  return (frame.health & mask) === mask
}

/**
 * Names of every peripheral currently down — the thing to show the operator
 * instead of a single failure boolean. Empty array = all nominal.
 */
export function failedSubsystems(frame: TelemetryFrame | null): string[] {
  if (!frame || !frame.healthKnown) return []
  return SUBSYSTEMS.filter((s) => !(frame.health & s.mask)).map((s) => s.name)
}

/** True once the vehicle has left the pad (T+ clock runs from the first BOOST). */
export function hasLaunched(state: FlightState): boolean {
  return state !== FlightState.PAD
}

/** Descent phases — used by the no-deploy alarm and tilt emphasis. */
export function isDescending(state: FlightState): boolean {
  return state === FlightState.DROGUE || state === FlightState.MAIN
}
