// Predicts where Automatic admission would put one more window. Published to the
// companion mod, which moves a new window there before it is first shown.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Placement {

// Stands in for the predicted window inside a workspace copy. Windows never uses
// it: user handles are 32-bit significant and this value sign-extends from
// 0xFFFFFFF0, an index far beyond any session's handle table.
inline const HWND kPredictedWindow = reinterpret_cast<HWND>(static_cast<intptr_t>(-16));

struct AdmissionPrediction {
  // Non-Floating layouts: the frame the new window's tile would get.
  std::optional<RECT> tiledFrame;
  // Every layout: the default floating size centred in the work area. Where a
  // new window goes in the Floating layout (NewWindowCenter), and where a window
  // that overflow floats is centred (FailureCenter), before size rules.
  std::optional<RECT> floatingFrame;
  // Non-Floating layouts: a new window will float regardless of its size.
  // BSP floats locally when it finds no fitting tile. Automatic overflow can
  // route other releases (AutomaticOverflowRoutes).
  bool newWindowFloats = false;
};

// Admits kPredictedWindow, with no size limits, as Automatic discovery admits a
// new window: after the retained focus when `insertAfterFocused`, else at the end.
static bool AdmitPredictedWindow(Workspace& trial, bool insertAfterFocused) {
  WindowRecord record;
  record.hwnd = kPredictedWindow;
  record.state = ManageState::Tiled;
  const HWND anchor = insertAfterFocused ? trial.LastFocusedTiledWindow() : nullptr;
  return anchor ? trial.AdmitTiledAfter(std::move(record), anchor) : trial.AdmitTiled(std::move(record));
}

// Where Automatic discovery would place an unknown, resizable window that has no
// reported minimum size and matches no window rule, if it were admitted into
// `workspace` now.
//
// - Tiled layouts run the real admission (AdmitTiled, or AdmitTiledAfter the
//   retained focus when `insertAfterFocused`) on a copy and plan it with
//   the same planner as arrangement, including alternate BSP tiles. No tiled
//   frame when `windowLimitReached` (the caller asks
//   WouldExceedAutomaticWindowLimit, which counts exactly as enforcement does)
//   or the plan is unsatisfiable. Both float the window instead
//   (`newWindowFloats`), except an unsatisfiable plan that recovery resolves
//   some other way.
// - The floating frame centres the configured default size in the work area,
//   as RepairFloatingGeometry does for NewWindowCenter and FailureCenter.
//
// Pure model and geometry work: never queries or moves a window, never changes
// `workspace`. Tiled migrations use these same insertion operations, so their
// arrivals share this template. WM thread only, because Workspace mutators assert it.
static AdmissionPrediction PredictAutomaticAdmission(
    const Workspace& workspace, const Platform::Monitors::WorkspaceMetrics& metrics,
    bool windowLimitReached, bool insertAfterFocused) {
  AdmissionPrediction prediction;
  if (workspace.HasRecord(kPredictedWindow)) return prediction;

  const LONG width = SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.width, metrics.dpi);
  const LONG height = SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.height, metrics.dpi);
  prediction.floatingFrame = SharedGeometry::CenteredClampedIn(metrics.workArea, width, height);
  if (workspace.Layout() == TileLayout::Floating) return prediction;

  if (windowLimitReached) {
    prediction.newWindowFloats = true;
    return prediction;
  }

  Workspace trial = workspace;
  if (!AdmitPredictedWindow(trial, insertAfterFocused)) return prediction;

  std::vector<RECT> rects;
  const Layout::LayoutPlanStatus status =
      Layout::PlanWithNewBspSplitFallback(trial, metrics.workArea, metrics.gap, &rects);
  if (status == Layout::LayoutPlanStatus::Unsatisfiable) {
    // Arrangement removes an intrinsically oversized window before searching
    // for a slot. Otherwise it blames the admission if the settled tiles fit.
    if (FindWindowLargerThanWorkArea(trial, Layout::ActiveWindowConstraints(trial), metrics.workArea)) return prediction;
    const auto candidates = OrderRemovalCandidates(trial, nullptr);
    Layout::AdmissionRecoveryPlan recovery;
    if (!Layout::TryRecoverPendingAdmission(trial, metrics.workArea, metrics.gap, candidates, nullptr, &recovery)) {
      prediction.newWindowFloats =
          FindIntroducingPendingAdmission(trial, candidates, metrics.workArea, metrics.gap) == kPredictedWindow;
      return prediction;
    }
    if (trial.Layout() != TileLayout::Bsp) return prediction;
    trial = std::move(recovery.workspace);
    rects = std::move(recovery.rects);
  } else if (status != Layout::LayoutPlanStatus::Success) {
    return prediction;
  }
  const size_t index = trial.TiledIndex(kPredictedWindow);
  if (index < rects.size()) prediction.tiledFrame = rects[index];
  return prediction;
}

// The tile kPredictedWindow gets in `trial` with no minimum, and the splits
// above it with the other windows' minimums (CompanionAbi::BspArrivalTile), so
// the companion can fit the window's own minimum exactly as the planner will.
// No value when the other windows don't fit. Plans only.
static std::optional<CompanionAbi::BspArrivalTile> MeasureBspArrivalTile(
    const Workspace& trial, const Platform::Monitors::WorkspaceMetrics& metrics) {
  std::vector<RECT> rects;
  if (Layout::BuildWorkspaceLayoutPlan(trial, metrics.workArea, metrics.gap, &rects) !=
      Layout::LayoutPlanStatus::Success) {
    return std::nullopt;
  }
  CompanionAbi::BspArrivalTile tile{};
  tile.tile = rects[trial.TiledIndex(kPredictedWindow)];
  const std::vector<SharedSplits::BspPathSplit> path =
      Layout::BspPathTo(trial.Bsp(), trial.TiledWindows(), metrics.workArea, metrics.gap,
                        Layout::ActiveWindowConstraints(trial), Layout::GetBspAdaptation(), kPredictedWindow);
  tile.pathLength = static_cast<uint32_t>(path.size());
  if (path.size() <= CompanionAbi::kMaxBspPathSplits) std::copy(path.begin(), path.end(), tile.path);
  return tile;
}

// True when every tiled window but kPredictedWindow holds its tile: none can
// still have its new split turned to make room (PlanWithNewBspSplitFallback).
static bool BspArrivalsSettled(const Workspace& workspace) {
  return std::none_of(workspace.TiledWindows().begin(), workspace.TiledWindows().end(), [&](HWND hwnd) {
    const WindowRecord* record = workspace.Find(hwnd);
    return hwnd != kPredictedWindow && record && record->pendingInsertionPlacement;
  });
}

// The tiles the next Automatic arrival in BSP `workspace` tries, in
// arrangement's order (CompanionAbi::BspArrivalTiles): the tile admission
// splits, then that split turned (PlanWithNewBspSplitFallback), then the same
// two at the largest other tile (TryRecoverPendingBspAdmission). Empty for
// other layouts. Pure; a few plans, published for the companion. WM thread only.
static CompanionAbi::BspArrivalTiles PredictBspArrivalTiles(
    const Workspace& workspace, const Platform::Monitors::WorkspaceMetrics& metrics, bool insertAfterFocused) {
  CompanionAbi::BspArrivalTiles tiles{};
  Workspace admitted = workspace;
  if (workspace.Layout() != TileLayout::Bsp || workspace.HasRecord(kPredictedWindow) ||
      !AdmitPredictedWindow(admitted, insertAfterFocused)) {
    return tiles;
  }
  tiles.othersSettled = BspArrivalsSettled(admitted) ? 1 : 0;
  tiles.gap = metrics.gap;
  tiles.adaptiveDeviation = Layout::GetBspAdaptation().deviation;
  auto add = [&](const Workspace& trial) {
    if (tiles.count == CompanionAbi::kMaxBspArrivalTiles) return;
    if (const auto tile = MeasureBspArrivalTile(trial, metrics)) tiles.tiles[tiles.count++] = *tile;
  };
  auto addBothDirections = [&](const Workspace& trial) {
    add(trial);
    const size_t split = trial.Bsp().Parent(trial.Bsp().FindLeaf(kPredictedWindow));
    if (split == BspTree::kNotFound) return;
    Workspace turned = trial;
    Layout::TurnBspSplit(turned, split, metrics.workArea);
    add(turned);
  };
  addBothDirections(admitted);
  if (Layout::MoveBspArrivalToLargestTile(admitted, kPredictedWindow)) addBothDirections(admitted);
  return tiles;
}

// Where `hwnd`, a suspended member of `workspace`, will be tiled when it is
// restored: MultiWM's own Restore on a copy (saved position, saved weight or
// ratio, master status) planned with the window's cached size limits. BSP also
// searches other tiles when the first cannot fit. That is the computation
// arrangement runs when the window really returns, so the result is exact
// unless something changes during the restore itself (a new minimum, a DPI
// change, the restore overflowing the window limit).
// No value for a Floating layout, a window Restore doesn't apply to, or an
// unsatisfiable plan. Pure model and geometry work; never changes `workspace`.
// WM thread only, because Workspace mutators assert it.
static std::optional<RECT> PredictRestoredFrame(
    const Workspace& workspace, HWND hwnd, const Platform::Monitors::WorkspaceMetrics& metrics) {
  if (workspace.Layout() == TileLayout::Floating) return std::nullopt;
  Workspace trial = workspace;
  if (!trial.Restore(hwnd)) return std::nullopt;
  std::vector<RECT> rects;
  if (Layout::PlanWithNewBspSplitFallback(trial, metrics.workArea, metrics.gap, &rects) !=
      Layout::LayoutPlanStatus::Success) {
    Layout::AdmissionRecoveryPlan recovery;
    if (trial.Layout() != TileLayout::Bsp ||
        !Layout::TryRecoverPendingAdmission(trial, metrics.workArea, metrics.gap, OrderRemovalCandidates(trial, nullptr),
                                            nullptr, &recovery)) {
      return std::nullopt;
    }
    trial = std::move(recovery.workspace);
    rects = std::move(recovery.rects);
  }
  const size_t index = trial.TiledIndex(hwnd);
  if (index >= rects.size()) return std::nullopt;
  return rects[index];
}

}  // namespace Placement
