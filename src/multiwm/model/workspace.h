// Workspace model interface and invariants.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

// Defined in layout/algorithms.h; named here by the window-size step helpers.
namespace Layout {
struct DwindleSplit;
}  // namespace Layout

namespace Model {

// Canonicalizes stored identity text without querying native displays.
static void NormalizeMonitorIdentityToken(std::wstring* value);

// Per-desktop + monitor state
struct GuidHash {
  size_t operator()(const GUID& guid) const {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(&guid);
    size_t hash = 0;
    for (int i = 0; i < 4; ++i) {
      hash ^= std::hash<uint32_t>{}(data[i]) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

struct GuidEqual {
  bool operator()(const GUID& a, const GUID& b) const { return IsEqualGUID(a, b); }
};

// Stable monitor identity used by workspace keys. HMONITOR remains a live
// routing token only and is resolved on demand at Win32 boundaries. Physical
// monitor device-interface IDs are preferred; the GDI device name is retained
// only as a compatibility fallback on systems where the interface ID query fails.
struct MonitorId {
  std::wstring deviceId;

  bool Empty() const { return deviceId.empty(); }
  explicit operator bool() const { return !Empty(); }

  bool operator==(const MonitorId& other) const {
    return deviceId == other.deviceId;
  }
  bool operator!=(const MonitorId& other) const {
    return !(*this == other);
  }

  static bool FromHMonitor(HMONITOR monitor, MonitorId* out);
  HMONITOR Resolve() const;
};

struct MonitorIdHash {
  size_t operator()(const MonitorId& monitor) const {
    return std::hash<std::wstring>{}(monitor.deviceId);
  }
};

struct DesktopMonitorKey {
  GUID desktopId{};
  MonitorId monitor;

  static bool FromHMonitor(
      const GUID& desktopId, HMONITOR hmonitor, DesktopMonitorKey* out) {
    if (!out) return false;
    MonitorId monitorId;
    if (!MonitorId::FromHMonitor(hmonitor, &monitorId)) return false;
    out->desktopId = desktopId;
    out->monitor = std::move(monitorId);
    return true;
  }

  HMONITOR ResolveMonitor() const { return monitor.Resolve(); }

  bool operator==(const DesktopMonitorKey& other) const {
    return IsEqualGUID(desktopId, other.desktopId) && monitor == other.monitor;
  }
};

struct DesktopMonitorKeyHash {
  size_t operator()(const DesktopMonitorKey& key) const {
    size_t hash = GuidHash{}(key.desktopId);
    hash ^= MonitorIdHash{}(key.monitor) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct DesktopMonitorKeyEqual {
  bool operator()(const DesktopMonitorKey& a, const DesktopMonitorKey& b) const { return a == b; }
};

// Where a suspended (minimized/maximized/hidden) window returns to when restored.
struct SuspendedSlot {
  size_t index = 0;        // Logical position, counting other suspended windows too.
  double weight = 1.0;     // Saved weight or Dwindle ratio for that slot.
  bool hasWeight = false;  // False: use the layout's default weight on restore.
  bool wasMaster = false;  // Master + Stack: it was the master window.
};

// How a known window participates in its workspace. See the glossary in CONTRIBUTING.md.
enum class ManageState {
  Tiled,      // Occupies a slot in the active layout (listed in TiledWindows()).
  Floating,   // Known, but placed freely; never occupies a slot.
  Suspended,  // Tiled member temporarily out of the layout; keeps a SuspendedSlot.
};

// Why a logically managed tiled window is temporarily absent from the active
// layout. Hidden is retained for close-to-tray behavior; maximize and fullscreen
// share the same save-slot/restore semantics.
enum class SuspensionReason {
  None,
  Hidden,
  Minimized,
  Maximized,
};

enum class PlacementResult {
  Success,
  SuppressedByPhysicalState,
  AdjustedByWindow,
  AccessDenied,
  Refused,
  Dead,
  Unresponsive,
};

// Native minimum tracking constraints converted to the DWM extended-frame
// geometry used by the layout engine. A successful WM_GETMINMAXINFO report is
// authoritative solver policy. If a later probe fails, callers retain the last
// valid report rather than inventing a weaker constraint.
// Width/height are physical frame pixels at the recorded DPI, not settings DIP.
struct WindowConstraints {
  LONG minWidth = 1;
  LONG minHeight = 1;
  UINT dpi = 96;
  bool valid = false;
  DWORD lastProbeError = ERROR_SUCCESS;
  // The app's maximum tracking size, in the same frame geometry as the
  // minimums. 0 means the app left the system default (no practical maximum).
  // A window can't grow past it, so the planner centres it inside a larger cell
  // instead of asking for a size Windows will refuse (see CenterWithinMaximumSizes).
  // Last, so positional initializers of the fields above keep their meaning.
  LONG maxWidth = 0;
  LONG maxHeight = 0;
};

static uint64_t NextTilingGeneration() {
  static uint64_t nextGeneration = 1;
  uint64_t generation = nextGeneration++;
  if (generation == 0) generation = nextGeneration++;
  return generation;
}

struct WindowRecord {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  ManageState state = ManageState::Tiled;

  // Capabilities are observations, not hard policy yet. They give later rule/
  // constraint work somewhere stable to live without re-querying every branch.
  bool canMove = true;
  bool canResize = true;
  bool topmost = false;

  // Order of focus observations in this workspace. Zero means never focused.
  uint64_t lastFocusSequence = 0;
  // Monotonic admission generation provides stable ordering for constraint
  // recovery candidates. Reordering never changes this value.
  uint64_t tilingGeneration = 0;
  // Generation (same counter as tilingGeneration) of the last time this window
  // raised its native minimum at the same DPI. Constraint recovery treats the
  // newest demand, admission or growth, as the likeliest cause of a conflict.
  uint64_t minimumGrowthGeneration = 0;
  // True for a settling window (see the Vocabulary in CONTRIBUTING.md): newly
  // admitted or restored, and not yet settled in its slot. While true,
  // arrangement may move it to a nearby slot that fits its minimum size instead
  // of floating it. Cleared once its placement holds.
  bool settling = false;
  // True when an "Open on monitor" rule chose this window's monitor, so overflow
  // must not move it to another monitor.
  bool automaticOverflowMonitorFixed = false;
  // Default-floating admission can precede a usable restored rectangle. Retain
  // the centering request until discovery can place the restored HWND.
  bool pendingDefaultFloatingCenter = false;
  // Set by an Always-floating rule. The policy is retained on the record so a
  // title change cannot make an explicitly protected window tileable by accident.
  bool alwaysFloating = false;
  WindowConstraints constraints{};
  // The visible ghost's fixed frame size in physical pixels while it stands in
  // for this window. Zero when there is no restored ghost.
  SIZE ghostSize{};

  PlacementResult lastPlacementResult = PlacementResult::Success;
  RECT lastRequestedRect{};
  RECT lastObservedRect{};

  // Last meaningful floating frame in physical screen pixels. Its monitor is
  // the one holding the rectangle; the capture DPI lets migration restore its
  // size sensibly across per-monitor scaling.
  RECT floatingRect{};
  UINT floatingDpi = 96;
  bool hasFloatingRect = false;

  // Only meaningful while state == Suspended. It preserves the tiled logical
  // position/weight across non-participating physical states.
  SuspensionReason suspensionReason = SuspensionReason::None;
  SuspendedSlot savedSlot{};
  bool hasSavedSlot = false;
};

// Most recent event that added this window's demands to the layout: admission
// or a raised minimum. Larger is newer.
static uint64_t LayoutDemandRecency(const WindowRecord& record) {
  return std::max(record.tilingGeneration, record.minimumGrowthGeneration);
}

// True when the window's newest demand is a raised minimum, not its admission.
static bool MinimumGrewSinceAdmission(const WindowRecord& record) {
  return record.minimumGrowthGeneration > record.tilingGeneration;
}

// A valid same-DPI report that raises either minimum. DPI rescaling and a first
// valid report are not growth: they describe the same application contract.
static bool IsSameDpiMinimumGrowth(
    const WindowConstraints& before, const WindowConstraints& after) {
  return before.valid && after.valid && before.dpi == after.dpi &&
         (after.minWidth > before.minWidth || after.minHeight > before.minHeight);
}

struct PlacementObservation {
  PlacementResult result = PlacementResult::Success;
  RECT requested{}; // Both frames use physical screen pixels.
  RECT observed{};
  // True only when PlaceWindowChecked actually issued SetWindowPos. A successful
  // no-op does not create/extend a conformance lease.
  bool placementIssued = false;
};

struct WorkspaceGeometryItem {
  WindowRecord record{};
  RECT frame{};
};

// The logical state of one workspace: one virtual desktop on one monitor.
//
// A Workspace is a plain value. Copying it is cheap enough, and it never talks
// to Windows. Typical use:
//
//   Workspace ws;
//   if (!g_workspaces.Load(key, &ws)) return;   // a copy
//   if (ws.Float(hwnd)) {                       // change the copy
//     g_workspaces.Save(key, ws);               // commit
//     Placement::ArrangeWorkspace(key);         // move real windows
//   }
//
// Conventions for every mutating method below, unless its comment says otherwise:
//  - Returns true when this workspace changed, false for a no-op (unknown HWND,
//    already in the requested state, request not applicable).
//  - Returns false without changing anything.
//  - WM thread only (asserted in debug builds).
//  - Changes only this object; callers must Save() it to the repository.
// Pointers from Find() point into this object. Do not keep them across another
// mutating call.
class Workspace {
 public:
  // What the caller should do after a placement attempt (ApplyPlacementObservation).
  enum class PlacementAction { None, Float, Forget };

  // Returned by TiledIndex() and LogicalIndex() when the window has no position.
  static constexpr size_t kNotFound = static_cast<size_t>(-1);

  Workspace() = default;

  // --- Reading state ---------------------------------------------------------

  TileLayout Layout() const { return layout_; }
  double MasterRatio() const { return masterRatio_; }
  // Tiled windows in layout order; index 0 is the master in Master + Stack.
  const std::vector<HWND>& TiledWindows() const { return windows_; }
  // Every known window (tiled, floating and suspended), keyed by HWND.
  const std::unordered_map<HWND, WindowRecord>& Records() const { return records_; }
  const std::vector<double>& StackWeights() const { return stackWeights_; }
  const std::vector<double>& GridWeights() const { return gridWeights_; }
  // One saved ratio per active Dwindle split. (A dormant ratio for the last slot
  // is kept internally so it survives that window being suspended.)
  std::vector<double> DwindleRatios() const {
    if (dwindleRatios_.empty()) return {};
    return {dwindleRatios_.begin(), dwindleRatios_.end() - 1};
  }
  // The BSP layout's split tree: one leaf per tiled or maximized window. Empty
  // in other layouts.
  const BspTree& Bsp() const { return bspTree_; }
  void SetBspSplitDirection(size_t node, std::optional<Layout::Axis> direction);
  size_t ActiveCount() const { return windows_.size(); }
  // Windows counted against the auto-tiling limit: tiled plus maximized-suspended.
  // Always 0 in the Floating layout.
  size_t VisibleWindowCount() const;
  // A workspace with no windows that keeps this one's layout, master ratio and
  // per-slot weights. Used for a desktop created by overflow.
  Workspace EmptyWithInheritedLayoutSettings() const;
  size_t RecordCount() const { return records_.size(); }
  // True when no window is tiled (floating/suspended records may still exist).
  bool NoTiledWindows() const { return windows_.empty(); }

  bool HasRecord(HWND hwnd) const;
  // The record for `hwnd`, or nullptr. See the pointer-lifetime note above.
  const WindowRecord* Find(HWND hwnd) const;
  bool IsTiled(HWND hwnd) const;
  // Position in TiledWindows(), or kNotFound.
  size_t TiledIndex(HWND hwnd) const;
  // Position counting suspended windows' saved slots too, or kNotFound
  // (floating windows have no logical position).
  size_t LogicalIndex(HWND hwnd) const;
  HWND LastFocusedWindow() const { return lastFocusedWindow_; }
  // Most recently focused window still tiled. An arriving BSP window is
  // excluded when choosing the leaf it will split.
  HWND LastFocusedTiledWindow(HWND excluded = nullptr) const;
  bool HasSuspended() const;
  // True when every tiled window appears in `snapshot`.
  bool AllTiledVisible(const std::vector<HWND>& snapshot) const;

  // --- Recording observations (never change membership or order) -------------

  // Remembers `hwnd` as the focused window. Requires a record for it.
  bool RememberFocusedWindow(HWND hwnd);
  // Stores a fresh minimum-size report. Returns true when it differs from the
  // previous one. Also assigns a tiling generation to a non-floating record that
  // had none, even when it returns false.
  bool UpdateConstraints(HWND hwnd, const WindowConstraints& constraints);
  bool UpdateGhostSize(HWND hwnd, SIZE size);
  // Stores the floating frame (physical pixels, captured at `dpi`) to restore
  // when floated again. Returns false for an empty frame or an unchanged value.
  bool RememberFloatingGeometry(HWND hwnd, const RECT& frame, UINT dpi);
  // Records the outcome of a placement attempt and says what to do next:
  // Float on access denied, Forget for a dead or unknown window, else None.
  PlacementAction ApplyPlacementObservation(HWND hwnd, const PlacementObservation& observation);

  // --- Layout and order ------------------------------------------------------

  // Switches layout. Remembered geometry for the new layout family is reused if
  // the tiled windows are unchanged since it was saved. No-op if already active.
  void SetLayout(TileLayout layout);
  // Moves to the layout after the current one in `cycle` (wrapping; the first
  // entry if the current layout is not listed). False if `cycle` is empty or
  // would not change the layout.
  bool CycleLayout(const std::vector<TileLayout>& cycle);
  // Clamped to 10%..90%.
  void SetMasterRatio(double ratio);
  // Swaps `hwnd` with the current master. False if it is already master, not
  // tiled, the layout is Floating, or fewer than two windows are tiled.
  // `oldMaster` (optional) receives the previous master on success.
  bool MakeMaster(HWND hwnd, HWND* oldMaster = nullptr);
  // Swaps two tiled windows' positions. False unless both are tiled and distinct.
  bool SwapTiled(HWND first, HWND second);
  // Exchanges two BSP sibling tiles, including their saved shares of the split.
  size_t BspSiblingDivider(HWND first, HWND second) const;
  bool SwapBspSiblingTiles(HWND first, HWND second);
  // Gives the source's space to its sibling and splits another BSP tile with it.
  bool MoveBspLeaf(HWND source, HWND target, bool allowAlternateDirection = false);
  // Lays the BSP tree out afresh, as a switch to BSP does: every saved split is
  // gone and the windows fill the largest leaves in their current order. False
  // in other layouts.
  bool RebuildBspTree();
  // Moves a settling window to another tiled index, shifting suspended slots so
  // every window keeps a unique position.
  bool MoveSettlingWindowToIndex(HWND hwnd, size_t targetIndex);
  // Marks a settling window as settled in its slot.
  bool MarkSettled(HWND hwnd);
  // Clears the request to center a default-floating window once it is placed.
  bool ClearDefaultFloatingCenter(HWND hwnd);
  // Applies an Always-floating rule: marks the record and floats it if needed.
  bool EnforceAlwaysFloating(HWND hwnd);

  // --- Comparing and rolling back --------------------------------------------

  // Same windows in the same participation states. Compares identity (PID and
  // tiling generation), not just the HWND values, which Windows can reuse.
  bool HasSameWindowMembershipAs(const Workspace& other) const;
  // Same membership plus identical order, weights, ratios and saved slots.
  bool HasSameLayoutStateAs(const Workspace& other) const;
  // Copies order, weights and ratios back from `source`, keeping this object's
  // live observations (constraints, floating frames, placement results).
  // Returns false, changing nothing, unless membership matches exactly.
  // True means "restored", not necessarily "something changed".
  bool RestoreLayoutStateFrom(const Workspace& source);
  // Like RestoreLayoutStateFrom, but only the tiled windows must match; extra or
  // missing floating/suspended records are allowed. Used to undo a pending
  // admission after that window has already left the layout.
  bool RestoreCompatibleActiveLayoutStateFrom(const Workspace& source);

  // --- Membership ------------------------------------------------------------
  //
  // Admit*  add a window that this workspace does not know yet.
  // Suspend / Restore  take a tiled window out of the layout and back, keeping
  //         its slot. Float  leaves the layout but keeps the record.
  // Forget  removes the record completely.

  // Puts a known window (floating or suspended) back into the layout, at the end.
  // False for unknown or Always-floating windows.
  bool ActivateTiled(HWND hwnd);
  // Adds an unknown window as Floating. False if `record.hwnd` is null or known.
  bool AdmitFloating(WindowRecord record);
  // Adds `record` as Tiled at the end; in BSP, by splitting the shallowest
  // tiled leaf. If the window is already known, refreshes its observations and
  // re-tiles it (a suspended one returns to its BSP leaf). True when added or re-tiled.
  // Exception to the convention: for a known window that is already tiled this
  // returns false but still refreshes its observed fields from `record`.
  bool AdmitTiled(WindowRecord record);
  // Like AdmitTiled, but inserts right after `anchor` when `anchor` is tiled and
  // the window is new (in BSP, by splitting `anchor`'s leaf). Otherwise behaves
  // exactly like AdmitTiled.
  bool AdmitTiledAfter(WindowRecord record, HWND anchor);
  // Adds a window found while the workspace is first created. With
  // reason == None it is tiled at the end; Minimized/Maximized admits it as
  // Suspended with a slot at the end. False for Hidden or a known window.
  bool AdmitInitial(WindowRecord record, SuspensionReason reason);
  // AdmitInitial that places the window (or its saved slot) right after `anchor`
  // when `anchor` is tiled; otherwise identical to AdmitInitial.
  bool AdmitInitialAfter(
      WindowRecord record, SuspensionReason reason, HWND anchor);
  // Takes a tiled window out of the layout and saves its slot. On an already
  // suspended window, updates the reason (true if it differed).
  bool Suspend(HWND hwnd, SuspensionReason reason);
  // Returns a suspended window to its saved slot with its saved weight, and marks
  // it as settling again (except in the Floating layout) so arrangement may move
  // it if that slot no longer fits.
  bool Restore(HWND hwnd);
  // Makes a window Floating and closes the gap it leaves. False if already
  // floating or unknown.
  bool Float(HWND hwnd);
  // Removes every trace of `hwnd`. False if it was unknown.
  bool Forget(HWND hwnd);

  // --- Moving windows between workspaces -------------------------------------
  //
  // Migration carries the record, not its old weights: the destination decides
  // order and weights according to its own layout.

  // Copies the record into `outRecord` (optional), then Forget()s it here.
  bool ExtractForMigration(HWND hwnd, WindowRecord* outRecord);
  // Tiled arrivals use the same insertion as admission: after a tiled `anchor`
  // or at the end. Suspended arrivals reserve that slot; floating stays floating.
  void AdmitMigrated(WindowRecord record, HWND anchor = nullptr);
  // Copies remembered floating frames from `source` for windows known here.
  void MergeRememberedFloatingGeometryFrom(const Workspace& source);
  // Copies what `source` remembers for switching back to each layout (weights,
  // Dwindle ratios, the BSP tree) and, outside Master + Stack, its master ratio:
  // what adopting the current layout's geometry doesn't produce.
  void MergeRememberedLayoutGeometryFrom(const Workspace& source);
  // Copies every non-tiled record from `source`, overwriting same-HWND records.
  void MergeNonTiledRecordsFrom(const Workspace& source);
  // Forgets `invalidWindows` and recreates records for tiled windows that lost
  // theirs. Used by arrangement to repair a workspace before placing it.
  void RepairForArrangement(
      const std::vector<HWND>& invalidWindows,
      const std::vector<WindowRecord>& missingActiveRecords);

  // --- Learning from user resizes (defined in layout/resize_learning.h) ------
  //
  // Each compares the gesture's before/after frame of the window at
  // `resizedIndex` with the planned layout and updates the matching weights or
  // ratios. Returns true when a preference changed. `workArea` and `gap` are
  // physical pixels. Call the variant that matches Layout(). A Dwindle drag
  // that changes a divider also accepts the shown layout, as the size commands
  // below do: every other divider is saved where adaptive fitting shows it,
  // and the dragged one stops where that layout runs out of room.

  bool LearnMasterStackResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnGridResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnDwindleResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  // Each dragged edge moves the nearest split whose divider it is.
  bool LearnBspResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);

  // --- Window-size commands (defined in layout/resize_steps.h) ---------------
  //
  // Both change only the preference that sizes the window at `index`: its grid
  // or stack weight, the master ratio, its own Dwindle split, or in BSP the
  // nearest split that divides it from a neighbour. Other windows'
  // preferences stay as they are, so their temporary minimum-size fitting never
  // becomes a preference. Monocle and Floating have none; both return false.

  // Grows (`grow`) or shrinks the window by `stepPercent` of the work area's span
  // along the axis it shares with its neighbours, starting from its visible
  // size and stopping at cached native minimums. False when nothing changes.
  //
  // Dwindle: acting on the layout accepts it as shown. Every divider is saved
  // where adaptive fitting currently shows it, and the step is checked against
  // that layout, so no other divider moves, except that a growing window may
  // push later dividers on its axis as far as their windows' minimums require.
  // Each pushed divider remembers its earlier ratio and springs back toward it
  // when a shrink or reset frees room before it. Dragging or stepping that
  // divider itself keeps its current position instead.
  bool StepTiledSize(
      const RECT& workArea, LONG gap, size_t index, bool grow, LONG stepPercent);
  // Returns the window's preference to the layout default: the mean of its
  // peers' weights, `defaultMasterRatio`, or an even Dwindle split (which also
  // lets dividers it pushed spring back). A Dwindle reset accepts the shown
  // layout first and stops short of even where that layout has no room.
  // `workArea` and `gap` are physical.
  bool ResetTiledSize(const RECT& workArea, LONG gap, size_t index, double defaultMasterRatio);

  // --- Construction and checks -----------------------------------------------

  // Checks the model invariants (core/shared_types.h). Returns true when all
  // hold. Appends messages to `errors` and logs them when requested. Read-only.
  bool Validate(
      const DesktopMonitorKey& key, const wchar_t* context = L"workspace",
      std::vector<std::wstring>* errors = nullptr, bool logFailures = true) const;

  // Builds a workspace whose order and weights reproduce the given window
  // frames (used by Tile Workspace to keep a layout the user arranged by hand).
  // Pure: uses only the captured frames, never live windows.
  static Workspace AdoptGeometry(
      TileLayout layout, const RECT& workArea, LONG gap,
      double fallbackMasterRatio,
      const std::vector<WorkspaceGeometryItem>& items,
      HWND preferredFirstWin = nullptr);
 private:
  // Merge observations without replacing participation/admission bookkeeping.
  WindowRecord& UpsertRecord(WindowRecord record);
  WindowRecord* FindMutable(HWND hwnd);
  // Brings the current layout's saved state in step with membership: one weight
  // or ratio per slot, and the other layouts' state cleared. In BSP the split
  // tree owns the order: leaves of departed windows go, arrivals get leaves
  // (SyncBspTree), and the tiled order becomes the leaves' order. On a workspace
  // that already meets the invariants (Validate) it changes nothing, so tiled
  // indices callers hold across it stay valid.
  void SyncLayoutState();
  void RemoveActiveAt(size_t index, SuspendedSlot* saved = nullptr);
  size_t ActiveIndexToLogicalIndex(size_t activeIndex) const;
  size_t LogicalIndexToActiveIndex(size_t logicalIndex, HWND restoringHwnd) const;
  void CloseLogicalGapAfterRemoval(size_t removedLogicalIndex, HWND exceptHwnd = nullptr);
  // Opens a gap at `logicalIndex`: every suspended slot at or after it moves one
  // position later, so an insertion there keeps all logical positions unique.
  void OpenLogicalGapAt(size_t logicalIndex);
  size_t LogicalWindowCount() const;
  SuspendedSlot MakeAppendedSuspendedSlot() const;
  // `followFocus`: BSP only, see InsertBspLeaf. Windows found while a
  // workspace is first created don't follow focus, so they lay out evenly.
  void AppendTiledWithDefaultWeight(HWND hwnd, bool followFocus = true);
  // The weight a new slot at `weightIndex` of the current layout's weights
  // starts with: the one an overflow-created workspace inherited for that
  // position, else 1.0 (an even 0.5 split in Dwindle).
  double NewSlotWeight(size_t weightIndex) const;
  // The weights the current layout keeps (stack weights, grid weights or Dwindle
  // ratios), or nullptr for Monocle and Floating, which keep none.
  std::vector<double>* CurrentLayoutWeights();
  const std::vector<double>* CurrentLayoutWeights() const;
  // How many weights the current layout keeps for its tiled windows.
  size_t ExpectedWeightCount() const;
  void ResetSuspendedLayoutHints();
  void CopyLayoutGeometryFrom(const Workspace& source);
  void RememberCurrentLayoutGeometry();
  void RestoreRememberedLayoutGeometry();
  void DebugValidateMutation(const wchar_t* operation) const;
  // Dwindle dividers a window-size step pushed, with the ratio each had before
  // (layout/resize_steps.h). Ratios belong to slots, so a push is identified by
  // its split index and the window that was the split's first side; it lapses
  // (the push is kept) once that window leaves the slot.
  struct DwindlePush {
    size_t split = 0;
    HWND owner = nullptr;
    uint64_t ownerGeneration = 0;
    double ratio = 0.5;
  };
  bool IsDwindlePushCurrent(const DwindlePush& push) const;
  // Saved Dwindle ratios and pushes, so a resize that ends up changing nothing
  // can put back what accepting the shown layout changed.
  struct DwindleState {
    std::vector<double> ratios;
    std::vector<DwindlePush> pushes;
  };
  DwindleState SaveDwindleState() const { return {dwindleRatios_, dwindlePushes_}; }
  void RestoreDwindleState(DwindleState state) {
    dwindleRatios_ = std::move(state.ratios);
    dwindlePushes_ = std::move(state.pushes);
  }
  bool AcceptShownDwindleLayout(const RECT& workArea, LONG gap);
  bool AcceptShownBspLayout(const RECT& workArea, LONG gap);
  bool ApplyTiledStep(const RECT& workArea, LONG gap, size_t index, bool grow, LONG stepPercent);
  // What a size step moves for the window at `index` (layout/resize_steps.h).
  struct TiledStepTarget;
  std::optional<TiledStepTarget> ChooseTiledStepTarget(
      const RECT& workArea, LONG gap, size_t index, bool grow, const std::vector<RECT>& rects,
      const std::vector<Layout::DwindleSplit>& splits, const std::vector<WindowConstraints>& constraints) const;
  bool ResetDwindleSplit(const RECT& workArea, LONG gap, size_t index);
  std::optional<TiledStepTarget> ChooseBspStepTarget(
      const RECT& workArea, LONG gap, size_t index, const std::vector<WindowConstraints>& constraints) const;
  bool ResetBspSplit(const RECT& workArea, LONG gap, size_t index);
  // BSP: keeps the tree in step with the records, then orders the tiled windows
  // as their leaves are (SyncLayoutState calls it). Tiled and maximized windows
  // have leaves; any other window loses its own. One without a leaf gets one
  // by InsertBspLeaf.
  void SyncBspTree();
  // BSP: puts the tiled windows in their leaves' order, changing nothing else.
  void OrderTiledByBspLeaves();
  // BSP's arrival rule: when following focus, splits the most recently focused
  // tiled leaf. Otherwise splits the largest tiled leaf or becomes the root.
  // Nothing when the window already has a leaf.
  void InsertBspLeaf(HWND hwnd, bool followFocus, bool allowAlternateDirection = false);
  // BSP: true for a tiled window's leaf, the only leaves the layout shows.
  bool IsBspLeafTiled(HWND window) const;
  // BSP: true when `record` keeps a leaf: tiled or maximized.
  static bool KeepsBspLeaf(const WindowRecord& record);
  // BSP tree invariant violations, for Validate: in other layouts, a tree that
  // should be empty.
  std::vector<const wchar_t*> BspTreeProblems() const;
  void RecordDwindlePushes(const std::vector<double>& before, size_t ownSplit);
  void KeepDwindleDivider(size_t split);
  void RelaxDwindlePushes(const RECT& workArea, LONG gap, size_t afterSplit, size_t keepIndex);

  TileLayout layout_ = TileLayout::MasterStack;
  std::vector<HWND> windows_;
  std::unordered_map<HWND, WindowRecord> records_;
  double masterRatio_ = 0.5;
  // Positional defaults inherited by an overflow-created workspace, without HWNDs.
  std::vector<double> inheritedSlotWeights_;
  std::vector<double> stackWeights_;
  std::vector<double> gridWeights_;
  // User preferences; native minimums adjust rendered geometry only.
  std::vector<double> dwindleRatios_;
  std::vector<DwindlePush> dwindlePushes_;
  BspTree bspTree_;

  // Active slot geometry is remembered per layout family. This makes Monocle
  // and ordinary layout cycling presentation-like with respect to the previous
  // tiling geometry: switching away and back restores the exact weights/ratios
  // when the active window order is unchanged. Membership/order changes make the
  // remembered snapshot ineligible rather than trying to reinterpret old slots.
  struct RememberedGeometry {
    std::vector<HWND> order;
    std::vector<double> weights;
    bool operator==(const RememberedGeometry&) const = default;
  };
  // Where the current layout family's geometry is remembered, or nullptr for
  // Monocle and Floating.
  RememberedGeometry* RememberedGeometryForCurrentLayout();
  RememberedGeometry rememberedMasterStack_;
  RememberedGeometry rememberedGrid_;
  RememberedGeometry rememberedDwindle_;
  // BSP's tree, kept on the same terms: reused when the tiled order is unchanged.
  struct RememberedBsp {
    std::vector<HWND> order;
    BspTree tree;
    bool operator==(const RememberedBsp&) const = default;
  };
  RememberedBsp rememberedBsp_;

  HWND lastFocusedWindow_ = nullptr;
};

constexpr LONG kPlacementTolerancePx = 4;

using WorkspaceMap = std::unordered_map<
    DesktopMonitorKey, Workspace, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>;

// Access boundary for the workspace graph. WM thread only: every caller,
// including the out-of-context WinEvent callbacks, the tray windows and the
// companion publisher, runs there, so the repository needs no lock. It keeps its
// workspace snapshots and reverse ownership index consistent.
//
// The repository hands out copies. Nothing you do to a loaded Workspace is
// visible to anyone else until you Save() it.
class WorkspaceRepository {
 public:
  // Copies the workspace for `key` into `*workspace`. Returns false, leaving
  // `*workspace` unchanged, when no workspace has been saved for `key` yet.
  bool Load(const DesktopMonitorKey& key, Workspace* workspace) const;
  // True when a workspace has been saved for `key`.
  bool Contains(const DesktopMonitorKey& key) const;
  // The layout of `key`'s saved workspace, without copying it.
  std::optional<TileLayout> LayoutOf(const DesktopMonitorKey& key) const;
  // Stores a copy of `workspace` under `key`, replacing any previous one, and
  // updates the HWND -> owner index. Logs (but still saves) invariant violations.
  void Save(const DesktopMonitorKey& key, const Workspace& workspace);
  // For each of `keys` with a saved workspace, in order: loads a copy, passes it
  // to `change(key, workspace)`, and when that returns true saves the copy and
  // then calls `afterSave(key)`. Each save happens before the next key is
  // loaded, so `afterSave` may arrange or reload. `keys` must not be changed
  // by the callbacks.
  template <typename Change, typename AfterSave>
  void UpdateEach(const std::vector<DesktopMonitorKey>& keys, Change&& change, AfterSave&& afterSave) {
    for (const DesktopMonitorKey& key : keys) {
      Workspace workspace;
      if (!Load(key, &workspace) || !change(key, workspace)) continue;
      Save(key, workspace);
      afterSave(key);
    }
  }
  template <typename Change>
  void UpdateEach(const std::vector<DesktopMonitorKey>& keys, Change&& change) {
    UpdateEach(keys, std::forward<Change>(change), [](const DesktopMonitorKey&) {});
  }
  // True when any workspace has a record for `hwnd`, in any state.
  bool IsTracked(HWND hwnd) const;
  // True when some owning workspace has `hwnd` in its tiled layout.
  bool IsTiled(HWND hwnd) const;
  // True when some owning workspace holds `hwnd` suspended because it is maximized.
  bool IsSuspendedMaximized(HWND hwnd) const;
  // Workspaces with a record for `hwnd`. Normally zero or one; more than one
  // means ownership needs repair (Reconcile::EnsureUniqueWindowOwnership).
  std::vector<DesktopMonitorKey> OwnersOf(HWND hwnd) const;
  // Every HWND known to any workspace.
  std::vector<HWND> KnownWindows() const;
  // Connected monitors that have a saved workspace on `desktopId`.
  std::vector<HMONITOR> MonitorsOnDesktop(const GUID& desktopId) const;
  // Copies of every saved workspace, for diagnostics and global passes.
  std::vector<std::pair<DesktopMonitorKey, Workspace>> Snapshot() const;
  // Copy of the HWND -> owners index, for diagnostics.
  std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>>
      OwnershipIndexSnapshot() const;
  // Increases with every Save(), whether or not the workspace changed. Lets
  // observers such as the companion snapshot skip work when nothing was saved.
  uint64_t Revision() const;

 private:
  WorkspaceMap states_;
  uint64_t revision_ = 0;

  // Fast reverse ownership index used by WinEvent/reconciliation hot paths.
  // The vector normally contains exactly one key (global uniqueness invariant),
  // but retaining all keys preserves duplicate-owner repair semantics.
  std::unordered_map<HWND, std::vector<DesktopMonitorKey>> ownersByWindow_;
};

}  // namespace Model

// These model types are the vocabulary of the whole window manager, used in
// nearly every file. Writing Model::Workspace at ~1,500 call sites would add
// noise, not clarity, so they are deliberately available unqualified. Model
// *functions* are not aliased; call them through the type or namespace.
using Model::DesktopMonitorKey;
using Model::ManageState;
using Model::PlacementObservation;
using Model::PlacementResult;
using Model::SuspendedSlot;
using Model::SuspensionReason;
using Model::WindowConstraints;
using Model::WindowRecord;
using Model::Workspace;
using Model::WorkspaceGeometryItem;
using Model::WorkspaceRepository;

