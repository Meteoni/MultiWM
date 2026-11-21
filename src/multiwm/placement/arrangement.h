// Workspace transactions, arrangement and placement outcomes.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Placement {

//
// Layout plans calculate rectangles without changing durable preferences.
// Arrangement applies those plans and folds placement outcomes into the model.
// Recovery trials remain local until a complete candidate can be committed.
// Effectful operations require the serialized WM thread. Planning uses cached
// value observations; native-query and placement boundaries are documented below.

static bool IsWorkspaceOnActiveDesktop(const DesktopMonitorKey& key) {
  GUID zeroGuid{};
  if (IsEqualGUID(key.desktopId, zeroGuid)) return false;

  // The reconciled desktop is actor-owned and authoritative between transition
  // signals. Avoid re-querying Explorer from high-frequency placement and
  // LOCATIONCHANGE paths; a dirty transition deliberately retains the old GUID
  // for comparison while falling through to the existing live query.
  GUID activeDesktop{};
  return RuntimeLifecycle::GetActiveDesktopId(&activeDesktop) && IsEqualGUID(activeDesktop, key.desktopId);
}

// Repairs only model-local arrangement preconditions. OS lifecycle/ownership
// reconciliation belongs elsewhere; this function handles missed destruction and
// legacy states whose active-order record marker drifted.
static void ObserveAndRepairWorkspaceForArrangement(Workspace& workspace) {
  std::vector<HWND> invalidWindows;
  std::vector<WindowRecord> missingActiveRecords;

  for (const auto& kv : workspace.Records()) {
    if (!IsWindow(kv.first)) invalidWindows.push_back(kv.first);
  }
  for (HWND hwnd : workspace.TiledWindows()) {
    if (!workspace.HasRecord(hwnd) && IsWindow(hwnd)) {
      missingActiveRecords.push_back(Platform::Windows::MakeWindowRecord(hwnd, ManageState::Tiled));
    }
  }
  workspace.RepairForArrangement(invalidWindows, missingActiveRecords);
}

// A fresh native size-limit report merged with the cached one.
struct ConstraintProbeRefresh {
  WindowConstraints merged;    // What the record should hold now.
  bool reportChanged = false;  // A valid fresh report changed the cached limits.
};

// Probes `hwnd`'s native size limits and merges the reply with `cached`: a failed
// probe keeps the last valid report. The probe sends WM_GETMINMAXINFO, which runs
// application code, so callers recheck identity/physical state when that matters.
// Changes nothing; callers store `merged` with Workspace::UpdateConstraints.
static ConstraintProbeRefresh ProbeConstraintsAgainstCache(HWND hwnd, WindowConstraints cached) {
  const WindowConstraints fresh = Platform::Windows::ProbeWindowConstraints(hwnd);
  ConstraintProbeRefresh refresh;
  refresh.merged = Model::MergeConstraintProbeWithCache(cached, fresh);
  refresh.reportChanged = fresh.valid && !Model::SameConstraintGeometry(cached, refresh.merged);
  return refresh;
}

// --- Retrying size limits a window didn't report ------------------------------
//
// A busy app (often one that has just appeared) may not answer the size-limit
// query in time, and is then tiled with no known minimum. Without a retry the
// next query waits for an unrelated arrangement, so the layout reshuffles for
// it when some other window opens. Instead it is asked again shortly, a bounded
// number of times; each retry arranges its workspace, which queries it again.

inline constexpr unsigned kMaxConstraintRetries = 4;

static void ArmConstraintRetryTimer() {
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.constraintRetry);
  auto& retries = g_wm.lifecycle.constraintRetries;
  ULONGLONG earliest = 0;
  for (const PendingConstraintRetry& retry : retries) {
    if (retry.dueTickMs && (!earliest || retry.dueTickMs < earliest)) earliest = retry.dueTickMs;
  }
  if (!earliest) return;
  const ULONGLONG now = GetTickCount64();
  const UINT delayMs = earliest > now ? static_cast<UINT>(earliest - now) : 1;
  g_wm.timers.constraintRetry = SetTimer(nullptr, 0, delayMs, nullptr);
  if (!g_wm.timers.constraintRetry) {
    Wh_Log(L"Failed to schedule a size-limit retry");
    retries.clear();
  }
}

// `hwnd`, a tiled member, didn't report its size limits: ask again after the
// New Window Retry Delay, unless a retry is already scheduled or it has had
// kMaxConstraintRetries. A delay of 0 disables retries, as it does for new windows.
static void ScheduleConstraintRetry(HWND hwnd, DWORD processId) {
  if (!hwnd || !g_settings.slowApplicationDelayMs) return;
  auto& retries = g_wm.lifecycle.constraintRetries;
  auto it = std::find_if(retries.begin(), retries.end(),
                         [&](const PendingConstraintRetry& retry) { return retry.hwnd == hwnd; });
  if (it == retries.end()) {
    retries.push_back({hwnd, processId, 0, 0});
    it = retries.end() - 1;
  } else if (it->processId != processId) {
    *it = {hwnd, processId, 0, 0};  // A reused HWND value: a new window.
  }
  if (it->dueTickMs || it->attempts >= kMaxConstraintRetries) return;
  ++it->attempts;
  it->dueTickMs = GetTickCount64() + g_settings.slowApplicationDelayMs;
  Diagnostics::RecordEvent(L"size limits unknown; retry %u scheduled hwnd=%p", it->attempts, hwnd);
  ArmConstraintRetryTimer();
}

// Runs the due retries: arranges each window's workspace, which queries it
// again. A retry the arrangement doesn't reschedule (the window answered, or
// is gone or no longer tiled) is finished.
static void ProcessConstraintRetryTimer() {
  RuntimeLifecycle::AssertWmThread(L"ProcessConstraintRetryTimer");
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.constraintRetry);
  const ULONGLONG now = GetTickCount64();
  std::vector<PendingConstraintRetry> due;
  for (PendingConstraintRetry& retry : g_wm.lifecycle.constraintRetries) {
    if (retry.dueTickMs && retry.dueTickMs <= now) {
      retry.dueTickMs = 0;
      due.push_back(retry);
    }
  }
  std::vector<DesktopMonitorKey> arranged;
  for (const PendingConstraintRetry& retry : due) {
    if (!Platform::Windows::IsSameLiveWindow(retry.hwnd, retry.processId)) continue;
    for (const DesktopMonitorKey& key : g_workspaces.OwnersOf(retry.hwnd)) {
      if (std::find(arranged.begin(), arranged.end(), key) != arranged.end()) continue;
      arranged.push_back(key);
      ArrangeWorkspace(key);
    }
  }
  std::erase_if(g_wm.lifecycle.constraintRetries,
                [](const PendingConstraintRetry& retry) { return !retry.dueTickMs; });
  ArmConstraintRetryTimer();
}

// Refreshes the application-owned native minimum contract without introducing a
// second solver notion of "proof". Normal arrangement probes only records that
// are missing, stale-DPI, or still resolving an admission/restore. User-authored
// mutations and explicit Tile Workspace operations can force a refresh so a
// relaxed same-DPI minimum cannot remain cached indefinitely.
static bool RefreshWorkspaceNativeConstraints(
    Workspace& workspace, bool forceAll, const wchar_t* reason) {
  bool changed = false;
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record ||
        Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
        Platform::Windows::IsWindowCloaked(hwnd)) {
      continue;
    }

    const HMONITOR windowMonitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
    const UINT currentDpi = windowMonitor
        ? Platform::Monitors::GetMonitorEffectiveDpi(windowMonitor)
        : 0;
    const bool needsProbe =
        forceAll || record->pendingInsertionPlacement ||
        !record->constraints.valid ||
        (currentDpi && currentDpi != record->constraints.dpi);
    if (!needsProbe) continue;

    const ConstraintProbeRefresh refresh = ProbeConstraintsAgainstCache(hwnd, record->constraints);
    changed |= workspace.UpdateConstraints(hwnd, refresh.merged);
    // No answer and nothing cached: the window is tiled without a known minimum.
    if (!refresh.merged.valid) ScheduleConstraintRetry(hwnd, record->pid);
    if (refresh.reportChanged) {
      Diagnostics::RecordEvent(
          L"native constraint report refreshed reason=%ls hwnd=%p min=%ldx%ld dpi=%u",
          reason ? reason : L"arrange", hwnd, refresh.merged.minWidth,
          refresh.merged.minHeight, refresh.merged.dpi);
    }
  }
  return changed;
}

static void RememberPendingLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& previous,
    const Workspace& candidate, HWND subject, UserLayoutMutationKind kind,
    const wchar_t* action) {
  PendingLayoutMutation mutation;
  mutation.previous = previous;
  mutation.candidate = candidate;
  mutation.subject = subject;
  mutation.kind = kind;
  mutation.action = action ? action : L"Layout mutation";
  g_pendingProofs.RememberLayoutMutation(key, std::move(mutation));
}

static void DiscardPendingAdmissionRecovery(HWND hwnd) {
  if (hwnd) g_pendingProofs.DiscardAdmissionRecovery(hwnd);
}

static void RememberPendingAdmissionRecovery(
    const DesktopMonitorKey& key, HWND hwnd, const Workspace& baseline) {
  if (!hwnd) return;
  const WindowRecord* record = baseline.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      !record->pendingInsertionPlacement) {
    return;
  }

  const PendingAdmissionRecovery* existing =
      g_pendingProofs.FindAdmissionRecovery(hwnd);
  if (existing && existing->generation == record->tilingGeneration &&
      existing->key == key) {
    // Keep the earliest pre-recovery snapshot for this unsettled admission.
    return;
  }

  PendingAdmissionRecovery recovery;
  recovery.key = key;
  recovery.baseline = baseline;
  recovery.generation = record->tilingGeneration;
  g_pendingProofs.RememberAdmissionRecovery(hwnd, std::move(recovery));
}

// If an admission/restore leaves active participation before its recovery has
// settled, derive the no-longer-active state from the pre-recovery snapshot and
// restore that established order/geometry into the live workspace. This keeps
// sibling reassignment provisional instead of leaking into the N-1 layout after
// a minimize/hidden participation race.
static bool RestorePendingAdmissionRecoveryAfterSuspension(
    const DesktopMonitorKey& key, HWND hwnd, Workspace* workspace) {
  if (!workspace || !hwnd) return false;
  std::optional<PendingAdmissionRecovery> taken =
      g_pendingProofs.TakeAdmissionRecovery(hwnd);
  if (!taken) return false;

  const PendingAdmissionRecovery recovery = std::move(*taken);
  if (recovery.key != key) return false;

  const WindowRecord* currentRecord = workspace->Find(hwnd);
  const WindowRecord* baselineRecord = recovery.baseline.Find(hwnd);
  if (!currentRecord || !baselineRecord ||
      currentRecord->state != ManageState::Suspended ||
      baselineRecord->state != ManageState::Tiled ||
      currentRecord->tilingGeneration != recovery.generation ||
      baselineRecord->tilingGeneration != recovery.generation) {
    return false;
  }

  Workspace restored = recovery.baseline;
  if (!restored.Suspend(hwnd, currentRecord->suspensionReason) ||
      !workspace->RestoreCompatibleActiveLayoutStateFrom(restored)) {
    Diagnostics::RecordEvent(
        L"pending admission recovery snapshot became incompatible hwnd=%p desktop=%08X",
        hwnd, key.desktopId.Data1);
    return false;
  }

  Diagnostics::RecordEvent(
      L"pending admission recovery rolled back after suspension hwnd=%p desktop=%08X",
      hwnd, key.desktopId.Data1);
  return true;
}

// The tiled windows (by index) a constraint conflict involves: those below their
// minimum in `rects`. In Dwindle, when no assignment of windows to slots fits,
// also every window competing for the slots an unmatched window could use, even
// one that fits where it is (it may hold the only slot another window fits).
static std::vector<bool> FindConflictingWindows(const Workspace& workspace, const std::vector<RECT>& rects) {
  const auto constraints = Layout::ActiveWindowConstraints(workspace);
  std::vector<bool> involved(constraints.size(), false);
  for (size_t i = 0; i < constraints.size(); ++i) {
    involved[i] = i < rects.size() && !Layout::WindowConstraintFitsSlot(constraints[i], rects[i]);
  }
  if (workspace.Layout() != TileLayout::Dwindle || rects.size() != constraints.size()) return involved;

  const Layout::ConstraintSlotCompatibility compatibility =
      Layout::BuildConstraintSlotCompatibility(constraints, rects, false);
  std::vector<size_t> owner(rects.size(), Layout::kUnassignedSlot);
  const std::vector<bool> fixed(constraints.size(), false);
  std::vector<size_t> unmatched;
  for (size_t window = 0; window < constraints.size(); ++window) {
    std::vector<bool> visited(rects.size(), false);
    if (!Layout::TryAssignConstraintWindow(window, compatibility, &owner, fixed, &visited, false)) {
      unmatched.push_back(window);
    }
  }
  if (unmatched.empty()) return involved;

  // Follow alternating paths from the unmatched windows to the other blockers.
  involved.assign(constraints.size(), false);
  auto include = [&](auto&& self, size_t member) -> void {
    if (member >= involved.size() || involved[member]) return;
    involved[member] = true;
    for (size_t slot : compatibility.slotsByWindow[member]) {
      if (slot < owner.size() && owner[slot] != Layout::kUnassignedSlot) self(self, owner[slot]);
    }
  };
  for (size_t window : unmatched) include(include, window);
  return involved;
}

// The name a toast uses for `hwnd`: its process name without ".exe", or empty.
static std::wstring ProcessDisplayName(HWND hwnd) {
  std::wstring name = Platform::Windows::GetWindowProcessName(hwnd);
  if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
  return name;
}

// "Firefox needs more space", "Firefox and Code need more space in this layout".
// `windows` named once each; an unnamed one is "A window".
static std::wstring NeedMoreSpaceMessage(const std::vector<HWND>& windows) {
  std::vector<std::wstring> names;
  for (HWND hwnd : windows) {
    std::wstring name = ProcessDisplayName(hwnd);
    if (name.empty()) name = L"A window";
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(std::move(name));
  }
  std::wstring message;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) message += i + 1 == names.size() ? L" and " : L", ";
    message += names[i];
  }
  if (names.empty()) message = L"A window";
  message += names.size() > 1 ? L" need more space in this layout" : L" needs more space";
  return message;
}

// NeedMoreSpaceMessage for the tiled windows `involved` marks; `fallback` is the
// window to blame when none is marked, if it can be named.
static std::wstring ConstraintConflictMessage(
    const Workspace& workspace, const std::vector<bool>& involved, HWND fallback) {
  std::vector<HWND> windows;
  for (size_t i = 0; i < involved.size(); ++i) {
    if (involved[i]) windows.push_back(workspace.TiledWindows()[i]);
  }
  if (windows.empty() && fallback && !ProcessDisplayName(fallback).empty()) windows.push_back(fallback);
  return NeedMoreSpaceMessage(windows);
}

// Tells the user which windows don't fit `rects`, the rejected plan.
static void ShowWindowConstraintConflict(
    const Workspace& workspace, const std::vector<RECT>& rects,
    HWND fallback, HMONITOR monitor) {
  const std::wstring message =
      ConstraintConflictMessage(workspace, FindConflictingWindows(workspace, rects), fallback);
  TrayUi::ShowCommandFailureFlyout(message.c_str(), monitor);
}

static void ReportRejectedLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor,
    UserLayoutMutationKind kind, const wchar_t* action, HWND subject,
    const Workspace& rejectedWorkspace, const std::vector<RECT>& rejectedRects,
    HWND culpritHint = nullptr) {
  switch (kind) {
    case UserLayoutMutationKind::Resize:
      ++Diagnostics::g_runtime.counters.rejectedResizeGestures;
      break;
    case UserLayoutMutationKind::Order:
      ++Diagnostics::g_runtime.counters.rejectedOrderCommands;
      break;
    case UserLayoutMutationKind::LayoutChange:
      ++Diagnostics::g_runtime.counters.rejectedLayoutChanges;
      break;
  }

  HWND culprit = Layout::FindConstraintCulprit(
      rejectedWorkspace, rejectedRects, subject);
  if (!culprit) culprit = culpritHint;
  ShowWindowConstraintConflict(
      rejectedWorkspace, rejectedRects, culprit, monitor);
  Wh_Log(
      L"%ls rejected by layout constraints subject=%p culprit=%p",
      action, subject, culprit);
  Diagnostics::RecordEvent(
      L"layout mutation rejected action=%ls subject=%p culprit=%p desktop=%08X",
      action, subject, culprit, key.desktopId.Data1);
}

static bool TryRollbackPendingLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor, Workspace* workspace,
    const std::vector<RECT>& rejectedRects, HWND culpritHint = nullptr) {
  if (!workspace) return false;
  const PendingLayoutMutation* pending = g_pendingProofs.FindLayoutMutation(key);
  if (!pending) return false;
  // Constraint learning and admission recovery are allowed to adjust ratios or
  // reorder pending admissions downstream of the user's command. Those are part
  // of the same delayed transaction, not evidence that the transaction is stale.
  // Only a membership/participation change makes the saved rollback snapshot
  // unsafe to apply.
  if (!workspace->HasSameWindowMembershipAs(pending->candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation rollback after membership change action=%ls desktop=%08X",
        pending->action, key.desktopId.Data1);
    g_pendingProofs.DiscardLayoutMutation(key);
    return false;
  }

  // Work from a copy: every exit below discards the stored mutation.
  const PendingLayoutMutation mutation = *pending;
  Workspace restored = *workspace;
  if (!restored.RestoreLayoutStateFrom(mutation.previous)) {
    g_pendingProofs.DiscardLayoutMutation(key);
    return false;
  }

  Platform::Monitors::WorkspaceMetrics rollbackMetrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &rollbackMetrics)) {
    g_pendingProofs.DiscardLayoutMutation(key);
    return false;
  }
  std::vector<RECT> restoredRects;
  const Layout::LayoutPlanStatus restoredStatus = Layout::BuildWorkspaceLayoutPlan(
      restored, rollbackMetrics.workArea, rollbackMetrics.gap, &restoredRects);
  if (restoredStatus != Layout::LayoutPlanStatus::Success &&
      restoredStatus != Layout::LayoutPlanStatus::NotApplicable) {
    Diagnostics::RecordEvent(
        L"layout mutation rollback no longer feasible action=%ls desktop=%08X",
        mutation.action, key.desktopId.Data1);
    g_pendingProofs.DiscardLayoutMutation(key);
    return false;
  }

  for (HWND hwnd : workspace->TiledWindows()) CancelConformanceLease(hwnd);
  if (restored.Layout() == TileLayout::Floating) {
    RestoreWorkspaceFloatingGeometry(key, restored);
  }
  ReportRejectedLayoutMutation(
      key, monitor, mutation.kind, mutation.action, mutation.subject,
      *workspace, rejectedRects, culpritHint);
  g_workspaces.Save(key, restored);
  *workspace = std::move(restored);
  g_pendingProofs.DiscardLayoutMutation(key);
  TrayUi::UpdateIcon(workspace->Layout(), monitor);
  return true;
}

static void ClearSettledLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& workspace) {
  const PendingLayoutMutation* pending = g_pendingProofs.FindLayoutMutation(key);
  if (!pending) return;
  if (!workspace.HasSameWindowMembershipAs(pending->candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation settle after membership change action=%ls desktop=%08X",
        pending->action, key.desktopId.Data1);
    g_pendingProofs.DiscardLayoutMutation(key);
    return;
  }
  if (HasOutstandingConformanceLease(workspace)) return;
  Diagnostics::RecordEvent(
      L"layout mutation settled action=%ls desktop=%08X",
      pending->action, key.desktopId.Data1);
  g_pendingProofs.DiscardLayoutMutation(key);
}

static bool FloatWindowAfterFailure(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd) {
  if (!workspace.Float(hwnd)) return false;
  FloatingPlacementHint hint;
  hint.intent = FloatingPlacementIntent::FailureCenter;
  if (!RepairFloatingGeometry(key, workspace, hwnd, hint)) {
    Diagnostics::RecordEvent(
        L"failure float geometry reset failed hwnd=%p desktop=%08X",
        hwnd, key.desktopId.Data1);
  }
  return true;
}

static size_t AutomaticWindowLimitForMonitor(const Model::MonitorId& monitor) {
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  bool monitorInfoRead = false;
  bool haveMonitorInfo = false;
  for (const auto& rule : g_settings.monitorWindowLimits) {
    if (rule.monitor == RuleMonitor::Specific) {
      if (rule.monitorId == monitor.deviceId) return rule.limit;
      continue;
    }
    if (!monitorInfoRead) {
      haveMonitorInfo = GetMonitorInfoW(monitor.Resolve(), &monitorInfo);
      monitorInfoRead = true;
    }
    if (haveMonitorInfo && ((monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0) ==
                              (rule.monitor == RuleMonitor::Primary)) return rule.limit;
  }
  return g_settings.automaticWindowLimit;
}

// True when Automatic overflow routes a window it releases to another monitor or
// a new desktop (it floats in place meanwhile). False: it floats the window and
// centres it (FloatWindowAfterFailure). The companion snapshot publishes this.
static bool AutomaticOverflowRoutes() {
  // With exactly one display, monitor-only routing has no destination. The
  // inexpensive count query also keeps the publisher current after hot-plug.
  // A zero/unknown count retains routing rather than promising a float hint.
  return RuntimeLifecycle::IsAutomaticMode() && Platform::WindowEvents::g_hooks.hideDestroy &&
         (g_settings.overflowCreateDesktop ||
          (g_settings.overflowTryOtherMonitors && GetSystemMetrics(SM_CMONITORS) != 1));
}

// Why Automatic overflow releases a window (FloatAutomaticOverflowWindow's
// `reason`). Compared by address.
inline constexpr const wchar_t* kOverflowCountLimit = L"count limit";  // Past the Auto-tiling Limit.
inline constexpr const wchar_t* kOverflowCannotFit = L"cannot fit";    // No room at its size limits.

// Arrangement releases the slot first; physical routing runs after every source
// workspace in the burst has been saved, so discovery snapshots cannot go stale
// midway through admission. A failed route therefore already has a safe fallback.
static bool FloatAutomaticOverflowWindow(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const wchar_t* reason) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record) return false;
  const DWORD processId = record->pid;
  const bool monitorFixed = record->automaticOverflowMonitorFixed;
  const uint64_t generation = record->tilingGeneration;
  const bool route = AutomaticOverflowRoutes();
  CancelConformanceLease(hwnd);
  DiscardPendingAdmissionRecovery(hwnd);
  if (!(route ? workspace.Float(hwnd) : FloatWindowAfterFailure(key, workspace, hwnd))) return false;
  if (route) {
    g_wm.overflow.windows.insert(hwnd);
    g_wm.overflow.queue.push_back(
        {hwnd, processId, key.desktopId, key.monitor.deviceId, monitorFixed, generation});
    Reconcile::ScheduleLifecycleReconcile(nullptr);
  }
  Diagnostics::RecordEvent(L"automatic overflow hwnd=%p reason=%ls route=%d", hwnd, reason, route ? 1 : 0);
  if (g_wm.commandReleases) g_wm.commandReleases->emplace_back(hwnd, reason);
  return true;
}

// A window that holds one of a workspace's automatic-limit slots.
struct LimitOccupant {
  HWND hwnd;
  DWORD processId;
  uint64_t generation;  // Arrival order: older windows keep their slots first.
  // Not a member yet: a maximized window awaiting initial placement on this
  // monitor. It only reserves its place in arrival order.
  bool reservedForInitialPlacement;
};

// Everything that counts against the automatic window limit, in arrival order:
// visible tiled members, visible maximized members that keep a slot, and
// maximized windows still awaiting initial placement here. Minimized, hidden and
// cloaked members don't count. EnforceAutomaticWindowLimit floats the occupants
// past the limit; the companion snapshot uses the same list to tell whether a
// new window would overflow, so the two can never disagree.
// Reads only; native queries, including one desktop COM call per pending
// window, which can deliver lifecycle events. WM thread.
static std::vector<LimitOccupant> CollectAutomaticLimitOccupants(
    const DesktopMonitorKey& key, const Workspace& workspace, HMONITOR monitor) {
  std::vector<LimitOccupant> occupants;
  for (const auto& member : workspace.Records()) {
    const WindowRecord& record = member.second;
    if (record.state == ManageState::Tiled ||
        (record.state == ManageState::Suspended && record.hasSavedSlot &&
         record.suspensionReason == SuspensionReason::Maximized)) {
      const SuspensionReason physicalReason = Platform::Windows::GetPhysicalSuspensionReason(record.hwnd);
      if (physicalReason == SuspensionReason::Minimized ||
          physicalReason == SuspensionReason::Hidden || Platform::Windows::IsWindowCloaked(record.hwnd)) continue;
      occupants.push_back({record.hwnd, record.pid, record.tilingGeneration, false});
    }
  }
  // Copy pending identities before desktop queries, which can deliver lifecycle events.
  std::vector<LimitOccupant> pendingWindows;
  for (const auto& pending : g_wm.routing.pending) {
    const auto& request = pending.second;
    if (request.visibleGeneration && IsEqualGUID(request.sourceDesktop, key.desktopId)) {
      pendingWindows.push_back({pending.first, request.processId, request.visibleGeneration, true});
    }
  }
  for (const LimitOccupant& candidate : pendingWindows) {
    if (!Platform::VirtualDesktop::IsWindowOnDesktop(candidate.hwnd, key.desktopId)) continue;
    if (!Platform::Windows::IsSameLiveWindow(candidate.hwnd, candidate.processId) ||
        Platform::Windows::IsWindowTrackedInAnyState(candidate.hwnd) || Platform::Windows::IsWindowCloaked(candidate.hwnd) ||
        Platform::Windows::GetPhysicalSuspensionReason(candidate.hwnd) != SuspensionReason::Maximized ||
        Platform::Windows::GetWindowPhysicalMonitor(candidate.hwnd) != monitor ||
        Platform::Windows::ShouldWindowStartFloating(candidate.hwnd)) continue;
    // Recheck after the queries above: the request may have been replaced meanwhile.
    const auto pending = g_wm.routing.pending.find(candidate.hwnd);
    if (pending == g_wm.routing.pending.end() || pending->second.processId != candidate.processId ||
        pending->second.visibleGeneration != candidate.generation) continue;
    occupants.push_back(candidate);
  }
  std::sort(occupants.begin(), occupants.end(),
            [](const LimitOccupant& first, const LimitOccupant& second) {
              return first.generation < second.generation;
            });
  return occupants;
}

// Counts visible arrivals in generation order, including maximized members
// awaiting initial placement, and releases excess slots through overflow policy.
// WM thread only; the caller saves the resulting workspace before routing it.
static void EnforceAutomaticWindowLimit(
    const DesktopMonitorKey& key, Workspace& workspace, HMONITOR monitor) {
  if (!RuntimeLifecycle::IsAutomaticMode() || workspace.Layout() == TileLayout::Floating) return;
  const size_t limit = AutomaticWindowLimitForMonitor(key.monitor);
  if (!limit) return;
  size_t retainedSlots = 0;
  for (const LimitOccupant& occupant : CollectAutomaticLimitOccupants(key, workspace, monitor)) {
    // A reserved place counts; restoration still performs initial placement.
    if (occupant.reservedForInitialPlacement) {
      ++retainedSlots;
      continue;
    }
    const bool exceedsLimit = retainedSlots >= limit;
    ++retainedSlots;
    // Visible maximized members count, but are not restored just to move them.
    if (exceedsLimit && !Gestures::IsMoveSizeGestureInProgress(occupant.hwnd) &&
        FloatAutomaticOverflowWindow(key, workspace, occupant.hwnd, kOverflowCountLimit)) {
      --retainedSlots;
    }
  }
}

// True when Automatic admission of one more window into `workspace` would
// overflow its limit: the newest arrival is the one released. Same counting as
// EnforceAutomaticWindowLimit. Native queries; WM thread.
static bool WouldExceedAutomaticWindowLimit(
    const DesktopMonitorKey& key, const Workspace& workspace, HMONITOR monitor) {
  if (workspace.Layout() == TileLayout::Floating) return false;
  const size_t limit = AutomaticWindowLimitForMonitor(key.monitor);
  return limit && CollectAutomaticLimitOccupants(key, workspace, monitor).size() >= limit;
}

// References stay valid for the enclosing ArrangeWorkspace call and are never
// retained. Stages share its local snapshot and emit conflict feedback at most once.
struct ArrangementContext {
  const DesktopMonitorKey& key;
  HMONITOR monitor;
  const Platform::Monitors::WorkspaceMetrics& metrics;
  HWND preferredTiledWindow;
  bool constraintFailureReported = false;
};

enum class ArrangementStep { Replan, Finished };
enum class ConstraintRemovalReason {
  IntrinsicMinimum, PendingAdmission, MinimumGrowth, SingleRemoval, AggregateConflict
};

// WM thread only. The snapshot contains victim and its matching record. Records
// diagnostic values without changing membership or retaining input references.
static void RecordConstraintRemoval(
    const Workspace& workspace, const std::vector<RECT>& rects,
    const std::vector<WindowConstraints>& activeConstraints,
    const WindowRecord& victimRecord, HWND victim,
    ConstraintRemovalReason reason, HWND preferredTiledWindow) {
  const uint64_t generation = victimRecord.tilingGeneration;
  const WindowConstraints constraints = victimRecord.constraints;
  if (reason == ConstraintRemovalReason::IntrinsicMinimum) {
    Wh_Log(
      L"Native minimum exceeds workspace; floating HWND %p (generation=%llu)",
      static_cast<void*>(victim),
      static_cast<unsigned long long>(generation));
  } else if (reason == ConstraintRemovalReason::PendingAdmission) {
    Wh_Log(
        L"Constraint conflict; rejecting pending admission HWND %p (generation=%llu)",
        static_cast<void*>(victim),
        static_cast<unsigned long long>(generation));
    Diagnostics::RecordEvent(
        L"pending admission rejected hwnd=%p generation=%llu",
        victim, static_cast<unsigned long long>(generation));
  } else if (reason == ConstraintRemovalReason::MinimumGrowth) {
    Wh_Log(
        L"Constraint conflict after raised native minimum; floating HWND %p (generation=%llu)",
        static_cast<void*>(victim),
        static_cast<unsigned long long>(generation));
  } else if (reason == ConstraintRemovalReason::SingleRemoval) {
    Wh_Log(
        L"Current-order constraint conflict; floating single-removal candidate HWND %p (generation=%llu)",
        static_cast<void*>(victim),
        static_cast<unsigned long long>(generation));
  } else {
    Wh_Log(
        L"Constraint conflict; floating fallback HWND %p (generation=%llu)",
        static_cast<void*>(victim),
        static_cast<unsigned long long>(generation));
  }
  Diagnostics::RecordEvent(
      L"native constraints floated hwnd=%p generation=%llu intrinsic=%d singleRemovalCurrentOrder=%d minimumGrowth=%d preferred=%d min=%ldx%ld",
      victim, static_cast<unsigned long long>(generation),
      reason == ConstraintRemovalReason::IntrinsicMinimum ? 1 : 0,
      reason == ConstraintRemovalReason::SingleRemoval ||
          reason == ConstraintRemovalReason::MinimumGrowth ? 1 : 0,
      reason == ConstraintRemovalReason::MinimumGrowth ? 1 : 0,
      victim == preferredTiledWindow ? 1 : 0,
      constraints.minWidth, constraints.minHeight);

  for (size_t i = 0;
       i < activeConstraints.size() &&
       i < workspace.TiledWindows().size();
       ++i) {
    const WindowConstraints& memberConstraint = activeConstraints[i];
    if (!memberConstraint.valid) continue;
    size_t compatibleSlotCount = 0;
    for (const RECT& slot : rects) {
      if (Layout::WindowConstraintFitsSlot(memberConstraint, slot)) {
        ++compatibleSlotCount;
      }
    }
    Diagnostics::RecordEvent(
        L"constraint conflict member hwnd=%p slot=%zu min=%ldx%ld compatibleSlots=%zu",
        workspace.TiledWindows()[i], i,
        memberConstraint.minWidth, memberConstraint.minHeight,
        compatibleSlotCount);
  }
}

// --- Recovering from a layout that cannot fit --------------------------------

// The window to float so the rest can fit, and why it was chosen.
struct ConstraintVictim {
  HWND hwnd = nullptr;
  ConstraintRemovalReason reason = ConstraintRemovalReason::AggregateConflict;
};

// Windows being dragged are never removed in Automatic mode.
static bool IsExemptFromRemoval(HWND hwnd) {
  return RuntimeLifecycle::IsAutomaticMode() && Gestures::IsMoveSizeGestureInProgress(hwnd);
}

// A tiled window whose minimum size is larger than the whole work area can never
// fit, so removing other windows cannot help. Returns the newest such window.
static HWND FindWindowLargerThanWorkArea(
    const Workspace& workspace, const std::vector<WindowConstraints>& activeConstraints, const RECT& workArea) {
  const LONG workspaceWidth = Layout::RectWidth(workArea);
  const LONG workspaceHeight = Layout::RectHeight(workArea);
  HWND victim = nullptr;
  uint64_t newestImpossibleGeneration = 0;
  for (size_t i = 0; i < workspace.TiledWindows().size() && i < activeConstraints.size(); ++i) {
    const WindowConstraints& constraint = activeConstraints[i];
    if (!constraint.valid) continue;
    const bool impossible = std::max<LONG>(1, constraint.minWidth) > workspaceWidth ||
                            std::max<LONG>(1, constraint.minHeight) > workspaceHeight;
    if (!impossible) continue;
    const HWND candidate = workspace.TiledWindows()[i];
    if (IsExemptFromRemoval(candidate)) continue;
    const WindowRecord* candidateRecord = workspace.Find(candidate);
    const uint64_t generation = candidateRecord ? candidateRecord->tilingGeneration : 0;
    if (!victim || generation >= newestImpossibleGeneration) {
      victim = candidate;
      newestImpossibleGeneration = generation;
    }
  }
  return victim;
}

// Tiled windows that may be removed, newest layout demand (admission,
// restoration or raised minimum) first, with the window the user just acted on
// (`preferredTiledWindow`) last.
static std::vector<Layout::ConstraintCandidate> OrderRemovalCandidates(
    const Workspace& workspace, HWND preferredTiledWindow) {
  std::vector<Layout::ConstraintCandidate> candidates;
  candidates.reserve(workspace.ActiveCount());
  for (HWND candidate : workspace.TiledWindows()) {
    if (IsExemptFromRemoval(candidate)) continue;
    const WindowRecord* candidateRecord = workspace.Find(candidate);
    candidates.push_back({candidate, candidateRecord ? Model::LayoutDemandRecency(*candidateRecord) : 0});
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [&](const Layout::ConstraintCandidate& first, const Layout::ConstraintCandidate& second) {
                     const bool firstPreferred = first.hwnd == preferredTiledWindow;
                     const bool secondPreferred = second.hwnd == preferredTiledWindow;
                     if (firstPreferred != secondPreferred) return !firstPreferred;
                     return first.demandGeneration > second.demandGeneration;
                   });
  return candidates;
}

// Returns the still-settling admission that introduced this conflict, or null.
// That is the newest pending admission, unless a settled sibling raised its
// minimum after it (the likelier cause), and only if the established windows
// fit once every removable pending admission is gone. Otherwise the conflict is
// among settled windows (a sibling left, or a minimum grew), and an unrelated
// pending window that merely has not settled yet is not blamed.
static HWND FindIntroducingPendingAdmission(
    const Workspace& workspace, const std::vector<Layout::ConstraintCandidate>& candidates,
    const RECT& workArea, LONG gap) {
  const Layout::ConstraintCandidate* newest = nullptr;
  uint64_t newestSettledGrowth = 0;
  for (const Layout::ConstraintCandidate& candidate : candidates) {
    const WindowRecord* record = workspace.Find(candidate.hwnd);
    if (!record) continue;
    if (record->pendingInsertionPlacement) {
      if (!newest || candidate.demandGeneration > newest->demandGeneration) newest = &candidate;
    } else if (Model::MinimumGrewSinceAdmission(*record)) {
      newestSettledGrowth = std::max(newestSettledGrowth, record->minimumGrowthGeneration);
    }
  }
  if (!newest || newestSettledGrowth > newest->demandGeneration) return nullptr;

  Workspace established = workspace;
  for (const Layout::ConstraintCandidate& candidate : candidates) {
    const WindowRecord* record = workspace.Find(candidate.hwnd);
    if (record && record->pendingInsertionPlacement) established.Float(candidate.hwnd);
  }
  std::vector<RECT> rects;
  return Layout::BuildWorkspaceLayoutPlan(established, workArea, gap, &rects) ==
          Layout::LayoutPlanStatus::Unsatisfiable
      ? nullptr
      : newest->hwnd;
}

// Picks the window to float once no recovery keeps everyone, in this order:
//  1. `introducingAdmission` (see FindIntroducingPendingAdmission): it caused
//     the failure, so it goes, never an already-settled sibling.
//  2. The newest-demand candidate whose removal alone makes the current order
//     fit, so a window that just raised its minimum goes before the siblings
//     it crowded out.
//  3. Automatic mode only: the first candidate, even if more removals will be
//     needed (each pass removes one and replans).
// No value of `hwnd` means: leave membership unchanged.
static ConstraintVictim ChooseConstraintVictim(
    const Workspace& workspace, const std::vector<Layout::ConstraintCandidate>& candidates,
    HWND introducingAdmission, const RECT& workArea, LONG gap) {
  if (introducingAdmission) {
    return {introducingAdmission, ConstraintRemovalReason::PendingAdmission};
  }
  for (const Layout::ConstraintCandidate& candidate : candidates) {
    Workspace trial = workspace;
    if (!trial.Float(candidate.hwnd)) continue;
    std::vector<RECT> trialRects;
    if (Layout::BuildWorkspaceLayoutPlan(trial, workArea, gap, &trialRects) == Layout::LayoutPlanStatus::Success) {
      const WindowRecord* record = workspace.Find(candidate.hwnd);
      const bool grew = record && !record->pendingInsertionPlacement &&
          Model::MinimumGrewSinceAdmission(*record);
      return {candidate.hwnd, record && record->pendingInsertionPlacement
                                  ? ConstraintRemovalReason::PendingAdmission
                                  : grew ? ConstraintRemovalReason::MinimumGrowth
                                         : ConstraintRemovalReason::SingleRemoval};
    }
  }
  if (RuntimeLifecycle::IsAutomaticMode() && !candidates.empty()) {
    return {candidates.front().hwnd, ConstraintRemovalReason::AggregateConflict};
  }
  return {};
}

// Commits a successful admission recovery (a pending window moved to a slot it
// fits, possibly reassigning siblings). The pre-recovery order is kept until the
// window settles, so it can be restored if a race strands its siblings.
static void ApplyAdmissionRecovery(
    const DesktopMonitorKey& key, Workspace& workspace, const Workspace& baseline,
    Layout::AdmissionRecoveryPlan& recovery) {
  RememberPendingAdmissionRecovery(key, recovery.window, baseline);
  workspace = std::move(recovery.workspace);
  if (recovery.reassignedWindows > 0) {
    Wh_Log(L"Dwindle admission reassigned %zu windows across compatible slots for HWND %p",
           recovery.reassignedWindows, static_cast<void*>(recovery.window));
    Diagnostics::RecordEvent(L"dwindle admission matched all constraints hwnd=%p slot=%zu reassigned=%zu",
                             recovery.window, recovery.toIndex, recovery.reassignedWindows);
  }
  if (recovery.fromIndex != recovery.toIndex) {
    Wh_Log(L"Constraint-aware admission repositioned HWND %p from slot %zu to %zu",
           static_cast<void*>(recovery.window), recovery.fromIndex, recovery.toIndex);
    Diagnostics::RecordEvent(L"constraint-aware admission repositioned hwnd=%p from=%zu to=%zu",
                             recovery.window, recovery.fromIndex, recovery.toIndex);
  }
  g_workspaces.Save(key, workspace);
}

// Floats the chosen window (Automatic mode routes it through overflow), tells
// the user once per arrangement in Manual mode, and saves.
static ArrangementStep ReleaseConstraintVictim(
    Workspace& workspace, ArrangementContext& context, const std::vector<RECT>& rects,
    const std::vector<WindowConstraints>& activeConstraints, const ConstraintVictim& victim,
    const WindowRecord& victimRecord) {
  const auto& key = context.key;
  RecordConstraintRemoval(workspace, rects, activeConstraints, victimRecord, victim.hwnd,
                          victim.reason, context.preferredTiledWindow);
  CancelConformanceLease(victim.hwnd);
  const HWND culprit = Layout::FindConstraintCulprit(workspace, rects, victim.hwnd);
  const bool automaticOverflow = RuntimeLifecycle::IsAutomaticMode();
  if (!context.constraintFailureReported && !automaticOverflow) {
    ShowWindowConstraintConflict(workspace, rects, culprit ? culprit : victim.hwnd, context.monitor);
    context.constraintFailureReported = true;
  }
  DiscardPendingAdmissionRecovery(victim.hwnd);
  const bool floated = automaticOverflow
      ? FloatAutomaticOverflowWindow(key, workspace, victim.hwnd, kOverflowCannotFit)
      : FloatWindowAfterFailure(key, workspace, victim.hwnd);
  if (!floated) {
    g_workspaces.Save(key, workspace);
    return ArrangementStep::Finished;
  }
  ++Diagnostics::g_runtime.counters.constraintFloats;
  g_workspaces.Save(key, workspace);
  return workspace.Empty() ? ArrangementStep::Finished : ArrangementStep::Replan;
}

// Handles one failed plan, trying in order: roll back a provisional user change;
// float a window that can never fit; move a pending admission to a slot it fits;
// unless a pending admission introduced the conflict, reorder the existing
// windows so all of them fit; float one window chosen by ChooseConstraintVictim. Saves are explicit; Replan
// means the next arrangement pass must plan again from the resulting workspace.
// WM thread only; rects are physical screen pixels from the rejected plan.
static ArrangementStep RecoverUnsatisfiableLayout(
    Workspace& workspace, ArrangementContext& context, const std::vector<RECT>& rects) {
  const auto& key = context.key;
  const RECT& workArea = context.metrics.workArea;
  const LONG gap = context.metrics.gap;
  ++Diagnostics::g_runtime.counters.constraintUnsatisfiablePlans;

  // An explicit order mutation has already passed a full native-constraint
  // preflight before commit. If it later becomes unsatisfiable, do not
  // reinterpret the command by reshaping/reassigning it again here. The
  // delayed transaction gets first refusal: restore the previous order and
  // report the same constraint toast. Only when that saved order is no
  // longer feasible either may ordinary global constraint recovery proceed.
  if (TryRollbackPendingLayoutMutation(key, context.monitor, &workspace, rects)) {
    return ArrangementStep::Replan;
  }

  // Keep order changes provisional until admission conformance settles.
  const Workspace pendingAdmissionRecoveryBaseline = workspace;
  const std::vector<WindowConstraints> activeConstraints = Layout::ActiveWindowConstraints(workspace);

  ConstraintVictim victim{FindWindowLargerThanWorkArea(workspace, activeConstraints, workArea),
                          ConstraintRemovalReason::IntrinsicMinimum};
  if (!victim.hwnd) {
    const auto candidates = OrderRemovalCandidates(workspace, context.preferredTiledWindow);
    Layout::AdmissionRecoveryPlan recovery;
    if (Layout::TryRecoverPendingAdmission(
            workspace, workArea, gap, candidates, context.preferredTiledWindow, &recovery)) {
      ApplyAdmissionRecovery(key, workspace, pendingAdmissionRecoveryBaseline, recovery);
      return ArrangementStep::Replan;
    }
    const HWND introducingAdmission =
        FindIntroducingPendingAdmission(workspace, candidates, workArea, gap);
    // Not a newcomer's fault: reorder the windows already here rather than float
    // one that fitted. A newcomer that cannot fit is still rejected instead.
    Workspace reordered;
    size_t movedWindowCount = 0;
    if (!introducingAdmission &&
        Layout::TryReorderForConstraints(
            workspace, workArea, gap, context.preferredTiledWindow, &reordered, &movedWindowCount)) {
      Wh_Log(L"Constraint conflict resolved by reordering %zu windows", movedWindowCount);
      Diagnostics::RecordEvent(L"constraint conflict reordered windows moved=%zu desktop=%08X",
                               movedWindowCount, key.desktopId.Data1);
      workspace = std::move(reordered);
      g_workspaces.Save(key, workspace);
      return ArrangementStep::Replan;
    }
    victim = ChooseConstraintVictim(workspace, candidates, introducingAdmission, workArea, gap);
  }

  const WindowRecord* victimRecord = victim.hwnd ? workspace.Find(victim.hwnd) : nullptr;
  if (!victimRecord) {
    // No removal can make this layout fit: keep everyone, tell the user once.
    const HWND culprit = Layout::FindConstraintCulprit(workspace, rects, context.preferredTiledWindow);
    if (!context.constraintFailureReported) {
      ShowWindowConstraintConflict(workspace, rects, culprit, context.monitor);
      context.constraintFailureReported = true;
    }
    Diagnostics::RecordEvent(
        L"constraint conflict left membership unchanged; no single settled removal is sufficient desktop=%08X",
        key.desktopId.Data1);
    g_workspaces.Save(key, workspace);
    return ArrangementStep::Finished;
  }
  return ReleaseConstraintVictim(workspace, context, rects, activeConstraints, victim, *victimRecord);
}
// What placing one tiled window means for the rest of the arrangement pass.
struct TiledPlacementOutcome {
  // What the model must do with the window once the pass has placed every one.
  Workspace::PlacementAction action = Workspace::PlacementAction::None;
  // Stop the pass and plan again: the window reported a new native minimum, or
  // a pending user layout change was rolled back (which replaced `workspace`).
  enum class Stop { No, NativeConstraints, LayoutRollback } stop = Stop::No;
};

// Places tiled window `hwnd` at `target` and starts, keeps or cancels its
// conformance lease. A window that is gone or has no record is to be forgotten;
// a minimized, maximized, hidden or cloaked one is left where it is. An app that
// corrects a legal target is re-probed in case its own WM_GETMINMAXINFO contract
// changed; an unchanged report leaves the correction to conformance handling
// rather than inventing a second, empirical minimum-size policy. With leases
// disabled, a refused placement may roll back a pending user layout change.
// WM thread only; `rects` is the whole plan, physical screen pixels.
static TiledPlacementOutcome PlaceTiledWindow(
    Workspace& workspace, const ArrangementContext& context, const std::vector<RECT>& rects,
    HWND hwnd, const RECT& target) {
  using Stop = TiledPlacementOutcome::Stop;
  const WindowRecord* record = IsWindow(hwnd) ? workspace.Find(hwnd) : nullptr;
  if (!record) return {Workspace::PlacementAction::Forget};
  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::IsWindowCloaked(hwnd)) {
    return {};
  }

  const PlacementObservation observation =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, target);
  TiledPlacementOutcome outcome{workspace.ApplyPlacementObservation(hwnd, observation)};
  if (observation.result == PlacementResult::AdjustedByWindow) {
    const ConstraintProbeRefresh refresh = ProbeConstraintsAgainstCache(hwnd, record->constraints);
    workspace.UpdateConstraints(hwnd, refresh.merged);
    if (refresh.reportChanged) {
      CancelConformanceLease(hwnd);
      Diagnostics::RecordEvent(
          L"placement refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
          hwnd, refresh.merged.minWidth, refresh.merged.minHeight, refresh.merged.dpi);
      return {Workspace::PlacementAction::None, Stop::NativeConstraints};
    }
  }

  if (outcome.action != Workspace::PlacementAction::None) {
    CancelConformanceLease(hwnd);
    return outcome;
  }
  switch (observation.result) {
    case PlacementResult::Success:
      if (observation.placementIssued) BeginConformanceLease(hwnd, target, false, context.preferredTiledWindow);
      break;
    case PlacementResult::AdjustedByWindow:
    case PlacementResult::Refused:
      BeginConformanceLease(hwnd, target, true, context.preferredTiledWindow);
      if (g_settings.conformanceLeaseMs == 0 &&
          TryRollbackPendingLayoutMutation(context.key, context.monitor, &workspace, rects, hwnd)) {
        outcome.stop = Stop::LayoutRollback;
      }
      break;
    case PlacementResult::SuppressedByPhysicalState:
    case PlacementResult::AccessDenied:
    case PlacementResult::Dead:
      break;
  }
  return outcome;
}

// Places current targets, observes the application's response, then commits
// membership changes. A native constraint refresh or rollback requests another
// pass. Replan after rollback deliberately does not save the rejected candidate.
// WM thread only; rects are physical screen pixels in active-window order.
static ArrangementStep ApplyWorkspacePlacements(
    Workspace& workspace, const ArrangementContext& context, const std::vector<RECT>& rects) {
  using Stop = TiledPlacementOutcome::Stop;
  const auto& key = context.key;
  std::vector<HWND> toFloat;
  std::vector<HWND> toForget;
  const std::vector<HWND> active = workspace.TiledWindows();
  const size_t count = std::min(active.size(), rects.size());
  for (size_t i = 0; i < count; ++i) {
    const TiledPlacementOutcome outcome = PlaceTiledWindow(workspace, context, rects, active[i], rects[i]);
    if (outcome.stop == Stop::LayoutRollback) return ArrangementStep::Replan;
    if (outcome.stop == Stop::NativeConstraints) {
      g_workspaces.Save(key, workspace);
      return ArrangementStep::Replan;
    }
    if (outcome.action == Workspace::PlacementAction::Float) toFloat.push_back(active[i]);
    if (outcome.action == Workspace::PlacementAction::Forget) toForget.push_back(active[i]);
  }

  bool participationChanged = false;
  for (HWND hwnd : toForget) {
    CancelConformanceLease(hwnd);
    participationChanged |= workspace.Forget(hwnd);
  }
  for (HWND hwnd : toFloat) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record || record->state != ManageState::Tiled) continue;
    Wh_Log(
        L"Window %p cannot be placed (result=%d); switching it to Floating",
        hwnd, static_cast<int>(record->lastPlacementResult));
    Diagnostics::RecordEvent(
        L"placement failure forced hwnd=%p to per-window Floating (result=%d)",
        hwnd, static_cast<int>(record->lastPlacementResult));
    CancelConformanceLease(hwnd);
    participationChanged |= FloatWindowAfterFailure(key, workspace, hwnd);
  }

  if (!participationChanged) {
    ClearSettledInsertionPlacements(key, workspace);
  }
  g_workspaces.Save(key, workspace);
  if (!participationChanged || workspace.Empty()) {
    ClearSettledLayoutMutation(key, workspace);
    return ArrangementStep::Finished;
  }
  return ArrangementStep::Replan;
}

// Renders one workspace from authoritative model state. Arrangement itself is a
// small pipeline: repair local invariants -> compute targets -> apply placement ->
// fold placement outcomes back into the model. Reflow is iterative: a pass may
// refresh a native constraint or select one constraint-removal candidate.
// WM thread only. Commits snapshots and native placement; a failed pass may have
// repaired ownership/geometry or released a member before the next replan.
static void ArrangeWorkspace(
    const DesktopMonitorKey& key, HWND preferredTiledWindow) {
  ++Diagnostics::g_runtime.counters.arrangeCalls;
  if (!IsWorkspaceOnActiveDesktop(key)) return;

  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace)) return;

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return;  // Detached monitor: keep workspace state dormant.

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return;
  const RECT& workArea = metrics.workArea;
  const LONG gap = metrics.gap;

  ObserveAndRepairWorkspaceForArrangement(workspace);
  EnforceAutomaticWindowLimit(key, workspace, monitor);
  if (const PendingLayoutMutation* pending = g_pendingProofs.FindLayoutMutation(key);
      pending && !workspace.HasSameWindowMembershipAs(pending->candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation before arrangement after membership change action=%ls desktop=%08X",
        pending->action, key.desktopId.Data1);
    g_pendingProofs.DiscardLayoutMutation(key);
  }
  // Native reports are cached between arrangements. The first pass below probes
  // missing/stale reports and pending admissions/restores before geometry is solved.
  const size_t maxPasses = workspace.ActiveCount() * 5 + 8;
  ArrangementContext context{key, monitor, metrics, preferredTiledWindow};
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    ++Diagnostics::g_runtime.counters.arrangePasses;
    if (workspace.Layout() == TileLayout::Floating || workspace.Empty()) {
      if (workspace.Layout() == TileLayout::Floating) {
        for (HWND hwnd : workspace.TiledWindows()) {
          CancelTiledConformanceLease(hwnd);
          workspace.ClearInsertionPlacement(hwnd);
        }
      }
      g_workspaces.Save(key, workspace);
      ClearSettledLayoutMutation(key, workspace);
      return;
    }

    if (pass == 0) {
      RefreshWorkspaceNativeConstraints(workspace, false, L"arrange");
    }

    std::vector<RECT> rects;
    Layout::LayoutPlanStatus planStatus =
        Layout::BuildWorkspaceLayoutPlan(workspace, workArea, gap, &rects);
    if (planStatus == Layout::LayoutPlanStatus::NotApplicable) {
      g_workspaces.Save(key, workspace);
      return;
    }
    if (planStatus == Layout::LayoutPlanStatus::Unsatisfiable) {
      if (RecoverUnsatisfiableLayout(workspace, context, rects) == ArrangementStep::Finished) return;
      continue;
    }

    if (ApplyWorkspacePlacements(workspace, context, rects) == ArrangementStep::Finished) return;
  }

  Diagnostics::RecordEvent(
      L"arrangement safety bound exhausted desktop=%08X active=%zu",
      key.desktopId.Data1, workspace.ActiveCount());
  g_workspaces.Save(key, workspace);
  ClearSettledLayoutMutation(key, workspace);
}

} // namespace Placement
