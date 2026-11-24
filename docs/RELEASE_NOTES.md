# Release notes

## Next release (MultiWM 1.16.1, Companion 0.3.2)

- **The `bsp` layout setting now means the new BSP layout.** Configurations
  saved before 1.13.29 used `bsp` for Dwindle; those now open in BSP. Choose
  Dwindle again in the layout settings to keep the old behaviour.
- MultiWM and MultiWM Companion use snapshot ABI 6. Update both together; a
  Companion on ABI 5 gives no hints.
- Toasts about an app show its icon before the text, and name the window in
  the BSP "no tile fits" and "couldn't return to its desktop" toasts. Several
  toasts are shorter.
- The tray icon is drawn at the taskbar's icon size with its own smoothing, so
  it stays sharp at 125-175% scaling, in light mode, and with font smoothing
  off (as in many VMs).
