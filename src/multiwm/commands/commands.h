// Hotkey and tray commands, and the policy for completed user drags and resizes.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Commands {

enum class TileDropZone { Center, Left, Right, Top, Bottom };

struct TiledDropTarget {
  HWND window;
  TileDropZone zone;
  bool operator==(const TiledDropTarget&) const = default;
};

// The edge a drop onto `target` splits, as a merge's edge drop. No value for a
// center drop, which never merges.
static std::optional<Placement::BspEdgeDrop> MergeEdgeDrop(const TiledDropTarget& target) {
  using Placement::TileEdge;
  switch (target.zone) {
    case TileDropZone::Left: return Placement::BspEdgeDrop{target.window, TileEdge::Left};
    case TileDropZone::Right: return Placement::BspEdgeDrop{target.window, TileEdge::Right};
    case TileDropZone::Top: return Placement::BspEdgeDrop{target.window, TileEdge::Top};
    case TileDropZone::Bottom: return Placement::BspEdgeDrop{target.window, TileEdge::Bottom};
    case TileDropZone::Center: break;
  }
  return std::nullopt;
}

// The foreground window with a copy of the workspace that owns its desktop and
// monitor. No value without a foreground window or a saved workspace there.
struct ForegroundWorkspace {
  HWND foreground = nullptr;
  HMONITOR monitor = nullptr;
  DesktopMonitorKey key{};
  Workspace workspace;
};

static std::optional<ForegroundWorkspace> LoadForegroundWorkspace(HWND foreground = GetForegroundWindow()) {
  ForegroundWorkspace current;
  if (!foreground) return std::nullopt;
  const HWND hung = Platform::Windows::HungWindowOfGhost(foreground);
  current.foreground = hung ? hung : foreground;
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

// A window command that does nothing says why, rather than failing silently
// (Show Toasts permitting). On the focused window's monitor when known.
static void ReportCommandRefused(const wchar_t* text, const std::optional<ForegroundWorkspace>& current) {
  TrayUi::ShowCommandFailureFlyout(text, current ? current->monitor : nullptr);
}

// The command found no window to act on: MultiWM doesn't manage the focused
// window, or knows it but doesn't tile it (floating, minimized or maximized).
static void ReportNoTargetWindow(const std::optional<ForegroundWorkspace>& current) {
  const bool known = current && current->workspace.HasRecord(current->foreground);
  ReportCommandRefused(known ? L"This window isn't tiled" : L"This window isn't managed", current);
}

static constexpr const wchar_t* kNotInFloatingLayout = L"Not available in the Floating layout";

// Promote the focused window, or rotate its immediate divider in BSP.
void ApplyLayoutAction(HWND foreground = GetForegroundWindow()) {
  ++Diagnostics::g_runtime.counters.layoutActionCommands;
  auto current = LoadForegroundWorkspace(foreground);
  if (!current) return ReportNoTargetWindow(current);
  const Workspace& workspace = current->workspace;
  const LayoutAction layoutAction = LayoutPolicyOf(workspace.Layout()).layoutAction;
  if (layoutAction == LayoutAction::None) return ReportCommandRefused(kNotInFloatingLayout, current);

  const HWND focused = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
  if (!focused) return ReportNoTargetWindow(current);
  Workspace candidate = workspace;
  using Kind = Placement::UserLayoutMutationKind;
  const bool rotate = layoutAction == LayoutAction::RotateSplit;
  const auto kind = rotate ? Kind::BspRotate : Kind::Order;
  const wchar_t* action = rotate ? L"Rotate split" : L"Set master";
  const BspTree& tree = workspace.Bsp();
  size_t divider = BspTree::kNotFound;
  HWND oldMaster = nullptr;
  if (rotate) {
    Platform::Monitors::WorkspaceMetrics metrics;
    if (!Platform::Monitors::GetWorkspaceMetrics(current->monitor, &metrics)) return;
    const size_t leaf = tree.FindLeaf(focused);
    if (leaf != BspTree::kNotFound) divider = tree.Parent(leaf);
    const auto shown = Model::PlanBspCells(workspace, metrics.workArea, metrics.gap);
    if (std::ranges::none_of(shown.splits, [divider](const Layout::BspSplit& split) { return split.node == divider; })) {
      return ReportCommandRefused(L"No shared split to rotate", current);
    }
    Layout::TurnBspSplit(candidate, divider, metrics.workArea);
  } else {
    if (focused == workspace.TiledWindows().front()) return ReportCommandRefused(L"Already the master window", current);
    if (!candidate.MakeMaster(focused, &oldMaster)) return;
  }

  if (!Placement::CommitUserLayoutMutation({current->key, current->monitor, workspace, focused, kind, action},
                                           std::move(candidate))) {
    return;
  }
  if (rotate) {
    Diagnostics::RecordEvent(L"BSP split rotated hwnd=%p", focused);
  } else {
    Diagnostics::RecordEvent(L"Swap Master old=%p new=%p", oldMaster, focused);
  }
}

// Moves the focused tiled window one slot toward or away from the front of the
// workspace's logical order. The operation is deliberately non-wrapping so the
// first and last slots remain stable boundaries.
static void MoveFocusedWindowOneSlot(bool promote, HWND foreground) {
  if (promote) {
    ++Diagnostics::g_runtime.counters.promoteWindowCommands;
  } else {
    ++Diagnostics::g_runtime.counters.demoteWindowCommands;
  }

  auto current = LoadForegroundWorkspace(foreground);
  if (!current) return ReportNoTargetWindow(current);
  if (current->workspace.Layout() == TileLayout::Floating) return ReportCommandRefused(kNotInFloatingLayout, current);
  const DesktopMonitorKey& key = current->key;
  const Workspace& workspace = current->workspace;

  HWND resolved = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
  if (!resolved) return ReportNoTargetWindow(current);

  const size_t index = workspace.TiledIndex(resolved);
  const size_t count = workspace.ActiveCount();
  if (index >= count) return;

  if (promote && index == 0) return ReportCommandRefused(L"Already first", current);
  if (!promote && index + 1 >= count) return ReportCommandRefused(L"Already last", current);

  const size_t otherIndex = promote ? index - 1 : index + 1;
  HWND other = workspace.TiledWindows()[otherIndex];
  Workspace candidate = workspace;
  if (!candidate.SwapTiled(resolved, other)) return;
  if (!Placement::CommitUserLayoutMutation(
          {key, current->monitor, workspace, resolved, Placement::UserLayoutMutationKind::Order,
           promote ? L"Promote" : L"Demote"},
          std::move(candidate))) {
    return;
  }

  Diagnostics::RecordEvent(
      promote ? L"Promote Window hwnd=%p from=%zu to=%zu"
              : L"Demote Window hwnd=%p from=%zu to=%zu",
      resolved, index, otherIndex);
}

void PromoteFocusedWindow(HWND foreground = GetForegroundWindow()) {
  MoveFocusedWindowOneSlot(true, foreground);
}

void DemoteFocusedWindow(HWND foreground = GetForegroundWindow()) {
  MoveFocusedWindowOneSlot(false, foreground);
}

void FloatFocusedWindow() {
  ++Diagnostics::g_runtime.counters.floatFocusedCommands;
  auto current = LoadForegroundWorkspace();
  if (!current) return ReportNoTargetWindow(current);
  const DesktopMonitorKey& key = current->key;
  Workspace& workspace = current->workspace;

  // Explicit floating also cancels a queued overflow of this focused HWND.
  g_wm.CancelPendingOverflow(current->foreground);
  HWND tiled = Platform::Windows::ResolveToTiledWindow(current->foreground, workspace.TiledWindows());
  if (!tiled) {
    const WindowRecord* record = workspace.Find(current->foreground);
    if (record && record->state == ManageState::Floating) return ReportCommandRefused(L"Already floating", current);
    return ReportNoTargetWindow(current);
  }

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

static std::optional<FocusedMember> LoadFocusedMember(std::optional<ForegroundWorkspace>& current) {
  if (!current) return std::nullopt;
  const HWND target = ResolveSizeCommandTarget(current->workspace, current->foreground);
  if (!target) return std::nullopt;

  FocusedMember focused;
  focused.key = std::move(current->key);
  focused.monitor = current->monitor;
  focused.workspace = std::move(current->workspace);
  focused.hwnd = target;
  const WindowRecord* record = focused.workspace.Find(focused.hwnd);
  focused.floating = focused.workspace.Layout() == TileLayout::Floating ||
                     (record && record->state == ManageState::Floating);
  if (!focused.floating) Placement::RefreshWorkspaceGhostSizes(focused.workspace);
  return focused;
}

static void ResizeUnmanagedFocusedWindow(
    HWND foreground, int direction, const wchar_t* action,
    const std::optional<ForegroundWorkspace>& current) {
  if (!foreground || Platform::Windows::IsWindowTrackedInAnyState(foreground)) {
    return ReportNoTargetWindow(current);
  }
  const HMONITOR monitor = Platform::Windows::GetCurrentManagedWindowMonitor(foreground);
  if (!monitor) return ReportNoTargetWindow(current);

  if (Placement::ResizeUnmanagedWindow(foreground, monitor, direction)) {
    Diagnostics::RecordEvent(L"%ls unmanaged hwnd=%p", action, foreground);
  } else {
    TrayUi::ShowCommandFailureFlyout(L"Couldn't resize this window", monitor);
  }
}

// Commits a tiled window-size change like a resize drag: validated against
// cached minimums, kept for rollback, then arranged with the window preferred.
static void CommitWindowSizeChange(FocusedMember& focused, Workspace candidate, const wchar_t* action) {
  if (!Placement::CommitUserLayoutMutation(
          {focused.key, focused.monitor, focused.workspace, focused.hwnd, Placement::UserLayoutMutationKind::Resize,
           action},
          std::move(candidate))) {
    return;
  }
  Diagnostics::RecordEvent(L"%ls hwnd=%p", action, focused.hwnd);
}

// Grows or shrinks the focused window by one step (hotkeys.WindowSize).
void StepFocusedWindowSize(bool grow) {
  if (grow) {
    ++Diagnostics::g_runtime.counters.growWindowCommands;
  } else {
    ++Diagnostics::g_runtime.counters.shrinkWindowCommands;
  }
  const wchar_t* action = grow ? L"Grow window" : L"Shrink window";
  const HWND foreground = GetForegroundWindow();
  auto current = LoadForegroundWorkspace(foreground);
  auto focused = LoadFocusedMember(current);
  if (!focused) return ResizeUnmanagedFocusedWindow(foreground, grow ? 1 : -1, action, current);

  // A press that can't move the window says why, rather than doing nothing
  // silently. Rejections by the layout check report their own message.
  auto reportNoRoom = [&] {
    TrayUi::ShowCommandFailureFlyout(grow ? L"No room to grow" : L"Already at its smallest size",
                                     focused->monitor);
  };

  if (focused->floating) {
    const WindowRecord* record = focused->workspace.Find(focused->hwnd);
    if ((record && !record->canResize) || Platform::Windows::GhostWindowOf(focused->hwnd)) {
      TrayUi::ShowCommandFailureFlyout(L"This window can't be resized", focused->monitor);
    } else if (Placement::StepFloatingWindowSize(focused->key, focused->workspace, focused->hwnd, grow ? 1 : -1)) {
      g_workspaces.Save(focused->key, focused->workspace);
      Diagnostics::RecordEvent(L"%ls floating hwnd=%p", action, focused->hwnd);
    } else {
      reportNoRoom();
    }
    return;
  }
  if (focused->workspace.Layout() == TileLayout::Monocle) {
    TrayUi::ShowCommandFailureFlyout(L"Nothing to resize in Monocle", focused->monitor);
    return;
  }

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
  const HWND foreground = GetForegroundWindow();
  auto current = LoadForegroundWorkspace(foreground);
  auto focused = LoadFocusedMember(current);
  if (!focused) return ResizeUnmanagedFocusedWindow(foreground, 0, L"Reset window size", current);

  if (focused->floating) {
    Placement::FloatingPlacementHint hint;
    hint.intent = Placement::FloatingPlacementIntent::NewWindowCenter;
    if (Placement::RepairFloatingGeometry(focused->key, focused->workspace, focused->hwnd, hint)) {
      g_workspaces.Save(focused->key, focused->workspace);
      Diagnostics::RecordEvent(L"Reset window size floating hwnd=%p", focused->hwnd);
    }
    return;
  }

  if (focused->workspace.Layout() == TileLayout::Monocle) {
    TrayUi::ShowCommandFailureFlyout(L"Nothing to resize in Monocle", focused->monitor);
    return;
  }
  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(focused->monitor, &metrics)) return;
  Workspace candidate = focused->workspace;
  if (!candidate.ResetTiledSize(
          metrics.workArea, metrics.gap, candidate.TiledIndex(focused->hwnd),
          g_settings.masterPercent / 100.0)) {
    // Nothing to reset: already at the layout's default, or the only window.
    TrayUi::ShowCommandFailureFlyout(L"Already at its default size", focused->monitor);
    return;
  }
  CommitWindowSizeChange(*focused, std::move(candidate), L"Reset window size");
}

// Collects local space-related floats so a user command can report them together.
class CommandReleaseScope {
 public:
  CommandReleaseScope() { g_wm.commandReleases = &released_; }
  ~CommandReleaseScope() { g_wm.commandReleases = nullptr; }
  CommandReleaseScope(const CommandReleaseScope&) = delete;
  CommandReleaseScope& operator=(const CommandReleaseScope&) = delete;
  const std::vector<SpaceFloat>& Released() const { return released_; }

 private:
  std::vector<SpaceFloat> released_;
};

// Tile Workspace (hotkey or tray): reconcile membership and explicitly adopt the
// user's current geometry. Automatic events never call this with
// adoptCurrentGeometry=true. Like every user command, the WM thread reports any
// window it tried to tile but had to float.
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

// Commits the layout change `candidate` as a user layout mutation, then presents
// it. Rejection keeps the user's layout; validation may still refresh native reports.
static bool CommitWorkspaceLayoutChange(
    const DesktopMonitorKey& key, HMONITOR monitor,
    const Workspace& workspace, Workspace candidate) {
  const TileLayout layout = candidate.Layout();
  if (!Placement::CommitUserLayoutMutation(
          {key, monitor, workspace, nullptr, Placement::UserLayoutMutationKind::LayoutChange, L"Change layout"},
          std::move(candidate))) {
    return false;
  }
  TrayUi::UpdateIcon(layout, monitor);
  TrayUi::ShowLayoutFlyout(layout, monitor);
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
  const TileLayout layout = candidate.Layout();
  if (!CommitWorkspaceLayoutChange(key, monitor, state, std::move(candidate))) return;
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(layout));
  Diagnostics::RecordEvent(
      L"layout cycle desktop=%08X monitorId=%ls from=%d to=%d", key.desktopId.Data1,
      key.monitor.deviceId.c_str(), static_cast<int>(previousLayout),
      static_cast<int>(layout));
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

static TileDropZone PickTileDropZone(const RECT& tile, POINT cursor, LONG edgePercent) {
  if (edgePercent <= 0 || IsRectEmpty(&tile)) return TileDropZone::Center;
  const double x = static_cast<double>(cursor.x - tile.left) / Layout::RectWidth(tile);
  const double y = static_cast<double>(cursor.y - tile.top) / Layout::RectHeight(tile);
  const std::array distances{x, 1.0 - x, y, 1.0 - y};
  const auto nearest = std::min_element(distances.begin(), distances.end());
  if (edgePercent < 50 && *nearest >= edgePercent / 100.0) return TileDropZone::Center;
  constexpr std::array edges{TileDropZone::Left, TileDropZone::Right, TileDropZone::Top, TileDropZone::Bottom};
  return edges[nearest - distances.begin()];
}

struct TiledDropFrame {
  HWND window;
  RECT frame;
};

static std::vector<TiledDropFrame> TiledDropFrames(const Workspace& workspace, HWND source) {
  std::vector<TiledDropFrame> frames;
  for (HWND window : workspace.TiledWindows()) {
    if (window == source) continue;
    RECT frame{};
    if (Platform::Windows::GetDisplayedWindowFrameRect(window, &frame)) frames.push_back({window, frame});
  }
  return frames;
}

static std::optional<TiledDropTarget> FindTiledDropTarget(
    const std::vector<TiledDropFrame>& frames, TileLayout layout, POINT cursor) {
  const LONG edgePercent = LayoutPolicyOf(layout).edgeDropZones ? g_settings.bspSplitEdgePercent : 0;
  for (const auto& [window, frame] : frames) {
    if (PtInRect(&frame, cursor)) return TiledDropTarget{window, PickTileDropZone(frame, cursor, edgePercent)};
  }
  return std::nullopt;
}

// Shared by the preview and the final drop. Only the supplied trial is changed.
static std::optional<Placement::UserLayoutMutationKind> PrepareTiledDrop(
    Workspace& candidate, HWND source, const TiledDropTarget& target, const RECT& workArea) {
  using Kind = Placement::UserLayoutMutationKind;
  const bool horizontal = target.zone == TileDropZone::Left || target.zone == TileDropZone::Right;
  const Layout::Axis requestedAxis = horizontal ? Layout::Axis::X : Layout::Axis::Y;
  const bool leading = target.zone == TileDropZone::Left || target.zone == TileDropZone::Top;
  if (g_settings.bspSplitEdgePercent > 0) {
    const size_t divider = candidate.BspSiblingDivider(source, target.window);
    if (divider != BspTree::kNotFound) {
      const BspTree& tree = candidate.Bsp();
      const Layout::Axis axis =
          tree.Nodes()[divider].direction.value_or(Layout::DefaultBspSplitAxis(tree, divider, workArea));
      if (target.zone == TileDropZone::Center || axis == requestedAxis) {
        if (candidate.SwapBspSiblingTiles(source, target.window)) return Kind::Order;
        return std::nullopt;
      }
      const bool sourceFirst = tree.Nodes()[divider + 1].window == source;
      if (sourceFirst != leading && !candidate.SwapBspSiblingTiles(source, target.window)) return std::nullopt;
      Layout::TurnBspSplit(candidate, divider, workArea);
      return Kind::BspRotate;
    }
  }
  if (target.zone == TileDropZone::Center) {
    if (candidate.SwapTiled(source, target.window)) return Kind::Order;
    return std::nullopt;
  }
  if (!candidate.MoveBspLeaf(source, target.window)) return std::nullopt;

  if (leading) candidate.SwapTiled(source, target.window);
  const BspTree& tree = candidate.Bsp();
  candidate.SetBspSplitDirection(tree.Parent(tree.FindLeaf(source)), requestedAxis);
  return Kind::BspMerge;
}

struct TiledDropPreview {
  RECT dragged;
  RECT affected;
  HWND draggedWindow = nullptr;
  HWND affectedWindow = nullptr;
};

// Uses cached constraints while the pointer is moving. A drop refreshes native limits.
static std::optional<TiledDropPreview> PlanTiledDropPreview(
    const Workspace& workspace, HWND source, const TiledDropTarget& target,
    const Platform::Monitors::WorkspaceMetrics& metrics) {
  Workspace candidate = workspace;
  const auto kind = PrepareTiledDrop(candidate, source, target, metrics.workArea);
  if (!kind) return std::nullopt;
  const bool merge = *kind == Placement::UserLayoutMutationKind::BspMerge;
  const auto rects = Placement::PlanUserLayoutMutation(
      {.previous = workspace, .subject = source, .kind = *kind,
       .edgeDrop = merge ? MergeEdgeDrop(target) : std::nullopt},
      candidate, metrics);
  if (!rects) return std::nullopt;
  const size_t sourceIndex = candidate.TiledIndex(source);
  const size_t targetIndex = candidate.TiledIndex(target.window);
  if (sourceIndex >= rects->size() || targetIndex >= rects->size() ||
      IsRectEmpty(&(*rects)[sourceIndex]) || IsRectEmpty(&(*rects)[targetIndex])) {
    return std::nullopt;
  }
  return TiledDropPreview{(*rects)[sourceIndex], (*rects)[targetIndex], source, target.window};
}

static void HandleUserMove(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const MoveSizeGesture& gesture, const RECT& workArea) {
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

  const auto target = workspace.IsTiled(hwnd) && gesture.hasDropPoint
      ? FindTiledDropTarget(TiledDropFrames(workspace, hwnd), workspace.Layout(), gesture.dropPoint) : std::nullopt;
  if (target) {
    Workspace candidate = workspace;
    const auto kind = PrepareTiledDrop(candidate, hwnd, *target, workArea);
    using Kind = Placement::UserLayoutMutationKind;
    const bool merge = kind == Kind::BspMerge;
    const wchar_t* action = merge ? L"Merge to target tile" : kind == Kind::BspRotate ? L"Rotate split" : L"Swap";
    if (kind && Placement::CommitUserLayoutMutation(
                    {key, key.ResolveMonitor(), workspace, hwnd, *kind, action,
                     merge ? MergeEdgeDrop(*target) : std::nullopt},
                    std::move(candidate))) {
      if (*kind == Kind::Order) ++Diagnostics::g_runtime.counters.tiledSwapActions;
      Wh_Log(L"User drag applied %ls to HWND %p and target %p", action, hwnd, target->window);
      Diagnostics::RecordEvent(L"user drag hwnd=%p target=%p action=%ls", hwnd, target->window, action);
      return;
    }
  }

  // An empty drop or a rejected change snaps the source back to its tile.
  Placement::ArrangeWorkspace(key);
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

  Placement::RefreshWorkspaceGhostSizes(workspace);
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

    case TileLayout::Bsp:
      changed = candidate.LearnBspResize(workArea, gap, resizedIndex, gesture);
      break;

    case TileLayout::Monocle:
    case TileLayout::Floating:
      break;
  }

  if (changed) {
    if (!Placement::CommitUserLayoutMutation(
            {key, key.ResolveMonitor(), workspace, hwnd, Placement::UserLayoutMutationKind::Resize, L"Resize"},
            std::move(candidate))) {
      Placement::ArrangeWorkspace(key, hwnd);
      return;
    }
    ++Diagnostics::g_runtime.counters.dividerUpdates;
    Wh_Log(L"User resize updated local divider state for %p", hwnd);
    Diagnostics::RecordEvent(L"user resize updated divider hwnd=%p", hwnd);
    return;
  }
  Placement::ArrangeWorkspace(key);
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
      HandleUserMove(key, state, tiledHwnd, gesture, metrics.workArea);
      break;

    case MoveSizeIntent::Resize:
      HandleUserResize(
          key, state, tiledHwnd, metrics.workArea, metrics.gap, gesture);
      break;
  }
}

}  // namespace Commands

