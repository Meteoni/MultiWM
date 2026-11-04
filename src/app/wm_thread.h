// Hotkeys, WM dispatch and tool-thread lifecycle.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace RuntimeLifecycle {

static void RegisterConfiguredHotkeys() {
  auto registerOne = [](
      int id, UINT modifiers, UINT key, const wchar_t* name,
      bool allowRepeat = false) {
    if (!key) return;
    const UINT registrationModifiers =
        allowRepeat ? modifiers : modifiers | MOD_NOREPEAT;
    if (!RegisterHotKey(nullptr, id, registrationModifiers, key)) {
      Wh_Log(L"Failed to register %s hotkey (id=%d error=%lu)",
             name, id, GetLastError());
    }
  };

  registerOne(HK_TILE, g_settings.tilingModifiers, g_settings.tileKey, L"Tile");
  registerOne(HK_SWAP_MASTER, g_settings.tilingModifiers,
              g_settings.swapMasterKey, L"Swap master");
  registerOne(HK_PROMOTE_WINDOW, g_settings.tilingModifiers,
              g_settings.promoteWindowKey, L"Promote window", true);
  registerOne(HK_DEMOTE_WINDOW, g_settings.tilingModifiers,
              g_settings.demoteWindowKey, L"Demote window", true);
  registerOne(HK_MANAGEMENT_MODE_TOGGLE, g_settings.tilingModifiers,
              g_settings.managementModeToggleKey, L"Management mode");
  registerOne(HK_FLOAT_FOCUSED, g_settings.tilingModifiers,
              g_settings.floatFocusedKey, L"Float focused");
  registerOne(HK_LAYOUT, g_settings.tilingModifiers,
              g_settings.layoutKey, L"Layout cycle");
  registerOne(HK_DIAGNOSTIC_DUMP, g_settings.tilingModifiers,
              g_settings.diagnosticDumpKey, L"Diagnostic dump");
}

static void UnregisterConfiguredHotkeys() {
  UnregisterHotKey(nullptr, HK_TILE);
  UnregisterHotKey(nullptr, HK_SWAP_MASTER);
  UnregisterHotKey(nullptr, HK_PROMOTE_WINDOW);
  UnregisterHotKey(nullptr, HK_DEMOTE_WINDOW);
  UnregisterHotKey(nullptr, HK_MANAGEMENT_MODE_TOGGLE);
  UnregisterHotKey(nullptr, HK_FLOAT_FOCUSED);
  UnregisterHotKey(nullptr, HK_LAYOUT);
  UnregisterHotKey(nullptr, HK_DIAGNOSTIC_DUMP);
}

static void RememberManagedForeground(HWND hwnd) {
  AssertWmThread(L"RememberManagedForeground");
  if (!hwnd) return;

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  for (const auto& key : owners) {
    Workspace workspace;
    if (!g_workspaces.Load(key, &workspace)) continue;
    if (!workspace.RememberFocusedWindow(hwnd)) continue;
    g_workspaces.Save(key, workspace);
  }
}

static void InitializeCurrentDesktopWorkspaces() {
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);
  for (HMONITOR monitor : monitors) {
    Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
  }
  RememberManagedForeground(GetForegroundWindow());

  Wh_Log(
      L"Window management mode at startup: %s (%zu connected monitor%s)",
      IsManualMode() ? L"Manual" : L"Automatic", monitors.size(),
      monitors.size() == 1 ? L"" : L"s");
}

static void SetWindowManagementMode(ManagementMode mode) {
  if (g_wm.managementMode == mode) return;
  g_wm.managementMode = mode;
  const bool automatic = IsAutomaticMode();
  if (!automatic) {
    g_wm.pendingWindowOverflow.clear();
    g_wm.pendingOverflowSwitch = {};
    if (g_wm.overflowTimer) KillTimer(nullptr, g_wm.overflowTimer);
    g_wm.overflowTimer = 0;
  }
  Wh_Log(L"Window management mode: %s", automatic ? L"Automatic" : L"Manual");
  Diagnostics::RecordEvent(
      L"management mode changed to %ls", automatic ? L"Automatic" : L"Manual");
  TrayUi::ShowManagementModeFlyout(automatic);

  // Entering Automatic mode runs the normal discovery path for every monitor.
  // Known Floating records remain managed-but-excluded and are never re-adopted.
  if (automatic) {
    std::vector<HMONITOR> monitors;
    Reconcile::EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
    RememberManagedForeground(GetForegroundWindow());
    Reconcile::ScheduleLifecycleReconcile(nullptr);
  }
}

static void ToggleWindowManagementMode() {
  SetWindowManagementMode(
      IsAutomaticMode() ? ManagementMode::Manual : ManagementMode::Automatic);
}

static void HandleMoveSizeEndMessage(HWND hwnd) {
  ++Diagnostics::g_runtime.counters.moveSizeEndMessages;
  // MOVESIZEEND is an authoritative monitor-transition boundary. Repair monitor
  // ownership before interpreting final geometry, without querying desktop
  // ownership for unrelated lifecycle noise.
  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::Monitor, &changedKeys, hwnd);
  Commands::ApplyUserMoveSize(hwnd);
  for (const auto& key : changedKeys) Placement::ArrangeWorkspace(key);
  Reconcile::ScheduleLifecycleReconcile(hwnd);
}

// True for messages that carry an explicit user command (hotkeys, tray menu).
// Writing a diagnostic report is not a command: it must not change WM behavior.
static bool IsUserCommandMessage(const MSG& msg) {
  switch (msg.message) {
    case WM_HOTKEY:
      return msg.wParam != HK_DIAGNOSTIC_DUMP;
    case WM_APP_LAYOUT_CYCLE:
    case WM_APP_LAYOUT_SET:
    case WM_APP_TILE_WORKSPACE:
    case WM_APP_MANAGEMENT_MODE_SET:
      return true;
    default:
      return false;
  }
}

// Runs the handler for one of the WM actor's own timers. Returns false when
// `timerId` is not one of ours (for example a COM or tray window timer).
static bool DispatchActorTimer(UINT_PTR timerId) {
  const struct {
    UINT_PTR id;
    void (*handler)();
  } actorTimers[] = {
      {g_wm.lifecycleTimer, Reconcile::ReconcileDeferredLifecycle},
      {g_wm.discoveryRetryTimer, Reconcile::ProcessDiscoveryRetryTimer},
      {g_wm.overflowTimer, Reconcile::ProcessWindowOverflow},
      {g_wm.maintenanceTimer, RunMaintenanceNow},
      {g_wm.conformanceTimer, Placement::ProcessConformanceTimer},
  };
  for (const auto& timer : actorTimers) {
    if (timer.id && timer.id == timerId) {
      timer.handler();
      return true;
    }
  }
  return false;
}

// Converts one serialized thread message into a domain command. Messages not
// owned by the WM are explicitly returned to Windows so STA COM's hidden window
// continues receiving cross-apartment calls.
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg) {
  // A new user command supersedes a pending "follow the overflowed window to its
  // new desktop" switch; never yank the user away after they have acted.
  if (IsUserCommandMessage(msg)) g_wm.pendingOverflowSwitch = {};
  switch (msg.message) {
    case WM_APP_MOVE_SIZE_END:
      HandleMoveSizeEndMessage(reinterpret_cast<HWND>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_WINDOW_EVENT:
      Reconcile::ProcessWindowLifecycleEvent(
          static_cast<DWORD>(msg.wParam), reinterpret_cast<HWND>(msg.lParam));
      return WmMessageDisposition::Handled;

    case WM_APP_VIRTUAL_DESKTOP_CHANGED:
      Reconcile::HandleVirtualDesktopChanged();
      return WmMessageDisposition::Handled;

    case WM_APP_FOREGROUND_CHANGED: {
      HWND hwnd = reinterpret_cast<HWND>(msg.wParam);
      RememberManagedForeground(hwnd);
      // Some frameworks make a top-level HWND foreground without delivering a
      // usable SHOW event. Foreground is only a discovery prompt; the settled
      // snapshot remains authoritative for eligibility and admission.
      if (IsAutomaticMode()) {
        if (!hwnd || !IsWindow(hwnd)) {
          // A rapidly replaced foreground HWND can disappear before this posted
          // message is consumed. Preserve the discovery prompt with one settled
          // global snapshot, which can observe its live replacement.
          Reconcile::ScheduleLifecycleReconcile(nullptr);
        } else if (!Platform::Windows::IsWindowTrackedInAnyState(hwnd) ||
                   g_wm.HasPendingOverflow(hwnd)) {
          Reconcile::ScheduleLifecycleReconcile(hwnd);
        }
      }
      ++Diagnostics::g_runtime.counters.trayRefreshMessages;
      TrayUi::RefreshForCurrentWorkspace();
      return WmMessageDisposition::Handled;
    }

    case WM_APP_LAYOUT_CYCLE:
      Commands::CycleCurrentWorkspaceLayout(
          reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_TILE_WORKSPACE:
      Commands::TileWindows(reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_LAYOUT_SET:
      Commands::SetCurrentWorkspaceLayout(
          static_cast<TileLayout>(msg.lParam),
          reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_MANAGEMENT_MODE_SET:
      if (msg.wParam == static_cast<WPARAM>(ManagementMode::Manual) ||
          msg.wParam == static_cast<WPARAM>(ManagementMode::Automatic)) {
        SetWindowManagementMode(static_cast<ManagementMode>(msg.wParam));
      }
      return WmMessageDisposition::Handled;

    case WM_APP_RECONCILE_NOW:
      Reconcile::ReconcileDeferredLifecycle();
      return WmMessageDisposition::Handled;

    case WM_TIMER:
      // Actor timers are created with SetTimer(nullptr, ...). A window timer can
      // use the same numeric ID and must still reach its WndProc.
      if (msg.hwnd) return WmMessageDisposition::DispatchToWindows;
      return DispatchActorTimer(static_cast<UINT_PTR>(msg.wParam))
          ? WmMessageDisposition::Handled
          : WmMessageDisposition::DispatchToWindows;

    case WM_HOTKEY:
      switch (static_cast<UINT>(msg.wParam)) {
        case HK_TILE:
          Commands::TileWindows();
          break;
        case HK_LAYOUT:
          Commands::CycleCurrentWorkspaceLayout();
          break;
        case HK_SWAP_MASTER:
          Commands::SwapMaster();
          break;
        case HK_PROMOTE_WINDOW:
          Commands::PromoteFocusedWindow();
          break;
        case HK_DEMOTE_WINDOW:
          Commands::DemoteFocusedWindow();
          break;
        case HK_FLOAT_FOCUSED:
          Commands::FloatFocusedWindow();
          break;
        case HK_DIAGNOSTIC_DUMP:
          Diagnostics::WriteDiagnosticReport();
          break;
        case HK_MANAGEMENT_MODE_TOGGLE:
          ToggleWindowManagementMode();
          break;
      }
      return WmMessageDisposition::Handled;
  }

  return WmMessageDisposition::DispatchToWindows;
}

static void RunWmMessageLoop() {
  // Standard Windhawk tool-mod worker pattern: one ordinary Windows message loop
  // owns thread messages, timers, the tray windows, and STA COM dispatch.
  MSG msg{};
  for (;;) {
    const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
    if (result == 0) return;  // WM_QUIT
    if (result == -1) {
      Wh_Log(L"WM worker GetMessage failed: %lu", GetLastError());
      return;
    }

    switch (HandleWmThreadMessage(msg)) {
      case WmMessageDisposition::Handled:
        break;

      case WmMessageDisposition::DispatchToWindows:
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        break;
    }
  }
}

static void CleanupWmThread() {
  // Deferred overflow is temporarily Floating while native routing waits. Return
  // those members to tiling before discarding the actor queue so settings reload
  // can enforce the new policy instead of retaining an accidental float forever.
  std::vector<PendingWindowOverflow> deferredOverflow;
  deferredOverflow.swap(g_wm.pendingWindowOverflow);
  for (const PendingWindowOverflow& pending : deferredOverflow) {
    if (!Platform::Windows::IsSameLiveWindow(pending.hwnd, pending.processId) ||
        !g_wm.overflowWindows.count(pending.hwnd)) continue;
    for (const auto& key : g_workspaces.OwnersOf(pending.hwnd)) {
      Workspace workspace;
      if (!g_workspaces.Load(key, &workspace) || workspace.Layout() == TileLayout::Floating) continue;
      const WindowRecord* record = workspace.Find(pending.hwnd);
      if (!record || record->pid != pending.processId || record->state != ManageState::Floating ||
          record->alwaysFloating || record->tilingGeneration != pending.tilingGeneration) continue;
      workspace.ActivateTiled(pending.hwnd);
      const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(pending.hwnd);
      if (reason != SuspensionReason::None) workspace.Suspend(pending.hwnd, reason);
      g_workspaces.Save(key, workspace);
    }
  }
  // Unhook WinEvent delivery before clearing transient callback observations so
  // no reentrant delivery can repopulate the next settings-reload session.
  UnregisterConfiguredHotkeys();
  TrayUi::Shutdown();
  Platform::WindowEvents::RemoveWinEventHooks();
  Platform::VirtualDesktop::CleanupVirtualDesktopAPI();

  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }
  if (g_wm.discoveryRetryTimer) {
    KillTimer(nullptr, g_wm.discoveryRetryTimer);
    g_wm.discoveryRetryTimer = 0;
  }
  if (g_wm.maintenanceTimer) {
    KillTimer(nullptr, g_wm.maintenanceTimer);
    g_wm.maintenanceTimer = 0;
  }
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }
  g_wm.lifecycleDirtyWindows.clear();
  g_wm.pendingDiscoveryRetries.clear();
  g_wm.pendingDesktopArranges.clear();
  if (g_wm.overflowTimer) KillTimer(nullptr, g_wm.overflowTimer);
  g_wm.overflowTimer = 0;
  g_wm.overflowWindows.clear();
  g_wm.pendingWindowOverflow.clear();
  g_wm.pendingOverflowSwitch = {};
  g_wm.windowRoutingReady = false;
  g_wm.windowRoutingEvaluated.clear();
  g_wm.pendingWindowRoutes.clear();
  g_wm.pendingInitialPlacementSwitch = {};
  g_wm.maintenanceAttempts = 0;
  g_wm.maintenanceRunning = false;
  g_wm.lifecycleRetryAfterPlatformRecovery = false;
  g_wm.forceMonitorReconcile = false;
  g_wm.reconciledDesktop = {};
  g_wm.reconciledDesktopState = ReconciledDesktopState::Unknown;
  g_wm.pendingDesktopSwitchFlyouts = false;
  Placement::ClearAllConformanceLeases();
  Gestures::ClearAllMoveSizeSamples();
  Platform::Monitors::ClearMonitorIdentityCache();
  InterlockedExchange(&g_vd.changeQueued, 0);
}

// Owns the STA apartment and delegates startup, serialized dispatch, and cleanup
// to narrow helpers. All workspace mutations reachable from this thread remain
// serialized; foreign STA messages are dispatched back to Windows.
DWORD WINAPI HotkeyThreadProc(LPVOID) {
  SetThreadDpiAwarenessContext(
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  const DWORD threadId = GetCurrentThreadId();
  g_wm.threadId.store(threadId, std::memory_order_release);
  Diagnostics::BeginWmSession();
  Wh_Log(L"Hotkey thread started, thread ID: %lu", threadId);

  // Readiness means the worker exists and owns a Windows message queue. Signal it
  // before any shell/COM acquisition so the controller can always request WM_QUIT
  // without waiting on Explorer readiness.
  MSG msg{};
  PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);
  if (g_wm.readyEvent) SetEvent(g_wm.readyEvent);

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_wm.comInitialized = SUCCEEDED(coHr);
  Wh_Log(L"CoInitializeEx result: 0x%08X", coHr);

  if (g_wm.comInitialized) {
    RunMaintenanceNow();
  } else {
    Platform::WindowEvents::InstallWinEventHooks();
    Wh_Log(L"COM initialization failed; virtual desktop integration unavailable for this WM session");
  }

  // Existing HWNDs, including hidden windows and windows on other desktops,
  // form a baseline before discovery or hook-driven placement can route them.
  if (std::any_of(g_settings.windowRules.begin(), g_settings.windowRules.end(),
                  [](const WindowRule& rule) {
                    return rule.treatment == WindowRuleTreatment::InitialPlacement;
                  })) {
    ++Diagnostics::g_runtime.counters.enumWindowsPasses;
    g_wm.windowRoutingReady = Platform::WindowEvents::g_hooks.hideDestroy && EnumWindows(
        [](HWND hwnd, LPARAM) WINAPI -> BOOL {
          ++Diagnostics::g_runtime.counters.enumWindowsVisited;
          g_wm.windowRoutingEvaluated.insert(hwnd);
          return TRUE;
        }, 0) != FALSE;
    if (!g_wm.windowRoutingReady) Wh_Log(L"Initial placement baseline failed");
  }

  Reconcile::ScheduleLifecycleReconcile(nullptr);
  RegisterConfiguredHotkeys();
  InitializeCurrentDesktopWorkspaces();

  if (!TrayUi::Initialize()) {
    Wh_Log(L"Tiling layout tray icon unavailable; window management remains active");
  }

  RunWmMessageLoop();

  // Stop callbacks from queueing new work as soon as the actor stops consuming
  // messages. Cleanup then unhooks delivery before clearing callback observations.
  g_wm.threadId.store(0, std::memory_order_release);
  CleanupWmThread();

  if (g_wm.comInitialized) CoUninitialize();
  g_wm.comInitialized = false;
  return 0;
}

bool StartHotkeyThread() {
  if (g_wm.thread) {
    Wh_Log(L"Refusing to start a second WM actor");
    return false;
  }

  g_wm.readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!g_wm.readyEvent) {
    Wh_Log(L"Failed to create WM readiness event: %lu", GetLastError());
    return false;
  }

  g_wm.thread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);
  if (!g_wm.thread) {
    Wh_Log(L"Failed to create WM thread: %lu", GetLastError());
    CloseHandle(g_wm.readyEvent);
    g_wm.readyEvent = nullptr;
    return false;
  }

  HANDLE waits[] = {g_wm.readyEvent, g_wm.thread};
  DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE);
  const bool ready = waitResult == WAIT_OBJECT_0;
  if (!ready) {
    Wh_Log(L"WM thread exited before signaling readiness (wait=%lu)", waitResult);
  }

  CloseHandle(g_wm.readyEvent);
  g_wm.readyEvent = nullptr;

  if (!ready) {
    WaitForSingleObject(g_wm.thread, INFINITE);
    CloseHandle(g_wm.thread);
    g_wm.thread = nullptr;
    g_wm.threadId.store(0, std::memory_order_release);
    return false;
  }
  return true;
}

bool StopHotkeyThread() {
  if (!g_wm.thread) return true;

  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (threadId && !PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
    Wh_Log(L"Failed to post WM_QUIT to WM thread: %lu", GetLastError());
  }

  const DWORD waitResult = WaitForSingleObject(g_wm.thread, 5000);
  if (waitResult != WAIT_OBJECT_0) {
    // During settings reload, retaining these handles prevents a second WM owner.
    // During WhTool_ModUninit, the standard tool-mod wrapper exits the dedicated
    // process immediately after the callback returns.
    Wh_Log(L"WM thread did not stop cleanly (wait=%lu); leaving the existing actor intact", waitResult);
    return false;
  }

  CloseHandle(g_wm.thread);
  g_wm.thread = nullptr;
  g_wm.threadId.store(0, std::memory_order_release);
  if (g_wm.readyEvent) {
    CloseHandle(g_wm.readyEvent);
    g_wm.readyEvent = nullptr;
  }
  return true;
}

} // namespace RuntimeLifecycle
