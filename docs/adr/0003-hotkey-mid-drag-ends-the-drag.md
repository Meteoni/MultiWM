# A hotkey pressed mid-drag ends the drag of windows whose tiles it moves

A user layout mutation committed while a window is being dragged or resized ends that window's gesture if the change moves its tile (`Placement::WindowsWhoseTilesChange`, called by `CommitUserLayoutMutation`): the window goes to its new tile and releasing the mouse does nothing. The gesture started from the old tile, so applying it afterwards counts the hotkey twice; a resize begun at y=400, released at y=300 after Grow moved the divider to y=330, would land near y=230. Pressing a hotkey while holding the mouse button is rare enough that cancelling is the safe reading, so a drop the user was still aiming is lost: that is accepted, not a bug.

## Considered Options

- **Leave the drag running** for size commands and layout changes, as before 6c5aed0 (only Promote, Set Master and Rotate Split cancelled): keeps the drop, but a mid-resize hotkey is learned twice.
- **Never cancel**: the same double count after Promote as well.
