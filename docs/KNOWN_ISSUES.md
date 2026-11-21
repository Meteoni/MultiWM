# Known issues

Behavior that is understood but not yet resolved.

## Switching to the Floating layout stacks windows that have never floated

A window has no remembered floating frame until it has been floated once (by
Float Focused, a drag that floats it, or an earlier time in the Floating
layout). When a workspace switches to the Floating layout,
`RestoreWorkspaceFloatingGeometry` centers every such window at the same frame:
the override-size rule's size, or the configured default floating size. Several
never-floated windows therefore land on exactly the same rectangle, one on top of
another. A suspended member restored into a Floating workspace is centered the
same way.

MultiWM deliberately records no floating frame when it first admits a window. The
frame a window has at that moment is often a placeholder (some apps show at the
system minimum size and resize afterwards) or, with the companion, already its
tile. Floating sizes come only from window rules, the configured default, or a
frame the window has actually floated at.

Possible resolutions: keep each window at its current frame when the layout
switches, or cascade them.

## A tile that covers the whole monitor can be taken for fullscreen

`GetPhysicalSuspensionReason` (platform/native_windows.h) treats a window whose
window rectangle covers its monitor, within 2 px on every edge, as Maximized.
That is how apps that go fullscreen without maximizing (browser F11, games,
video players) are suspended with their slot saved. A tiled window can match the
same test when three things hold at once:

- **The tile is the whole monitor.** The work area must be the monitor, which it
  is when the taskbar is hidden or auto-hidden, and the insets must be 0 on every
  edge (1 DIP at 200% scaling is 2 px, inside the tolerance).
- **The window has the work area to itself:** a lone window in any layout, or
  every window in Monocle. The gap plays no part.
- **The window has no invisible resize border,** so its window rectangle equals
  its visible frame. Windows with the usual border extend past the tile by more
  than the tolerance and are not affected. Which real apps lack the border has
  not been checked; apps that draw their own title bar are the likely cases.

`EffectiveSuspensionReason` (reconcile/reconcile.h) already says a tiled, not
truly maximized window that sits exactly on its own tile is not fullscreen, but
only the participation check and overflow's trial use it. About eleven other
checks call `GetPhysicalSuspensionReason` directly. So the model keeps the window
tiled while those checks treat it as fullscreen.

Observed in a test (work area = monitor, insets 0, a borderless test window): a
lone window tiled to the whole monitor reads as Maximized but stays tiled. When
a second window is admitted, the second gets the right half while the first
stays full-screen underneath it, because arrangement (`PlaceTiledWindow`) skips
windows that look fullscreen.

Expected from the code, not tested:

- A layout change, including leaving Monocle, doesn't move such a window.
- Conformance leases cancel instead of snapping it back.
- Discovery snapshots and Tile Workspace leave it out, and Tile Workspace then
  skips adopting the current geometry.
- With the companion, a new window in an empty workspace is shown on its
  predicted tile, the whole monitor, and is then admitted as suspended-maximized
  instead of tiled. Each following window would get the whole monitor too.

Chosen direction, not yet done: limit the insets so a tile can't cover the
monitor. Only when all four insets would be 2 px or less, raise them to 3 and say
so in the settings toast; every other setup is left alone.

Considered and set aside:

- Applying the on-its-own-tile exception in every state check. That fixes
  managed windows, but a new window has no tile yet to compare against.
- GlazeWM's rule: fullscreen only when the visible frame extends past the area
  the WM tiles into, so its own tiles can never match. With zero insets and a
  hidden taskbar, that area is the monitor, and a real fullscreen app is then not
  detected at all.
