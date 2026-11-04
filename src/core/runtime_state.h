// Shared WM actor state, diagnostic recording and thread-affinity policy.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

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
  uint64_t rejectedResizeGestures = 0;

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
  uint64_t placementSuppressedByPhysicalState = 0;
  uint64_t placementAdjusted = 0;
  uint64_t placementAccessDenied = 0;
  uint64_t placementRefused = 0;
  uint64_t placementDead = 0;
  uint64_t floatingGeometryRepairs = 0;

  uint64_t constraintProbeAttempts = 0;
  uint64_t constraintProbeSuccesses = 0;
  uint64_t constraintProbeTimeouts = 0;
  uint64_t constraintProbeFailures = 0;
  uint64_t constraintUnsatisfiablePlans = 0;
  uint64_t constraintFloats = 0;

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
  uint64_t conformanceLeaseDeferredRepairs = 0;
  uint64_t conformanceLeaseExpiredCompliant = 0;
  uint64_t conformanceLeaseExpiredFloats = 0;
  uint64_t conformanceLeaseExpiredStale = 0;

  uint64_t tileCommands = 0;
  uint64_t layoutCycleCommands = 0;
  uint64_t floatFocusedCommands = 0;
  uint64_t swapMasterCommands = 0;
  uint64_t promoteWindowCommands = 0;
  uint64_t demoteWindowCommands = 0;
  uint64_t rejectedOrderCommands = 0;
  uint64_t rejectedLayoutChanges = 0;

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

// Serialized actor state and thread-affinity policy.

struct PendingWorkspaceArrange {
  GUID desktopId{};
  HMONITOR monitor = nullptr;
};

struct PendingWindowOverflow {
  HWND hwnd = nullptr;
  DWORD processId = 0;
  GUID sourceDesktop{};
  std::wstring sourceMonitorId;
  bool monitorFixed = false;
  uint64_t tilingGeneration = 0;
};

// A requested switch to the desktop a window was moved to, cancelled if the
// user acts first (input tick changes) or the deadline passes.
struct PendingDesktopFollow {
  HWND hwnd = nullptr;
  DWORD processId = 0;
  GUID sourceDesktop{};
  GUID targetDesktop{};
  RECT sourceFrame{};
  ULONGLONG deadlineTickMs = 0;
  DWORD inputTickMs = 0;
};

struct PendingDiscoveryRetry {
  HWND hwnd = nullptr;
  DWORD processId = 0;
  ULONGLONG dueTickMs = 0;
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

enum class ReconciledDesktopState {
  Unknown,
  Settled,
  TransitionPending,
};

// An initial-placement rule frozen for one window: where it should go and
// whether to follow it there. Values are copied when the rule first matches.
struct PendingWindowRoute {
  DWORD processId = 0;
  GUID sourceDesktop{};
  GUID targetDesktop{};
  RuleMonitor monitor = RuleMonitor::Any;
  std::wstring monitorId;
  bool followWindow = false;
  uint64_t visibleGeneration = 0;  // Counts a maximized arrival without admitting it.
};

// State of the WM thread (the "actor"): everything below is read and written
// only on that thread, except `threadId`, which callbacks read to post messages.
// Management mode affects admission/retention policy only; observation,
// reconciliation, resize learning, and arrangement run in both modes.
struct WmRuntime {
  HANDLE thread = nullptr;
  std::atomic<DWORD> threadId{0};
  HANDLE readyEvent = nullptr;
  bool comInitialized = false;
  ManagementMode managementMode = ManagementMode::Automatic;

  // Thread timers created with SetTimer(nullptr, ...). 0 means not scheduled.
  struct Timers {
    UINT_PTR lifecycle = 0;       // Settles a burst of window events (ReconcileDelayMs).
    UINT_PTR discoveryRetry = 0;  // Re-checks slow new windows (SlowApplicationDelayMs).
    UINT_PTR overflow = 0;        // Checks desktop arrival, without activating HWNDs.
    UINT_PTR maintenance = 0;     // Retries virtual-desktop COM / notification setup.
    UINT_PTR conformance = 0;     // Next conformance-lease repair or deadline.
  } timers;

  // Work collected from window events, processed by the settled lifecycle pass.
  struct Lifecycle {
    std::vector<HWND> dirtyWindows;
    std::vector<PendingWorkspaceArrange> pendingArranges;
    std::vector<PendingDiscoveryRetry> discoveryRetries;
    bool retryAfterPlatformRecovery = false;  // Last pass was deferred until COM recovers.
    bool forceMonitorReconcile = false;       // Display topology changed.
  } lifecycle;

  // The virtual desktop the model was last reconciled against.
  struct DesktopTracking {
    GUID reconciled{};
    ReconciledDesktopState state = ReconciledDesktopState::Unknown;
    bool pendingSwitchFlyouts = false;  // Show "desktop N" toasts after the next pass.
  } desktop;

  struct Maintenance {
    UINT attempts = 0;
    bool running = false;
  } maintenance;

  // Automatic overflow: windows that exceeded a limit and are waiting to move.
  struct Overflow {
    // Destroy events retire queued HWND lifetimes before deferred work runs.
    std::unordered_set<HWND> windows;
    std::vector<PendingWindowOverflow> queue;
    PendingDesktopFollow follow{};  // Switch to the desktop a window overflowed to.
  } overflow;

  // Initial-placement rules ("Open on monitor / desktop"). Independent of
  // management mode, so it also works in Manual mode.
  struct Routing {
    bool ready = false;                     // Hooks needed for routing are installed.
    std::unordered_set<HWND> evaluated;     // Rules already checked for these windows.
    std::unordered_map<HWND, PendingWindowRoute> pending;
    PendingDesktopFollow follow{};          // "Follow window" desktop switch.
  } routing;

  // True when `hwnd` has a queued overflow request that has not run yet.
  bool HasPendingOverflow(HWND hwnd) const {
    return std::any_of(overflow.queue.begin(), overflow.queue.end(),
                       [hwnd](const PendingWindowOverflow& request) { return request.hwnd == hwnd; });
  }

  // Drops any queued overflow request for `hwnd`. Does not touch desktop switches.
  void CancelPendingOverflow(HWND hwnd) {
    overflow.windows.erase(hwnd);
    std::erase_if(overflow.queue,
                  [hwnd](const PendingWindowOverflow& request) { return request.hwnd == hwnd; });
  }

  // Drops queued overflow plus any desktop switch that would follow `hwnd`.
  // Used when the window is destroyed or the user starts dragging it.
  void CancelPendingWindowMoves(HWND hwnd) {
    CancelPendingOverflow(hwnd);
    if (overflow.follow.hwnd == hwnd) overflow.follow = {};
    if (routing.follow.hwnd == hwnd) routing.follow = {};
  }
};
static SettingsState g_settings;
static VirtualDesktopRuntime g_vd;
static WmRuntime g_wm;

namespace RuntimeLifecycle {
static bool IsManualMode() {
  return g_wm.managementMode == ManagementMode::Manual;
}

static bool IsAutomaticMode() {
  return g_wm.managementMode == ManagementMode::Automatic;
}

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

} // namespace RuntimeLifecycle

namespace Platform::Monitors {
struct MonitorIdentityCacheEntry {
  HMONITOR monitor = nullptr;
  Model::MonitorId id;
};

// HMONITOR <-> stable monitor identity is valid for one display-topology
// generation. DPI and work-area observations deliberately aren't cached here.
static std::vector<MonitorIdentityCacheEntry> g_monitorIdentityCache;

} // namespace Platform::Monitors

// The one repository of workspace state. WM thread only.
static WorkspaceRepository g_workspaces;

namespace Gestures {
// Frame rectangles captured by the WinEvent callback at the start and end of a
// user move/resize, consumed later on the WM thread to classify the gesture.
struct MoveSizeTracker {
  std::unordered_map<HWND, RECT> startRects;
  std::unordered_map<HWND, RECT> endRects;
  std::unordered_map<HWND, POINT> endPoints;
};

static MoveSizeTracker g_moveSize;
}  // namespace Gestures

namespace Platform::WindowEvents {
// Installed SetWinEventHook handles. Null means that hook is not installed.
struct WinEventHooks {
  HWINEVENTHOOK moveSize = nullptr;
  HWINEVENTHOOK minimize = nullptr;
  HWINEVENTHOOK hideDestroy = nullptr;
  HWINEVENTHOOK cloak = nullptr;
  HWINEVENTHOOK state = nullptr;
  HWINEVENTHOOK foreground = nullptr;
  HWINEVENTHOOK locationChange = nullptr;
};

static WinEventHooks g_hooks;
}  // namespace Platform::WindowEvents

namespace Placement {

enum class UserLayoutMutationKind {
  Resize,
  Order,
  LayoutChange,
};

// A user change (resize, reorder, layout switch) that has been applied but not yet
// proven: if the windows refuse their new tiles, `previous` is restored.
struct PendingLayoutMutation {
  Workspace previous;
  Workspace candidate;
  HWND subject = nullptr;
  UserLayoutMutationKind kind = UserLayoutMutationKind::Resize;
  const wchar_t* action = L"Layout mutation";
};

static std::unordered_map<
    DesktopMonitorKey, PendingLayoutMutation,
    Model::DesktopMonitorKeyHash, Model::DesktopMonitorKeyEqual>
    g_pendingLayoutMutations;

// Admission recovery can move the pending member and reassign siblings before
// conformance has proved the resulting arrangement. Keep the pre-recovery order
// until that member settles so a suspend/restore race cannot strand established
// members in slots chosen only for the pending admission.
struct PendingAdmissionRecovery {
  DesktopMonitorKey key;
  Workspace baseline;
  uint64_t generation = 0;
};

static std::unordered_map<HWND, PendingAdmissionRecovery>
    g_pendingAdmissionRecoveries;

// A bounded period after placement during which a window is pushed back onto its
// tile if it drifts (see the glossary in CONTRIBUTING.md).
struct ConformanceLease {
  RECT expectedRect{};
  // Preserve the user action that produced this target through asynchronous
  // constraint discovery and its subsequent arrangement.
  HWND preferredTiledWindow = nullptr;
  ULONGLONG expiresAtTickMs = 0;
  // Diagnostic only. Attempt count never controls lease lifetime or policy.
  unsigned attempts = 0;
  // Reinforcement cadence. repairDueTickMs coalesces a burst, and also schedules
  // another checked attempt when the application remains nonconforming.
  ULONGLONG lastRepairTickMs = 0;
  ULONGLONG repairDueTickMs = 0;
  // Distinguishes an old copied lease from a later replacement/cancellation.
  uint64_t generation = 0;
};

struct ConformanceLeaseTracker {
  std::unordered_map<HWND, ConformanceLease> leases;
  uint64_t nextGeneration = 1;
};

static ConformanceLeaseTracker g_conformanceLeases;

enum class ConformanceLeaseReadResult {
  Missing,
  Active,
  Expired,
};

}  // namespace Placement
