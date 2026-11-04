//
// Keep recovery centralized. Callers may only request maintenance; this WM-STA
// owner is the only place that retries WinEvent hooks, the private VD core, and
// the native VD notification subscription. This mirrors the simple Windhawk
// tool-mod worker/message-loop pattern while avoiding one-shot startup failures.
namespace RuntimeLifecycle {

static constexpr UINT kMaintenanceRetryMs = 1000;
static constexpr UINT kMaintenanceMaxAttempts = 30;

static void CancelMaintenanceTimer() {
  if (!g_wm.timers.maintenance) return;
  KillTimer(nullptr, g_wm.timers.maintenance);
  g_wm.timers.maintenance = 0;
}

static void ArmMaintenanceTimer() {
  if (g_wm.timers.maintenance ||
      g_wm.maintenance.attempts >= kMaintenanceMaxAttempts) {
    return;
  }

  g_wm.timers.maintenance =
      SetTimer(nullptr, 0, kMaintenanceRetryMs, nullptr);
  if (!g_wm.timers.maintenance) {
    Wh_Log(L"Failed to schedule runtime maintenance retry");
  }
}

void RequestMaintenance(bool resetAttempts) {
  AssertWmThread(L"RuntimeLifecycle::RequestMaintenance");

  // Retry in bounded bursts rather than polling forever. After one burst is
  // exhausted, a later *external* demand (user action/lifecycle observation)
  // starts a fresh burst automatically; calls made from inside the active
  // maintenance attempt never reset their own budget.
  const bool exhaustedAndDemandedAgain =
      !g_wm.maintenance.running && !g_wm.timers.maintenance &&
      g_wm.maintenance.attempts >= kMaintenanceMaxAttempts;
  if (resetAttempts || exhaustedAndDemandedAgain) {
    g_wm.maintenance.attempts = 0;
  }
  g_wm.lifecycle.retryAfterPlatformRecovery = true;
  if (!g_wm.maintenance.running) ArmMaintenanceTimer();
}

void RunMaintenanceNow() {
  AssertWmThread(L"RuntimeLifecycle::RunMaintenanceNow");
  CancelMaintenanceTimer();
  g_wm.maintenance.running = true;
  ++Diagnostics::g_runtime.counters.runtimeMaintenanceRuns;

  const bool hooksReady = Platform::WindowEvents::InstallWinEventHooks();
  const bool vdCoreReady =
      g_wm.comInitialized && Platform::VirtualDesktop::InitializeVirtualDesktopAPI();
  const bool notificationsReady =
      vdCoreReady && Platform::VirtualDesktop::RegisterVirtualDesktopNotifications();

  if (hooksReady && vdCoreReady && notificationsReady) {
    const bool recoveredDeferredWork =
        g_wm.lifecycle.retryAfterPlatformRecovery;
    g_wm.maintenance.attempts = 0;
    g_wm.lifecycle.retryAfterPlatformRecovery = false;

    if (recoveredDeferredWork) {
      Diagnostics::RecordEvent(L"runtime platform maintenance recovered");
      // Startup/settings-reload may have skipped the initial current-workspace
      // arrangement while Explorer was unavailable. The recovery pass must do
      // more than rediscover membership: explicitly reinforce known current workspaces.
      g_wm.lifecycle.forceMonitorReconcile = true;
      Reconcile::ScheduleLifecycleReconcile(nullptr);
    }
    g_wm.maintenance.running = false;
    return;
  }

  g_wm.lifecycle.retryAfterPlatformRecovery = true;
  ++g_wm.maintenance.attempts;
  if (g_wm.maintenance.attempts < kMaintenanceMaxAttempts) {
    ++Diagnostics::g_runtime.counters.runtimeMaintenanceRetries;
    ArmMaintenanceTimer();
  } else {
    CancelMaintenanceTimer();
    ++Diagnostics::g_runtime.counters.runtimeMaintenanceExhausted;
    Diagnostics::RecordEvent(
        L"runtime maintenance retry burst exhausted attempts=%u hooks=%d vdCore=%d notifications=%d",
        g_wm.maintenance.attempts, hooksReady ? 1 : 0, vdCoreReady ? 1 : 0,
        notificationsReady ? 1 : 0);
    Wh_Log(
        L"Runtime maintenance retry burst exhausted (hooks=%d vdCore=%d notifications=%d)",
        hooksReady ? 1 : 0, vdCoreReady ? 1 : 0,
        notificationsReady ? 1 : 0);
  }
  g_wm.maintenance.running = false;
}

void NotifyShellRestarted() {
  AssertWmThread(L"RuntimeLifecycle::NotifyShellRestarted");
  ++Diagnostics::g_runtime.counters.shellRestartSignals;

  // TaskbarCreated is a shell-lifecycle signal, but Explorer can occasionally
  // recreate taskbar UI inside the same process. Only abandon private COM proxies
  // when the shell process identity actually changed (or disappeared). This avoids
  // duplicating a still-live notification subscription in the same Explorer.
  HWND shell = GetShellWindow();
  DWORD currentShellPid = 0;
  if (shell && IsWindow(shell)) GetWindowThreadProcessId(shell, &currentShellPid);
  const bool shellReplaced =
      !currentShellPid || !g_vd.shellPid || currentShellPid != g_vd.shellPid;

  Diagnostics::RecordEvent(
      L"Explorer shell lifecycle signal oldPid=%lu currentPid=%lu replacement=%d",
      g_vd.shellPid, currentShellPid, shellReplaced ? 1 : 0);

  if (shellReplaced) {
    Platform::VirtualDesktop::AbandonVirtualDesktopAPIForShellRestart();
    g_wm.desktop.reconciled = {};
    g_wm.desktop.state = ReconciledDesktopState::Unknown;
  }

  // A shell replacement can move/recreate windows asynchronously. Never let a
  // pre-restart conformance rectangle fight the replacement shell.
  Placement::ClearAllConformanceLeases();
  Placement::ScheduleNextConformanceTimer();
  g_wm.lifecycle.forceMonitorReconcile = true;
  const bool platformNeedsRecovery =
      !Platform::WindowEvents::AllWinEventHooksInstalled() ||
      !g_vd.initialized ||
      (g_vd.abi.notification.methodCount > 0 && !g_vd.notificationsRegistered);
  if (shellReplaced || platformNeedsRecovery) RequestMaintenance(true);
  Reconcile::ScheduleLifecycleReconcile(nullptr);
}

void NotifyDisplayTopologyChanged() {
  AssertWmThread(L"RuntimeLifecycle::NotifyDisplayTopologyChanged");
  ++Diagnostics::g_runtime.counters.displayTopologySignals;
  Diagnostics::RecordEvent(L"display/work-area topology changed");
  Platform::Monitors::ClearMonitorIdentityCache();

  // Every existing lease was computed against the old work-area/monitor topology.
  // Cancel it before Windows' own topology-induced LOCATIONCHANGE burst arrives.
  Placement::ClearAllConformanceLeases();
  Placement::ScheduleNextConformanceTimer();
  g_wm.lifecycle.pendingArranges.erase(
      std::remove_if(
          g_wm.lifecycle.pendingArranges.begin(), g_wm.lifecycle.pendingArranges.end(),
          [](const PendingWorkspaceArrange& pending) {
            return !Platform::Monitors::IsLiveMonitorHandle(pending.monitor);
          }),
      g_wm.lifecycle.pendingArranges.end());
  g_wm.lifecycle.forceMonitorReconcile = true;

  // Display/work-area changes normally require only model/geometry reconciliation.
  // Do not start a COM retry burst unless platform resources are already unhealthy.
  const bool platformNeedsRecovery =
      !Platform::WindowEvents::AllWinEventHooksInstalled() ||
      !g_vd.initialized ||
      (g_vd.abi.notification.methodCount > 0 && !g_vd.notificationsRegistered);
  if (platformNeedsRecovery) RequestMaintenance(true);
  Reconcile::ScheduleLifecycleReconcile(nullptr);
}

}  // namespace RuntimeLifecycle

