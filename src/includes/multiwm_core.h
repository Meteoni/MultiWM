#include <dwmapi.h>
#include <initguid.h>
#include <objbase.h>
#include <objectarray.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windhawk_utils.h>
#include <windows.h>
#include <winver.h>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <new>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

//=============================================================================
// Maintainer architecture map
//=============================================================================
//
// Runtime ownership
// -----------------
// OS callbacks (WinEvent + private virtual-desktop COM) never mutate workspace
// state directly. They enqueue messages onto the STA WM thread. That thread owns
// command handling, lifecycle reconciliation, workspace mutation, placement, and
// UI refresh sequencing.
//
// Event flow
// ----------
// WinEvent / VD callback
//        -> WM thread message
//        -> reconcile observed OS state into the model
//        -> arrange only affected workspaces
//        -> refresh user feedback
//
// Logical modules / reading order
// -------------------------------
// Model                    authoritative Workspace + repository invariants
// Layout                   deterministic rectangle generation
// Window observation       HWND -> value observations, eligibility, placement
// Reconcile                OS facts -> model transitions / ownership repair
// Commands                 explicit user intents over the model
// Platform::WindowEvents   WinEvent adapter + queueing only
// Platform::VirtualDesktop private shell COM adapter + notification sink
// TrayUi / Diagnostics     presentation and observability
// Bootstrap                settings, STA dispatch, Windhawk lifecycle
//
// Packaging deliberately remains one .wh.cpp translation unit for Windhawk. The
// namespaces above are the module boundaries; a future repository split should
// preserve them and amalgamate back to this single distributable source rather
// than reintroducing cross-file global ownership.
//
// Model invariants
// ----------------
// 1. Workspace::records is the authoritative set of logically known HWNDs.
// 2. Workspace::windows contains only active Tiled records, exactly once each,
//    in logical layout order.
// 3. Floating records exist in records but never consume a slot in windows.
// 4. Suspended records exist in records, do not appear in windows, and preserve
//    a saved logical slot/weight until restored or explicitly floated/forgotten.
// 5. Columns/Rows keep one grid weight per active tiled window.
//    MasterStack/MasterStackH keep one stack weight per non-master active window.
// 6. A managed HWND has exactly one desktop+monitor workspace owner.
// 7. Visibility/cloak events are dirty signals; ownership comes from explicit
//    desktop/monitor queries, not from cloak state.
// 8. Automatic vs Manual mode changes only admission of previously unknown HWNDs;
//    all already-managed reconciliation and retile behavior remains active.
//
// If a change cannot be explained in terms of those invariants, it probably
// belongs in the observation/reconciliation layer rather than layout code.

#define SAFE_RELEASE(p) \
  do {                  \
    if (p) {            \
      (p)->Release();   \
      (p) = nullptr;    \
    }                   \
  } while (0)

// Private virtual-desktop interface types are deliberately incomplete here.
// Their ABI definitions live in the Platform::VirtualDesktop section after the
// model/layout code, so maintainers encounter the WM model before shell internals.
namespace Platform::VirtualDesktop {
struct IVirtualDesktop;
struct IVirtualDesktopManagerInternal;
struct IVirtualDesktopNotificationService;
struct VirtualDesktopNotificationObject;

bool InitializeVirtualDesktopAPI();
void CleanupVirtualDesktopAPI();
bool GetCurrentDesktopId(GUID* outGuid);
bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid);
bool RegisterVirtualDesktopNotifications();
}  // namespace Platform::VirtualDesktop

// Narrow platform surface used by the WM core. The private COM implementation
// lives with the other Windows adapters near the end of the source.
using Platform::VirtualDesktop::CleanupVirtualDesktopAPI;
using Platform::VirtualDesktop::GetCurrentDesktopId;
using Platform::VirtualDesktop::GetWindowDesktopIdSafe;
using Platform::VirtualDesktop::InitializeVirtualDesktopAPI;
using Platform::VirtualDesktop::RegisterVirtualDesktopNotifications;

struct PendingWorkspaceArrange {
  GUID desktopId{};
  HMONITOR monitor = nullptr;
};

//=============================================================================
// Runtime/configuration state groups
//=============================================================================

// Hotkey IDs are stable message identifiers, not user configuration.
enum HotkeyIds {
  HK_TILE = 1,
  HK_LAYOUT = 2,
  HK_SWAP_MASTER = 3,
  HK_MANAGEMENT_MODE_TOGGLE = 4,
  HK_FLOAT_FOCUSED = 5,
  HK_DIAGNOSTIC_DUMP = 6,
};

// Messages consumed by the serialized WM thread.
constexpr UINT WM_APP_MOVE_SIZE_END = WM_APP + 1;
constexpr UINT WM_APP_WINDOW_EVENT = WM_APP + 2;
constexpr UINT WM_APP_VIRTUAL_DESKTOP_CHANGED = WM_APP + 3;
constexpr UINT WM_APP_TRAY_REFRESH = WM_APP + 4;
constexpr UINT WM_APP_LAYOUT_CYCLE = WM_APP + 5;

enum class TileLayout { MasterStack, Columns, Rows, MasterStackH, BSP, Monocle, Floating, COUNT };
enum class MouseMoveBehavior { Float, Swap };
enum class ExclusionMatch { Process, Class, Title };

struct ExclusionRule {
  ExclusionMatch match = ExclusionMatch::Process;
  std::wstring value;
};

struct WorkspaceInsets {
  LONG left = 4;
  LONG top = 4;
  LONG right = 4;
  LONG bottom = 4;
};

// Windhawk settings and immutable-at-runtime policy values. Settings reloads
// replace these fields; transient WM state lives in WmRuntime instead.
struct SettingsState {
  int windowsVersionIndex = 4;
  UINT reconcileDelayMs = 80;

  UINT tilingModifiers = MOD_ALT;
  UINT tileKey = 'D';
  UINT layoutKey = 'L';
  UINT swapMasterKey = 'M';
  UINT managementModeToggleKey = 'U';
  UINT floatFocusedKey = 0;
  UINT diagnosticDumpKey = 'I';

  WorkspaceInsets insets{};
  LONG gap = 4;
  LONG masterPercent = 50;

  MouseMoveBehavior mouseMoveBehavior = MouseMoveBehavior::Float;
  TileLayout defaultLayout = TileLayout::MasterStack;
  bool enabled = true;
  std::vector<ExclusionRule> exclusionRules;
};

// Private virtual-desktop COM state. This entire object belongs to the STA WM
// thread; callback entry points only post work back to that same thread.
struct VirtualDesktopRuntime {
  IServiceProvider* serviceProvider = nullptr;
  Platform::VirtualDesktop::IVirtualDesktopManagerInternal* managerInternal = nullptr;
  IVirtualDesktopManager* desktopManager = nullptr;
  Platform::VirtualDesktop::IVirtualDesktopNotificationService* notificationService = nullptr;
  Platform::VirtualDesktop::VirtualDesktopNotificationObject* notificationObject = nullptr;
  DWORD notificationCookie = 0;
  bool notificationsRegistered = false;
  DWORD explorerBuild = 0;
  DWORD explorerRevision = 0;
  LONG changeQueued = 0;
  bool initialized = false;
};

// Serialized actor/thread state. Manual mode is represented only by
// autoDiscoverySuspended; the rest of the WM pipeline remains live.
struct WmRuntime {
  HANDLE thread = nullptr;
  DWORD threadId = 0;
  HANDLE readyEvent = nullptr;
  volatile bool stopRequested = false;

  UINT_PTR lifecycleTimer = 0;
  std::vector<HWND> lifecycleDirtyWindows;
  std::vector<PendingWorkspaceArrange> pendingDesktopArranges;
  bool autoDiscoverySuspended = true;
};

static SettingsState g_settings;
static VirtualDesktopRuntime g_vd;
static WmRuntime g_wm;

struct MoveSizeGesture;

static void AssertWmThread(const wchar_t* operation) {
  if (!g_wm.threadId) return;
  const DWORD currentThread = GetCurrentThreadId();
  if (currentThread == g_wm.threadId) return;

  Wh_Log(
      L"WM-thread affinity violation in %s: expected=%lu actual=%lu",
      operation ? operation : L"<unknown>", g_wm.threadId, currentThread);
  assert(currentThread == g_wm.threadId);
}

namespace Model {

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

struct DesktopMonitorKey {
  GUID desktopId;
  HMONITOR monitor;
};

struct DesktopMonitorKeyHash {
  size_t operator()(const DesktopMonitorKey& key) const {
    size_t hash = GuidHash{}(key.desktopId);
    hash ^= std::hash<void*>{}(key.monitor) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
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
// layout. This mirrors FancyWM's Restored vs Minimized/Maximized state boundary,
// while retaining Hidden for close-to-tray behavior.
enum class SuspensionReason {
  None,
  Hidden,
  Minimized,
  Maximized,
};

enum class PlacementResult {
  Success,
  AdjustedByWindow,
  AccessDenied,
  Refused,
  Dead,
};

struct WindowRecord {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  ManageState state = ManageState::Tiled;

  // Capabilities are observations, not hard policy yet. They give later rule/
  // constraint work somewhere stable to live without re-querying every branch.
  bool canMove = true;
  bool canResize = true;
  bool topmost = false;

  int placementFailures = 0;
  PlacementResult lastPlacementResult = PlacementResult::Success;
  RECT lastRequestedRect{};
  RECT lastObservedRect{};

  // Only meaningful while state == Suspended. It preserves the tiled logical
  // position/weight across non-participating physical states.
  SuspensionReason suspensionReason = SuspensionReason::None;
  SuspendedSlot savedSlot{};
  bool hasSavedSlot = false;
};

struct PlacementObservation {
  PlacementResult result = PlacementResult::Success;
  RECT requested{};
  RECT observed{};
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
  size_t ActiveCount() const { return windows_.size(); }
  size_t RecordCount() const { return records_.size(); }
  bool Empty() const { return windows_.empty(); }

  bool HasRecord(HWND hwnd) const;
  const WindowRecord* Find(HWND hwnd) const;
  bool IsTiled(HWND hwnd) const;
  size_t TiledIndex(HWND hwnd) const;
  size_t LogicalIndex(HWND hwnd) const;
  bool HasSuspended() const;
  bool AllTiledVisible(const std::vector<HWND>& snapshot) const;

  void SetLayout(TileLayout layout);
  void CycleLayout();
  void SetMasterRatio(double ratio);
  bool MakeMaster(HWND hwnd, HWND* oldMaster = nullptr);
  bool SwapTiled(HWND first, HWND second);
  bool ResetPlacementFailures(HWND hwnd);

  bool ActivateTiled(HWND hwnd, bool resetPlacementFailures = false);
  bool AdmitTiled(WindowRecord record, bool resetPlacementFailures = false);
  bool AdmitInitial(WindowRecord record, SuspensionReason reason);
  bool Suspend(HWND hwnd, SuspensionReason reason);
  bool Restore(HWND hwnd);
  bool Float(HWND hwnd);
  bool Forget(HWND hwnd);
  bool ExtractForMigration(HWND hwnd, WindowRecord* outRecord);
  void AdmitMigrated(WindowRecord record);
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
  PlacementAction ApplyPlacementObservation(HWND hwnd, const PlacementObservation& observation);

  bool Validate(
      const DesktopMonitorKey& key, const wchar_t* context = L"workspace") const;

  static Workspace AdoptGeometry(
      TileLayout layout, const RECT& workArea, LONG gap,
      double fallbackMasterRatio,
      const std::vector<WorkspaceGeometryItem>& items,
      HWND preferredFirstWin = nullptr);

 private:
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
  void DebugValidateMutation(const wchar_t* operation) const;

  TileLayout layout_ = TileLayout::MasterStack;
  std::vector<HWND> windows_;
  std::unordered_map<HWND, WindowRecord> records_;
  double masterRatio_ = 0.5;
  std::vector<double> stackWeights_;
  std::vector<double> gridWeights_;
};

constexpr int kPlacementFailureLimit = 3;
constexpr LONG kPlacementTolerancePx = 4;

using WorkspaceMap = std::unordered_map<
    DesktopMonitorKey, Workspace, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>;

// Synchronization boundary for the workspace graph. Writers are restricted to
// the serialized WM thread; WinEvent callbacks may perform lock-protected reads.
class WorkspaceRepository {
 public:
  bool Load(const DesktopMonitorKey& key, Workspace* workspace) const;
  void Save(const DesktopMonitorKey& key, const Workspace& workspace);
  bool IsTracked(HWND hwnd) const;
  bool IsSuspendedMaximized(HWND hwnd) const;
  std::vector<DesktopMonitorKey> OwnersOf(HWND hwnd) const;
  std::vector<HWND> KnownWindows() const;
  std::vector<HMONITOR> MonitorsOnDesktop(const GUID& desktopId) const;
  std::vector<std::pair<DesktopMonitorKey, Workspace>> Snapshot() const;
  void ClearAfterWmStopped();

 private:
  mutable SRWLOCK lock_ = SRWLOCK_INIT;
  WorkspaceMap states_;
};


}  // namespace Model

using Model::DesktopMonitorKey;
using Model::ManageState;
using Model::PlacementObservation;
using Model::PlacementResult;
using Model::SuspendedSlot;
using Model::SuspensionReason;
using Model::WindowRecord;
using Model::Workspace;
using Model::WorkspaceGeometryItem;
using Model::WorkspaceRepository;

struct MoveSizeTracker {
  SRWLOCK lock = SRWLOCK_INIT;
  std::unordered_map<HWND, RECT> startRects;
  std::unordered_map<HWND, RECT> endRects;
  std::unordered_map<HWND, POINT> endPoints;
};

struct WinEventHooks {
  HWINEVENTHOOK moveSize = nullptr;
  HWINEVENTHOOK minimize = nullptr;
  HWINEVENTHOOK hideDestroy = nullptr;
  HWINEVENTHOOK cloak = nullptr;
  HWINEVENTHOOK state = nullptr;
  HWINEVENTHOOK foreground = nullptr;
  HWINEVENTHOOK locationChange = nullptr;
};

static WorkspaceRepository g_workspaces;
static MoveSizeTracker g_moveSize;
static WinEventHooks g_hooks;

