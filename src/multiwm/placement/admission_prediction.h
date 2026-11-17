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
  // Non-Floating layouts: Automatic overflow will float a new window, whatever
  // its size, instead of tiling it. Whether it is then centred or routed is
  // AutomaticOverflowRoutes.
  bool newWindowFloats = false;
};

// For `trial`, a workspace whose plan a just-admitted minimal window
// (kPredictedWindow, minimum 1x1) made unsatisfiable: true when arrangement's
// recovery (RecoverUnsatisfiableLayout) will float that window. That is when no
// established window is larger than the work area (that one would go first),
// recovery can't move a pending admission to a slot that fits, and the windows
// already settled fit without the pending ones, which blames the newest pending
// admission: this one. A real window has a minimum at least as large, so it
// can't fit where the minimal one doesn't.
// Recovery runs without a preferred window here, the least restricted search;
// with one it can only find fewer slots. Not modelled: an explicit layout
// command still settling, whose rollback runs first.
// Only reached for an unsatisfiable plan, which arrangement resolves at once, so
// normally never; then a few extra plans. WM thread.
static bool RecoveryFloatsPredictedWindow(const Workspace& trial, const Platform::Monitors::WorkspaceMetrics& metrics) {
  if (FindWindowLargerThanWorkArea(trial, Layout::ActiveWindowConstraints(trial), metrics.workArea)) return false;
  const auto candidates = OrderRemovalCandidates(trial, nullptr);
  Layout::AdmissionRecoveryPlan recovery;
  if (Layout::TryRecoverPendingAdmission(trial, metrics.workArea, metrics.gap, candidates, nullptr, &recovery)) {
    return false;
  }
  return FindIntroducingPendingAdmission(trial, candidates, metrics.workArea, metrics.gap) == kPredictedWindow;
}

// Where Automatic discovery would place an unknown, resizable window that has no
// reported minimum size and matches no window rule, if it were admitted into
// `workspace` now.
//
// - Tiled layouts run the real admission (AdmitTiled, or AdmitTiledAfter the
//   retained focus when `insertAfterFocused`) on a copy and plan it with
//   BuildWorkspaceLayoutPlan, so the result is exactly what arrangement would
//   request. No tiled frame when `windowLimitReached` (the caller asks
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
  prediction.floatingFrame = SharedGeometry::ClampToArea(
      SharedGeometry::CenteredIn(metrics.workArea, width, height), metrics.workArea);
  if (workspace.Layout() == TileLayout::Floating) return prediction;

  if (windowLimitReached) {
    prediction.newWindowFloats = true;
    return prediction;
  }

  Workspace trial = workspace;
  WindowRecord record;
  record.hwnd = kPredictedWindow;
  record.state = ManageState::Tiled;
  const HWND anchor = insertAfterFocused ? trial.LastFocusedTiledWindow() : nullptr;
  const bool admitted = anchor ? trial.AdmitTiledAfter(std::move(record), anchor)
                               : trial.AdmitTiled(std::move(record));
  if (!admitted) return prediction;

  std::vector<RECT> rects;
  const Layout::LayoutPlanStatus status =
      Layout::BuildWorkspaceLayoutPlan(trial, metrics.workArea, metrics.gap, &rects);
  if (status == Layout::LayoutPlanStatus::Unsatisfiable) {
    prediction.newWindowFloats = RecoveryFloatsPredictedWindow(trial, metrics);
    return prediction;
  }
  if (status != Layout::LayoutPlanStatus::Success) return prediction;
  const size_t index = trial.TiledIndex(kPredictedWindow);
  if (index < rects.size()) prediction.tiledFrame = rects[index];
  return prediction;
}

// Where `hwnd`, a suspended member of `workspace`, will be tiled when it is
// restored: MultiWM's own Restore on a copy (saved position, saved weight or
// ratio, master status) planned with BuildWorkspaceLayoutPlan, which includes
// the window's cached minimum and maximum sizes. That is the computation
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
  if (Layout::BuildWorkspaceLayoutPlan(trial, metrics.workArea, metrics.gap, &rects) !=
      Layout::LayoutPlanStatus::Success) {
    return std::nullopt;
  }
  const size_t index = trial.TiledIndex(hwnd);
  if (index >= rects.size()) return std::nullopt;
  return rects[index];
}

}  // namespace Placement
