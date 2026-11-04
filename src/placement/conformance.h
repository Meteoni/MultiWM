// Placement leases, repair cadence and final conformance checks.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Placement {

struct ConformanceWorkspace {
  DesktopMonitorKey key{};
  Workspace workspace;
};

enum class ConformanceConstraintRefresh { Stale, Unchanged, Changed };

// Reads lease state without consuming expiry. Expired leases stay present until
// the WM actor performs the final passive conformance verdict; WinEvent
// observations must never silently erase the evidence needed for that action.
static ConformanceLeaseReadResult ReadConformanceLease(
    HWND hwnd, ConformanceLease* outLease) {
  RuntimeLifecycle::AssertWmThread(L"ReadConformanceLease");
  if (!hwnd) return ConformanceLeaseReadResult::Missing;

  const ULONGLONG now = GetTickCount64();
  ConformanceLeaseReadResult result = ConformanceLeaseReadResult::Missing;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end()) {
    if (outLease) *outLease = it->second;
    result = now < it->second.expiresAtTickMs
                 ? ConformanceLeaseReadResult::Active
                 : ConformanceLeaseReadResult::Expired;
  }
  return result;
}

static bool ReadActiveConformanceLease(HWND hwnd, ConformanceLease* outLease) {
  if (!outLease) return false;
  return ReadConformanceLease(hwnd, outLease) ==
         ConformanceLeaseReadResult::Active;
}

static bool HasActiveConformanceLease(HWND hwnd) {
  ConformanceLease ignored;
  return ReadActiveConformanceLease(hwnd, &ignored);
}

// generation==0 means an unconditional cancellation (explicit user intent,
// topology/lifecycle teardown, migration, etc.). A nonzero generation lets a
// stale WM-thread observation avoid deleting a newer lease.
static bool CancelConformanceLease(HWND hwnd, uint64_t generation) {
  RuntimeLifecycle::AssertWmThread(L"CancelConformanceLease");
  if (!hwnd) return false;
  bool erased = false;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      (generation == 0 || it->second.generation == generation)) {
    g_conformanceLeases.leases.erase(it);
    erased = true;
  }
  return erased;
}

static void ClearAllConformanceLeases() {
  RuntimeLifecycle::AssertWmThread(L"ClearAllConformanceLeases");
  g_conformanceLeases.leases.clear();
  g_pendingLayoutMutations.clear();
  g_pendingAdmissionRecoveries.clear();
}

static size_t CountActiveConformanceLeases() {
  RuntimeLifecycle::AssertWmThread(L"CountActiveConformanceLeases");
  const ULONGLONG now = GetTickCount64();
  size_t count = 0;
  for (const auto& kv : g_conformanceLeases.leases) {
    if (now < kv.second.expiresAtTickMs) ++count;
  }
  return count;
}

static void ScheduleNextConformanceTimer() {
  RuntimeLifecycle::AssertWmThread(L"ScheduleNextConformanceTimer");
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }

  const ULONGLONG now = GetTickCount64();
  ULONGLONG earliestDue = 0;
  for (const auto& kv : g_conformanceLeases.leases) {
    const ConformanceLease& lease = kv.second;
    ULONGLONG due = lease.expiresAtTickMs;
    if (lease.repairDueTickMs && lease.repairDueTickMs < due) {
      due = lease.repairDueTickMs;
    }
    if (!earliestDue || due < earliestDue) earliestDue = due;
  }
  if (!earliestDue) return;
  const ULONGLONG remaining = earliestDue > now ? earliestDue - now : 1;
  const UINT delayMs = static_cast<UINT>(std::min<ULONGLONG>(remaining, 0x7FFFFFFFULL));
  g_wm.conformanceTimer = SetTimer(nullptr, 0, std::max<UINT>(1, delayMs), nullptr);
  if (!g_wm.conformanceTimer) {
    Wh_Log(L"Failed to schedule conformance repair/deadline timer");
  }
}

static bool DeferConformanceLeaseRepair(
    HWND hwnd, uint64_t generation, ULONGLONG dueTickMs) {
  RuntimeLifecycle::AssertWmThread(L"DeferConformanceLeaseRepair");
  if (!hwnd || generation == 0 || !dueTickMs) return false;

  const ULONGLONG now = GetTickCount64();
  bool retained = false;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      now < it->second.expiresAtTickMs &&
      it->second.generation == generation) {
    if (!it->second.repairDueTickMs || dueTickMs < it->second.repairDueTickMs) {
      it->second.repairDueTickMs = dueTickMs;
    }
    retained = true;
  }
  if (retained) ScheduleNextConformanceTimer();
  return retained;
}

static uint64_t AllocateConformanceLeaseGeneration() {
  RuntimeLifecycle::AssertWmThread(L"AllocateConformanceLeaseGeneration");
  uint64_t generation = g_conformanceLeases.nextGeneration++;
  if (generation == 0) {
    generation = g_conformanceLeases.nextGeneration++;
  }
  return generation;
}

static void BeginConformanceLease(
    HWND hwnd, const RECT& expectedRect, bool scheduleRepair,
    HWND preferredTiledWindow) {
  RuntimeLifecycle::AssertWmThread(L"BeginConformanceLease");
  if (!hwnd || g_settings.conformanceLeaseMs == 0 ||
      expectedRect.right <= expectedRect.left ||
      expectedRect.bottom <= expectedRect.top) {
    CancelConformanceLease(hwnd);
    return;
  }

  const ULONGLONG now = GetTickCount64();
  auto it = g_conformanceLeases.leases.find(hwnd);

  // Re-observing the same authoritative target while its lease is still active
  // must not extend the original deadline or replenish any implicit budget.
  if (it != g_conformanceLeases.leases.end() &&
      now < it->second.expiresAtTickMs &&
      EqualRect(&it->second.expectedRect, &expectedRect)) {
    if (preferredTiledWindow) {
      it->second.preferredTiledWindow = preferredTiledWindow;
    }
    if (scheduleRepair && !it->second.repairDueTickMs) {
      it->second.repairDueTickMs = std::min(
          it->second.expiresAtTickMs,
          now + g_settings.conformanceRepairIntervalMs);
    }
    ScheduleNextConformanceTimer();
    return;
  }

  ConformanceLease lease;
  lease.expectedRect = expectedRect;
  lease.preferredTiledWindow = preferredTiledWindow;
  lease.expiresAtTickMs = now + g_settings.conformanceLeaseMs;
  if (scheduleRepair) {
    lease.repairDueTickMs = std::min(
        lease.expiresAtTickMs,
        now + g_settings.conformanceRepairIntervalMs);
  }
  lease.generation = AllocateConformanceLeaseGeneration();
  g_conformanceLeases.leases[hwnd] = lease;

  ++Diagnostics::g_runtime.counters.conformanceLeasesStarted;
  ScheduleNextConformanceTimer();
}

static bool IsCurrentConformanceLease(HWND hwnd, uint64_t generation) {
  if (!hwnd || generation == 0) return false;
  ConformanceLease current;
  return ReadActiveConformanceLease(hwnd, &current) &&
         current.generation == generation;
}

static bool RecordConformanceLeaseAttempt(
    HWND hwnd, uint64_t generation, unsigned* outAttemptNumber = nullptr) {
  RuntimeLifecycle::AssertWmThread(L"RecordConformanceLeaseAttempt");
  if (!hwnd || generation == 0) return false;

  const ULONGLONG now = GetTickCount64();
  bool retained = false;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      now < it->second.expiresAtTickMs &&
      it->second.generation == generation) {
    ++it->second.attempts;
    it->second.lastRepairTickMs = now;
    it->second.repairDueTickMs = 0;
    if (outAttemptNumber) *outAttemptNumber = it->second.attempts;
    retained = true;
  }
  return retained;
}

static bool HasOutstandingConformanceLease(const Workspace& workspace) {
  for (HWND hwnd : workspace.TiledWindows()) {
    if (ReadConformanceLease(hwnd, nullptr) !=
        ConformanceLeaseReadResult::Missing) {
      return true;
    }
  }
  return false;
}

// Consumes one expired generation. A user gesture, migration, or new
// authoritative placement that cancelled/replaced it wins if it happens first.
static bool TakeExpiredConformanceLease(
    HWND hwnd, uint64_t generation, ConformanceLease* outLease) {
  RuntimeLifecycle::AssertWmThread(L"TakeExpiredConformanceLease");
  if (!hwnd || generation == 0 || !outLease) return false;

  const ULONGLONG now = GetTickCount64();
  bool taken = false;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      it->second.generation == generation &&
      now >= it->second.expiresAtTickMs) {
    *outLease = it->second;
    g_conformanceLeases.leases.erase(it);
    taken = true;
  }
  return taken;
}

// Recomputes the target from the current workspace rather than trusting a copied
// lease blindly. This makes expiry harmless if a layout/work-area change replaced
// the authoritative tile without issuing another SetWindowPos for this HWND.
// WM thread only. Writes physical screen pixels to outRect only on success;
// does not query the HWND, save model state, or place windows.
static bool GetCurrentAuthoritativeTiledRect(
    const DesktopMonitorKey& key, const Workspace& workspace, HWND hwnd,
    RECT* outRect) {
  if (!outRect || !hwnd || workspace.Layout() == TileLayout::Floating) return false;
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled) return false;

  const size_t tiledIndex = workspace.TiledIndex(hwnd);
  if (tiledIndex == static_cast<size_t>(-1)) return false;

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;
  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return false;

  std::vector<RECT> rects;
  if (Layout::BuildWorkspaceLayoutPlan(
          workspace, metrics.workArea, metrics.gap, &rects) !=
          Layout::LayoutPlanStatus::Success ||
      tiledIndex >= rects.size()) {
    return false;
  }
  *outRect = rects[tiledIndex];
  return true;
}

// An admission remains movable until every placement produced by its reflow has
// survived conformance. This matters because an existing sibling, not only the
// new HWND, can reveal the constraint that requires an alternate insertion slot.
static bool ClearSettledInsertionPlacements(
    const DesktopMonitorKey& key, Workspace& workspace) {
  if (HasOutstandingConformanceLease(workspace)) return false;

  bool changed = false;
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record || !record->pendingInsertionPlacement ||
        Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
        Platform::Windows::IsWindowCloaked(hwnd)) {
      continue;
    }

    RECT authoritative{};
    RECT current{};
    if (GetCurrentAuthoritativeTiledRect(
            key, workspace, hwnd, &authoritative) &&
        Platform::Windows::GetWindowFrameRect(hwnd, &current) &&
        Layout::RectsNear(current, authoritative)) {
      if (workspace.ClearInsertionPlacement(hwnd)) {
        changed = true;
        DiscardPendingAdmissionRecovery(hwnd);
      }
    }
  }
  return changed;
}

// HWND values can be reused. Conformance is allowed to act only on the same
// process identity that owns the authoritative WindowRecord; normal settled
// reconciliation remains responsible for forgetting a stale reused handle.
static bool ConformanceWindowMatchesRecordIdentity(
    HWND hwnd, const WindowRecord& record) {
  return hwnd && record.pid && Platform::Windows::IsSameLiveWindow(hwnd, record.pid);
}

// WM thread only. Returns a local copy of the sole owner on the active desktop,
// or no value if ownership cannot be established. Retains no HWND/record pointers.
// Physical eligibility and target checks stay with each caller's preflight.
static std::optional<ConformanceWorkspace> LoadConformanceWorkspace(HWND hwnd) {
  const auto owners = g_workspaces.OwnersOf(hwnd);
  if (owners.size() != 1 || !IsWorkspaceOnActiveDesktop(owners.front())) return std::nullopt;
  ConformanceWorkspace current;
  current.key = owners.front();
  if (!g_workspaces.Load(current.key, &current.workspace)) return std::nullopt;
  return current;
}

// WM thread only. record belongs to workspace and remains valid during this call.
// The probe executes application code, so recheck identity and physical state
// afterward. Stale leaves workspace unchanged; other outcomes refresh its cache.
// Changed means a valid native report changed geometry and requires replanning.
static ConformanceConstraintRefresh RefreshConformanceConstraints(
    HWND hwnd, Workspace& workspace, const WindowRecord& record) {
  const WindowConstraints before = record.constraints;
  const WindowConstraints fresh = Platform::Windows::ProbeWindowConstraints(hwnd);
  const WindowConstraints observed = Model::MergeConstraintProbeWithCache(before, fresh);
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, record) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::IsWindowCloaked(hwnd)) {
    return ConformanceConstraintRefresh::Stale;
  }
  const bool geometryChanged = !Model::SameConstraintGeometry(before, observed);
  workspace.UpdateConstraints(hwnd, observed);
  return fresh.valid && geometryChanged ? ConformanceConstraintRefresh::Changed
                                       : ConformanceConstraintRefresh::Unchanged;
}

// The deadline is a final passive verdict, not one more fight with the app. If
// this exact lease still describes the current tile and the HWND is materially
// elsewhere, concede that it is unsuitable for tiling: float it and close the
// remaining layout around it. Stale/replaced state is simply discarded.
static void FinalizeExpiredConformanceLease(
    HWND hwnd, const ConformanceLease& lease) {
  RuntimeLifecycle::AssertWmThread(L"FinalizeExpiredConformanceLease");

  auto stale = [&] {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredStale;
  };

  if (!IsWindow(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::IsWindowCloaked(hwnd) ||
      Gestures::IsMoveSizeGestureInProgress(hwnd)) {
    stale();
    return;
  }

  auto currentWorkspace = LoadConformanceWorkspace(hwnd);
  if (!currentWorkspace) {
    stale();
    return;
  }
  const auto& key = currentWorkspace->key;
  Workspace& workspace = currentWorkspace->workspace;

  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    stale();
    return;
  }

  RECT authoritative{};
  if (!GetCurrentAuthoritativeTiledRect(
          key, workspace, hwnd, &authoritative) ||
      !Layout::RectsNear(authoritative, lease.expectedRect)) {
    stale();
    return;
  }

  RECT current{};
  if (!Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    stale();
    return;
  }
  if (Layout::RectsNear(current, authoritative)) {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredCompliant;
    Diagnostics::RecordEvent(
        L"conformance lease completed compliant hwnd=%p attempts=%u",
        hwnd, lease.attempts);
    if (ClearSettledInsertionPlacements(key, workspace)) {
      g_workspaces.Save(key, workspace);
    }
    ClearSettledLayoutMutation(key, workspace);
    return;
  }

  // Expiry processing performs several live queries after its initial preflight.
  // A window that became suspended meanwhile must not be converted to Floating.
  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    stale();
    return;
  }

  // The deadline is the last chance to catch a dynamic native minimum report
  // that changed after planning. If the app's own report is unchanged, remaining
  // nonconformance is not reinterpreted as a second empirical minimum policy.
  const auto constraintRefresh = RefreshConformanceConstraints(hwnd, workspace, *record);
  if (constraintRefresh == ConformanceConstraintRefresh::Stale) {
    stale();
    return;
  }
  if (constraintRefresh == ConformanceConstraintRefresh::Changed) {
    const WindowConstraints& observed = record->constraints;
    Diagnostics::RecordEvent(
        L"conformance deadline refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
        hwnd, observed.minWidth, observed.minHeight, observed.dpi);
    g_workspaces.Save(key, workspace);
    ArrangeWorkspace(key, lease.preferredTiledWindow);
    return;
  }

  const std::vector<RECT> noRejectedRects;
  if (TryRollbackPendingLayoutMutation(
          key, key.ResolveMonitor(), &workspace,
          noRejectedRects, hwnd)) {
    ArrangeWorkspace(key);
    return;
  }

  if (!FloatWindowAfterFailure(key, workspace, hwnd)) {
    stale();
    return;
  }

  ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredFloats;
  Diagnostics::RecordEvent(
      L"conformance lease expired nonconforming; floated hwnd=%p attempts=%u",
      hwnd, lease.attempts);
  Wh_Log(
      L"Window %p remained outside its assigned tile at conformance deadline; switching it to Floating",
      hwnd);
  g_workspaces.Save(key, workspace);
  ArrangeWorkspace(key, lease.preferredTiledWindow);
}

// A tracked tiled HWND either confirms its authoritative rectangle or opens/reuses
// one bounded conformance lease. Native user gestures remain authoritative and
// are handled separately after MOVESIZEEND.
// Returns true when handled/deferred. False may cancel a stale lease; it does not
// imply that this routine had no effects. All rectangles are physical screen pixels.
static bool HandleTiledWindowLocationChange(HWND hwnd) {
  if (Gestures::IsMoveSizeGestureInProgress(hwnd)) return true;

  ConformanceLease lease;
  const ConformanceLeaseReadResult leaseState =
      ReadConformanceLease(hwnd, &lease);
  if (leaseState == ConformanceLeaseReadResult::Expired) {
    ScheduleNextConformanceTimer();
    return true;
  }
  const bool hasLease = leaseState == ConformanceLeaseReadResult::Active;
  auto rejectObservation = [&] {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  };

  if (!IsWindow(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::IsWindowCloaked(hwnd)) {
    return rejectObservation();
  }

  auto currentWorkspace = LoadConformanceWorkspace(hwnd);
  if (!currentWorkspace) return rejectObservation();
  const auto& key = currentWorkspace->key;
  Workspace& workspace = currentWorkspace->workspace;
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      workspace.Layout() == TileLayout::Floating ||
      !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    return rejectObservation();
  }

  // A lease is target-specific. Layout/order/weight changes can alter a tile
  // without necessarily issuing SetWindowPos for this HWND, so never reinforce
  // the copied target until it is checked against the current layout plan.
  RECT authoritative{};
  if (!GetCurrentAuthoritativeTiledRect(
          key, workspace, hwnd, &authoritative)) {
    return rejectObservation();
  }

  RECT current{};
  if (!Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    return rejectObservation();
  }
  if (Layout::RectsNear(current, authoritative)) {
    return true;  // Our own placement/event echo; nothing to repair.
  }

  if (!hasLease || !Layout::RectsNear(authoritative, lease.expectedRect)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    BeginConformanceLease(hwnd, authoritative, true);
    return true;
  }

  const ULONGLONG now = GetTickCount64();
  if (lease.repairDueTickMs && now < lease.repairDueTickMs) {
    return true;
  }
  if (lease.lastRepairTickMs &&
      now - lease.lastRepairTickMs < g_settings.conformanceRepairIntervalMs) {
    const ULONGLONG due =
        lease.lastRepairTickMs + g_settings.conformanceRepairIntervalMs;
    // Do not simply drop a bursty snap-back event: schedule one delayed re-check
    // so a final app move inside the rate window cannot leave the tile displaced.
    if (DeferConformanceLeaseRepair(hwnd, lease.generation, due)) {
      ++Diagnostics::g_runtime.counters.conformanceLeaseDeferredRepairs;
      return true;
    }
    return false;
  }

  // A copied observation must still refer to the same active lease immediately
  // before we act. Earlier actor work can have cancelled or replaced it; the
  // generation check prevents stale work from accounting against the replacement.
  if (!IsCurrentConformanceLease(hwnd, lease.generation)) return false;

  // Re-check at the mutation boundary: Chromium can finish entering fullscreen
  // while the ownership/layout queries above are in progress.
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  // The incoming LOCATIONCHANGE says the application did not remain on its
  // authoritative tile. Re-probe its own native minimum contract before another
  // SetWindowPos, in case that contract changed dynamically.
  // WM_GETMINMAXINFO executes target window code. Revalidate physical state
  // after that bounded call before either learning policy or reinforcing a tile.
  const auto constraintRefresh = RefreshConformanceConstraints(hwnd, workspace, *record);
  if (constraintRefresh == ConformanceConstraintRefresh::Stale) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (constraintRefresh == ConformanceConstraintRefresh::Changed) {
    const WindowConstraints& observedConstraints = record->constraints;
    CancelConformanceLease(hwnd, lease.generation);
    Diagnostics::RecordEvent(
        L"conformance location refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
        hwnd, observedConstraints.minWidth, observedConstraints.minHeight,
        observedConstraints.dpi);
    g_workspaces.Save(key, workspace);
    ArrangeWorkspace(key, lease.preferredTiledWindow);
    return true;
  }

  PlacementObservation observation =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, authoritative);

  // Count every issued reinforcement attempt, not only successful ones. A failed
  // checked placement is still part of the lease's cadence and must not look like
  // repeated "repair=1" telemetry. This does not extend the original deadline.
  unsigned attemptNumber = lease.attempts + 1;
  const bool leaseRetained =
      RecordConformanceLeaseAttempt(hwnd, lease.generation, &attemptNumber);

  Workspace::PlacementAction action =
      workspace.ApplyPlacementObservation(hwnd, observation);

  ++Diagnostics::g_runtime.counters.conformanceLeaseRepairs;
  Diagnostics::RecordEvent(
      L"conformance lease reinforced hwnd=%p attempt=%u result=%d",
      hwnd, attemptNumber, static_cast<int>(observation.result));

  if (action == Workspace::PlacementAction::Forget) {
    CancelConformanceLease(hwnd, lease.generation);
    workspace.Forget(hwnd);
    g_workspaces.Save(key, workspace);
    ArrangeWorkspace(key, lease.preferredTiledWindow);
    return true;
  }
  if (action == Workspace::PlacementAction::Float) {
    CancelConformanceLease(hwnd, lease.generation);
    FloatWindowAfterFailure(key, workspace, hwnd);
    g_workspaces.Save(key, workspace);
    ArrangeWorkspace(key, lease.preferredTiledWindow);
    return true;
  }

  g_workspaces.Save(key, workspace);
  if (observation.result != PlacementResult::Success && leaseRetained) {
    // Adjusted/refused placements are exactly the cases where relying on another
    // LOCATIONCHANGE is unsafe: the application may already be sitting at its
    // final self-selected rectangle. Keep the lease alive and schedule one more
    // checked attempt at the configured cadence. The hard deadline performs the
    // passive final verdict and Floats only if the HWND is still materially off
    // its current tile. AccessDenied remains an immediate terminal result above.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG due = std::min(
        lease.expiresAtTickMs,
        now + g_settings.conformanceRepairIntervalMs);
    DeferConformanceLeaseRepair(hwnd, lease.generation, due);
  }
  return true;
}

static void ProcessConformanceTimer() {
  RuntimeLifecycle::AssertWmThread(L"ProcessConformanceTimer");
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }

  struct ExpiredCandidate {
    HWND hwnd = nullptr;
    uint64_t generation = 0;
  };

  const ULONGLONG now = GetTickCount64();
  std::vector<ExpiredCandidate> expired;
  std::vector<HWND> dueWindows;
  for (auto& kv : g_conformanceLeases.leases) {
    ConformanceLease& lease = kv.second;
    if (now >= lease.expiresAtTickMs) {
      expired.push_back({kv.first, lease.generation});
      continue;
    }
    if (lease.repairDueTickMs && lease.repairDueTickMs <= now) {
      dueWindows.push_back(kv.first);
      lease.repairDueTickMs = 0;
    }
  }
  for (const ExpiredCandidate& candidate : expired) {
    ConformanceLease lease;
    if (TakeExpiredConformanceLease(
            candidate.hwnd, candidate.generation, &lease)) {
      FinalizeExpiredConformanceLease(candidate.hwnd, lease);
    }
  }
  for (HWND hwnd : dueWindows) {
    HandleTiledWindowLocationChange(hwnd);
  }
  ScheduleNextConformanceTimer();
}

} // namespace Placement
