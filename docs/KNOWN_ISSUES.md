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

## A new window can jump when its minimum size exceeds the predicted tile

This applies to the layouts other than BSP. BSP publishes every tile an arrival
tries with the splits above it, and both mods fit the window's minimum to them
with the same code (shared/split_geometry.h), so the companion's tile is MultiWM's. MultiWM publishes one predicted admission tile
per workspace before it knows the new window's size limits. The companion reads the window's live minimum when
the app shows it. In Dwindle, if that minimum exceeds the predicted tile but
fits the work area, the companion can grow the rectangle toward neighboring
tiles. MultiWM then checks the existing windows' constraints and may adjust
dividers, choose another insertion point, or float the new window. The
correction can be visible.

A possible fix is to share Dwindle's fitting code and publish the existing
windows' minimum sizes in tile order, the preferred insertion index, and the
divider ratios and settings for admission. When the live minimum exceeds the
published tile, the companion could use those inputs to try the same insertion
points in the same order as MultiWM.

## BSP: windows restored together can jump

Each published BSP restore frame assumes that window is the only one returning.
When several return at once (Win+Shift+M, or several taskbar clicks in quick
succession), the second and later ones use frames computed before the first was
placed, so MultiWM moves them after they appear. With "After focused" they also
all split the same focused window, halving it each time. Minimizing gives up a
window's place, so restoring them never rebuilds the earlier layout either.

A possible fix: treat restores that arrive within one reconcile burst as one
arrival group, splitting the largest tiles in turn, and publish restore frames
for that case.

## BSP: an arrival right after another may get no hint

Until a window holds its tile, arrangement may still turn its new split to fit a
later arrival, which the published tiles don't model. While another arrival is
settling, the companion therefore uses only the first published tile, which needs
no turn; a window that doesn't fit that one gets no hint and shows where the app
puts it, then MultiWM moves it.
