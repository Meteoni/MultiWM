# MultiWM architecture

This page is the map. Read it before your first change; it should take ten
minutes. For coding rules, units, and the glossary, see
[CONTRIBUTING.md](../CONTRIBUTING.md).

## One source file, many chapters

Windhawk compiles a mod from a single `.cpp` file. MultiWM keeps that single
file readable by splitting it into headers under `src/multiwm/`, which
[`src/multiwm/multiwm.cpp`](../src/multiwm/multiwm.cpp) includes in a fixed order. WHAM pastes them
together into `output/multiwm/multiwm.wh.cpp` for publishing.

Consequences you will run into:

- **A header is a chapter, not a standalone header.** It can use anything
  defined in an earlier chapter, and it will not compile on its own. Build
  `src/multiwm/multiwm.cpp` (or run the tests) instead.
- **Order matters.** If you need a function from a later chapter, declare it in
  [`core/forward_declarations.h`](../src/multiwm/core/forward_declarations.h).
- **Namespaces show ownership, not privacy.** Everything can see everything.
  Call across owners with the full name (`Platform::Windows::…`) so native
  effects stay visible; inside an owner, short names are fine.

## Folders and owners

The roots `src/multiwm/`, `src/companion/`, and `src/shared/` separate the two
mods from their shared code. Chapter paths below are relative to `src/multiwm/`,
except the sibling `shared/` and `companion/` roots.

| Folder | Namespace | Owns |
| --- | --- | --- |
| `core/` | (global), `Diagnostics`, `RuntimeLifecycle` | Settings struct, message IDs, enums, the WM actor state `g_wm`, model invariants |
| `model/` | `Model` | `Workspace` (membership, order, saved ratios) and `WorkspaceRepository` (all workspaces, who owns which window) |
| `layout/` | `Layout` | Pure rectangle math, planning with minimum sizes, learning ratios from resize gestures. No Win32 calls. |
| `placement/` | `Placement` | Applying a plan to real windows, floating geometry, and conformance leases |
| `reconcile/` | `Reconcile` | Turning observed OS state into model changes: discovery, ownership repair, initial-placement rules, overflow |
| `commands/` | `Commands`, `Gestures` | Hotkey/tray commands and interpreting user drags and resizes |
| `platform/` | `Platform::*` | The only code that talks to Windows: windows, monitors, WinEvent hooks, private virtual-desktop COM |
| `app/` | `Configuration`, `TrayUi`, `Diagnostics`, `RuntimeLifecycle`, `CompanionSnapshot` | Settings loading, tray icon, diagnostic reports, the WM thread and its message loop, publishing the companion snapshot |
| `windhawk/` | — | Windhawk's stock tool-mod launcher. Not MultiWM logic. |
| `shared/` | `SharedRules`, `CompanionAbi`, `SharedGeometry` | Code compiled by both mods: window-rule matching, the companion snapshot layout, and geometry/size-limit arithmetic. Windows and the standard library only. |
| `companion/` | `Companion` | The separate MultiWM Companion mod (`src/companion/multiwm-companion.cpp`). Never included by MultiWM. |

## The companion mod

WHAM builds every `.cpp` under `src/` as its own mod, so `src/companion/multiwm-companion.cpp`
becomes `output/companion/multiwm-companion.wh.cpp`. That mod is loaded into every
application and predicts placement before a new window is shown or a known tiled
window is restored.

Data flows one way. Between messages, the WM thread
(`CompanionSnapshot::PublishIfChanged`) checks repository revision, mode, desktop,
display changes and effective overflow routing. Changed inputs rebuild a snapshot;
changed contents are written with a seqlock. Unchanged contents skip the write.
There is no polling or request back to MultiWM. A named liveness event distinguishes
a running writer from a section kept alive only by readers.

The snapshot holds each current-desktop workspace's next-admission tile and default
floating frame, definite overflow-float predictions, known window ownership/treatment, rules and per-window
restore frames. Prediction uses MultiWM's layout planner with the monitor's physical
work area, DPI-scaled insets and gap. The companion starts from the application's
proposed geometry, queries its live size limits, and adjusts its original positioning
call. The native call retains its command, flags, batching and return value. An
unavailable hint passes through; MultiWM remains responsible for final placement.

All companion placement hints, including restoration, require Automatic mode.
In Manual mode the hooks pass placement and restore calls through unchanged;
MultiWM itself continues managing its existing groups under Manual policy.

The companion covers normal and asynchronous show calls, immediate and deferred
positioning with `SWP_SHOWWINDOW`, saved placements, and visible creation.
`SW_RESTORE` is eligible for unknown hidden windows; `SW_SHOWDEFAULT` uses the
startup show command. Cross-thread requests pass through without a hint, keeping
asynchronous shows nonblocking. Deferred batches retain their original commit
timing and returned handles. A local Windows probe found `AnimateWindow` reaches
the existing native positioning hook while hidden, so no animation hook is added.
Visible creation is best effort: the app may paint before creation returns.

For restoration, the publisher calls `Workspace::Restore(hwnd)` on a copy and
plans the result separately for each suspended member. This accounts for saved
order, weights, Dwindle ratios, master status and cached minimum/maximum sizes.
An unsatisfiable plan has no restore frame. The companion updates the normal
restore rectangle just before a supported owning-thread restore, retaining
visibility, min/max state and activation until the caller's original operation.
This preparation is not performed while a minimized window waits for restoration.

A known window hidden without minimizing uses the same published restore frame
and measured borders when it is shown again. Show calls position it while hidden;
immediate/deferred positioning and saved-placement calls have their geometry
rewritten. Deferred batches still wait for the application's commit. Known
windows returning to their workspace without a usable restore hint stay untouched.

Tracked entries identify HWND, PID, owning desktop/monitor and tiled/floating
treatment. The reader selects admission for unknown windows, restore for members
returning to their workspace, and arrival for tiled owners elsewhere. Borders
belong to the tracked entry, so inactive-desktop owners retain measurements too.
Ambiguous ownership, PID mismatch and retained floating treatment pass through.

Tiled migration uses the same AdmitTiled/AdmitTiledAfter insertion as fresh
admission. Suspended arrivals reserve that position with Suspend; hidden members
therefore honour After focused rather than silently appending. Once restored,
they share the destination's published admission tile, without an additional
layout plan for each inactive window or destination. Arrival prediction keeps the
record's treatment and does not rerun opening rules. Floating destinations and
destinations without a tiled hint pass through. Constraint fitting remains best
effort, and the published active desktop is assumed to be the destination; an
app/shell desktop switch during showing can still require later correction.

Resize borders come from measurements taken while each window was tiled and
restored. Their cached window DPI and frame styles must still match; the hint
must also match the workspace DPI. Missing measurements cause a skipped hint.
Visibility, min/max, enabled and topmost state do not invalidate saved restored
borders. Fresh measurements still require unchanged DPI and full styles throughout.
The shared geometry header converts reported limits from window-DPI units to
physical frame pixels. Both mods centre capped windows inside their tiles. The
companion's growth for a minimum larger than its predicted tile remains approximate;
MultiWM's constrained solver can subsequently choose another slot or float it.

Explicit show/placement calls and default `SC_RESTORE` handling are covered.
Real taskbar tests reached `DefWindowProcW(SC_RESTORE)` on the owning thread while
still minimized. This establishes an interception point for those tested apps;
it does not prove every application uses default handling. Restore-to-maximized,
Floating workspaces, unsupported thread contexts and unavailable hints pass through.

Local overflow-float hints cover definite capacity overflow and a minimum larger
than the work area. The direct-centre policy applies when routing is disabled, or
there is exactly one monitor with desktop creation off. General routed-overflow
destinations and full geometry-based admission prediction remain deferred; see
[the plan](VFX_COMPANION_PLAN.md).

ABI 5.0 uses fixed-width structures with layout assertions for x86 and x64. Bounds
are 16 workspaces, 2048 tracked HWNDs, 128 restore hints and 32 KiB of encoded rules.
The reader caches the body by generation and writer PID and decodes rules only
when their bytes change. ABI 5 uses 64-byte tracked entries and 32-byte restore
entries; its fixed snapshot section is 169160 bytes. Ownership publication walks
repository metadata and validates cached borders, without planning inactive
workspaces. The larger body is copied only on generation/writer changes.
A major-version mismatch gives no hint; both mods must be updated together.

Diagnostics expose ABI, section/liveness state, published contents, rebuild/write
counts, burst and unchanged rebuilds, and average/slowest rebuild time. The measured
workload did not justify queue-drain coalescing; event-driven scheduling remains.

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
  top of [`core/shared_types.h`](../src/multiwm/core/shared_types.h). If a change can't
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

`tests/run.ps1 -CompilerRoot <Windhawk Compiler>` builds layout, reconciliation,
companion, and restore suites. The first two include `src/multiwm/multiwm.cpp` directly;
the others include `src/companion/multiwm-companion.cpp`. They use real code with
fake desktop COM objects, test-owned hidden windows, and recorded placement
calls. They never move other applications' windows. Run them before every commit.
The restore suite uses app-cloaked test windows on a private Win32 desktop and
never switches the input desktop.
The runner starts every test with Microsoft's signed-DLL-only process policy
and verifies it before resuming execution. This blocks installed Windhawk mods
without changing their configuration. Test executables must remain statically
linked; run them through `tests/run.ps1` rather than directly.
