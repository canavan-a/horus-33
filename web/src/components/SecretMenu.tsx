import { useAppMode } from '@/hooks/useAppMode'
import {
  AlertDialog,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog'
import { Label } from '@/components/ui/label'
import { Switch } from '@/components/ui/switch'

interface SecretMenuProps {
  open: boolean
  onOpenChange: (open: boolean) => void
}

// Hidden menu reached by tapping the header logo six times. Its only control is
// the app-wide simple/advanced switch.
export function SecretMenu({ open, onOpenChange }: SecretMenuProps) {
  const [mode, setMode] = useAppMode()

  return (
    <AlertDialog open={open} onOpenChange={onOpenChange}>
      <AlertDialogContent>
        <AlertDialogHeader>
          <AlertDialogTitle>display mode</AlertDialogTitle>
          <AlertDialogDescription>
            simple hides the camera controls and locks the clip-recording toggle — the
            stream and saved clips stay viewable.
          </AlertDialogDescription>
        </AlertDialogHeader>

        <div className="flex items-center justify-between gap-3">
          <Label htmlFor="secret-mode">advanced mode</Label>
          <Switch
            id="secret-mode"
            checked={mode === 'advanced'}
            onCheckedChange={(v) => setMode(v ? 'advanced' : 'simple')}
          />
        </div>

        <AlertDialogFooter>
          <AlertDialogCancel>done</AlertDialogCancel>
        </AlertDialogFooter>
      </AlertDialogContent>
    </AlertDialog>
  )
}
