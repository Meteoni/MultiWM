# MultiWM VFX companion: plan

Status: the move/size part (Parts A and B) is implemented as a first version;
borders and animation are not. Decisions made while building it, which override
the text below where they differ:

- **File layout.** WHAM builds each `.cpp` under `src/` as its own mod: the
  companion is `src/companion/multiwm-companion.cpp` with chapters in `src/companion/`.
  Shared code is in `src/shared/`.
- **No tool process yet.** Without borders, the companion is only the part
  injected into apps. The change event and the border list are left for Part C.
- **No polling.** MultiWM republishes when a workspace is saved, the mode or
  desktop changes, or the tray window reports a display, DPI or work-area change.
- **One-shot, no per-window state.** One decision when a window goes from hidden to
  shown. Windows MultiWM already knows are skipped through the snapshot's tracked
  list, so no window tagging is needed. The "re-apply" idea in B4 is dropped.
- **Frame bounds before show (risk 1).** Use `DWMWA_EXTENDED_FRAME_BOUNDS` when
  available, otherwise use the window rectangle. Live app testing is still needed
  for hidden-window frame margins and mixed-DPI moves.
- **Window size limits (B3 step 6).** Query `WM_GETMINMAXINFO` on the window's
  owning thread and clamp the hinted size to its limits, preserving the predicted
  origin. MultiWM may refine the layout afterward. Reentrant shows pass through;
  if the query changes or destroys the window, abandon the hint.
- **Show calls.** `SetWindowPos`, `DeferWindowPos` and `SetWindowPlacement` predict from the app's
  proposed geometry, then adjust that original call. No preliminary move or hide
  is performed, and an unavailable hint leaves the call unchanged.
- **Additional paths.** Hook `NtUserShowWindowAsync` separately and adjust
  `DeferWindowPos` arguments without committing its batch. Classify `SW_RESTORE`
  and resolve explicit `SW_SHOWDEFAULT` from startup information. A process-local
  import probe on Windows confirmed that asynchronous and deferred shows bypass
  the original native show/position hooks, while `AnimateWindow` reaches the
  position hook with `SWP_SHOWWINDOW` while still hidden. No animation hook is needed
  on that build. Visible creation still cannot guarantee placement before app code
  paints during creation.
- **Same thread only.** The companion only moves a window when the calling thread
  owns it.

## Goals

1. **No first jump.** A window that MultiWM will manage opens directly at the
   rect MultiWM would give it: the next tiled slot, or the default floating rect.
2. **Custom borders** on managed windows, drawn by a separate process.
3. **Open animations** that land on the window's real destination, either through
   [Mobile Open Animation](../output/mobile-open-animation.wh.cpp) (MOA) or the
   companion's own implementation later.

## Non-goals

- Repairing other windows immediately. They reflow on MultiWM's normal schedule
  (`ReconcileDelayMs` after the new window is admitted).
- Any communication from the companion back to MultiWM.
- Animating MultiWM's own tiling moves (swaps, layout changes).

## Principles

- **MultiWM stays the authority.** The companion only places a window *before*
  MultiWM sees it. If the hint is wrong, MultiWM corrects the window exactly as it
  does today, so a bad hint costs at most one jump, the same as having no companion.
- **One-way data flow.** MultiWM writes a snapshot; the companion reads it. MultiWM
  never reads anything the companion writes.
- **MultiWM's cloak handling stays as it is.** "Cloaked means not visible yet, wait"
  is correct. Because the window is already at its destination when it is revealed,
  admitting it is a no-op placement
  ([`PlaceWindowChecked`](../src/multiwm/platform/native_windows.h#L670-L673) finds it in
  place).
- **The code inside apps stays minimal.** It runs in every process and a crash takes
  the app down.

## Architecture

```
MultiWM (tool process, WM thread)
  │  writes after each model change / arrange / focus / desktop / mode change
  ▼
Shared memory "Local\MultiWM.Vfx.v1"        (read-only for everyone else)
Named event   "Local\MultiWM.Vfx.v1.Changed" (signalled after each write)
  │                                   │
  ▼                                   ▼
Companion, injected part (@include *)   Companion, tool part (UI-access host)
  reads at show time; places the window   waits on the event; draws borders
  before it is shown; later, animation
```

Both halves of the companion are one Windhawk mod, split by command line the way
[Window Centralizer](../example_mod.wh.cpp) splits its hooks from its host process,
using the tool-mod launcher pattern.

---

## Part A: MultiWM changes

### A1. Shareable window-rule header

Make the rule matcher pure so both mods compile the same code.

- New chapter, e.g. `src/multiwm/core/window_rules.h`, with no `g_settings`, `g_wm`,
  `Wh_*` or Win32 calls:
  - `struct WindowFacts { std::wstring process, className, title; DWORD style, exStyle; bool hasOwner, isRoot; }`
  - `struct RuleSet { std::vector<WindowRule> rules; }`
  - `const WindowRule* MatchRule(const RuleSet&, const WindowFacts&, WindowRuleTreatment)`
  - `bool IsStructurallyManageable(const WindowFacts&, ...)`: the pure part of
    [`WindowCanBeManaged`](../src/multiwm/platform/native_windows.h#L540), including
    `kIgnoredWindowClasses`.
  - `SerializeRules(const RuleSet&, uint8_t* out, size_t cap)` / `DeserializeRules(...)`.
- [`FindMatchingWindowRule`](../src/multiwm/platform/native_windows.h#L444) becomes a thin
  wrapper: gather `WindowFacts` lazily (keep the current lazy process-name lookup),
  then call `MatchRule`.
- The companion fills `WindowFacts` cheaply inside the app (process name from
  `GetModuleFileName`).
- How both WHAM projects include one file: see Open questions.

### A2. Next-slot prediction

New function in `layout/` (pure, no Win32):

```cpp
struct NextSlotPrediction {
  bool tiledValid = false;   RECT tiledFrame{};     // visible frame, physical px
  bool floatingValid = false; RECT floatingFrame{}; // Floating-layout default
};
NextSlotPrediction PredictNextAdmission(const Workspace&, const RECT& workArea,
                                        LONG gap, UINT dpi);
```

- **Tiled layouts:** take a copy of the workspace (`Load` already returns one),
  admit a placeholder record the same way
  [`AdmitUntrackedSnapshot`](../src/multiwm/reconcile/reconcile.h#L232) would (`AdmitTiled`,
  or `AdmitTiledAfter(LastFocusedTiledWindow())` when `after_focused` is set), run
  [`BuildWorkspaceLayoutPlan`](../src/multiwm/layout/planning.h#L221), and take the
  placeholder's rect. The placeholder has no constraints (`valid = false`), so
  adaptive Dwindle plans as if the new window fits. The companion checks that
  (see B3).
  - The placeholder needs a value that can never be a real HWND. Confirm `Workspace`
    methods do nothing native with it.
- **Floating layout:** `FloatingDefaultSize` scaled by `dpi` and centered in the
  work area, clamped like
  [`RepairFloatingGeometry`](../src/multiwm/placement/floating.h#L72) with
  `NewWindowCenter`. Size-override rules are applied by the companion through A1.
- **No prediction (both invalid)** when:
  - Manual mode is on
  - the workspace is at its overflow limit (the overflow action is not predicted in v1)
  - the layout is Monocle with a constraint failure (Monocle is otherwise trivial:
    the work area)
- **Tests** (`tests/reconciliation_tests.cpp`): for each layout × {last_slot,
  after_focused} × {0..5 existing windows}, the predicted rect equals the rect the
  real admission and arrange produce for a fake window with no constraints. This
  test is what keeps the prediction honest as the planner changes.

### A3. Snapshot publisher

New chapter, e.g. `src/multiwm/app/vfx_snapshot.h` (or `platform/`, since it is a native
object).

Layout: fixed size, plain data, little-endian, append-only:

```cpp
struct VfxHeader {
  uint32_t magic;            // 'MWFX'
  uint16_t abiMajor, abiMinor;
  uint32_t totalSize;
  uint32_t writerPid;        // lets readers detect a restarted MultiWM
  volatile LONG seq;         // seqlock: odd while writing
  uint32_t flags;            // Running | AutomaticMode
  uint64_t generation;       // bumps on every publish
  GUID     currentDesktop;
  uint32_t workspaceCount;   VfxWorkspace workspaces[16];   // current desktop only
  uint32_t borderCount;      VfxBorder    borders[512];
  uint32_t rulesBytes;       uint8_t      rules[32768];     // A1 serialization
};
struct VfxWorkspace {
  RECT monitorRect;          // readers match by rect, not HMONITOR
  RECT workArea;             // what MultiWM plans in (insets applied)
  uint32_t dpi;
  uint8_t  layout;           // TileLayout
  uint8_t  tiledValid, floatingValid, reserved;
  RECT tiledFrame, floatingFrame;
};
struct VfxBorder {
  uint64_t hwnd;
  uint8_t  kind;             // Tiled | Floating
  uint8_t  focused;
  uint16_t reserved;
};
```

- **Seqlock write:** `InterlockedIncrement(seq)` (odd), write, `InterlockedIncrement(seq)`
  (even), then `SetEvent(changed)`. Writes happen on the WM thread only.
- **When it publishes:** after `ArrangeWorkspace`, after the focus anchor changes,
  after a desktop switch is reconciled, after a mode toggle and after a settings
  load. Merge bursts into one write per message-loop turn.
- **Lifetime:** create the section and the event once per process (next to
  `Diagnostics::InitializeProcessRuntime`), not per WM thread.
  `WhTool_ModSettingsChanged` restarts the WM thread, so clear `Running` while it
  is stopped. On uninit, clear `Running`, publish once, then close.
- **Security:** readable by every integrity level and by AppContainer apps; writable
  by MultiWM only. Draft SDDL: `D:(A;;GA;;;OW)(A;;GR;;;WD)(A;;GR;;;AC)S:(ML;;NW;;;LW)`.
  Verify AppContainer access (see Risks).
- **Diagnostics:** add counters for publishes and torn-read-free generations, and
  put the current generation in the diagnostic report.

### A4. Optional: more accurate desktop-switch hint

In [`OnLifecycleEvent`](../src/multiwm/platform/window_events.h#L171), set
`TransitionPending` on `EVENT_OBJECT_CLOAKED` only when the window's cloak flags
include `DWM_CLOAKED_SHELL`. This only cuts extra Explorer queries when apps or
animation mods cloak windows. Uncloak events can't be classified, because the flags
are already clear when they arrive, so leave those alone. Independent of the
companion; do it whenever convenient.

---

## Part B: companion, injected part (`@include *`)

### B1. Hooks

Use the show paths MOA and Window Centralizer both found necessary:

- `ShowWindow` (or `NtUserShowWindow`), `ShowWindowAsync`
- `SetWindowPos` with `SWP_SHOWWINDOW`
- `SetWindowPlacement`
- `CreateWindowExW` / `CreateWindowExA` with `WS_VISIBLE`

Process exclusions: start from MOA's `@exclude` list (shell hosts,
`msedgewebview2.exe`, ...).

### B2. Reading the snapshot

- Open the section lazily on the first candidate show, then cache it. Reopen if
  `writerPid` changed or `magic` / `abiMajor` don't match. If it can't be opened,
  do nothing.
- Seqlock read: read `seq`; if odd, retry; copy the parts you need; read `seq`
  again; if it changed, retry. After 4 attempts, skip the hint.
- Never block, wait or call MultiWM.

### B3. Deciding the hint

All of the following must hold, otherwise do nothing:

1. `Running` and `AutomaticMode` are set.
2. The window passes `IsStructurallyManageable` (A1), is not shown maximized or
   minimized, and has no Exclude rule.
3. No Open-on-monitor/desktop rule matches. MultiWM will route the window
   elsewhere anyway.
4. Its proposed rect's monitor (`MonitorFromRect`, compared by `rcMonitor`) matches
   a snapshot workspace.
5. Target rect:
   - Always-floating or Default-to-floating rule: the window's own size centered in
     `workArea`, matching `CenterPreservingSize`.
   - Workspace layout is Floating: `floatingFrame`, or the size-override rule's size
     scaled by `dpi`.
   - Otherwise `tiledFrame`, if valid.
6. The target is at least the window's minimum track size (`WM_GETMINMAXINFO`).
   Otherwise MultiWM's adaptive fitting would pick a different rect, so skip.

### B4. Placing the window

- The hint is a **visible frame** rect; `SetWindowPos` needs the window rect,
  including invisible resize borders.
  - **Before show:** estimate the borders. For `WS_THICKFRAME` windows, left, right
    and bottom are `SM_CXSIZEFRAME + SM_CXPADDEDBORDER` at the window's DPI, and top
    is 0. Custom-frame apps (Chromium, Electron) usually have none.
  - **Right after the original show returns,** before the app pumps its first
    paint: read `DWMWA_EXTENDED_FRAME_BOUNDS` and correct the position if the
    estimate was off.
- Call `SetWindowPos` with `SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER`,
  with the thread's DPI awareness temporarily set to
  `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`, because the snapshot is in physical
  pixels.
- Guard against recursion: a thread-local flag around the companion's own calls.
- v2, optional: apps that restore a saved position right after showing override the
  hint. Re-apply it for a short time, like Window Centralizer's `RecentCenter`
  ([example_mod.wh.cpp:861-876](../example_mod.wh.cpp#L861-L876)).

### B5. Working with MOA

No extra work for the splash path. MOA reads the window's rect when it starts, and
its in-process `SetWindowPos` hook retargets the panel if the window moves during
the zoom
([lines 3546-3571](../output/mobile-open-animation.wh.cpp#L3546-L3571)). Since the
companion's move runs inside the same process, hook order doesn't matter.

---

## Part C: companion, tool part (borders)

### C1. v1: DWM border color

- On each change event: diff the border list, then call
  `DwmSetWindowAttribute(DWMWA_BORDER_COLOR)` with the color for each window's
  kind and focus state.
- Restore `DWMWA_COLOR_DEFAULT` when a window leaves the list and for every window
  on unload.
- Windows 11 only, 1 px, color only. No overlay windows, no position tracking.

### C2. v2: overlay borders (PowerToys Always on Top style)

- One click-through overlay per bordered window:
  `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, not
  owned by the target.
- Read the geometry from `DWMWA_EXTENDED_FRAME_BOUNDS` and respect
  `DWMWA_WINDOW_CORNER_PREFERENCE` for rounding.
- Track movement with an out-of-context `EVENT_OBJECT_LOCATIONCHANGE` hook, filtered
  to the border set. Keep z-order with
  `SetWindowPos(overlay, GetWindow(target, GW_HWNDPREV))` on foreground and
  location changes.
- Hide the overlay while its window is minimized, maximized or cloaked.
- Expect slight lag during drags, as in PowerToys.

Animations: a window being animated is cloaked, so MultiWM hasn't admitted it and
it isn't in the border list yet. Its border appears after the reveal with no extra
logic.

---

## Part D: companion animation (optional, after B and C)

Prefer recommending MOA (splash on) first. If the companion gets its own animation:

- Follow MOA's splash path:
  - cloak the window at show time (only if it isn't already cloaked)
  - an opaque panel with the app icon zooms to the **hinted** rect
  - wait until the window has painted (with a timeout)
  - uncloak the window under the panel, then fade the panel out
- Don't implement MOA's real-window fallback (`WS_EX_LAYERED` plus resizing the real
  window every frame). MultiWM admits that window mid-animation and the conformance
  lease fights it.
- Rules for code running inside apps:
  - a dedicated animation thread with its own message loop
  - a panel not owned by the target window
  - the thread's DPI awareness set to match the window's
  - a guard against the companion's own hooks
  - wall-clock limits on every wait
  - on unload: uncloak, destroy, join, unregister
- Set `DWMWA_TRANSITIONS_FORCEDISABLED` on the window during the animation so
  Windows' own open animation doesn't play as well.

---

## Phases

| Phase | Scope | Done when |
|---|---|---|
| 1 | A1, A2, A3 in MultiWM | Prediction parity tests pass; a small reader tool dumps the snapshot correctly as layouts, focus and desktops change |
| 2 | B1-B5 | A manual test pass (below) shows no first jump in tiled and Floating layouts, with and without MOA |
| 3 | C1 | Borders follow focus and state; all colors restored on unload |
| 4 | C2 (if C1 is not enough) | Overlay tracks moves, z-order and state without flicker |
| 5 | D (optional) | Same test pass with the companion's own animation and MOA disabled |
| any | A4 | Fewer `TransitionPending` entries in diagnostics during animated opens |

## Manual test matrix

- **Apps:**
  - Notepad / classic Win32
  - Explorer
  - Chromium or Electron (custom frame, DirectComposition)
  - a WPF app
  - an elevated app (regedit)
  - a DPI-unaware app (an old installer)
  - an app that restores its saved position
- **Layouts:** Master + Stack (V and H), Dwindle (normal and reversed), Columns,
  Rows, Monocle, Floating.
- **Insertion modes:** `last_slot` and `after_focused`.
- **States:** Manual mode, overflow limit reached, Default-to-floating and
  Always-floating rules, an Open-on-monitor rule.
- **Monitors:** two monitors with different DPI and the taskbar on a non-bottom edge.
- **Other mods:** MOA with splash on and with splash off (the conflict is known),
  and Window Centralizer (expected to conflict; see Compatibility).

## Compatibility

| Mod | Status |
|---|---|
| Mobile Open Animation, splash on | Works (B5) |
| Mobile Open Animation, splash off | Conflicts: it resizes the real window while MultiWM manages it. Document: keep its splash on |
| Window Centralizer | Conflicts: both move windows before show. Document as incompatible |
| FancyZones and similar | Conflicts with MultiWM already; no change |

## Risks to verify early (spikes)

1. **Frame bounds before show:** what `DWMWA_EXTENDED_FRAME_BOUNDS` returns for a
   window that hasn't been shown yet, and whether the after-show correction in B4 is
   invisible.
2. **`DWMWA_BORDER_COLOR` from another process:** confirm it works on other
   processes' windows, including elevated ones from the UI-access host.
3. **AppContainer apps:** whether they can open a `Local\` section created by
   MultiWM given the SDDL above. If not, they simply get no hints.
4. **Placeholder admission:** that `Workspace` admission with a sentinel HWND stays
   purely in the model.
5. **Hook chaining:** with MOA and the companion both hooking `ShowWindow`, check
   both orders on a real install.

## Open questions for review

1. **Where the shared header lives:** a third folder both WHAM configs include,
   a git submodule, or a copy with a version constant checked at runtime (the
   companion compares the serialized rules' version anyway).
2. **Should Overflow → Float get a floating hint in v1,** or no hint (current plan)?
3. **Should snapshot publishing be a MultiWM setting** (default on), or always on?
   It's cheap and has no visible effect without the companion.
4. **Borders v1:** is DWM color enough, or start straight with overlays?
5. **Animation:** recommend MOA, or plan for the companion's own (Part D)?
