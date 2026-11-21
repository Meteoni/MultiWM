// Hotkey and tray commands, and the policy for completed user drags and resizes.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Commands {

// User-authored geometry changes are rejected before commit when cached native
// constraints already make them impossible. A short-lived transaction handles a
// minimum that an application changes only during the resulting placement.
static bool ValidateUserLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor,
    Workspace& candidate, HWND subject, Placement::UserLayoutMutationKind kind,
    const wchar_t* action) {
  // A same-DPI native minimum can change while no placement is occurring.
  // Refresh the committed workspace first, persist that application-owned state,
  // then mirror it into the geometry-mutated candidate before deciding whether
  // the user's operation is feasible.
  Workspace current;
  bool committedConstraintsChanged = false;
  if (g_workspaces.Load(key, &current)) {
    committedConstraintsChanged =
        Placement::RefreshWorkspaceNativeConstraints(current, true, L"user mutation");
    if (committedConstraintsChanged) {
      g_workspaces.Save(key, current);
    }
    for (HWND hwnd : candidate.TiledWindows()) {
      const WindowRecord* currentRecord = current.Find(hwnd);
      if (currentRecord) {
        candidate.UpdateConstraints(hwnd, currentRecord->constraints);
      }
    }
  }

  Platform::Monitors::WorkspaceMetrics metrics;
  std::vector<RECT> rects;
  Layout::LayoutPlanStatus status = Layout::LayoutPlanStatus::Unsatisfiable;
  if (Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) {
    status = Layout::BuildWorkspaceLayoutPlan(
        candidate, metrics.workArea, metrics.gap, &rects);
    if (status == Layout::LayoutPlanStatus::Unsatisfiable &&
        candidate.Layout() == TileLayout::Dwindle) {
      if (kind == Placement::UserLayoutMutationKind::LayoutChange) {
        Workspace reassigned = candidate;
        size_t movedWindowCount = 0;
        if (Layout::ReassignDwindleLayoutConversionForConstraints(
                reassigned, metrics.workArea, metrics.gap,
                &movedWindowCount)) {
          candidate = std::move(reassigned);
          Diagnostics::RecordEvent(
              L"dwindle layout conversion reassigned windows action=%ls moved=%zu",
              action ? action : L"Change layout", movedWindowCount);
          return true;
        }
      }
    }
  }
  if (status == Layout::LayoutPlanStatus::Success ||
      status == Layout::LayoutPlanStatus::NotApplicable) {
    return true;
  }

  Placement::ReportRejectedLayoutMutation(
      key, monitor, kind, action ? action : L"Layout mutation", subject,
      candidate, rects);
  // If the rejection exposed a changed native contract, reconcile the committed
  // layout only when that existing layout is itself no longer feasible. Avoid an
  // unnecessary reflow merely because the rejected candidate needed fresh data.
  if (committedConstraintsChanged) {
    Platform::Monitors::WorkspaceMetrics committedMetrics;
    std::vector<RECT> committedRects;
    if (Platform::Monitors::GetWorkspaceMetrics(monitor, &committedMetrics)) {
      if (Layout::BuildWorkspaceLayoutPlan(
              current, committedMetrics.workArea, committedMetrics.gap,
              &committedRects) == Layout::LayoutPlanStatus::Unsatisfiable) {
        Placement::ArrangeWorkspace(key);
      }
    }
  }
  return false;
}

// Validates `candidate`, a user change to `previous`, and on success keeps
// `previous` so the change can be rolled back if the windows refuse their new
// tiles. Validation may update `candidate` (refreshed native minimums, or a
// Dwindle reassignment for a layout change). Saving the candidate stays with
// the caller. False when the change is rejected; the user was told why.
static bool AcceptUserLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor, const Workspace& previous,
    Workspace& candidate, HWND subject, Placement::UserLayoutMutationKind kind,
    const wchar_t* action) {
  if (!ValidateUserLayoutMutation(key, monitor, candidate, subject, kind, action)) return false;
  Placement::RememberPendingLayoutMutation(key, previous, candidate, subject, kind, action);
  return true;
}

// The foreground window with a copy of the workspace that owns its desktop and
// monitor. No value without a foreground window or a saved workspace there.
struct ForegroundWorkspace {
  HWND foreground = nullptr;
  HMONITOR monitor = nullptr;
  DesktopMonitorKey key{};
  Workspace workspace;
};

static std::optional<ForegroundWorkspace> LoadForegroundWorkspace() {
  ForegroundWorkspace current;
  current.foreground = GetForegroundWindow();
  if (!current.foreground) return std::nullopt;
  current.monitor = Platform::Windows::GetWindowPhysicalMonitor(current.foreground);
  GUID desktopId{};
  if (!current.monitor ||
      !Platform::VirtualDesktop::GetWindowDesktopIdSafe(current.foreground, &desktopId) ||
      !DesktopMonitorKey::FromHMonitor(desktopId, current.monitor, &current.key) ||
      !g_workspaces.Load(current.key, &current.workspace)) {
    return std::nullopt;
  }
  return current;
}

// Promotes the foreground window, or its tiled owner, to the master slot, then
// clears stale gesture samples for both windows and reflows the workspace.
void SwapMaster() {
  ++Diagnostics::g_runtime.counters.swapMasterCommands;
  auto current = LoadForegroundWorkspace();
  if (!current || current->workspace.ActiveCount() < 2 ||
      current->workspace.Layout() == TileLayout::Floating) {
    return;
  }
  const DesktopMonitorKey& key = current->key;
  const Workspace& workspace = current->workspace;

  HWND resolved = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
  Workspace candidate = workspace;
  HWND oldMaster = nullptr;
  if (!resolved || !candidate.MakeMaster(resolved, &oldMaster)) return;
  if (!AcceptUserLayoutMutation(key, current->monitor, workspace, candidate, resolved,
                                Placement::UserLayoutMutationKind::Order, L"Set master")) {
    return;
  }
  g_workspaces.Save(key, candidate);
  Gestures::ClearMoveSizeSamples(oldMaster);
  Gestures::ClearMoveSizeSamples(resolved);
  Placement::ArrangeWorkspace(key, resolved);
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

  auto current = LoadForegroundWorkspace();
  if (!current || current->workspace.ActiveCount() < 2 ||
      current->workspace.Layout() == TileLayout::Floating) {
    return;
  }
  const DesktopMonitorKey& key = current->key;
  const Workspace& workspace = current->workspace;

  HWND resolved = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
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
  if (!AcceptUserLayoutMutation(key, current->monitor, workspace, candidate, resolved,
                                Placement::UserLayoutMutationKind::Order,
                                promote ? L"Promote" : L"Demote")) {
    return;
  }
  g_workspaces.Save(key, candidate);
  Gestures::ClearMoveSizeSamples(resolved);
  Gestures::ClearMoveSizeSamples(other);
  Placement::ArrangeWorkspace(key, resolved);

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
  auto current = LoadForegroundWorkspace();
  if (!current) return;
  const DesktopMonitorKey& key = current->key;
  Workspace& workspace = current->workspace;

  // Explicit floating also cancels a queued overflow of this focused HWND.
  g_wm.CancelPendingOverflow(current->foreground);
  HWND tiled = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
  if (!tiled) return;

  Placement::FloatingPlacementHint hint;
  hint.intent = Placement::FloatingPlacementIntent::CenterWithFloatingSize;

  Placement::CancelConformanceLease(tiled);
  Placement::DiscardPendingAdmissionRecovery(tiled);
  if (!workspace.Float(tiled)) return;
  Placement::RepairFloatingGeometry(key, workspace, tiled, hint);
  g_workspaces.Save(key, workspace);
  Placement::ArrangeWorkspace(key);
  Wh_Log(L"Focused window floated %p", tiled);
  Diagnostics::RecordEvent(L"Float Focused hwnd=%p", tiled);
}

// The window a size command acts on, or nullptr. A focused per-window floating
// window (floated by a rule, Alt+F or a drag) is the target itself, never its
// owner's tile. Otherwise the focused window resolves to the tiled window it
// belongs to: itself or an owner.
static HWND ResolveSizeCommandTarget(const Workspace& workspace, HWND focused) {
  const WindowRecord* record = workspace.Find(focused);
  if (record && record->state == ManageState::Floating) return focused;
  return Platform::Windows::ResolveToTiledWindow(focused, workspace.TiledWindows());
}

// The window a size command acts on, with a copy of its workspace. `floating`
// when it is sized as a floating window: a per-window floating record, or a
// member of the Floating layout.
struct FocusedMember {
  DesktopMonitorKey key{};
  HMONITOR monitor = nullptr;
  Workspace workspace;
  HWND hwnd = nullptr;
  bool floating = false;
};

static std::optional<FocusedMember> LoadFocusedMember() {
  auto current = LoadForegroundWorkspace();
  if (!current) return std::nullopt;
  FocusedMember focused;
  focused.key = std::move(current->key);
  focused.monitor = current->monitor;
  focused.workspace = std::move(current->workspace);
  focused.hwnd = ResolveSizeCommandTarget(focused.workspace, current->foreground);
  if (!focused.hwnd) return std::nullopt;
  const WindowRecord* record = focused.workspace.Find(focused.hwnd);
  focused.floating = focused.workspace.Layout() == TileLayout::Floating ||
                     (record && record->state == ManageState::Floating);
  return focused;
}

// Commits a tiled window-size change like a resize drag: validated against
// cached minimums, kept for rollback, then arranged with the window preferred.
static void CommitWindowSizeChange(FocusedMember& focused, Workspace candidate, const wchar_t* action) {
  if (!AcceptUserLayoutMutation(focused.key, focused.monitor, focused.workspace, candidate,
                                focused.hwnd, Placement::UserLayoutMutationKind::Resize, action)) {
    return;
  }
  g_workspaces.Save(focused.key, candidate);
  Placement::ArrangeWorkspace(focused.key, focused.hwnd);
  Diagnostics::RecordEvent(L"%ls hwnd=%p", action, focused.hwnd);
}

// Grows or shrinks the focused window by one step (hotkeys.WindowSize).
void StepFocusedWindowSize(bool grow) {
  if (grow) {
    ++Diagnostics::g_runtime.counters.growWindowCommands;
  } else {
    ++Diagnostics::g_runtime.counters.shrinkWindowCommands;
  }
  auto focused = LoadFocusedMember();
  if (!focused) return;
  const wchar_t* action = grow ? L"Grow window" : L"Shrink window";
  // A press that can't move the window says why, rather than doing nothing
  // silently. Rejections by the layout check report their own message.
  auto reportNoRoom = [&] {
    TrayUi::ShowCommandFailureFlyout(grow ? L"No room to grow" : L"Already at its smallest size",
                                     focused->monitor);
  };

  if (focused->floating) {
    const WindowRecord* record = focused->workspace.Find(focused->hwnd);
    if (record && !record->canResize) {
      TrayUi::ShowCommandFailureFlyout(L"This window can't be resized", focused->monitor);
    } else if (Placement::StepFloatingWindowSize(focused->key, focused->workspace, focused->hwnd, grow ? 1 : -1)) {
      g_workspaces.Save(focused->key, focused->workspace);
      Diagnostics::RecordEvent(L"%ls floating hwnd=%p", action, focused->hwnd);
    } else {
      reportNoRoom();
    }
    return;
  }
  if (focused->workspace.Layout() == TileLayout::Monocle) return;  // Nothing to size.

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(focused->monitor, &metrics)) return;
  Workspace candidate = focused->workspace;
  if (!candidate.StepTiledSize(
          metrics.workArea, metrics.gap, candidate.TiledIndex(focused->hwnd), grow,
          g_settings.resizeStepPercent)) {
    reportNoRoom();
    return;
  }
  CommitWindowSizeChange(*focused, std::move(candidate), action);
}

// Returns the focused window to its default size. A tiled window gets the
// layout default and keeps its position. A floating window is placed as if it
// were newly admitted (NewWindowCenter): centered at its override-size rule's
// size, else the default floating size; a preserve-size rule keeps its size.
void ResetFocusedWindowSize() {
  ++Diagnostics::g_runtime.counters.resetWindowSizeCommands;
  auto focused = LoadFocusedMember();
  if (!focused) return;

  if (focused->floating) {
    Placement::FloatingPlacementHint hint;
    hint.intent = Placement::FloatingPlacementIntent::NewWindowCenter;
    if (Placement::RepairFloatingGeometry(focused->key, focused->workspace, focused->hwnd, hint)) {
      g_workspaces.Save(focused->key, focused->workspace);
      Diagnostics::RecordEvent(L"Reset window size floating hwnd=%p", focused->hwnd);
    }
    return;
  }

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(focused->monitor, &metrics)) return;
  Workspace candidate = focused->workspace;
  if (!candidate.ResetTiledSize(
          metrics.workArea, metrics.gap, candidate.TiledIndex(focused->hwnd),
          g_settings.masterPercent / 100.0)) {
    return;
  }
  CommitWindowSizeChange(*focused, std::move(candidate), L"Reset window size");
}

// Manual tile: reconcile membership and explicitly adopt the user's current
// geometry. Automatic events never call this with adoptCurrentGeometry=true.
void TileWindows(HMONITOR monitor = nullptr) {
  ++Diagnostics::g_runtime.counters.tileCommands;
  if (!monitor) monitor = Platform::Monitors::GetWorkspaceCommandMonitor();
  if (!monitor) return;

  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::All, &changedKeys);

  DesktopMonitorKey targetKey{};
  if (!Platform::Monitors::GetCurrentWorkspaceKey(monitor, &targetKey)) return;
  if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, true, &changedKeys)) return;

  // Ensure other affected current workspaces also lose stale slots. Keep the
  // target workspace until after geometry adoption so manual tiling does not
  // overwrite the user's geometry before capturing it.
  for (const auto& key : changedKeys) {
    if (key != targetKey) Placement::ArrangeWorkspace(key);
  }

  TrayUi::RefreshForMonitor(monitor);
  Wh_Log(L"Manual tile/global reconcile completed");
  Diagnostics::RecordEvent(L"Tile Workspace command completed monitor=%p", monitor);
}

// Validate the candidate, retain rollback geometry, then commit and present it.
// Rejection keeps the user's layout; validation may still refresh native reports.
static bool CommitWorkspaceLayoutChange(
    const DesktopMonitorKey& key, HMONITOR monitor,
    Workspace& workspace, Workspace candidate) {
  const TileLayout previousLayout = workspace.Layout();
  if (!AcceptUserLayoutMutation(key, monitor, workspace, candidate, nullptr,
                                Placement::UserLayoutMutationKind::LayoutChange, L"Change layout")) {
    return false;
  }
  workspace = std::move(candidate);
  if (previousLayout != TileLayout::Floating &&
      workspace.Layout() == TileLayout::Floating) {
    Placement::RestoreWorkspaceFloatingGeometry(key, workspace);
  } else if (previousLayout == TileLayout::Floating &&
             workspace.Layout() != TileLayout::Floating) {
    // Floating leases end with the layout; tiling places every window afresh.
    for (HWND hwnd : workspace.TiledWindows()) Placement::CancelConformanceLease(hwnd);
  }
  g_workspaces.Save(key, workspace);
  Placement::ArrangeWorkspace(key);
  TrayUi::UpdateIcon(workspace.Layout(), monitor);
  TrayUi::ShowLayoutFlyout(workspace.Layout(), monitor);
  return true;
}

// Advances the current workspace layout and invalidates layout-specific hints in
// suspended slots before persisting, arranging, and updating UI feedback.
void CycleCurrentWorkspaceLayout(HMONITOR monitor = nullptr) {
  ++Diagnostics::g_runtime.counters.layoutCycleCommands;
  if (!monitor) monitor = Platform::Monitors::GetWorkspaceCommandMonitor();
  if (!monitor) return;
  DesktopMonitorKey key{};
  if (!Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) return;

  Workspace state;
  if (!g_workspaces.Load(key, &state)) {
    if (RuntimeLifecycle::IsAutomaticMode()) {
      if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, false) ||
          !g_workspaces.Load(key, &state)) {
        return;
      }
    } else {
      state = Reconcile::MakeDefaultWorkspace(key);
    }
  }

  const TileLayout previousLayout = state.Layout();
  Workspace candidate = state;
  if (!candidate.CycleLayout(g_settings.layoutCycle)) return;
  if (!CommitWorkspaceLayoutChange(key, monitor, state, std::move(candidate))) return;
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
  if (!monitor) monitor = Platform::Monitors::GetWorkspaceCommandMonitor();
  if (!monitor) return;

  DesktopMonitorKey key{};
  if (!Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) return;

  Workspace state;
  bool workspaceCreated = false;
  if (!g_workspaces.Load(key, &state)) {
    if (RuntimeLifecycle::IsAutomaticMode()) {
      if (!Reconcile::EnsureWorkspaceFromSnapshot(monitor, false) ||
          !g_workspaces.Load(key, &state)) {
        return;
      }
    } else {
      state = Reconcile::MakeDefaultWorkspace(key);
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
  if (!CommitWorkspaceLayoutChange(key, monitor, state, std::move(candidate))) return;
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
    Placement::CancelConformanceLease(hwnd);
    Placement::DiscardPendingAdmissionRecovery(hwnd);
    if (workspace.Float(hwnd)) {
      Placement::RepairFloatingGeometry(
          key, workspace, hwnd, Placement::MakeFloatingPlacementHintFromGesture(gesture));
      g_workspaces.Save(key, workspace);
      ++Diagnostics::g_runtime.counters.tiledFloatActions;
      Wh_Log(L"User move floated managed window %p", hwnd);
      Diagnostics::RecordEvent(L"user drag floated hwnd=%p", hwnd);
    }
    Placement::ArrangeWorkspace(key);
    return;
  }

  HWND target = nullptr;
  HWND preferredTiledWindow = nullptr;
  if (workspace.IsTiled(hwnd) && gesture.hasDropPoint) {
    for (HWND candidate : workspace.TiledWindows()) {
      if (candidate == hwnd) continue;
      RECT rect{};
      if (Platform::Windows::GetWindowFrameRect(candidate, &rect) && PtInRect(&rect, gesture.dropPoint)) {
        target = candidate;
        break;
      }
    }
  }

  if (target) {
    Workspace candidate = workspace;
    if (candidate.SwapTiled(hwnd, target) &&
        AcceptUserLayoutMutation(key, key.ResolveMonitor(), workspace, candidate, hwnd,
                                 Placement::UserLayoutMutationKind::Order, L"Swap")) {
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
  Placement::ArrangeWorkspace(key, preferredTiledWindow);
}

static void HandleUserResize(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const RECT& workArea, LONG gap, const MoveSizeGesture& gesture) {
  if (!gesture.hasRects) {
    Placement::ArrangeWorkspace(key);
    return;
  }

  const size_t resizedIndex = workspace.TiledIndex(hwnd);
  if (resizedIndex == Workspace::kNotFound) {
    Placement::ArrangeWorkspace(key);
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
    if (!AcceptUserLayoutMutation(key, key.ResolveMonitor(), workspace, candidate, hwnd,
                                  Placement::UserLayoutMutationKind::Resize, L"Resize")) {
      Placement::ArrangeWorkspace(key, hwnd);
      return;
    }
    workspace = std::move(candidate);
    g_workspaces.Save(key, workspace);
    ++Diagnostics::g_runtime.counters.dividerUpdates;
    Wh_Log(L"User resize updated local divider state for %p", hwnd);
    Diagnostics::RecordEvent(L"user resize updated divider hwnd=%p", hwnd);
  }
  Placement::ArrangeWorkspace(key, changed ? hwnd : nullptr);
}

// Consumes one completed user gesture and routes it to the move or resize policy.
// The top-level handler deliberately contains no layout-specific math.
void ApplyUserMoveSize(HWND hwnd) {
  if (!IsWindow(hwnd)) return;

  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    Gestures::ClearMoveSizeSamples(hwnd);
    return;
  }

  HMONITOR monitor = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
  if (!monitor) return;

  GUID desktopId{};
  if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) return;
  Workspace state;
  if (!g_workspaces.Load(key, &state)) return;

  // Classify every completed move/size gesture at the common boundary before
  // routing it to tiled or floating policy. This makes the counters describe
  // actual user gestures instead of only gestures that reached tiled handlers.
  MoveSizeGesture gesture = Gestures::ConsumeMoveSizeGesture(hwnd);
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
    if (Placement::RememberCurrentFloatingGeometry(key, state, hwnd)) {
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

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return;

  HWND tiledHwnd = state.IsTiled(hwnd)
      ? hwnd
      : Platform::Windows::ResolveToTiledWindow(hwnd, state.TiledWindows());
  if (!tiledHwnd) {
    Wh_Log(L"Unresolved window");
    return;
  }

  switch (gesture.intent) {
    case MoveSizeIntent::None:
      Placement::ArrangeWorkspace(key);
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

