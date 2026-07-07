/**
 * useGroundTrack — accumulates the vehicle's GPS ground track from the stream of
 * decoded frames, without touching the telemetry data layer.
 *
 * useTelemetry deliberately keeps only the altitude series; the map needs the
 * lat/lon history too. Rather than widen that hook (owned by another window),
 * this one piggybacks on its published `frame`: high-rate points live in a ref
 * (never reconciled), a `rev` counter signals Leaflet to redraw, and a compact
 * immutable summary is republished per fix. Same ref+rev discipline as the
 * altitude chart (see useTelemetry's header).
 */
import { useEffect, useRef, useState } from "react"
import { GpsFix, type TelemetryFrame } from "@/lib/protocol"

export interface TrackPoint {
  lat: number
  lon: number
  seq: number
}

export interface GroundTrack {
  /** Chronological fixes, mutated in place; read alongside `rev`. */
  points: TrackPoint[]
  /** Bumps whenever `points` changes — lets the map diff without deep compare. */
  rev: number
  /** Most recent valid fix, or null before the first one. */
  last: TrackPoint | null
  /** Wall-clock (epoch ms) the last fix arrived — drives the "N s ago" age. */
  lastFixAt: number | null
}

/** onboard clock jumping back = a fresh flight/session (matches useTelemetry). */
const SESSION_RESET_MS = 1500
/** Safety cap; a real flight is a few hundred fixes. */
const MAX_POINTS = 4000

const EMPTY: GroundTrack = { points: [], rev: 0, last: null, lastFixAt: null }

export function useGroundTrack(frame: TelemetryFrame | null): GroundTrack {
  const pointsRef = useRef<TrackPoint[]>([])
  const revRef = useRef(0)
  const lastSeqRef = useRef<number | null>(null)
  const lastOnboardRef = useRef<number>(0)
  const [track, setTrack] = useState<GroundTrack>(EMPTY)

  useEffect(() => {
    if (!frame) return

    // New flight: the onboard clock jumped backwards (mock loop / real reboot).
    // Drop the old track so we don't draw a line from landing back to the pad.
    if (frame.onboardMs + SESSION_RESET_MS < lastOnboardRef.current) {
      pointsRef.current = []
      lastSeqRef.current = null
    }
    lastOnboardRef.current = frame.onboardMs

    const hasFix =
      frame.gpsFix >= GpsFix.FIX_2D && (frame.gpsLat !== 0 || frame.gpsLon !== 0)
    const isNewPacket = frame.seq !== lastSeqRef.current
    if (!hasFix || !isNewPacket) return

    lastSeqRef.current = frame.seq
    const point: TrackPoint = { lat: frame.gpsLat, lon: frame.gpsLon, seq: frame.seq }
    pointsRef.current.push(point)
    if (pointsRef.current.length > MAX_POINTS) pointsRef.current.shift()
    revRef.current += 1

    setTrack({
      points: pointsRef.current,
      rev: revRef.current,
      last: point,
      lastFixAt: Date.now(),
    })
  }, [frame])

  return track
}
