# MultiWM architecture

This page is the map. Read it before your first change; it should take ten
minutes. For coding rules, units, and the glossary, see
[CONTRIBUTING.md](../CONTRIBUTING.md).

## One source file, many chapters

Windhawk compiles a mod from a single `.cpp` file. MultiWM keeps that single
file readable by splitting it into headers under `src/`, which
[`src/multiwm.cpp`](../src/multiwm.cpp) includes in a fixed order. WHAM pastes them
together into `output/multiwm.wh.cpp` for publishing.

Consequences you will run into:

- **A header is a chapter, not a standalone header.** It can use anything
  defined in an earlier chapter, and it will not compile on its own. Build
  `src/multiwm.cpp` (or run the tests) instead.
- **Order matters.** If you need a function from a later chapter, declare it in
  [`core/forward_declarations.h`](../src/core/forward_declarations.h).
- **Namespaces show ownership, not privacy.** Everything can see everything.
  Call across owners with the full name (`Platform::Windows::…`) so native
  effects stay visible; inside an owner, short names are fine.

## Folders and owners

| Folder | Namespace | Owns |
| --- | --- | --- |
| `core/` | (global), `Diagnostics`, `RuntimeLifecycle` | Settings struct, message IDs, enums, the WM actor state `g_wm`, model invariants |
| `model/` | `Model` | `Workspace` (membership, order, saved ratios) and `WorkspaceRepository` (all workspaces, who owns which window) |
| `layout/` | `Layout` | Pure rectangle math, planning with minimum sizes, learning ratios from resize gestures. No Win32 calls. |
| `placement/` | `Placement` | Applying a plan to real windows, floating geometry, and conformance leases |
| `reconcile/` | `Reconcile` | Turning observed OS state into model changes: discovery, ownership repair, initial-placement rules, overflow |
| `commands/` | `Commands`, `Gestures` | Hotkey/tray commands and interpreting user drags and resizes |
| `platform/` | `Platform::*` | The only code that talks to Windows: windows, monitors, WinEvent hooks, private virtual-desktop COM |
| `app/` | `Configuration`, `TrayUi`, `Diagnostics`, `RuntimeLifecycle` | Settings loading, tray icon, diagnostic reports, the WM thread and its message loop |
| `windhawk/` | — | Windhawk's stock tool-mod launcher. Not MultiWM logic. |

Comparable projects use the same split. i3 has `handlers.c` (events),
`commands.c`, `workspace.c` (model), `render.c` (layout) and `x.c` (pushing to
the display server). GlazeWM has `events/`, `commands/` and `models/`.

## How an event becomes window movement

Everything below runs on **one thread**, the WM thread. Windows callbacks
never change the model directly; they post a message and return.

```
Windows                       platform/window_events.h   WinEventProc filters, QueueWindowEvent posts
  │ WinEvent / VD notification / hotkey
  ▼
WM thread message loop        app/wm_thread.h            HandleWmThreadMessage dispatches by message
  │
  ├─ window event ──► reconcile/reconcile.h   ProcessWindowLifecycleEvent marks the HWND dirty,
  │                                           ScheduleLifecycleReconcile waits ReconcileDelayMs,
  │                                           then ReconcileDeferredLifecycle handles the burst
  ├─ desktop switch ► reconcile/reconcile.h   HandleVirtualDesktopChanged
  ├─ drag/resize ───► commands/commands.h     ApplyUserMoveSize (swap, float, or learn ratios)
  └─ hotkey ────────► commands/commands.h     TileWindows, CycleCurrentWorkspaceLayout, …
          │
          ▼  model changed: save the Workspace, then
placement/arrangement.h       ArrangeWorkspace
  │  Layout::BuildWorkspaceLayoutPlan    rectangles from saved ratios + minimum sizes
  │  Platform::Windows::PlaceWindowChecked   SetWindowPos, then observe the result
  ▼
placement/conformance.h       BeginConformanceLease  keep the window on its tile for a while
```

The rule that keeps this safe: **observe, decide, re-check, commit.** Native
calls (especially COM and `WM_GETMINMAXINFO`) can run other code and deliver
events, so a window can close or move desktop while you wait. Code re-checks
with helpers such as `Platform::Windows::IsSameLiveWindow` and
`Platform::VirtualDesktop::IsWindowOnDesktop` before it saves or places
anything. When you see a check repeated after a native call, that is why.

## Working with the model

- `g_workspaces.Load(key, &workspace)` returns a **copy**. Change the copy, then
  `g_workspaces.Save(key, workspace)` to commit it. Nothing changes until you save.
- A workspace is keyed by `DesktopMonitorKey` (virtual desktop GUID + monitor ID).
- Change membership only through `Workspace` methods (`AdmitTiled`, `Float`,
  `Suspend`, `Restore`, `Forget`, …). They maintain the invariants listed at the
  top of [`core/shared_types.h`](../src/core/shared_types.h). If a change can't
  be explained by those invariants, it probably belongs in `reconcile/`.

## Where to start for common changes

| You want to… | Start in |
| --- | --- |
| Add a layout or change how one divides space | `layout/algorithms.h`, then `layout/planning.h` (`BuildWorkspaceLayoutPlan`) |
| Change what happens when a window appears | `reconcile/reconcile.h` (admission helpers near the top) |
| Change a window rule's effect | `app/configuration.h` (parsing) and `platform/native_windows.h` (`FindMatchingWindowRule`) |
| Add a hotkey or tray command | `core/shared_types.h` (IDs), `app/configuration.h`, `app/wm_thread.h`, `commands/commands.h` |
| Investigate a bug report | `app/diagnostics.h`: the report lists counters, workspaces and recent events |

## Tests

`tests/run.ps1 -CompilerRoot <Windhawk Compiler>` builds two suites that
include `src/multiwm.cpp` directly. They use the real model and planner with
fake desktop COM objects, and never move other applications' windows. Run them
before every commit.
