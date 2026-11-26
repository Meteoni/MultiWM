# Maintaining MultiWM

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the invariants at the
top of `src/multiwm/core/shared_types.h`. The headers are chapters of one source file;
compile `src/multiwm/multiwm.cpp` (or run the tests) rather than a header on its own.

## File and namespace ownership

The source roots are `src/multiwm/`, `src/companion/`, and `src/shared/`. Each
chapter folder under `src/multiwm/` has one owner namespace (see ARCHITECTURE.md).
Put new code in the folder whose owner it belongs to. A new file does not need a
new namespace or manager class; split a file when its responsibilities differ,
and keep an algorithm together with its local helpers.

Define model members inside `namespace Model`. Qualify calls across owners,
especially `Platform::Windows`, `Platform::Monitors`, and
`Platform::VirtualDesktop`, so native boundaries are visible. Keep same-owner
calls short. The core model types (`Workspace`, `WindowRecord`, …) are
deliberately usable unqualified; do not add global aliases for functions,
because they hide which owner has side effects.

Shared state that belongs to one owner lives in that owner's namespace in
`src/multiwm/core/runtime_state.h` (for example `Placement::g_pendingProofs`). If a
function from a later chapter is needed earlier, declare it in
`src/multiwm/core/forward_declarations.h`.

Make shared state a class only when several containers must change together,
for example a reset that must not miss one. Declare it in `runtime_state.h` and
define its members in the owner's chapter, as `model/workspace.h` and
`model/workspace_impl.h` do. Keep policy that observes or changes Windows in free
functions. Hold function-local COM pointers in `Platform::VirtualDesktop::ComRef`;
release the long-lived `g_vd` proxies explicitly, because after an Explorer
restart they must be dropped without `Release`.

## Readability rules

- Give each function one explainable operation. Extract substantial decisions
  such as planning a destination or rolling back a move. Keep simple, single-use
  calculations beside their callers; local lambdas are useful for repeated work
  confined to that operation.
- Give interfaces descriptive names and meaningful types. Use enums for choices
  such as `Layout::Axis::X` / `Y`, and named result structs for related outputs.
  Prefer returned values, including `std::optional` when a result can be absent.
  Use a boolean when its two meanings are clear at the call site.
- Declare temporary state near its use and prefer `const` observations. Group
  related state when it has a shared lifetime or invariant.
- Make effects easy to find: observe Windows, calculate a candidate, revalidate
  any native assumptions, then commit model state and placement. Preserve native
  query order when refactoring existing code.
- Explain intent, invariants, and non-obvious failures in comments. Add a short
  contract to a non-obvious interface covering its inputs, result, mutations,
  failure effects, and ownership/lifetime requirements. State whether a failed
  trial leaves partial output or leaves its arguments unchanged.
- Use ordinary control flow, guard clauses, and descriptive intermediate values.
  Review deep nesting and long routines for separate logical operations. Judge
  an extraction by how much easier the caller is to understand.

These practices draw on the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
(F.2, F.8, I.4, F.20, ES.6, NL.1–2) and
[Google's function-comment guidance](https://google.github.io/styleguide/cppguide.html#Function_Comments).

## Units, ownership, and native calls

- Settings with a `Dip` suffix are logical pixels at 96 DPI. Layout work areas,
  frames, gesture coordinates, placement requests, and native minimum sizes are
  physical screen pixels. Convert at the monitor/window boundary; record the DPI
  alongside cached native sizes.
- The serialized WM thread owns workspace mutation, reconciliation, placement,
  and private virtual-desktop COM state. Callbacks enqueue work. Repository reads
  return copies; mutate a copy and explicitly save it to commit the change.
- Pointers returned by `Workspace::Find` refer into that particular workspace
  copy. Keep them local and reacquire them after operations that replace or remove
  records. Avoid retaining them across workspace assignment.
- A native observation can become stale during a desktop switch or window
  lifetime change. In particular, `WM_GETMINMAXINFO` executes application code.
  Preserve identity/physical-state checks after the probe and before mutation.
  When a repeated check protects a boundary, explain that reason beside it.
- Failure contracts must describe effects. For example, discovery may repair
  other owners while its requested workspace remains unchanged; a failed overflow
  trial may contain partial state but never saves it or places a window.

## Vocabulary

| Term | Meaning | Example |
| --- | --- | --- |
| Workspace | One virtual desktop and physical monitor's logical state. | Desktop 2 on the left monitor has its own Dwindle order. |
| Record | A known window's membership, preferences, and cached observations. | Floating a window keeps its record and remembered floating frame. |
| Tiled participant | A record currently consuming a slot in the active layout. | Three participants produce three rectangles in Columns. |
| Suspended window | A retained member temporarily absent from the active layout, with its slot saved. | A maximized tiled editor returns to its saved slot when restored. |
| Preferred geometry | Durable user intent, expressed by ratios or weights. | A divider prefers 50% even while a large minimum temporarily requires 65%. |
| Observed geometry | The native window frame sampled from Windows. | An application reports a 700-pixel frame after a 650-pixel placement request. |
| Conformance lease | A bounded period for a window to settle on its assigned tile, or, in the Floating layout, on the frame MultiWM just gave it. | A snap-back can trigger a repair; expiry checks the current tile before floating a persistently displaced window. A floating lease's expiry instead keeps the app's frame. |
| User layout mutation | A change the user asks for to a workspace's order, weights or layout. It applies at once, but is rolled back if the windows refuse their new tiles. | Promoting a window whose app then refuses its new tile puts the previous order back and names the app in a toast. |

## Verification and review tools

Run the maintained regression suites with the Windhawk compiler:

```powershell
./tests/run.ps1 -CompilerRoot 'C:/path/to/Windhawk/Compiler'
```

Use `-Suite layout`, `-Suite reconciliation`, `-Suite companion`, or `-Suite restore` to run one suite.
The layout suite uses the real model/planner without native placement. The
reconciliation suite uses fake desktop COM responses and test-owned hidden
windows. The companion suite checks show-call preservation and size limits with
test-owned hidden windows and recorded native calls. The restore suite exercises
native min/max restores using app-cloaked windows on a private Win32 desktop.
None of the suites switches the input desktop or real virtual desktops, or moves
other apps. Executables go under `output/tests/`.

`run.ps1` builds a small isolated launcher and starts each test with
`PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON`.
The launcher verifies the child's Microsoft-signed-only policy while it is
suspended and stops if isolation cannot be established. Windhawk's injector
skips processes with this policy, and Windows prevents its DLLs from loading.
No global exclusions, mod disabling or Windhawk restart is needed. Keep tests
statically linked and run them through this script; direct `.exe` launches do
not receive the policy. See the [Windows policy documentation](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process_mitigation_binary_signature_policy)
and [Windhawk's injector](https://github.com/ramensoftware/windhawk/blob/main/src/windhawk/engine/new_process_injector.cpp).

For manual hung-window testing, build and launch the standalone fixture from
the repository root.

```powershell
$CompilerRoot = 'C:/path/to/Windhawk/Compiler'
New-Item -ItemType Directory -Force output/tests | Out-Null
& "$CompilerRoot/bin/clang++.exe" -target x86_64-w64-mingw32 -municode -mwindows -static `
  tests/hung_window.cpp -o output/tests/hung_window.exe
./output/tests/hung_window.exe
```

Let MultiWM tile the resizable window, then click **Hang for 60 seconds**.
Interact with the window during the pause to exercise Windows' ghost window.
The app resumes automatically and can be hung again.

Build with the Windhawk/WHAM compiler configuration. For changes to layout,
ownership, or native sequencing, exercise the affected behavior and compare the
result with the prior implementation. Include failures, stale observations, and
desktop changes; verify that saved preferences and unrelated windows survive.
Keep lasting checks in `tests/`, asserting user-visible behavior and invariants.
Use comparison baselines temporarily during a refactor, then remove copied
implementations, generators, traces, and obsolete reproducers from `output/`.
Retain distinct orientation, policy, and edge cases when they cover different
branches; avoid adding checks just to increase a test count.

The repository's `.clang-tidy` enables advisory function-size and cognitive-
complexity checks. A complexity score above 25, more than 120 lines, or nesting
past 4 is a review prompt. Existing routines can exceed these values while a
focused refactor is in progress. The checks introduce no naming or ABI rewrites.

Enable clang-tidy diagnostics in clangd (WHAM's generated `.clangd` supplies the
editor's compiler configuration), or list every function over the thresholds with:

```powershell
./tests/tidy.ps1 -CompilerRoot 'C:/path/to/Windhawk/Compiler'
```

The Windhawk compiler ships no clang-tidy; the script uses one from `PATH` or the
one bundled with the VS Code C/C++ extension (or pass `-ClangTidy`), and parses
both mods with the compiler's own headers. Use `-Mod multiwm` or `-Mod companion`
for one mod.
See the [clang-tidy check documentation](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html)
for how branching and nesting contribute to the score.

Review acceptance: can a new contributor explain what an operation decides,
what it changes, and what happens on failure after reading it and its immediate
types?
