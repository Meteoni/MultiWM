# MultiWM

MultiWM is a continuation of the original [Tiling Helper](https://windhawk.net/mods/tiling-helper) by [U2X1](https://github.com/u2x1).

MultiWM is a Windhawk mod instead of a standalone app. To run it, download Windhawk [here](https://github.com/ramensoftware/windhawk).

A lightweight, low-cortisol window manager for Windows 11 with true per-virtual-desktop layouts - 
including floating - and simple, predictable controls. 

![GIF](https://raw.githubusercontent.com/Meteoni/meteoni-assets/main/MultiWM/MultiWM.gif)

## Notes
- *This repository is used to store the source code of MultiWM only. Contributions are welcome.*
- *Source amalgamation in this project is done automatically through [WHAM](https://github.com/Meteoni/WHAM-VSCode). Download [WHAM on VSCode Marketplace.](https://marketplace.visualstudio.com/items?itemName=meteoni.windhawk-amalgamation)*
- *MultiWM's virtual-desktop notification integration is based on [Taskbar Desktop Indicator by Simon Benedict](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-desktop-indicator.wh.cpp) (MIT).*

## Features
- **First-class floating** - use Floating as a whole-workspace layout 
or float individual windows while retaining useful geometry.
- **Automatic or Manual management** - let MultiWM continuously discover 
and migrate windows, or keep explicitly tiled groups under your control.
- **True virtual-desktop layouts** - uses Windows' existing virtual desktops 
instead of implementing a separate desktop system.
- **Event-driven** - changes are signaled through native events rather than 
continuous polling.

## Source layout

Windhawk builds a mod from one source file. `src/multiwm/multiwm.cpp` holds the mod's
metadata and settings, and includes the headers below in a fixed order; WHAM
combines them into `output/multiwm/multiwm.wh.cpp`. Each header is a chapter of that one
file, so build `src/multiwm/multiwm.cpp` rather than a header on its own.

```
src/
  multiwm/
    multiwm.cpp   Windhawk metadata, settings schema, entry points, include order
    core/         shared types, settings, messages, WM thread state
    model/        Workspace and WorkspaceRepository
    layout/       rectangle math, constraint planning, resize learning
    placement/    floating geometry, applying layouts, conformance leases
    reconcile/    discovery, ownership repair, window routing and overflow
    commands/     hotkey/tray commands and drag/resize gestures
    platform/     Windows APIs: windows, monitors, WinEvents, virtual desktops
    app/          settings loading, tray UI, diagnostics, WM thread lifecycle
    windhawk/     Windhawk's standard tool-mod launcher
  companion/      MultiWM Companion entry point, snapshot reader and show hooks
  shared/         rule matching, companion snapshot layout, geometry and size limits
tests/            regression suites (run with tests/run.ps1)
```

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit
together, then [CONTRIBUTING.md](CONTRIBUTING.md) for coding rules, units, the
glossary, and testing.

## Layouts
- Floating
- Master + Stack (vertical or horizontal)
- Dwindle
- Columns / Rows
- Monocle

## Getting Started
Move a tiled window to **float it** or **swap it with the window underneath**. 

By default, hotkeys are available for tiling (Alt+T), changing layouts (Alt+L), setting the master window (Alt+M),
moving windows through the logical order (Alt+, / .), floating a window (Alt+F), and switching between Automatic
and Manual management (Alt+R).

A small tray indicator shows the active workspace layout and provides quick access to layouts and management mode.

## Optional MultiWM Companion

[MultiWM Companion](src/companion/multiwm-companion.cpp) is a separate mod that
reduces opening and restoration jumps in Automatic mode. It reads MultiWM's
published rectangles, including your gap and insets, and prepares a window
before supported show/restore operations. In Manual mode it passes calls through.

Restore hints use the window's saved slot and weights, cached size limits, and
measured resize borders. New windows also respect live minimum/maximum sizes
and DPI scaling. Definite local overflow floats can be centred early; general
geometry-based overflow and routing to another monitor can still need correction.
Both mods must use snapshot ABI major 4; ABI 4.0 and 4.1 interoperate, with the new
overflow hints available only when both sides support 4.1.

Build the companion separately; WHAM produces
`output/companion/multiwm-companion.wh.cpp`. The companion requires a running
MultiWM and does not replace its membership, layout or conformance handling.

For runtime measurements, configure **Write Diagnostic Report** in the hotkey
settings and use the shortcut. Reports include companion ABI/liveness state and
rebuild costs. See [the plan](docs/VFX_COMPANION_PLAN.md) for known limitations
and the remaining live-check checklist.


## Also check out
- [Virtual Desktop Helper by u2x1](https://windhawk.net/mods/virtual-desktop-helper)
- [Taskbar Desktop Indicator by Simon Benedict](https://windhawk.net/mods/taskbar-desktop-indicator)
- [GNOME-like dynamic virtual desktops by Giggig](https://windhawk.net/mods/gnome-dynamic-desktops)
