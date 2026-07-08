/**
 * useSettings — the dashboard's small, persisted preference store.
 *
 * This is the ONE cross-cutting piece the Log/Settings feature adds: a React
 * Context backed by localStorage that the rest of the UI reads (high-contrast
 * mode, telemetry-table density/cap, buzzer + alarm arming). It is presentation
 * state only — it never touches the frozen telemetry data layer.
 *
 * Persistence is defensive: a versioned key, read-with-defaults-merge on load
 * (so an old or partial saved blob still boots), and a try/catch around every
 * localStorage call (private mode / quota / disabled storage → in-memory only).
 */
import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from "react"

export type TableDensity = "comfortable" | "compact"

export interface Settings {
  /** Sunlight/outdoor mode: bumps ink + border contrast via a root class. */
  highContrast: boolean
  table: {
    density: TableDensity
    /** Max rows shown in the Log telemetry table (newest-first). */
    rowCap: number
  }
  /** Master audio switch for the alarm buzzer (Web Audio). */
  buzzer: boolean
  /** Which alarms are armed — gates whether the buzzer sounds for each. */
  alarms: {
    linkStale: boolean
    noDeploy: boolean
  }
}

export const DEFAULT_SETTINGS: Settings = {
  highContrast: false,
  table: { density: "comfortable", rowCap: 300 },
  buzzer: false,
  alarms: { linkStale: true, noDeploy: true },
}

/** Selectable row caps surfaced in the Display card. */
export const ROW_CAP_OPTIONS = [100, 300, 1000] as const

const STORAGE_KEY = "apex.settings.v1"

/** Deep-partial over our known two-level shape (top-level + table/alarms). */
export type SettingsPatch = {
  highContrast?: boolean
  table?: Partial<Settings["table"]>
  buzzer?: boolean
  alarms?: Partial<Settings["alarms"]>
}

/** Merge a saved/partial blob onto defaults so missing keys always have a value. */
function coerce(raw: unknown): Settings {
  if (!raw || typeof raw !== "object") return DEFAULT_SETTINGS
  const r = raw as Record<string, unknown>
  const table = (r.table ?? {}) as Record<string, unknown>
  const alarms = (r.alarms ?? {}) as Record<string, unknown>
  return {
    highContrast: typeof r.highContrast === "boolean" ? r.highContrast : DEFAULT_SETTINGS.highContrast,
    table: {
      density: table.density === "compact" ? "compact" : "comfortable",
      rowCap:
        typeof table.rowCap === "number" && Number.isFinite(table.rowCap)
          ? table.rowCap
          : DEFAULT_SETTINGS.table.rowCap,
    },
    buzzer: typeof r.buzzer === "boolean" ? r.buzzer : DEFAULT_SETTINGS.buzzer,
    alarms: {
      linkStale: typeof alarms.linkStale === "boolean" ? alarms.linkStale : DEFAULT_SETTINGS.alarms.linkStale,
      noDeploy: typeof alarms.noDeploy === "boolean" ? alarms.noDeploy : DEFAULT_SETTINGS.alarms.noDeploy,
    },
  }
}

function load(): Settings {
  try {
    const stored = window.localStorage.getItem(STORAGE_KEY)
    return stored ? coerce(JSON.parse(stored)) : DEFAULT_SETTINGS
  } catch {
    return DEFAULT_SETTINGS
  }
}

interface SettingsContextValue {
  settings: Settings
  /** Shallow-merge a patch (nested table/alarms are merged one level deep). */
  update: (patch: SettingsPatch) => void
  reset: () => void
}

const SettingsContext = createContext<SettingsContextValue | null>(null)

export function SettingsProvider({ children }: { children: ReactNode }) {
  const [settings, setSettings] = useState<Settings>(load)
  // First render shouldn't re-write storage with what we just read.
  const hydrated = useRef(false)

  useEffect(() => {
    if (!hydrated.current) {
      hydrated.current = true
      return
    }
    try {
      window.localStorage.setItem(STORAGE_KEY, JSON.stringify(settings))
    } catch {
      /* storage unavailable — keep running from in-memory state */
    }
  }, [settings])

  const update = useCallback((patch: SettingsPatch) => {
    setSettings((s) => ({
      highContrast: patch.highContrast ?? s.highContrast,
      table: { ...s.table, ...patch.table },
      buzzer: patch.buzzer ?? s.buzzer,
      alarms: { ...s.alarms, ...patch.alarms },
    }))
  }, [])

  const reset = useCallback(() => setSettings(DEFAULT_SETTINGS), [])

  return (
    <SettingsContext.Provider value={{ settings, update, reset }}>
      {children}
    </SettingsContext.Provider>
  )
}

export function useSettings(): SettingsContextValue {
  const ctx = useContext(SettingsContext)
  if (!ctx) throw new Error("useSettings must be used within a SettingsProvider")
  return ctx
}
