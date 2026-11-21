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

// Converts remembered floating geometry into a destination-monitor rectangle.
// Passive migration restores the full rectangle on the same monitor and centers
// remembered/default size on a different monitor. Drag intent preserves the
// user's anchor; explicit centering can retain either floating or current size.
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

  RECT current{};
  const bool haveCurrent = Platform::Windows::GetWindowFrameRect(hwnd, &current) &&
                           current.right > current.left &&
                           current.bottom > current.top;

  const bool ruleFloatedCenter =
      hint.intent == FloatingPlacementIntent::RuleFloatedCenter;
  if (ruleFloatedCenter && !haveCurrent) return false;
  const bool useNewWindowSize =
      hint.intent == FloatingPlacementIntent::NewWindowCenter ||
      hint.intent == FloatingPlacementIntent::FailureCenter;
  const bool centerInWorkspace =
      hint.intent == FloatingPlacementIntent::CenterWithFloatingSize ||
      useNewWindowSize || ruleFloatedCenter;

  const bool needsFallbackSize =
      useNewWindowSize || ruleFloatedCenter || !record->hasFloatingRect;
  const WindowRule* placementOverride =
      RuntimeLifecycle::IsAutomaticMode() && needsFallbackSize
          ? Platform::Windows::FindMatchingWindowRule(
                hwnd, WindowRuleTreatment::FloatingPlacementOverride)
          : nullptr;
  const bool preserveCurrentSize =
      hint.intent != FloatingPlacementIntent::FailureCenter &&
      placementOverride && placementOverride->preserveFloatingSize;
  const bool useCustomSize =
      placementOverride && !placementOverride->preserveFloatingSize;
  // A rule-floated window kept at its own size is only moved, and may extend
  // past the work area. A size rule makes it an ordinary clamped placement,
  // unless the window can't be resized anyway.
  const bool centerWithoutResize =
      ruleFloatedCenter && (!useCustomSize || !record->canResize);

  LONG width = 0;
  LONG height = 0;
  if (centerWithoutResize || (preserveCurrentSize && haveCurrent)) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  } else if (useCustomSize) {
    const SIZE size = SharedGeometry::RuleFloatingSize(*placementOverride, workArea, targetDpi);
    width = size.cx;
    height = size.cy;
  } else if (!useNewWindowSize && record->hasFloatingRect) {
    const UINT sourceDpi = record->floatingDpi ? record->floatingDpi : 96;
    width = MulDiv(
        record->floatingRect.right - record->floatingRect.left,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
    height = MulDiv(
        record->floatingRect.bottom - record->floatingRect.top,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
  } else {
    width = SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.width, targetDpi);
    height = SharedGeometry::ScaleDip(g_settings.floatingDefaultSizeDip.height, targetDpi);
  }

  if (!record->canResize && haveCurrent) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  }

  POINT center{};
  if (hint.intent == FloatingPlacementIntent::PreserveAnchor) {
    center = hint.anchor;
  } else if (!centerInWorkspace && record->hasFloatingRect &&
             MonitorFromRect(&record->floatingRect, MONITOR_DEFAULTTONULL) == monitor) {
    center = SharedGeometry::RectCenter(record->floatingRect);
  } else {
    center = SharedGeometry::RectCenter(workArea);
  }

  RECT target = SharedGeometry::CenteredRect(center, width, height);
  if (!centerWithoutResize) {
    target = SharedGeometry::ClampToArea(target, workArea);
  }
  // Apply the app's size limits as Windows would: SWP_NOSENDCHANGING skips that,
  // so without this the window is forced past its own limits. The companion
  // predicts the same frame (ClampSizeKeepingOrigin).
  if (!centerWithoutResize && record->canResize) {
    if (const auto limits = FloatingSizeLimits(workspace, hwnd, targetDpi)) {
      target = SharedGeometry::ClampSizeKeepingOrigin(target, *limits);
    }
  }
  // The window can enter a non-participating physical state while the placement
  // target is being calculated. Never overwrite it.
  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }
  return PlaceFloatingWindow(workspace, hwnd, targetDpi, target, centerWithoutResize);
}

// Grows (direction > 0) or shrinks a floating window (a Floating-layout member
// or a per-window floating record) by one step around its center. Each dimension changes by the step percentage of the work area's,
// within the app's size limits and the work area. A window already past a limit
// is not pushed further in that direction. False when nothing changes.
static bool StepFloatingWindowSize(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd, int direction) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !record->canMove || !record->canResize || !IsWindow(hwnd) ||
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
  if (!Platform::Windows::GetWindowFrameRect(hwnd, &frame) || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }
  return workspace.RememberFloatingGeometry(hwnd, frame, Platform::Monitors::GetMonitorEffectiveDpi(monitor));
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
