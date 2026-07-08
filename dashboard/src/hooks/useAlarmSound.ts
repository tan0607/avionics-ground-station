/**
 * useAlarmSound — the alarm buzzer (Web Audio), gated by the settings store.
 *
 * It sounds a short beep, then repeats gently while any ARMED alarm is active
 * and the master buzzer is on. Everything is defensive:
 *   • the AudioContext is created lazily — browsers block audio until a user
 *     gesture, so we unlock on the first pointer/key event AND on the Settings
 *     "test" button, whichever comes first;
 *   • every Web Audio call is wrapped so a missing/blocked API can never break
 *     the telemetry UI. Silent by default (buzzer off).
 *
 * This reads telemetry + settings; it never writes the frozen data layer.
 */
import { useCallback, useEffect, useRef } from "react"
import type { TelemetryState } from "./useTelemetry"
import type { Settings } from "./useSettings"

// Gentle, not shrill: a short mid-high tone at low gain, repeated slowly.
const BEEP_HZ = 880
const BEEP_MS = 180
const BEEP_GAIN = 0.05
const REPEAT_MS = 2000

type AudioCtor = typeof AudioContext

function audioCtor(): AudioCtor | null {
  if (typeof window === "undefined") return null
  return (window.AudioContext ?? (window as unknown as { webkitAudioContext?: AudioCtor }).webkitAudioContext) ?? null
}

export interface AlarmSound {
  /** Play a single test beep (also unlocks audio for later alarms). */
  test: () => void
  /** Whether the browser exposes Web Audio at all. */
  supported: boolean
}

export function useAlarmSound(t: TelemetryState, settings: Settings): AlarmSound {
  const ctxRef = useRef<AudioContext | null>(null)
  const Ctor = audioCtor()
  const supported = Ctor !== null

  /** Create/resume the context; safe to call from any gesture. Returns null on failure. */
  const ensureCtx = useCallback((): AudioContext | null => {
    if (!Ctor) return null
    try {
      if (!ctxRef.current) ctxRef.current = new Ctor()
      if (ctxRef.current.state === "suspended") void ctxRef.current.resume()
      return ctxRef.current
    } catch {
      return null
    }
  }, [Ctor])

  const beep = useCallback((ctx: AudioContext) => {
    try {
      const osc = ctx.createOscillator()
      const gain = ctx.createGain()
      osc.type = "square"
      osc.frequency.value = BEEP_HZ
      const now = ctx.currentTime
      // short attack/decay envelope so it clicks cleanly, no lingering tail
      gain.gain.setValueAtTime(0, now)
      gain.gain.linearRampToValueAtTime(BEEP_GAIN, now + 0.01)
      gain.gain.linearRampToValueAtTime(0, now + BEEP_MS / 1000)
      osc.connect(gain).connect(ctx.destination)
      osc.start(now)
      osc.stop(now + BEEP_MS / 1000 + 0.02)
    } catch {
      /* audio graph unavailable — no-op */
    }
  }, [])

  const test = useCallback(() => {
    const ctx = ensureCtx()
    if (ctx) beep(ctx)
  }, [ensureCtx, beep])

  // Unlock on the first user gesture so a real alarm can sound without the
  // operator having pressed "test" first.
  useEffect(() => {
    if (!supported) return
    const unlock = () => ensureCtx()
    const opts = { once: true, passive: true } as const
    window.addEventListener("pointerdown", unlock, opts)
    window.addEventListener("keydown", unlock, opts)
    return () => {
      window.removeEventListener("pointerdown", unlock)
      window.removeEventListener("keydown", unlock)
    }
  }, [supported, ensureCtx])

  // Is an armed alarm currently active with the buzzer on?
  const active =
    settings.buzzer &&
    ((settings.alarms.linkStale && t.alarms.linkStale) ||
      (settings.alarms.noDeploy && t.alarms.noDeploy))

  useEffect(() => {
    if (!active) return
    const ctx = ensureCtx()
    if (!ctx) return // no gesture yet / unsupported → stays silent until unlocked
    beep(ctx)
    const id = window.setInterval(() => beep(ctx), REPEAT_MS)
    return () => window.clearInterval(id)
  }, [active, ensureCtx, beep])

  return { test, supported }
}
