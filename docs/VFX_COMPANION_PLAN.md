# MultiWM companion: implementation and remaining work

Release preparation: MultiWM 1.15.0 and Companion 0.3.0, snapshot ABI 5.0.
Placement hints and diagnostics are implemented. Borders and animations remain
future work. Source versions are updated; installation and the live checks
below are separate steps.

## Current implementation

- **Separate mods.** MultiWM runs in its tool process and owns membership,
  layouts, routing and conformance. The companion runs inside applications,
  including Explorer where permitted by Windhawk's process configuration.
- **One-way snapshot.** MultiWM publishes current-desktop workspaces, admission
  frames, known window ownership/treatment, rules and per-window restore frames. Readers map the
  section read-only and never send placement requests back to MultiWM.
- **Event-driven publication.** Between messages, repository revisions, mode,
  desktop, display and effective routing changes trigger a rebuild. Identical
  contents skip the write. There is no background minimized-window seeding.
- **ABI 5.0.** The section is `Local\MultiWM.Companion.Snapshot.v5`; the
  liveness event is `Local\MultiWM.Companion.Alive.v5`. Event presence identifies
  a running MultiWM; it is not a change-notification event. A seqlock protects
  writes. Limits are 16 workspaces, 2048 tracked HWNDs, 128 restore hints and
  32 KiB of serialized rules. Layout assertions cover x86 and x64.
- **Reader cache.** The companion refreshes its body when generation or writer
  PID changes, and decodes rules only when their bytes change. Major-version
  mismatches and incomplete required data give no hint.
- **Automatic mode only.** Opening and restore hints require Automatic mode.
  Manual-mode calls pass through. MultiWM itself retains its existing Manual
  group policy; the companion does not admit or restore group membership.
- **Owning thread only.** Native preparation and size-limit queries occur on
  the window's own thread. Cross-thread calls pass through without synchronous
  foreign-window preparation. Nested shows during preparation also pass through.
- **Admission frames.** MultiWM runs its real admission and layout planner on a
  workspace copy. Frames include configured gap and insets, scaled to the
  monitor's physical DPI. Known members returning to their workspace use their
  own restore frame. Tiled arrivals elsewhere share destination insertion frames.
- **Ownership and placement intent.** Each tracked entry publishes HWND, PID,
  owner desktop/monitor, tiled/floating treatment and valid measured borders.
  The reader distinguishes admission, restore, arrival and retained floating
  treatment. Missing restore geometry never implies new admission. Duplicate
  ownership and stale PID records give no hint.
- **Matching migration insertion.** Real migration and admission use the same
  insertion methods. Hidden suspended migrants now reserve the configured
  destination position, including After focused. The publisher reuses one
  destination tile rather than planning every off-desktop HWND against it.
  Arrival hints do not rerun opening rules or override explicit floating state.
  Floating destinations and missing tiled hints (including overflow) pass through.
- **Live limits.** The companion asks `WM_GETMINMAXINFO`, then converts the reply
  from the window's DPI units to physical visible-frame sizes. Both mods centre
  capped windows within their cells. System-default maxima mean no explicit cap.
  Window destruction or geometry/state changes during the query abandon the hint.
- **Shared arithmetic.** `src/shared/geometry.h` owns conversion, centring,
  clamping, DIP scaling, fullscreen bounds and maximum-size centring. Native
  queries and callback safeguards stay in their respective mods. The layout
  solver remains in MultiWM.
- **Exact restore planning.** For each suspended member, MultiWM calls
  `Restore(hwnd)` on a workspace copy and plans it with cached size limits.
  The published frame accounts for saved position, weight, Dwindle ratio and
  master status. An unsatisfiable plan has no frame. Constraints, DPI, membership
  or overflow decisions can still change before the actual restore.
- **Hidden normal members.** Hide-to-tray does not have to minimize a window.
  Known hidden normal members use their published restore frame and measured
  borders on show, asynchronous show, saved placement and immediate/deferred
  show positioning. Deferred placement waits for the application's commit;
  unavailable hints leave the window untouched.
- **Measured borders.** Resize borders are cached while a window is tiled and
  restored. Window DPI and frame styles must still match that measurement, and
  its measurement DPI must match the destination workspace. Missing/stale
  measurements give no restore hint; minimized/maximized borders are not guessed.
  Cached restored borders survive visibility, min/max, enabled and topmost
  changes; fresh measurements reject any DPI or style/state change while sampled.
- **Hook coverage.** Native show and asynchronous-show entry points,
  immediate/deferred show positioning, saved placements, visible creation and
  default `SC_RESTORE` handling are covered. Commands, flags, results and deferred
  batch timing survive. Restore preparation preserves visibility, min/max state
  and activation until the original operation. Restore-to-maximized is skipped.
- **Taskbar evidence.** Real tests reached `DefWindowProcW(SC_RESTORE)` on the
  application's own thread while still minimized. This supplies an early hook
  for the tested apps; it does not prove coverage of custom handlers or every
  shell/dialog restore route. Explorer-specific preparation is not implemented.
- **Best-effort visibility.** DWM frame bounds are used when available. Visible
  creation may paint before the hook returns. Changed startup geometry or limits
  can still require MultiWM to correct placement afterward.
- **Diagnostics.** Reports include ABI, section and liveness-event state,
  generation, contents, maximum sizes, rebuild/write counts, burst and unchanged
  rebuilds, and average/slowest build-and-compare time.

## Current overflow coverage and remaining jumps (ABI 5.0)

The companion can hint a centred overflow float in two cases without an extra
fit-envelope calculation: the workspace's window limit is reached (or admission
and recovery establish that even a minimal newcomer will float), and the new
window's live minimum width or height exceeds the whole work area. MultiWM
publishes the default floating frame; a matching size override still applies.
Overflow ignores a preserve-app-size rule, matching MultiWM's FailureCenter
placement. Native minimum and maximum sizes still apply to the hinted frame.

These floating hints apply only when overflow centres the window on its current
workspace. With exactly one display and Create desktop disabled, Try other
monitors first has no destination: MultiWM centres directly and publishes the
floating hint. The shared routing decision uses a cheap monitor-count query,
so connecting another display restores monitor routing automatically. A failed
count query conservatively retains routing. With another monitor available or
Create desktop enabled, overflow initially floats the window in place, then
routes it. The companion does not yet predict that destination or a later
centring fallback if routing fails.

A minimum larger than the predicted tile but small enough for the work area is
still a gap. The companion expands that tile towards its neighbours, but does
not run MultiWM's complete admission and recovery solver against every member's
limits. MultiWM can subsequently choose another slot or float the arrival, so a
visible correction remains possible. No fit envelope is published. Predictions
also do not model rollback of an explicit layout command that has not settled;
such a rollback can change the destination after the hint was calculated.

ABI 5 adds ownership and treatment to tracked entries and moves measured borders
out of restore entries so both restore and arrival decisions can use them. Both
mods must be updated together; ABI 4 peers give no hints. The fixed section grows
from 48840 to 169160 bytes. Cached readers copy it only on generation/writer
changes. Off-desktop metadata publication adds no layout plans, but its metadata
reads and larger body copies require a fresh live cost sample; the earlier ABI 4
measurements below do not measure this change.

Arrival prediction assumes the published active desktop is where the app intends
to show its window. Desktop switches during showing, concurrent layout changes
and live constraints can still produce a later correction. Native tests use
test-owned windows and synthetic ownership; real shell desktop movement remains
on the live checklist.

General routed-overflow hints are deferred. Proving overflow and choosing the
winning destination require the incoming window's live limits, the destination
members' limits, candidate ordering and DPI handling. Moving a window early also
changes its observed source monitor. A destination's next-slot rectangle alone
does not establish the route MultiWM would commit. Keep the definite local-float
cases above; revisit routing only with a bounded prediction that preserves the
existing routing policy.

The common geometry arithmetic is consolidated in `src/shared/geometry.h`:
centres, work-area clamping, DIP scaling, fullscreen bounds, tracking-limit
conversion and maximum-size centring. Native metric queries and message sending
remain in their respective mods, including MultiWM's timeout and the companion's
owning-thread/reentrancy checks. The companion's approximate minimum-size growth
stays local; MultiWM continues using its constrained layout solver.

## Measuring snapshot rebuilds before changing scheduling

After installing the instrumented MultiWM, configure its Write Diagnostic Report
hotkey (blank by default). Use that shortcut to write a report, use MultiWM
normally for several minutes, then write a second report from the same running
process. Include ordinary window opening/closing, minimization
and restoration, and desktop or layout changes. Keep the two reports as the
baseline and final sample; restarting MultiWM makes their counter differences
incomparable. Reports use the configured Output Directory, or
`%USERPROFILE%\Documents\MultiWMDiagnostics` when it is blank.

In the performance counters' Companion snapshot group, use the difference
between the two reports' total columns to calculate:

- Burst share: rebuilds within 16 ms of the previous rebuild / all rebuilds.
- Unchanged share: rebuilds whose result was unchanged / all rebuilds.
- Average rebuild time: total rebuild time in microseconds / all rebuilds.
- Rebuild frequency: all rebuilds / elapsed sample seconds.

Only calculate ratios when the interval contains rebuilds. The burst counter
uses a strict interval of less than 16 ms. Rebuild time is elapsed time for
building and comparing the snapshot, including native queries; it excludes the
shared-memory write. The separate Companion snapshot section shows the latest
published contents and lifetime average/slowest rebuild, rather than an interval
maximum. Writes and unchanged rebuilds are counted separately.

Use both frequency and elapsed cost when deciding item 7b (rebuild after the
message queue drains). A high burst share alone does not establish expensive
or redundant work. Frequent unchanged rebuilds during bursts plus meaningful
rebuild time would justify trying coalescing, followed by the same measurement
and a check that new-window and restore hints remain timely. Until a live sample
is collected, leave scheduling unchanged; synthetic parity tests and benchmarks
do not substitute for that sample.

Live sample, 2026-11-17 (first report, including startup): 539.547 seconds,
191 rebuilds (21.24/min), 14 burst rebuilds (7.33%), 34 unchanged (17.80%),
and 157 writes. Build-and-compare time totalled 24.893 ms: 130.3 us average,
1.411 ms slowest, and 0.0046% of elapsed runtime (not a CPU measurement).
The sample included eight desktop-change notifications and minimize/restore
activity, but no layout commands or resize gestures. Decision: defer 7b for this
workload and proceed to shared geometry helpers (item 8); revisit scheduling if
a heavier sample shows meaningful cost. No scheduling change was made.

## Verification completed

All four maintained suites pass: layout, reconciliation, companion and native
restoration. The overflow parity run uses 8000 generated workspaces: 98 predicted
floats were actual floats, and none of 3016 predicted tiles floated. Three
same-minimum floats received no hint; missing hints retain ordinary placement.
Migration parity covers all six tiled layouts, both insertion modes, reverse
Dwindle and hidden/minimized/maximized arrivals. Native tests cover 57 restore
and arrival paths, including the first visible position and deferred commits.
Ownership tests cover retained floating state, stale/ambiguous identity, missing
geometry and truncated metadata.

Warning checks cover x64 MultiWM and x86/x64 companion. The repository's warning
profile passes with warnings treated as errors. Unsuppressed checks still report
SDK editor-stub unused parameters and the project's Windows-structure initializer
idioms. These checks compile local source and do not install mods.

The report-shape digest includes the new ABI/liveness fields. Other maintained
digests are unchanged. `tests/run.ps1` launches statically linked tests with
Windows' Microsoft-signed-DLL-only policy, verified before the child resumes.
This prevents installed Windhawk mods from altering test behavior without
changing the installed mods or Windhawk configuration. The native restore suite
also uses app-cloaked windows on a private desktop and never switches the input
desktop.

## Remaining live checks

Run these after installing the prepared builds; no installation is performed by
the test runner. Enable Windhawk's logging for the companion while investigating,
and capture a diagnostic report after the scenario.

| Scenario | Expected result |
| --- | --- |
| New tiled window, each insertion mode | It first appears near its final tile, with configured gap/insets; MultiWM makes no unnecessary correction. |
| Real app with a maximum size | Its frame is centred in its cell at the cap and remains tiled. |
| DPI-unaware app at 200% | Reported limits convert to physical pixels and the visible frame matches the intended placement. |
| Taskbar restore of a weighted/master member | Installed companion logs restore-tile use before the original restore; final saved slot and weight remain correct. |
| Hide a normal window to the tray, change the layout, then show it | Installed companion uses the window's restore tile before it appears; test both hide-only and minimize-to-tray behavior. |
| Hide a tiled UniGetUI window on A, show it on B | Logs destination arrival tile use; first position and final insertion follow B's configured insertion mode. |
| Explicitly float UniGetUI, hide on A, show on B | Floating treatment survives and the companion gives no tiled hint. |
| Collect a report after normal use with ABI 5 | Measure the larger snapshot and ownership metadata cost; compare with the historical ABI 4 sample. |
| Minimize then restore in Manual mode | Companion passes calls through; ordinary MultiWM Manual membership policy applies. |
| Restore Down in Manual mode | Companion gives no hint; MultiWM handles its retained group normally. |
| One-monitor local overflow, Create desktop off | `overflowCenters=1`; definite overflow floats use the centred hint even with Try other monitors first on. |
| Connect/disconnect another monitor | Effective routing and snapshot flags update without a settings change. |
| Mixed DPI and non-bottom taskbar | Window/frame/workspace coordinates remain correct. |
| App handles restore itself | Observe whether a supported hook is reached; no universal coverage claim. |
| Compatible animation mod enabled | Check hook order and that its visual target follows the prepared geometry. |

Taskbar dispatch and the nine-minute publisher-cost sample are verified, but
they do not replace these installed-companion visual checks. The temporary
restore probe can be disabled/removed after its investigation is complete.

## Deferred work

- **General routed-overflow hints:** require a bounded approach that preserves
  destination selection; no copying the full solver into application processes.
- **7b/7c scheduling changes:** the measured publisher cost does not justify them
  for the sampled workload. Revisit with a heavier live sample if needed.
- **Geometry-based float prediction:** no fit envelope is implemented.
- **Other compatibility work:** proportional growth between neighbours, Mobile
  Open Animation with DPI-unaware apps, AppContainer readers, and restore
  preparation bypassing other mods' placement hooks.

## Future visuals (not implemented)

Custom borders should remain independent of admission hints. Start by evaluating
DWM border colour for managed windows, restoring original attributes on unload;
consider a separate UI-access overlay process only if DWM colour is insufficient.
Focus/state changes and windows on other virtual desktops need explicit handling.

Open animations should be evaluated against Mobile Open Animation before adding
a second animation engine. Preserve MultiWM's cloak/visibility handling and
avoid competing placement writers. Current compatibility needs live verification:
Window Centralizer and Center New Windows conflict by moving the same windows;
Mobile Open Animation's resize-real-window mode conflicts with tiling, and
DPI-unaware targets remain a separate compatibility item.

Any border/event transport or animation settings need their own design. They
are not part of snapshot ABI 5.0 or this release's placement implementation.
