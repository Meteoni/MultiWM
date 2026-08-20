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
#include <atomic>
#include <cmath>
#include <cassert>
#include <cstdarg>
#include <deque>
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
// 8. Management mode controls group membership policy, not the core WM pipeline:
//    Automatic admits unknown HWNDs and migrates managed HWNDs between workspaces;
//    Manual admits only through explicit commands, forgets minimized members, and
//    releases membership when a managed HWND moves to another virtual desktop.
//    Maximized members remain suspended so restoring preserves their exact slot.
// 9. Floating geometry is remembered independently of tiled geometry. Tiling and
//    geometry adoption never overwrite the last meaningful floating rectangle.
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

struct NotificationInterfaceConfig {
  IID iid{};
  int methodCount = 0;
  int currentChangedIndex = -1;
  bool currentChangedHasMonitors = false;
};

struct VirtualDesktopAbiProfile {
  IID managerInternal{};
  bool usesHMonitor = false;
  NotificationInterfaceConfig notification{};
};

bool InitializeVirtualDesktopAPI();
void CleanupVirtualDesktopAPI();
void AbandonVirtualDesktopAPIForShellRestart();
bool GetCurrentDesktopId(GUID* outGuid);
bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid);
bool IsWindowOnCurrentDesktopSafe(HWND hwnd, BOOL* onCurrent);
bool RegisterVirtualDesktopNotifications();
}  // namespace Platform::VirtualDesktop

namespace RuntimeLifecycle {
void RequestMaintenance(bool resetAttempts = false);
void RunMaintenanceNow();
void NotifyShellRestarted();
void NotifyDisplayTopologyChanged();
}  // namespace RuntimeLifecycle

// Narrow platform surface used by the WM core. The private COM implementation
// lives with the other Windows adapters near the end of the source.
using Platform::VirtualDesktop::CleanupVirtualDesktopAPI;
using Platform::VirtualDesktop::GetCurrentDesktopId;
using Platform::VirtualDesktop::GetWindowDesktopIdSafe;
using Platform::VirtualDesktop::InitializeVirtualDesktopAPI;
using Platform::VirtualDesktop::IsWindowOnCurrentDesktopSafe;

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
constexpr UINT WM_APP_RECONCILE_NOW = WM_APP + 6;

enum class TileLayout { MasterStack, Columns, Rows, MasterStackH, BSP, Monocle, Floating, COUNT };
enum class MouseMoveBehavior { Float, Swap };
enum class ExclusionMatch { Process, Class, Title };
enum class ManagementMode { Manual, Automatic };

static std::vector<TileLayout> MakeBuiltInLayoutCycle() {
  return {
      TileLayout::MasterStack,
      TileLayout::Columns,
      TileLayout::Rows,
      TileLayout::MasterStackH,
      TileLayout::BSP,
      TileLayout::Monocle,
      TileLayout::Floating};
}

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

struct FloatingDefaultSize {
  LONG width = 960;
  LONG height = 640;
};

// Windhawk settings and immutable-at-runtime policy values. Settings reloads
// replace these fields; transient WM state lives in WmRuntime instead.
struct SettingsState {
  UINT reconcileDelayMs = 80;
  UINT conformanceLeaseMs = 1500;

  UINT tilingModifiers = MOD_ALT;
  UINT tileKey = 'D';
  UINT layoutKey = 'L';
  UINT swapMasterKey = 'M';
  UINT managementModeToggleKey = 'U';
  UINT floatFocusedKey = 0;
  UINT diagnosticDumpKey = 'I';

  // User-facing pixel values are logical pixels at 96 DPI; scale only at a
  // concrete monitor/window boundary so stored screen geometry remains physical.
  WorkspaceInsets insetsDip{};
  LONG gapDip = 4;
  FloatingDefaultSize floatingDefaultSizeDip{};
  LONG masterPercent = 50;

  MouseMoveBehavior mouseMoveBehavior = MouseMoveBehavior::Float;
  TileLayout defaultLayout = TileLayout::MasterStack;
  std::vector<TileLayout> layoutCycle = MakeBuiltInLayoutCycle();
  std::vector<ExclusionRule> exclusionRules;
  std::wstring diagnosticsOutputPath =
      L"%USERPROFILE%\\Documents\\TilingHelperDiagnostics";
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
  HRESULT notificationLastHr = S_OK;
  DWORD shellPid = 0;
  ULONGLONG notificationLastAttemptTickMs = 0;
  ULONGLONG notificationLastSuccessTickMs = 0;
  DWORD explorerBuild = 0;
  DWORD explorerRevision = 0;
  Platform::VirtualDesktop::VirtualDesktopAbiProfile abi{};
  bool abiResolved = false;
  LONG changeQueued = 0;
  GUID lastKnownDesktop{};
  bool lastKnownDesktopValid = false;
  bool initialized = false;
};

// Serialized actor/thread state. Management mode affects admission/retention
// policy only; observation, reconciliation, resize learning, and arrangement
// remain live in both modes. Only threadId is observed outside the actor.
struct WmRuntime {
  HANDLE thread = nullptr;
  std::atomic<DWORD> threadId{0};
  HANDLE readyEvent = nullptr;

  UINT_PTR lifecycleTimer = 0;
  UINT_PTR maintenanceTimer = 0;
  UINT maintenanceAttempts = 0;
  bool maintenanceRunning = false;
  bool lifecycleRetryAfterPlatformRecovery = false;
  bool forceMonitorReconcile = false;
  bool comInitialized = false;
  GUID reconciledDesktop{};
  bool reconciledDesktopValid = false;
  bool pendingDesktopSwitchFlyouts = false;
  std::vector<HWND> lifecycleDirtyWindows;
  std::vector<PendingWorkspaceArrange> pendingDesktopArranges;
  ManagementMode managementMode = ManagementMode::Manual;
};

namespace Diagnostics {

// Lifetime counters are deliberately cheap increments on the serialized WM
// actor, or on the controller only after that actor has been joined. They are
// never flushed continuously; the diagnostic hotkey snapshots them into a
// report, preserving the mod's event-driven/idle-zero-cost design.
struct Counters {
  uint64_t wmSessionStarts = 0;
  uint64_t settingsReloads = 0;

  uint64_t lifecycleEventsProcessed = 0;
  uint64_t lifecycleDestroy = 0;
  uint64_t lifecycleMinimizeStart = 0;
  uint64_t lifecycleMinimizeEnd = 0;
  uint64_t lifecycleStateChange = 0;
  uint64_t lifecycleLocationChange = 0;
  uint64_t lifecycleHide = 0;
  uint64_t lifecycleShow = 0;
  uint64_t lifecycleCloaked = 0;
  uint64_t lifecycleUncloaked = 0;
  uint64_t lifecycleScheduleRequests = 0;
  uint64_t lifecycleTimerResets = 0;
  uint64_t lifecycleReconciles = 0;
  uint64_t lifecycleDirtyWindowsProcessed = 0;
  uint64_t moveSizeEndMessages = 0;
  uint64_t trayRefreshMessages = 0;
  uint64_t userMoveGestures = 0;
  uint64_t userResizeGestures = 0;
  uint64_t userNoopGestures = 0;
  uint64_t floatingGeometryUserUpdates = 0;
  uint64_t tiledSwapActions = 0;
  uint64_t tiledFloatActions = 0;
  uint64_t dividerUpdates = 0;

  uint64_t reconcileCalls = 0;
  uint64_t reconcileWindowsExamined = 0;
  uint64_t workspaceSaves = 0;
  uint64_t workspaceMigrations = 0;

  uint64_t enumWindowsPasses = 0;
  uint64_t enumWindowsVisited = 0;
  uint64_t tileCandidatesCollected = 0;

  uint64_t arrangeCalls = 0;
  uint64_t arrangePasses = 0;
  uint64_t layoutPlansBuilt = 0;

  uint64_t placementChecks = 0;
  uint64_t placementNoOps = 0;
  uint64_t placementPreflightStops = 0;
  uint64_t setWindowPosCalls = 0;
  uint64_t placementSuccess = 0;
  uint64_t placementAdjusted = 0;
  uint64_t placementAccessDenied = 0;
  uint64_t placementRefused = 0;
  uint64_t placementDead = 0;
  uint64_t floatingGeometryRepairs = 0;

  uint64_t virtualDesktopChangesProcessed = 0;
  uint64_t virtualDesktopFallbackChanges = 0;
  uint64_t vdApiInitAttempts = 0;
  uint64_t vdApiInitSuccesses = 0;
  uint64_t vdApiInitFailures = 0;
  uint64_t vdApiReinitializations = 0;
  uint64_t vdNotificationRegisterAttempts = 0;
  uint64_t vdNotificationRegisterSuccesses = 0;
  uint64_t vdNotificationRegisterFailures = 0;
  uint64_t runtimeMaintenanceRuns = 0;
  uint64_t runtimeMaintenanceRetries = 0;
  uint64_t runtimeMaintenanceExhausted = 0;
  uint64_t shellRestartSignals = 0;
  uint64_t displayTopologySignals = 0;
  uint64_t winEventHookInstallFailures = 0;

  uint64_t conformanceLeasesStarted = 0;
  uint64_t conformanceLeaseRepairs = 0;
  uint64_t conformanceLeaseExhausted = 0;

  uint64_t tileCommands = 0;
  uint64_t layoutCycleCommands = 0;
  uint64_t floatFocusedCommands = 0;
  uint64_t swapMasterCommands = 0;

  uint64_t reportsWritten = 0;
  uint64_t reportWriteFailures = 0;
};

struct RecentEvent {
  ULONGLONG tickMs = 0;
  std::wstring text;
};

struct LifecycleBurst {
  ULONGLONG tickMs = 0;
  size_t dirtyWindows = 0;
  size_t pendingArranges = 0;
};

struct DiagnosticCost {
  uint64_t attempts = 0;
  ULONGLONG wallMs = 0;
  uint64_t processCpu100ns = 0;
  uint64_t wmCpu100ns = 0;
};

struct Runtime {
  ULONGLONG processStartedTickMs = 0;
  ULONGLONG wmSessionStartedTickMs = 0;
  ULONGLONG lastReportTickMs = 0;
  uint64_t reportSequence = 0;
  Counters counters{};
  Counters lastReportCounters{};
  DiagnosticCost lifetimeDiagnosticCost{};
  DiagnosticCost wmSessionDiagnosticCost{};
  std::deque<RecentEvent> significantEvents;
  std::deque<LifecycleBurst> lifecycleBursts;
};

static Runtime g_runtime;
static constexpr size_t kRecentEventLimit = 64;
static constexpr size_t kLifecycleBurstLimit = 16;

static void InitializeProcessRuntime() {
  if (!g_runtime.processStartedTickMs) {
    g_runtime.processStartedTickMs = GetTickCount64();
  }
}

static void RecordEvent(const wchar_t* format, ...) {
  InitializeProcessRuntime();
  wchar_t buffer[768]{};
  va_list args;
  va_start(args, format);
  int written = vswprintf(buffer, ARRAYSIZE(buffer), format, args);
  va_end(args);
  if (written < 0) {
    const wchar_t fallback[] = L"<diagnostic event formatting failed>";
    wcsncpy(buffer, fallback, ARRAYSIZE(buffer) - 1);
  }

  g_runtime.significantEvents.push_back({GetTickCount64(), buffer});
  while (g_runtime.significantEvents.size() > kRecentEventLimit) {
    g_runtime.significantEvents.pop_front();
  }
}

static void RecordLifecycleBurst(size_t dirtyWindows, size_t pendingArranges) {
  InitializeProcessRuntime();
  g_runtime.lifecycleBursts.push_back(
      {GetTickCount64(), dirtyWindows, pendingArranges});
  while (g_runtime.lifecycleBursts.size() > kLifecycleBurstLimit) {
    g_runtime.lifecycleBursts.pop_front();
  }
}

static void BeginWmSession() {
  InitializeProcessRuntime();
  g_runtime.wmSessionStartedTickMs = GetTickCount64();
  g_runtime.wmSessionDiagnosticCost = {};
  ++g_runtime.counters.wmSessionStarts;
  RecordEvent(L"WM session started");
}

}  // namespace Diagnostics

static SettingsState g_settings;
static VirtualDesktopRuntime g_vd;
static WmRuntime g_wm;

static bool IsManualMode() {
  return g_wm.managementMode == ManagementMode::Manual;
}

static bool IsAutomaticMode() {
  return g_wm.managementMode == ManagementMode::Automatic;
}

struct MoveSizeGesture;

static void AssertWmThread(const wchar_t* operation) {
  const DWORD expectedThread = g_wm.threadId.load(std::memory_order_acquire);
  if (!expectedThread) return;
  const DWORD currentThread = GetCurrentThreadId();
  if (currentThread == expectedThread) return;

  Wh_Log(
      L"WM-thread affinity violation in %s: expected=%lu actual=%lu",
      operation ? operation : L"<unknown>", expectedThread, currentThread);
  assert(currentThread == expectedThread);
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
  RECT requested{};
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
  bool CycleLayout(const std::vector<TileLayout>& cycle);
  void SetMasterRatio(double ratio);
  bool MakeMaster(HWND hwnd, HWND* oldMaster = nullptr);
  bool SwapTiled(HWND first, HWND second);
  bool ResetPlacementFailures(HWND hwnd);
  bool RememberFloatingGeometry(
      HWND hwnd, const RECT& frame, HMONITOR monitor, UINT dpi);

  bool ActivateTiled(HWND hwnd, bool resetPlacementFailures = false);
  bool AdmitTiled(WindowRecord record, bool resetPlacementFailures = false);
  bool AdmitInitial(WindowRecord record, SuspensionReason reason);
  bool Suspend(HWND hwnd, SuspensionReason reason);
  bool Restore(HWND hwnd);
  bool Float(HWND hwnd);
  bool Forget(HWND hwnd);
  bool ExtractForMigration(HWND hwnd, WindowRecord* outRecord);
  void AdmitMigrated(WindowRecord record);
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
  PlacementAction ApplyPlacementObservation(HWND hwnd, const PlacementObservation& observation);

  bool Validate(
      const DesktopMonitorKey& key, const wchar_t* context = L"workspace",
      std::vector<std::wstring>* errors = nullptr, bool logFailures = true) const;

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
  std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>>
      OwnershipIndexSnapshot() const;
  void ClearAfterWmStopped();

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

struct ConformanceLease {
  RECT expectedRect{};
  ULONGLONG expiresAtTickMs = 0;
  unsigned repairs = 0;
};

struct ConformanceLeaseTracker {
  SRWLOCK lock = SRWLOCK_INIT;
  std::unordered_map<HWND, ConformanceLease> leases;
};

static ConformanceLeaseTracker g_conformanceLeases;
static constexpr unsigned kConformanceLeaseMaxRepairs = 4;

static void CancelConformanceLease(HWND hwnd);

static bool ReadActiveConformanceLease(HWND hwnd, ConformanceLease* outLease) {
  if (!hwnd || !outLease) return false;
  bool active = false;
  bool expired = false;
  const ULONGLONG now = GetTickCount64();
  AcquireSRWLockShared(&g_conformanceLeases.lock);
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end()) {
    if (now < it->second.expiresAtTickMs) {
      *outLease = it->second;
      active = true;
    } else {
      expired = true;
    }
  }
  ReleaseSRWLockShared(&g_conformanceLeases.lock);
  if (expired) CancelConformanceLease(hwnd);
  return active;
}

static bool HasActiveConformanceLease(HWND hwnd) {
  ConformanceLease ignored;
  return ReadActiveConformanceLease(hwnd, &ignored);
}

static void CancelConformanceLease(HWND hwnd) {
  if (!hwnd) return;
  AcquireSRWLockExclusive(&g_conformanceLeases.lock);
  g_conformanceLeases.leases.erase(hwnd);
  ReleaseSRWLockExclusive(&g_conformanceLeases.lock);
}

static void ClearAllConformanceLeases() {
  AcquireSRWLockExclusive(&g_conformanceLeases.lock);
  g_conformanceLeases.leases.clear();
  ReleaseSRWLockExclusive(&g_conformanceLeases.lock);
}

static size_t CountActiveConformanceLeases() {
  const ULONGLONG now = GetTickCount64();
  size_t count = 0;
  AcquireSRWLockShared(&g_conformanceLeases.lock);
  for (const auto& kv : g_conformanceLeases.leases) {
    if (now < kv.second.expiresAtTickMs) ++count;
  }
  ReleaseSRWLockShared(&g_conformanceLeases.lock);
  return count;
}

static void BeginConformanceLease(HWND hwnd, const RECT& expectedRect) {
  if (!hwnd || g_settings.conformanceLeaseMs == 0 ||
      expectedRect.right <= expectedRect.left ||
      expectedRect.bottom <= expectedRect.top) {
    CancelConformanceLease(hwnd);
    return;
  }

  ConformanceLease lease;
  lease.expectedRect = expectedRect;
  lease.expiresAtTickMs = GetTickCount64() + g_settings.conformanceLeaseMs;
  AcquireSRWLockExclusive(&g_conformanceLeases.lock);
  g_conformanceLeases.leases[hwnd] = lease;
  ReleaseSRWLockExclusive(&g_conformanceLeases.lock);
  ++Diagnostics::g_runtime.counters.conformanceLeasesStarted;
}

static bool RecordConformanceLeaseRepair(HWND hwnd, ConformanceLease* updated) {
  bool retained = false;
  AcquireSRWLockExclusive(&g_conformanceLeases.lock);
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      GetTickCount64() < it->second.expiresAtTickMs) {
    ++it->second.repairs;
    if (updated) *updated = it->second;
    retained = true;
  }
  ReleaseSRWLockExclusive(&g_conformanceLeases.lock);
  return retained;
}

