/**
 * TopBar — the console header strip: mission picker · T+ mission clock · link pill ·
 * measured rate · packet loss · source tag. Calm-until-alarm: normally near-black surface, but
 * the WHOLE bar flips to the alarm wash when the link goes stale or a no-deploy
 * is detected, so a lost vehicle is impossible to miss (PRODUCT.md §6).
 */
import { useEffect, useRef, useState } from "react";
import { fmtDuration, fmtLinkAge, fmtPercent, fmtTimer } from "@/lib/format";
import { cn } from "@/lib/utils";
import type {
  LinkState,
  TelemetryState,
  TelemetrySource,
} from "@/hooks/useTelemetry";
import type { BackendSource } from "@/hooks/useBackendStats";
import {
  recordingElapsedSec,
  type RecorderState,
} from "@/hooks/useFlightRecorder";
import { defaultFlightLabel } from "@/lib/mission";
import type { MissionState } from "@/hooks/useMission";

/**
 * MissionSelect — the vehicle picker, which is also the channel switch.
 *
 * Changing it sends the receiver the channel that rocket flies on, so the
 * header cannot name one vehicle while the radio listens to the other. What it
 * CANNOT do is prove the box obeyed, and a receiver on the wrong channel is
 * silent rather than broken — so the marker beside it carries the box's own
 * word: caution while the announced channel still belongs to the other rocket,
 * dim while the box has not said anything at all. Both clear themselves the
 * moment the receiver announces the channel that was asked for.
 */
function MissionSelect({ mission }: { mission: MissionState }) {
  const { confirmed, pending, supported, busy } = mission;
  const note = pending && confirmed
    ? `box on ${confirmed?.name}`
    : supported && !confirmed
      ? "channel unheard"
      : null;

  return (
    <span className="flex items-baseline gap-2">
      <select
        value={mission.mission.name}
        onChange={(e) => mission.select(e.target.value, e.currentTarget)}
        disabled={busy}
        aria-label="Mission — selects the vehicle and retunes the receiver"
        title={
          supported
            ? "Mission — switching retunes the receiver to that rocket's channel"
            : "Mission — names the flight folder; no radio in this session to retune"
        }
        className={cn(
          "-mx-1 cursor-pointer rounded-sm border border-transparent bg-transparent px-1 py-0.5",
          "text-sm font-medium tracking-wide text-ink",
          "hover:border-hairline focus:border-ink-mute focus:outline-none",
          "disabled:cursor-not-allowed disabled:opacity-40",
        )}
      >
        {mission.missions.map((m) => (
          <option key={m.name} value={m.name} className="bg-surface text-ink">
            {m.name} · {m.channel}
          </option>
        ))}
      </select>
      {note && (
        <span
          title={
            pending
              ? `The receiver still says it is on channel ${confirmed?.channel} (${confirmed?.name}). Frames arriving now are that rocket's.`
              : "The receiver has not announced a channel yet — nothing confirms what it is listening to."
          }
          className={cn(
            "text-[0.625rem] uppercase tracking-[0.16em]",
            pending ? "text-caution" : "text-ink-mute",
          )}
        >
          <span aria-hidden>{pending ? "⚠ " : "◆ "}</span>
          {note}
        </span>
      )}
    </span>
  );
}

function LinkPill({ link, ageMs }: { link: LinkState; ageMs: number }) {
  const dot =
    link === "live"
      ? "bg-nominal"
      : link === "stale"
        ? "bg-caution"
        : "bg-alarm";
  const ink =
    link === "live"
      ? "text-nominal"
      : link === "stale"
        ? "text-caution"
        : "text-alarm";
  const label =
    link === "live" ? "LINK" : link === "stale" ? "STALE" : "NO LINK";
  return (
    <span className="flex items-center gap-2 rounded-sm border border-hairline px-2.5 py-1">
      <span aria-hidden className={`size-2 rounded-full ${dot}`} />
      <span className={`text-[0.6875rem] uppercase tracking-wide ${ink}`}>
        {label}
      </span>
      <span className="tnum text-[0.6875rem] tabular-nums text-ink-dim">
        {fmtLinkAge(ageMs)}
      </span>
    </span>
  );
}

/**
 * SourceTag — where these numbers actually came from.
 *
 * THIS TAG ONLY SAYS "LIVE" FOR A RADIO. It used to be derived from
 * `telemetry.source` alone, which distinguishes exactly two things: frames
 * generated in this browser ("mock") and frames arriving over the WebSocket
 * ("ws"). Everything in the second bucket was labelled LIVE — including a
 * backend running `--replay` on a recorded raw.log and a backend running
 * `--fake`. So a console replaying a two-minute capture on `--loop` presented
 * as a live vehicle: LINK LIVE, a running mission clock, a flight arc, events
 * firing, and none of it off the air. The operator's only clue was that the
 * numbers disagreed with the serial monitor.
 *
 * The backend already knew — /stats reports `source.kind` — so the browser asks
 * instead of assuming. Anything that is not a serial port is CAUTION-coloured
 * and named for what it is.
 */
function SourceTag({
  source,
  backend,
}: {
  source: TelemetrySource;
  backend?: BackendSource;
}) {
  // Browser-side sim: no backend involved, so /stats has nothing to say here.
  let label = "MOCK";
  let real = false;

  if (source !== "mock") {
    const kind = backend?.kind;
    if (kind === "serial") {
      label = "LIVE";
      real = true;
    } else if (kind === "replay") {
      label = backend?.loop ? "REPLAY ⟳" : "REPLAY";
    } else if (kind === "fake") {
      label = "SIM";
    } else {
      // /stats unreachable or a kind this build doesn't know. "LIVE" is the one
      // answer that must never be a guess, so say the source is unverified
      // rather than asserting the reassuring one.
      label = "SOURCE ?";
    }
  }

  const title = real
    ? `radio on ${backend?.port ?? "serial"}`
    : label === "MOCK"
      ? "simulated in this browser — not off the air"
      : label.startsWith("REPLAY")
        ? `replaying ${backend?.path ?? "a recorded log"} — not off the air`
        : label === "SIM"
          ? "backend simulator — not off the air"
          : "backend source unknown — cannot confirm these frames came off the air";

  return (
    <span
      title={title}
      className={cn(
        "text-[0.625rem] uppercase tracking-[0.16em]",
        real ? "text-ink-mute" : "text-caution",
      )}
    >
      <span aria-hidden>{real ? "● " : "◆ "}</span>
      {label}
    </span>
  );
}

/**
 * RATE — measured arrivals per second, not a constant.
 *
 * Two numbers, because they answer two different questions and confusing them
 * is what makes the console look broken: `frameHz` is how often new telemetry
 * arrives (and so how often anything here can change), `lineHz` is how many
 * packets the receiver actually decoded. This transmitter sends every packet
 * twice, so a healthy link reads "2.0/s · 4.0 rx" — and an operator watching a
 * serial monitor scroll at four lines a second can now see, rather than guess,
 * that the dashboard is not behind.
 *
 * Dim once the link is not live: a rate averaged over a window the vehicle
 * stopped transmitting into is a decaying number, not a measurement.
 */
function RateReadout({
  frameHz,
  lineHz,
  live,
}: {
  frameHz: number;
  lineHz: number;
  live: boolean;
}) {
  return (
    <span className="flex items-baseline gap-1.5">
      <span
        className={cn(
          "tnum text-sm tabular-nums",
          live ? "text-ink" : "text-ink-mute",
        )}
      >
        {frameHz > 0 ? frameHz.toFixed(1) : "—"}
      </span>
      <span className="text-[0.625rem] text-ink-mute">/s</span>
      {lineHz > 0 && (
        <span
          className="tnum text-[0.625rem] text-ink-mute"
          title="decoded packets per second, including the transmitter's repeats"
        >
          · {lineHz.toFixed(1)} rx
        </span>
      )}
    </span>
  );
}

/**
 * RecordButton — cut a flight folder, or close the one being written.
 *
 * The console cannot infer where a flight begins (see useFlightRecorder), so
 * this is the control that says so. It lives in the top bar rather than in
 * Settings because it is pressed on the pad, seconds before launch, from
 * whichever view the operator is already on.
 *
 * STOPPING IS ARMED, NOT IMMEDIATE. A single stray click on a live recording
 * would end the flight record mid-flight, and the whole point of the folder is
 * that it holds the whole flight; a first click arms, a second confirms, and it
 * disarms itself after three seconds. Starting is one click — an accidental
 * extra folder costs nothing.
 */
const ARM_MS = 3000;

/**
 * The name this recording will be filed under.
 *
 * Pre-filled from the mission (defaultFlightLabel) so pressing REC alone always
 * produces something identifiable, and editable because the operator is the
 * only one who knows whether this is the real attempt or the third scrub. The
 * backend slugifies whatever arrives, so typing "Launch #2 (wet)" is safe — it
 * lands as flight-02_launch-2-wet.
 *
 * Hidden while recording: the name is fixed once the folder exists, and an
 * input that still looks editable would imply otherwise.
 */
function FlightNameField({
  value,
  onChange,
  disabled,
  mission,
}: {
  value: string;
  onChange: (v: string) => void;
  disabled: boolean;
  mission: string;
}) {
  return (
    <input
      type="text"
      value={value}
      onChange={(e) => onChange(e.target.value)}
      disabled={disabled}
      spellCheck={false}
      maxLength={80}
      aria-label="Name for the next flight recording"
      placeholder={mission}
      title="Name for the next flight recording — folder is flight-NN_<name>"
      className={cn(
        "w-36 rounded-sm border border-hairline bg-transparent px-2 py-1",
        "text-[0.6875rem] text-ink placeholder:text-ink-mute/60",
        "hover:border-ink-mute focus:border-ink-mute focus:outline-none",
        "disabled:cursor-not-allowed disabled:opacity-40",
      )}
    />
  );
}

function RecordButton({
  recorder,
  mission,
  switching,
}: {
  recorder: RecorderState;
  mission: string;
  switching: boolean;
}) {
  const [armed, setArmed] = useState(false);
  // Lazy initialiser: defaultFlightLabel() reads the clock, and running it on
  // every render would rewrite the operator's typed name.
  const [name, setName] = useState(() => defaultFlightLabel(mission));
  // Whether the field holds the operator's own words. The suggested name is
  // re-stamped when the mission changes, and overwriting something they typed
  // to say that would be worse than a stale suggestion.
  const typed = useRef(false);
  // Ticks the elapsed readout locally. The top bar re-renders at 10 Hz off the
  // telemetry publish, but that stops when frames stop — and a recording timer
  // that freezes because the LINK went quiet says the wrong thing entirely.
  const [, tick] = useState(0);

  const { recording, flight, busy, status, session } = recorder;
  const offline = status !== "ok" || !session;
  const blocked = offline || busy || (!recording && switching);

  useEffect(() => {
    if (!armed) return;
    const id = window.setTimeout(() => setArmed(false), ARM_MS);
    return () => window.clearTimeout(id);
  }, [armed]);

  useEffect(() => {
    if (!recording) return;
    const id = window.setInterval(() => tick((n) => n + 1), 500);
    return () => window.clearInterval(id);
  }, [recording]);

  // A recording that ends from anywhere else (server shutdown, another tab)
  // must not leave this button sitting armed.
  useEffect(() => {
    if (!recording) setArmed(false);
  }, [recording]);

  // When a flight closes, refresh the suggested name so the next one is stamped
  // with the time it actually starts rather than the time the console booted.
  const wasRecording = useRef(false);
  useEffect(() => {
    if (wasRecording.current && !recording) {
      typed.current = false;
      setName(defaultFlightLabel(mission));
    }
    wasRecording.current = recording;
  }, [recording, mission]);

  // Switching vehicles re-stamps the suggestion: pressing REC after picking A2R
  // must not file the folder under A1R.
  useEffect(() => {
    if (!typed.current) setName(defaultFlightLabel(mission));
  }, [mission]);

  const onClick = () => {
    if (blocked) return;
    if (!recording) {
      recorder.start(name.trim() || defaultFlightLabel(mission));
    } else if (armed) {
      setArmed(false);
      recorder.stop();
    } else {
      setArmed(true);
    }
  };

  const elapsed = recordingElapsedSec(flight, Date.now());
  const label = !recording
    ? "REC"
    : armed
      ? "STOP?"
      : `REC ${fmtDuration(elapsed)}`;
  const title = !recording && switching
    ? "Wait for the receiver to confirm the selected vehicle before recording"
    : offline
    ? "No backend session — start backend.app to record a flight folder"
    : recording
      ? `Recording flights/${session}/${flight?.flight} · ${flight?.rows ?? 0} rows` +
        (armed ? " — click again to stop" : "")
      : "Start a new flight folder (telemetry, events, mission log, raw bytes)";

  return (
    <span className="flex items-center gap-2">
      {!recording && (
        <FlightNameField
          value={name}
          mission={mission}
          onChange={(v) => {
            typed.current = true;
            setName(v);
          }}
          disabled={blocked}
        />
      )}
      <button
        type="button"
        onClick={onClick}
        disabled={blocked}
        aria-pressed={recording}
        title={title}
        className={cn(
          "flex items-center gap-2 rounded-sm border px-2.5 py-1 transition-colors",
          "disabled:cursor-not-allowed disabled:opacity-40",
          armed
            ? "border-caution/60 bg-caution/10"
            : recording
              ? "border-alarm/50 bg-alarm/10"
              : "border-hairline hover:border-ink-mute",
        )}
      >
        <span
          aria-hidden
          className={cn(
            "size-2 rounded-full",
            recording ? "animate-pulse bg-alarm" : "border border-ink-mute",
          )}
        />
        <span
          className={cn(
            "tnum text-[0.6875rem] uppercase tracking-wide tabular-nums",
            armed ? "text-caution" : recording ? "text-alarm" : "text-ink-mute",
          )}
        >
          {label}
        </span>
      </button>
    </span>
  );
}

function Segment({
  label,
  children,
}: {
  label: string;
  children: React.ReactNode;
}) {
  return (
    <div className="flex items-center px-4">
      {/* center the group vertically in the bar, keep label + value on one baseline */}
      <span className="flex items-baseline gap-2">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">
          {label}
        </span>
        {children}
      </span>
    </div>
  );
}

export function TopBar({
  state,
  backend,
  recorder,
  mission,
}: {
  state: TelemetryState;
  backend?: BackendSource;
  recorder: RecorderState;
  mission: MissionState;
}) {
  const {
    link,
    linkAgeMs,
    tPlusSec,
    lossFraction,
    frameHz,
    lineHz,
    alarms,
    source,
  } = state;
  const alarmed = alarms.linkStale || alarms.noDeploy;
  const lossInk =
    lossFraction > 0.1
      ? "text-alarm"
      : lossFraction > 0.03
        ? "text-caution"
        : "text-ink-dim";

  return (
    <header
      role="banner"
      className={cn(
        "flex h-11 shrink-0 items-stretch divide-x divide-hairline border-b border-hairline transition-colors",
        alarmed ? "bg-alarm-bg" : "bg-surface",
      )}
    >
      <div className="flex items-center gap-2 px-4">
        <span className="text-[0.625rem] uppercase tracking-[0.16em] text-ink-mute">
          Mission
        </span>
        <MissionSelect mission={mission} />
      </div>

      <div className="flex items-center px-4">
        <span className="tnum text-sm tabular-nums text-ink">
          {fmtTimer(tPlusSec)}
        </span>
      </div>

      <div className="flex items-center px-4">
        <LinkPill link={link} ageMs={linkAgeMs} />
      </div>

      <Segment label="Rate">
        <RateReadout frameHz={frameHz} lineHz={lineHz} live={link === "live"} />
      </Segment>

      <Segment label="Loss">
        <span className={`tnum text-sm tabular-nums ${lossInk}`}>
          {fmtPercent(lossFraction, 1)}
        </span>
      </Segment>

      <div className="ml-auto flex items-center px-4">
        <RecordButton recorder={recorder} mission={mission.mission.name}
          switching={mission.busy || mission.pending || Boolean(mission.requested)} />
      </div>

      <div className="flex items-center px-4">
        <SourceTag source={source} backend={backend} />
      </div>
    </header>
  );
}
