/**
 * Backend origin resolver for the HTTP endpoints the Settings page uses
 * (/stats + /session/* downloads). Mirrors the WebSocket URL logic in
 * useTelemetry: an explicit VITE_WS_URL wins (ws→http), otherwise dev talks to
 * the backend on :8000 and a backend-served production build is same-origin.
 */
export function apiBase(): string {
  const env = import.meta.env.VITE_WS_URL as string | undefined
  if (env) {
    try {
      const u = new URL(env)
      const proto = u.protocol === "wss:" ? "https:" : "http:"
      return `${proto}//${u.host}`
    } catch {
      /* malformed env — fall through */
    }
  }
  // Vite dev server (5173) → backend runs separately on 8000.
  if (import.meta.env.DEV) return `http://${window.location.hostname}:8000`
  // Production build is served BY the backend → same origin, relative URLs.
  return ""
}

export function apiUrl(path: string): string {
  const base = apiBase()
  return base + (path.startsWith("/") ? path : `/${path}`)
}
