/**
 * Native WebSocket client with automatic reconnect. No socket.io — the backend
 * (FastAPI) serves a raw WS that broadcasts decoded telemetry JSON. On drop it
 * reconnects with exponential backoff + jitter and reports status transitions so
 * the UI can show the link as connecting / down. "Native" per DESIGN_SPECS §2.
 */
export type SocketStatus = "connecting" | "open" | "closed"

export interface TelemetrySocketOptions {
  url: string
  onMessage: (data: unknown) => void
  onStatus: (status: SocketStatus) => void
  /** backoff ceiling, ms */
  maxBackoffMs?: number
}

export class TelemetrySocket {
  private ws: WebSocket | null = null
  private stopped = false
  private attempt = 0
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private readonly opts: TelemetrySocketOptions
  private readonly maxBackoffMs: number

  constructor(opts: TelemetrySocketOptions) {
    this.opts = opts
    this.maxBackoffMs = opts.maxBackoffMs ?? 15_000
  }

  start(): void {
    this.stopped = false
    this.open()
  }

  private open(): void {
    if (this.stopped) return
    this.opts.onStatus("connecting")
    let ws: WebSocket
    try {
      ws = new WebSocket(this.opts.url)
    } catch {
      this.scheduleReconnect()
      return
    }
    this.ws = ws

    ws.onopen = () => {
      this.attempt = 0
      this.opts.onStatus("open")
    }

    ws.onmessage = (event: MessageEvent) => {
      if (typeof event.data !== "string") return
      try {
        this.opts.onMessage(JSON.parse(event.data))
      } catch {
        // ignore non-JSON / malformed frames; the parser is upstream's contract
      }
    }

    ws.onerror = () => {
      // onclose always follows; reconnect is handled there.
      ws.close()
    }

    ws.onclose = () => {
      this.ws = null
      this.opts.onStatus("closed")
      this.scheduleReconnect()
    }
  }

  private scheduleReconnect(): void {
    if (this.stopped) return
    const base = Math.min(this.maxBackoffMs, 500 * 2 ** this.attempt)
    const delay = base / 2 + Math.random() * (base / 2) // 50–100% of base
    this.attempt += 1
    this.reconnectTimer = setTimeout(() => this.open(), delay)
  }

  stop(): void {
    this.stopped = true
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer)
    this.reconnectTimer = null
    if (this.ws) {
      this.ws.onclose = null // don't reconnect on an intentional close
      this.ws.close()
      this.ws = null
    }
  }
}
