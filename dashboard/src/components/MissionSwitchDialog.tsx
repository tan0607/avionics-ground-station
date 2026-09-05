import { AlertDialog } from "radix-ui"
import { Button } from "@/components/ui/button"
import type { MissionState } from "@/hooks/useMission"
import type { RecorderState } from "@/hooks/useFlightRecorder"

export function MissionSwitchDialog({ mission, recorder }: {
  mission: MissionState
  recorder: RecorderState
}) {
  const target = mission.requested
  const unavailable = mission.supported && recorder.status !== "ok"
  const busy = mission.busy || recorder.busy

  return (
    <AlertDialog.Root open={Boolean(target)} onOpenChange={(open) => { if (!open) mission.cancel() }}>
      <AlertDialog.Portal>
        <AlertDialog.Overlay className="fixed inset-0 z-[var(--z-modal-backdrop)] bg-black/75" />
        <AlertDialog.Content onCloseAutoFocus={(event) => event.preventDefault()}
          className="fixed left-1/2 top-1/2 z-[var(--z-modal)] w-[calc(100%-2rem)] max-w-lg -translate-x-1/2 -translate-y-1/2 rounded-sm border border-hairline bg-surface p-5 text-ink">
          <AlertDialog.Title className="text-base font-medium">
            Switch to {target?.name} (Vehicle {target?.channel})?
          </AlertDialog.Title>
          <AlertDialog.Description className="mt-3 text-sm leading-relaxed text-ink-dim">
            This clears the current live graphs and readouts for {mission.mission.name}.
            New data will appear for {target?.name}. Saved flight logs are kept.
          </AlertDialog.Description>
          {recorder.recording && (
            <p className="mt-3 text-sm text-caution">
              The current recording will be saved and stopped before switching.
              Start a new recording for {target?.name} afterwards.
            </p>
          )}
          {(recorder.error || unavailable) && (
            <p role="alert" className="mt-3 text-sm text-caution">
              {unavailable ? "Recording status unavailable. Reconnect before switching." : recorder.error}
            </p>
          )}
          <div className="mt-5 flex flex-wrap justify-end gap-2">
            <AlertDialog.Cancel asChild>
              <Button variant="outline" disabled={busy}>Cancel</Button>
            </AlertDialog.Cancel>
            <Button disabled={busy || unavailable} onClick={() => void mission.confirm()}>
              {busy ? "Switching…" : "Confirm switch"}
            </Button>
          </div>
        </AlertDialog.Content>
      </AlertDialog.Portal>
    </AlertDialog.Root>
  )
}
