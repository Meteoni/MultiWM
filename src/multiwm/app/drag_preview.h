// Accent-colored feedback during a managed tiled-window drag.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// The WM thread decides what to show: the drag session, its drop target and
// the planned frames. Overlay (app/overlay.h) draws it on the overlay thread.

namespace DragPreview {

struct DragSession {
  HWND source;
  HWND displayedWindow;
  HMONITOR monitor;
  DesktopMonitorKey key;
  RECT start;
  Workspace workspace;
  std::vector<Commands::TiledDropFrame> frames;
  std::optional<uint64_t> revision;
  std::optional<Commands::TiledDropTarget> target;
};

static std::optional<DragSession> g_drag;

// The settings a preview is drawn with, read on the WM thread.
static Overlay::PreviewStyle CurrentStyle() {
  return {g_settings.dragPreviewFillOpacity, g_settings.dragPreviewOutlineOpacity,
          g_settings.dragPreviewOutlineWidthDip, g_settings.dragPreviewOutlineRadiusDip,
          g_settings.dragPreviewIconSizeDip, g_settings.dragPreviewFadeMs, TrayUi::CurrentPopupAppearance()};
}

static void Show(const Commands::TiledDropPreview& preview, bool badges = false, UINT dpi = 96,
                 const GUID& desktopId = {}) {
  Overlay::ShowPreview({preview, CurrentStyle(), dpi, badges, desktopId});
}

static void Hide() { Overlay::HidePreview(); }

static void End(HWND source = nullptr) {
  if (source && (!g_drag || g_drag->source != source)) return;
  g_drag.reset();
  Hide();
}

static void Begin(HWND source, HWND displayedWindow) {
  End();
  if (!g_settings.DragPreviewEnabled() ||
      !Gestures::IsMoveSizeGestureInProgress(source) || !IsWindow(displayedWindow)) {
    return;
  }
  const HMONITOR monitor = Platform::Windows::GetWindowPhysicalMonitor(displayedWindow);
  DesktopMonitorKey key{};
  if (!Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) return;
  const auto start = Gestures::g_moveSize.startRects.find(source);
  if (start == Gestures::g_moveSize.startRects.end()) return;
  Overlay::BeginIconDrag();
  g_drag.emplace(DragSession{source, displayedWindow, monitor, std::move(key), start->second, {}, {}, {}, {}});
}

// Capture tile bounds once per workspace revision and replan only when the
// cursor enters a different drop zone (or leaves all target tiles).
static void UpdateDropPreview(DragSession& drag, POINT cursor) {
  const uint64_t revision = g_workspaces.Revision();
  const bool workspaceChanged = drag.revision != revision;
  if (workspaceChanged) {
    if (!g_workspaces.Load(drag.key, &drag.workspace) || drag.workspace.Layout() == TileLayout::Floating ||
        !drag.workspace.IsTiled(drag.source)) {
      End(drag.source);
      return;
    }
    drag.frames = Commands::TiledDropFrames(drag.workspace, drag.source);
    drag.revision = revision;
  }
  const auto target = Commands::FindTiledDropTarget(drag.frames, drag.workspace.Layout(), cursor);
  if (!workspaceChanged && target == drag.target) return;
  drag.target = target;

  Platform::Monitors::WorkspaceMetrics metrics;
  if (Platform::Monitors::GetWorkspaceMetrics(drag.monitor, &metrics)) {
    if (target) {
      if (const auto preview = Commands::PlanTiledDropPreview(drag.workspace, drag.source, *target, metrics)) {
        Show(*preview, true, metrics.dpi, drag.key.desktopId);
        return;
      }
    }
    // A failed or empty drop returns a movable window to its tile. A ghost
    // window cannot be moved back by MultiWM.
    const WindowRecord* source = drag.workspace.Find(drag.source);
    if (drag.displayedWindow == drag.source && source && source->canMove && !IsRectEmpty(&drag.start)) {
      Show({drag.start, drag.start, drag.source}, true, metrics.dpi, drag.key.desktopId);
      return;
    }
  }
  Hide();
}

static void OnWindowEvent(DWORD event, HWND hwnd) {
  if (event == EVENT_OBJECT_DESTROY) Overlay::ForgetWindow(hwnd);
  if (!g_drag || g_drag->source != hwnd) return;
  if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_CLOAKED ||
      event == EVENT_SYSTEM_MINIMIZESTART || !Gestures::IsMoveSizeGestureInProgress(hwnd)) {
    End(hwnd);
    return;
  }
  if (event != EVENT_OBJECT_LOCATIONCHANGE &&
      !(event == EVENT_OBJECT_STATECHANGE && g_drag->displayedWindow != hwnd)) {
    return;
  }

  auto& drag = *g_drag;
  RECT current{};
  POINT cursor{};
  if (!Platform::Windows::GetWindowFrameRect(drag.displayedWindow, &current) || !GetCursorPos(&cursor)) {
    End(hwnd);
    return;
  }
  if (MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL) != drag.monitor ||
      MonitorFromRect(&current, MONITOR_DEFAULTTONULL) != drag.monitor ||
      Gestures::ClassifyMoveSizeIntent(drag.start, current, 1, false) != MoveSizeIntent::Move) {
    drag.revision.reset();
    Hide();
    return;
  }

  UpdateDropPreview(drag, cursor);
}

// The drag session only; Overlay::Shutdown ends the drawing.
static void Shutdown() { g_drag.reset(); }

}  // namespace DragPreview
