// Initial-placement rules and automatic overflow routing.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Reconcile {

// How long an overflow move waits for the shell to finish switching to the
// destination desktop before the move is treated as failed and rolled back.
constexpr ULONGLONG kOverflowFollowTimeoutMs = 2000;

// Freezes a matching initial-placement rule into a pending request. Native
// calls may deliver events, so rule data is copied and HWND identity rechecked.
// False can mean deferred observation or an evaluated rule with no destination;
// the existing evaluated/pending sets retain that distinction. WM thread only.
static bool TryQueueInitialWindowRoute(HWND hwnd, DWORD processId, const GUID& sourceDesktop) {
  if (g_wm.routing.evaluated.count(hwnd) || !IsWindow(hwnd)) return false;
  const WindowRule* rule = Platform::Windows::FindMatchingWindowRule(hwnd, WindowRuleTreatment::InitialPlacement);
  if (g_wm.routing.evaluated.count(hwnd) || !IsWindow(hwnd)) return false;
  if (!rule) {
    g_wm.routing.evaluated.insert(hwnd);
    return false;
  }
  // Copy the rule before COM calls, which may deliver lifecycle events.
  PendingWindowRoute request;
  request.processId = processId;
  request.sourceDesktop = sourceDesktop;
  request.targetDesktop = sourceDesktop;
  request.monitor = rule->monitor;
  request.monitorId = rule->monitorId;
  request.followWindow = rule->followWindow;
  const int desktopNumber = rule->desktopNumber;
  const std::wstring desktopName = rule->desktopNameContains;
  if (desktopNumber || !desktopName.empty()) {
    std::vector<GUID> order;
    if (!Platform::VirtualDesktop::GetDesktopIdsInOrder(&order)) return false;
    request.targetDesktop = GUID_NULL;
    for (size_t i = 0; i < order.size(); ++i) {
      if (desktopNumber && desktopNumber != static_cast<int>(i + 1)) continue;
      if (!desktopName.empty()) {
        const auto metadata = Platform::VirtualDesktop::ReadDesktopMetadata(order[i]);
        if (!Platform::Windows::ContainsInsensitive(metadata.name, desktopName)) continue;
      }
      request.targetDesktop = order[i];
      break;
    }
  }
  if (g_wm.routing.evaluated.count(hwnd) || !Platform::Windows::IsSameLiveWindow(hwnd, processId) ||
      !Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, sourceDesktop)) return false;
  g_wm.routing.evaluated.insert(hwnd);
  if (IsEqualGUID(request.targetDesktop, GUID_NULL)) {
    Wh_Log(L"Initial placement desktop unavailable for HWND %p", hwnd);
    return false;
  }
  g_wm.routing.pending.emplace(hwnd, std::move(request));
  return true;
}

// Chooses a connected monitor using the rule and stable monitor IDs. Performs
// native observations only; nullptr means the configured destination is absent.
static HMONITOR ResolveInitialRouteMonitor(
    const PendingWindowRoute& request, HMONITOR sourceMonitor) {
  HMONITOR targetMonitor = sourceMonitor;
  if (request.monitor == RuleMonitor::Specific) {
    Model::MonitorId identity;
    identity.deviceId = request.monitorId;
    targetMonitor = identity.Resolve();
  } else if (request.monitor == RuleMonitor::Primary || request.monitor == RuleMonitor::NonPrimary) {
    auto matchesMonitor = [&](HMONITOR monitor) {
      MONITORINFO info{sizeof(info)};
      return GetMonitorInfoW(monitor, &info) &&
          ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) == (request.monitor == RuleMonitor::Primary);
    };
    if (!matchesMonitor(sourceMonitor)) {
      std::vector<std::pair<std::wstring, HMONITOR>> connected;
      EnumDisplayMonitors(nullptr, nullptr,
          [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) WINAPI -> BOOL {
            Model::MonitorId identity;
            if (Model::MonitorId::FromHMonitor(monitor, &identity)) {
              reinterpret_cast<std::vector<std::pair<std::wstring, HMONITOR>>*>(parameter)
                  ->emplace_back(identity.deviceId, monitor);
            }
            return TRUE;
          }, reinterpret_cast<LPARAM>(&connected));
      // Stable IDs give a repeatable choice among several secondary monitors.
      std::sort(connected.begin(), connected.end(), [](const auto& first, const auto& second) {
        return first.first < second.first;
      });
      targetMonitor = nullptr;
      for (const auto& candidate : connected) {
        if (matchesMonitor(candidate.second)) { targetMonitor = candidate.second; break; }
      }
    }
  }
  return targetMonitor;
}

// --- Routing one window to its initial-placement destination ----------------

// Result of one routing stage. Retry keeps the request for a later pass;
// Abandon drops it, leaving the window where it is now.
enum class RouteStep { Continue, Retry, Abandon };

// One routing attempt. Values are copied up front because native calls below
// can deliver events that change or remove the stored request.
struct InitialRouteAttempt {
  HWND hwnd = nullptr;
  DWORD processId = 0;
  GUID sourceDesktop{};
  PendingWindowRoute request;
  HMONITOR sourceMonitor = nullptr;
  HMONITOR targetMonitor = nullptr;

  bool ChangesDesktop() const { return !IsEqualGUID(request.targetDesktop, sourceDesktop); }
  bool ChangesMonitor() const { return targetMonitor != sourceMonitor; }
};

// True while the request is still pending and the window is still the same,
// unmanaged, restored, visible window on its source desktop, which is active.
// Re-check after every native call: they can run application or shell code.
static bool IsInitialRouteStillValid(const InitialRouteAttempt& route) {
  const HWND hwnd = route.hwnd;
  const auto pending = g_wm.routing.pending.find(hwnd);
  if (pending == g_wm.routing.pending.end() || pending->second.processId != route.processId) return false;
  return Platform::Windows::IsSameLiveWindow(hwnd, route.processId) &&
      !Platform::Windows::IsWindowTrackedInAnyState(hwnd) &&
      !Gestures::IsMoveSizeGestureInProgress(hwnd) && !Platform::Windows::IsWindowCloaked(hwnd) &&
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
      Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, route.sourceDesktop) &&
      Platform::VirtualDesktop::IsCurrentDesktop(route.sourceDesktop) &&
      g_wm.routing.pending.count(hwnd);  // COM calls above may have cancelled the route.
}

// A minimized or maximized window is routed once it is restored. Meanwhile a
// maximized one gets an arrival generation so its source workspace can count it.
static void DeferSuspendedInitialRoute(const InitialRouteAttempt& route, SuspensionReason reason) {
  const auto pending = g_wm.routing.pending.find(route.hwnd);
  if (pending == g_wm.routing.pending.end()) return;
  if (reason == SuspensionReason::Maximized) {
    if (!pending->second.visibleGeneration) pending->second.visibleGeneration = Model::NextTilingGeneration();
    AddPendingWorkspaceArrange(route.sourceDesktop, route.sourceMonitor);
  } else {
    pending->second.visibleGeneration = 0;
  }
}

// The target desktop was frozen as a GUID when the rule matched. It must still
// exist; reordering desktops must not retarget the window.
static RouteStep CheckRouteDesktopExists(const InitialRouteAttempt& route) {
  std::vector<GUID> order;
  if (!Platform::VirtualDesktop::GetDesktopIdsInOrder(&order)) return RouteStep::Retry;
  const bool exists = std::any_of(order.begin(), order.end(), [&](const GUID& id) {
    return IsEqualGUID(id, route.request.targetDesktop);
  });
  return exists ? RouteStep::Continue : RouteStep::Abandon;
}

// Centers the window on the target monitor, keeping its size, and confirms it
// arrived. `sourceFrame` receives the original frame for rollback.
static RouteStep MoveToRouteMonitor(
    const InitialRouteAttempt& route, RECT* sourceFrame, std::vector<HMONITOR>& monitors) {
  const HWND hwnd = route.hwnd;
  RECT workArea{};
  if (!Platform::Windows::GetWindowFrameRect(hwnd, sourceFrame) || Layout::RectWidth(*sourceFrame) <= 0 ||
      Layout::RectHeight(*sourceFrame) <= 0 ||
      !Platform::Monitors::GetWorkspaceWorkArea(route.targetMonitor, &workArea)) {
    return RouteStep::Abandon;
  }
  const RECT target = Placement::CenteredRect(
      Placement::RectCenter(workArea), Layout::RectWidth(*sourceFrame), Layout::RectHeight(*sourceFrame));
  const auto placed = Platform::Windows::PlaceWindowChecked(hwnd, true, target, true);
  if (placed.result == PlacementResult::SuppressedByPhysicalState) return RouteStep::Retry;
  AddUniqueMonitor(monitors, Platform::Windows::GetWindowPhysicalMonitor(hwnd));
  if (placed.result != PlacementResult::Success ||
      Platform::Windows::GetWindowPhysicalMonitor(hwnd) != route.targetMonitor) {
    Diagnostics::RecordEvent(L"initial placement monitor refused hwnd=%p target=%p result=%d",
                             hwnd, route.targetMonitor, static_cast<int>(placed.result));
    return RouteStep::Abandon;
  }
  return RouteStep::Continue;
}

// After a failed desktop move, puts the window back on its original monitor, but
// only if we moved it, the user has not acted since, and the request is still live.
static void UndoRouteMonitorMove(const InitialRouteAttempt& route, const RECT& sourceFrame, DWORD inputTickMs) {
  LASTINPUTINFO latestInput{sizeof(latestInput)};
  if (route.ChangesMonitor() && GetLastInputInfo(&latestInput) &&
      latestInput.dwTime == inputTickMs && IsInitialRouteStillValid(route)) {
    Platform::Windows::PlaceWindowChecked(route.hwnd, true, sourceFrame, true);
  }
}

// The window now lives on another desktop. Make sure a workspace exists there and
// let the ordinary discovery pass admit it, which enforces fit and window limits
// even when several windows arrive together. Optionally follow it.
static void PrepareRouteDestination(const InitialRouteAttempt& route, HWND foregroundWindow, DWORD inputTickMs) {
  DesktopMonitorKey targetKey{};
  Workspace workspace;
  if (DesktopMonitorKey::FromHMonitor(route.request.targetDesktop, route.targetMonitor, &targetKey) &&
      !g_workspaces.Load(targetKey, &workspace)) {
    g_workspaces.Save(targetKey, MakeDefaultWorkspace(targetKey));
  }
  AddPendingWorkspaceArrange(route.request.targetDesktop, route.targetMonitor);
  if (route.request.followWindow &&
      (!g_wm.routing.follow.hwnd || route.hwnd == foregroundWindow)) {
    g_wm.routing.follow = {route.hwnd, route.processId, route.sourceDesktop,
                                          route.request.targetDesktop, {}, 0, inputTickMs};
  }
  ScheduleLifecycleReconcile(nullptr);
}

// Processes one observed HWND: matches an initial-placement rule (once), then
// moves the window to the rule's monitor and/or desktop. Updates the
// pending/evaluated routing sets and may move the native window. Admission is
// left to ordinary discovery at the destination. WM thread only.
static void ProcessInitialWindowRoute(
    HWND hwnd, HWND foregroundWindow, std::vector<HMONITOR>& monitors) {
  // Explicit admission and user gestures override pending initial placement.
  if (Platform::Windows::IsWindowTrackedInAnyState(hwnd) || Gestures::IsMoveSizeGestureInProgress(hwnd)) {
    g_wm.routing.evaluated.insert(hwnd);
    g_wm.routing.pending.erase(hwnd);
    return;
  }
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || IsHungAppWindow(hwnd)) return;
  if (!Platform::Windows::WindowCanBeManaged(hwnd, true)) {
    // Exclusion wins. Ineligible framework HWNDs may become eligible later.
    g_wm.routing.pending.erase(hwnd);
    return;
  }
  DWORD processId = 0;
  GUID sourceDesktop{};
  GetWindowThreadProcessId(hwnd, &processId);
  if (!processId || !Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &sourceDesktop) ||
      IsEqualGUID(sourceDesktop, GUID_NULL)) return;

  // 1. Find (or create, by matching rules) the pending request.
  if (!g_wm.routing.pending.count(hwnd) &&
      !TryQueueInitialWindowRoute(hwnd, processId, sourceDesktop)) return;
  // Never retain map iterators or references across native calls.
  const auto pending = g_wm.routing.pending.find(hwnd);
  if (pending == g_wm.routing.pending.end()) return;
  InitialRouteAttempt route{hwnd, processId, sourceDesktop, pending->second};
  if (route.request.processId != processId || !IsEqualGUID(route.request.sourceDesktop, sourceDesktop)) {
    g_wm.routing.pending.erase(hwnd);  // The app or user relocated it.
    return;
  }
  if (!Platform::VirtualDesktop::IsCurrentDesktop(sourceDesktop) ||
      Platform::Windows::IsWindowCloaked(hwnd)) return;

  // 2. Decide where it goes.
  route.sourceMonitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
  AddUniqueMonitor(monitors, route.sourceMonitor);
  route.targetMonitor = ResolveInitialRouteMonitor(route.request, route.sourceMonitor);
  if (!route.targetMonitor) {
    Wh_Log(L"Initial placement monitor unavailable for HWND %p", hwnd);
    g_wm.routing.pending.erase(hwnd);
    return;
  }
  if (!route.ChangesDesktop() && !route.ChangesMonitor()) {
    // Already at its destination: normal suspended admission must still count
    // maximized windows, without waiting for an unnecessary physical move.
    g_wm.routing.pending.erase(hwnd);
    return;
  }
  const SuspensionReason suspensionReason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);
  if (suspensionReason != SuspensionReason::None) {
    DeferSuspendedInitialRoute(route, suspensionReason);
    return;
  }
  if (route.ChangesDesktop()) {
    const RouteStep step = CheckRouteDesktopExists(route);
    if (step == RouteStep::Abandon) g_wm.routing.pending.erase(hwnd);
    if (step != RouteStep::Continue) return;
  }

  // 3. Move it: monitor first (while still visible), then desktop.
  LASTINPUTINFO input{sizeof(input)};
  if (!GetLastInputInfo(&input) || !IsInitialRouteStillValid(route)) return;
  RECT sourceFrame{};
  if (route.ChangesMonitor()) {
    const RouteStep step = MoveToRouteMonitor(route, &sourceFrame, monitors);
    if (step == RouteStep::Abandon) g_wm.routing.pending.erase(hwnd);
    if (step != RouteStep::Continue) return;
  }
  if (!IsInitialRouteStillValid(route)) return;
  if (route.ChangesDesktop() && !Platform::VirtualDesktop::MoveWindowToDesktop(
          hwnd, processId, sourceDesktop, route.request.targetDesktop,
          Platform::VirtualDesktop::WindowMovePurpose::InitialPlacement)) {
    UndoRouteMonitorMove(route, sourceFrame, input.dwTime);
    g_wm.routing.pending.erase(hwnd);
    return;
  }

  // 4. Confirm and commit.
  if (!Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, route.request.targetDesktop)) return;
  if (!g_wm.routing.pending.count(hwnd) || !Platform::Windows::IsSameLiveWindow(hwnd, processId)) return;
  g_wm.routing.pending.erase(hwnd);  // Overflow must never reapply this rule.
  AddUniqueMonitor(monitors, route.targetMonitor);
  if (route.ChangesDesktop()) PrepareRouteDestination(route, foregroundWindow, input.dwTime);
  Diagnostics::RecordEvent(L"initial placement hwnd=%p targetDesktop=%08X monitor=%p follow=%d",
                           hwnd, route.request.targetDesktop.Data1, route.targetMonitor,
                           route.request.followWindow ? 1 : 0);
}
// Run before snapshots are built. A rule chooses the initial destination once;
// admission and overflow remain owned by the destination's ordinary active pass.
static void RouteNewWindows(std::vector<HMONITOR>& monitors) {
  if (!g_wm.routing.ready) return;

  const HWND foregroundWindow = GetForegroundWindow();
  std::vector<HWND> candidates;
  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        if (!g_wm.routing.evaluated.count(hwnd) || g_wm.routing.pending.count(hwnd)) {
          reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&candidates));

  for (HWND hwnd : candidates) {
    ProcessInitialWindowRoute(hwnd, foregroundWindow, monitors);
  }
}

// Switch only after snapshots have finished. No forced HWND activation is needed.
static void FollowInitiallyPlacedWindow() {
  const PendingDesktopFollow pending = g_wm.routing.follow;
  if (!pending.hwnd) return;
  if (Platform::VirtualDesktop::SwitchToDesktop(
          pending.hwnd, pending.processId, pending.targetDesktop, pending.sourceDesktop, pending.inputTickMs,
          Platform::VirtualDesktop::WindowMovePurpose::InitialPlacement)) {
    g_wm.desktop.state = ReconciledDesktopState::TransitionPending;
    ScheduleLifecycleReconcile(nullptr);
  }
  g_wm.routing.follow = {};
}

// Keep the repository aligned with the observed result even if a native move
// or its rollback fails. Shell identity, never our intended target, wins.
static void ReconcileOverflowWindowOwnership(HWND hwnd) {
  GUID observedDesktop{};
  DesktopMonitorKey observedKey{};
  if (Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &observedDesktop) &&
      DesktopMonitorKey::FromHMonitor(observedDesktop, Platform::Windows::GetWindowPhysicalMonitor(hwnd), &observedKey)) {
    std::vector<DesktopMonitorKey> changed;
    EnsureUniqueWindowOwnership(hwnd, observedKey, &changed);
    for (const auto& key : changed) Placement::ArrangeWorkspace(key);
  }
}

// Restores a failed overflow move when user input and HWND identity still permit it.
// Queries/moves native windows, repairs repository ownership, and arranges owners.
static void RollbackOverflowMove(const PendingDesktopFollow& pending) {
  GUID observed{};
  if (!Platform::Windows::IsSameLiveWindow(pending.hwnd, pending.processId) ||
      !g_wm.overflow.windows.count(pending.hwnd)) {
    Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(pending.targetDesktop, pending.sourceDesktop);
    return;
  }
  if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(pending.hwnd, &observed) ||
      (!IsEqualGUID(observed, pending.sourceDesktop) &&
       !IsEqualGUID(observed, pending.targetDesktop))) {
    ReconcileOverflowWindowOwnership(pending.hwnd);
    Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(pending.targetDesktop, pending.sourceDesktop);
    return;
  }
  LASTINPUTINFO input{sizeof(input)};
  if (!Platform::VirtualDesktop::IsCurrentDesktop(pending.sourceDesktop) ||
      !GetLastInputInfo(&input) || input.dwTime != pending.inputTickMs ||
      Gestures::IsMoveSizeGestureInProgress(pending.hwnd)) {
    ReconcileOverflowWindowOwnership(pending.hwnd);
    Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(pending.targetDesktop, pending.sourceDesktop);
    return;
  }
  const bool alreadyHome = IsEqualGUID(observed, pending.sourceDesktop);
  const bool returned = alreadyHome || Platform::VirtualDesktop::MoveWindowToDesktop(
      pending.hwnd, pending.processId, pending.targetDesktop, pending.sourceDesktop);
  if (returned && Platform::VirtualDesktop::IsWindowOnDesktop(pending.hwnd, pending.sourceDesktop)) {
    Platform::Windows::PlaceWindowChecked(pending.hwnd, true, pending.sourceFrame);
  }
  ReconcileOverflowWindowOwnership(pending.hwnd);
  // A failed native follow uses the configured floating fallback, including
  // when rollback cannot return the window home.
  for (const auto& key : g_workspaces.OwnersOf(pending.hwnd)) {
    Workspace workspace;
    if (g_workspaces.Load(key, &workspace) && workspace.Float(pending.hwnd)) {
      g_workspaces.Save(key, workspace);
      Placement::ArrangeWorkspace(key);
    }
  }
  Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(pending.targetDesktop, pending.sourceDesktop);
  Diagnostics::RecordEvent(L"overflow rollback hwnd=%p returned=%d", pending.hwnd, returned ? 1 : 0);
  if (!returned) TrayUi::ShowCommandFailureFlyout(L"Could not return the window to its desktop.");
}

// Polls an existing follow request; returns whether its arrival still needs a timer.
// A user desktop change cancels the follow and is never reversed.
static bool UpdateOverflowDesktopFollow(
    const GUID& currentDesktop, bool desktopKnown, ULONGLONG now) {
  bool waitForSwitch = false;
  PendingDesktopFollow transition = g_wm.overflow.follow;
  if (transition.hwnd) {
    LASTINPUTINFO input{sizeof(input)};
    if (!Platform::Windows::IsSameLiveWindow(transition.hwnd, transition.processId) ||
        !g_wm.overflow.windows.count(transition.hwnd) ||
        !GetLastInputInfo(&input) || input.dwTime != transition.inputTickMs ||
        !Platform::VirtualDesktop::IsWindowOnDesktop(transition.hwnd, transition.targetDesktop)) {
      transition = {}; // User input or a changed lifetime cancels the follow.
    } else if (desktopKnown && IsEqualGUID(currentDesktop, transition.targetDesktop)) {
      // Desktop arrival is enough. Let the shell reveal and focus application
      // views; neither a cloaking sample nor SetForegroundWindow proves readiness.
      Diagnostics::RecordEvent(L"overflow desktop arrived target=%08X hwnd=%p",
                               transition.targetDesktop.Data1, transition.hwnd);
      ScheduleLifecycleReconcile(transition.hwnd);
      transition = {};
    } else if ((desktopKnown && !IsEqualGUID(currentDesktop, transition.sourceDesktop)) ||
               now >= transition.deadlineTickMs) {
      const PendingDesktopFollow failed = transition;
      transition = {};
      // A user-initiated desktop change should never be reversed.
      if (desktopKnown && IsEqualGUID(currentDesktop, failed.sourceDesktop)) {
        RollbackOverflowMove(failed);
      }
    } else {
      waitForSwitch = true;
    }
  }
  g_wm.overflow.follow = transition;
  return waitForSwitch;
}

struct OverflowDestination {
  Workspace workspace;
  RECT frame{}; // Incoming window frame in physical screen pixels.
};

// Builds a trial destination using live observations on the WM thread.
// Returns a complete candidate or no value; partial trials stay local. Native
// queries may refresh platform caches, but never save or place windows. An
// initial workspace denotes a new desktop; otherwise members are enumerated.
static std::optional<OverflowDestination> PlanOverflowDestination(
    const DesktopMonitorKey& key, const WindowRecord& incoming,
    const RECT& sourceFrame, const Workspace* initial = nullptr) {
  OverflowDestination destination;
  const HWND hwnd = incoming.hwnd;
  Workspace& trial = destination.workspace;
  const HMONITOR monitor = key.ResolveMonitor();
  Platform::Monitors::WorkspaceMetrics metrics;
  if (!monitor || !Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return std::nullopt;
  if (initial) trial = *initial;
  else if (!g_workspaces.Load(key, &trial)) trial = MakeDefaultWorkspace(key);
  trial.Forget(hwnd);
  if (trial.Layout() == TileLayout::Floating) {
    if (!initial) return std::nullopt; // Another monitor must have room to tile.
    WindowRecord record = incoming;
    record.pendingInsertionPlacement = false;
    trial.AdmitFloating(std::move(record));
    destination.frame = sourceFrame;
    return destination;
  }
  if (!incoming.constraints.valid || !incoming.constraints.dpi) return std::nullopt;
  const auto oldRecords = trial.Records();
  for (const auto& member : oldRecords) {
    GUID owner{};
    if (!Platform::Windows::IsSameLiveWindow(member.first, member.second.pid)) {
      trial.Forget(member.first);
    } else if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(member.first, &owner)) {
      return std::nullopt;
    } else if (!IsEqualGUID(owner, key.desktopId) || Platform::Windows::GetWindowPhysicalMonitor(member.first) != monitor) {
      trial.Forget(member.first);
    } else {
      SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(member.first);
      if (reason == SuspensionReason::Maximized && !IsZoomed(member.first) &&
          member.second.state == ManageState::Tiled) {
        RECT authoritative{}, current{};
        if (Placement::GetCurrentAuthoritativeTiledRect(key, trial, member.first, &authoritative) &&
            Platform::Windows::GetWindowFrameRect(member.first, &current) && Layout::RectsNear(current, authoritative)) {
          reason = SuspensionReason::None;
        }
      }
      if (member.second.state == ManageState::Tiled && reason != SuspensionReason::None) {
        trial.Suspend(member.first, reason);
      } else if (member.second.state == ManageState::Suspended) {
        if (reason == SuspensionReason::None) trial.Restore(member.first);
        else trial.Suspend(member.first, reason);
      }
    }
  }
  struct CandidateEnumeration {
    const DesktopMonitorKey* key;
    HMONITOR monitor;
    Workspace* trial;
    HWND incoming;
    bool complete = true;
  } context{&key, monitor, &trial, hwnd};
  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  const BOOL enumerated = initial ? TRUE : EnumWindows([](HWND candidate, LPARAM parameter) WINAPI -> BOOL {
    ++Diagnostics::g_runtime.counters.enumWindowsVisited;
    auto& context = *reinterpret_cast<CandidateEnumeration*>(parameter);
    if (candidate == context.incoming || !IsWindowVisible(candidate) ||
        !Platform::Windows::WindowCanBeManaged(candidate) || Platform::Windows::GetWindowPhysicalMonitor(candidate) != context.monitor ||
        (!Platform::Windows::IsWindowTrackedInAnyState(candidate) && !IsWindowEnabled(candidate))) return TRUE;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(candidate, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
      context.complete = false;
      return TRUE;
    }
    if (cloaked) return TRUE; // Only source-desktop monitors are scanned.
    GUID desktop{};
    if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(candidate, &desktop)) {
      context.complete = false;
      return TRUE;
    }
    if (!IsEqualGUID(desktop, context.key->desktopId)) return TRUE;
    if (context.trial->HasRecord(candidate)) return TRUE;
    // A stale owner must be reconciled before importing this destination;
    // copying it here would duplicate ownership or lose its floating policy.
    if (Platform::Windows::IsWindowTrackedInAnyState(candidate) || Platform::Windows::IsWindowAwaitingInitialPlacement(candidate)) {
      context.complete = false;
      return TRUE;
    }
    bool alwaysFloating = false;
    if (Platform::Windows::ShouldWindowStartFloating(candidate, &alwaysFloating)) {
      WindowRecord record = Platform::Windows::MakeWindowRecord(candidate, ManageState::Floating);
      record.alwaysFloating = alwaysFloating;
      context.trial->AdmitFloating(std::move(record));
    } else {
      context.trial->AdmitInitial(Platform::Windows::MakeWindowRecord(candidate), Platform::Windows::GetPhysicalSuspensionReason(candidate));
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&context));
  if (!enumerated || !context.complete) return std::nullopt;
  trial.Forget(hwnd);
  const size_t limit = Placement::AutomaticWindowLimitForMonitor(key.monitor);
  if (limit && trial.VisibleWindowCount() >= limit) return std::nullopt;
  WindowRecord record = incoming;
  // Predict monitor-DPI constraints, then probe again after a physical move.
  if (record.constraints.dpi != metrics.dpi) {
    record.constraints.minWidth = std::max<LONG>(1, MulDiv(record.constraints.minWidth, metrics.dpi, record.constraints.dpi));
    record.constraints.minHeight = std::max<LONG>(1, MulDiv(record.constraints.minHeight, metrics.dpi, record.constraints.dpi));
    record.constraints.dpi = metrics.dpi;
  }
  const HWND anchor = g_settings.automaticNewWindowPosition == AutomaticNewWindowPosition::AfterFocused
      ? trial.LastFocusedTiledWindow() : nullptr;
  if (anchor) trial.AdmitTiledAfter(std::move(record), anchor);
  else trial.AdmitTiled(std::move(record));
  for (HWND member : trial.TiledWindows()) {
    if (member == hwnd) continue;
    const WindowConstraints constraints = Platform::Windows::ProbeWindowConstraints(member);
    if (!constraints.valid) return std::nullopt;
    trial.UpdateConstraints(member, constraints);
  }
  auto fits = [&](Workspace& candidate) {
    std::vector<RECT> rects;
    if (Layout::BuildWorkspaceLayoutPlan(candidate, metrics.workArea, metrics.gap, &rects) != Layout::LayoutPlanStatus::Success) return false;
    const size_t index = candidate.TiledIndex(hwnd);
    if (index >= rects.size()) return false;
    destination.frame = rects[index];
    return true;
  };
  if (fits(trial)) return destination;
  const Workspace preferred = trial;
  const size_t preferredIndex = preferred.TiledIndex(hwnd);
  for (size_t offset = 1; offset < preferred.ActiveCount(); ++offset) {
    for (int direction : {-1, 1}) {
      const long long index = static_cast<long long>(preferredIndex) + direction * static_cast<long long>(offset);
      if (index < 0 || static_cast<size_t>(index) >= preferred.ActiveCount()) continue;
      Workspace alternative = preferred;
      if (alternative.MovePendingAdmissionToIndex(hwnd, static_cast<size_t>(index)) && fits(alternative)) {
        trial = std::move(alternative);
        return destination;
      }
    }
  }
  return std::nullopt;
}

// Results distinguish a retryable observation from a failed native mutation.
// Only NoDestination permits trying another candidate; Ignored drops stale work.
enum class OverflowOutcome { Ignored, Retry, Placed, Failed, NoDestination };

struct OverflowBatch {
  GUID desktop{};
  bool desktopKnown = false;
  HWND foreground = nullptr;
  LASTINPUTINFO input{sizeof(LASTINPUTINFO)};
};

struct OverflowSource {
  DesktopMonitorKey key{};
  HMONITOR monitor = nullptr;
  RECT frame{};
  Workspace workspace;
  WindowRecord incoming;
};

struct OverflowRequestResult {
  OverflowOutcome outcome;
  PendingDesktopFollow follow{};
};

// WM thread only. Plans and commits one destination, with native checks repeated
// after application/shell calls. Failed moves may repair ownership or roll back;
// NoDestination performs neither placement nor repository saves.
static OverflowRequestResult TryCommitOverflowDestination(
    const PendingWindowOverflow& request, const OverflowBatch& batch,
    OverflowSource& source, HMONITOR monitor, bool createDesktop) {
  const HWND hwnd = request.hwnd;
  const DWORD processId = request.processId;
  GUID desktop = request.sourceDesktop;
  GUID createdDesktop{};
  int desktopNumber = 0;
  DesktopMonitorKey targetKey{};
  Workspace initial;
  Platform::VirtualDesktop::DesktopMetadata metadata;
  auto resolveOverflowLayout = [&](const DesktopMonitorKey& key) {
    int matchedRule = 0;
    const TileLayout ruleLayout = ResolveInitialWorkspaceLayout(key, &matchedRule, &metadata);
    if (matchedRule) {
      Workspace selected;
      selected.SetLayout(ruleLayout);
      selected.SetMasterRatio(g_settings.masterPercent / 100.0);
      return selected;
    }
    return source.workspace.EmptyWithInheritedLayoutSettings();
  };
  if (createDesktop) {
    std::vector<GUID> order;
    if (!Platform::VirtualDesktop::GetDesktopIdsInOrder(&order)) return {OverflowOutcome::NoDestination};
    auto position = std::find_if(order.begin(), order.end(),
        [&](const GUID& id) { return IsEqualGUID(id, request.sourceDesktop); });
    if (position == order.end()) return {OverflowOutcome::NoDestination};
    metadata.number = static_cast<int>(position - order.begin()) + 2;
    // A desktop created by this operation starts without a custom name.
    targetKey = {GUID_NULL, source.key.monitor};
    initial = resolveOverflowLayout(targetKey);
  } else if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &targetKey)) return {OverflowOutcome::NoDestination};
  auto destination = PlanOverflowDestination(
      targetKey, source.incoming, source.frame, createDesktop ? &initial : nullptr);
  if (!destination) return {OverflowOutcome::NoDestination};
  auto canMoveIncoming = [&] {
    LASTINPUTINFO liveInput{sizeof(liveInput)};
    return GetLastInputInfo(&liveInput) &&
        (hwnd != batch.foreground || (GetForegroundWindow() == hwnd && liveInput.dwTime == batch.input.dwTime)) &&
        Platform::Windows::IsSameLiveWindow(hwnd, processId) && g_wm.overflow.windows.count(hwnd) &&
        Platform::VirtualDesktop::IsCurrentDesktop(request.sourceDesktop) &&
        Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, request.sourceDesktop) &&
        Platform::Windows::GetWindowPhysicalMonitor(hwnd) == source.monitor &&
        !Gestures::IsMoveSizeGestureInProgress(hwnd) && !Platform::Windows::IsWindowCloaked(hwnd) &&
        Platform::Windows::GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None;
  };
  if (!canMoveIncoming()) return {OverflowOutcome::Retry};
  if (createDesktop) {
    const bool created = Platform::VirtualDesktop::CreateDesktopAfter(
        request.sourceDesktop, &createdDesktop, &desktopNumber);
    desktop = createdDesktop;
    targetKey.desktopId = desktop;
    metadata = Platform::VirtualDesktop::ReadDesktopMetadata(desktop);
    metadata.number = desktopNumber; // Native order wins over lagging registry data.
    initial = resolveOverflowLayout(targetKey);
    const bool readyAfterCreation = canMoveIncoming();
    bool destinationReady = created && readyAfterCreation;
    if (destinationReady) destinationReady = Platform::VirtualDesktop::IsDesktopEmptyExcept(desktop);
    if (destinationReady) {
      destination = PlanOverflowDestination(targetKey, source.incoming, source.frame, &initial);
      destinationReady = destination.has_value();
    }
    if (destinationReady) destinationReady = canMoveIncoming();
    if (!destinationReady) {
      const bool retry = created && (!readyAfterCreation || !canMoveIncoming());
      Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(createdDesktop, request.sourceDesktop);
      return {retry ? OverflowOutcome::Retry : OverflowOutcome::Failed};
    }
  }
  // Change monitors while the HWND is still visible. Off-desktop placement
  // is suppressed by the ordinary safety gate. Verify the app's real DPI
  // contract before committing its virtual-desktop ownership.
  if (monitor != source.monitor) {
    const PlacementObservation placed = Platform::Windows::PlaceWindowChecked(hwnd, source.incoming.canMove, destination->frame);
    source.incoming.constraints = Platform::Windows::ProbeWindowConstraints(hwnd);
    bool moveAccepted = placed.result == PlacementResult::Success &&
        Platform::Windows::GetWindowPhysicalMonitor(hwnd) == monitor && source.incoming.constraints.valid;
    if (moveAccepted) {
      destination = PlanOverflowDestination(
          targetKey, source.incoming, source.frame, createDesktop ? &initial : nullptr);
      moveAccepted = destination.has_value();
    }
    if (!moveAccepted) {
      RollbackOverflowMove({hwnd, processId, request.sourceDesktop, desktop, source.frame, 0, batch.input.dwTime});
      return {OverflowOutcome::Failed};
    }
  }
  const bool differentDesktop = !IsEqualGUID(desktop, request.sourceDesktop);
  if (differentDesktop && !Platform::VirtualDesktop::MoveWindowToDesktop(
          hwnd, processId, request.sourceDesktop, desktop)) {
    RollbackOverflowMove({hwnd, processId, request.sourceDesktop, desktop, source.frame, 0, batch.input.dwTime});
    return {OverflowOutcome::Failed};
  }
  if (!Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, desktop) ||
      Platform::Windows::GetWindowPhysicalMonitor(hwnd) != monitor) {
    RollbackOverflowMove({hwnd, processId, request.sourceDesktop, desktop, source.frame, 0, batch.input.dwTime});
    return {OverflowOutcome::Failed};
  }
  const bool sourceLoaded = g_workspaces.Load(source.key, &source.workspace);
  const WindowRecord* liveRecord = sourceLoaded ? source.workspace.Find(hwnd) : nullptr;
  if (!liveRecord || liveRecord->pid != processId ||
      liveRecord->tilingGeneration != request.tilingGeneration ||
      !g_wm.overflow.windows.count(hwnd)) {
    ReconcileOverflowWindowOwnership(hwnd);
    if (createDesktop) Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(createdDesktop, request.sourceDesktop);
    return {OverflowOutcome::Failed};
  }
  // Destination membership can change during the native calls. Repeat the
  // proof after movement; a new conflict rolls back only this incoming HWND.
  source.incoming.constraints = Platform::Windows::ProbeWindowConstraints(hwnd);
  bool destinationStillFits = destination->workspace.Layout() == TileLayout::Floating ||
      source.incoming.constraints.valid;
  if (destinationStillFits && createDesktop) {
    destinationStillFits = Platform::VirtualDesktop::IsDesktopEmptyExcept(desktop, hwnd);
  }
  if (destinationStillFits) {
    destination = PlanOverflowDestination(
        targetKey, source.incoming, source.frame, createDesktop ? &initial : nullptr);
    destinationStillFits = destination.has_value();
  }
  if (!destinationStillFits) {
    RollbackOverflowMove({hwnd, processId, request.sourceDesktop, desktop, source.frame, 0, batch.input.dwTime});
    return {OverflowOutcome::Failed};
  }
  LASTINPUTINFO finalInput{sizeof(finalInput)};
  if (!Platform::Windows::IsSameLiveWindow(hwnd, processId) ||
      !g_wm.overflow.windows.count(hwnd) || Gestures::IsMoveSizeGestureInProgress(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      !GetLastInputInfo(&finalInput) ||
      (hwnd == batch.foreground && finalInput.dwTime != batch.input.dwTime) ||
      !Platform::VirtualDesktop::IsCurrentDesktop(request.sourceDesktop) ||
      !Platform::VirtualDesktop::IsWindowOnDesktop(hwnd, desktop) ||
      Platform::Windows::GetWindowPhysicalMonitor(hwnd) != monitor) {
    ReconcileOverflowWindowOwnership(hwnd);
    if (createDesktop) Platform::VirtualDesktop::RemoveEmptyOverflowDesktop(createdDesktop, request.sourceDesktop);
    return {OverflowOutcome::Failed};
  }
  source.workspace.Forget(hwnd);
  g_workspaces.Save(source.key, source.workspace);
  g_workspaces.Save(targetKey, destination->workspace);
  if (createDesktop) {
    std::vector<HMONITOR> connected;
    EnumerateCurrentMonitors(connected);
    for (HMONITOR otherMonitor : connected) {
      DesktopMonitorKey otherKey{}, otherSourceKey{};
      Workspace existing, otherSource;
      if (otherMonitor == monitor ||
          !DesktopMonitorKey::FromHMonitor(desktop, otherMonitor, &otherKey) ||
          g_workspaces.Load(otherKey, &existing) ||
          !DesktopMonitorKey::FromHMonitor(request.sourceDesktop, otherMonitor, &otherSourceKey)) continue;
      if (!g_workspaces.Load(otherSourceKey, &otherSource)) otherSource = MakeDefaultWorkspace(otherSourceKey);
      int matchedRule = 0;
      const TileLayout ruleLayout = ResolveInitialWorkspaceLayout(otherKey, &matchedRule, &metadata);
      Workspace inherited = otherSource.EmptyWithInheritedLayoutSettings();
      if (matchedRule) {
        inherited = Workspace{};
        inherited.SetLayout(ruleLayout);
        inherited.SetMasterRatio(g_settings.masterPercent / 100.0);
      }
      g_workspaces.Save(otherKey, inherited);
    }
  }
  AddPendingWorkspaceArrange(desktop, monitor);
  if (!differentDesktop) Placement::ArrangeWorkspace(targetKey);
  OverflowRequestResult result{OverflowOutcome::Placed};
  if (differentDesktop && hwnd == batch.foreground) {
    result.follow = {hwnd, processId, request.sourceDesktop, desktop, source.frame, GetTickCount64() + kOverflowFollowTimeoutMs, batch.input.dwTime};
  }
  Diagnostics::RecordEvent(L"overflow placed hwnd=%p targetDesktop=%08X monitorId=%ls", hwnd, desktop.Data1, targetKey.monitor.deviceId.c_str());
  return result;
}

// Observes and routes one request. Rebased ownership is written back to request.
// Retry asks the batch caller to retain it; other outcomes consume it. A follow
// is returned only for a committed foreground-window desktop move.
static OverflowRequestResult ProcessOverflowRequest(
    PendingWindowOverflow& request, const OverflowBatch& batch) {
  OverflowSource source;
  const HWND hwnd = request.hwnd;
  const DWORD processId = request.processId;
  if (!processId || !Platform::Windows::IsSameLiveWindow(hwnd, processId) ||
      !g_wm.overflow.windows.count(hwnd) || Gestures::IsMoveSizeGestureInProgress(hwnd)) return {OverflowOutcome::Ignored};
  GUID observedDesktop{};
  source.monitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
  if (!batch.desktopKnown || !Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &observedDesktop) ||
      !DesktopMonitorKey::FromHMonitor(observedDesktop, source.monitor, &source.key)) {
    return {OverflowOutcome::Retry};
  }
  if (!IsEqualGUID(observedDesktop, request.sourceDesktop) ||
      source.key.monitor.deviceId != request.sourceMonitorId) {
    // An app or the shell can relocate a queued window. Preserve its overflow
    // intent while rebasing ownership to the observed desktop and monitor.
    ReconcileOverflowWindowOwnership(hwnd);
    request.sourceDesktop = observedDesktop;
    request.sourceMonitorId = source.key.monitor.deviceId;
  }
  if (!g_workspaces.Load(source.key, &source.workspace)) return {OverflowOutcome::Ignored};
  const WindowRecord* sourceRecord = source.workspace.Find(hwnd);
  if (!sourceRecord || sourceRecord->pid != processId ||
      sourceRecord->tilingGeneration != request.tilingGeneration ||
      !g_wm.overflow.windows.count(hwnd) ||
      sourceRecord->state != ManageState::Floating || sourceRecord->alwaysFloating ||
      source.workspace.Layout() == TileLayout::Floating || Platform::Windows::IsWindowExcludedByRules(hwnd) ||
      Platform::Windows::ShouldWindowAlwaysFloat(hwnd)) return {OverflowOutcome::Ignored};
  if (!IsEqualGUID(batch.desktop, request.sourceDesktop) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None || Platform::Windows::IsWindowCloaked(hwnd)) {
    // Wait for desktop arrival or restoration rather than polling hidden apps.
    return {OverflowOutcome::Retry};
  }
  if (!Platform::Windows::WindowCanBeManaged(hwnd) || IsHungAppWindow(hwnd) ||
      !Platform::Windows::GetWindowFrameRect(hwnd, &source.frame)) {
    return {OverflowOutcome::Retry};
  }

  source.incoming = *sourceRecord;
  source.incoming.state = ManageState::Tiled;
  source.incoming.pendingDefaultFloatingCenter = false;
  source.incoming.constraints = Platform::Windows::ProbeWindowConstraints(hwnd);
  if (!source.incoming.constraints.valid || !source.incoming.constraints.dpi) {
    // A missing startup constraint report is not evidence that the app cannot
    // fit. Retry on the next lifecycle event without losing the routing request.
    Diagnostics::RecordEvent(L"overflow deferred hwnd=%p reason=constraints unavailable", hwnd);
    return {OverflowOutcome::Retry};
  }

  std::vector<HMONITOR> monitors{source.monitor};
  if (g_settings.overflowTryOtherMonitors && !request.monitorFixed) {
    std::vector<HMONITOR> connected;
    EnumerateCurrentMonitors(connected);
    MONITORINFO sourceInfo{sizeof(sourceInfo)};
    GetMonitorInfoW(source.monitor, &sourceInfo);
    auto distance = [&](HMONITOR monitor) {
      MONITORINFO info{sizeof(info)};
      if (!GetMonitorInfoW(monitor, &info)) return LLONG_MAX;
      const long long dx = static_cast<long long>(info.rcMonitor.left) + info.rcMonitor.right -
                           sourceInfo.rcMonitor.left - sourceInfo.rcMonitor.right;
      const long long dy = static_cast<long long>(info.rcMonitor.top) + info.rcMonitor.bottom -
                           sourceInfo.rcMonitor.top - sourceInfo.rcMonitor.bottom;
      return dx * dx + dy * dy;
    };
    std::stable_sort(connected.begin(), connected.end(),
                     [&](HMONITOR first, HMONITOR second) { return distance(first) < distance(second); });
    for (HMONITOR monitor : connected) AddUniqueMonitor(monitors, monitor);
  }

  OverflowRequestResult result{OverflowOutcome::NoDestination};
  // Try source capacity first, then other monitors, then at most one new desktop.
  const size_t candidateCount = monitors.size() + (g_settings.overflowCreateDesktop ? 1 : 0);
  for (size_t index = 0; index < candidateCount; ++index) {
    const bool createDesktop = index == monitors.size();
    const HMONITOR monitor = createDesktop ? source.monitor : monitors[index];
    result = TryCommitOverflowDestination(request, batch, source, monitor, createDesktop);
    if (result.outcome != OverflowOutcome::NoDestination) break;
  }

  if (result.outcome != OverflowOutcome::Placed && IsWindow(hwnd) && g_wm.overflow.windows.count(hwnd) &&
      (result.outcome == OverflowOutcome::Retry || Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
       Platform::Windows::IsWindowCloaked(hwnd))) {
    return {OverflowOutcome::Retry};
  }
  if (result.outcome == OverflowOutcome::NoDestination) {
    Diagnostics::RecordEvent(L"overflow fallback hwnd=%p reason=no viable destination", hwnd);
    Workspace fallback;
    if (g_workspaces.Load(source.key, &fallback)) {
      Placement::FloatingPlacementHint hint;
      hint.intent = Placement::FloatingPlacementIntent::FailureCenter;
      if (Placement::RepairFloatingGeometry(source.key, fallback, hwnd, hint)) g_workspaces.Save(source.key, fallback);
    }
  }
  return {result.outcome == OverflowOutcome::Retry ? OverflowOutcome::Failed : result.outcome, result.follow};

}

// Overflow work runs only after admission/arrangement has saved every source.
// Trial workspaces never change native geometry or evict destination members.
// WM thread only. Native moves are revalidated before saving their trial model;
// failed moves may require ownership repair even when rollback is unavailable.
static void ProcessWindowOverflow() {
  if (g_wm.timers.overflow) {
    KillTimer(nullptr, g_wm.timers.overflow);
    g_wm.timers.overflow = 0;
  }
  if (!RuntimeLifecycle::IsAutomaticMode()) {
    g_wm.overflow.queue.clear();
    g_wm.overflow.follow = {};
    return;
  }

  if (g_wm.overflow.queue.empty() && !g_wm.overflow.follow.hwnd) return;
  const ULONGLONG now = GetTickCount64();
  OverflowBatch batch;
  batch.desktopKnown = Platform::VirtualDesktop::GetCurrentDesktopId(&batch.desktop);
  bool waitForSwitch = UpdateOverflowDesktopFollow(batch.desktop, batch.desktopKnown, now);

  std::vector<PendingWindowOverflow> pending;
  pending.swap(g_wm.overflow.queue);
  batch.foreground = GetForegroundWindow();
  GetLastInputInfo(&batch.input);
  PendingDesktopFollow follow{};
  for (PendingWindowOverflow request : pending) {
    const auto result = ProcessOverflowRequest(request, batch);
    if (result.outcome == OverflowOutcome::Retry) {
      g_wm.overflow.queue.push_back(std::move(request));
    }
    if (result.follow.hwnd) follow = result.follow;
  }

  if (follow.hwnd) {
    LASTINPUTINFO liveInput{sizeof(liveInput)};
    GetLastInputInfo(&liveInput);
    if (liveInput.dwTime == follow.inputTickMs && IsWindow(follow.hwnd) &&
        g_wm.overflow.windows.count(follow.hwnd) &&
        Platform::VirtualDesktop::IsCurrentDesktop(follow.sourceDesktop)) {
      if (Platform::VirtualDesktop::SwitchToDesktop(follow.hwnd, follow.processId, follow.targetDesktop, follow.sourceDesktop, follow.inputTickMs)) {
        follow.deadlineTickMs = GetTickCount64() + kOverflowFollowTimeoutMs;
        g_wm.overflow.follow = follow;
        g_wm.desktop.state = ReconciledDesktopState::TransitionPending;
        waitForSwitch = true;
        ScheduleLifecycleReconcile(nullptr);
      } else {
        GetLastInputInfo(&liveInput);
        if (liveInput.dwTime == follow.inputTickMs &&
            Platform::VirtualDesktop::IsCurrentDesktop(follow.sourceDesktop)) RollbackOverflowMove(follow);
      }
    }
  }
  ULONGLONG nextDue = waitForSwitch ? GetTickCount64() + 50 : 0;
  if (nextDue) {
    const ULONGLONG tick = GetTickCount64();
    g_wm.timers.overflow = SetTimer(nullptr, 0, nextDue > tick ? static_cast<UINT>(nextDue - tick) : 1, nullptr);
    if (!g_wm.timers.overflow) {
      g_wm.overflow.follow = {};
      Wh_Log(L"Overflow switch timer unavailable; retaining observed window ownership");
    }
  }
}

} // namespace Reconcile
