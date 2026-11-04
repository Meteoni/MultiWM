// Remembered floating geometry and placement policy.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Placement {

static POINT RectCenter(const RECT& rect) {
  return {
      rect.left + (rect.right - rect.left) / 2,
      rect.top + (rect.bottom - rect.top) / 2};
}

static RECT CenteredRect(POINT center, LONG width, LONG height) {
  width = std::max<LONG>(1, width);
  height = std::max<LONG>(1, height);
  const LONG left = center.x - width / 2;
  const LONG top = center.y - height / 2;
  return {left, top, left + width, top + height};
}

static RECT ClampFloatingRectToWorkArea(RECT rect, const RECT& workArea) {
  const LONG areaWidth = std::max<LONG>(1, workArea.right - workArea.left);
  const LONG areaHeight = std::max<LONG>(1, workArea.bottom - workArea.top);
  const LONG width = std::min<LONG>(
      std::max<LONG>(1, rect.right - rect.left), areaWidth);
  const LONG height = std::min<LONG>(
      std::max<LONG>(1, rect.bottom - rect.top), areaHeight);

  LONG left = rect.left;
  LONG top = rect.top;
  if (left < workArea.left) left = workArea.left;
  if (top < workArea.top) top = workArea.top;
  if (left + width > workArea.right) left = workArea.right - width;
  if (top + height > workArea.bottom) top = workArea.bottom - height;
  return {left, top, left + width, top + height};
}

static FloatingPlacementHint MakeFloatingPlacementHintFromGesture(
    const MoveSizeGesture& gesture) {
  FloatingPlacementHint hint;
  if (gesture.intent != MoveSizeIntent::Move) return hint;

  hint.intent = FloatingPlacementIntent::PreserveAnchor;
  // Preserve the moved window's final center rather than the mouse pointer. The
  // pointer is usually on the title bar, so centering a repaired window on it
  // would visibly jump away from the user's chosen placement.
  if (gesture.hasRects) {
    hint.anchor = RectCenter(gesture.end);
  } else if (gesture.hasDropPoint) {
    hint.anchor = gesture.dropPoint;
  }
  return hint;
}

static HMONITOR ResolveRememberedFloatingMonitor(
    const WindowRecord& record) {
  if (!record.hasFloatingRect) return nullptr;
  if (Platform::Monitors::IsLiveMonitorHandle(record.floatingMonitor)) {
    return record.floatingMonitor;
  }

  // HMONITOR can be invalidated by a live display-topology rebuild. The remembered
  // physical rectangle is still a useful best-effort hint inside the same session;
  // if it maps to a current monitor, treat that as the remembered boundary.
  return MonitorFromRect(
      &record.floatingRect, MONITOR_DEFAULTTONULL);
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

  const bool centerWithoutResize =
      hint.intent == FloatingPlacementIntent::CenterPreservingSize;
  const bool useNewWindowSize =
      hint.intent == FloatingPlacementIntent::NewWindowCenter ||
      hint.intent == FloatingPlacementIntent::FailureCenter;
  const bool centerInWorkspace =
      hint.intent == FloatingPlacementIntent::CenterWithFloatingSize ||
      useNewWindowSize || centerWithoutResize;
  if (centerWithoutResize && !haveCurrent) return false;

  const bool needsFallbackSize =
      useNewWindowSize || centerWithoutResize || !record->hasFloatingRect;
  const WindowRule* placementOverride =
      RuntimeLifecycle::IsAutomaticMode() && needsFallbackSize && !centerWithoutResize
          ? Platform::Windows::FindMatchingWindowRule(
                hwnd, WindowRuleTreatment::FloatingPlacementOverride)
          : nullptr;
  const bool preserveCurrentSize =
      hint.intent != FloatingPlacementIntent::FailureCenter &&
      placementOverride && placementOverride->preserveFloatingSize;
  const bool useCustomSize =
      placementOverride && !placementOverride->preserveFloatingSize;

  LONG width = 0;
  LONG height = 0;
  if (centerWithoutResize || (preserveCurrentSize && haveCurrent)) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  } else if (useCustomSize) {
    if (placementOverride->useWorkspaceSize) {
      width = workArea.right - workArea.left;
      height = workArea.bottom - workArea.top;
    } else {
      width = Platform::Monitors::ScaleDip(placementOverride->floatingSizeDip.width, targetDpi);
      height = Platform::Monitors::ScaleDip(placementOverride->floatingSizeDip.height, targetDpi);
    }
  } else if (!useNewWindowSize && record->hasFloatingRect) {
    const UINT sourceDpi = record->floatingDpi ? record->floatingDpi : 96;
    width = MulDiv(
        record->floatingRect.right - record->floatingRect.left,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
    height = MulDiv(
        record->floatingRect.bottom - record->floatingRect.top,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
  } else {
    width = Platform::Monitors::ScaleDip(g_settings.floatingDefaultSizeDip.width, targetDpi);
    height = Platform::Monitors::ScaleDip(g_settings.floatingDefaultSizeDip.height, targetDpi);
  }

  if (!record->canResize && haveCurrent) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  }

  POINT center{};
  if (hint.intent == FloatingPlacementIntent::PreserveAnchor) {
    center = hint.anchor;
  } else if (!centerInWorkspace && record->hasFloatingRect &&
             ResolveRememberedFloatingMonitor(*record) == monitor) {
    center = RectCenter(record->floatingRect);
  } else {
    center = RectCenter(workArea);
  }

  RECT target = CenteredRect(center, width, height);
  if (!centerWithoutResize) {
    target = ClampFloatingRectToWorkArea(target, workArea);
  }
  // The window can enter a non-participating physical state while the placement
  // target is being calculated. Never overwrite it.
  if (Platform::Windows::GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }
  PlacementObservation observation =
      Platform::Windows::PlaceWindowChecked(hwnd, record->canMove, target, centerWithoutResize);
  if (observation.result != PlacementResult::Success &&
      observation.result != PlacementResult::AdjustedByWindow) {
    return false;
  }

  const RECT remembered =
      observation.observed.right > observation.observed.left &&
              observation.observed.bottom > observation.observed.top
          ? observation.observed
          : target;
  workspace.RememberFloatingGeometry(hwnd, remembered, monitor, targetDpi);
  ++Diagnostics::g_runtime.counters.floatingGeometryRepairs;
  return true;
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
  return workspace.RememberFloatingGeometry(
      hwnd, frame, monitor, Platform::Monitors::GetMonitorEffectiveDpi(monitor));
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
