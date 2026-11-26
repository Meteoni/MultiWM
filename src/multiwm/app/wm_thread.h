// Hotkeys, WM dispatch and tool-thread lifecycle.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

#include <mutex>
#include <utility>

namespace RuntimeLifecycle {

// Registers every hotkey with a key set, in kHotkeyBindings order. Returns the
// names of those Windows refused: another program, or an earlier MultiWM
// hotkey, already uses the key combination.
static std::vector<const wchar_t*> RegisterConfiguredHotkeys() {
  std::vector<const wchar_t*> unavailable;
  for (const HotkeyBinding& hotkey : kHotkeyBindings) {
    const UINT key = g_settings.*hotkey.key;
    if (!key) continue;
    const UINT modifiers = hotkey.allowRepeat ? g_settings.tilingModifiers
                                              : g_settings.tilingModifiers | MOD_NOREPEAT;
    if (!RegisterHotKey(nullptr, hotkey.id, modifiers, key)) {
      Wh_Log(L"Failed to register %s hotkey (id=%d error=%lu)",
             hotkey.name, static_cast<int>(hotkey.id), GetLastError());
      unavailable.push_back(hotkey.name);
    }
  }
  return unavailable;
}

// The toast text for what didn't take effect: hotkeys another program already
// uses, and settings the load couldn't use (`problems`: the first, and how many
// more are in the log). Empty when everything did.
static std::wstring StartupProblemsText(const std::vector<const wchar_t*>& unavailableHotkeys,
                                        const std::vector<std::wstring>& problems) {
  std::wstring text;
  if (!unavailableHotkeys.empty()) {
    text = unavailableHotkeys.size() == 1 ? L"Hotkey in use: " : L"Hotkeys in use: ";
    for (size_t i = 0; i < unavailableHotkeys.size(); ++i) {
      if (i) text += L", ";
      text += unavailableHotkeys[i];
    }
  }
  if (!problems.empty()) {
    if (!text.empty()) text += L"; ";
    text += problems.front();
    if (problems.size() > 1) text += L" (+" + std::to_wstring(problems.size() - 1) + L" more in the log)";
  }
  return text;
}

// One toast, at startup and after every settings change, for what didn't take
// effect (StartupProblemsText). One, because a second would replace the first.
// Needs the tray UI.
static void ReportStartupProblems(const std::vector<const wchar_t*>& unavailableHotkeys) {
  const std::wstring text = StartupProblemsText(unavailableHotkeys, g_settings.problems);
  if (!text.empty()) TrayUi::ShowCommandFailureToast(text.c_str());
}

static void UnregisterConfiguredHotkeys() {
  for (const HotkeyBinding& hotkey : kHotkeyBindings) UnregisterHotKey(nullptr, hotkey.id);
}

static void RememberManagedForeground(HWND hwnd) {
  AssertWmThread(L"RememberManagedForeground");
  if (!hwnd) return;
  if (const HWND hung = Platform::Windows::HungWindowOfGhost(hwnd)) hwnd = hung;

  g_workspaces.UpdateEach(g_workspaces.OwnersOf(hwnd), [&](const DesktopMonitorKey&, Workspace& workspace) {
    return workspace.RememberFocusedWindow(hwnd);
  });
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
    g_wm.overflow.queue.clear();
    g_wm.overflow.follow = {};
    CancelActorTimer(g_wm.timers.overflow);
  }
  Wh_Log(L"Window management mode: %s", automatic ? L"Automatic" : L"Manual");
  Diagnostics::RecordEvent(
      L"management mode changed to %ls", automatic ? L"Automatic" : L"Manual");
  TrayUi::ShowManagementModeToast(automatic);

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
  DragPreview::End(hwnd);
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
      {g_wm.timers.lifecycle, Reconcile::ReconcileDeferredLifecycle},
      {g_wm.timers.discoveryRetry, Reconcile::ProcessDiscoveryRetryTimer},
      {g_wm.timers.overflow, Reconcile::ProcessWindowOverflow},
      {g_wm.timers.maintenance, RunMaintenanceNow},
      {g_wm.timers.conformance, Placement::ProcessConformanceTimer},
      {g_wm.timers.constraintRetry, Placement::ProcessConstraintRetryTimer},
  };
  for (const auto& timer : actorTimers) {
    if (timer.id && timer.id == timerId) {
      timer.handler();
      return true;
    }
  }
  return false;
}

// True for a management-mode change (hotkey or tray). Its own toast names the
// new mode; what Automatic admission then does stays quiet, as it always does.
static bool IsManagementModeMessage(const MSG& msg) {
  return msg.message == WM_APP_MANAGEMENT_MODE_SET ||
         (msg.message == WM_HOTKEY && msg.wParam == HK_MANAGEMENT_MODE_TOGGLE);
}

// Converts one serialized thread message into a domain command. Messages not
// owned by the WM are explicitly returned to Windows so STA COM's hidden window
// continues receiving cross-apartment calls.
static WmMessageDisposition DispatchWmThreadMessage(const MSG& msg) {
  switch (msg.message) {
    case WM_APP_MOVE_SIZE_START:
      DragPreview::Begin(reinterpret_cast<HWND>(msg.wParam), reinterpret_cast<HWND>(msg.lParam));
      return WmMessageDisposition::Handled;

    case WM_APP_MOVE_SIZE_END:
      HandleMoveSizeEndMessage(reinterpret_cast<HWND>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_WINDOW_EVENT: {
      const auto event = static_cast<DWORD>(msg.wParam);
      const HWND hwnd = reinterpret_cast<HWND>(msg.lParam);
      Reconcile::ProcessWindowLifecycleEvent(event, hwnd);
      DragPreview::OnWindowEvent(event, hwnd);
      return WmMessageDisposition::Handled;
    }

    case WM_APP_VIRTUAL_DESKTOP_CHANGED:
      DragPreview::End();
      Reconcile::HandleVirtualDesktopChanged();
      Overlay::PruneDesktopIcons();
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
        case HK_LAYOUT_ACTION:
          Commands::ApplyLayoutAction();
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
        case HK_GROW_WINDOW:
          Commands::StepFocusedWindowSize(true);
          break;
        case HK_SHRINK_WINDOW:
          Commands::StepFocusedWindowSize(false);
          break;
        case HK_RESET_WINDOW_SIZE:
          Commands::ResetFocusedWindowSize();
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

// Handles one thread message. A user command first cancels a pending desktop
// follow, and afterwards says why any window it tried to tile was floated
// instead.
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg) {
  if (!IsUserCommandMessage(msg)) return DispatchWmThreadMessage(msg);
  // A new user command supersedes a pending "follow the overflowed window to its
  // new desktop" switch; never yank the user away after they have acted.
  g_wm.overflow.follow = {};
  if (IsManagementModeMessage(msg)) return DispatchWmThreadMessage(msg);
  Commands::CommandReleaseScope releases;
  const WmMessageDisposition disposition = DispatchWmThreadMessage(msg);
  Placement::ShowSpaceFloatToast(releases.Released(), nullptr);
  return disposition;
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
    // Between messages no WM operation is in progress, so the model is coherent.
    CompanionSnapshot::PublishIfChanged();
  }
}

static void CleanupWmThread() {
  // Stop companion hints first: from here on the model no longer follows Windows.
  CompanionSnapshot::PublishStopped();

  // Deferred overflow is temporarily Floating while native routing waits. Return
  // those members to tiling before discarding the actor queue so settings reload
  // can enforce the new policy instead of retaining an accidental float forever.
  std::vector<PendingWindowOverflow> deferredOverflow;
  deferredOverflow.swap(g_wm.overflow.queue);
  for (const PendingWindowOverflow& pending : deferredOverflow) {
    if (!Platform::Windows::IsSameLiveWindow(pending.hwnd, pending.processId) ||
        !g_wm.overflow.windows.count(pending.hwnd)) continue;
    g_workspaces.UpdateEach(g_workspaces.OwnersOf(pending.hwnd), [&](const DesktopMonitorKey&, Workspace& workspace) {
      if (workspace.Layout() == TileLayout::Floating) return false;
      const WindowRecord* record = workspace.Find(pending.hwnd);
      if (!record || record->pid != pending.processId || record->state != ManageState::Floating ||
          record->alwaysFloating || record->tilingGeneration != pending.tilingGeneration) {
        return false;
      }
      workspace.ActivateTiled(pending.hwnd);
      const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(pending.hwnd);
      if (reason != SuspensionReason::None) workspace.Suspend(pending.hwnd, reason);
      return true;
    });
  }
  // Unhook WinEvent delivery before clearing transient callback observations so
  // no reentrant delivery can repopulate the next settings-reload session.
  UnregisterConfiguredHotkeys();
  DragPreview::Shutdown();
  TrayUi::Shutdown();
  Overlay::Shutdown();  // After both of its clients.
  Platform::WindowEvents::RemoveWinEventHooks();
  Platform::VirtualDesktop::CleanupVirtualDesktopAPI();

  g_wm.ResetSession();
  Placement::DiscardAllPendingProofs();
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
    g_wm.routing.ready = Platform::WindowEvents::g_hooks.hideDestroy && EnumWindows(
        [](HWND hwnd, LPARAM) WINAPI -> BOOL {
          ++Diagnostics::g_runtime.counters.enumWindowsVisited;
          g_wm.routing.evaluated.insert(hwnd);
          return TRUE;
        }, 0) != FALSE;
    if (!g_wm.routing.ready) Wh_Log(L"Initial placement baseline failed");
  }

  Reconcile::ScheduleLifecycleReconcile(nullptr);
  const std::vector<const wchar_t*> unavailableHotkeys = RegisterConfiguredHotkeys();
  InitializeCurrentDesktopWorkspaces();

  if (!TrayUi::Initialize()) {
    Wh_Log(L"Tiling layout tray icon unavailable; window management remains active");
  }
  if (g_settings.DragPreviewEnabled() || TrayUi::g_preferences.showToasts) Overlay::WarmPrimaryMonitor();
  ReportStartupProblems(unavailableHotkeys);

  CompanionSnapshot::PublishIfChanged();
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

// How long StopHotkeyThread waits for the worker. Tests shorten it.
static DWORD g_stopTimeoutMs = 5000;

bool StopHotkeyThread() {
  if (!g_wm.thread) return true;

  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (threadId && !PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
    Wh_Log(L"Failed to post WM_QUIT to WM thread: %lu", GetLastError());
  }

  const DWORD waitResult = WaitForSingleObject(g_wm.thread, g_stopTimeoutMs);
  if (waitResult != WAIT_OBJECT_0) {
    // The queued WM_QUIT can't be withdrawn: the worker cleans up and exits once
    // it is free. Retaining these handles prevents a second WM owner meanwhile;
    // a settings reload finishes when it exits (DeferUntilWorkerExits). During
    // WhTool_ModUninit, the standard tool-mod wrapper exits the dedicated
    // process immediately after the callback returns.
    Wh_Log(L"WM thread did not stop within %lu ms (wait=%lu); it exits once it is free",
           g_stopTimeoutMs, waitResult);
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

// Who may stop or start the worker. Init, uninit, settings changes and a
// deferred restart each hold `mutex` while they do, so there is never a second
// worker. `deferredWait` is set while a restart waits for a worker that didn't
// stop in time; `deferredFinish` is what it then runs.
struct WorkerLifecycle {
  std::mutex mutex;
  HANDLE deferredWait = nullptr;
  void (*deferredFinish)() = nullptr;
};
static WorkerLifecycle g_lifecycle;

// Thread pool, once the old worker has exited: joins it and runs the deferred
// finish. Does nothing if CancelDeferredRestart got there first.
static void CALLBACK RunDeferredRestart(PVOID, BOOLEAN) {
  const std::scoped_lock lock(g_lifecycle.mutex);
  const HANDLE wait = std::exchange(g_lifecycle.deferredWait, nullptr);
  if (!wait) return;
  UnregisterWaitEx(wait, nullptr);  // Non-blocking, as a callback must.
  const auto finish = std::exchange(g_lifecycle.deferredFinish, nullptr);
  if (!StopHotkeyThread()) {
    Wh_Log(L"Deferred restart: the exited WM thread could not be joined");
    return;
  }
  Wh_Log(L"Deferred restart: the previous WM worker has exited");
  if (finish) finish();
}

// After StopHotkeyThread timed out: runs `finish` (on a thread-pool thread,
// holding g_lifecycle.mutex) once the old worker exits. The caller holds
// g_lifecycle.mutex. False, deferring nothing, if the wait can't be registered.
static bool DeferUntilWorkerExits(void (*finish)()) {
  if (!g_wm.thread || g_lifecycle.deferredWait) return false;
  g_lifecycle.deferredFinish = finish;
  if (!RegisterWaitForSingleObject(&g_lifecycle.deferredWait, g_wm.thread, RunDeferredRestart, nullptr, INFINITE,
                                   WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
    Wh_Log(L"Could not wait for the WM worker to exit: %lu", GetLastError());
    g_lifecycle.deferredWait = nullptr;
    g_lifecycle.deferredFinish = nullptr;
    return false;
  }
  return true;
}

// Drops a pending deferred restart and waits out one already running. The
// caller must not hold g_lifecycle.mutex.
static void CancelDeferredRestart() {
  HANDLE wait = nullptr;
  {
    const std::scoped_lock lock(g_lifecycle.mutex);
    wait = std::exchange(g_lifecycle.deferredWait, nullptr);
    g_lifecycle.deferredFinish = nullptr;
  }
  if (wait) UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
}

} // namespace RuntimeLifecycle
