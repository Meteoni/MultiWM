// Restoring a minimized or maximized member straight into the tile MultiWM will
// give it, using the exact restore tile MultiWM publishes for each suspended member.
// Part of the companion; included after placement.h and before show_hooks.h.
// This chapter also owns show-command and native coordinate conversion.

namespace Companion {

static decltype(&SetWindowPlacement) g_unhookedSetWindowPlacement = nullptr;

// Show commands that bring up a restored window. Maximized and minimized shows
// are suspended by MultiWM rather than tiled, so they are left alone.
static int ResolveShowCommand(int command) {
  if (command == SW_SHOWDEFAULT) {
    STARTUPINFOW startup{sizeof(startup)};
    GetStartupInfoW(&startup);
    command = (startup.dwFlags & STARTF_USESHOWWINDOW) ? startup.wShowWindow : SW_SHOWNORMAL;
  }
  return command;
}

static bool IsRestoredShowCommand(int command) {
  command = ResolveShowCommand(command);
  return command == SW_SHOWNORMAL || command == SW_SHOW || command == SW_SHOWNA ||
         command == SW_SHOWNOACTIVATE || command == SW_RESTORE;
}

// Geometry arguments use the caller's DPI coordinates. The conversion APIs use
// the window's context, so skip unusual mixed-context calls rather than guess.
// Per-monitor callers already use physical pixels, even for an unaware window.
static std::optional<RECT> ConvertScreenRect(
    HWND hwnd, const RECT& rect, BOOL(WINAPI* convert)(HWND, LPPOINT)) {
  const auto context = GetThreadDpiAwarenessContext();
  if (GetAwarenessFromDpiAwarenessContext(context) == DPI_AWARENESS_PER_MONITOR_AWARE) return rect;
  if (!AreDpiAwarenessContextsEqual(context, GetWindowDpiAwarenessContext(hwnd))) return std::nullopt;
  POINT topLeft{rect.left, rect.top};
  POINT bottomRight{rect.right, rect.bottom};
  if (!convert(hwnd, &topLeft) || !convert(hwnd, &bottomRight)) return std::nullopt;
  return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

// Points the window's restore at the tile MultiWM will give it. MultiWM
// publishes that tile for each suspended member by running its own Restore on a
// copy of the workspace, so the hint is exact, not a guess. Caller holds
// PlacementScope. Only the copy's normal rectangle is changed.
static bool AdjustRestorePlacement(HWND hwnd, WINDOWPLACEMENT* placement, const char* path) {
  const int command = ResolveShowCommand(static_cast<int>(placement->showCmd));
  if (command != SW_RESTORE && command != SW_SHOWNORMAL && command != SW_SHOWNOACTIVATE) return false;
  if ((!IsIconic(hwnd) && !IsZoomed(hwnd)) || RejectStructurally(hwnd, true) ||
      (IsIconic(hwnd) && (placement->flags & WPF_RESTORETOMAXIMIZED))) return false;

  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  RECT monitorRect{};
  {
    PhysicalCoordinatesScope physical;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo)) return false;
    monitorRect = monitorInfo.rcMonitor;
  }
  HintInputs inputs;
  if (!ReadHintInputs(hwnd, monitorRect, &inputs) || !inputs.tracked) return false;
  TargetKind kind;
  std::optional<RECT> overflowFrame;
  const char* reason = nullptr;
  // Known transitions ignore the proposed rectangle. Both hidden-normal shows
  // and min/max restores select the same model-owned restore or arrival intent.
  const auto frame = PredictFrame(hwnd, placement->rcNormalPosition, inputs, &kind, &overflowFrame, &reason);
  if (!frame) {
    LogDecision(hwnd, path, reason);
    return false;
  }
  const auto& workspace = *inputs.workspace;
  RECT target = *frame;
  // The tile is a visible frame; the restored window rectangle adds the
  // invisible resize borders. A minimized or maximized window can't report them
  // (see RememberFrameMargins in MultiWM), and system metrics overestimate them by
  // a few pixels, within MultiWM's placement tolerance, so MultiWM would never
  // correct the result. Use MultiWM's measurement from the restored window, or
  // give no hint.
  if (!inputs.restoreFrameMargins || inputs.restoreMarginsDpi != workspace.dpi) return false;
  const RECT& margins = *inputs.restoreFrameMargins;
  {
    // MultiWM planned the tile with the window's cached limits. Apply the live
    // ones the same way (FitSlotToLimits), which changes nothing unless they
    // changed since, then add the borders to get the rectangle Windows restores to.
    PhysicalCoordinatesScope physical;
    const auto limits = QueryFrameLimits(hwnd, workspace.dpi ? workspace.dpi : 96, margins);
    if (!limits) return false;
    const auto fitted = FitSlotToLimits(target, workspace.workArea, *limits);
    if (!fitted) return false;
    target = *fitted;
  }
  target = SharedGeometry::AddFrameMargins(target, margins);
  const auto logicalTarget = ConvertScreenRect(hwnd, target, PhysicalToLogicalPointForPerMonitorDPI);
  if (!logicalTarget || !GetMonitorInfoW(MonitorFromRect(&*logicalTarget, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
    return false;
  }
  placement->rcNormalPosition = *logicalTarget;
  OffsetRect(&placement->rcNormalPosition, monitorInfo.rcMonitor.left - monitorInfo.rcWork.left,
              monitorInfo.rcMonitor.top - monitorInfo.rcWork.top);
  LogDecision(hwnd, path, reason);
  return true;
}

// The SetWindowPlacement show command that updates only the normal rectangle:
// same visibility, same minimized/maximized state, no activation. Measured on a
// private desktop: SW_SHOWMAXIMIZED and SW_SHOWMINNOACTIVE both show a hidden
// window (SW_SHOWMAXIMIZED also activates it); SW_SHOWNA shows a hidden window;
// SW_HIDE keeps a hidden window hidden and keeps its state.
static UINT StatePreservingShowCommand(HWND hwnd) {
  if (!IsWindowVisible(hwnd)) return SW_HIDE;
  return IsIconic(hwnd) ? SW_SHOWMINNOACTIVE : SW_SHOWNA;
}

// Seed the rectangle Windows will restore, preserving the current min/max state.
// Never move the iconic/maximized rectangle itself, show the window early or
// activate it: the caller's own show call decides both.
static bool PrepareRestorePlacement(HWND hwnd, int command, const char* path) {
  if (g_placing || !hwnd || (!IsIconic(hwnd) && !IsZoomed(hwnd)) || RejectStructurally(hwnd, true)) return false;
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (!GetWindowPlacement(hwnd, &placement)) return false;
  placement.showCmd = static_cast<UINT>(command);
  PlacementScope placing;
  if (!AdjustRestorePlacement(hwnd, &placement, path)) return false;
  placement.showCmd = StatePreservingShowCommand(hwnd);
  return g_unhookedSetWindowPlacement && g_unhookedSetWindowPlacement(hwnd, &placement);
}

}  // namespace Companion
