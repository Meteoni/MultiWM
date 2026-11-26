---
status: accepted
---

# BSP arrivals split the nearest tile that fits

A BSP arrival (new, restored, re-tiled or moved in) tries tiles in order and splits the first one that fits in either direction. With Automatic Window Insertion Position set to Focused tile, the order starts at the focused tile and moves outward through the tree: its sibling's subtree first, then each level up, tiles equally near largest first. A nearby tile that fits wins over a bigger one further away, because the point of the setting is that the new window opens where the user is working. With Biggest tile, or when nothing is focused, the order is largest to smallest. This replaces trying the selected tile and then only the largest other one (`TryRecoverSettlingBspWindow`), which floated a window that only the second-largest tile could hold.

When no tile fits, the monitor is full, and the window goes to Automatic overflow as in every other layout: Overflow Action and Try Other Monitors First decide, including a new desktop. BSP's two exceptions go: floating such an arrival in place, and never creating a desktop. Both were safe only while BSP tried two tiles, when "no tile fits" often meant "a tile we didn't try fits", and moving that window off the monitor would have been wrong. Manual mode still floats it in place.

Tile Workspace (Alt+T) uses the same order and, if a window still fits nowhere, rebuilds the tree evenly before giving up on it. That resets the user's splits, so it happens only when the alternative is overflow. A window it still can't tile goes to overflow like any other arrival.

## Considered Options

- **Largest tile first in Focused mode too**: the window can land far from the user's work whenever another tile is bigger.
- **Keep the two-step fallback**: cheap, but floats windows that some tile could hold.
- **Keep BSP's float-in-place and no-new-desktop exceptions**: no recorded reason beyond the weak fallback, and they made BSP the one layout that ignores the Overflow settings.
- **Exempt Tile Workspace from overflow**, floating what it can't tile where it is: a third rule beside Automatic and Manual, for users Manual mode already serves.
- **Best-effort tiling that ignores app minimums**, as bspwm or yabai do: rejected; an app's native minimum stays authoritative, and a window that can't meet it floats.
