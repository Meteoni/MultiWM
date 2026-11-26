// Remembered floating geometry and placement policy.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Placement {

static FloatingPlacementHint MakeFloatingPlacementHintFromGesture(
    const MoveSizeGesture& gesture) {
  FloatingPlacementHint hint;
  if (gesture.intent != MoveSizeIntent::Move) return hint;

  hint.intent = FloatingPlacementIntent::PreserveAnchor;
  // Preserve the moved window's final center rather than the mouse pointer. The
  // pointer is usually on the title bar, so centering a repaired window on it
  // would visibly jump away from the user's chosen placement.
  if (gesture.hasRects) {
    hint.anchor = SharedGeometry::RectCenter(gesture.end);
  } else if (gesture.hasDropPoint) {
    hint.anchor = gesture.dropPoint;
  }
  return hint;
}

// Places a floating window at `target` and remembers the frame it ends up with,
// captured at `dpi`. A Floating-layout member placed exactly where asked gets a
// floating lease, so an app that snaps back right afterwards is put back
// (placement/conformance.h). False when the placement failed; nothing is
// remembered then.
static bool PlaceFloatingWindow(
    Workspace& workspace, HWND hwnd, UINT dpi, const RECT& target, bool positionOnly) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record) return false;
  const bool floatingLayoutMember =
      workspace.Layout() == TileLayout::Floating && record->state == ManageState::Tiled;
  PlacementObservation observation =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, target, positionOnly);
  if (observation.result != PlacementResult::Success &&
      observation.result != PlacementResult::AdjustedByWindow) {
    return false;
  }

  const RECT remembered =
      observation.observed.right > observation.observed.left &&
              observation.observed.bottom > observation.observed.top
          ? observation.observed
          : target;
  workspace.RememberFloatingGeometry(hwnd, remembered, dpi);
  ++Diagnostics::g_runtime.counters.floatingGeometryRepairs;
  if (floatingLayoutMember && observation.result == PlacementResult::Success &&
      observation.placementIssued) {
    BeginConformanceLease(hwnd, remembered, false, nullptr, ConformanceLeaseKind::Floating);
  }
  return true;
}

static std::optional<SharedGeometry::FrameLimits> FloatingFrameLimits(
    WindowConstraints constraints, UINT dpi) {
  if (!constraints.valid || !constraints.dpi) return std::nullopt;
  constraints = Model::RescaleConstraintsToDpi(constraints, dpi);
  return SharedGeometry::FrameLimits{
      constraints.minWidth, constraints.minHeight, constraints.maxWidth, constraints.maxHeight};
}

// `hwnd`'s size limits as visible-frame sizes at `dpi`: its cached report, or a
// fresh one (stored in `workspace`) when none was taken at that DPI. The probe
// runs application code.
static std::optional<SharedGeometry::FrameLimits> FloatingSizeLimits(
    Workspace& workspace, HWND hwnd, UINT dpi) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record) return std::nullopt;
  WindowConstraints constraints = record->constraints;
  if (!constraints.valid || constraints.dpi != dpi) {
    constraints = Model::MergeConstraintProbeWithCache(
        constraints, Platform::Windows::ProbeWindowConstraints(hwnd));
    workspace.UpdateConstraints(hwnd, constraints);
  }
  return FloatingFrameLimits(constraints, dpi);
}

static SIZE FloatingDefaultSizeAtDpi(UINT dpi) {
  return {SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.width, dpi),
          SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.height, dpi)};
}

// What `intent` sizes a floating window by (SharedGeometry::FloatingSizeSource).
static SharedGeometry::FloatingSizeSource FloatingSizeSourceFor(FloatingPlacementIntent intent) {
  switch (intent) {
    case FloatingPlacementIntent::NewWindowCenter:
      return SharedGeometry::FloatingSizeSource::Fallback;
    case FloatingPlacementIntent::RuleFloatedCenter:
      return SharedGeometry::FloatingSizeSource::Own;
    case FloatingPlacementIntent::PassiveRestore:
    case FloatingPlacementIntent::PreserveAnchor:
    case FloatingPlacementIntent::CenterWithFloatingSize:
    case FloatingPlacementIntent::FailureCenter:
      break;
  }
  return SharedGeometry::FloatingSizeSource::Remembered;
}

// Places a floating window as `hint` asks, by the rule both mods share
// (SharedGeometry::PlanFloatingPlacement). A drag keeps the user's drop point;
// a passive restore or a failed tile keeps a remembered frame's center on its
// own monitor; every other placement is centered on the work area.
static bool PlaceFloatingWindowByHint(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const FloatingPlacementHint& hint) {
  const WindowRecord* record = workspace.Find(hwnd);
  // A hung window shown by its ghost can't be placed (see PlaceTiledWindow);
  // the ghost stays wherever it is, or wherever the user drags it.
  if (!record || !record->canMove || !IsWindow(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      Platform::Windows::GhostWindowOf(hwnd)) {
    return false;
  }

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return false;
  const RECT& workArea = metrics.workArea;
  const UINT targetDpi = metrics.dpi;

  SharedGeometry::FloatingRequest request;
  request.source = FloatingSizeSourceFor(hint.intent);
  request.workArea = workArea;
  request.canResize = record->canResize;
  request.honorPreserveSize = hint.intent != FloatingPlacementIntent::FailureCenter;
  request.defaultSize = FloatingDefaultSizeAtDpi(targetDpi);
  RECT current{};
  if (Platform::Windows::GetWindowFrameRect(hwnd, &current) && current.right > current.left &&
      current.bottom > current.top) {
    request.current = SIZE{SharedGeometry::Width(current), SharedGeometry::Height(current)};
  }
  if (record->hasFloatingRect) {
    const int sourceDpi = static_cast<int>(record->floatingDpi ? record->floatingDpi : 96);
    const int dpi = static_cast<int>(targetDpi);
    request.remembered = SIZE{MulDiv(SharedGeometry::Width(record->floatingRect), dpi, sourceDpi),
                              MulDiv(SharedGeometry::Height(record->floatingRect), dpi, sourceDpi)};
  }
  // A remembered size wins over rules, so they are only looked up when needed.
  const bool usesRules =
      request.source != SharedGeometry::FloatingSizeSource::Remembered || !record->hasFloatingRect;
  if (usesRules && RuntimeLifecycle::IsAutomaticMode()) {
    if (const WindowRule* rule =
            Platform::Windows::FindMatchingWindowRule(hwnd, WindowRuleTreatment::FloatingPlacementOverride)) {
      request.rule = SharedGeometry::ResolveFloatingSizeRule(*rule, workArea, targetDpi);
    }
  }
  if (hint.intent == FloatingPlacementIntent::PreserveAnchor) {
    request.center = hint.anchor;
  } else if ((hint.intent == FloatingPlacementIntent::PassiveRestore ||
              hint.intent == FloatingPlacementIntent::FailureCenter) &&
             record->hasFloatingRect &&
             MonitorFromRect(&record->floatingRect, MONITOR_DEFAULTTONULL) == monitor) {
    request.center = SharedGeometry::RectCenter(record->floatingRect);
  }

  const std::optional<SharedGeometry::FloatingPlacement> placement =
      SharedGeometry::PlanFloatingPlacement(request);
  if (!placement) return false;
  // Fit the app's size limits: SWP_NOSENDCHANGING skips Windows' own clamp, so
  // without this the window is forced past them. Only probed when they apply.
  RECT target = placement->frame;
  if (placement->appliesLimits) {
    if (const auto limits = FloatingSizeLimits(workspace, hwnd, targetDpi)) {
      target = SharedGeometry::FitFloatingPlacementToLimits(*placement, *limits);
    }
  }
  // The window can enter a non-participating physical state while the placement
  // target is being calculated. Never overwrite it.
  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }
  return PlaceFloatingWindow(workspace, hwnd, targetDpi, target, placement->moveOnly);
}

// One grow or shrink step around the current center. Each dimension changes by
// a percentage of the work area's span, within the app and work-area limits.
static std::optional<RECT> PlanFloatingSizeStep(
    const RECT& current, const Platform::Monitors::WorkspaceMetrics& metrics,
    const std::optional<SharedGeometry::FrameLimits>& limits, int direction) {
  const RECT& workArea = metrics.workArea;
  const LONG smallest = SharedGeometry::ScaleDip(100, metrics.dpi);
  auto stepSpan = [&](LONG span, LONG areaSpan, LONG minimum, LONG maximum) {
    const LONG step = std::max<LONG>(1, MulDiv(areaSpan, static_cast<int>(g_settings.resizeStepPercent), 100));
    const LONG lowest = std::max(smallest, limits ? minimum : 1);
    LONG highest = areaSpan;
    if (limits && maximum > 0) highest = std::min(highest, maximum);
    return direction > 0 ? std::max(span, std::min(span + step, highest))
                         : std::min(span, std::max(span - step, lowest));
  };
  const LONG width = Layout::RectWidth(current);
  const LONG height = Layout::RectHeight(current);
  const SharedGeometry::FrameLimits none{};
  const SharedGeometry::FrameLimits& frame = limits ? *limits : none;
  const LONG newWidth = stepSpan(width, Layout::RectWidth(workArea), frame.minWidth, frame.maxWidth);
  const LONG newHeight = stepSpan(height, Layout::RectHeight(workArea), frame.minHeight, frame.maxHeight);
  if (newWidth == width && newHeight == height) return std::nullopt;

  return SharedGeometry::ClampToArea(
      SharedGeometry::CenteredRect(SharedGeometry::RectCenter(current), newWidth, newHeight), workArea);
}

static bool StepFloatingWindowSize(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd, int direction) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !record->canMove || !record->canResize || !IsWindow(hwnd) ||
      Platform::Windows::GhostWindowOf(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }
  HMONITOR monitor = key.ResolveMonitor();
  Platform::Monitors::WorkspaceMetrics metrics;
  RECT current{};
  if (!monitor || !Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics) ||
      !Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    return false;
  }

  const auto target = PlanFloatingSizeStep(
      current, metrics, FloatingSizeLimits(workspace, hwnd, metrics.dpi), direction);
  return target && PlaceFloatingWindow(workspace, hwnd, metrics.dpi, *target, false);
}

// An explicit resize of a window outside the workspace model. A zero direction
// resets it to the configured floating size at the center of its work area.
static bool ResizeUnmanagedWindow(HWND hwnd, HMONITOR monitor, int direction) {
  const WindowRecord window = Platform::Windows::MakeWindowRecord(hwnd);
  Platform::Monitors::WorkspaceMetrics metrics;
  RECT current{};
  if (!window.canMove || !window.canResize || Platform::Windows::GhostWindowOf(hwnd) ||
      !Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics) ||
      !Platform::Windows::GetWindowFrameRect(hwnd, &current)) {
    return false;
  }

  const auto limits = FloatingFrameLimits(Platform::Windows::ProbeWindowConstraints(hwnd), metrics.dpi);
  std::optional<RECT> target;
  if (direction) {
    target = PlanFloatingSizeStep(current, metrics, limits, direction);
  } else {
    const auto placement = SharedGeometry::PlanFloatingPlacement({
        .workArea = metrics.workArea, .defaultSize = FloatingDefaultSizeAtDpi(metrics.dpi)});
    if (!placement) return false;
    target = limits ? SharedGeometry::FitFloatingPlacementToLimits(*placement, *limits) : placement->frame;
  }
  if (!target || !Platform::Windows::IsSameLiveWindow(hwnd, window.pid)) return false;

  const auto result = Platform::Windows::PlaceWindowChecked(hwnd, window.canMove, *target).result;
  return result == PlacementResult::Success || result == PlacementResult::AdjustedByWindow;
}

static bool RememberCurrentFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record) return false;
  if (workspace.Layout() != TileLayout::Floating &&
      record->state != ManageState::Floating) {
    return false;
  }

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;

  RECT frame{};
  if (!Platform::Windows::GetDisplayedWindowFrameRect(hwnd, &frame) || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }
  return workspace.RememberFloatingGeometry(hwnd, frame, Platform::Monitors::GetMonitorEffectiveDpi(monitor));
}

// Leaving the Floating layout: remembers each active member's current frame,
// captured at `dpi`, so returning to the layout restores where the windows
// really were. A frame the app chose, or a keyboard snap, is not recorded
// otherwise. Minimized, maximized, hidden and cloaked members keep theirs.
static void RememberFloatingLayoutFrames(Workspace& workspace, UINT dpi) {
  for (HWND hwnd : workspace.TiledWindows()) {
    RECT frame{};
    if (Platform::Windows::IsSuspendedOrCloaked(hwnd) || !Platform::Windows::GetDisplayedWindowFrameRect(hwnd, &frame)) {
      continue;
    }
    workspace.RememberFloatingGeometry(hwnd, frame, dpi);
  }
}

// Entering the workspace-level Floating layout is a state restoration, not a
// per-window drag/float command. Restore each active tiled member's complete
// remembered floating rectangle; windows without memory use the configured
// centered fallback. Per-window Floating records are already user-positioned and
// deliberately remain untouched.
static void RestoreWorkspaceFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace) {
  if (workspace.Layout() != TileLayout::Floating) return;

  const std::vector<HWND> active = workspace.TiledWindows();
  const FloatingPlacementHint passiveRestore{};
  for (HWND hwnd : active) {
    PlaceFloatingWindowByHint(key, workspace, hwnd, passiveRestore);
  }
}

} // namespace Placement
