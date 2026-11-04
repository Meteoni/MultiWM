# Maintaining MultiWM

Start with the [source map](README.md#source-layout) and the invariants in
`multiwm_core.h`. The implementation headers share one translation unit; compile
`src/multiwm.cpp` or its WHAM output rather than a header in isolation.

## File and namespace ownership

Use the existing responsibilities in the source map. Related files reopen a
shared namespace: `Layout` owns planning, `Placement` owns floating/tiled placement
and conformance, and `Reconcile` owns discovery and routing. A new file does not
require a new subsystem, manager class, or namespace. Split when responsibilities
differ; keep cohesive algorithms and their local helpers together.

Define model members inside `namespace Model`. Qualify calls across owners,
especially `Platform::Windows`, `Platform::Monitors`, and
`Platform::VirtualDesktop`, so native boundaries are visible. Keep same-owner
calls concise. Common model value-type aliases are allowed; global aliases for
platform operations hide their effects.

The `.h` files here are deliberately dependency-ordered fragments for the
Windhawk translation unit. `multiwm_workspace.h` holds model declarations;
`multiwm_runtime_state.h` holds actor instances and diagnostic state;
`multiwm_interfaces.h` holds the few shared placement types and forward calls
needed by the include order. Keep implementation helpers with their owner.
Namespaces organize this code; they do not create separate compilation units.

The [file-boundary review](docs/file-boundaries-review.md) records the concrete
changes and references used to keep this organization proportionate.

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
| Conformance lease | A bounded period for a window to settle on its assigned tile. | A snap-back can trigger a repair; expiry checks the current tile before floating a persistently displaced window. |

## Verification and review tools

Run the two maintained regression suites with the Windhawk compiler:

```powershell
./tests/run.ps1 -CompilerRoot 'C:/path/to/Windhawk/Compiler'
```

Use `-Suite layout` or `-Suite reconciliation` to run the affected suite alone.
The layout suite uses the real model/planner without native placement. The
reconciliation suite uses fake desktop COM responses and test-owned hidden
windows. Neither suite switches real virtual desktops or moves other apps.
The runner writes only two reusable executables under `output/tests/`.

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

Enable clang-tidy diagnostics in clangd, or run `clang-tidy -p <build-directory>
src/multiwm.cpp` with a compilation database containing the Windhawk flags.
WHAM's generated `.clangd` supplies the editor's compiler configuration.
See the [clang-tidy check documentation](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html)
for how branching and nesting contribute to the score.

Review acceptance: can a new contributor explain what an operation decides,
what it changes, and what happens on failure after reading it and its immediate
types? The [saved cleanup review](docs/readability-review.md) records the scope
and verification of the current refactoring pass.
