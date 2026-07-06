/**
 * TopBar — the console header strip: mission id · T+ mission clock · link pill ·
 * packet loss · source tag. Calm-until-alarm: normally near-black surface, but
 * the WHOLE bar flips to the alarm wash when the link goes stale or a no-deploy
 * is detected, so a lost vehicle is impossible to miss (PRODUCT.md §6).
 */
import { fmtLinkAge, fmtPercent, fmtTimer } from "@/lib/format"
import { cn } from "@/lib/utils"
import type { LinkState, TelemetryState, TelemetrySource } from "@/hooks/useTelemetry"

const MISSION = "APEX-1"

function LinkPill({ link, ageMs }: { link: LinkState; ageMs: number }) {
  const dot = link === "live" ? "bg-nominal" : link === "stale" ? "bg-caution" : "bg-alarm"
  const ink = link === "live" ? "text-nominal" : link === "stale" ? "text-caution" : "text-alarm"
  const label = link === "live" ? "LINK" : link === "stale" ? "STALE" : "NO LINK"
  return (
    <span className="flex items-center gap-2 rounded-sm border border-hairline px-2.5 py-1">
      <span aria-hidden className={`size-2 rounded-full ${dot}`} />
      <span className={`text-[0.6875rem] uppercase tracking-wide ${ink}`}>{label}</span>
      <span className="tnum text-[0.6875rem] tabular-nums text-ink-dim">{fmtLinkAge(ageMs)}</span>
    </span>
  )
}

function SourceTag({ source }: { source: TelemetrySource }) {
  const mock = source === "mock"
  return (
    <span
      className={cn(
        "text-[0.625rem] uppercase tracking-[0.16em]",
        mock ? "text-caution" : "text-ink-mute",
      )}
    >
      <span aria-hidden>{mock ? "◆ " : "● "}</span>
      {mock ? "MOCK" : "LIVE"}
    </span>
  )
}

function Segment({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-baseline gap-2 px-4">
      <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">{label}</span>
      {children}
    </div>
  )
}

export function TopBar({ state }: { state: TelemetryState }) {
  const { link, linkAgeMs, tPlusSec, lossFraction, alarms, source } = state
  const alarmed = alarms.linkStale || alarms.noDeploy
  const lossInk =
    lossFraction > 0.1 ? "text-alarm" : lossFraction > 0.03 ? "text-caution" : "text-ink-dim"

  return (
    <header
      role="banner"
      className={cn(
        "flex h-11 shrink-0 items-stretch divide-x divide-hairline border-b border-hairline transition-colors",
        alarmed ? "bg-alarm-bg" : "bg-surface",
      )}
    >
      <div className="flex items-center gap-2 px-4">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">Mission</span>
        <span className="text-sm font-medium tracking-wide text-ink">{MISSION}</span>
      </div>

      <div className="flex items-center px-4">
        <span className="tnum text-sm tabular-nums text-ink">{fmtTimer(tPlusSec)}</span>
      </div>

      <div className="flex items-center px-4">
        <LinkPill link={link} ageMs={linkAgeMs} />
      </div>

      <Segment label="Loss">
        <span className={`tnum text-sm tabular-nums ${lossInk}`}>{fmtPercent(lossFraction, 1)}</span>
      </Segment>

      <div className="ml-auto flex items-center px-4">
        <SourceTag source={source} />
      </div>
    </header>
  )
}
