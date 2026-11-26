// Placement leases, repair cadence and final conformance checks.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Placement {

struct ConformanceWorkspace {
  DesktopMonitorKey key{};
  Workspace workspace;
};

enum class ConformanceConstraintRefresh { Stale, Unchanged, Changed };

//-----------------------------------------------------------------------------
// PendingProofs members (declared in core/runtime_state.h)
//-----------------------------------------------------------------------------

ConformanceLeaseReadResult PendingProofs::ReadLease(
    HWND hwnd, ULONGLONG nowMs, ConformanceLease* outLease) const {
  const auto it = leases_.find(hwnd);
  if (it == leases_.end()) return ConformanceLeaseReadResult::Missing;
  if (outLease) *outLease = it->second;
  return nowMs < it->second.expiresAtTickMs ? ConformanceLeaseReadResult::Active
                                            : ConformanceLeaseReadResult::Expired;
}

uint64_t PendingProofs::AllocateGeneration() {
  uint64_t generation = nextGeneration_++;
  if (generation == 0) generation = nextGeneration_++;  // 0 means "any generation".
  return generation;
}

bool PendingProofs::BeginLease(
    HWND hwnd, ConformanceLeaseKind kind, const RECT& expectedRect,
    HWND preferredTiledWindow, bool scheduleRepair, ULONGLONG nowMs,
    const ConformanceLeaseTiming& timing) {
  const auto it = leases_.find(hwnd);

  // Re-observing the same authoritative target while its lease is still active
  // must not extend the original deadline or replenish any implicit budget.
  if (it != leases_.end() && nowMs < it->second.expiresAtTickMs &&
      it->second.kind == kind && EqualRect(&it->second.expectedRect, &expectedRect)) {
    ConformanceLease& lease = it->second;
    if (preferredTiledWindow) lease.preferredTiledWindow = preferredTiledWindow;
    if (scheduleRepair && !lease.repairDueTickMs) {
      lease.repairDueTickMs =
          std::min(lease.expiresAtTickMs, nowMs + timing.repairIntervalMs);
    }
    return false;
  }

  ConformanceLease lease;
  lease.kind = kind;
  lease.expectedRect = expectedRect;
  lease.preferredTiledWindow = preferredTiledWindow;
  lease.expiresAtTickMs = nowMs + timing.durationMs;
  if (scheduleRepair) {
    lease.repairDueTickMs =
        std::min(lease.expiresAtTickMs, nowMs + timing.repairIntervalMs);
  }
  lease.generation = AllocateGeneration();
  leases_[hwnd] = lease;
  return true;
}

bool PendingProofs::CancelLease(HWND hwnd, uint64_t generation) {
  const auto it = leases_.find(hwnd);
  if (it == leases_.end() ||
      (generation != 0 && it->second.generation != generation)) {
    return false;
  }
  leases_.erase(it);
  return true;
}

bool PendingProofs::DeferLeaseRepair(
    HWND hwnd, uint64_t generation, ULONGLONG dueTickMs, ULONGLONG nowMs) {
  if (generation == 0 || !dueTickMs) return false;
  const auto it = leases_.find(hwnd);
  if (it == leases_.end() || nowMs >= it->second.expiresAtTickMs ||
      it->second.generation != generation) {
    return false;
  }
  ConformanceLease& lease = it->second;
  if (!lease.repairDueTickMs || dueTickMs < lease.repairDueTickMs) {
    lease.repairDueTickMs = dueTickMs;
  }
  return true;
}

bool PendingProofs::RecordLeaseAttempt(
    HWND hwnd, uint64_t generation, ULONGLONG nowMs, unsigned* outAttemptNumber) {
  if (generation == 0) return false;
  const auto it = leases_.find(hwnd);
  if (it == leases_.end() || nowMs >= it->second.expiresAtTickMs ||
      it->second.generation != generation) {
    return false;
  }
  ConformanceLease& lease = it->second;
  ++lease.attempts;
  lease.lastRepairTickMs = nowMs;
  lease.repairDueTickMs = 0;
  if (outAttemptNumber) *outAttemptNumber = lease.attempts;
  return true;
}

std::optional<ConformanceLease> PendingProofs::TakeExpiredLease(
    HWND hwnd, uint64_t generation, ULONGLONG nowMs) {
  if (generation == 0) return std::nullopt;
  const auto it = leases_.find(hwnd);
  if (it == leases_.end() || it->second.generation != generation ||
      nowMs < it->second.expiresAtTickMs) {
    return std::nullopt;
  }
  ConformanceLease lease = it->second;
  leases_.erase(it);
  return lease;
}

DueConformanceWork PendingProofs::TakeDueLeaseWork(ULONGLONG nowMs) {
  DueConformanceWork work;
  for (auto& [hwnd, lease] : leases_) {
    if (nowMs >= lease.expiresAtTickMs) {
      work.expired.push_back({hwnd, lease.generation});
      continue;
    }
    if (lease.repairDueTickMs && lease.repairDueTickMs <= nowMs) {
      work.repairs.push_back(hwnd);
      lease.repairDueTickMs = 0;
    }
  }
  return work;
}

size_t PendingProofs::CountActiveLeases(ULONGLONG nowMs) const {
  return static_cast<size_t>(std::count_if(
      leases_.begin(), leases_.end(),
      [nowMs](const auto& entry) { return nowMs < entry.second.expiresAtTickMs; }));
}

ULONGLONG PendingProofs::NextLeaseDeadline() const {
  ULONGLONG earliestDue = 0;
  for (const auto& [hwnd, lease] : leases_) {
    ULONGLONG due = lease.expiresAtTickMs;
    if (lease.repairDueTickMs && lease.repairDueTickMs < due) {
      due = lease.repairDueTickMs;
    }
    if (!earliestDue || due < earliestDue) earliestDue = due;
  }
  return earliestDue;
}

void PendingProofs::RememberLayoutMutation(
    const DesktopMonitorKey& key, PendingLayoutMutation mutation) {
  mutations_[key] = std::move(mutation);
}

const PendingLayoutMutation* PendingProofs::FindLayoutMutation(
    const DesktopMonitorKey& key) const {
  const auto it = mutations_.find(key);
  return it == mutations_.end() ? nullptr : &it->second;
}

void PendingProofs::DiscardLayoutMutation(const DesktopMonitorKey& key) {
  mutations_.erase(key);
}

const SettlingRecovery* PendingProofs::FindSettlingRecovery(
    HWND hwnd) const {
  const auto it = recoveries_.find(hwnd);
  return it == recoveries_.end() ? nullptr : &it->second;
}

void PendingProofs::RememberSettlingRecovery(
    HWND hwnd, SettlingRecovery recovery) {
  recoveries_[hwnd] = std::move(recovery);
}

std::optional<SettlingRecovery> PendingProofs::TakeSettlingRecovery(
    HWND hwnd) {
  const auto it = recoveries_.find(hwnd);
  if (it == recoveries_.end()) return std::nullopt;
  SettlingRecovery recovery = std::move(it->second);
  recoveries_.erase(it);
  return recovery;
}

void PendingProofs::DiscardSettlingRecovery(HWND hwnd) {
  recoveries_.erase(hwnd);
}

void PendingProofs::Clear() noexcept {
  leases_.clear();
  mutations_.clear();
  recoveries_.clear();
}

//-----------------------------------------------------------------------------
// Lease operations used by placement, reconciliation and commands
//-----------------------------------------------------------------------------

// Reads lease state without consuming expiry. Expired leases stay present until
// the WM actor performs the final passive conformance verdict; WinEvent
// observations must never silently erase the evidence needed for that action.
static ConformanceLeaseReadResult ReadConformanceLease(
    HWND hwnd, ConformanceLease* outLease) {
  RuntimeLifecycle::AssertWmThread(L"ReadConformanceLease");
  if (!hwnd) return ConformanceLeaseReadResult::Missing;
  return g_pendingProofs.ReadLease(hwnd, GetTickCount64(), outLease);
}

static bool HasActiveConformanceLease(HWND hwnd) {
  return ReadConformanceLease(hwnd, nullptr) == ConformanceLeaseReadResult::Active;
}

// generation==0 means an unconditional cancellation (explicit user intent,
// topology/lifecycle teardown, migration, etc.). A nonzero generation lets a
// stale WM-thread observation avoid deleting a newer lease.
static bool CancelConformanceLease(HWND hwnd, uint64_t generation) {
  RuntimeLifecycle::AssertWmThread(L"CancelConformanceLease");
  if (!hwnd) return false;
  return g_pendingProofs.CancelLease(hwnd, generation);
}

// Cancels a tiled lease and leaves a floating one alone. Arranging the Floating
// layout uses this: that layout has no tiles, but its floating leases outlive it.
static bool CancelTiledConformanceLease(HWND hwnd) {
  ConformanceLease lease;
  if (ReadConformanceLease(hwnd, &lease) == ConformanceLeaseReadResult::Missing ||
      lease.kind != ConformanceLeaseKind::Tiled) {
    return false;
  }
  return CancelConformanceLease(hwnd, lease.generation);
}

// Forgets every unproven placement change (conformance leases, pending layout
// mutations and settling-recovery baselines) and stops the conformance timer,
// which has nothing left to wait for.
static void DiscardAllPendingProofs() {
  RuntimeLifecycle::AssertWmThread(L"DiscardAllPendingProofs");
  g_pendingProofs.Clear();
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.conformance);
}

static size_t CountActiveConformanceLeases() {
  RuntimeLifecycle::AssertWmThread(L"CountActiveConformanceLeases");
  return g_pendingProofs.CountActiveLeases(GetTickCount64());
}

static void ScheduleNextConformanceTimer() {
  RuntimeLifecycle::AssertWmThread(L"ScheduleNextConformanceTimer");
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.conformance);

  const ULONGLONG now = GetTickCount64();
  const ULONGLONG earliestDue = g_pendingProofs.NextLeaseDeadline();
  if (!earliestDue) return;
  const ULONGLONG remaining = earliestDue > now ? earliestDue - now : 1;
  const UINT delayMs = static_cast<UINT>(std::min<ULONGLONG>(remaining, 0x7FFFFFFFULL));
  g_wm.timers.conformance = SetTimer(nullptr, 0, std::max<UINT>(1, delayMs), nullptr);
  if (!g_wm.timers.conformance) {
    Wh_Log(L"Failed to schedule conformance repair/deadline timer");
  }
}

static bool DeferConformanceLeaseRepair(
    HWND hwnd, uint64_t generation, ULONGLONG dueTickMs) {
  RuntimeLifecycle::AssertWmThread(L"DeferConformanceLeaseRepair");
  if (!hwnd) return false;
  const bool retained = g_pendingProofs.DeferLeaseRepair(
      hwnd, generation, dueTickMs, GetTickCount64());
  if (retained) ScheduleNextConformanceTimer();
  return retained;
}

static void BeginConformanceLease(
    HWND hwnd, const RECT& expectedRect, bool scheduleRepair,
    HWND preferredTiledWindow, ConformanceLeaseKind kind) {
  RuntimeLifecycle::AssertWmThread(L"BeginConformanceLease");
  if (!hwnd || g_settings.conformanceLeaseMs == 0 ||
      expectedRect.right <= expectedRect.left ||
      expectedRect.bottom <= expectedRect.top) {
    CancelConformanceLease(hwnd);
    return;
  }

  const ConformanceLeaseTiming timing{g_settings.conformanceLeaseMs,
                                      g_settings.conformanceRepairIntervalMs};
  if (g_pendingProofs.BeginLease(hwnd, kind, expectedRect, preferredTiledWindow,
                                 scheduleRepair, GetTickCount64(), timing)) {
    ++Diagnostics::g_runtime.counters.conformanceLeasesStarted;
  }
  ScheduleNextConformanceTimer();
}

static bool IsCurrentConformanceLease(HWND hwnd, uint64_t generation) {
  if (!hwnd || generation == 0) return false;
  ConformanceLease current;
  return ReadConformanceLease(hwnd, &current) == ConformanceLeaseReadResult::Active &&
         current.generation == generation;
}

static bool RecordConformanceLeaseAttempt(
    HWND hwnd, uint64_t generation, unsigned* outAttemptNumber = nullptr) {
  RuntimeLifecycle::AssertWmThread(L"RecordConformanceLeaseAttempt");
  if (!hwnd) return false;
  return g_pendingProofs.RecordLeaseAttempt(
      hwnd, generation, GetTickCount64(), outAttemptNumber);
}

// Tiled leases only: a floating lease proves no tile and settles no layout change.
static bool HasOutstandingConformanceLease(const Workspace& workspace) {
  for (HWND hwnd : workspace.TiledWindows()) {
    ConformanceLease lease;
    if (ReadConformanceLease(hwnd, &lease) != ConformanceLeaseReadResult::Missing &&
        lease.kind == ConformanceLeaseKind::Tiled) {
      return true;
    }
  }
  return false;
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
  if (tiledIndex == Workspace::kNotFound) return false;

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
static bool MarkSettledWindows(
    const DesktopMonitorKey& key, Workspace& workspace) {
  if (HasOutstandingConformanceLease(workspace)) return false;

  bool changed = false;
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record || !record->settling ||
        Platform::Windows::IsSuspendedOrCloaked(hwnd)) {
      continue;
    }

    RECT authoritative{};
    RECT current{};
    if (GetCurrentAuthoritativeTiledRect(
            key, workspace, hwnd, &authoritative) &&
        Platform::Windows::GetWindowFrameRect(hwnd, &current) &&
        Layout::RectsNear(current, authoritative)) {
      if (workspace.MarkSettled(hwnd)) {
        changed = true;
        DiscardPendingSettlingRecovery(hwnd);
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
  const ConstraintProbeRefresh refresh = ProbeConstraintsAgainstCache(hwnd, record.constraints);
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, record) ||
      Platform::Windows::IsSuspendedOrCloaked(hwnd)) {
    return ConformanceConstraintRefresh::Stale;
  }
  workspace.UpdateConstraints(hwnd, refresh.merged);
  return refresh.reportChanged ? ConformanceConstraintRefresh::Changed
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
      Platform::Windows::IsSuspendedOrCloaked(hwnd) ||
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
    if (MarkSettledWindows(key, workspace)) {
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
  // A window that still cannot answer has given us no evidence that it rejects
  // the tile. Keep its slot and let responsive siblings settle the mutation.
  if (record->constraints.lastProbeError == ERROR_TIMEOUT) {
    g_workspaces.Save(key, workspace);
    ClearSettledLayoutMutation(key, workspace);
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
  if (record->lastPlacementResult == PlacementResult::Unresponsive) {
    // The window answered again. Give it a placement before judging its frame.
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

// --- Reacting to a tiled window that moved -----------------------------------

// What a location-change check observed, copied so nothing points into the
// repository. `authoritative` is the window's tile in the current layout plan;
// `current` is where the window actually is. Physical screen pixels.
struct TiledLocationObservation {
  DesktopMonitorKey key;
  Workspace workspace;
  RECT authoritative{};
  RECT current{};
};

// Loads the owning workspace and both rectangles. No value means the
// observation does not apply (window gone, suspended, cloaked, not tiled here,
// HWND reused by another process, or no tile can be planned).
static std::optional<TiledLocationObservation> ObserveTiledWindowLocation(HWND hwnd) {
  if (!IsWindow(hwnd) ||
      Platform::Windows::IsSuspendedOrCloaked(hwnd)) {
    return std::nullopt;
  }

  auto owner = LoadConformanceWorkspace(hwnd);
  if (!owner) return std::nullopt;
  TiledLocationObservation observed;
  observed.key = owner->key;
  observed.workspace = std::move(owner->workspace);
  const WindowRecord* record = observed.workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      observed.workspace.Layout() == TileLayout::Floating ||
      !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    return std::nullopt;
  }

  // A lease is target-specific. Layout/order/weight changes can alter a tile
  // without necessarily issuing SetWindowPos for this HWND, so never reinforce
  // the copied target until it is checked against the current layout plan.
  if (!GetCurrentAuthoritativeTiledRect(observed.key, observed.workspace, hwnd, &observed.authoritative) ||
      !Platform::Windows::GetWindowFrameRect(hwnd, &observed.current)) {
    return std::nullopt;
  }
  return observed;
}

enum class ConformanceRepairTiming {
  RepairNow,    // Go ahead and reinforce the tile.
  Waiting,      // A repair is already scheduled, or one was just deferred.
  DeferFailed,  // Rate-limited, but the lease vanished before it could be deferred.
};

// Repairs are rate-limited to one per ConformanceRepairIntervalMs. A burst of
// snap-back events inside that window is not dropped: one delayed re-check is
// scheduled so the app's final move still gets corrected.
static ConformanceRepairTiming CheckConformanceRepairTiming(HWND hwnd, const ConformanceLease& lease) {
  const ULONGLONG now = GetTickCount64();
  if (lease.repairDueTickMs && now < lease.repairDueTickMs) return ConformanceRepairTiming::Waiting;
  if (!lease.lastRepairTickMs || now - lease.lastRepairTickMs >= g_settings.conformanceRepairIntervalMs) {
    return ConformanceRepairTiming::RepairNow;
  }
  const ULONGLONG due = lease.lastRepairTickMs + g_settings.conformanceRepairIntervalMs;
  if (!DeferConformanceLeaseRepair(hwnd, lease.generation, due)) return ConformanceRepairTiming::DeferFailed;
  ++Diagnostics::g_runtime.counters.conformanceLeaseDeferredRepairs;
  return ConformanceRepairTiming::Waiting;
}

// How long until a lease's next repair after attempt `attemptNumber` gave
// `result`: the repair interval, doubled after each attempt the window didn't
// answer (up to 16 times the interval), since each costs a response timeout.
static ULONGLONG ConformanceRetryDelayMs(PlacementResult result, unsigned attemptNumber) {
  const ULONGLONG interval = g_settings.conformanceRepairIntervalMs;
  if (result != PlacementResult::Unresponsive) return interval;
  return interval << std::min(attemptNumber, 4u);
}

// Moves the window back onto its tile once more, after re-checking everything
// that could have changed since the observation. Saves the workspace and may
// float or forget the window if placement reports it cannot be managed.
// Returns false when the lease or window turned out to be stale.
static bool ReinforceConformanceTile(
    HWND hwnd, const ConformanceLease& lease, TiledLocationObservation& observed) {
  const DesktopMonitorKey& key = observed.key;
  Workspace& workspace = observed.workspace;
  const WindowRecord* record = workspace.Find(hwnd);

  // A copied observation must still refer to the same active lease immediately
  // before we act. Earlier actor work can have cancelled or replaced it; the
  // generation check prevents stale work from accounting against the replacement.
  if (!IsCurrentConformanceLease(hwnd, lease.generation)) return false;

  // Re-check at the mutation boundary: Chromium can finish entering fullscreen
  // while the ownership/layout queries above are in progress.
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      Platform::Windows::IsSuspendedOrCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  // The incoming LOCATIONCHANGE says the application did not remain on its
  // authoritative tile. Re-probe its own native minimum contract before another
  // SetWindowPos, in case that contract changed dynamically.
  // WM_GETMINMAXINFO executes target window code. Revalidate physical state
  // after that bounded call before either learning policy or reinforcing a tile.
  // A window that didn't answer the last placement skips it: the placement's own
  // responsiveness check comes first, and once that answers the next repair
  // re-probes. Otherwise a busy app costs two timeouts per repair.
  const auto constraintRefresh = record->lastPlacementResult == PlacementResult::Unresponsive
      ? ConformanceConstraintRefresh::Unchanged
      : RefreshConformanceConstraints(hwnd, workspace, *record);
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

  PlacementObservation placement =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, observed.authoritative);

  // Count every issued reinforcement attempt, not only successful ones. A failed
  // checked placement is still part of the lease's cadence and must not look like
  // repeated "repair=1" telemetry. This does not extend the original deadline.
  unsigned attemptNumber = lease.attempts + 1;
  const bool leaseRetained =
      RecordConformanceLeaseAttempt(hwnd, lease.generation, &attemptNumber);

  const Workspace::PlacementAction action = workspace.ApplyPlacementObservation(hwnd, placement);

  ++Diagnostics::g_runtime.counters.conformanceLeaseRepairs;
  Diagnostics::RecordEvent(
      L"conformance lease reinforced hwnd=%p attempt=%u result=%d",
      hwnd, attemptNumber, static_cast<int>(placement.result));

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
  if (placement.result != PlacementResult::Success && leaseRetained) {
    // Adjusted/refused placements are exactly the cases where relying on another
    // LOCATIONCHANGE is unsafe: the application may already be sitting at its
    // final self-selected rectangle. Keep the lease alive and schedule one more
    // checked attempt at the configured cadence. The hard deadline performs the
    // passive final verdict and Floats only if the HWND is still materially off
    // its current tile. AccessDenied remains an immediate terminal result above.
    const ULONGLONG due = std::min(
        lease.expiresAtTickMs,
        GetTickCount64() + ConformanceRetryDelayMs(placement.result, attemptNumber));
    DeferConformanceLeaseRepair(hwnd, lease.generation, due);
  }
  return true;
}

// --- Floating leases ---------------------------------------------------------
//
// In the Floating layout MultiWM owns a window's frame only right after placing
// it (PlaceFloatingWindowByHint, the window-size commands). An app that snaps back
// within the lease is put back; one that answers the placement with its own
// adjustment keeps it, and at the deadline the window keeps wherever it is. A
// floating lease never floats a window or rolls back a layout.

// Loads the workspace a floating lease still applies to: the sole owner on the
// active desktop, in the Floating layout, with the window a member whose
// remembered frame is still the lease's target. No value means the lease is stale.
static std::optional<ConformanceWorkspace> LoadFloatingLeaseWorkspace(
    HWND hwnd, const ConformanceLease& lease) {
  if (!IsWindow(hwnd) ||
      Platform::Windows::IsSuspendedOrCloaked(hwnd)) {
    return std::nullopt;
  }
  auto owner = LoadConformanceWorkspace(hwnd);
  if (!owner || owner->workspace.Layout() != TileLayout::Floating) return std::nullopt;
  const WindowRecord* record = owner->workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled || !record->hasFloatingRect ||
      !Layout::RectsNear(record->floatingRect, lease.expectedRect) ||
      !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    return std::nullopt;
  }
  return owner;
}

// The app moved a window inside its floating lease. Put it back at the lease's
// repair cadence. Returns true when handled; false after cancelling a stale lease.
static bool HandleFloatingLeaseLocationChange(HWND hwnd, const ConformanceLease& lease) {
  auto owner = LoadFloatingLeaseWorkspace(hwnd, lease);
  RECT current{};
  if (!owner || !Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (Layout::RectsNear(current, lease.expectedRect)) return true;

  switch (CheckConformanceRepairTiming(hwnd, lease)) {
    case ConformanceRepairTiming::Waiting: return true;
    case ConformanceRepairTiming::DeferFailed: return false;
    case ConformanceRepairTiming::RepairNow: break;
  }
  if (!IsCurrentConformanceLease(hwnd, lease.generation)) return false;

  Workspace& workspace = owner->workspace;
  const WindowRecord* record = workspace.Find(hwnd);
  const PlacementObservation placement =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, lease.expectedRect);
  unsigned attemptNumber = lease.attempts + 1;
  const bool leaseRetained =
      RecordConformanceLeaseAttempt(hwnd, lease.generation, &attemptNumber);
  const Workspace::PlacementAction action = workspace.ApplyPlacementObservation(hwnd, placement);
  ++Diagnostics::g_runtime.counters.conformanceLeaseRepairs;
  Diagnostics::RecordEvent(
      L"floating lease reinforced hwnd=%p attempt=%u result=%d",
      hwnd, attemptNumber, static_cast<int>(placement.result));

  if (action == Workspace::PlacementAction::Forget) {
    CancelConformanceLease(hwnd, lease.generation);
    workspace.Forget(hwnd);
  } else if (placement.result == PlacementResult::AdjustedByWindow) {
    // The app's answer to this placement is its own frame. Keep it.
    CancelConformanceLease(hwnd, lease.generation);
    RememberCurrentFloatingGeometry(owner->key, workspace, hwnd);
  } else if ((placement.result == PlacementResult::Refused ||
              placement.result == PlacementResult::Unresponsive) && leaseRetained) {
    const ULONGLONG due = std::min(
        lease.expiresAtTickMs, GetTickCount64() + ConformanceRetryDelayMs(placement.result, attemptNumber));
    DeferConformanceLeaseRepair(hwnd, lease.generation, due);
  } else if (placement.result != PlacementResult::Success) {
    CancelConformanceLease(hwnd, lease.generation);  // Access denied or suppressed.
  }
  g_workspaces.Save(owner->key, workspace);
  return true;
}

// The deadline of a floating lease: a window that left its frame keeps where the
// app put it, which becomes its remembered floating frame.
static void FinalizeExpiredFloatingLease(HWND hwnd, const ConformanceLease& lease) {
  RuntimeLifecycle::AssertWmThread(L"FinalizeExpiredFloatingLease");
  auto owner = Gestures::IsMoveSizeGestureInProgress(hwnd)
      ? std::nullopt : LoadFloatingLeaseWorkspace(hwnd, lease);
  RECT current{};
  if (!owner || !Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredStale;
    return;
  }
  if (Layout::RectsNear(current, lease.expectedRect)) {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredCompliant;
    Diagnostics::RecordEvent(
        L"floating lease completed compliant hwnd=%p attempts=%u", hwnd, lease.attempts);
    return;
  }
  if (RememberCurrentFloatingGeometry(owner->key, owner->workspace, hwnd)) {
    g_workspaces.Save(owner->key, owner->workspace);
  }
  ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredAdopted;
  Diagnostics::RecordEvent(
      L"floating lease expired; kept the app's frame hwnd=%p attempts=%u", hwnd, lease.attempts);
}

// A tracked tiled HWND either confirms its authoritative rectangle or opens/reuses
// one bounded conformance lease. Native user gestures remain authoritative and
// are handled separately after MOVESIZEEND. A window under a floating lease is
// handled by HandleFloatingLeaseLocationChange instead.
// Returns true when handled/deferred. False may cancel a stale lease; it does not
// imply that this routine had no effects. All rectangles are physical screen pixels.
static bool HandleTiledWindowLocationChange(HWND hwnd) {
  if (Gestures::IsMoveSizeGestureInProgress(hwnd)) return true;
  if (Platform::Windows::GhostWindowOf(hwnd)) return true;

  ConformanceLease lease;
  const ConformanceLeaseReadResult leaseState = ReadConformanceLease(hwnd, &lease);
  if (leaseState == ConformanceLeaseReadResult::Expired) {
    ScheduleNextConformanceTimer();  // The deadline verdict belongs to the timer.
    return true;
  }
  const bool hasLease = leaseState == ConformanceLeaseReadResult::Active;
  // Once Windows knows the app is hung, its next repair cannot succeed. The
  // existing deadline can settle bookkeeping and ghost recovery can reflow.
  if (hasLease && IsHungAppWindow(hwnd)) return true;
  if (hasLease && lease.kind == ConformanceLeaseKind::Floating) {
    return HandleFloatingLeaseLocationChange(hwnd, lease);
  }

  auto observed = ObserveTiledWindowLocation(hwnd);
  if (!observed) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (Layout::RectsNear(observed->current, observed->authoritative)) {
    return true;  // Our own placement/event echo; nothing to repair.
  }

  // Off its tile. Without a lease for this exact tile, start a fresh one.
  if (!hasLease || !Layout::RectsNear(observed->authoritative, lease.expectedRect)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    BeginConformanceLease(hwnd, observed->authoritative, true);
    return true;
  }

  switch (CheckConformanceRepairTiming(hwnd, lease)) {
    case ConformanceRepairTiming::Waiting: return true;
    case ConformanceRepairTiming::DeferFailed: return false;
    case ConformanceRepairTiming::RepairNow: break;
  }
  return ReinforceConformanceTile(hwnd, lease, *observed);
}
static void ProcessConformanceTimer() {
  RuntimeLifecycle::AssertWmThread(L"ProcessConformanceTimer");
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.conformance);

  const DueConformanceWork work = g_pendingProofs.TakeDueLeaseWork(GetTickCount64());
  for (const DueConformanceWork::ExpiredLease& candidate : work.expired) {
    // Consumes one expired generation. A user gesture, migration, or new
    // authoritative placement that cancelled/replaced it wins if it happened first.
    const std::optional<ConformanceLease> lease =
        g_pendingProofs.TakeExpiredLease(candidate.hwnd, candidate.generation, GetTickCount64());
    if (!lease) continue;
    if (lease->kind == ConformanceLeaseKind::Floating) {
      FinalizeExpiredFloatingLease(candidate.hwnd, *lease);
    } else {
      FinalizeExpiredConformanceLease(candidate.hwnd, *lease);
    }
  }
  for (HWND hwnd : work.repairs) {
    HandleTiledWindowLocationChange(hwnd);
  }
  ScheduleNextConformanceTimer();
}

} // namespace Placement
