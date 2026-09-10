/**
 * Mission identity — one source of truth for which rocket this console flies.
 *
 * It used to be a bare constant ("APEX-1") inside TopBar, which was fine while
 * one box flew one vehicle. Two rockets fly now, each on its own radio channel,
 * and the mission name is how the team refers to them: A1R is the vehicle on
 * channel A, A2R the one on channel B.
 *
 * THE NAME AND THE CHANNEL ARE THE SAME FACT. Naming the mission and tuning the
 * receiver used to be two unrelated actions — a constant in the source, and an
 * A/B switch in Settings — so the console could sit there labelled one rocket
 * while listening to the other, and a receiver on the wrong channel is not weak
 * or garbled, it is SILENT. Pairing them here means picking the mission is
 * picking the channel; there is no second place to get it wrong.
 *
 * Flight folders are named from the mission too, so the record on disk says
 * which vehicle it came off.
 */

/** The radio channels MRCC_GroundStation knows (firmware CHANNELS[], same keys). */
export type ChannelId = "A" | "B"

export interface Mission {
  /** What the team calls this vehicle. */
  name: string
  /** The channel its rocket transmits on — the key the box takes over serial. */
  channel: ChannelId
}

/**
 * The flyable vehicles, in the order they appear in every picker.
 *
 * Adding a third rocket is this array plus a CHANNEL_C_HZ in the firmware —
 * nothing in the UI counts to two.
 */
export const MISSIONS: readonly Mission[] = [
  { name: "A1R", channel: "A" },
  { name: "A2R", channel: "B" },
]

/** What the console shows before the receiver has said anything. */
export const DEFAULT_MISSION: Mission = MISSIONS[0]

/** The mission flying on a channel the box reported, or null if it is not one of ours. */
export function missionForChannel(channel: string | null | undefined): Mission | null {
  if (!channel) return null
  const key = channel.trim().toUpperCase()
  return MISSIONS.find((m) => m.channel === key) ?? null
}

/** Look a mission up by name — for turning a <select> value back into a mission. */
export function missionByName(name: string): Mission | null {
  return MISSIONS.find((m) => m.name === name) ?? null
}

/**
 * The name a new flight recording gets if the operator does not type one.
 *
 * Mission plus local HH:MM, because the two things you want when reading a
 * folder name back are WHICH vehicle and WHICH attempt — a pad full of scrubbed
 * starts otherwise produces flight-01, flight-02, flight-03 with nothing to
 * tell them apart until you open each metadata.json. The session folder already
 * carries the date, so only the time is added here.
 */
export function defaultFlightLabel(
  mission: string = DEFAULT_MISSION.name,
  now: Date = new Date(),
): string {
  const hh = String(now.getHours()).padStart(2, "0")
  const mm = String(now.getMinutes()).padStart(2, "0")
  return `${mission}-${hh}${mm}`
}


/**
 * Peripherals this flight is deliberately going WITHOUT, by SUBSYSTEMS name.
 *
 * Not fitted is not failed, and the Peripherals panel already draws that
 * distinction: PYRO and VBAT sit grey at "—" because PYRO_CONT_ENABLED and
 * VBAT_ENABLED are 0 in the firmware, and nothing counts them as DOWN. This
 * list makes the same statement about a peripheral the firmware still compiles
 * in and still reports a health bit for.
 *
 * IT IS A DECLARATION, NOT A DETECTOR, and that is the whole discipline of it.
 * It must never be derived from the health bit: a row that greys itself out the
 * moment it goes down is a row that can no longer report going down, which
 * costs you the one thing the panel exists for. Naming a peripheral here is a
 * decision made on the ground, before the flight, about what is not on board.
 *
 * WHAT IT DOES NOT TOUCH: the vehicle still initialises the card, still sends
 * the bit, and the backend still records it — `hw_sd` stays 0 on every row of
 * telemetry.csv. The flight record keeps saying what actually happened; only
 * the pad display stops calling it a fault it can act on.
 *
 * 2026-09-10, A1R: the onboard recorder failed pre-flight. The flight record
 * for this launch is the 10 Hz downlink into the backend's telemetry.csv.
 * Remove the entry when the card is replaced.
 */
export const UNFITTED_PERIPHERALS: readonly string[] = ["SD"]

/** True when a peripheral is not part of this flight's configuration. */
export function isUnfitted(name: string): boolean {
  return UNFITTED_PERIPHERALS.includes(name)
}
