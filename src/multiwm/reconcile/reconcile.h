// Effectful reconciliation runs on the serialized WM thread. Native calls can
// outlive HWND/request observations; copy pending values and revalidate identity
// and physical state at the mutation boundaries documented below.

namespace Reconcile {

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

constexpr ReconcileScope operator&(ReconcileScope a, ReconcileScope b) {
  return static_cast<ReconcileScope>(
      static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr bool HasReconcileScope(ReconcileScope value, ReconcileScope flag) {
  return (value & flag) != ReconcileScope::None;
}

static void ReconcileKnownWindows(
    ReconcileScope scope, std::vector<DesktopMonitorKey>* changedKeys,
    HWND onlyHwnd = nullptr);
static bool EnsureUniqueWindowOwnership(
    HWND hwnd, const DesktopMonitorKey& targetKey,
    std::vector<DesktopMonitorKey>* changedKeys = nullptr);
static void ForgetWindowFromOwners(
    HWND hwnd, const std::vector<DesktopMonitorKey>& ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys);
static void RequeueDirtyWindows(const std::vector<HWND>& dirtyWindows);

// Commit boundary for desktop-bound discovery. False schedules a settled retry
// and makes active-desktop checks query the shell; it never migrates records.
// Enumeration and COM calls can span a switch even on the serialized WM thread.
static bool ValidateSnapshotDesktop(const GUID& expectedDesktop) {
  GUID currentDesktop{};
  if (Platform::VirtualDesktop::GetCurrentDesktopId(&currentDesktop) &&
      IsEqualGUID(currentDesktop, expectedDesktop)) {
    return true;
  }
  RuntimeLifecycle::MarkDesktopTransitionPending();
  Diagnostics::RecordEvent(
      L"desktop snapshot deferred expected=%08X observed=%08X",
      expectedDesktop.Data1, currentDesktop.Data1);
  ScheduleLifecycleReconcile(nullptr);
  return false;
}

// Where a desktop switch takes effect, whether a native notification or a
// settled pass noticed it. Records `currentDesktop` as the reconciled active
// desktop and says whether that was a switch: a desktop other than the one last
// reconciled or, before any was, a native notification (only it proves a switch
// happened). On a switch, drops the old desktop's pending proofs, since a lease
// never carries across DWM's cloak/uncloak transition, and queues the switch
// flyouts for the end of the next settled pass.
static bool ObserveCurrentDesktop(const GUID& currentDesktop, bool notified) {
  const bool switched = g_wm.desktop.state == ReconciledDesktopState::Unknown
                            ? notified
                            : !IsEqualGUID(g_wm.desktop.reconciled, currentDesktop);
  if (switched) {
    Placement::DiscardAllPendingProofs();
    g_wm.desktop.pendingSwitchFlyouts = true;
  }
  g_wm.desktop.reconciled = currentDesktop;
  g_wm.desktop.state = ReconciledDesktopState::Settled;
  return switched;
}

static TileLayout ResolveInitialWorkspaceLayout(
    const DesktopMonitorKey& key, int* matchedRuleIndex = nullptr,
    const Platform::VirtualDesktop::DesktopMetadata* liveMetadata = nullptr) {
  if (matchedRuleIndex) *matchedRuleIndex = 0;
  Platform::VirtualDesktop::DesktopMetadata desktop;
  bool metadataRead = false;
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  const bool haveMonitorInfo =
      !g_settings.workspaceInitializationRules.empty() &&
      GetMonitorInfoW(key.ResolveMonitor(), &monitorInfo);

  for (const auto& rule : g_settings.workspaceInitializationRules) {
    if (!rule.monitorId.empty() && rule.monitorId != key.monitor.deviceId) continue;
    if (rule.monitor == RuleMonitor::Primary || rule.monitor == RuleMonitor::NonPrimary) {
      if (!haveMonitorInfo) continue;
      const bool primary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
      if (primary != (rule.monitor == RuleMonitor::Primary)) continue;
    }
    if (!rule.desktopNameContains.empty() || rule.desktopNumber != 0) {
      if (!metadataRead) {
        desktop = liveMetadata ? *liveMetadata : Platform::VirtualDesktop::ReadDesktopMetadata(key.desktopId);
        std::transform(
            desktop.name.begin(), desktop.name.end(), desktop.name.begin(),
            [](wchar_t ch) { return std::towlower(ch); });
        metadataRead = true;
      }
      if (rule.desktopNumber != 0 && rule.desktopNumber != desktop.number) continue;
      if (!rule.desktopNameContains.empty() &&
          desktop.name.find(rule.desktopNameContains) == std::wstring::npos) {
        continue;
      }
    }
    if (matchedRuleIndex) *matchedRuleIndex = rule.settingsIndex;
    return rule.layout;
  }
  return g_settings.defaultLayout;
}

static Workspace MakeDefaultWorkspace(const DesktopMonitorKey& key) {
  Workspace workspace;
  int matchedRuleIndex = 0;
  workspace.SetLayout(ResolveInitialWorkspaceLayout(key, &matchedRuleIndex));
  workspace.SetMasterRatio(g_settings.masterPercent / 100.0);
  if (matchedRuleIndex != 0) {
    Diagnostics::RecordEvent(
        L"workspace initial layout desktop=%08X monitorId=%ls rule=%d layout=%d",
        key.desktopId.Data1, key.monitor.deviceId.c_str(), matchedRuleIndex,
        static_cast<int>(workspace.Layout()));
  }
  return workspace;
}

static bool ReconcileTrackedSnapshotOwnership(
    const DesktopMonitorKey& key, const std::vector<HWND>& snapshot,
    std::vector<DesktopMonitorKey>* changedKeys) {
  bool changed = false;
  for (HWND hwnd : snapshot) {
    if (!Platform::Windows::IsWindowTrackedInAnyState(hwnd)) continue;

    // A snapshot can outlive a desktop transition. Visibility on the current
    // desktop is never evidence that a tracked window moved to this saved key.
    // An unavailable ownership query must preserve the existing slot and weights.
    GUID windowDesktop{};
    if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
        !IsEqualGUID(windowDesktop, key.desktopId)) {
      continue;
    }
    const auto owners = g_workspaces.OwnersOf(hwnd);
    if (RuntimeLifecycle::IsManualMode() && std::any_of(
            owners.begin(), owners.end(), [&](const DesktopMonitorKey& owner) {
              return !IsEqualGUID(owner.desktopId, windowDesktop);
            })) {
      ForgetWindowFromOwners(hwnd, owners, changedKeys);
      changed = true;
      continue;
    }
    if (EnsureUniqueWindowOwnership(hwnd, key, changedKeys)) changed = true;
  }
  return changed;
}

// --- Admission helpers shared by every discovery path ------------------------
//
// "Automatic admission policy" means the window appeared by itself while in
// Automatic mode (as opposed to an explicit Tile Workspace command). Only that
// policy honours default-to-floating rules and waits for initial-placement rules.

// True when `hwnd` is unknown to every workspace and is ready to be admitted.
static bool IsAdmissionCandidate(
    const Workspace& workspace, HWND hwnd, bool useAutomaticAdmissionPolicy) {
  if (workspace.HasRecord(hwnd) || Platform::Windows::IsWindowTrackedInAnyState(hwnd) ||
      Platform::Windows::IsWindowCloaked(hwnd)) {
    return false;
  }
  // An initial-placement rule will move this window first; admit it at its destination.
  return !(useAutomaticAdmissionPolicy && Platform::Windows::IsWindowAwaitingInitialPlacement(hwnd));
}

// Returns a Floating record when a window rule says this window starts floating,
// or no value when it should be tiled. Always-floating rules apply under every
// policy; default-to-floating rules apply only to automatic admission.
static std::optional<WindowRecord> MakeFloatingAdmissionRecord(
    HWND hwnd, bool useAutomaticAdmissionPolicy) {
  bool alwaysFloating = false;
  const bool startsFloating = Platform::Windows::ShouldWindowStartFloating(hwnd, &alwaysFloating);
  if (!alwaysFloating && !(useAutomaticAdmissionPolicy && startsFloating)) return std::nullopt;
  WindowRecord record = Platform::Windows::MakeWindowRecord(hwnd, ManageState::Floating);
  record.pendingDefaultFloatingCenter = true;
  record.alwaysFloating = alwaysFloating;
  return record;
}

static WindowRecord MakeTiledAdmissionRecord(HWND hwnd) {
  WindowRecord record = Platform::Windows::MakeWindowRecord(hwnd, ManageState::Tiled);
  record.automaticOverflowMonitorFixed = Platform::Windows::HasMonitorPlacementRule(hwnd);
  return record;
}

// True when new windows should be inserted after the focused tiled window rather
// than appended in the last slot.
static bool InsertsAfterFocusedWindow(bool useAutomaticAdmissionPolicy) {
  return useAutomaticAdmissionPolicy &&
      g_settings.automaticNewWindowPosition == AutomaticNewWindowPosition::AfterFocused;
}

// Admits one window found while creating a brand-new workspace. Hidden windows are
// skipped; minimized/maximized windows are admitted as Suspended.
static bool AdmitInitialObservedWindow(
    Workspace& workspace, HWND hwnd, bool useAutomaticAdmissionPolicy) {
  if (!hwnd || !IsWindow(hwnd) || !IsAdmissionCandidate(workspace, hwnd, useAutomaticAdmissionPolicy)) {
    return false;
  }
  const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);
  if (reason == SuspensionReason::Hidden) return false;
  if (auto floating = MakeFloatingAdmissionRecord(hwnd, useAutomaticAdmissionPolicy)) {
    return workspace.AdmitFloating(std::move(*floating));
  }
  return workspace.AdmitInitial(MakeTiledAdmissionRecord(hwnd), reason);
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
      if (workspace.Restore(hwnd)) {
        changed = true;
      }
    }
  }
  return changed;
}

// Tile Workspace: re-tiles the floating windows in `snapshot` (restored windows
// on this monitor, captured at `dpi`). Each one's current frame is remembered
// first, however it got there (the app or a keyboard snap is never recorded
// otherwise), so a window that then can't fit floats back exactly where it was.
static bool ReadoptFloatingSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot, UINT dpi) {
  bool changed = false;
  for (HWND hwnd : snapshot) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->state == ManageState::Floating) {
      RECT frame{};
      if (Platform::Windows::GetDisplayedWindowFrameRect(hwnd, &frame)) {
        workspace.RememberFloatingGeometry(hwnd, frame, dpi);
      }
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

  const bool afterFocused = InsertsAfterFocusedWindow(useAutomaticAdmissionPolicy);
  HWND anchor = afterFocused ? workspace.LastFocusedTiledWindow() : nullptr;

  bool changed = false;
  for (HWND hwnd : snapshot) {
    if (!IsAdmissionCandidate(workspace, hwnd, useAutomaticAdmissionPolicy)) continue;

    if (auto floating = MakeFloatingAdmissionRecord(hwnd, useAutomaticAdmissionPolicy)) {
      if (workspace.AdmitFloating(std::move(*floating))) {
        changed = true;
        if (admittedWindows) admittedWindows->push_back(hwnd);
      }
      continue;
    }

    WindowRecord record = MakeTiledAdmissionRecord(hwnd);
    const bool admitted = anchor ? workspace.AdmitTiledAfter(std::move(record), anchor)
                                 : workspace.AdmitTiled(std::move(record));
    if (!admitted) continue;
    changed = true;
    if (admittedWindows) admittedWindows->push_back(hwnd);

    // Preserve enumeration order in linear layouts. BSP keeps splitting the
    // focused leaf throughout the burst.
    if (afterFocused && workspace.Layout() != TileLayout::Bsp) anchor = hwnd;
  }
  return changed;
}

// Automatic discovery can observe a new window while Windows still reports it as
// minimized, maximized, or fullscreen. Such HWNDs belong to the workspace
// logically but must not enter the active tiling set until restored.
static bool AdmitUntrackedSuspendedSnapshot(
    Workspace& workspace, const std::vector<HWND>& snapshot, bool allowAdmission,
    bool useAutomaticAdmissionPolicy = false) {
  if (!allowAdmission) return false;

  const HWND anchor = InsertsAfterFocusedWindow(useAutomaticAdmissionPolicy)
      ? workspace.LastFocusedTiledWindow() : nullptr;

  auto admitOne = [&](HWND hwnd) {
    if (!IsAdmissionCandidate(workspace, hwnd, useAutomaticAdmissionPolicy)) return false;

    const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);
    if (reason != SuspensionReason::Minimized && reason != SuspensionReason::Maximized) {
      return false;
    }

    if (auto floating = MakeFloatingAdmissionRecord(hwnd, useAutomaticAdmissionPolicy)) {
      return workspace.AdmitFloating(std::move(*floating));
    }
    WindowRecord record = MakeTiledAdmissionRecord(hwnd);
    return anchor ? workspace.AdmitInitialAfter(std::move(record), reason, anchor)
                  : workspace.AdmitInitial(std::move(record), reason);
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
      !Platform::Windows::SnapshotLooksTiled(workspace.TiledWindows(), workArea)) {
    return false;
  }

  const HWND preferredMaster = workspace.Empty() ? nullptr
                                                 : workspace.TiledWindows().front();
  std::vector<WorkspaceGeometryItem> geometry = Platform::Windows::CaptureWorkspaceGeometry(
      workspace.TiledWindows(), monitor, workArea);
  for (auto& item : geometry) {
    const WindowRecord* previous = workspace.Find(item.record.hwnd);
    if (previous && previous->pid == item.record.pid) {
      item.record.lastFocusSequence = previous->lastFocusSequence;
    }
  }
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
  if (!Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) return false;
  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return false;

  const std::vector<HWND> snapshot = Platform::Windows::CollectTileWindows(monitor, key.desktopId);
  Workspace workspace;
  const bool workspaceExisted = g_workspaces.Load(key, &workspace);
  const std::vector<HWND> initialSnapshot = workspaceExisted
      ? std::vector<HWND>{}
      : Platform::Windows::CollectWorkspaceWindowsForInitialization(monitor, key.desktopId);

  if (!ValidateSnapshotDesktop(key.desktopId)) return false;
  ReconcileTrackedSnapshotOwnership(
      key, workspaceExisted ? snapshot : initialSnapshot, ownershipChangedKeys);

  if (!g_workspaces.Load(key, &workspace)) workspace = MakeDefaultWorkspace(key);
  if (!workspaceExisted) {
    SeedInitialSnapshot(
        workspace, initialSnapshot,
        RuntimeLifecycle::IsAutomaticMode() && !adoptCurrentGeometry);
  }

  // Tile Workspace may readopt current geometry, but it never bypasses native
  // application minimums. Constraint overrides belong at the application layer,
  // not in the tiling solver.

  RestoreSuspendedSnapshot(workspace, snapshot);
  if (adoptCurrentGeometry) ReadoptFloatingSnapshot(workspace, snapshot, metrics.dpi);
  AdmitUntrackedSnapshot(workspace, snapshot, true, false);

  if (adoptCurrentGeometry) {
    Placement::RefreshWorkspaceNativeConstraints(workspace, true, L"Tile Workspace");
    AdoptSnapshotGeometryIfEligible(
        workspace, snapshot, metrics.workArea, metrics.gap, monitor);
  }

  g_workspaces.Save(key, workspace);
  Placement::ArrangeWorkspace(key);
  return true;
}

// Forgets a destroyed (or, in Manual mode, minimized) window in every workspace
// that knows it, then arranges the workspaces it left.
static void ForgetManagedWindow(HWND hwnd) {
  std::vector<DesktopMonitorKey> changedKeys;
  ForgetWindowFromOwners(hwnd, g_workspaces.OwnersOf(hwnd), &changedKeys);
  for (const auto& key : changedKeys) Placement::ArrangeWorkspace(key);
}

static void AddUniqueWorkspaceKey(
    std::vector<DesktopMonitorKey>& keys, const DesktopMonitorKey& key) {
  if (!key.monitor) return;
  if (std::none_of(keys.begin(), keys.end(),
                   [&](const DesktopMonitorKey& existing) {
                     return existing == key;
                   })) {
    keys.push_back(key);
  }
}

// Transfers one managed record between repositories while allowing Workspace to
// rebase its layout-specific logical slot/weight at the destination.
static bool MigrateManagedWindow(
    HWND hwnd, const DesktopMonitorKey& sourceKey, const DesktopMonitorKey& targetKey) {
  if (!hwnd || sourceKey == targetKey) return false;
  Placement::CancelConformanceLease(hwnd);
  Placement::DiscardPendingAdmissionRecovery(hwnd);

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
    target = MakeDefaultWorkspace(targetKey);
  }

  HWND insertionAnchor = nullptr;
  if (InsertsAfterFocusedWindow(RuntimeLifecycle::IsAutomaticMode())) {
    insertionAnchor = target.LastFocusedTiledWindow();
    if (insertionAnchor == hwnd) insertionAnchor = nullptr;
  }

  target.AdmitMigrated(std::move(record), insertionAnchor);
  if (wasFocused) target.RememberFocusedWindow(hwnd);

  if (sourceLayout != TileLayout::Floating &&
      sourceState != ManageState::Floating &&
      target.Layout() == TileLayout::Floating) {
    const MoveSizeGesture gesture = Gestures::PeekMoveSizeGesture(hwnd);
    const Placement::FloatingPlacementHint hint = Placement::MakeFloatingPlacementHintFromGesture(gesture);
    // Automatic migration owns passive geometry. Manual cross-desktop moves
    // never reach this path; same-desktop Manual monitor drags may repair only
    // when the completed user gesture supplies an explicit anchor.
    if (RuntimeLifecycle::IsAutomaticMode() ||
        hint.intent == Placement::FloatingPlacementIntent::PreserveAnchor) {
      const bool repaired = Placement::RepairFloatingGeometry(targetKey, target, hwnd, hint);
      if (repaired && hint.intent == Placement::FloatingPlacementIntent::PreserveAnchor) {
        // The completed gesture has already served as migration context. Clear it
        // so a deferred desktop migration cannot leave an old drag sample behind.
        Gestures::ClearMoveSizeSamples(hwnd);
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
  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  if (owners.empty()) return false;

  const bool targetAlreadyOwns = std::any_of(
      owners.begin(), owners.end(), [&](const DesktopMonitorKey& key) {
        return key == targetKey;
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
  g_workspaces.UpdateEach(
      owners,
      [&](const DesktopMonitorKey& ownerKey, Workspace& ownerState) {
        return ownerKey != targetKey && ownerState.Forget(hwnd);
      },
      [&](const DesktopMonitorKey& ownerKey) {
        changed = true;
        if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
      });

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
    // A window shown moments ago can report GUID_NULL until the shell assigns
    // its desktop (about 10 ms, traced). That is no desktop to move it to.
    GUID desktopId{};
    if (Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &desktopId) &&
        !IsEqualGUID(desktopId, GUID_NULL)) {
      observed.desktopId = desktopId;
      observedAnything = true;
    }
  }

  if (HasReconcileScope(scope, ReconcileScope::Monitor)) {
    HMONITOR monitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
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
  Placement::CancelConformanceLease(hwnd);
  Placement::DiscardPendingAdmissionRecovery(hwnd);
  g_workspaces.UpdateEach(
      ownerKeys,
      [&](const DesktopMonitorKey&, Workspace& workspace) { return workspace.Forget(hwnd); },
      [&](const DesktopMonitorKey& ownerKey) {
        if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
      });
  Gestures::ClearMoveSizeSamples(hwnd);
}

static std::vector<DesktopMonitorKey> ReconcileWindowOwnership(
    HWND hwnd, ReconcileScope scope,
    std::vector<DesktopMonitorKey> ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys) {
  const ReconcileScope ownershipScope = scope & (ReconcileScope::Desktop | ReconcileScope::Monitor);
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
    if (RuntimeLifecycle::IsManualMode() && desktopChanged) {
      ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
      return {};
    }

    EnsureUniqueWindowOwnership(hwnd, observedKey, changedKeys);
    ownerKeys = g_workspaces.OwnersOf(hwnd);
  }
  return ownerKeys;
}

// True when a suspended window that is visible again is on another desktop than
// `ownerKey`'s, or has no desktop yet (GUID_NULL, just after it is shown). It is
// leaving that workspace: restoring it there would place it at that workspace's
// geometry first, a visible jump (a tray app restored on another desktop flashed
// at its old Floating-layout frame before its new tile, traced). A failed query
// can't tell, so it holds nothing back.
static bool AwaitsDesktopOwnership(HWND hwnd, const DesktopMonitorKey& ownerKey) {
  GUID desktopId{};
  if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &desktopId)) return false;
  return IsEqualGUID(desktopId, GUID_NULL) || !IsEqualGUID(desktopId, ownerKey.desktopId);
}

// The suspension reason that applies to `hwnd`, a member of `workspace` in
// `state`, given its physical one. A borderless tile can legitimately cover the
// monitor when the configured work area has no visible insets or gaps: a tiled
// window that only looks fullscreen because it sits exactly on its own tile is
// not maximized. Native queries; plans `workspace` without changing it.
static SuspensionReason EffectiveSuspensionReason(
    const DesktopMonitorKey& key, const Workspace& workspace, HWND hwnd, ManageState state,
    SuspensionReason physicalReason) {
  if (physicalReason != SuspensionReason::Maximized || IsZoomed(hwnd) || state != ManageState::Tiled) {
    return physicalReason;
  }
  RECT authoritative{};
  RECT current{};
  if (Placement::GetCurrentAuthoritativeTiledRect(key, workspace, hwnd, &authoritative) &&
      Platform::Windows::GetWindowFrameRect(hwnd, &current) && Layout::RectsNear(current, authoritative)) {
    return SuspensionReason::None;
  }
  return physicalReason;
}

static void ReconcileWindowParticipation(
    HWND hwnd, const std::vector<DesktopMonitorKey>& ownerKeys,
    std::vector<DesktopMonitorKey>* changedKeys) {
  const SuspensionReason physicalReason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);

  // In Manual mode, minimizing is an explicit departure from the managed group.
  // Forget the logical member entirely; restoring the HWND won't re-admit it
  // because Manual discovery remains closed. Maximization is intentionally
  // different: suspend it so restore returns to the exact saved slot/weight.
  if (RuntimeLifecycle::IsManualMode() && physicalReason == SuspensionReason::Minimized) {
    ForgetWindowFromOwners(hwnd, ownerKeys, changedKeys);
    return;
  }

  for (const auto& ownerKey : ownerKeys) {
    Workspace workspace;
    if (!g_workspaces.Load(ownerKey, &workspace)) continue;

    const WindowRecord* record = workspace.Find(hwnd);
    if (!record) continue;

    const SuspensionReason reason =
        EffectiveSuspensionReason(ownerKey, workspace, hwnd, record->state, physicalReason);

    bool changed = false;
    bool restoredIntoFloatingLayout = false;
    const bool pendingInsertionBeforeSuspension =
        record->state == ManageState::Tiled &&
        record->pendingInsertionPlacement;

    if (record->state == ManageState::Tiled &&
        reason != SuspensionReason::None) {
      Placement::CancelConformanceLease(hwnd);
      Gestures::ClearMoveSizeSamples(hwnd);
      const bool suspended = workspace.Suspend(hwnd, reason);
      changed |= suspended;
      if (suspended && pendingInsertionBeforeSuspension) {
        changed |= Placement::RestorePendingAdmissionRecoveryAfterSuspension(
            ownerKey, hwnd, &workspace);
      }
    } else if (record->state == ManageState::Suspended) {
      if (reason == SuspensionReason::None) {
        if (AwaitsDesktopOwnership(hwnd, ownerKey)) {
          // The next settled pass moves it to its desktop's workspace first,
          // then restores it there (ReconcileSettledKnownWindows).
          RequeueDirtyWindows({hwnd});
          continue;
        }
        const bool restored = workspace.Restore(hwnd);
        changed |= restored;
        restoredIntoFloatingLayout =
            restored && workspace.Layout() == TileLayout::Floating;
      } else {
        if (workspace.Suspend(hwnd, reason)) {
          changed = true;
        }
      }
    }

    if (changed) {
      // A member that was suspended while its workspace entered Floating missed
      // the layout-transition restore. Apply the same passive remembered-geometry
      // policy when it becomes active again.
      if (restoredIntoFloatingLayout) {
        Placement::RepairFloatingGeometry(
            ownerKey, workspace, hwnd, Placement::FloatingPlacementHint{});
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

  if (!IsWindow(hwnd) || Platform::Windows::IsWindowExcludedByRules(hwnd)) {
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
    return;
  }

  ownerKeys = ReconcileWindowOwnership(hwnd, scope, std::move(ownerKeys), changedKeys);
  if (ownerKeys.empty()) return;

  // Rules normally classify a window at admission. Also enforce Always floating
  // for an already-managed window after a settings reload; once applied, retain
  // it for that HWND's lifetime even if a title-based match later changes.
  if (Platform::Windows::ShouldWindowAlwaysFloat(hwnd)) {
    bool policyChanged = false;
    g_workspaces.UpdateEach(
        ownerKeys,
        [&](const DesktopMonitorKey&, Workspace& workspace) { return workspace.EnforceAlwaysFloating(hwnd); },
        [&](const DesktopMonitorKey& ownerKey) {
          policyChanged = true;
          if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
        });
    if (policyChanged) Placement::CancelConformanceLease(hwnd);
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
  if (Platform::Monitors::IsLiveMonitorHandle(monitor) &&
      std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
    monitors.push_back(monitor);
  }
}

static void AddPendingWorkspaceArrange(const GUID& desktopId, HMONITOR monitor) {
  if (!Platform::Monitors::IsLiveMonitorHandle(monitor)) return;
  // Only if not scheduled already
  auto it = std::find_if(
      g_wm.lifecycle.pendingArranges.begin(), g_wm.lifecycle.pendingArranges.end(),
      [&](const PendingWorkspaceArrange& pending) {
        return pending.monitor == monitor && IsEqualGUID(pending.desktopId, desktopId);
      });
  if (it == g_wm.lifecycle.pendingArranges.end()) {
    g_wm.lifecycle.pendingArranges.push_back({desktopId, monitor});
  }
}

static std::vector<std::vector<HWND>> CollectDirtySuspendedWindowsForMonitors(
    const std::vector<HWND>& dirtyWindows,
    const std::vector<HMONITOR>& monitors,
    const GUID& desktopId) {
  std::vector<std::vector<HWND>> snapshots(monitors.size());
  if (!RuntimeLifecycle::IsAutomaticMode() || dirtyWindows.empty() || monitors.empty()) {
    return snapshots;
  }

  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd) || Platform::Windows::IsWindowTrackedInAnyState(hwnd)) continue;
    if (!IsWindowVisible(hwnd) || !Platform::Windows::WindowCanBeManaged(hwnd) || !IsWindowEnabled(hwnd)) {
      continue;
    }

    const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);
    if (reason != SuspensionReason::Minimized &&
        reason != SuspensionReason::Maximized) {
      continue;
    }

    GUID windowDesktop{};
    if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
        !IsEqualGUID(windowDesktop, desktopId)) {
      continue;
    }

    const HMONITOR monitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
    auto it = std::find(monitors.begin(), monitors.end(), monitor);
    if (it == monitors.end()) continue;

    snapshots[static_cast<size_t>(it - monitors.begin())].push_back(hwnd);
  }

  return snapshots;
}

enum class WorkspaceDiscoveryStatus { Unavailable, DesktopChanged, Unchanged, Saved };

struct WorkspaceDiscovery {
  DesktopMonitorKey key{}; // Resolved unless status is Unavailable.
  WorkspaceDiscoveryStatus status = WorkspaceDiscoveryStatus::Unavailable;
  // Ownership repair may save other workspaces even when this one is Unchanged.
  std::vector<DesktopMonitorKey> ownershipChanges;
};

// WM thread only. Merges a desktop-bound snapshot; absence never removes a record.
// Tracked ownership is verified separately from Automatic-only new admission.
// DesktopChanged schedules a retry without importing this snapshot. Other outcomes
// may query/place native floating windows and save ownership repairs; Saved means
// the requested workspace itself was saved. Input snapshots are never retained.
static WorkspaceDiscovery DiscoverWorkspaceWindows(
    HMONITOR monitor, const GUID& desktopId, const std::vector<HWND>& snapshot,
    const std::vector<HWND>* suspendedSnapshot = nullptr) {
  WorkspaceDiscovery result;
  if (!Platform::Monitors::IsLiveMonitorHandle(monitor)) return result;

  DesktopMonitorKey& key = result.key;
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return result;

  Workspace workspace;
  const bool workspaceExisted = g_workspaces.Load(key, &workspace);
  const bool admitUntracked = RuntimeLifecycle::IsAutomaticMode();
  const bool initializing = !workspaceExisted && admitUntracked;
  const std::vector<HWND> initialSnapshot = initializing
      ? Platform::Windows::CollectWorkspaceWindowsForInitialization(monitor, desktopId)
      : std::vector<HWND>{};

  if (!ValidateSnapshotDesktop(desktopId)) {
    result.status = WorkspaceDiscoveryStatus::DesktopChanged;
    return result;
  }
  result.status = WorkspaceDiscoveryStatus::Unchanged;
  bool changed = ReconcileTrackedSnapshotOwnership(
      key, initializing ? initialSnapshot : snapshot, &result.ownershipChanges);

  bool hasState = g_workspaces.Load(key, &workspace);
  if (!hasState) workspace = MakeDefaultWorkspace(key);

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
  // is suspended. Once it appears in the active snapshot, route the retained
  // centering request through the same placement pass.
  for (HWND hwnd : snapshot) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (record && record->state == ManageState::Floating &&
        record->pendingDefaultFloatingCenter &&
        !Model::ContainsWindow(newlyAdmittedWindows, hwnd)) {
      newlyAdmittedWindows.push_back(hwnd);
    }
  }

  // Automatic discovery owns the first floating geometry of a genuinely new
  // restored window. A rule-floated admission keeps its application-selected
  // size unless a size rule overrides it, including when the whole workspace is
  // already Floating. Other Floating-workspace admissions use the configured
  // sizing policy.
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

      Placement::FloatingPlacementHint newWindowHint;
      newWindowHint.intent =
          pendingDefaultCenter
              ? Placement::FloatingPlacementIntent::RuleFloatedCenter
              : Placement::FloatingPlacementIntent::NewWindowCenter;
      if (Placement::RepairFloatingGeometry(key, workspace, hwnd, newWindowHint)) {
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

  if (!hasState || !changed) return result;
  g_workspaces.Save(key, workspace);
  result.status = WorkspaceDiscoveryStatus::Saved;
  return result;
}

// Runs the normal discovery path for one monitor. The pipeline is always active;
// DiscoverWorkspaceWindows() alone decides whether unknown HWNDs may be admitted;
// Manual-mode retention decisions are handled by the reconciliation policy.
static bool DiscoverCurrentWorkspaceOnMonitor(HMONITOR monitor) {
  if (!monitor) return false;

  GUID currentDesktop{};
  if (!Platform::VirtualDesktop::InitializeVirtualDesktopAPI() || !Platform::VirtualDesktop::GetCurrentDesktopId(&currentDesktop)) {
    return false;
  }

  std::vector<HMONITOR> monitors{monitor};
  RouteNewWindows(monitors);
  const auto snapshots = Platform::Windows::CollectTileWindowsForMonitors(monitors, currentDesktop);
  bool arranged = false;
  for (size_t i = 0; i < monitors.size(); ++i) {
    DesktopMonitorKey key{};
    if (!DesktopMonitorKey::FromHMonitor(currentDesktop, monitors[i], &key)) continue;
    const auto discovery =
        DiscoverWorkspaceWindows(monitors[i], currentDesktop, snapshots[i]);

    // An unchanged workspace may still need its existing layout reinforced.
    if (discovery.status != WorkspaceDiscoveryStatus::Saved && !g_workspaces.Contains(key)) continue;
    Placement::ArrangeWorkspace(key);
    arranged = true;
  }
  return arranged;
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
// that transient cloak bit. Known membership therefore survives the transition;
// the admission boundary defers unknown cloaked HWNDs to the settled pass.
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

        if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) return TRUE;
        if (!Platform::Windows::WindowCanBeManaged(hwnd)) return TRUE;
        if (!Platform::Windows::IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        if (Platform::Windows::GetWindowPhysicalMonitor(hwnd) == ctx->targetMonitor) {
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
  if (!Platform::VirtualDesktop::InitializeVirtualDesktopAPI() || !Platform::VirtualDesktop::GetCurrentDesktopId(&currentDesktop)) {
    // The native callback is an authoritative transition signal, but without a
    // current GUID we cannot prove that an existing placement lease still belongs
    // to the active desktop. Drop those short-lived contracts and retry normally.
    Placement::DiscardAllPendingProofs();
    Wh_Log(L"Native VD change received but current desktop query failed");
    Diagnostics::RecordEvent(L"virtual desktop notification: current desktop query failed");
    ScheduleLifecycleReconcile(nullptr);
    return;
  }

  Diagnostics::RecordEvent(
      L"virtual desktop changed currentDesktop=%08X", currentDesktop.Data1);
  // Duplicate callbacks for the desktop already reconciled are not a switch, so
  // they queue no second flyout. The incoming settled pass establishes fresh
  // leases for any placements it has to issue.
  ObserveCurrentDesktop(currentDesktop, true);

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
    if (g_workspaces.Contains(knownKey)) {
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  for (HMONITOR monitor : monitors) {
    std::vector<HWND> snapshot = GetTileWindowsAfterDesktopSwitch(monitor, currentDesktop);
    const auto discovery = DiscoverWorkspaceWindows(monitor, currentDesktop, snapshot);
    if (discovery.status == WorkspaceDiscoveryStatus::Saved) {
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
  if (hwnd && std::find(g_wm.lifecycle.dirtyWindows.begin(), g_wm.lifecycle.dirtyWindows.end(), hwnd) ==
                  g_wm.lifecycle.dirtyWindows.end()) {
    g_wm.lifecycle.dirtyWindows.push_back(hwnd);
  }

  if (g_wm.timers.lifecycle) ++Diagnostics::g_runtime.counters.lifecycleTimerResets;
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.lifecycle);

  g_wm.timers.lifecycle = SetTimer(nullptr, 0, g_settings.reconcileDelayMs, nullptr);
  if (!g_wm.timers.lifecycle) {
    Wh_Log(L"Failed to schedule lifecycle timer; queueing immediate reconciliation");
    const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
    if (threadId) {
      PostThreadMessage(threadId, WM_APP_RECONCILE_NOW, 0, 0);
    }
  }
}

static void ScheduleNextDiscoveryRetryTimer() {
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.discoveryRetry);
  if (g_wm.lifecycle.discoveryRetries.empty()) return;

  const ULONGLONG now = GetTickCount64();
  ULONGLONG earliestDue = g_wm.lifecycle.discoveryRetries.front().dueTickMs;
  for (const auto& pending : g_wm.lifecycle.discoveryRetries) {
    earliestDue = std::min(earliestDue, pending.dueTickMs);
  }
  const UINT delayMs = earliestDue > now
                           ? static_cast<UINT>(earliestDue - now)
                           : 1;
  g_wm.timers.discoveryRetry = SetTimer(nullptr, 0, delayMs, nullptr);
  if (!g_wm.timers.discoveryRetry) {
    Wh_Log(L"Failed to schedule slow-application discovery timer");
    g_wm.lifecycle.discoveryRetries.clear();
  }
}

// This stage owns only untracked HWNDs. Once discovery succeeds,
// ArrangeWorkspace establishes any needed post-placement conformance lease.
static void ProcessDiscoveryRetryTimer() {
  RuntimeLifecycle::AssertWmThread(L"ProcessDiscoveryRetryTimer");
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.discoveryRetry);
  if (!RuntimeLifecycle::IsAutomaticMode() && !g_wm.routing.ready) {
    g_wm.lifecycle.discoveryRetries.clear();
    return;
  }

  const ULONGLONG now = GetTickCount64();
  std::vector<PendingDiscoveryRetry> due;
  g_wm.lifecycle.discoveryRetries.erase(
      std::remove_if(
          g_wm.lifecycle.discoveryRetries.begin(),
          g_wm.lifecycle.discoveryRetries.end(),
          [&](const PendingDiscoveryRetry& pending) {
            if (pending.dueTickMs > now) return false;
            due.push_back(pending);
            return true;
          }),
      g_wm.lifecycle.discoveryRetries.end());
  ScheduleNextDiscoveryRetryTimer();

  std::vector<HMONITOR> monitors;
  for (const auto& pending : due) {
    if (!pending.hwnd || !pending.processId ||
        !Platform::Windows::IsSameLiveWindow(pending.hwnd, pending.processId) ||
        Platform::Windows::IsWindowTrackedInAnyState(pending.hwnd)) {
      continue;
    }
    AddUniqueMonitor(monitors,
                     Platform::Windows::GetCurrentManagedWindowMonitor(pending.hwnd));
  }
  RouteNewWindows(monitors);
  for (HMONITOR monitor : monitors) DiscoverCurrentWorkspaceOnMonitor(monitor);
  if (!g_wm.overflow.queue.empty()) ScheduleLifecycleReconcile(nullptr);
}

// --- Settled lifecycle pass: stages ------------------------------------------

// Puts dirty windows back so the next pass handles them again (no duplicates).
static void RequeueDirtyWindows(const std::vector<HWND>& dirtyWindows) {
  for (HWND hwnd : dirtyWindows) {
    if (hwnd && !Model::ContainsWindow(g_wm.lifecycle.dirtyWindows, hwnd)) {
      g_wm.lifecycle.dirtyWindows.push_back(hwnd);
    }
  }
}

// Native notifications are primary, but a settled cloak/uncloak burst can
// independently detect a missed subscription/callback. ObserveCurrentDesktop has
// already made the switch take effect; this records that the notification missed it.
static void RecordMissedDesktopSwitch(const GUID& currentDesktop) {
  ++Diagnostics::g_runtime.counters.virtualDesktopFallbackChanges;
  Diagnostics::RecordEvent(
      L"virtual desktop change recovered from settled WinEvents currentDesktop=%08X",
      currentDesktop.Data1);
  // A missed native callback is also a high-confidence signal that an exhausted
  // subscription retry burst deserves one fresh attempt sequence.
  if (g_vd.abi.notification.methodCount > 0 && !g_vd.notificationsRegistered) {
    RuntimeLifecycle::RequestMaintenance(true);
  }
}

// Chooses the monitors this pass must discover on. After a desktop switch or a
// display change, that is every connected monitor (and existing workspaces are
// re-arranged); otherwise only monitors with a known workspace. Either way, add
// monitors showing a dirty window, so a new desktop+monitor workspace can start.
// May reconcile monitor ownership (topology changes) into `globallyChanged`.
static std::vector<HMONITOR> CollectMonitorsForPass(
    const GUID& currentDesktop, bool desktopSwitched, const std::vector<HWND>& dirtyWindows,
    std::vector<DesktopMonitorKey>* globallyChanged) {
  std::vector<HMONITOR> monitors;
  if (desktopSwitched || g_wm.lifecycle.forceMonitorReconcile) {
    // Authoritative desktop/topology passes derive their boundary set from the OS,
    // not from whatever workspace keys happened to exist before the transition.
    EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      DesktopMonitorKey key{};
      if (!DesktopMonitorKey::FromHMonitor(currentDesktop, monitor, &key)) continue;
      if (g_workspaces.Contains(key)) AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  } else {
    monitors = g_workspaces.MonitorsOnDesktop(currentDesktop);
  }

  if (g_wm.lifecycle.forceMonitorReconcile) {
    ReconcileKnownWindows(ReconcileScope::Monitor, globallyChanged);
    g_wm.lifecycle.forceMonitorReconcile = false;
  }

  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd)) continue;
    BOOL onCurrent = FALSE;
    if (!Platform::VirtualDesktop::IsWindowOnCurrentDesktopSafe(hwnd, &onCurrent) || !onCurrent) continue;
    AddUniqueMonitor(monitors, Platform::Windows::GetWindowPhysicalMonitor(hwnd));
  }
  return monitors;
}

// Runs discovery for each monitor's snapshot and arranges every workspace that
// was saved or has a pending arrange request. Returns the keys it arranged;
// ownership repairs found along the way are added to `globallyChanged`.
static std::vector<DesktopMonitorKey> DiscoverAndArrangeMonitors(
    const GUID& currentDesktop, const std::vector<HMONITOR>& monitors,
    const std::vector<std::vector<HWND>>& snapshots,
    const std::vector<std::vector<HWND>>& suspendedSnapshots,
    std::vector<DesktopMonitorKey>* globallyChanged) {
  std::vector<DesktopMonitorKey> arrangedKeys;
  for (size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
    const HMONITOR monitor = monitors[monitorIndex];
    const auto discovery = DiscoverWorkspaceWindows(
        monitor, currentDesktop, snapshots[monitorIndex], &suspendedSnapshots[monitorIndex]);
    for (const auto& key : discovery.ownershipChanges) AddUniqueWorkspaceKey(*globallyChanged, key);
    const bool pendingArrange = std::any_of(
        g_wm.lifecycle.pendingArranges.begin(), g_wm.lifecycle.pendingArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == monitor && IsEqualGUID(pending.desktopId, currentDesktop);
        });
    if (discovery.status == WorkspaceDiscoveryStatus::Saved || pendingArrange) {
      Placement::ArrangeWorkspace(discovery.key);
      AddUniqueWorkspaceKey(arrangedKeys, discovery.key);
    }
  }
  return arrangedKeys;
}

// Admission is intentionally decided only after the settled snapshot. If a
// plausible candidate is still unknown, its first observation may have raced
// application initialization or shell desktop registration. Give it one
// independent re-observation after SlowApplicationDelayMs, without polling.
static void ScheduleDiscoveryRetries(const std::vector<HWND>& dirtyWindows) {
  if (!(RuntimeLifecycle::IsAutomaticMode() || g_wm.routing.ready) ||
      !g_settings.slowApplicationDelayMs) {
    return;
  }
  bool addedRetry = false;
  const ULONGLONG dueTickMs = GetTickCount64() + g_settings.slowApplicationDelayMs;
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd) || Platform::Windows::IsWindowTrackedInAnyState(hwnd) ||
        !(RuntimeLifecycle::IsAutomaticMode() || !g_wm.routing.evaluated.count(hwnd)) ||
        Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
        GetAncestor(hwnd, GA_ROOT) != hwnd) {
      continue;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (!processId) continue;

    auto existing = std::find_if(
        g_wm.lifecycle.discoveryRetries.begin(), g_wm.lifecycle.discoveryRetries.end(),
        [&](const PendingDiscoveryRetry& pending) { return pending.hwnd == hwnd; });
    if (existing != g_wm.lifecycle.discoveryRetries.end()) {
      if (existing->processId == processId) continue;
      *existing = {hwnd, processId, dueTickMs};
    } else {
      g_wm.lifecycle.discoveryRetries.push_back({hwnd, processId, dueTickMs});
    }
    addedRetry = true;
  }
  if (addedRetry) ScheduleNextDiscoveryRetryTimer();
}

// The settled pass's known-window step. Desktop ownership comes first: dirty
// tracked HWNDs (every known one after a missed switch) get a desktop check,
// since per-window moves emit no desktop change. A hidden window shown on
// another desktop then moves to that desktop's workspace before participation
// restores it there, never at its old workspace's geometry first.
static void ReconcileSettledKnownWindows(
    const std::vector<HWND>& dirtyWindows, bool desktopSwitched,
    std::vector<DesktopMonitorKey>* changedKeys) {
  if (desktopSwitched) ReconcileKnownWindows(ReconcileScope::Desktop, changedKeys);
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !Platform::Windows::IsWindowTrackedInAnyState(hwnd)) continue;
    ReconcileKnownWindows(ReconcileScope::Desktop, changedKeys, hwnd);
  }
  ReconcileKnownWindows(ReconcileScope::Participation, changedKeys);
}

// After a lifecycle burst settles, reconcile tracked-window physical and desktop
// state, discover eligible windows only on the current desktop, then arrange each
// changed or pending workspace once and refresh the tray.
//
// If the active desktop changes while this runs, the pass stops and requeues its
// work; nothing it already committed is lost. WM thread only.
static void ReconcileDeferredLifecycle() {
  ++Diagnostics::g_runtime.counters.lifecycleReconciles;
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.lifecycle);

  std::vector<HWND> dirtyWindows;
  dirtyWindows.swap(g_wm.lifecycle.dirtyWindows);
  Diagnostics::RecordLifecycleBurst(dirtyWindows.size(), g_wm.lifecycle.pendingArranges.size());

  // 1. Resolve desktop identity before mutating model state. If Explorer is still
  // coming up (or restarting), preserve the dirty set and let the single runtime
  // maintenance owner recover COM before this exact work is retried.
  GUID currentDesktop{};
  if (!Platform::VirtualDesktop::InitializeVirtualDesktopAPI() ||
      !Platform::VirtualDesktop::GetCurrentDesktopId(&currentDesktop)) {
    RequeueDirtyWindows(dirtyWindows);
    g_wm.lifecycle.retryAfterPlatformRecovery = true;
    RuntimeLifecycle::RequestMaintenance(false);
    Wh_Log(L"Lifecycle reconcile deferred: current virtual desktop unavailable");
    return;
  }
  Diagnostics::g_runtime.counters.lifecycleDirtyWindowsProcessed += dirtyWindows.size();

  // A switch the native notification already observed is not seen again here.
  // Leases created on the old desktop are dropped before ownership repair and
  // incoming-workspace work.
  const bool missedDesktopSwitch = ObserveCurrentDesktop(currentDesktop, false);
  if (missedDesktopSwitch) RecordMissedDesktopSwitch(currentDesktop);

  // 2. Repair ownership, liveness/exclusions and participation globally before
  // doing current-desktop discovery.
  std::vector<DesktopMonitorKey> globallyChanged;
  ReconcileSettledKnownWindows(dirtyWindows, missedDesktopSwitch, &globallyChanged);

  // 3. Snapshot the monitors involved (one top-level enumeration for all).
  std::vector<HMONITOR> monitors =
      CollectMonitorsForPass(currentDesktop, missedDesktopSwitch, dirtyWindows, &globallyChanged);
  RouteNewWindows(monitors);
  const std::vector<std::vector<HWND>> snapshots =
      Platform::Windows::CollectTileWindowsForMonitors(monitors, currentDesktop);
  const std::vector<std::vector<HWND>> suspendedSnapshots =
      CollectDirtySuspendedWindowsForMonitors(dirtyWindows, monitors, currentDesktop);

  // The snapshots are only valid while the desktop has not changed. On a change,
  // retain dirty suspended admissions and already-committed ownership changes.
  auto snapshotStillCurrent = [&] {
    if (ValidateSnapshotDesktop(currentDesktop)) return true;
    RequeueDirtyWindows(dirtyWindows);
    for (const auto& key : globallyChanged) AddPendingWorkspaceArrange(key.desktopId, key.ResolveMonitor());
    return false;
  };
  if (!snapshotStillCurrent()) return;

  // 4. Discover, arrange, and give slow new windows one more look.
  const std::vector<DesktopMonitorKey> arrangedKeys = DiscoverAndArrangeMonitors(
      currentDesktop, monitors, snapshots, suspendedSnapshots, &globallyChanged);
  ScheduleDiscoveryRetries(dirtyWindows);
  for (const auto& key : globallyChanged) {
    if (std::none_of(arrangedKeys.begin(), arrangedKeys.end(),
                     [&](const DesktopMonitorKey& arranged) { return arranged == key; })) {
      Placement::ArrangeWorkspace(key);
    }
  }

  // 5. Discovery/placement can itself take time. Do not consume pending work for
  // a pass overtaken by another switch.
  if (!snapshotStillCurrent()) return;
  std::erase_if(g_wm.lifecycle.pendingArranges, [&](const PendingWorkspaceArrange& pending) {
    return IsEqualGUID(pending.desktopId, currentDesktop);
  });

  g_wm.lifecycle.retryAfterPlatformRecovery = false;
  if (g_wm.desktop.pendingSwitchFlyouts) {
    g_wm.desktop.pendingSwitchFlyouts = false;
    TrayUi::ShowDesktopSwitchFlyouts(currentDesktop);
  }
  ProcessWindowOverflow();
  FollowInitiallyPlacedWindow();
  // Ordinary lifecycle work updates the icon/tooltip for the monitor the tray is
  // already representing. Only a foreground event or explicit workspace command
  // retargets that single shell icon to another monitor.
  TrayUi::RefreshDisplayedMonitor();
}
static void ReconcileManagedWindowStateNow(
    HWND hwnd, ReconcileScope scope = ReconcileScope::Participation) {
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(scope, &changedKeys, hwnd);
  for (const auto& key : changedKeys) Placement::ArrangeWorkspace(key);
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
      if (RuntimeLifecycle::IsManualMode()) {
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
      if (Gestures::IsMoveSizeGestureInProgress(hwnd)) break;
      if (!Platform::WindowEvents::HasTrackedMonitorOwnershipMismatch(hwnd) &&
          Placement::HandleTiledWindowLocationChange(hwnd)) {
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
      // minimize/maximize/fullscreen transitions preserve membership through a
      // saved slot. Run immediately and after settle.
      ReconcileManagedWindowStateNow(hwnd);
      // Ghost creation, size changes and recovery can change tile geometry
      // without changing participation.
      if (event == EVENT_OBJECT_STATECHANGE && Platform::Windows::IsWindowTrackedInAnyState(hwnd)) {
        const auto owner = Placement::LoadConformanceWorkspace(hwnd);
        const WindowRecord* record = owner ? owner->workspace.Find(hwnd) : nullptr;
        if (record && owner->workspace.IsTiled(hwnd) &&
            owner->workspace.Layout() != TileLayout::Floating) {
          const SIZE size = Platform::Windows::GetRestoredGhostSize(hwnd);
          if (!Gestures::IsMoveSizeGestureInProgress(hwnd) &&
              (size.cx != record->ghostSize.cx || size.cy != record->ghostSize.cy ||
               (!size.cx && record->lastPlacementResult == PlacementResult::Unresponsive &&
                !IsHungAppWindow(hwnd)))) {
            Placement::ArrangeWorkspace(owner->key);
          }
        }
      }
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

