// Workspace model interface and invariants.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

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
};

struct DesktopMonitorKeyHash {
  size_t operator()(const DesktopMonitorKey& key) const {
    size_t hash = GuidHash{}(key.desktopId);
    hash ^= MonitorIdHash{}(key.monitor) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct DesktopMonitorKeyEqual {
  bool operator()(const DesktopMonitorKey& a, const DesktopMonitorKey& b) const {
    return IsEqualGUID(a.desktopId, b.desktopId) && a.monitor == b.monitor;
  }
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
  Ignored,    // Reserved. Current code never assigns it; treated like Floating.
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

  // Monotonic admission generation provides stable ordering for constraint
  // recovery candidates. Reordering never changes this value.
  uint64_t tilingGeneration = 0;
  // True for a newly admitted or restored window that has not yet settled in its
  // slot. While true, arrangement may move it to a nearby slot that fits its
  // minimum size instead of floating it. Cleared once its placement holds.
  bool pendingInsertionPlacement = false;
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

  PlacementResult lastPlacementResult = PlacementResult::Success;
  RECT lastRequestedRect{};
  RECT lastObservedRect{};

  // Last meaningful floating frame in physical screen pixels. The capture DPI
  // and monitor let migration restore size sensibly across per-monitor scaling.
  RECT floatingRect{};
  HMONITOR floatingMonitor = nullptr;
  UINT floatingDpi = 96;
  bool hasFloatingRect = false;

  // Only meaningful while state == Suspended. It preserves the tiled logical
  // position/weight across non-participating physical states.
  SuspensionReason suspensionReason = SuspensionReason::None;
  SuspendedSlot savedSlot{};
  bool hasSavedSlot = false;
};

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
  size_t ActiveCount() const { return windows_.size(); }
  // Windows counted against the auto-tiling limit: tiled plus maximized-suspended.
  // Always 0 in the Floating layout.
  size_t VisibleWindowCount() const;
  // A workspace with no windows that keeps this one's layout, master ratio and
  // per-slot weights. Used for a desktop created by overflow.
  Workspace EmptyWithInheritedLayoutSettings() const;
  size_t RecordCount() const { return records_.size(); }
  // True when no window is tiled (floating/suspended records may still exist).
  bool Empty() const { return windows_.empty(); }

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
  HWND LastFocusedTiledWindow() const {
    return IsTiled(lastFocusedWindow_) ? lastFocusedWindow_ : nullptr;
  }
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
  // Stores the floating frame (physical pixels) to restore when floated again.
  // Returns false for an empty frame, null monitor, or an unchanged value.
  bool RememberFloatingGeometry(
      HWND hwnd, const RECT& frame, HMONITOR monitor, UINT dpi);
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
  // Moves a still-settling window (pendingInsertionPlacement) to another tiled
  // index, shifting suspended slots so every window keeps a unique position.
  bool MovePendingAdmissionToIndex(HWND hwnd, size_t targetIndex);
  // Marks a window as settled in its slot (clears pendingInsertionPlacement).
  bool ClearInsertionPlacement(HWND hwnd);
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
  // Adds `record` as Tiled at the end. If the window is already known, refreshes
  // its observations and re-tiles it. True when added or re-tiled.
  // Exception to the convention: for a known window that is already tiled this
  // returns false but still refreshes its observed fields from `record`.
  bool AdmitTiled(WindowRecord record);
  // Like AdmitTiled, but inserts right after `anchor` when `anchor` is tiled and
  // the window is new. Otherwise behaves exactly like AdmitTiled.
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
  // Adds a record extracted from another workspace, after `anchor` if given and
  // tiled, otherwise at the end. Tiled/suspended windows start settling again.
  void AdmitMigrated(WindowRecord record, HWND anchor = nullptr);
  // Copies remembered floating frames from `source` for windows known here.
  void MergeRememberedFloatingGeometryFrom(const Workspace& source);
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
  // physical pixels. Call the variant that matches Layout().

  bool LearnMasterStackResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnGridResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnDwindleResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);

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
  void EnsureWeights();
  void RemoveActiveAt(size_t index, SuspendedSlot* saved = nullptr);
  size_t ActiveIndexToLogicalIndex(size_t activeIndex) const;
  size_t LogicalIndexToActiveIndex(size_t logicalIndex, HWND restoringHwnd) const;
  void CloseLogicalGapAfterRemoval(size_t removedLogicalIndex, HWND exceptHwnd = nullptr);
  // Opens a gap at `logicalIndex`: every suspended slot at or after it moves one
  // position later, so an insertion there keeps all logical positions unique.
  void OpenLogicalGapAt(size_t logicalIndex);
  size_t LogicalWindowCount() const;
  SuspendedSlot MakeAppendedSuspendedSlot() const;
  void AppendTiledWithDefaultWeight(HWND hwnd);
  void ResetSuspendedLayoutHints();
  void CopyLayoutGeometryFrom(const Workspace& source);
  void RememberCurrentLayoutGeometry();
  void RestoreRememberedLayoutGeometry();
  void DebugValidateMutation(const wchar_t* operation) const;

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

  // Active slot geometry is remembered per layout family. This makes Monocle
  // and ordinary layout cycling presentation-like with respect to the previous
  // tiling geometry: switching away and back restores the exact weights/ratios
  // when the active window order is unchanged. Membership/order changes make the
  // remembered snapshot ineligible rather than trying to reinterpret old slots.
  std::vector<HWND> rememberedMasterStackOrder_;
  std::vector<double> rememberedMasterStackWeights_;
  std::vector<HWND> rememberedGridOrder_;
  std::vector<double> rememberedGridWeights_;
  std::vector<HWND> rememberedDwindleOrder_;
  std::vector<double> rememberedDwindleRatios_;

  HWND lastFocusedWindow_ = nullptr;
};

constexpr LONG kPlacementTolerancePx = 4;

using WorkspaceMap = std::unordered_map<
    DesktopMonitorKey, Workspace, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>;

// Access boundary for the workspace graph. All current callers, including
// WinEvent callbacks, run on the serialized WM thread; the repository keeps its
// workspace snapshots and reverse ownership index consistent.
//
// The repository hands out copies. Nothing you do to a loaded Workspace is
// visible to anyone else until you Save() it.
class WorkspaceRepository {
 public:
  // Copies the workspace for `key` into `*workspace`. Returns false, leaving
  // `*workspace` unchanged, when no workspace has been saved for `key` yet.
  bool Load(const DesktopMonitorKey& key, Workspace* workspace) const;
  // Stores a copy of `workspace` under `key`, replacing any previous one, and
  // updates the HWND -> owner index. Logs (but still saves) invariant violations.
  void Save(const DesktopMonitorKey& key, const Workspace& workspace);
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

 private:
  mutable SRWLOCK lock_ = SRWLOCK_INIT;
  WorkspaceMap states_;

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

