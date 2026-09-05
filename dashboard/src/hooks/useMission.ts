/**
 * useMission — which rocket the console is flying, and the one control that
 * changes it.
 *
 * Picking a mission RETUNES THE RECEIVER. A1R lives on channel A, A2R on
 * channel B (lib/mission.ts), so selecting a vehicle sends the same A/B key an
 * operator would type at the serial monitor and the box retunes. That is the
 * whole point: the name on the header and the channel in the radio cannot be
 * set separately, so they cannot disagree.
 *
 * BUT THE BOX IS STILL THE AUTHORITY. `mission` is what the operator asked for;
 * `confirmed` is the mission matching the channel the receiver itself
 * announced, and it is null until the box has spoken. When the two differ the
 * command has not landed yet (or at all — port wedged, board hung), which is
 * exactly what `pending` says, and the UI shows it rather than quietly drawing
 * the requested name as if it were fact. A silent receiver looks identical to a
 * healthy one on a channel with nothing on it, so this distinction is the only
 * warning the operator gets.
 *
 * Before the operator picks anything the selection FOLLOWS the box, so a
 * console that boots next to a running receiver starts on the truth instead of
 * on a default.
 */
import { useCallback, useEffect, useRef, useState } from "react"
import {
  DEFAULT_MISSION,
  MISSIONS,
  missionByName,
  missionForChannel,
  type Mission,
} from "@/lib/mission"
import type { GroundStation } from "@/hooks/useGroundStation"

export interface MissionState {
  /** Every flyable vehicle, in picker order. */
  missions: readonly Mission[]
  /** The mission this console is flying — the operator's pick. */
  mission: Mission
  /** The mission the receiver's own reported channel belongs to; null if unheard. */
  confirmed: Mission | null
  /** The box is on a different rocket than the one selected. */
  pending: boolean
  /** A --serial session: selecting can actually command the radio. */
  supported: boolean
  /** A channel command is in flight. */
  busy: boolean
  /** Target awaiting the operator's confirmation; selection has not changed. */
  requested: Mission | null
  /** Whether incoming frames can belong to the selected vehicle. */
  acceptTelemetry: boolean
  /** Request a switch; only confirm() retunes the receiver. */
  select: (name: string, trigger?: HTMLElement) => void
  cancel: () => void
  confirm: () => Promise<void>
}

export function useMission(
  gs: GroundStation,
  beforeSwitch?: () => Promise<boolean>,
): MissionState {
  const [mission, setMission] = useState<Mission>(DEFAULT_MISSION)
  const [requested, setRequested] = useState<Mission | null>(null)
  const [switching, setSwitching] = useState(false)
  const switchingRef = useRef(false)
  const returnFocus = useRef<HTMLElement | null>(null)
  // Once the operator has chosen, the box no longer moves the selection under
  // them — otherwise a slow retune would snap the picker back mid-switch.
  const chosen = useRef(false)
  // A failed /gs poll must not turn a serial session into a label-only session
  // and allow the old rocket's frames back into a newly cleared graph.
  const radioRequired = useRef(gs.supported)
  if (gs.supported) radioRequired.current = true

  const confirmed = missionForChannel(gs.channel)

  useEffect(() => {
    if (!chosen.current && confirmed) setMission(confirmed)
  }, [confirmed])
  useEffect(() => {
    if (!requested && !switching && !gs.busy && returnFocus.current) {
      returnFocus.current.focus()
      returnFocus.current = null
    }
  }, [requested, switching, gs.busy])

  const { supported, setChannel } = gs
  const pending = radioRequired.current && confirmed?.name !== mission.name
  const select = useCallback(
    (name: string, trigger?: HTMLElement) => {
      const next = missionByName(name)
      if (!next || gs.busy || switchingRef.current) return
      if (next.name === mission.name && !pending) return
      returnFocus.current = trigger ?? (document.activeElement as HTMLElement | null)
      setRequested(next)
    },
    [gs.busy, mission.name, pending],
  )
  const cancel = useCallback(() => {
    if (!switchingRef.current) setRequested(null)
  }, [])
  const confirm = useCallback(async () => {
    if (!requested || switchingRef.current || gs.busy) return
    switchingRef.current = true
    setSwitching(true)
    try {
      if (beforeSwitch && !(await beforeSwitch())) return
      chosen.current = true
      setMission(requested)
      setRequested(null)
      if (radioRequired.current) await setChannel(requested.channel)
    } finally {
      switchingRef.current = false
      setSwitching(false)
    }
  }, [requested, beforeSwitch, gs.busy, setChannel])

  return {
    missions: MISSIONS,
    mission,
    confirmed,
    pending,
    supported,
    busy: gs.busy || switching,
    requested,
    acceptTelemetry: !chosen.current || (!pending && !gs.busy && !switching),
    select,
    cancel,
    confirm,
  }
}
