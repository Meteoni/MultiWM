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
