/**
 * useOnboardLog — the VEHICLE's own recording state, latched across frames.
 *
 * The flight computer prints this to USB every 5 s (`printStatus`'s `[SD]`
 * line): which file is open, how many lines are in it, at what rate, and how
 * many writes failed. On the pad that port is inside a sealed airframe 19 km
 * away, so the console had one bit to show instead — SD OK / SD LOST — and
 * those are not the same claim. SD OK means the card MOUNTED. A card that
 * mounts, opens a file, and then stops accepting writes reports SD OK for the
 * whole flight, and the operator finds out when they pull the card.
 *
 * The vehicle now downlinks three fields for it (Radio.cpp, SD_BLOCK_EVERY):
 *
 *   SDF  the open /FLIGHTnnn.CSV, by index    SDL  its line count
 *   SDE  failed writes
 *
 * ONE PACKET IN TEN carries them — the packet is 189-204 bytes against a
 * 255-byte limit and there is no room at 2 Hz — so this hook latches the last
 * report rather than reading the current frame. Nine frames out of ten have
 * nothing to say about the recorder, and rendering that as zero would describe
 * a healthy recorder as a dead one.
 *
 * NOT `useFlightRecorder`, which is the GROUND station cutting its own flight
 * folders on this laptop. This is the card inside the rocket.
 *
 * The write rate is DERIVED here, from the line count across two reports, the
 * same way `printStatus` derives `logHz` on the vehicle. Sending a rate would
 * have cost bytes to say what a subtraction already says.
 */
import { useEffect, useRef, useState } from "react"
import type { TelemetryState } from "./useTelemetry"

export interface OnboardLog {
  /** 1..999 — the /FLIGHTnnn.CSV the vehicle has open. 0 = none, null = unreported. */
  fileIndex: number | null
  /** `FLIGHT007.CSV`, or null when no file is open. */
  fileName: string | null
  lines: number | null
  errors: number | null
  /** Lines per second between the last two reports; null until there are two. */
  rateHz: number | null
  /** Host clock (epoch ms) of the last report; null if none has arrived. */
  reportedAt: number | null
  /**
   * Two reports, same line count: the card is mounted, the file is open, and
   * nothing is being written to it. This is the failure the SD health bit
   * cannot express, and the reason these fields are on the air at all.
   */
  stalled: boolean
}

const NOTHING_YET: OnboardLog = {
  fileIndex: null,
  fileName: null,
  lines: null,
  errors: null,
  rateHz: null,
  reportedAt: null,
  stalled: false,
}

function fileNameFor(index: number): string | null {
  // Storage.cpp names files /FLIGHT%03d.CSV from 1. 0 is its "no file was ever
  // opened" value, which is not a file and must not render as FLIGHT000.CSV.
  return index >= 1 ? `FLIGHT${String(index).padStart(3, "0")}.CSV` : null
}

export function useOnboardLog(t: TelemetryState): OnboardLog {
  const [status, setStatus] = useState<OnboardLog>(NOTHING_YET)

  // One update per PACKET, not per render and not per copy. The transmitter
  // sends every packet twice ~205 ms apart; treating the second copy as a
  // second report would put a 0-line delta over a 0.2 s gap into the rate, and
  // read a perfectly healthy recorder as STALLED twice a second.
  const lastSeq = useRef<number | null>(null)

  const frame = t.frame

  useEffect(() => {
    if (!frame) {
      lastSeq.current = null
      setStatus(NOTHING_YET)
      return
    }
    if (frame.seq === lastSeq.current) return
    lastSeq.current = frame.seq

    const lines = frame.extra.SDL
    if (lines === undefined) return   // one of the nine packets in between

    const index = frame.extra.SDF ?? null
    const errors = frame.extra.SDE ?? null
    const at = frame.hostTime

    setStatus((prev) => {
      // A new file (the vehicle rebooted and opened the next one) restarts the
      // count from zero, so the previous report is not a baseline for this one.
      // Same file with a SMALLER count is the same event seen without SDF.
      const continues =
        prev.reportedAt != null &&
        prev.lines != null &&
        (index == null || prev.fileIndex === index) &&
        lines >= prev.lines

      const dtSec = continues ? (at - prev.reportedAt!) / 1000 : 0
      const rateHz = continues && dtSec > 0 ? (lines - prev.lines!) / dtSec : null

      return {
        fileIndex: index,
        fileName: index == null ? prev.fileName : fileNameFor(index),
        lines,
        errors,
        rateHz,
        reportedAt: at,
        stalled: rateHz === 0,
      }
    })
  }, [frame])

  return status
}
