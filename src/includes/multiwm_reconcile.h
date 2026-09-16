//=============================================================================
// Workspace reconciliation
//=============================================================================

namespace Platform::WindowEvents {
static bool HasTrackedMonitorOwnershipMismatch(HWND hwnd);
}

namespace Reconcile {

static bool SameWorkspaceKey(const DesktopMonitorKey& a, const DesktopMonitorKey& b);

enum class ReconcileScope : uint8_t {
  None = 0,
  Participation = 1 << 0,
  Desktop = 1 << 1,
  Monitor = 1 << 2,
  All = (1 << 0) | (1 << 1) | (1 << 2),
};

constexpr ReconcileScope operator|(ReconcileScope a, ReconcileScope b) {
  return static_cast<ReconcileScope>(
      static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool HasReconcileScope(ReconcileScope value, ReconcileScope flag) {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

static void ReconcileKnownWindows(
    ReconcileScope scope, std::vector<DesktopMonitorKey>* changedKeys,
    HWND onlyHwnd = nullptr);
static bool EnsureUniqueWindowOwnership(
    HWND hwnd, const DesktopMonitorKey& targetKey,
    std::vector<DesktopMonitorKey>* changedKeys = nullptr);

static Workspace MakeDefaultWorkspace() {
  Workspace workspace;
  workspace.SetLayout(g_settings.defaultLayout);
  workspace.SetMasterRatio(g_settings.masterPercent / 100.0);
  return workspace;
}

static bool ReconcileTrackedSnapshotOwnership(
    const DesktopMonitorKey& key, const std::vector<HWND>& snapshot,
    std::vector<DesktopMonitorKey>* changedKeys) {
  bool changed = false;
  for (HWND hwnd : snapshot) {
    if (IsWindowTrackedInAnyState(hwnd) &&
        EnsureUniqueWindowOwnership(hwnd, key, changedKeys)) {
      changed = true;
    }
  }
  return changed;
}

static bool SeedInitialSnapshot(
    Workspace& workspace, const std::vector<HWND>& initialSnapshot,
    bool useAutomaticAdmissionPolicy) {
  bool changed = false;
  for (HWND hwnd : initialSnapshot) {
    changed |= AdmitInitialObservedWindow(
        workspace, hwnd, useAutomaticAdmissionPolicy);
  }
  return changed;
}

static bool RestoreSuspendedSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot) {
  bool changed = false;
  for (HWND hwnd : snapshot) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->state == ManageState::Suspended) {
      changed |= workspace.Restore(hwnd);
    }
  }
  return changed;
}

static bool ReadoptFloatingSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot) {
  bool changed = false;
  for (HWND hwnd : snapshot) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->state == ManageState::Floating) {
      changed |= workspace.ActivateTiled(hwnd);
    }
  }
  return changed;
}

static bool AdmitUntrackedSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot, bool allowAdmission,
    bool useAutomaticAdmissionPolicy = false,
    std::vector<HWND>* admittedWindows = nullptr) {
  if (!allowAdmission) return false;

  const bool afterFocused =
      useAutomaticAdmissionPolicy &&
      g_settings.automaticNewWindowPosition ==
          AutomaticNewWindowPosition::AfterFocused;
  HWND anchor = afterFocused ? workspace.LastFocusedTiledWindow() : nullptr;

  bool changed = false;
  for (HWND hwnd : snapshot) {
    if (workspace.HasRecord(hwnd) || IsWindowTrackedInAnyState(hwnd)) continue;

    bool alwaysFloating = false;
    const bool startsFloating =
        ShouldWindowStartFloating(hwnd, &alwaysFloating);
    if (alwaysFloating || (useAutomaticAdmissionPolicy && startsFloating)) {
      WindowRecord record = MakeWindowRecord(hwnd, ManageState::Floating);
      record.pendingDefaultFloatingCenter = true;
      record.alwaysFloating = alwaysFloating;
      if (workspace.AdmitFloating(std::move(record))) {
        changed = true;
        if (admittedWindows) admittedWindows->push_back(hwnd);
      }
      continue;
    }

    bool admitted = false;
    WindowRecord record = MakeWindowRecord(hwnd, ManageState::Tiled);
    if (anchor) {
      admitted = workspace.AdmitTiledAfter(std::move(record), anchor);
    } else {
      admitted = workspace.AdmitTiled(std::move(record));
    }

    if (!admitted) continue;
    changed = true;
    if (admittedWindows) admittedWindows->push_back(hwnd);

    // Preserve enumeration order when one reconciliation burst discovers several
    // windows without changing the retained user-focus anchor.
    if (afterFocused) anchor = hwnd;
  }
  return changed;
}

// Automatic discovery can observe a new window while Windows still reports it as
// minimized. Such HWNDs belong to the workspace logically but must not enter the
// active tiling set until restored. Maximized/fullscreen windows are ordinary active
// tiled members with a presentation override and are discovered by the normal path.
static bool AdmitUntrackedSuspendedSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot, bool allowAdmission,
    bool useAutomaticAdmissionPolicy = false) {
  if (!allowAdmission) return false;

  const bool afterFocused =
      useAutomaticAdmissionPolicy &&
      g_settings.automaticNewWindowPosition ==
          AutomaticNewWindowPosition::AfterFocused;
  const HWND anchor = afterFocused ? workspace.LastFocusedTiledWindow() : nullptr;

  auto admitOne = [&](HWND hwnd) {
    if (workspace.HasRecord(hwnd) || IsWindowTrackedInAnyState(hwnd)) return false;

    const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
    if (reason != SuspensionReason::Minimized) {
      return false;
    }

    bool alwaysFloating = false;
    const bool startsFloating =
        ShouldWindowStartFloating(hwnd, &alwaysFloating);
    if (alwaysFloating || (useAutomaticAdmissionPolicy && startsFloating)) {
      WindowRecord record = MakeWindowRecord(hwnd, ManageState::Floating);
      record.pendingDefaultFloatingCenter = true;
      record.alwaysFloating = alwaysFloating;
      return workspace.AdmitFloating(std::move(record));
    }

    if (anchor) {
      return workspace.AdmitInitialAfter(MakeWindowRecord(hwnd), reason, anchor);
    }
    return workspace.AdmitInitial(MakeWindowRecord(hwnd), reason);
  };

  bool changed = false;
  if (anchor) {
    // Each suspended member targets the same logical point after the anchor.
    // Reverse traversal preserves the snapshot's original order.
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
      changed |= admitOne(*it);
    }
  } else {
    for (HWND hwnd : snapshot) changed |= admitOne(hwnd);
  }
  return changed;
}

static bool AdoptSnapshotGeometryIfEligible(
    Workspace& workspace, const std::vector<HWND>& snapshot,
    const RECT& workArea, LONG gap, HMONITOR monitor) {
  if (workspace.Layout() == TileLayout::Floating || workspace.HasSuspended() ||
      !workspace.AllTiledVisible(snapshot) ||
      !SnapshotLooksTiled(workspace.TiledWindows(), workArea)) {
    return false;
  }

  const HWND preferredMaster = workspace.Empty() ? nullptr
                                                 : workspace.TiledWindows().front();
  std::vector<WorkspaceGeometryItem> geometry = CaptureWorkspaceGeometry(
      workspace.TiledWindows(), monitor, workArea);
  Workspace captured = Workspace::AdoptGeometry(
      workspace.Layout(), workArea, gap, g_settings.masterPercent / 100.0,
      geometry, preferredMaster);
  const HWND retainedFocus = workspace.LastFocusedWindow();
  captured.MergeRememberedFloatingGeometryFrom(workspace);
  captured.MergeNonTiledRecordsFrom(workspace);
  captured.RememberFocusedWindow(retainedFocus);
  workspace = std::move(captured);
  return true;
}

// Reconciles a monitor snapshot into the current workspace while preserving known
// order and inactive records. Explicit adoption can re-tile floated windows and
// learn complete tiled geometry before persisting and arranging the result.
static bool EnsureWorkspaceFromSnapshot(
    HMONITOR monitor, bool adoptCurrentGeometry,
    std::vector<DesktopMonitorKey>* ownershipChangedKeys = nullptr) {
  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return false;
  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return false;

  const std::vector<HWND> snapshot = CollectTileWindows(monitor);
  Workspace workspace;
  const bool workspaceExisted = g_workspaces.Load(key, &workspace);
  const std::vector<HWND> initialSnapshot = workspaceExisted
      ? std::vector<HWND>{}
      : CollectWorkspaceWindowsForInitialization(monitor, key.desktopId);

  ReconcileTrackedSnapshotOwnership(
      key, workspaceExisted ? snapshot : initialSnapshot, ownershipChangedKeys);

  if (!g_workspaces.Load(key, &workspace)) workspace = MakeDefaultWorkspace();
  if (!workspaceExisted) {
    SeedInitialSnapshot(
        workspace, initialSnapshot,
        IsAutomaticMode() && !adoptCurrentGeometry);
  }

  // Tile Workspace may readopt current geometry, but it never bypasses native
  // application minimums. Constraint overrides belong at the application layer,
  // not in the tiling solver.

  RestoreSuspendedSnapshot(workspace, snapshot);
  if (adoptCurrentGeometry) ReadoptFloatingSnapshot(workspace, snapshot);
  AdmitUntrackedSnapshot(workspace, snapshot, true, false);

  if (adoptCurrentGeometry) {
    RefreshWorkspaceNativeConstraints(workspace, true, L"Tile Workspace");
    AdoptSnapshotGeometryIfEligible(
        workspace, snapshot, metrics.workArea, metrics.gap, monitor);
  }

  g_workspaces.Save(key, workspace);
  ArrangeWorkspace(key);
  return true;
}




// A managed HWND is allowed to have exactly one logical workspace owner.
static std::vector<DesktopMonitorKey> FindWorkspaceKeysContainingWindow(HWND hwnd) {
  return g_workspaces.OwnersOf(hwnd);
}

static void ForgetManagedWindow(HWND hwnd) {
  CancelConformanceLease(hwnd);
  const std::vector<DesktopMonitorKey> keys = g_workspaces.OwnersOf(hwnd);
  for (const auto& key : keys) {
    Workspace workspace;
    if (!g_workspaces.Load(key, &workspace)) continue;
    if (workspace.Forget(hwnd)) {
      g_workspaces.Save(key, workspace);
      ArrangeWorkspace(key);
    }
  }
  ClearMoveSizeSamples(hwnd);
}

static bool SameWorkspaceKey(const DesktopMonitorKey& a, const DesktopMonitorKey& b) {
  return a.monitor == b.monitor && IsEqualGUID(a.desktopId, b.desktopId);
}

static void AddUniqueWorkspaceKey(
    std::vector<DesktopMonitorKey>& keys, const DesktopMonitorKey& key) {
  if (!key.monitor) return;
  if (std::none_of(keys.begin(), keys.end(),
                   [&](const DesktopMonitorKey& existing) {
                     return SameWorkspaceKey(existing, key);
                   })) {
    keys.push_back(key);
  }
}

// Transfers one managed record between repositories while allowing Workspace to
// rebase its layout-specific logical slot/weight at the destination.
static bool MigrateManagedWindow(
    HWND hwnd, const DesktopMonitorKey& sourceKey, const DesktopMonitorKey& targetKey) {
  if (!hwnd || SameWorkspaceKey(sourceKey, targetKey)) return false;
  CancelConformanceLease(hwnd);
  DiscardPendingAdmissionRecovery(hwnd);

  Workspace source;
  if (!g_workspaces.Load(sourceKey, &source)) return false;
  const TileLayout sourceLayout = source.Layout();
  const bool wasFocused = source.LastFocusedWindow() == hwnd;

  WindowRecord record;
  if (!source.ExtractForMigration(hwnd, &record)) return false;
  const ManageState sourceState = record.state;
  g_workspaces.Save(sourceKey, source);

  Workspace target;
  if (!g_workspaces.Load(targetKey, &target)) {
    target.SetLayout(g_settings.defaultLayout);
    target.SetMasterRatio(g_settings.masterPercent / 100.0);
  }

  HWND insertionAnchor = nullptr;
  if (IsAutomaticMode() &&
      g_settings.automaticNewWindowPosition ==
          AutomaticNewWindowPosition::AfterFocused) {
    insertionAnchor = target.LastFocusedTiledWindow();
    if (insertionAnchor == hwnd) insertionAnchor = nullptr;
  }

  target.AdmitMigrated(std::move(record), insertionAnchor);
  if (wasFocused) target.RememberFocusedWindow(hwnd);

  if (sourceLayout != TileLayout::Floating &&
      sourceState != ManageState::Floating &&
      target.Layout() == TileLayout::Floating) {
    const MoveSizeGesture gesture = PeekMoveSizeGesture(hwnd);
    const FloatingPlacementHint hint = MakeFloatingPlacementHintFromGesture(gesture);
    // Automatic migration owns passive geometry. Manual cross-desktop moves
    // never reach this path; same-desktop Manual monitor drags may repair only
    // when the completed user gesture supplies an explicit anchor.
    if (IsAutomaticMode() ||
        hint.intent == FloatingPlacementIntent::PreserveAnchor) {
      const bool repaired = RepairFloatingGeometry(targetKey, target, hwnd, hint);
      if (repaired && hint.intent == FloatingPlacementIntent::PreserveAnchor) {
        // The completed gesture has already served as migration context. Clear it
        // so a deferred desktop migration cannot leave an old drag sample behind.
        ClearMoveSizeSamples(hwnd);
      }
    }
  }

  g_workspaces.Save(targetKey, target);
  ++Diagnostics::g_runtime.counters.workspaceMigrations;
  Diagnostics::RecordEvent(
      L"workspace migration hwnd=%p sourceDesktop=%08X targetDesktop=%08X sourceMonitorId=%ls targetMonitorId=%ls",
      hwnd, sourceKey.desktopId.Data1, targetKey.desktopId.Data1,
      sourceKey.monitor.deviceId.c_str(), targetKey.monitor.deviceId.c_str());
  return true;
}

// Reconcile one tracked HWND to a single logical workspace. This mirrors the
// mature WM model: physical ownership is queried/observed globally, while the
// layout tree keeps one logical owner. If an older path already duplicated the
// HWND, prefer the record that is already in the observed workspace and remove
// every stale copy.
static bool EnsureUniqueWindowOwnership(
    HWND hwnd, const DesktopMonitorKey& targetKey,
    std::vector<DesktopMonitorKey>* changedKeys) {
  std::vector<DesktopMonitorKey> owners = FindWorkspaceKeysContainingWindow(hwnd);
  if (owners.empty()) return false;

  const bool targetAlreadyOwns = std::any_of(
      owners.begin(), owners.end(), [&](const DesktopMonitorKey& key) {
        return SameWorkspaceKey(key, targetKey);
      });

  bool changed = false;
  if (!targetAlreadyOwns) {
    const DesktopMonitorKey sourceKey = owners.front();
    if (!MigrateManagedWindow(hwnd, sourceKey, targetKey)) {
      Wh_Log(L"Failed to reconcile HWND %p to observed workspace", hwnd);
      return false;
    }

    changed = true;
    if (changedKeys) {
      AddUniqueWorkspaceKey(*changedKeys, sourceKey);
      AddUniqueWorkspaceKey(*changedKeys, targetKey);
    }
  }

  // MigrateManagedWindow removes its selected source and any pre-existing
  // destination record, but an old bug may have left more than two copies.
  // Remove every remaining non-target owner explicitly.
  for (const auto& ownerKey : owners) {
    if (SameWorkspaceKey(ownerKey, targetKey)) continue;

    Workspace ownerState;
    if (!g_workspaces.Load(ownerKey, &ownerState)) continue;
    if (ownerState.Forget(hwnd)) {
      g_workspaces.Save(ownerKey, ownerState);
      changed = true;
      if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
    }
  }

  if (owners.size() > 1) {
    Wh_Log(L"Repaired duplicate HWND ownership for %p (%zu logical owners)",
           hwnd, owners.size());
  }

  return changed;
}

// Refreshes only the requested ownership aspects from physical window state.
static bool GetObservedWorkspaceKey(
    HWND hwnd, const DesktopMonitorKey& fallback, ReconcileScope scope,
    DesktopMonitorKey* outKey) {
  if (!outKey) return false;

  DesktopMonitorKey observed = fallback;
  bool observedAnything = false;

  if (HasReconcileScope(scope, ReconcileScope::Desktop)) {
    GUID desktopId{};
    if (GetWindowDesktopIdSafe(hwnd, &desktopId)) {
      observed.desktopId = desktopId;
      observedAnything = true;
    }
  }

  if (HasReconcileScope(scope, ReconcileScope::Monitor)) {
    HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
    Model::MonitorId monitorId;
    if (monitor && Model::MonitorId::FromHMonitor(monitor, &monitorId)) {
      observed.monitor = std::move(monitorId);
      observedAnything = true;
    }
  }

  if (!observedAnything || !observed.monitor) return false;
  *outKey = observed;
  return true;
}

static void ForgetWindowFromOwners(
    HWND hwnd, const std::vector<DesktopMonitorKey>& ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys) {
  CancelConformanceLease(hwnd);
  DiscardPendingAdmissionRecovery(hwnd);
  for (const auto& ownerKey : ownerKeys) {
    Workspace workspace;
    if (!g_workspaces.Load(ownerKey, &workspace)) continue;
    if (workspace.Forget(hwnd)) {
      g_workspaces.Save(ownerKey, workspace);
      if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
    }
  }
  ClearMoveSizeSamples(hwnd);
}

static std::vector<DesktopMonitorKey> ReconcileWindowOwnership(
    HWND hwnd, ReconcileScope scope,
    std::vector<DesktopMonitorKey> ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys) {
  const ReconcileScope ownershipScope = static_cast<ReconcileScope>(
      static_cast<uint8_t>(scope) &
      (static_cast<uint8_t>(ReconcileScope::Desktop) |
       static_cast<uint8_t>(ReconcileScope::Monitor)));
  if (ownershipScope == ReconcileScope::None || ownerKeys.empty()) return ownerKeys;

  DesktopMonitorKey observedKey{};
  if (GetObservedWorkspaceKey(hwnd, ownerKeys.front(), ownershipScope, &observedKey)) {
    const bool desktopChanged =
        HasReconcileScope(ownershipScope, ReconcileScope::Desktop) &&
        !IsEqualGUID(ownerKeys.front().desktopId, observedKey.desktopId);

    // Manual groups are deliberately inert. Moving a member to another virtual
    // desktop breaks its membership instead of transplanting it into that
    // desktop's workspace. Monitor-only moves retain the existing migration
    // behavior; Automatic mode continues to migrate across both dimensions.
    if (IsManualMode() && desktopChanged) {
      ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
      return {};
    }

    EnsureUniqueWindowOwnership(hwnd, observedKey, changedKeys);
    ownerKeys = g_workspaces.OwnersOf(hwnd);
  }
  return ownerKeys;
}

static void ReconcileWindowParticipation(
    HWND hwnd, const std::vector<DesktopMonitorKey>& ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys) {
  const SuspensionReason physicalReason = GetPhysicalSuspensionReason(hwnd);
  const PresentationState physicalPresentation =
      GetPhysicalPresentationState(hwnd);

  // In Manual mode, minimizing is an explicit departure from the managed group.
  // Forget the logical member entirely; restoring the HWND won't re-admit it
  // because Manual discovery remains closed. Maximize/fullscreen are presentation
  // overrides only and therefore never alter membership in either mode.
  if (IsManualMode() && physicalReason == SuspensionReason::Minimized) {
    ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
    return;
  }

  for (const auto& ownerKey : ownerKeys) {
    Workspace workspace;
    if (!g_workspaces.Load(ownerKey, &workspace)) continue;

    const WindowRecord* record = workspace.Find(hwnd);
    if (!record) continue;

    PresentationState presentation = physicalPresentation;
    if (presentation == PresentationState::Fullscreen &&
        record->state == ManageState::Tiled) {
      // A borderless tile can legitimately cover the monitor when the configured
      // work area has no visible insets/gaps. If it already occupies its exact
      // authoritative tile, don't mistake that geometry for application fullscreen.
      RECT authoritative{};
      RECT current{};
      if (GetCurrentAuthoritativeTiledRect(
              ownerKey, workspace, hwnd, &authoritative) &&
          GetWindowFrameRect(hwnd, &current) &&
          RectsNear(current, authoritative)) {
        presentation = PresentationState::Normal;
      }
    }

    bool changed = workspace.UpdatePresentationState(hwnd, presentation);
    if (presentation != PresentationState::Normal) {
      // Presentation overrides are application-owned geometry. Keep the tile in
      // the logical plan but stop any in-flight attempt to reinforce that HWND.
      CancelConformanceLease(hwnd);
      ClearMoveSizeSamples(hwnd);
    }

    bool restoredIntoFloatingLayout = false;
    const WindowRecord* refreshedRecord = workspace.Find(hwnd);
    if (!refreshedRecord) continue;
    const bool pendingInsertionBeforeSuspension =
        refreshedRecord->state == ManageState::Tiled &&
        refreshedRecord->pendingInsertionPlacement;

    if (refreshedRecord->state == ManageState::Tiled &&
        physicalReason != SuspensionReason::None) {
      CancelConformanceLease(hwnd);
      const bool suspended = workspace.Suspend(hwnd, physicalReason);
      changed |= suspended;
      if (suspended && pendingInsertionBeforeSuspension) {
        changed |= RestorePendingAdmissionRecoveryAfterSuspension(
            ownerKey, hwnd, &workspace);
      }
    } else if (refreshedRecord->state == ManageState::Suspended) {
      if (physicalReason == SuspensionReason::None) {
        const bool restored = workspace.Restore(hwnd);
        changed |= restored;
        restoredIntoFloatingLayout =
            restored && workspace.Layout() == TileLayout::Floating;
      } else {
        changed |= workspace.Suspend(hwnd, physicalReason);
      }
    }

    if (changed) {
      // A member that was suspended while its workspace entered Floating missed
      // the layout-transition restore. Apply the same passive remembered-geometry
      // policy when it becomes active again.
      if (restoredIntoFloatingLayout &&
          presentation == PresentationState::Normal) {
        RepairFloatingGeometry(
            ownerKey, workspace, hwnd, FloatingPlacementHint{});
      }
      g_workspaces.Save(ownerKey, workspace);
      if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
    }
  }
}

static void ReconcileKnownWindow(
    HWND hwnd, ReconcileScope scope,
    std::vector<DesktopMonitorKey>* changedKeys) {
  std::vector<DesktopMonitorKey> ownerKeys = g_workspaces.OwnersOf(hwnd);
  if (ownerKeys.empty()) return;

  if (!IsWindow(hwnd) || IsWindowExcludedByRules(hwnd)) {
    ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
    return;
  }

  // HWND values are reusable. A destroy event can be missed during a brief hook/
  // shell lifecycle gap and the numeric handle may later identify another process.
  // Never let the old logical record silently attach itself to that new window.
  DWORD livePid = 0;
  GetWindowThreadProcessId(hwnd, &livePid);
  bool pidMismatch = false;
  for (const auto& ownerKey : ownerKeys) {
    Workspace workspace;
    if (!g_workspaces.Load(ownerKey, &workspace)) continue;
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->pid != 0 && livePid != 0 && record->pid != livePid) {
      pidMismatch = true;
      break;
    }
  }
  if (pidMismatch) {
    Diagnostics::RecordEvent(
        L"discarded stale HWND ownership after PID reuse hwnd=%p livePid=%lu",
        hwnd, livePid);
    ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
    ClearMoveSizeSamples(hwnd);
    CancelConformanceLease(hwnd);
    return;
  }

  ownerKeys = ReconcileWindowOwnership(hwnd, scope, std::move(ownerKeys), changedKeys);
  if (ownerKeys.empty()) return;

  // Rules normally classify a window at admission. Also enforce Always floating
  // for an already-managed window after a settings reload; once applied, retain
  // it for that HWND's lifetime even if a title-based match later changes.
  if (ShouldWindowAlwaysFloat(hwnd)) {
    bool policyChanged = false;
    for (const auto& ownerKey : ownerKeys) {
      Workspace workspace;
      if (!g_workspaces.Load(ownerKey, &workspace) ||
          !workspace.EnforceAlwaysFloating(hwnd)) {
        continue;
      }
      policyChanged = true;
      g_workspaces.Save(ownerKey, workspace);
      if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
    }
    if (policyChanged) CancelConformanceLease(hwnd);
  }

  if (HasReconcileScope(scope, ReconcileScope::Participation)) {
    ReconcileWindowParticipation(hwnd, ownerKeys, changedKeys);
  }
}

// Orchestration only: choose the known HWND set, then run the explicitly scoped
// ownership/participation phases for each one.
static void ReconcileKnownWindows(
    ReconcileScope scope, std::vector<DesktopMonitorKey>* changedKeys,
    HWND onlyHwnd) {
  ++Diagnostics::g_runtime.counters.reconcileCalls;
  const std::vector<HWND> known = onlyHwnd
                                      ? std::vector<HWND>{onlyHwnd}
                                      : g_workspaces.KnownWindows();
  Diagnostics::g_runtime.counters.reconcileWindowsExamined += known.size();
  for (HWND hwnd : known) ReconcileKnownWindow(hwnd, scope, changedKeys);
}

static void AddUniqueMonitor(std::vector<HMONITOR>& monitors, HMONITOR monitor) {
  if (IsLiveMonitorHandle(monitor) &&
      std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
    monitors.push_back(monitor);
  }
}

static void AddPendingWorkspaceArrange(const GUID& desktopId, HMONITOR monitor) {
  if (!IsLiveMonitorHandle(monitor)) return;
  // Only if not scheduled already
  auto it = std::find_if(
      g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
      [&](const PendingWorkspaceArrange& pending) {
        return pending.monitor == monitor && IsEqualGUID(pending.desktopId, desktopId);
      });
  if (it == g_wm.pendingDesktopArranges.end()) {
    g_wm.pendingDesktopArranges.push_back({desktopId, monitor});
  }
}

static void ScheduleLifecycleReconcile(HWND hwnd);

// Build only the extra inactive candidates implicated by this settled lifecycle
// burst. This deliberately does not add another EnumWindows pass: normal active
// discovery (including maximize/fullscreen presentation overrides) still comes from
// the one-pass per-monitor snapshot, while newly observed minimized HWNDs are
// classified from the already-dirty set.
static std::vector<std::vector<HWND>> CollectDirtySuspendedWindowsForMonitors(
    const std::vector<HWND>& dirtyWindows,
    const std::vector<HMONITOR>& monitors,
    const GUID& desktopId) {
  std::vector<std::vector<HWND>> snapshots(monitors.size());
  if (!IsAutomaticMode() || dirtyWindows.empty() || monitors.empty()) {
    return snapshots;
  }

  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd) || IsWindowTrackedInAnyState(hwnd)) continue;
    if (!IsWindowVisible(hwnd) || !WindowCanBeManaged(hwnd) || !IsWindowEnabled(hwnd)) {
      continue;
    }

    const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
    if (reason != SuspensionReason::Minimized) {
      continue;
    }

    GUID windowDesktop{};
    if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
        !IsEqualGUID(windowDesktop, desktopId)) {
      continue;
    }

    const HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
    auto it = std::find(monitors.begin(), monitors.end(), monitor);
    if (it == monitors.end()) continue;

    snapshots[static_cast<size_t>(it - monitors.begin())].push_back(hwnd);
  }

  return snapshots;
}

// Current-desktop discovery has two separate jobs: reconcile already-managed
// HWND ownership according to the active management policy, and optionally admit
// truly new windows while Automatic mode is active.
static bool DiscoverWorkspaceWindows(
    HMONITOR monitor, const GUID& desktopId, const std::vector<HWND>& snapshot,
    DesktopMonitorKey* outKey,
    std::vector<DesktopMonitorKey>* ownershipChangedKeys = nullptr,
    const std::vector<HWND>* suspendedSnapshot = nullptr) {
  if (!IsLiveMonitorHandle(monitor)) return false;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return false;
  if (outKey) *outKey = key;

  Workspace workspace;
  const bool workspaceExisted = g_workspaces.Load(key, &workspace);
  const bool admitUntracked = IsAutomaticMode();
  const bool initializing = !workspaceExisted && admitUntracked;
  const std::vector<HWND> initialSnapshot = initializing
      ? CollectWorkspaceWindowsForInitialization(monitor, desktopId)
      : std::vector<HWND>{};

  bool changed = ReconcileTrackedSnapshotOwnership(
      key, initializing ? initialSnapshot : snapshot, ownershipChangedKeys);

  bool hasState = g_workspaces.Load(key, &workspace);
  if (!hasState) workspace = MakeDefaultWorkspace();

  if (initializing &&
      SeedInitialSnapshot(workspace, initialSnapshot, true)) {
    hasState = true;
    changed = true;
  }
  changed |= RestoreSuspendedSnapshot(workspace, snapshot);

  std::vector<HWND> newlyAdmittedWindows;
  if (AdmitUntrackedSnapshot(
          workspace, snapshot, admitUntracked, true,
          &newlyAdmittedWindows)) {
    hasState = true;
    changed = true;
  }
  if (suspendedSnapshot &&
      AdmitUntrackedSuspendedSnapshot(
          workspace, *suspendedSnapshot, admitUntracked, true)) {
    hasState = true;
    changed = true;
  }

  // A default-floating HWND can be admitted during initial seeding or while it
  // is minimized. Once it appears in the active snapshot, route the retained
  // centering request through the same placement pass. Maximize/fullscreen do not
  // change its logical state and physical repair stays suppressed until Normal.
  for (HWND hwnd : snapshot) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->state == ManageState::Floating &&
        record->pendingDefaultFloatingCenter &&
        !ContainsWindow(newlyAdmittedWindows, hwnd)) {
      newlyAdmittedWindows.push_back(hwnd);
    }
  }

  // Automatic discovery owns the first floating geometry of a genuinely new
  // restored window. A rule-floated admission always keeps its application-
  // selected size, including when the whole workspace is already Floating.
  // Other Floating-workspace admissions use the configured sizing policy.
  if (admitUntracked) {
    for (HWND hwnd : newlyAdmittedWindows) {
      const WindowRecord* record = workspace.Find(hwnd);
      const bool pendingDefaultCenter =
          record && record->state == ManageState::Floating &&
          record->pendingDefaultFloatingCenter;
      if (!record ||
          (workspace.Layout() != TileLayout::Floating &&
           !pendingDefaultCenter)) {
        continue;
      }

      FloatingPlacementHint newWindowHint;
      newWindowHint.intent =
          pendingDefaultCenter
              ? FloatingPlacementIntent::CenterPreservingSize
              : FloatingPlacementIntent::NewWindowCenter;
      if (RepairFloatingGeometry(key, workspace, hwnd, newWindowHint)) {
        workspace.ClearDefaultFloatingCenter(hwnd);
        changed = true;
      }
    }
  }

  // Selection used the retained pre-admission focus. Once membership is stable,
  // let a newly admitted foreground HWND become the next retained focus anchor.
  HWND foreground = GetForegroundWindow();
  if (foreground && workspace.HasRecord(foreground)) {
    workspace.RememberFocusedWindow(foreground);
  }

  if (!hasState || !changed) return false;
  g_workspaces.Save(key, workspace);
  return true;
}

// Runs the normal discovery path for one monitor. The pipeline is always active;
// DiscoverWorkspaceWindows() alone decides whether unknown HWNDs may be admitted;
// Manual-mode retention decisions are handled by the reconciliation policy.
static bool DiscoverCurrentWorkspaceOnMonitor(HMONITOR monitor) {
  if (!monitor) return false;

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    return false;
  }

  std::vector<HWND> snapshot = CollectTileWindows(monitor);
  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(currentDesktop, monitor, &key)) return false;
  const bool changed =
      DiscoverWorkspaceWindows(monitor, currentDesktop, snapshot, &key);

  // "No model mutation" is not the same as "no workspace". Startup, mode changes,
  // and explicit monitor passes may need to reinforce already-correct membership.
  Workspace existing;
  const bool hasWorkspace = changed || g_workspaces.Load(key, &existing);
  if (!hasWorkspace) return false;

  ArrangeWorkspace(key);
  return true;
}

static void EnumerateCurrentMonitors(std::vector<HMONITOR>& monitors) {
  // This helper means "current OS topology", not "merge more monitor handles".
  monitors.clear();
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM lParam) WINAPI -> BOOL {
        auto* list = reinterpret_cast<std::vector<HMONITOR>*>(lParam);
        AddUniqueMonitor(*list, monitor);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
}

// During a virtual-desktop switch, DWM cloaks and uncloaks windows one at a time.
// Snapshot otherwise eligible windows by desktop GUID while deliberately ignoring
// that transient cloak bit; settled reconciliation handles genuine cloaks later.
static std::vector<HWND> GetTileWindowsAfterDesktopSwitch(
    HMONITOR monitor, const GUID& desktopId) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    GUID desktopId;
    std::vector<HWND>* windowList;
  } context = {monitor, desktopId, &windows};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);

        if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) return TRUE;
        if (!WindowCanBeManaged(hwnd)) return TRUE;
        if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        if (GetWindowPhysicalMonitor(hwnd) == ctx->targetMonitor) {
          ctx->windowList->push_back(hwnd);
          ++Diagnostics::g_runtime.counters.tileCandidatesCollected;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

// Native CurrentVirtualDesktopChanged is the authoritative workspace transition
// signal. The cloak-insensitive snapshot is used only to discover new incoming
// windows; known WindowRecord membership is never rebuilt from shell visibility.
static void HandleVirtualDesktopChanged() {
  InterlockedExchange(&g_vd.changeQueued, 0);
  ++Diagnostics::g_runtime.counters.virtualDesktopChangesProcessed;

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    // The native callback is an authoritative transition signal, but without a
    // current GUID we cannot prove that an existing placement lease still belongs
    // to the active desktop. Drop those short-lived contracts and retry normally.
    ClearAllConformanceLeases();
    ScheduleNextConformanceTimer();
    Wh_Log(L"Native VD change received but current desktop query failed");
    Diagnostics::RecordEvent(L"virtual desktop notification: current desktop query failed");
    ScheduleLifecycleReconcile(nullptr);
    return;
  }

  Diagnostics::RecordEvent(
      L"virtual desktop changed currentDesktop=%08X", currentDesktop.Data1);
  const bool desktopActuallyChanged =
      g_wm.reconciledDesktopState == ReconciledDesktopState::Unknown ||
      !IsEqualGUID(g_wm.reconciledDesktop, currentDesktop);
  if (desktopActuallyChanged) {
    // A lease describes post-placement behavior on one active desktop. Never carry
    // it across DWM's cloak/uncloak transition; the incoming settled arrange pass
    // will establish fresh leases for any placements it actually has to issue.
    ClearAllConformanceLeases();
    ScheduleNextConformanceTimer();
  }
  g_wm.reconciledDesktop = currentDesktop;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  // The visual transition belongs to the settled desktop state, not the raw COM
  // callback. Coalesce rapid switches and emit one monitor-local OSD per connected
  // display after reconciliation has finished. Duplicate callbacks for the already
  // represented desktop do not generate another visual transition.
  if (desktopActuallyChanged) g_wm.pendingDesktopSwitchFlyouts = true;

  // A native VD transition is an authoritative reason to repair desktop ownership.
  // Do not infer monitor migration here; defer visibility/window-state participation
  // until the shell's cloak/show burst has settled.
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(ReconcileScope::Desktop, &changedKeys);
  for (const auto& key : changedKeys) {
    if (IsEqualGUID(key.desktopId, currentDesktop)) {
      AddPendingWorkspaceArrange(key.desktopId, key.ResolveMonitor());
    }
  }

  std::vector<HMONITOR> monitors;
  EnumerateCurrentMonitors(monitors);

  // FancyWM refreshes ownership and then unconditionally invalidates the incoming
  // desktop layout. Do the equivalent here, but defer actual placement until the
  // shell's cloak/uncloak burst settles. Existing workspaces are reinforced even
  // when their bookkeeping did not change.
  for (HMONITOR monitor : monitors) {
    DesktopMonitorKey knownKey{};
    if (!DesktopMonitorKey::FromHMonitor(currentDesktop, monitor, &knownKey)) continue;
    Workspace knownState;
    if (g_workspaces.Load(knownKey, &knownState)) {
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  for (HMONITOR monitor : monitors) {
    std::vector<HWND> snapshot = GetTileWindowsAfterDesktopSwitch(monitor, currentDesktop);
    DesktopMonitorKey key{};
    if (DiscoverWorkspaceWindows(
            monitor, currentDesktop, snapshot, &key)) {
      // New/migrated membership is also reinforced by the same settled pass.
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  // Coalesce the remaining shell burst for discovery/fallback and the unconditional
  // incoming-desktop reinforcement. The settled pass may also repair physical state.
  ScheduleLifecycleReconcile(nullptr);
}

// Coalesce shell visibility bursts for discovery and deferred placement. Generic
// lifecycle events are dirty signals: after the settle delay we may re-query
// desktop ownership for the affected tracked HWND, but never infer ownership
// directly from HIDE/SHOW/CLOAK itself.
static void ScheduleLifecycleReconcile(HWND hwnd) {
  ++Diagnostics::g_runtime.counters.lifecycleScheduleRequests;
  // HotkeyThreadProc owns this vector/timer; no additional lock is required.
  if (hwnd && std::find(g_wm.lifecycleDirtyWindows.begin(), g_wm.lifecycleDirtyWindows.end(), hwnd) ==
                  g_wm.lifecycleDirtyWindows.end()) {
    g_wm.lifecycleDirtyWindows.push_back(hwnd);
  }

  if (g_wm.lifecycleTimer) {
    ++Diagnostics::g_runtime.counters.lifecycleTimerResets;
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }

  g_wm.lifecycleTimer = SetTimer(nullptr, 0, g_settings.reconcileDelayMs, nullptr);
  if (!g_wm.lifecycleTimer) {
    Wh_Log(L"Failed to schedule lifecycle timer; queueing immediate reconciliation");
    const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
    if (threadId) {
      PostThreadMessage(threadId, WM_APP_RECONCILE_NOW, 0, 0);
    }
  }
}

static void ScheduleNextDiscoveryRetryTimer() {
  if (g_wm.discoveryRetryTimer) {
    KillTimer(nullptr, g_wm.discoveryRetryTimer);
    g_wm.discoveryRetryTimer = 0;
  }
  if (g_wm.pendingDiscoveryRetries.empty()) return;

  const ULONGLONG now = GetTickCount64();
  ULONGLONG earliestDue = g_wm.pendingDiscoveryRetries.front().dueTickMs;
  for (const auto& pending : g_wm.pendingDiscoveryRetries) {
    earliestDue = std::min(earliestDue, pending.dueTickMs);
  }
  const UINT delayMs = earliestDue > now
                           ? static_cast<UINT>(earliestDue - now)
                           : 1;
  g_wm.discoveryRetryTimer = SetTimer(nullptr, 0, delayMs, nullptr);
  if (!g_wm.discoveryRetryTimer) {
    Wh_Log(L"Failed to schedule slow-application discovery timer");
    g_wm.pendingDiscoveryRetries.clear();
  }
}

// This stage owns only untracked HWNDs. Once discovery succeeds,
// ArrangeWorkspace establishes any needed post-placement conformance lease.
static void ProcessDiscoveryRetryTimer() {
  AssertWmThread(L"ProcessDiscoveryRetryTimer");
  if (g_wm.discoveryRetryTimer) {
    KillTimer(nullptr, g_wm.discoveryRetryTimer);
    g_wm.discoveryRetryTimer = 0;
  }
  if (!IsAutomaticMode()) {
    g_wm.pendingDiscoveryRetries.clear();
    return;
  }

  const ULONGLONG now = GetTickCount64();
  std::vector<PendingDiscoveryRetry> due;
  g_wm.pendingDiscoveryRetries.erase(
      std::remove_if(
          g_wm.pendingDiscoveryRetries.begin(),
          g_wm.pendingDiscoveryRetries.end(),
          [&](const PendingDiscoveryRetry& pending) {
            if (pending.dueTickMs > now) return false;
            due.push_back(pending);
            return true;
          }),
      g_wm.pendingDiscoveryRetries.end());
  ScheduleNextDiscoveryRetryTimer();

  std::vector<HMONITOR> monitors;
  for (const auto& pending : due) {
    if (!pending.hwnd || !IsWindow(pending.hwnd) ||
        IsWindowTrackedInAnyState(pending.hwnd)) {
      continue;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(pending.hwnd, &processId);
    if (!processId || processId != pending.processId) continue;
    AddUniqueMonitor(monitors,
                     GetCurrentManagedWindowMonitor(pending.hwnd));
  }
  for (HMONITOR monitor : monitors) DiscoverCurrentWorkspaceOnMonitor(monitor);
}

// After a lifecycle burst settles, reconcile tracked-window physical and desktop
// state, discover eligible windows only on the current desktop, then arrange each
// changed or pending workspace once and refresh the tray.
static void ReconcileDeferredLifecycle() {
  ++Diagnostics::g_runtime.counters.lifecycleReconciles;
  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }

  std::vector<HWND> dirtyWindows;
  dirtyWindows.swap(g_wm.lifecycleDirtyWindows);
  Diagnostics::RecordLifecycleBurst(
      dirtyWindows.size(), g_wm.pendingDesktopArranges.size());

  // Resolve desktop identity before mutating model state. If Explorer is still
  // coming up (or restarting), preserve the dirty set and let the single runtime
  // maintenance owner recover COM before this exact work is retried.
  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    for (HWND hwnd : dirtyWindows) {
      if (hwnd && std::find(g_wm.lifecycleDirtyWindows.begin(),
                            g_wm.lifecycleDirtyWindows.end(), hwnd) ==
                      g_wm.lifecycleDirtyWindows.end()) {
        g_wm.lifecycleDirtyWindows.push_back(hwnd);
      }
    }
    g_wm.lifecycleRetryAfterPlatformRecovery = true;
    RuntimeLifecycle::RequestMaintenance(false);
    Wh_Log(L"Lifecycle reconcile deferred: current virtual desktop unavailable");
    return;
  }

  Diagnostics::g_runtime.counters.lifecycleDirtyWindowsProcessed += dirtyWindows.size();

  // Native notifications are primary, but a settled cloak/uncloak burst can
  // independently detect a missed subscription/callback. Treat an observed GUID
  // change as a real desktop transition: repair global desktop ownership and
  // reinforce all known/current monitors on the incoming desktop.
  const bool fallbackDesktopChange =
      g_wm.reconciledDesktopState != ReconciledDesktopState::Unknown &&
      !IsEqualGUID(g_wm.reconciledDesktop, currentDesktop);
  if (fallbackDesktopChange) {
    // The native callback was missed, so this is the first point where the WM can
    // prove that the active desktop changed. Discard any lease created on the old
    // desktop before doing ownership repair or incoming-workspace reinforcement.
    ClearAllConformanceLeases();
    ScheduleNextConformanceTimer();
    ++Diagnostics::g_runtime.counters.virtualDesktopFallbackChanges;
    Diagnostics::RecordEvent(
        L"virtual desktop change recovered from settled WinEvents currentDesktop=%08X",
        currentDesktop.Data1);
    // A missed native callback is also a high-confidence signal that an exhausted
    // subscription retry burst deserves one fresh attempt sequence.
    if (g_vd.abi.notification.methodCount > 0 &&
        !g_vd.notificationsRegistered) {
      RuntimeLifecycle::RequestMaintenance(true);
    }
    g_wm.pendingDesktopSwitchFlyouts = true;
  }
  g_wm.reconciledDesktop = currentDesktop;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;

  // Repair liveness/exclusions and settled suspended participation globally
  // before doing current-desktop discovery.
  std::vector<DesktopMonitorKey> globallyChanged;
  ReconcileKnownWindows(ReconcileScope::Participation, &globallyChanged);
  if (fallbackDesktopChange) {
    ReconcileKnownWindows(ReconcileScope::Desktop, &globallyChanged);
  }

  // Dirty tracked HWNDs also get a desktop ownership check; per-window moves do
  // not emit CurrentVirtualDesktopChanged.
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindowTrackedInAnyState(hwnd)) continue;
    ReconcileKnownWindows(ReconcileScope::Desktop, &globallyChanged, hwnd);
  }

  std::vector<HMONITOR> monitors;
  if (fallbackDesktopChange || g_wm.forceMonitorReconcile) {
    // Authoritative desktop/topology passes derive their boundary set from the OS,
    // not from whatever workspace keys happened to exist before the transition.
    EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      DesktopMonitorKey key{};
      if (!DesktopMonitorKey::FromHMonitor(currentDesktop, monitor, &key)) continue;
      Workspace existing;
      if (g_workspaces.Load(key, &existing)) {
        AddPendingWorkspaceArrange(currentDesktop, monitor);
      }
    }
  } else {
    // Incremental lifecycle work stays cheap: start from known live workspace
    // boundaries and add only monitors implicated by this burst.
    monitors = g_workspaces.MonitorsOnDesktop(currentDesktop);
  }

  if (g_wm.forceMonitorReconcile) {
    ReconcileKnownWindows(ReconcileScope::Monitor, &globallyChanged);
    g_wm.forceMonitorReconcile = false;
  }

  // Also include monitors implicated by the shell burst so automatic discovery
  // can seed a new desktop+monitor workspace.
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd)) continue;
    BOOL onCurrent = FALSE;
    if (!IsWindowOnCurrentDesktopSafe(hwnd, &onCurrent) || !onCurrent) continue;
    AddUniqueMonitor(monitors, GetWindowPhysicalMonitor(hwnd));
  }

  // One top-level enumeration is enough for every monitor involved in this burst.
  std::vector<std::vector<HWND>> snapshots = CollectTileWindowsForMonitors(monitors);
  std::vector<std::vector<HWND>> suspendedSnapshots =
      CollectDirtySuspendedWindowsForMonitors(dirtyWindows, monitors, currentDesktop);

  std::vector<DesktopMonitorKey> arrangedKeys;
  for (size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
    HMONITOR monitor = monitors[monitorIndex];
    DesktopMonitorKey key{};
    const std::vector<HWND>& snapshot = snapshots[monitorIndex];
    const std::vector<HWND>& suspendedSnapshot = suspendedSnapshots[monitorIndex];
    bool changed = DiscoverWorkspaceWindows(
        monitor, currentDesktop, snapshot, &key, &globallyChanged,
        &suspendedSnapshot);
    bool pendingArrange = std::any_of(
        g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == monitor &&
                 IsEqualGUID(pending.desktopId, currentDesktop);
        });
    if (changed || pendingArrange) {
      ArrangeWorkspace(key);
      AddUniqueWorkspaceKey(arrangedKeys, key);
    }
  }

  // Admission is intentionally decided only after the settled snapshot. If a
  // plausible Automatic-mode candidate is still unknown, its first observation
  // may have raced application initialization or shell desktop registration.
  // Give it one independent re-observation without turning discovery into polling.
  if (IsAutomaticMode() && g_settings.slowApplicationDelayMs) {
    bool addedRetry = false;
    const ULONGLONG dueTickMs =
        GetTickCount64() + g_settings.slowApplicationDelayMs;
    for (HWND hwnd : dirtyWindows) {
      if (hwnd && IsWindow(hwnd) && !IsWindowTrackedInAnyState(hwnd) &&
          GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
          GetAncestor(hwnd, GA_ROOT) == hwnd) {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (!processId) continue;

        auto existing = std::find_if(
            g_wm.pendingDiscoveryRetries.begin(),
            g_wm.pendingDiscoveryRetries.end(),
            [&](const PendingDiscoveryRetry& pending) {
              return pending.hwnd == hwnd;
            });
        if (existing != g_wm.pendingDiscoveryRetries.end()) {
          if (existing->processId == processId) continue;
          *existing = {hwnd, processId, dueTickMs};
        } else {
          g_wm.pendingDiscoveryRetries.push_back(
              {hwnd, processId, dueTickMs});
        }
        addedRetry = true;
      }
    }

    if (addedRetry) {
      ScheduleNextDiscoveryRetryTimer();
    }
  }

  for (const auto& key : globallyChanged) {
    if (std::none_of(arrangedKeys.begin(), arrangedKeys.end(),
                     [&](const DesktopMonitorKey& arranged) {
                       return SameWorkspaceKey(arranged, key);
                     })) {
      ArrangeWorkspace(key);
    }
  }

  g_wm.pendingDesktopArranges.erase(
      std::remove_if(
          g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
          [&](const PendingWorkspaceArrange& pending) {
            return IsEqualGUID(pending.desktopId, currentDesktop);
          }),
      g_wm.pendingDesktopArranges.end());

  g_wm.lifecycleRetryAfterPlatformRecovery = false;
  if (g_wm.pendingDesktopSwitchFlyouts) {
    g_wm.pendingDesktopSwitchFlyouts = false;
    TrayUi::ShowDesktopSwitchFlyouts(currentDesktop);
  }
  // Ordinary lifecycle work updates the icon/tooltip for the monitor the tray is
  // already representing. Only a foreground event or explicit workspace command
  // retargets that single shell icon to another monitor.
  TrayUi::RefreshDisplayedMonitor();
}

static void ReconcileManagedWindowStateNow(
    HWND hwnd, ReconcileScope scope = ReconcileScope::Participation) {
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(scope, &changedKeys, hwnd);
  for (const auto& key : changedKeys) ArrangeWorkspace(key);
}

// Converts queued lifecycle events into model actions: destruction is authoritative,
// state changes get immediate and settled reconciliation, while visibility and
// cloak events remain dirty signals until shell state stabilizes.
static void ProcessWindowLifecycleEvent(DWORD event, HWND hwnd) {
  ++Diagnostics::g_runtime.counters.lifecycleEventsProcessed;
  switch (event) {
    case EVENT_OBJECT_DESTROY:
      ++Diagnostics::g_runtime.counters.lifecycleDestroy;
      break;
    case EVENT_SYSTEM_MINIMIZESTART:
      ++Diagnostics::g_runtime.counters.lifecycleMinimizeStart;
      break;
    case EVENT_SYSTEM_MINIMIZEEND:
      ++Diagnostics::g_runtime.counters.lifecycleMinimizeEnd;
      break;
    case EVENT_OBJECT_STATECHANGE:
      ++Diagnostics::g_runtime.counters.lifecycleStateChange;
      break;
    case EVENT_OBJECT_LOCATIONCHANGE:
      ++Diagnostics::g_runtime.counters.lifecycleLocationChange;
      break;
    case EVENT_OBJECT_HIDE:
      ++Diagnostics::g_runtime.counters.lifecycleHide;
      break;
    case EVENT_OBJECT_SHOW:
      ++Diagnostics::g_runtime.counters.lifecycleShow;
      break;
    case EVENT_OBJECT_CLOAKED:
      ++Diagnostics::g_runtime.counters.lifecycleCloaked;
      break;
    case EVENT_OBJECT_UNCLOAKED:
      ++Diagnostics::g_runtime.counters.lifecycleUncloaked;
      break;
  }

  switch (event) {
    case EVENT_OBJECT_DESTROY:
      ForgetManagedWindow(hwnd);
      break;

    case EVENT_SYSTEM_MINIMIZESTART:
      if (IsManualMode()) {
        // Minimize is an explicit exit from an inert Manual group. Act on
        // the event itself so a very fast minimize/restore cannot outrun the
        // settled IsIconic() check and accidentally retain membership.
        ForgetManagedWindow(hwnd);
      } else {
        ReconcileManagedWindowStateNow(hwnd);
        ScheduleLifecycleReconcile(hwnd);
      }
      break;

    case EVENT_OBJECT_LOCATIONCHANGE:
      // Cross-monitor movement remains ownership migration, not conformance drift.
      // Otherwise a tiled HWND that leaves its authoritative rectangle opens or
      // reuses one bounded lease; expected SetWindowPos echoes are consumed there.
      // A native drag remains authoritative until MOVESIZEEND reconciles monitor
      // ownership for this HWND. Never probe or migrate underneath the user.
      if (IsMoveSizeGestureInProgress(hwnd)) break;
      if (!Platform::WindowEvents::HasTrackedMonitorOwnershipMismatch(hwnd) &&
          HandleTiledWindowLocationChange(hwnd)) {
        break;
      }

      // Non-tiled/state transitions and monitor migration retain ordinary
      // participation/ownership reconciliation.
      ReconcileManagedWindowStateNow(
          hwnd, ReconcileScope::Participation | ReconcileScope::Monitor);
      ScheduleLifecycleReconcile(hwnd);
      break;

    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_STATECHANGE:
      // These are prompts to query actual physical state. In Automatic mode,
      // minimize/restore preserves membership through a saved slot. Maximize and
      // fullscreen never leave the active layout; reconciliation only toggles
      // their presentation override. Run immediately and after settle.
      ReconcileManagedWindowStateNow(hwnd);
      ScheduleLifecycleReconcile(hwnd);
      break;

    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
      // These are dirty signals, not ownership decisions. The settled lifecycle
      // pass re-queries physical state and may repair this tracked HWND's VD.
      ScheduleLifecycleReconcile(hwnd);
      break;
  }
}


}  // namespace Reconcile

// Consumes and clears the WinEvent samples for one completed gesture.
// The WM core never reads these caches piecemeal after this boundary.
static MoveSizeGesture ConsumeMoveSizeGesture(HWND hwnd) {
  AssertWmThread(L"ConsumeMoveSizeGesture");
  MoveSizeGesture gesture;

  auto startIt = g_moveSize.startRects.find(hwnd);
  auto endIt = g_moveSize.endRects.find(hwnd);
  auto pointIt = g_moveSize.endPoints.find(hwnd);

  if (startIt != g_moveSize.startRects.end() && endIt != g_moveSize.endRects.end()) {
    gesture.start = startIt->second;
    gesture.end = endIt->second;
    gesture.hasRects = true;
  }
  if (pointIt != g_moveSize.endPoints.end()) {
    gesture.dropPoint = pointIt->second;
    gesture.hasDropPoint = true;
  }

  g_moveSize.startRects.erase(hwnd);
  g_moveSize.endRects.erase(hwnd);
  g_moveSize.endPoints.erase(hwnd);
  if (gesture.hasRects) {
    gesture.intent = ClassifyMoveSizeIntent(gesture.start, gesture.end, 1);
  }
  return gesture;
}

