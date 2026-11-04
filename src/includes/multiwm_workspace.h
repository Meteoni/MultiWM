// Workspace model interface and invariants.
// Implementation/interface fragment assembled by src/multiwm.cpp in dependency order.

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

struct SuspendedSlot {
  size_t index = 0;
  double weight = 1.0;
  bool hasWeight = false;
  bool wasMaster = false;
};

enum class ManageState {
  Tiled,
  Floating,
  Suspended,
  Ignored,
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
  // Retained through constraint-refresh replans so a pending admission can try
  // nearby insertion slots before it is floated.
  bool pendingInsertionPlacement = false;
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

// The authoritative logical model for one virtual-desktop + monitor workspace.
// All invariant-bearing storage is private: callers can inspect const views, but
// participation/order/weight changes must go through semantic operations below.
class Workspace {
 public:
  enum class PlacementAction { None, Float, Forget };

  Workspace() = default;

  TileLayout Layout() const { return layout_; }
  double MasterRatio() const { return masterRatio_; }
  const std::vector<HWND>& TiledWindows() const { return windows_; }
  const std::unordered_map<HWND, WindowRecord>& Records() const { return records_; }
  const std::vector<double>& StackWeights() const { return stackWeights_; }
  const std::vector<double>& GridWeights() const { return gridWeights_; }
  // Preferred active splits only; the final dormant slot ratio stays internal.
  std::vector<double> DwindleRatios() const {
    if (dwindleRatios_.empty()) return {};
    return {dwindleRatios_.begin(), dwindleRatios_.end() - 1};
  }
  size_t ActiveCount() const { return windows_.size(); }
  size_t VisibleWindowCount() const;
  Workspace EmptyWithInheritedLayoutSettings() const;
  size_t RecordCount() const { return records_.size(); }
  bool Empty() const { return windows_.empty(); }

  bool HasRecord(HWND hwnd) const;
  const WindowRecord* Find(HWND hwnd) const;
  bool IsTiled(HWND hwnd) const;
  size_t TiledIndex(HWND hwnd) const;
  size_t LogicalIndex(HWND hwnd) const;
  HWND LastFocusedWindow() const { return lastFocusedWindow_; }
  HWND LastFocusedTiledWindow() const {
    return IsTiled(lastFocusedWindow_) ? lastFocusedWindow_ : nullptr;
  }
  // Observation refresh does not alter logical participation or slot ownership.
  bool RememberFocusedWindow(HWND hwnd);
  bool UpdateConstraints(HWND hwnd, const WindowConstraints& constraints);
  bool HasSuspended() const;
  bool AllTiledVisible(const std::vector<HWND>& snapshot) const;

  void SetLayout(TileLayout layout);
  bool CycleLayout(const std::vector<TileLayout>& cycle);
  void SetMasterRatio(double ratio);
  bool MakeMaster(HWND hwnd, HWND* oldMaster = nullptr);
  bool SwapTiled(HWND first, HWND second);
  bool MovePendingAdmissionToIndex(HWND hwnd, size_t targetIndex);
  bool ClearInsertionPlacement(HWND hwnd);
  bool ClearDefaultFloatingCenter(HWND hwnd);
  bool EnforceAlwaysFloating(HWND hwnd);
  // Rollback compares identities/participation, never just the HWND set.
  bool HasSameWindowMembershipAs(const Workspace& other) const;
  bool HasSameLayoutStateAs(const Workspace& other) const;
  // Returns compatibility, not whether anything changed. Failure leaves this
  // object untouched; success restores geometry while retaining live observations.
  bool RestoreLayoutStateFrom(const Workspace& source);
  bool RestoreCompatibleActiveLayoutStateFrom(const Workspace& source);
  bool RememberFloatingGeometry(
      HWND hwnd, const RECT& frame, HMONITOR monitor, UINT dpi);

  // Participation changes preserve logical slot hints across Suspend/Restore.
  // Float releases a slot but keeps the record; Forget removes the record too.
  bool ActivateTiled(HWND hwnd);
  bool AdmitFloating(WindowRecord record);
  bool AdmitTiled(WindowRecord record);
  bool AdmitTiledAfter(WindowRecord record, HWND anchor);
  bool AdmitInitial(WindowRecord record, SuspensionReason reason);
  bool AdmitInitialAfter(
      WindowRecord record, SuspensionReason reason, HWND anchor);
  bool Suspend(HWND hwnd, SuspensionReason reason);
  bool Restore(HWND hwnd);
  bool Float(HWND hwnd);
  bool Forget(HWND hwnd);
  // Migration carries the record, not its old slot weights. Destination admission
  // rebases those weights and logical order according to the destination layout.
  bool ExtractForMigration(HWND hwnd, WindowRecord* outRecord);
  void AdmitMigrated(WindowRecord record, HWND anchor = nullptr);
  void MergeRememberedFloatingGeometryFrom(const Workspace& source);
  void MergeNonTiledRecordsFrom(const Workspace& source);
  void RepairForArrangement(
      const std::vector<HWND>& invalidWindows,
      const std::vector<WindowRecord>& missingActiveRecords);

  bool LearnMasterStackResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnGridResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  bool LearnDwindleResize(
      const RECT& workArea, LONG gap, size_t resizedIndex,
      const ::MoveSizeGesture& gesture);
  PlacementAction ApplyPlacementObservation(HWND hwnd, const PlacementObservation& observation);

  bool Validate(
      const DesktopMonitorKey& key, const wchar_t* context = L"workspace",
      std::vector<std::wstring>* errors = nullptr, bool logFailures = true) const;

  // Pure import from captured frames: no HWND queries or placement side effects.
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
class WorkspaceRepository {
 public:
  bool Load(const DesktopMonitorKey& key, Workspace* workspace) const;
  void Save(const DesktopMonitorKey& key, const Workspace& workspace);
  bool IsTracked(HWND hwnd) const;
  bool IsTiled(HWND hwnd) const;
  bool IsSuspendedMaximized(HWND hwnd) const;
  std::vector<DesktopMonitorKey> OwnersOf(HWND hwnd) const;
  std::vector<HWND> KnownWindows() const;
  std::vector<HMONITOR> MonitorsOnDesktop(const GUID& desktopId) const;
  std::vector<std::pair<DesktopMonitorKey, Workspace>> Snapshot() const;
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

