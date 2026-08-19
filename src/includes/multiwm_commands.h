//=============================================================================
// User commands
//=============================================================================
namespace Commands {

// Promotes the foreground window, or its tiled owner, to the master slot, then
// clears stale gesture samples for both windows and reflows the workspace.
void SwapMaster() {
  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace) || workspace.ActiveCount() < 2 ||
      workspace.Layout() == TileLayout::Floating) {
    return;
  }

  HWND resolved = ResolveToTiledWindow(fg, workspace.TiledWindows());
  HWND oldMaster = nullptr;
  if (!resolved || !workspace.MakeMaster(resolved, &oldMaster)) return;

  g_workspaces.Save(key, workspace);
  ClearMoveSizeSamples(oldMaster);
  ClearMoveSizeSamples(resolved);
  ArrangeWorkspace(key);
}

void FloatFocusedWindow() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return;

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  GUID desktopId{};
  if (!monitor || !GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
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

  if (!workspace.Float(tiled)) return;
  workspace.ResetPlacementFailures(tiled);
  RepairFloatingGeometry(key, workspace, tiled, hint);
  g_workspaces.Save(key, workspace);
  ArrangeWorkspace(key);
  Wh_Log(L"Focused window floated %p", tiled);
}

// Manual tile: reconcile membership and explicitly adopt the user's current
// geometry. Automatic events never call this with adoptCurrentGeometry=true.
void TileWindows() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;

  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::All, &changedKeys);

  DesktopMonitorKey targetKey{};
  GetCurrentWorkspaceKey(monitor, &targetKey);
  if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, true, &changedKeys)) return;

  // Ensure other affected current workspaces also lose stale slots. Keep the
  // target workspace until after geometry adoption so Alt+D does not overwrite
  // the user's geometry before capturing it.
  for (const auto& key : changedKeys) {
    if (!Reconcile::SameWorkspaceKey(key, targetKey)) ArrangeWorkspace(key);
  }

  TrayUi::RefreshForCurrentWorkspace();
  Wh_Log(L"Manual tile/global reconcile completed");
}

// Advances the current workspace layout and invalidates layout-specific hints in
// suspended slots before persisting, arranging, and updating UI feedback.
void CycleCurrentWorkspaceLayout() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;
  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return;

  Workspace state;
  if (!g_workspaces.Load(key, &state)) {
    if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, false) || !g_workspaces.Load(key, &state)) return;
  }

  state.CycleLayout();
  g_workspaces.Save(key, state);
  ArrangeWorkspace(key);
  TrayUi::UpdateIcon(state.Layout());
  TrayUi::ShowLayoutFlyout(state.Layout());
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(state.Layout()));
}

static void HandleUserMove(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const MoveSizeGesture& gesture) {
  if (g_settings.mouseMoveBehavior == MouseMoveBehavior::Float) {
    if (workspace.Float(hwnd)) {
      workspace.ResetPlacementFailures(hwnd);
      RepairFloatingGeometry(
          key, workspace, hwnd, MakeFloatingPlacementHintFromGesture(gesture));
      g_workspaces.Save(key, workspace);
      Wh_Log(L"User move floated managed window %p", hwnd);
    }
    ArrangeWorkspace(key);
    return;
  }

  HWND target = nullptr;
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

  if (target && workspace.SwapTiled(hwnd, target)) {
    g_workspaces.Save(key, workspace);
    Wh_Log(L"User move swapped managed windows %p and %p", hwnd, target);
  }

  // No target means the source remains tiled and simply snaps back to its slot.
  ArrangeWorkspace(key);
}

static void HandleUserResize(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const RECT& workArea, const MoveSizeGesture& gesture) {
  if (!gesture.hasRects) {
    ArrangeWorkspace(key);
    return;
  }

  const size_t resizedIndex = workspace.TiledIndex(hwnd);
  if (resizedIndex == static_cast<size_t>(-1)) {
    ArrangeWorkspace(key);
    return;
  }

  bool changed = false;
  switch (workspace.Layout()) {
    case TileLayout::MasterStack:
    case TileLayout::MasterStackH:
      changed = workspace.LearnMasterStackResize(
          workArea, GetWorkspaceGapPixels(key.monitor), resizedIndex, gesture);
      break;

    case TileLayout::Columns:
    case TileLayout::Rows:
      changed = workspace.LearnGridResize(
          workArea, GetWorkspaceGapPixels(key.monitor), resizedIndex, gesture);
      break;

    case TileLayout::COUNT:
      return;

    case TileLayout::BSP:
    case TileLayout::Monocle:
    case TileLayout::Floating:
      break;
  }

  if (changed) {
    g_workspaces.Save(key, workspace);
    Wh_Log(L"User resize updated local divider state for %p", hwnd);
  }
  ArrangeWorkspace(key);
}

// Consumes one completed user gesture and routes it to the move or resize policy.
// The top-level handler deliberately contains no layout-specific math.
void ApplyUserMoveSize(HWND hwnd) {
  if (!IsWindow(hwnd)) return;

  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    ClearMoveSizeSamples(hwnd);
    return;
  }

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  Workspace state;
  if (!g_workspaces.Load(key, &state)) return;

  const WindowRecord* directRecord = state.Find(hwnd);
  if (directRecord &&
      (state.Layout() == TileLayout::Floating ||
       directRecord->state == ManageState::Floating)) {
    ConsumeMoveSizeGesture(hwnd);
    if (RememberCurrentFloatingGeometry(key, state, hwnd)) {
      g_workspaces.Save(key, state);
    }
    return;
  }

  if (state.Layout() == TileLayout::Floating) {
    ClearMoveSizeSamples(hwnd);
    return;
  }

  RECT workArea{};
  if (!GetWorkspaceWorkArea(monitor, &workArea)) return;

  HWND tiledHwnd = state.IsTiled(hwnd)
      ? hwnd
      : ResolveToTiledWindow(hwnd, state.TiledWindows());
  if (!tiledHwnd) {
    Wh_Log(L"Unresolved window");
    return;
  }

  MoveSizeGesture gesture = ConsumeMoveSizeGesture(hwnd);
  switch (gesture.intent) {
    case MoveSizeIntent::None:
      ArrangeWorkspace(key);
      break;

    case MoveSizeIntent::Move:
      HandleUserMove(key, state, tiledHwnd, gesture);
      break;

    case MoveSizeIntent::Resize:
      HandleUserResize(key, state, tiledHwnd, workArea, gesture);
      break;
  }
}

}  // namespace Commands

