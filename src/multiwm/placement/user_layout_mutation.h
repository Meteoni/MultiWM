// User layout mutations: changes the user asks for to a workspace's order,
// weights or layout. Each applies at once, but is rolled back if the windows
// refuse their new tiles (see the Vocabulary in CONTRIBUTING.md).
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// Commands build the candidate workspace and call CommitUserLayoutMutation; the
// drag preview calls PlanUserLayoutMutation. Arrangement and conformance call the
// rest, which keeps or rolls back the mutation pending on a workspace.

namespace Placement {

// The edge of a tile that a BSP drop splits.
enum class TileEdge { Left, Right, Top, Bottom };

// A drop near a tile's edge (BSP Split Edge Size): splits `target` at `edge`.
struct BspEdgeDrop {
  HWND target = nullptr;
  TileEdge edge = TileEdge::Left;
};

// One user layout mutation of the workspace at `key`. The drag preview's plan
// reads only `previous`, `subject`, `kind` and `edgeDrop`.
struct UserLayoutMutation {
  DesktopMonitorKey key;
  HMONITOR monitor = nullptr;
  Workspace previous;      // The committed state the user acted on; restored on rollback.
  HWND subject = nullptr;  // The window the user acted on, if any; arranged first.
  UserLayoutMutationKind kind = UserLayoutMutationKind::Resize;
  const wchar_t* action = L"Layout mutation";  // Names it in toasts, logs and diagnostics.
  std::optional<BspEdgeDrop> edgeDrop;         // BspMerge only.
};

// A tiled window whose cell `candidate` (planned as `candidateRects`) would move
// while the window is hung and shown by its ghost, or nullptr. MultiWM can't
// move a ghost: dwm.exe owns it and refuses positioning from other processes, so
// only the ghost itself can move, when the user drags it. A cell may stay where
// `current` plans it, or land exactly where the ghost is shown.
static HWND FindGhostAUserChangeWouldMove(
    const Workspace* current, const Workspace& candidate, const std::vector<RECT>& candidateRects,
    const Platform::Monitors::WorkspaceMetrics& metrics) {
  std::vector<RECT> currentRects;
  bool currentPlanned = false;
  for (size_t i = 0; i < candidate.TiledWindows().size() && i < candidateRects.size(); ++i) {
    const HWND hwnd = candidate.TiledWindows()[i];
    const WindowRecord* record = candidate.Find(hwnd);
    if (!record || record->ghostSize.cx <= 0 || record->ghostSize.cy <= 0) continue;
    const RECT& planned = candidateRects[i];
    if (current && !currentPlanned) {
      currentPlanned = true;
      if (Layout::BuildWorkspaceLayoutPlan(*current, metrics.workArea, metrics.gap, &currentRects) !=
          Layout::LayoutPlanStatus::Success) {
        currentRects.clear();
      }
    }
    const size_t was = current ? current->TiledIndex(hwnd) : Workspace::kNotFound;
    if (was < currentRects.size() && Layout::RectsNear(planned, currentRects[was])) continue;
    RECT shown{};
    if (Platform::Windows::GetDisplayedWindowFrameRect(hwnd, &shown) && Layout::RectsNear(planned, shown)) continue;
    return hwnd;
  }
  return nullptr;
}

// True, after telling the user, when `candidate` would move a hung window's
// ghost (FindGhostAUserChangeWouldMove).
static bool RefuseMovingAGhost(
    const Workspace* current, const Workspace& candidate, const std::vector<RECT>& rects,
    const Platform::Monitors::WorkspaceMetrics& metrics, HMONITOR monitor, const wchar_t* action) {
  const HWND ghosted = FindGhostAUserChangeWouldMove(current, candidate, rects, metrics);
  if (!ghosted) return false;
  std::wstring name = ProcessDisplayName(ghosted);
  if (name.empty()) name = L"A window";
  TrayUi::ShowAppFlyout(name + L" isn't responding and can't be moved", {ghosted}, monitor);
  Diagnostics::RecordEvent(L"layout mutation rejected action=%ls: hung window can't move hwnd=%p",
                           action ? action : L"Layout mutation", ghosted);
  return true;
}

// Plans the BSP edge drop of `source` onto `drop.target` into `rects`, keeping
// the requested edge of the target's tile in `previous` in `bspDropEdge`.
static Layout::LayoutPlanStatus PlanBspDrop(
    Workspace& candidate, const Workspace& previous, HWND source, const BspEdgeDrop& drop,
    const Platform::Monitors::WorkspaceMetrics& metrics, std::vector<RECT>& rects,
    std::optional<RECT>& bspDropEdge) {
  const auto cells = Model::PlanBspCells(previous, metrics.workArea, metrics.gap).rects;
  const size_t targetIndex = previous.TiledIndex(drop.target);
  if (targetIndex >= cells.size() || IsRectEmpty(&cells[targetIndex])) {
    return Layout::LayoutPlanStatus::Unsatisfiable;
  }

  RECT edge = cells[targetIndex];
  const bool horizontal = drop.edge == TileEdge::Left || drop.edge == TileEdge::Right;
  const LONG span = horizontal ? Layout::RectWidth(edge) : Layout::RectHeight(edge);
  const LONG edgeSize = static_cast<LONG>((static_cast<int64_t>(span) * g_settings.bspSplitEdgePercent + 99) / 100);
  switch (drop.edge) {
    case TileEdge::Left: edge.right = edge.left + edgeSize; break;
    case TileEdge::Right: edge.left = edge.right - edgeSize; break;
    case TileEdge::Top: edge.bottom = edge.top + edgeSize; break;
    case TileEdge::Bottom: edge.top = edge.bottom - edgeSize; break;
  }
  bspDropEdge = edge;
  return Layout::PlanOrTurnNewBspSplit(candidate, metrics.workArea, metrics.gap, &rects,
                                       Layout::BspMergeDestination{source, *bspDropEdge});
}

// The plan of a user layout mutation's candidate.
struct UserLayoutMutationPlan {
  Layout::LayoutPlanStatus status = Layout::LayoutPlanStatus::Unsatisfiable;
  std::vector<RECT> rects;
  // A Dwindle reassignment (see PlanCandidate) that can't then be planned.
  bool acceptAsIs = false;
  // The requested edge of a BSP edge drop, kept while the drop settles.
  std::optional<RECT> bspDropEdge;
};

// Plans `candidate`, the result of `mutation`. A layout change to Dwindle that
// doesn't fit as is may reassign windows to slots that fit, which replaces
// `candidate`. That reassignment was always accepted unplanned, so `acceptAsIs`
// is set when it can't then be planned for the checks that follow.
static UserLayoutMutationPlan PlanCandidate(
    const UserLayoutMutation& mutation, Workspace& candidate, const Platform::Monitors::WorkspaceMetrics& metrics) {
  UserLayoutMutationPlan plan;
  if (mutation.edgeDrop) {
    plan.status = PlanBspDrop(candidate, mutation.previous, mutation.subject, *mutation.edgeDrop, metrics, plan.rects,
                              plan.bspDropEdge);
    return plan;
  }
  plan.status = Layout::BuildWorkspaceLayoutPlan(candidate, metrics.workArea, metrics.gap, &plan.rects);
  if (plan.status != Layout::LayoutPlanStatus::Unsatisfiable || candidate.Layout() != TileLayout::Dwindle ||
      mutation.kind != UserLayoutMutationKind::LayoutChange) {
    return plan;
  }
  Workspace reassigned = candidate;
  size_t movedWindowCount = 0;
  if (!Layout::ReassignDwindleLayoutConversionForConstraints(reassigned, metrics.workArea, metrics.gap,
                                                              &movedWindowCount)) {
    return plan;
  }
  candidate = std::move(reassigned);
  Diagnostics::RecordEvent(L"dwindle layout conversion reassigned windows action=%ls moved=%zu",
                           mutation.action ? mutation.action : L"Change layout", movedWindowCount);
  plan.status = Layout::BuildWorkspaceLayoutPlan(candidate, metrics.workArea, metrics.gap, &plan.rects);
  plan.acceptAsIs = plan.status != Layout::LayoutPlanStatus::Success;
  return plan;
}

// The drag preview's plan of `candidate`, the result of `mutation`, using cached
// native minimums (the drop itself refreshes them). No value when it can't be
// planned or would move a hung window's ghost.
static std::optional<std::vector<RECT>> PlanUserLayoutMutation(
    const UserLayoutMutation& mutation, Workspace& candidate, const Platform::Monitors::WorkspaceMetrics& metrics) {
  UserLayoutMutationPlan plan = PlanCandidate(mutation, candidate, metrics);
  if (plan.status != Layout::LayoutPlanStatus::Success ||
      FindGhostAUserChangeWouldMove(&mutation.previous, candidate, plan.rects, metrics)) {
    return std::nullopt;
  }
  return std::move(plan.rects);
}

static void RememberPendingLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& previous,
    const Workspace& candidate, HWND subject, UserLayoutMutationKind kind,
    const wchar_t* action, std::optional<RECT> bspDropEdge = std::nullopt) {
  PendingLayoutMutation mutation;
  mutation.previous = previous;
  mutation.candidate = candidate;
  mutation.subject = subject;
  mutation.kind = kind;
  mutation.bspDropEdge = bspDropEdge;
  mutation.action = action ? action : L"Layout mutation";
  g_pendingProofs.RememberLayoutMutation(key, std::move(mutation));
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
    case UserLayoutMutationKind::BspMerge:
    case UserLayoutMutationKind::BspRotate:
      ++Diagnostics::g_runtime.counters.rejectedOrderCommands;
      break;
    case UserLayoutMutationKind::LayoutChange:
      ++Diagnostics::g_runtime.counters.rejectedLayoutChanges;
      break;
  }

  HWND culprit = Layout::FindConstraintCulprit(
      rejectedWorkspace, rejectedRects, subject);
  if (!culprit) culprit = culpritHint;
  if (kind == UserLayoutMutationKind::BspMerge) {
    TrayUi::ShowCommandFailureFlyout(L"Not enough space to split that tile", monitor);
  } else {
    ShowWindowConstraintConflict(rejectedWorkspace, rejectedRects, culprit, monitor);
  }
  Wh_Log(
      L"%ls rejected by layout constraints subject=%p culprit=%p",
      action, subject, culprit);
  Diagnostics::RecordEvent(
      L"layout mutation rejected action=%ls subject=%p culprit=%p desktop=%08X",
      action, subject, culprit, key.desktopId.Data1);
}

// User-authored geometry changes are rejected before commit when cached native
// constraints already make them impossible. A short-lived transaction handles a
// minimum that an application changes only during the resulting placement.
// May update `candidate` (refreshed native minimums, or a Dwindle reassignment).
static bool ValidateUserLayoutMutation(
    const UserLayoutMutation& mutation, Workspace& candidate, std::optional<RECT>& bspDropEdge) {
  const DesktopMonitorKey& key = mutation.key;
  const HMONITOR monitor = mutation.monitor;
  // A same-DPI native minimum can change while no placement is occurring.
  // Refresh the committed workspace first, persist that application-owned state,
  // then mirror it into the geometry-mutated candidate before deciding whether
  // the user's operation is feasible.
  Workspace current;
  bool committedConstraintsChanged = false;
  const bool haveCurrent = g_workspaces.Load(key, &current);
  if (haveCurrent) {
    committedConstraintsChanged =
        RefreshWorkspaceNativeConstraints(current, true, L"user mutation");
    if (committedConstraintsChanged) {
      g_workspaces.Save(key, current);
    }
    for (HWND hwnd : candidate.TiledWindows()) {
      const WindowRecord* currentRecord = current.Find(hwnd);
      if (currentRecord) {
        candidate.UpdateConstraints(hwnd, currentRecord->constraints);
        candidate.UpdateGhostSize(hwnd, currentRecord->ghostSize);
      }
    }
  }

  Platform::Monitors::WorkspaceMetrics metrics;
  UserLayoutMutationPlan plan;
  if (Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) {
    plan = PlanCandidate(mutation, candidate, metrics);
  }
  bspDropEdge = plan.bspDropEdge;
  if (plan.acceptAsIs || plan.status == Layout::LayoutPlanStatus::NotApplicable) return true;
  if (plan.status == Layout::LayoutPlanStatus::Success) {
    return !RefuseMovingAGhost(haveCurrent ? &current : nullptr, candidate, plan.rects, metrics, monitor,
                               mutation.action);
  }

  ReportRejectedLayoutMutation(
      key, monitor, mutation.kind, mutation.action ? mutation.action : L"Layout mutation", mutation.subject,
      candidate, plan.rects);
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
        ArrangeWorkspace(key);
      }
    }
  }
  return false;
}

// The windows whose tiles `candidate` moves from where `previous` has them:
// tiled in only one of the two, or planned elsewhere. Every tiled window when
// either can't be planned (the Floating layout has no tiles).
static std::vector<HWND> WindowsWhoseTilesChange(
    const Workspace& previous, const Workspace& candidate, HMONITOR monitor) {
  Platform::Monitors::WorkspaceMetrics metrics;
  std::vector<RECT> before;
  std::vector<RECT> after;
  const bool planned =
      Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics) &&
      Layout::BuildWorkspaceLayoutPlan(previous, metrics.workArea, metrics.gap, &before) ==
          Layout::LayoutPlanStatus::Success &&
      Layout::BuildWorkspaceLayoutPlan(candidate, metrics.workArea, metrics.gap, &after) ==
          Layout::LayoutPlanStatus::Success;
  std::vector<HWND> moved;
  for (size_t i = 0; i < candidate.TiledWindows().size(); ++i) {
    const HWND hwnd = candidate.TiledWindows()[i];
    const size_t was = previous.TiledIndex(hwnd);
    if (!planned || was >= before.size() || i >= after.size() || !Layout::RectsNear(before[was], after[i])) {
      moved.push_back(hwnd);
    }
  }
  for (HWND hwnd : previous.TiledWindows()) {
    if (!candidate.IsTiled(hwnd)) moved.push_back(hwnd);
  }
  return moved;
}

// Commits `candidate`, the result of `mutation`. First refreshes the tiled
// windows' native minimums, saving the committed workspace if they changed, and
// plans the candidate with them. A plan that fails, or that would move a hung
// window's ghost, is rejected: the user is told why, nothing else is saved and
// nothing is arranged. Otherwise keeps `mutation.previous` so the change can be
// rolled back if the windows refuse their new tiles, applies entering or leaving
// the Floating layout, saves the candidate, ends any drag or resize in progress
// on a window whose tile it moves (that gesture started from the old tile, so
// the window goes to its new one and the gesture's drop is ignored), and arranges
// with `mutation.subject` preferred. Validation may replace `candidate`
// (refreshed minimums, or a Dwindle reassignment for a layout change). True when
// committed. WM thread only.
static bool CommitUserLayoutMutation(const UserLayoutMutation& mutation, Workspace candidate) {
  const DesktopMonitorKey& key = mutation.key;
  std::optional<RECT> bspDropEdge;
  if (!ValidateUserLayoutMutation(mutation, candidate, bspDropEdge)) return false;
  RememberPendingLayoutMutation(key, mutation.previous, candidate, mutation.subject, mutation.kind, mutation.action,
                                bspDropEdge);
  const std::vector<HWND> movedTiles = WindowsWhoseTilesChange(mutation.previous, candidate, mutation.monitor);

  const bool wasFloating = mutation.previous.Layout() == TileLayout::Floating;
  const bool isFloating = candidate.Layout() == TileLayout::Floating;
  if (wasFloating && !isFloating) {
    RememberFloatingLayoutFrames(candidate, Platform::Monitors::GetMonitorEffectiveDpi(mutation.monitor));
    // Floating leases end with the layout; tiling places every window afresh.
    for (HWND hwnd : candidate.TiledWindows()) CancelConformanceLease(hwnd);
  } else if (!wasFloating && isFloating) {
    RestoreWorkspaceFloatingGeometry(key, candidate);
  }

  g_workspaces.Save(key, candidate);
  for (HWND hwnd : movedTiles) Gestures::ClearMoveSizeSamples(hwnd);
  ArrangeWorkspace(key, mutation.subject);
  return true;
}

// --- Keeping or rolling back the pending mutation -----------------------------

// The user layout mutation pending on `key`, or nullptr. One whose windows have
// changed since (`workspace` no longer has its candidate's membership) can't be
// rolled back safely: it is discarded, and the discard is logged as found
// during `stage`.
static const PendingLayoutMutation* CurrentLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& workspace, const wchar_t* stage) {
  const PendingLayoutMutation* pending = g_pendingProofs.FindLayoutMutation(key);
  if (!pending || workspace.HasSameWindowMembershipAs(pending->candidate)) return pending;
  Diagnostics::RecordEvent(
      L"discarded stale layout mutation %ls after membership change action=%ls desktop=%08X",
      stage, pending->action, key.desktopId.Data1);
  g_pendingProofs.DiscardLayoutMutation(key);
  return nullptr;
}

// Plans `workspace` for arrangement as the mutation pending on `key` requires. A
// rotated split is planned as it is, without turning a new split; an edge drop
// must keep overlapping its requested edge. Otherwise, in BSP, a still-settling
// arrival's split may be turned to fit (Layout::PlanOrTurnNewBspSplit).
static Layout::LayoutPlanStatus PlanWithPendingLayoutMutation(
    const DesktopMonitorKey& key, Workspace& workspace, const RECT& workArea, LONG gap, std::vector<RECT>* rects) {
  const PendingLayoutMutation* pending = g_pendingProofs.FindLayoutMutation(key);
  if (pending && pending->kind == UserLayoutMutationKind::BspRotate) {
    return Layout::BuildWorkspaceLayoutPlan(workspace, workArea, gap, rects);
  }
  std::optional<Layout::BspMergeDestination> merge;
  if (pending && pending->bspDropEdge) merge = Layout::BspMergeDestination{pending->subject, *pending->bspDropEdge};
  return Layout::PlanOrTurnNewBspSplit(workspace, workArea, gap, rects, merge);
}

static bool TryRollbackPendingLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor, Workspace* workspace,
    const std::vector<RECT>& rejectedRects, HWND culpritHint = nullptr) {
  if (!workspace) return false;
  // Constraint learning and admission recovery are allowed to adjust ratios or
  // reorder pending admissions downstream of the user's command. Those are part
  // of the same delayed transaction, not evidence that the transaction is stale.
  // Only a membership/participation change makes the saved rollback snapshot
  // unsafe to apply.
  const PendingLayoutMutation* pending = CurrentLayoutMutation(key, *workspace, L"rollback");
  if (!pending) return false;

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
  // A rejected split change restores its previous geometry even if that
  // layout now needs constraint recovery of its own.
  if (mutation.kind != UserLayoutMutationKind::BspMerge && mutation.kind != UserLayoutMutationKind::BspRotate) {
    std::vector<RECT> restoredRects;
    const auto restoredStatus = Layout::BuildWorkspaceLayoutPlan(
        restored, rollbackMetrics.workArea, rollbackMetrics.gap, &restoredRects);
    if (restoredStatus == Layout::LayoutPlanStatus::Unsatisfiable) {
      Diagnostics::RecordEvent(
          L"layout mutation rollback no longer feasible action=%ls desktop=%08X",
          mutation.action, key.desktopId.Data1);
      g_pendingProofs.DiscardLayoutMutation(key);
      return false;
    }
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
  const PendingLayoutMutation* pending = CurrentLayoutMutation(key, workspace, L"settle");
  if (!pending) return;
  if (HasOutstandingConformanceLease(workspace)) return;
  Diagnostics::RecordEvent(
      L"layout mutation settled action=%ls desktop=%08X",
      pending->action, key.desktopId.Data1);
  g_pendingProofs.DiscardLayoutMutation(key);
}

}  // namespace Placement
