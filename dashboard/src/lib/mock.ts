/**
 * Mock telemetry source. Simulates one full flight and emits WireFrame objects
 * (snake_case, engineering units — same shape the backend will broadcast) at
 * PACKET_HZ, so the whole dashboard runs with no hardware and no backend.
 *
 * The profile mirrors shared/fake_telemetry.py qualitatively: pad hold →
 * boost → coast to ~1390 m apogee → drogue → main → landed, with downrange
 * wind drift, tilt growth on descent, a slow Vbat droop, a monotonic seq
 * counter, PYRO_FIRED set at deploys, and a small random packet-loss rate so
 * the link stats show something real. It loops so a dev session keeps streaming.
 */
import {
  FlightState,
  LAUNCH_SITE,
  PACKET_INTERVAL_MS,
  type WireFrame,
} from "./protocol"

const G = 9.81
const DT = PACKET_INTERVAL_MS / 1000 // 0.25 s per 4 Hz step

// --- tunables (chosen so apogee lands near ~1390 m) ---
const PAD_HOLD_S = 5
const BURN_S = 2.6
const BOOST_NET_A = 60 // m/s^2 net accel during burn
const COAST_DRAG_K = 0.00015 // quadratic drag so apogee ≈ 1385 m (vs ~1443 drag-free)
const DROGUE_TERMINAL = -26 // m/s
const MAIN_TERMINAL = -6.5 // m/s
const MAIN_DEPLOY_ALT = 260 // m AGL
const LANDED_HOLD_S = 8
const DROP_RATE = 0.02 // ~2% simulated packet loss

const METERS_PER_DEG_LAT = 111_320
const metersPerDegLon = (lat: number) => 111_320 * Math.cos((lat * Math.PI) / 180)

/** Box–Muller gaussian for light sensor noise. */
function gaussian(sigma: number): number {
  const u = 1 - Math.random()
  const v = Math.random()
  return sigma * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v)
}

function flagsToBits(state: {
  continuity: boolean
  pyroFired: boolean
  sdOk: boolean
  armed: boolean
}) {
  return {
    continuity: state.continuity,
    pyro_fired: state.pyroFired,
    sd_ok: state.sdOk,
    armed: state.armed,
  }
}

export class MockFlightSim {
  private phase: FlightState = FlightState.PAD
  private phaseT = 0 // seconds in current phase
  private missionT = 0 // seconds since boot
  private h = 0 // altitude AGL, m
  private v = 0 // vertical speed, m/s
  private tilt = 1
  private vbat = 8.0
  private seq = 0
  private pyroFired = false
  private lat = LAUNCH_SITE.lat
  private lon = LAUNCH_SITE.lon
  // steady downrange wind, m/s (drifts the vehicle NE while aloft)
  private windE = 4.5
  private windN = 2.0

  private readonly dropRate: number

  constructor(dropRate = DROP_RATE) {
    this.dropRate = dropRate
  }

  /** Advance one 4 Hz tick. Returns a WireFrame, or null for a dropped packet. */
  step(): WireFrame | null {
    this.advance(DT)
    this.seq = (this.seq + 1) & 0xffff
    // A dropped packet still burns a seq number — that's how loss is detectable.
    if (Math.random() < this.dropRate) return null
    return this.frame()
  }

  private advance(dt: number) {
    this.missionT += dt
    this.phaseT += dt
    this.vbat = Math.max(7.4, this.vbat - 0.0009 * dt * (this.phase === FlightState.PAD ? 0.3 : 1))

    switch (this.phase) {
      case FlightState.PAD:
        this.h = 0
        this.v = 0
        this.tilt = 1
        if (this.phaseT >= PAD_HOLD_S) this.enter(FlightState.BOOST)
        break

      case FlightState.BOOST: {
        this.v += BOOST_NET_A * dt
        this.h += this.v * dt
        this.tilt = 2 + gaussian(0.6)
        if (this.phaseT >= BURN_S) this.enter(FlightState.COAST)
        break
      }

      case FlightState.COAST: {
        const drag = COAST_DRAG_K * this.v * Math.abs(this.v)
        this.v += (-G - drag) * dt
        this.h += this.v * dt
        this.tilt = 3 + gaussian(0.8)
        if (this.v <= 0) this.enter(FlightState.APOGEE)
        break
      }

      case FlightState.APOGEE:
        // momentary peak, then drogue out
        this.v += -G * dt
        this.h += this.v * dt
        this.pyroFired = true
        if (this.phaseT >= 0.5) this.enter(FlightState.DROGUE)
        break

      case FlightState.DROGUE: {
        // relax toward drogue terminal velocity; tumble grows tilt
        this.v += (DROGUE_TERMINAL - this.v) * Math.min(1, 1.5 * dt)
        this.h = Math.max(0, this.h + this.v * dt)
        this.tilt = Math.min(55, this.tilt + 8 * dt + gaussian(1.2))
        if (this.h <= MAIN_DEPLOY_ALT) this.enter(FlightState.MAIN)
        break
      }

      case FlightState.MAIN: {
        this.v += (MAIN_TERMINAL - this.v) * Math.min(1, 2 * dt)
        this.h = Math.max(0, this.h + this.v * dt)
        this.tilt = Math.max(4, this.tilt - 10 * dt + gaussian(0.8))
        if (this.h <= 0) {
          this.h = 0
          this.v = 0
          this.enter(FlightState.LANDED)
        }
        break
      }

      case FlightState.LANDED:
        this.h = 0
        this.v = 0
        this.tilt = 88 + gaussian(1) // rocket lying on its side
        if (this.phaseT >= LANDED_HOLD_S) this.reset()
        break
    }

    // downrange drift only while airborne
    if (this.phase !== FlightState.PAD && this.phase !== FlightState.LANDED) {
      this.lat += (this.windN * dt) / METERS_PER_DEG_LAT
      this.lon += (this.windE * dt) / metersPerDegLon(this.lat)
    }
  }

  private enter(next: FlightState) {
    this.phase = next
    this.phaseT = 0
  }

  private reset() {
    this.phase = FlightState.PAD
    this.phaseT = 0
    this.missionT = 0
    this.h = 0
    this.v = 0
    this.tilt = 1
    this.pyroFired = false
    this.lat = LAUNCH_SITE.lat
    this.lon = LAUNCH_SITE.lon
    // seq + vbat intentionally persist across the loop (a fresh "flight",
    // same radio session) so link stats stay continuous.
  }

  private frame(): WireFrame {
    const airborne = this.phase !== FlightState.PAD
    const baro = Math.round(this.h + (airborne ? gaussian(1.2) : gaussian(0.3)))
    const vspeedMs = Math.round((this.v + gaussian(0.15)) * 10) / 10
    return {
      host_time: Date.now(),
      seq: this.seq,
      flight_state: this.phase,
      onboard_ms: Math.round(this.missionT * 1000),
      baro_alt_m: baro,
      vspeed_ms: vspeedMs,
      gps_lat: Math.round(this.lat * 1e7) / 1e7,
      gps_lon: Math.round(this.lon * 1e7) / 1e7,
      gps_alt_m: Math.round(this.h) + 30, // MSL ≈ AGL + pad elevation
      gps_sats: airborne ? 11 : 12,
      gps_fix: 3,
      tilt_deg: Math.max(0, Math.min(180, Math.round(this.tilt))),
      vbat_v: Math.round(this.vbat * 10) / 10,
      ...flagsToBits({
        continuity: !this.pyroFired,
        pyroFired: this.pyroFired,
        sdOk: true,
        armed: this.phase !== FlightState.LANDED,
      }),
    }
  }
}
