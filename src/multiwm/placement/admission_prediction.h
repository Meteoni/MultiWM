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
  // Floating layout: the frame a new window gets at the default floating size.
  std::optional<RECT> floatingFrame;
};

// Where Automatic discovery would place an unknown, resizable window that has no
// reported minimum size and matches no window rule, if it were admitted into
// `workspace` now.
//
// - Tiled layouts run the real admission (AdmitTiled, or AdmitTiledAfter the
//   retained focus when `insertAfterFocused`) on a copy and plan it with
//   BuildWorkspaceLayoutPlan, so the result is exactly what arrangement would
//   request. No tiled frame when admission would overflow `windowLimit`
//   (0 = unlimited) or the plan is unsatisfiable.
// - The Floating layout centers the configured default size in the work area,
//   as RepairFloatingGeometry does for a new window (NewWindowCenter).
//
// Pure model and geometry work: never queries or moves a window, never changes
// `workspace`. WM thread only, because Workspace mutators assert it.
static AdmissionPrediction PredictAutomaticAdmission(
    const Workspace& workspace, const Platform::Monitors::WorkspaceMetrics& metrics,
    size_t windowLimit, bool insertAfterFocused) {
  AdmissionPrediction prediction;
  if (workspace.HasRecord(kPredictedWindow)) return prediction;

  if (workspace.Layout() == TileLayout::Floating) {
    const LONG width = Platform::Monitors::ScaleDip(g_settings.floatingDefaultSizeDip.width, metrics.dpi);
    const LONG height = Platform::Monitors::ScaleDip(g_settings.floatingDefaultSizeDip.height, metrics.dpi);
    prediction.floatingFrame = ClampFloatingRectToWorkArea(
        CenteredRect(RectCenter(metrics.workArea), width, height), metrics.workArea);
    return prediction;
  }

  if (windowLimit && workspace.VisibleWindowCount() >= windowLimit) return prediction;

  Workspace trial = workspace;
  WindowRecord record;
  record.hwnd = kPredictedWindow;
  record.state = ManageState::Tiled;
  const HWND anchor = insertAfterFocused ? trial.LastFocusedTiledWindow() : nullptr;
  const bool admitted = anchor ? trial.AdmitTiledAfter(std::move(record), anchor)
                               : trial.AdmitTiled(std::move(record));
  if (!admitted) return prediction;

  std::vector<RECT> rects;
  if (Layout::BuildWorkspaceLayoutPlan(trial, metrics.workArea, metrics.gap, &rects) !=
      Layout::LayoutPlanStatus::Success) {
    return prediction;
  }
  const size_t index = trial.TiledIndex(kPredictedWindow);
  if (index < rects.size()) prediction.tiledFrame = rects[index];
  return prediction;
}

}  // namespace Placement
