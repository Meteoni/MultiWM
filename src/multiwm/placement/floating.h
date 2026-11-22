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

// `hwnd`'s size limits as visible-frame sizes at `dpi`: its cached report, or a
// fresh one (stored in `workspace`) when none was taken at that DPI. No value
// when the window has never answered. The probe runs application code.
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
  if (!constraints.valid || !constraints.dpi) return std::nullopt;
  constraints = Model::RescaleConstraintsToDpi(constraints, dpi);
  return SharedGeometry::FrameLimits{
      constraints.minWidth, constraints.minHeight, constraints.maxWidth, constraints.maxHeight};
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
static bool RepairFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const FloatingPlacementHint& hint) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !record->canMove || !IsWindow(hwnd) ||
      Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;

  Platform::Monitors::WorkspaceMetrics metrics;
  if (!Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) return false;
  const RECT& workArea = metrics.workArea;
  const UINT targetDpi = metrics.dpi;
  const HWND ghost = Platform::Windows::GhostWindowOf(hwnd);

  SharedGeometry::FloatingRequest request;
  request.source = ghost ? SharedGeometry::FloatingSizeSource::Own : FloatingSizeSourceFor(hint.intent);
  request.workArea = workArea;
  request.canResize = !ghost && record->canResize;
  request.honorPreserveSize = hint.intent != FloatingPlacementIntent::FailureCenter;
  request.defaultSize = SIZE{SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.width, targetDpi),
                             SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.height, targetDpi)};
  RECT current{};
  if (Platform::Windows::GetWindowFrameRect(ghost ? ghost : hwnd, &current) && current.right > current.left &&
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
  if (ghost) {
    if (!Platform::Windows::MoveGhostWindow(hwnd, ghost, target)) return false;
    workspace.RememberFloatingGeometry(hwnd, target, targetDpi);
    ++Diagnostics::g_runtime.counters.floatingGeometryRepairs;
    return true;
  }
  return PlaceFloatingWindow(workspace, hwnd, targetDpi, target, placement->moveOnly);
}

// Grows (direction > 0) or shrinks a floating window (a Floating-layout member
// or a per-window floating record) by one step around its center. Each dimension changes by the step percentage of the work area's,
// within the app's size limits and the work area. A window already past a limit
// is not pushed further in that direction. False when nothing changes.
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

  const RECT& workArea = metrics.workArea;
  const WindowConstraints& limits = record->constraints;
  const LONG smallest = SharedGeometry::ScaleDip(100, metrics.dpi);
  auto stepSpan = [&](LONG span, LONG areaSpan, LONG minimum, LONG maximum) {
    const LONG step = std::max<LONG>(1, MulDiv(areaSpan, static_cast<int>(g_settings.resizeStepPercent), 100));
    const LONG lowest = std::max(smallest, limits.valid ? minimum : 1);
    LONG highest = areaSpan;
    if (limits.valid && maximum > 0) highest = std::min(highest, maximum);
    return direction > 0 ? std::max(span, std::min(span + step, highest))
                         : std::min(span, std::max(span - step, lowest));
  };
  const LONG width = Layout::RectWidth(current);
  const LONG height = Layout::RectHeight(current);
  const LONG newWidth = stepSpan(width, Layout::RectWidth(workArea), limits.minWidth, limits.maxWidth);
  const LONG newHeight = stepSpan(height, Layout::RectHeight(workArea), limits.minHeight, limits.maxHeight);
  if (newWidth == width && newHeight == height) return false;

  const RECT target = SharedGeometry::ClampToArea(
      SharedGeometry::CenteredRect(SharedGeometry::RectCenter(current), newWidth, newHeight), workArea);
  return PlaceFloatingWindow(workspace, hwnd, metrics.dpi, target, false);
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
    RepairFloatingGeometry(key, workspace, hwnd, passiveRestore);
  }
}

} // namespace Placement
