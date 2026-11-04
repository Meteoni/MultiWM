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

`src/multiwm.cpp` includes the implementation in dependency order and is the entry point for Windhawk amalgamation. The headers share one translation unit.

The headers are dependency-ordered implementation/declaration fragments for this
Windhawk build. They are read through the entry point, rather than compiled in
isolation. Namespaces identify a few shared responsibilities across files.

| Responsibility / namespace | Files under `src/includes` |
| --- | --- |
| Shared declarations and actor state | `multiwm_core.h`, `multiwm_workspace.h`, `multiwm_runtime_state.h`, `multiwm_interfaces.h` |
| `Model`: membership, preferences, validation and snapshot storage | `multiwm_workspace_model.h`, `multiwm_workspace_repository.h` |
| `Layout`: allocation, cached constraint planning and resize learning | `multiwm_layout.h`, `multiwm_workspace_planning.h` |
| `Placement`: floating geometry, tiled arrangement and conformance leases | `multiwm_floating_placement.h`, `multiwm_workspace_arrangement.h`, `multiwm_conformance.h` |
| `Reconcile`: observed ownership, discovery and routing | `multiwm_reconcile.h`, `multiwm_window_routing.h` |
| `Platform`: native adapters | `multiwm_monitors.h`, `multiwm_windows.h`, `multiwm_window_events.h`, `multiwm_virtual_desktop.h` |
| Settings and gestures | `multiwm_configuration.h`, `multiwm_gestures.h` |
| Commands and runtime lifecycle | `multiwm_commands.h`, `multiwm_bootstrap.h`, `multiwm_runtime_lifecycle.h` |
| Presentation and diagnostics | `multiwm_tray_ui.h`, `multiwm_diagnostics.h` |

`multiwm_workspace.h` declares the model. Runtime instances and diagnostic state
live in `multiwm_runtime_state.h`; the small interfaces fragment resolves calls
that must precede their definitions. Model resize methods are defined explicitly
inside `namespace Model` beside the planner they use. Native monitor conversion
methods likewise stay beside the monitor adapter. Namespaces communicate
ownership within the shared translation unit, rather than enforcing privacy.

Keep layout preferences in the model and temporary constraint adjustments in the planner. Recovery searches produce local candidate workspaces; the caller commits them after retaining any required rollback state. Native queries can outlive a desktop transition, so retain the ownership checks at discovery and migration boundaries.

See [Maintaining MultiWM](CONTRIBUTING.md) for readability rules, contracts, units,
ownership, the project glossary, and review tooling.

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


## Also check out
- [Virtual Desktop Helper by u2x1](https://windhawk.net/mods/virtual-desktop-helper)
- [Taskbar Desktop Indicator by Simon Benedict](https://windhawk.net/mods/taskbar-desktop-indicator)
- [GNOME-like dynamic virtual desktops by Giggig](https://windhawk.net/mods/gnome-dynamic-desktops)
