//=============================================================================
// User commands
//=============================================================================
namespace Commands {

// User-authored geometry changes are rejected before commit when existing
// evidence already proves them impossible. A short-lived transaction handles a
// minimum that an application reveals only during the resulting placement.
static bool ValidateUserLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor,
    Workspace& candidate, HWND subject, UserLayoutMutationKind kind,
    const wchar_t* action) {
  WorkspaceMetrics metrics;
  std::vector<RECT> rects;
  LayoutPlanStatus status = LayoutPlanStatus::Unsatisfiable;
  if (GetWorkspaceMetrics(monitor, &metrics)) {
    ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
    status = BuildWorkspaceLayoutPlan(
        candidate, metrics.workArea, metrics.gap, &rects);
    if (status == LayoutPlanStatus::Unsatisfiable &&
        kind == UserLayoutMutationKind::Order) {
      Workspace previous;
      if (g_workspaces.Load(key, &previous) &&
          IsAdaptiveDwindleExchange(previous, candidate) &&
          FitAdaptiveDwindleDividers(candidate, metrics.workArea, metrics.gap)) {
        Diagnostics::RecordEvent(L"adaptive dwindle exchange accommodated action=%ls", action);
        return true;
      }
    }
  }
  if (status == LayoutPlanStatus::Success ||
      status == LayoutPlanStatus::NotApplicable) {
    return true;
  }

  ReportRejectedLayoutMutation(
      key, monitor, kind, action ? action : L"Layout mutation", subject,
      candidate, rects);
  return false;
}

// Promotes the foreground window, or its tiled owner, to the master slot, then
// clears stale gesture samples for both windows and reflows the workspace.
void SwapMaster() {
  ++Diagnostics::g_runtime.counters.swapMasterCommands;
  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR monitor = GetWindowPhysicalMonitor(fg);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return;
  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace) || workspace.ActiveCount() < 2 ||
      workspace.Layout() == TileLayout::Floating) {
    return;
  }

  HWND resolved = ResolveToTiledWindow(fg, workspace.TiledWindows());
  Workspace candidate = workspace;
  HWND oldMaster = nullptr;
  if (!resolved || !candidate.MakeMaster(resolved, &oldMaster)) return;
  if (!ValidateUserLayoutMutation(
          key, monitor, candidate, resolved, UserLayoutMutationKind::Order,
          L"Set master")) {
    return;
  }

  RememberPendingLayoutMutation(
      key, workspace, candidate, resolved, UserLayoutMutationKind::Order,
      L"Set master");
  g_workspaces.Save(key, candidate);
  ClearMoveSizeSamples(oldMaster);
  ClearMoveSizeSamples(resolved);
  ArrangeWorkspace(key, resolved);
  Diagnostics::RecordEvent(
      L"Swap Master old=%p new=%p", oldMaster, resolved);
}

// Moves the focused tiled window one slot toward or away from the front of the
// workspace's logical order. The operation is deliberately non-wrapping so the
// first and last slots remain stable boundaries.
static void MoveFocusedWindowOneSlot(bool promote) {
  if (promote) {
    ++Diagnostics::g_runtime.counters.promoteWindowCommands;
  } else {
    ++Diagnostics::g_runtime.counters.demoteWindowCommands;
  }

  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR monitor = GetWindowPhysicalMonitor(fg);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return;

  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace) || workspace.ActiveCount() < 2 ||
      workspace.Layout() == TileLayout::Floating) {
    return;
  }

  HWND resolved = ResolveToTiledWindow(fg, workspace.TiledWindows());
  if (!resolved) return;

  const size_t index = workspace.TiledIndex(resolved);
  const size_t count = workspace.ActiveCount();
  if (index >= count) return;

  if ((promote && index == 0) || (!promote && index + 1 >= count)) {
    return;
  }

  const size_t otherIndex = promote ? index - 1 : index + 1;
  HWND other = workspace.TiledWindows()[otherIndex];
  Workspace candidate = workspace;
  if (!candidate.SwapTiled(resolved, other)) return;
  if (!ValidateUserLayoutMutation(
          key, monitor, candidate, resolved, UserLayoutMutationKind::Order,
          promote ? L"Promote" : L"Demote")) {
    return;
  }

  RememberPendingLayoutMutation(
      key, workspace, candidate, resolved, UserLayoutMutationKind::Order,
      promote ? L"Promote" : L"Demote");
  g_workspaces.Save(key, candidate);
  ClearMoveSizeSamples(resolved);
  ClearMoveSizeSamples(other);
  ArrangeWorkspace(key, resolved);

  Diagnostics::RecordEvent(
      promote ? L"Promote Window hwnd=%p from=%zu to=%zu"
              : L"Demote Window hwnd=%p from=%zu to=%zu",
      resolved, index, otherIndex);
}

void PromoteFocusedWindow() {
  MoveFocusedWindowOneSlot(true);
}

void DemoteFocusedWindow() {
  MoveFocusedWindowOneSlot(false);
}

void FloatFocusedWindow() {
  ++Diagnostics::g_runtime.counters.floatFocusedCommands;
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return;

  HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
  GUID desktopId{};
  if (!monitor || !GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return;
  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace)) return;

  HWND tiled = ResolveToTiledWindow(hwnd, workspace.TiledWindows());
  if (!tiled) return;

  RECT before{};
  FloatingPlacementHint hint;
  if (GetWindowFrameRect(tiled, &before) && before.right > before.left &&
      before.bottom > before.top) {
    hint.intent = FloatingPlacementIntent::PreserveAnchor;
    hint.anchor = RectCenter(before);
  }

  CancelConformanceLease(tiled);
  if (!workspace.Float(tiled)) return;
  RepairFloatingGeometry(key, workspace, tiled, hint);
  g_workspaces.Save(key, workspace);
  ArrangeWorkspace(key);
  Wh_Log(L"Focused window floated %p", tiled);
  Diagnostics::RecordEvent(L"Float Focused hwnd=%p", tiled);
}

// Manual tile: reconcile membership and explicitly adopt the user's current
// geometry. Automatic events never call this with adoptCurrentGeometry=true.
void TileWindows(HMONITOR monitor = nullptr) {
  ++Diagnostics::g_runtime.counters.tileCommands;
  if (!monitor) monitor = GetWorkspaceCommandMonitor();
  if (!monitor) return;

  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::All, &changedKeys);

  DesktopMonitorKey targetKey{};
  if (!GetCurrentWorkspaceKey(monitor, &targetKey)) return;
  if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, true, &changedKeys)) return;

  // Ensure other affected current workspaces also lose stale slots. Keep the
  // target workspace until after geometry adoption so manual tiling does not
  // overwrite the user's geometry before capturing it.
  for (const auto& key : changedKeys) {
    if (!Reconcile::SameWorkspaceKey(key, targetKey)) ArrangeWorkspace(key);
  }

  TrayUi::RefreshForMonitor(monitor);
  Wh_Log(L"Manual tile/global reconcile completed");
  Diagnostics::RecordEvent(L"Tile Workspace command completed monitor=%p", monitor);
}

// Advances the current workspace layout and invalidates layout-specific hints in
// suspended slots before persisting, arranging, and updating UI feedback.
void CycleCurrentWorkspaceLayout(HMONITOR monitor = nullptr) {
  ++Diagnostics::g_runtime.counters.layoutCycleCommands;
  if (!monitor) monitor = GetWorkspaceCommandMonitor();
  if (!monitor) return;
  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return;

  Workspace state;
  if (!g_workspaces.Load(key, &state)) {
    if (IsAutomaticMode()) {
      if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, false) ||
          !g_workspaces.Load(key, &state)) {
        return;
      }
    } else {
      state = Reconcile::MakeDefaultWorkspace();
    }
  }

  const TileLayout previousLayout = state.Layout();
  Workspace candidate = state;
  if (!candidate.CycleLayout(g_settings.layoutCycle)) return;
  if (!ValidateUserLayoutMutation(
          key, monitor, candidate, nullptr,
          UserLayoutMutationKind::LayoutChange, L"Change layout")) {
    return;
  }
  RememberPendingLayoutMutation(
      key, state, candidate, nullptr, UserLayoutMutationKind::LayoutChange,
      L"Change layout");
  state = std::move(candidate);
  if (previousLayout != TileLayout::Floating &&
      state.Layout() == TileLayout::Floating) {
    RestoreWorkspaceFloatingGeometry(key, state);
  }
  g_workspaces.Save(key, state);
  ArrangeWorkspace(key);
  TrayUi::UpdateIcon(state.Layout(), monitor);
  TrayUi::ShowLayoutFlyout(state.Layout(), monitor);
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(state.Layout()));
  Diagnostics::RecordEvent(
      L"layout cycle desktop=%08X monitorId=%ls from=%d to=%d", key.desktopId.Data1,
      key.monitor.deviceId.c_str(), static_cast<int>(previousLayout),
      static_cast<int>(state.Layout()));
}

// Direct layout selection used by the tray context menu. This is intentionally
// separate from CycleCurrentWorkspaceLayout: selecting a named layout should not
// depend on its current position in the configured cycle.
void SetCurrentWorkspaceLayout(TileLayout layout, HMONITOR monitor = nullptr) {
  if (layout == TileLayout::COUNT) return;
  if (!monitor) monitor = GetWorkspaceCommandMonitor();
  if (!monitor) return;

  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return;

  Workspace state;
  bool workspaceCreated = false;
  if (!g_workspaces.Load(key, &state)) {
    if (IsAutomaticMode()) {
      if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, false) ||
          !g_workspaces.Load(key, &state)) {
        return;
      }
    } else {
      state = Reconcile::MakeDefaultWorkspace();
      workspaceCreated = true;
    }
  }

  const TileLayout previousLayout = state.Layout();
  if (previousLayout == layout) {
    if (workspaceCreated) g_workspaces.Save(key, state);
    // Keep the tray view coherent even if the selected item was already active.
    TrayUi::UpdateIcon(layout, monitor);
    return;
  }

  Workspace candidate = state;
  candidate.SetLayout(layout);
  if (!ValidateUserLayoutMutation(
          key, monitor, candidate, nullptr,
          UserLayoutMutationKind::LayoutChange, L"Change layout")) {
    return;
  }
  RememberPendingLayoutMutation(
      key, state, candidate, nullptr, UserLayoutMutationKind::LayoutChange,
      L"Change layout");
  state = std::move(candidate);
  if (previousLayout != TileLayout::Floating && layout == TileLayout::Floating) {
    RestoreWorkspaceFloatingGeometry(key, state);
  }
  g_workspaces.Save(key, state);
  ArrangeWorkspace(key);
  TrayUi::UpdateIcon(layout, monitor);
  TrayUi::ShowLayoutFlyout(layout, monitor);
  Wh_Log(L"Workspace layout set to %d", static_cast<int>(layout));
  Diagnostics::RecordEvent(
      L"layout set desktop=%08X monitorId=%ls from=%d to=%d", key.desktopId.Data1,
      key.monitor.deviceId.c_str(), static_cast<int>(previousLayout),
      static_cast<int>(layout));
}

static void HandleUserMove(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const MoveSizeGesture& gesture) {
  if (g_settings.mouseMoveBehavior == MouseMoveBehavior::Float) {
    CancelConformanceLease(hwnd);
    if (workspace.Float(hwnd)) {
      RepairFloatingGeometry(
          key, workspace, hwnd, MakeFloatingPlacementHintFromGesture(gesture));
      g_workspaces.Save(key, workspace);
      ++Diagnostics::g_runtime.counters.tiledFloatActions;
      Wh_Log(L"User move floated managed window %p", hwnd);
      Diagnostics::RecordEvent(L"user drag floated hwnd=%p", hwnd);
    }
    ArrangeWorkspace(key);
    return;
  }

  HWND target = nullptr;
  HWND preferredTiledWindow = nullptr;
  if (workspace.IsTiled(hwnd) && gesture.hasDropPoint) {
    for (HWND candidate : workspace.TiledWindows()) {
      if (candidate == hwnd) continue;
      RECT rect{};
      if (GetWindowFrameRect(candidate, &rect) && PtInRect(&rect, gesture.dropPoint)) {
        target = candidate;
        break;
      }
    }
  }

  if (target) {
    Workspace candidate = workspace;
    if (candidate.SwapTiled(hwnd, target) &&
        ValidateUserLayoutMutation(
            key, key.ResolveMonitor(), candidate, hwnd,
            UserLayoutMutationKind::Order, L"Swap")) {
      RememberPendingLayoutMutation(
          key, workspace, candidate, hwnd, UserLayoutMutationKind::Order,
          L"Swap");
      workspace = std::move(candidate);
      preferredTiledWindow = hwnd;
      g_workspaces.Save(key, workspace);
      ++Diagnostics::g_runtime.counters.tiledSwapActions;
      Wh_Log(L"User move swapped managed windows %p and %p", hwnd, target);
      Diagnostics::RecordEvent(
          L"user drag swapped hwnd=%p target=%p", hwnd, target);
    }
  }

  // No target means the source remains tiled and simply snaps back to its slot.
  ArrangeWorkspace(key, preferredTiledWindow);
}

static void HandleUserResize(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const RECT& workArea, LONG gap, const MoveSizeGesture& gesture) {
  if (!gesture.hasRects) {
    ArrangeWorkspace(key);
    return;
  }

  const size_t resizedIndex = workspace.TiledIndex(hwnd);
  if (resizedIndex == static_cast<size_t>(-1)) {
    ArrangeWorkspace(key);
    return;
  }

  Workspace candidate = workspace;
  bool changed = false;
  switch (candidate.Layout()) {
    case TileLayout::MasterStack:
    case TileLayout::MasterStackH:
      changed = candidate.LearnMasterStackResize(
          workArea, gap, resizedIndex, gesture);
      break;

    case TileLayout::Columns:
    case TileLayout::Rows:
      changed = candidate.LearnGridResize(
          workArea, gap, resizedIndex, gesture);
      break;

    case TileLayout::COUNT:
      return;

    case TileLayout::Dwindle:
      changed = candidate.LearnDwindleResize(
          workArea, gap, resizedIndex, gesture);
      break;

    case TileLayout::Monocle:
    case TileLayout::Floating:
      break;
  }

  if (changed) {
    if (!ValidateUserLayoutMutation(
            key, key.ResolveMonitor(), candidate, hwnd,
            UserLayoutMutationKind::Resize, L"Resize")) {
      ArrangeWorkspace(key, hwnd);
      return;
    }
    RememberPendingLayoutMutation(
        key, workspace, candidate, hwnd, UserLayoutMutationKind::Resize,
        L"Resize");
    workspace = std::move(candidate);
    g_workspaces.Save(key, workspace);
    ++Diagnostics::g_runtime.counters.dividerUpdates;
    Wh_Log(L"User resize updated local divider state for %p", hwnd);
    Diagnostics::RecordEvent(L"user resize updated divider hwnd=%p", hwnd);
  }
  ArrangeWorkspace(key, changed ? hwnd : nullptr);
}

// Consumes one completed user gesture and routes it to the move or resize policy.
// The top-level handler deliberately contains no layout-specific math.
void ApplyUserMoveSize(HWND hwnd) {
  if (!IsWindow(hwnd)) return;

  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    ClearMoveSizeSamples(hwnd);
    return;
  }

  HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return;
  Workspace state;
  if (!g_workspaces.Load(key, &state)) return;

  // Classify every completed move/size gesture at the common boundary before
  // routing it to tiled or floating policy. This makes the counters describe
  // actual user gestures instead of only gestures that reached tiled handlers.
  MoveSizeGesture gesture = ConsumeMoveSizeGesture(hwnd);
  switch (gesture.intent) {
    case MoveSizeIntent::Move:
      ++Diagnostics::g_runtime.counters.userMoveGestures;
      break;
    case MoveSizeIntent::Resize:
      ++Diagnostics::g_runtime.counters.userResizeGestures;
      break;
    case MoveSizeIntent::None:
      ++Diagnostics::g_runtime.counters.userNoopGestures;
      break;
  }

  const WindowRecord* directRecord = state.Find(hwnd);
  if (directRecord &&
      (state.Layout() == TileLayout::Floating ||
       directRecord->state == ManageState::Floating)) {
    if (RememberCurrentFloatingGeometry(key, state, hwnd)) {
      g_workspaces.Save(key, state);
      if (gesture.intent != MoveSizeIntent::None) {
        ++Diagnostics::g_runtime.counters.floatingGeometryUserUpdates;
      }
    }
    return;
  }

  if (state.Layout() == TileLayout::Floating) {
    return;
  }

  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return;

  HWND tiledHwnd = state.IsTiled(hwnd)
      ? hwnd
      : ResolveToTiledWindow(hwnd, state.TiledWindows());
  if (!tiledHwnd) {
    Wh_Log(L"Unresolved window");
    return;
  }

  switch (gesture.intent) {
    case MoveSizeIntent::None:
      ArrangeWorkspace(key);
      break;

    case MoveSizeIntent::Move:
      HandleUserMove(key, state, tiledHwnd, gesture);
      break;

    case MoveSizeIntent::Resize:
      HandleUserResize(
          key, state, tiledHwnd, metrics.workArea, metrics.gap, gesture);
      break;
  }
}

}  // namespace Commands

