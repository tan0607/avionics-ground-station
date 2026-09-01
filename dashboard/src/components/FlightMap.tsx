/**
 * FlightMap — the offline GPS panel (GROUND_STATION_PLAN §5, panel 2).
 *
 * A fully self-contained, network-free map: the basemap is a local Protomaps
 * PMTiles archive (public/basemap.pmtiles, cut from a Protomaps daily build with
 * go-pmtiles — vector tiles, no raster scraping), rendered by protomaps-leaflet.
 * On top we draw the live ground track, the ground-station reference, and the
 * vehicle's current position; the overlay shows the LAST KNOWN COORDINATE big
 * (recovery hinges on this) plus range + bearing from the GS.
 *
 * Everything renders from the local .pmtiles + the telemetry stream — with the
 * network fully disabled nothing here changes. Data comes from useTelemetry's
 * frame; the lat/lon history is accumulated by useGroundTrack without touching
 * the data layer.
 */
import { useEffect, useRef, useState } from "react"
import L from "leaflet"
import "leaflet/dist/leaflet.css"
import { leafletLayer } from "protomaps-leaflet"
import "./FlightMap.css"
import { Crosshair } from "lucide-react"
import {
  GpsFix,
  LAUNCH_SITE,
  FLIGHT_STATE_NAME,
  type TelemetryFrame,
} from "@/lib/protocol"
import type { LinkState } from "@/hooks/useTelemetry"
import { useGroundTrack } from "@/hooks/useGroundTrack"
import { bearingDeg, compass16, formatBearing, formatDistance, haversineMeters } from "@/lib/geo"
import { cn } from "@/lib/utils"

/**
 * The area covered by the offline extract (bbox from the go-pmtiles cut) —
 * Perak Tengah: the FELCRA Seberang Perak paddy scheme (the pad) up through
 * Kg. Gajah to ILD UiTM / Tanjung Tualang (the competition base), so the whole
 * drive between base and pad stays on-map. Re-cut with:
 *
 *   pmtiles extract https://build.protomaps.com/<YYYYMMDD>.pmtiles \
 *     dashboard/public/basemap.pmtiles --bbox=100.82,3.96,101.14,4.36 --maxzoom=15
 *
 * Keep these bounds and that --bbox identical, or the map lets you pan into
 * blank space that has no tiles behind it.
 */
const EXTRACT_BOUNDS = L.latLngBounds([3.96, 100.82], [4.36, 101.14])
const BASEMAP_URL = `${import.meta.env.BASE_URL}basemap.pmtiles`
const PMTILES_MAX_ZOOM = 15 // source (Protomaps planet) max; overzoomed above this

/** Concrete marker colors (canvas can't read CSS vars) — mirror the console tokens. */
const MARKER_COLOR: Record<LinkState, string> = {
  live: "#f5f5f5", // --data (neutral white)
  stale: "#e0a92e", // --caution (amber)
  down: "#e2503b", // --alarm (red)
}
const TRACK_COLOR = "rgba(245, 245, 245, 0.5)" // faint white — the path is context, not the hero

const gsIcon = L.divIcon({
  className: "gs-marker",
  html: `<svg width="22" height="22" viewBox="0 0 22 22" fill="none" stroke="currentColor" stroke-width="1.4">
    <circle cx="11" cy="11" r="5.5"/>
    <line x1="11" y1="0.5" x2="11" y2="4"/><line x1="11" y1="18" x2="11" y2="21.5"/>
    <line x1="0.5" y1="11" x2="4" y2="11"/><line x1="18" y1="11" x2="21.5" y2="11"/>
  </svg>`,
  iconSize: [22, 22],
  iconAnchor: [11, 11],
})

interface FlightMapProps {
  frame: TelemetryFrame | null
  link: LinkState
}

export function FlightMap({ frame, link }: FlightMapProps) {
  const containerRef = useRef<HTMLDivElement | null>(null)
  const mapRef = useRef<L.Map | null>(null)
  const trackLineRef = useRef<L.Polyline | null>(null)
  const vehicleRef = useRef<L.CircleMarker | null>(null)
  const vehicleRingRef = useRef<L.CircleMarker | null>(null)
  const followRef = useRef(true)

  const track = useGroundTrack(frame)

  // a slow tick so the "N s ago" age advances even when packets stop
  const [, setNow] = useState(0)
  useEffect(() => {
    const id = setInterval(() => setNow((n) => n + 1), 250)
    return () => clearInterval(id)
  }, [])

  // --- map lifecycle: create once, tear down on unmount ---------------------
  useEffect(() => {
    if (!containerRef.current) return

    const map = L.map(containerRef.current, {
      center: [LAUNCH_SITE.lat, LAUNCH_SITE.lon],
      zoom: 14,
      minZoom: 11,
      maxZoom: 18,
      zoomControl: false, // re-added bottom-left so it clears the fix readout
      attributionControl: true,
      maxBounds: EXTRACT_BOUNDS.pad(0.15),
      maxBoundsViscosity: 0.85,
    })
    mapRef.current = map
    L.control.zoom({ position: "bottomleft" }).addTo(map)

    leafletLayer({
      url: BASEMAP_URL,
      theme: "black",
      backgroundColor: "#0a0a0a",
      maxDataZoom: PMTILES_MAX_ZOOM,
      attribution:
        '© <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noreferrer">OpenStreetMap</a> · <a href="https://protomaps.com" target="_blank" rel="noreferrer">Protomaps</a>',
    }).addTo(map)

    L.marker([LAUNCH_SITE.lat, LAUNCH_SITE.lon], { icon: gsIcon, interactive: false, keyboard: false })
      .bindTooltip("GS", { permanent: true, direction: "right", offset: [8, 0] })
      .addTo(map)

    trackLineRef.current = L.polyline([], { color: TRACK_COLOR, weight: 2, interactive: false }).addTo(map)

    // manual pan/drag hands control back to the operator (stop auto-follow)
    map.on("dragstart", () => {
      followRef.current = false
    })

    // container starts at flex size; make sure Leaflet measures it correctly
    requestAnimationFrame(() => map.invalidateSize())

    return () => {
      map.remove()
      mapRef.current = null
      trackLineRef.current = null
      vehicleRef.current = null
      vehicleRingRef.current = null
    }
  }, [])

  // --- redraw track + vehicle when a fix arrives ----------------------------
  useEffect(() => {
    const map = mapRef.current
    const line = trackLineRef.current
    if (!map || !line) return

    line.setLatLngs(track.points.map((p) => [p.lat, p.lon] as [number, number]))

    const last = track.last
    if (!last) return
    const here: [number, number] = [last.lat, last.lon]
    const color = MARKER_COLOR[link]

    if (!vehicleRef.current) {
      vehicleRingRef.current = L.circleMarker(here, {
        radius: 9,
        color,
        weight: 1,
        opacity: 0.5,
        fill: false,
        interactive: false,
      }).addTo(map)
      vehicleRef.current = L.circleMarker(here, {
        radius: 4.5,
        color: "#0a0a0a",
        weight: 1.5,
        fillColor: color,
        fillOpacity: 1,
        interactive: false,
      }).addTo(map)
    } else {
      vehicleRef.current.setLatLng(here).setStyle({ fillColor: color })
      vehicleRingRef.current?.setLatLng(here).setStyle({ color })
    }

    // gentle follow: only nudge the view if the vehicle drifts near the edge
    if (followRef.current) map.panInside(here, { padding: [48, 48], animate: true })
    // track.points is a stable, mutated-in-place array (ref+rev pattern, like
    // useTelemetry's chart); `track.rev` is the redraw signal, not track.points.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [track.rev, track.last, link])

  const recenter = () => {
    const map = mapRef.current
    if (!map) return
    followRef.current = true
    const latlngs = track.points.map((p) => [p.lat, p.lon] as [number, number])
    map.fitBounds(L.latLngBounds([[LAUNCH_SITE.lat, LAUNCH_SITE.lon], ...latlngs]).pad(0.25), {
      maxZoom: 16,
    })
  }

  return (
    <div className="flightmap relative flex h-full min-h-0 w-full flex-col overflow-hidden rounded-md border border-hairline">
      <div ref={containerRef} className="min-h-0 flex-1" />
      <FixReadout frame={frame} link={link} track={track} />
      <button
        type="button"
        onClick={recenter}
        className="absolute right-2 top-2 z-[500] flex items-center gap-1.5 rounded-sm border border-hairline bg-surface px-2 py-1 text-[0.625rem] uppercase tracking-[0.12em] text-ink-dim transition-colors hover:text-ink"
      >
        <Crosshair aria-hidden strokeWidth={1.75} className="size-3" />
        Recenter
      </button>
    </div>
  )
}

// --- the recovery readout: last known coordinate BIG + range/bearing ---------

const LINK_ACCENT: Record<LinkState, string> = {
  live: "text-ink-mute",
  stale: "text-caution",
  down: "text-alarm",
}
const LINK_DOT: Record<LinkState, string> = {
  live: "bg-data",
  stale: "bg-caution",
  down: "bg-alarm",
}

function FixReadout({
  frame,
  link,
  track,
}: {
  frame: TelemetryFrame | null
  link: LinkState
  track: ReturnType<typeof useGroundTrack>
}) {
  const last = track.last
  const ageS = track.lastFixAt ? (Date.now() - track.lastFixAt) / 1000 : null
  const fixName = frame ? (frame.gpsFix === GpsFix.FIX_3D ? "3D" : frame.gpsFix === GpsFix.FIX_2D ? "2D" : "—") : "—"

  return (
    <div className="pointer-events-none absolute left-2 top-2 z-[500] w-[15.5rem] rounded-md border border-hairline bg-surface">
      <div className="flex items-center justify-between border-b border-hairline px-3 py-1.5">
        <span className="flex items-center gap-2 text-[0.625rem] uppercase tracking-[0.14em] text-ink-mute">
          <span className={cn("size-1.5 rounded-full", LINK_DOT[link])} aria-hidden />
          Last Known Fix
        </span>
        <span className="tnum text-[0.625rem] text-ink-mute">
          {fixName}
          {frame && frame.gpsFix >= GpsFix.FIX_2D ? ` · ${frame.gpsSats} sat` : ""}
        </span>
      </div>

      <div className="px-3 py-2">
        {last ? (
          <>
            <div className="tnum text-[1.55rem] font-medium leading-[1.15] text-data">
              {Math.abs(last.lat).toFixed(6)}
              <span className="text-sm text-ink-mute">°{last.lat >= 0 ? "N" : "S"}</span>
            </div>
            <div className="tnum text-[1.55rem] font-medium leading-[1.15] text-data">
              {Math.abs(last.lon).toFixed(6)}
              <span className="text-sm text-ink-mute">°{last.lon >= 0 ? "E" : "W"}</span>
            </div>

            <div className="mt-2 flex items-baseline gap-3 border-t border-hairline pt-2">
              <RangeBearing last={last} />
            </div>
            <div className="mt-1 flex items-center justify-between text-[0.6875rem] text-ink-mute">
              <span className="uppercase tracking-[0.1em]">
                {frame ? FLIGHT_STATE_NAME[frame.flightState] : "—"}
              </span>
              <span className={cn("tnum", ageS != null && link !== "live" && LINK_ACCENT[link])}>
                {ageS == null ? "no fix yet" : `${ageS.toFixed(1)} s ago`}
              </span>
            </div>
          </>
        ) : (
          <div className="py-1">
            <div className="text-lg font-medium text-ink-dim">NO GPS FIX</div>
            <div className="mt-0.5 text-[0.6875rem] uppercase tracking-[0.1em] text-ink-mute">
              awaiting 3D lock
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

function RangeBearing({ last }: { last: { lat: number; lon: number } }) {
  const dist = haversineMeters(LAUNCH_SITE, last)
  const brg = bearingDeg(LAUNCH_SITE, last)
  const d = formatDistance(dist)
  return (
    <>
      <span className="tnum text-lg leading-none text-data">
        {d.value}
        <span className="ml-0.5 text-xs text-ink-mute">{d.unit}</span>
      </span>
      <span className="tnum text-sm leading-none text-ink-dim">
        {formatBearing(brg)}°<span className="ml-1 text-ink-mute">{compass16(brg)}</span>
      </span>
      <span className="ml-auto text-[0.5625rem] uppercase tracking-[0.12em] text-ink-mute">from GS</span>
    </>
  )
}
