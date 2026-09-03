/**
 * alerts — turn console state into a short list of things that are wrong, each
 * with the fix.
 *
 * WHY THIS IS A PURE FUNCTION OF LIVE STATE, not an event log. Alerts are
 * derived fresh from every poll, so one clears the moment its cause does. A
 * queue of past errors would keep "port busy" on screen after the port was
 * freed, and an operator cannot tell a stale warning from a live one at a
 * glance — which is worse than no warning, because it teaches them to ignore
 * the corner of the screen where the real ones appear.
 *
 * The distinction this exists to preserve: "the backend cannot read the radio"
 * is NOT "the radio is quiet", even though both leave the console empty. One is
 * fixed on the laptop in seconds; the other sends someone walking to the pad.
 * A single "NO LINK" indicator conflated them. (This replaced a top-of-screen
 * SourceAlarm banner that made the same distinction for serial errors only.)
 *
 * WHY EVERY ALERT CARRIES A FIX. "Serial error" tells an operator nothing they
 * did not already know from the blank screen. The failures this console
 * actually sees have specific, boring causes — the Arduino Serial Monitor is
 * holding the port, the port name changed when the cable moved, the backend was
 * stopped — and each has one action. The action is the payload; the message is
 * just how you find it.
 */

export type AlertSeverity = "error" | "warn"

export type Alert = {
  /** Stable across polls, so a live alert is not re-created (and re-animated)
   *  every two seconds, and so a dismissal can be remembered. */
  id: string
  severity: AlertSeverity
  title: string
  /** What the console observed. */
  detail: string
  /** What to do about it, in the imperative. */
  fix: string
  /** A command that does the fix, when one exists. Rendered copyable. */
  command?: string
}

export type AlertInputs = {
  /** From useBackendStats. */
  backendStatus: "loading" | "ok" | "unreachable"
  sourceError?: string | null
  sourceKind?: string
  framesDecoded?: number
  crcErrors?: number
  unknownStates?: string[]
  /** From useTelemetry: the WebSocket. "mock" means no backend by design. */
  socketStatus: "connecting" | "open" | "closed" | "mock"
  /** From useGroundStation: last failed channel command, if any. */
  channelError?: string | null
}

// The serial layer reports OS text, which varies by platform and driver. Match
// on the part that is stable and say what each one actually means -- an
// operator should not have to know that "Resource busy" is pyserial's way of
// saying the Arduino IDE still has the port.
function serialAlert(raw: string, port: string): Alert {
  const t = raw.toLowerCase()

  if (t.includes("busy") || t.includes("resource temporarily unavailable")) {
    return {
      id: "serial-busy",
      severity: "error",
      title: "Serial port is held by another program",
      detail: `${port} opened, but something else already owns it.`,
      fix: "Close the Arduino IDE's Serial Monitor — that is the usual culprit. " +
           "A stray `screen` or `pio device monitor` does it too. The backend " +
           "retries on its own, so the link comes back by itself once the port is free.",
      command: "pkill -f 'screen /dev/tty' ; pkill -f 'device monitor'",
    }
  }

  if (t.includes("no such file") || t.includes("could not open port") ||
      t.includes("filenotfounderror")) {
    return {
      id: "serial-missing",
      severity: "error",
      title: "Serial port does not exist",
      detail: `${port} is not there. Usually the cable moved to another USB socket, ` +
              "which renames the device, or the board is unplugged.",
      fix: "List the ports, then restart the backend with the name you find. " +
           "Replugging the same cable into the same socket restores the old name.",
      command: "ls /dev/tty.usbserial-* /dev/tty.wchusbserial* 2>/dev/null",
    }
  }

  if (t.includes("permission")) {
    return {
      id: "serial-permission",
      severity: "error",
      title: "No permission to open the serial port",
      detail: `The OS refused ${port}.`,
      fix: "On Linux, add yourself to the dialout group and log out and back in. " +
           "On macOS this usually means the wrong device node — check the port name.",
      command: "sudo usermod -aG dialout $USER",
    }
  }

  return {
    id: "serial-error",
    severity: "error",
    title: "Serial port error",
    detail: `${port}: ${raw}`,
    fix: "Replug the receiver and give the backend a few seconds — it retries " +
         "automatically. If it persists, restart the backend with the current port name.",
    command: "ls /dev/tty.usbserial-* /dev/tty.wchusbserial* 2>/dev/null",
  }
}

export function deriveAlerts(input: AlertInputs): Alert[] {
  const out: Alert[] = []

  // Mock is a deliberate no-backend mode. Alerting there would fire the whole
  // stack permanently for anyone doing UI work.
  if (input.socketStatus === "mock") return out

  if (input.backendStatus === "unreachable") {
    out.push({
      id: "backend-unreachable",
      severity: "error",
      title: "Backend is not answering",
      detail: "The console cannot reach the server that feeds it. Everything on " +
              "screen is the last thing received, however old.",
      fix: "Check the terminal running the backend — it has probably stopped or " +
           "crashed. Start it again from the repo root and the console " +
           "reconnects on its own. Add --demo to run without the receiver.",
      command: "./start.command",
    })
    // A dead backend explains every symptom below it. Reporting those too would
    // bury the one alert that matters under its own consequences.
    return out
  }

  if (input.sourceError) {
    out.push(serialAlert(input.sourceError, input.sourceKind || "the port"))
  }

  if (input.socketStatus === "closed" || input.socketStatus === "connecting") {
    out.push({
      id: "socket-down",
      severity: "warn",
      title: "Live socket is down",
      detail: "The backend is reachable but the telemetry WebSocket is not open, " +
              "so frames are not arriving on screen.",
      fix: "It reconnects by itself; give it a few seconds. If the numbers stay " +
           "frozen after that, reload the page.",
    })
  }

  // The silent one. Lines are arriving and failing to decode, which looks
  // exactly like a dead radio from the charts alone.
  if ((input.framesDecoded ?? 0) === 0 && (input.crcErrors ?? 0) > 0) {
    out.push({
      id: "format-mismatch",
      severity: "error",
      title: "Data is arriving but nothing decodes",
      detail: `${input.crcErrors} unusable frames and none decoded. The receiver is ` +
              "hearing something, so this is a decoding problem, not a radio one.",
      fix: "Usually the wrong wire format. Restart the backend with --format mrcc " +
           "for the live ASCII link (--format binary is only for --fake). If the " +
           "format is right, the two radios disagree on SF/BW/CR — check them " +
           "against firmware/README.md.",
      command: "--format mrcc",
    })
  }

  if ((input.unknownStates?.length ?? 0) > 0) {
    const names = input.unknownStates!.join(", ")
    out.push({
      id: "unknown-states",
      severity: "warn",
      title: "Transmitter is sending an unknown flight phase",
      detail: `Unrecognised: ${names}. Those frames read as PAD on this console, ` +
              "so the flight state is wrong while they arrive.",
      fix: "The transmitter renamed a phase. Add the new word to STATE_PREFIXES " +
           "in shared/protocol/mrcc.py — it matches by prefix, so the short form " +
           "of an existing phase only needs one entry.",
    })
  }

  if (input.channelError) {
    out.push({
      id: "channel-command-failed",
      severity: "warn",
      title: "Channel switch did not go through",
      detail: input.channelError,
      fix: "The receiver is still on whatever channel it reports. Check the port " +
           "is open, then press A or B again.",
    })
  }

  return out
}
