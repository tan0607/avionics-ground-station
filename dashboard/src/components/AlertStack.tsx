/**
 * AlertStack — the bottom-right corner where things that are wrong appear, each
 * with what to do about it.
 *
 * Alerts are derived from live state (lib/alerts.ts), so one vanishes when its
 * cause is fixed. Dismissing hides an alert the operator has read; the
 * dismissal is forgotten once the condition clears, so a fault that comes back
 * announces itself again rather than staying silent because it was waved away
 * an hour ago.
 *
 * Positioned over the console rather than in it: these interrupt, and a flight
 * is not the moment to go hunting for a status line.
 */
import { useEffect, useRef, useState } from "react"
import { cn } from "@/lib/utils"
import { deriveAlerts, type Alert, type AlertInputs } from "@/lib/alerts"

function CopyableCommand({ command }: { command: string }) {
  const [copied, setCopied] = useState(false)
  const timer = useRef<number | undefined>(undefined)

  useEffect(() => () => window.clearTimeout(timer.current), [])

  return (
    <button
      type="button"
      onClick={() => {
        // Clipboard is unavailable on insecure origins other than localhost.
        // Failing quietly is right: the command is on screen and readable, so
        // the fix still works, it just needs typing.
        navigator.clipboard?.writeText(command).then(
          () => {
            setCopied(true)
            window.clearTimeout(timer.current)
            timer.current = window.setTimeout(() => setCopied(false), 1200)
          },
          () => {},
        )
      }}
      title="Copy"
      className={cn(
        "mt-1.5 block w-full truncate rounded-sm border border-hairline bg-surface-2",
        "px-1.5 py-1 text-left font-mono text-[0.6875rem] text-ink-dim",
        "transition-colors hover:text-ink",
      )}
    >
      {copied ? "copied" : command}
    </button>
  )
}

function AlertCard({ alert, onDismiss }: { alert: Alert; onDismiss: () => void }) {
  const isError = alert.severity === "error"
  return (
    <div
      role="alert"
      className={cn(
        "pointer-events-auto w-80 rounded-sm border bg-surface shadow-lg",
        isError ? "border-alarm" : "border-caution",
      )}
    >
      <div className="flex items-start gap-2 px-2 pt-1.5">
        <span
          aria-hidden
          className={cn(
            "mt-1 h-1.5 w-1.5 shrink-0 rounded-full",
            isError ? "bg-alarm" : "bg-caution",
          )}
        />
        <p
          className={cn(
            "flex-1 text-[0.6875rem] font-medium uppercase leading-snug tracking-wide",
            isError ? "text-alarm" : "text-caution",
          )}
        >
          {alert.title}
        </p>
        <button
          type="button"
          onClick={onDismiss}
          aria-label="Dismiss"
          className="-mt-0.5 shrink-0 px-1 text-ink-mute transition-colors hover:text-ink"
        >
          ×
        </button>
      </div>

      <div className="px-2 pb-2 pl-[1.375rem]">
        <p className="mt-1 text-[0.6875rem] leading-snug text-ink-dim">{alert.detail}</p>
        <p className="mt-1.5 text-[0.6875rem] leading-snug text-ink">{alert.fix}</p>
        {alert.command && <CopyableCommand command={alert.command} />}
      </div>
    </div>
  )
}

export function AlertStack(props: AlertInputs) {
  const alerts = deriveAlerts(props)
  const [dismissed, setDismissed] = useState<string[]>([])

  // Forget a dismissal once its alert is gone, so the same fault recurring is
  // shown again instead of being silently suppressed for the rest of the
  // session. Keyed on the id list so this only runs when the set changes.
  const live = alerts.map((a) => a.id).join("|")
  useEffect(() => {
    const ids = live ? live.split("|") : []
    setDismissed((d) => {
      const kept = d.filter((id) => ids.includes(id))
      return kept.length === d.length ? d : kept
    })
  }, [live])

  const shown = alerts.filter((a) => !dismissed.includes(a.id))
  if (shown.length === 0) return null

  return (
    <div
      // pointer-events-none on the container so the console underneath stays
      // clickable around the cards; each card turns them back on.
      className="pointer-events-none fixed bottom-2 right-2 z-50 flex flex-col gap-2"
    >
      {shown.map((a) => (
        <AlertCard
          key={a.id}
          alert={a}
          onDismiss={() => setDismissed((d) => [...d, a.id])}
        />
      ))}
    </div>
  )
}
