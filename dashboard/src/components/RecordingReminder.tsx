import { CircleAlert, Circle } from "lucide-react"
import { Button } from "@/components/ui/button"
import type { RecorderState } from "@/hooks/useFlightRecorder"

/** Persistent across views; only a backend-confirmed recording clears it. */
export function RecordingReminder({
  liveSource, recorder, mission, onStart, switching = false,
}: {
  liveSource: boolean
  recorder: RecorderState
  mission: string
  onStart: () => void
  switching?: boolean
}) {
  if (!liveSource || (recorder.status === "ok" && recorder.recording)) return null
  const available = recorder.status === "ok" && Boolean(recorder.session)

  return (
    <div role="status" className="flex shrink-0 flex-wrap items-center gap-x-4 gap-y-2 border-b border-caution/50 bg-caution-bg px-4 py-2 text-caution">
      <CircleAlert className="size-4 shrink-0" aria-hidden />
      <div className="min-w-0 flex-1 text-xs">
        <p className="font-medium">
          {available ? `Not recording a flight · ${mission}` : "Recording status unavailable"}
        </p>
        <p className="mt-0.5 text-ink-dim">
          {available
            ? "Start a flight recording before launch. Session data is saved separately."
            : "Cannot verify the flight recorder. Check the backend connection."}
        </p>
        {recorder.error && <p className="mt-1 text-caution">Recording failed: {recorder.error}</p>}
      </div>
      {available && (
        <Button variant="outline" size="sm" className="border-caution/60 text-caution hover:text-caution"
          disabled={recorder.busy || switching} onClick={onStart}>
          <Circle className="size-3" aria-hidden />
          {recorder.busy ? "Starting…" : `Start recording ${mission}`}
        </Button>
      )}
    </div>
  )
}
