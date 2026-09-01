/**
 * Mission identity — one source of truth for the vehicle this console is flying.
 *
 * It used to be a bare constant inside TopBar, which was fine while the only
 * thing that needed it was the header. Flight folders are named from it now, so
 * a mission rename has to reach the record on disk as well as the strip at the
 * top of the screen, and those must not be able to drift apart.
 */
export const MISSION = "APEX-1"

/**
 * The name a new flight recording gets if the operator does not type one.
 *
 * Mission plus local HH:MM, because the two things you want when reading a
 * folder name back are WHICH vehicle and WHICH attempt — a pad full of scrubbed
 * starts otherwise produces flight-01, flight-02, flight-03 with nothing to
 * tell them apart until you open each metadata.json. The session folder already
 * carries the date, so only the time is added here.
 */
export function defaultFlightLabel(now: Date = new Date()): string {
  const hh = String(now.getHours()).padStart(2, "0")
  const mm = String(now.getMinutes()).padStart(2, "0")
  return `${MISSION}-${hh}${mm}`
}
