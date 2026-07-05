/**
 * StatusBar — the top strip: flight-state pill · T+ mission clock · link
 * freshness · packet loss · source tag. It is the calm-until-alarm surface:
 * normally near-black with quiet ink, but the WHOLE bar flips to the alarm
 * wash when the link goes stale or a no-deploy is detected, so a lost vehicle
 * is impossible to miss from across the launch site (PRODUCT.md §6).
 */
import { fmtLinkAge, fmtPercent, fmtTimer } from "@/lib/format"
import { FLIGHT_STATE_NAME, FlightState } from "@/lib/protocol"
import type { LinkState, TelemetryState } from "@/hooks/useTelemetry"

/** Flight-state → semantic tone. Ascent = data cyan, deploys = caution amber,
 *  main/landed = nominal green, pad = quiet ink. */
const STATE_TONE: Record<FlightState, string> = {
  [FlightState.PAD]: "text-ink-dim",
  [FlightState.BOOST]: "text-caution",
  [FlightState.COAST]: "text-data",
  [FlightState.APOGEE]: "text-caution",
  [FlightState.DROGUE]: "text-caution",
  [FlightState.MAIN]: "text-nominal",
  [FlightState.LANDED]: "text-nominal",
}

const LINK_DOT: Record<LinkState, string> = {
  live: "bg-nominal",
  stale: "bg-caution",
  down: "bg-alarm",
}

const LINK_LABEL: Record<LinkState, string> = {
  live: "LINK",
  stale: "STALE",
  down: "NO LINK",
}

function Segment({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-baseline gap-2 px-4">
      <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">
        {label}
      </span>
      {children}
    </div>
  )
}

export function StatusBar({ state }: { state: TelemetryState }) {
  const { frame, link, linkAgeMs, tPlusSec, lossFraction, alarms, source } = state
  const flightState = frame?.flightState ?? FlightState.PAD
  const alarmed = alarms.linkStale || alarms.noDeploy

  return (
    <header
      role="banner"
      className={`flex h-11 items-stretch divide-x divide-hairline border-b border-hairline transition-colors ${
        alarmed ? "bg-alarm-bg" : "bg-surface"
      }`}
    >
      {/* flight state — the loudest label on the bar */}
      <div className="flex items-center px-4">
        <span
          className={`text-sm font-medium tracking-[0.18em] ${STATE_TONE[flightState]}`}
        >
          {FLIGHT_STATE_NAME[flightState]}
        </span>
      </div>

      <div className="flex items-center">
        <Segment label="T">
          <span className="tnum text-sm text-ink tabular-nums">{fmtTimer(tPlusSec)}</span>
        </Segment>
      </div>

      <div className="flex items-center">
        <Segment label={LINK_LABEL[link]}>
          <span className="flex items-center gap-2">
            <span aria-hidden className={`size-2 rounded-full ${LINK_DOT[link]}`} />
            <span className="tnum text-sm text-ink-dim tabular-nums">
              {fmtLinkAge(linkAgeMs)}
            </span>
          </span>
        </Segment>
      </div>

      <div className="flex items-center">
        <Segment label="Loss">
          <span
            className={`tnum text-sm tabular-nums ${
              lossFraction > 0.1 ? "text-alarm" : lossFraction > 0.03 ? "text-caution" : "text-ink-dim"
            }`}
          >
            {fmtPercent(lossFraction, 1)}
          </span>
        </Segment>
      </div>

      {/* source tag pushed to the right — MOCK must never be mistaken for live */}
      <div className="ml-auto flex items-center px-4">
        <span
          className={`text-[0.625rem] uppercase tracking-[0.16em] ${
            source === "mock" ? "text-caution" : "text-ink-mute"
          }`}
        >
          <span aria-hidden>{source === "mock" ? "◆ " : "● "}</span>
          {source === "mock" ? "MOCK" : "LIVE"}
        </span>
      </div>
    </header>
  )
}
