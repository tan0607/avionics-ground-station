/**
 * Geodesy for the recovery readout — distance + bearing from the ground station
 * to the vehicle's last known GPS fix (GROUND_STATION_PLAN §5, panel 2).
 *
 * Scale note: a launch site fits inside a ~20 km box, so a spherical-earth
 * haversine is exact to well under a metre here — ellipsoidal (Vincenty)
 * precision buys nothing at this range and adds iteration + failure modes.
 *
 * Bearing is TRUE north (°T). If you navigate with a handheld magnetic compass,
 * apply the local declination (Perak Tengah ≈ 0° in 2026, so true ≈ magnetic here):
 * magnetic = true − declinationEast.
 */

export interface LatLon {
  lat: number
  lon: number
}

/** IUGG mean Earth radius, metres. */
const R_EARTH_M = 6_371_008.8

const toRad = (deg: number) => (deg * Math.PI) / 180
const toDeg = (rad: number) => (rad * 180) / Math.PI

/** Great-circle distance between two points, in metres. */
export function haversineMeters(a: LatLon, b: LatLon): number {
  const lat1 = toRad(a.lat)
  const lat2 = toRad(b.lat)
  const dLat = toRad(b.lat - a.lat)
  const dLon = toRad(b.lon - a.lon)
  const h =
    Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2
  return 2 * R_EARTH_M * Math.asin(Math.min(1, Math.sqrt(h)))
}

/** Initial great-circle bearing a→b, degrees clockwise from TRUE north, 0..360. */
export function bearingDeg(a: LatLon, b: LatLon): number {
  const lat1 = toRad(a.lat)
  const lat2 = toRad(b.lat)
  const dLon = toRad(b.lon - a.lon)
  const y = Math.sin(dLon) * Math.cos(lat2)
  const x = Math.cos(lat1) * Math.sin(lat2) - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon)
  return (toDeg(Math.atan2(y, x)) + 360) % 360
}

const COMPASS_16 = [
  "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
] as const

/** 16-point compass abbreviation for a bearing (0..360). */
export function compass16(bearing: number): string {
  return COMPASS_16[Math.round(bearing / 22.5) % 16]
}

/** Zero-padded whole-degree bearing, e.g. 41 → "041". */
export function formatBearing(bearing: number): string {
  return String(Math.round(bearing) % 360).padStart(3, "0")
}

/**
 * Human distance readout. Sub-kilometre in whole metres (what a recovery walk
 * cares about); kilometres above that, with the fraction tightening as range
 * grows so the digit count stays glanceable.
 */
export function formatDistance(meters: number): { value: string; unit: string } {
  if (!Number.isFinite(meters)) return { value: "—", unit: "" }
  if (meters < 1000) return { value: meters.toFixed(0), unit: "m" }
  return { value: (meters / 1000).toFixed(meters < 10_000 ? 2 : 1), unit: "km" }
}
