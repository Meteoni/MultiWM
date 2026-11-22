// Accent-colored feedback during a managed tiled-window drag.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace DragPreview {

constexpr wchar_t kPreviewClassName[] = L"WindhawkMultiWMDragPreview";

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

constexpr UINT_PTR kFadeTimer = 1;
constexpr UINT kFadeIntervalMs = 16;

struct Overlay {
  HWND fill = nullptr;
  HWND outline = nullptr;
  ATOM windowClass = 0;
  COLORREF accent = 0;
  COLORREF colorKey = RGB(1, 0, 1);
  BYTE fadeAlpha = 0;
  BYTE fadeFrom = 0;
  BYTE fadeTo = 0;
  ULONGLONG fadeStarted = 0;
  std::optional<Commands::TiledDropPreview> shown;
};

static Overlay g_overlay;

static bool ApplyAlpha(BYTE fadeAlpha) {
  g_overlay.fadeAlpha = fadeAlpha;
  const BYTE fillAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_settings.dragPreviewFillOpacity, 100));
  const BYTE outlineAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_settings.dragPreviewOutlineOpacity, 100));
  const BOOL fillSet = SetLayeredWindowAttributes(g_overlay.fill, 0, fillAlpha, LWA_ALPHA);
  const BOOL outlineSet = SetLayeredWindowAttributes(
      g_overlay.outline, g_overlay.colorKey, outlineAlpha, LWA_COLORKEY | LWA_ALPHA);
  return fillSet && outlineSet;
}

static void StepFade(ULONGLONG now) {
  const UINT duration = g_settings.dragPreviewFadeMs;
  const ULONGLONG elapsed = now - g_overlay.fadeStarted;
  const BYTE alpha = elapsed >= duration
      ? g_overlay.fadeTo
      : static_cast<BYTE>(g_overlay.fadeFrom + MulDiv(g_overlay.fadeTo - g_overlay.fadeFrom, static_cast<int>(elapsed),
                                                     duration));
  if (alpha != g_overlay.fadeAlpha) ApplyAlpha(alpha);
  if (elapsed < duration) return;

  KillTimer(g_overlay.fill, kFadeTimer);
  if (alpha == 0) {
    ShowWindow(g_overlay.fill, SW_HIDE);
    ShowWindow(g_overlay.outline, SW_HIDE);
  }
}

// The window timer runs only during the transition.
static void FadeTo(BYTE alpha, bool restart = false) {
  if (!g_overlay.fill || !g_overlay.outline || (!restart && g_overlay.fadeTo == alpha)) return;
  g_overlay.fadeFrom = g_overlay.fadeAlpha;
  g_overlay.fadeTo = alpha;
  g_overlay.fadeStarted = GetTickCount64();
  if (g_overlay.fadeFrom == alpha || g_settings.dragPreviewFadeMs == 0 ||
      !SetTimer(g_overlay.fill, kFadeTimer, kFadeIntervalMs, nullptr)) {
    StepFade(g_overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  }
}

static LRESULT CALLBACK PreviewWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_TIMER && hwnd == g_overlay.fill && wparam == kFadeTimer) {
    StepFade(GetTickCount64());
    return 0;
  }
  if (message != WM_PAINT) return DefWindowProcW(hwnd, message, wparam, lparam);

  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  HBRUSH accent = CreateSolidBrush(g_overlay.accent);
  if (hwnd == g_overlay.outline) {
    HBRUSH transparent = CreateSolidBrush(g_overlay.colorKey);
    if (transparent) {
      FillRect(dc, &paint.rcPaint, transparent);
      if (accent) {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        const UINT dpi = GetDpiForWindow(hwnd);
        const int width = std::max(1, MulDiv(g_settings.dragPreviewOutlineWidthDip, dpi, 96));
        const int radius = std::min<LONG>(MulDiv(g_settings.dragPreviewOutlineRadiusDip, dpi, 96),
                                         std::min(Layout::RectWidth(bounds), Layout::RectHeight(bounds)) / 2);
        auto paintRoundedRect = [dc](const RECT& rect, int cornerRadius, HBRUSH brush) {
          if (IsRectEmpty(&rect)) return;
          if (cornerRadius > 0) {
            const int diameter = 2 * cornerRadius;
            // GDI drops the last row and column from the rounded region's bounds.
            if (HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                                                 diameter, diameter)) {
              FillRgn(dc, region, brush);
              DeleteObject(region);
              return;
            }
          }
          FillRect(dc, &rect, brush);
        };
        paintRoundedRect(bounds, radius, accent);
        InflateRect(&bounds, -width, -width);
        paintRoundedRect(bounds, radius - width, transparent);
      }
      DeleteObject(transparent);
    }
  } else if (accent) {
    FillRect(dc, &paint.rcPaint, accent);
  }
  if (accent) DeleteObject(accent);
  EndPaint(hwnd, &paint);
  return 0;
}

static void Show(const Commands::TiledDropPreview& preview) {
  if (g_overlay.fadeTo == 255 && g_overlay.shown &&
      EqualRect(&g_overlay.shown->dragged, &preview.dragged) &&
      EqualRect(&g_overlay.shown->affected, &preview.affected)) {
    return;
  }

  const HINSTANCE module = GetModuleHandleW(nullptr);
  if (!g_overlay.windowClass) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = PreviewWindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = kPreviewClassName;
    g_overlay.windowClass = RegisterClassW(&windowClass);
    if (!g_overlay.windowClass) return;
  }
  for (HWND* window : {&g_overlay.fill, &g_overlay.outline}) {
    if (*window) continue;
    *window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kPreviewClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module, nullptr);
    if (!*window) return;
    const DWM_WINDOW_CORNER_PREFERENCE corners = window == &g_overlay.fill ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(*window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  }

  DWORD accent = 0;
  BOOL opaqueBlend = FALSE;
  g_overlay.accent = SUCCEEDED(DwmGetColorizationColor(&accent, &opaqueBlend))
      ? RGB((accent >> 16) & 0xff, (accent >> 8) & 0xff, accent & 0xff)
      : GetSysColor(COLOR_HIGHLIGHT);
  g_overlay.colorKey = g_overlay.accent == RGB(1, 0, 1) ? RGB(0, 1, 0) : RGB(1, 0, 1);
  // Reveal the new destination immediately at half strength. If the hint
  // is already fading in, let that fade finish without a new opacity drop.
  const bool changedTarget = g_overlay.fadeTo == 255 && g_overlay.fadeAlpha == 255 &&
                             g_settings.dragPreviewFadeMs > 0;
  if (!ApplyAlpha(changedTarget ? 128 : g_overlay.fadeAlpha)) {
    FadeTo(0);
    return;
  }

  auto position = [](HDWP batch, HWND window, const RECT& frame, LONG opacity) {
    const UINT visibility = opacity > 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    return batch ? DeferWindowPos(batch, window, HWND_TOPMOST, frame.left, frame.top, Layout::RectWidth(frame),
                                  Layout::RectHeight(frame), visibility | SWP_NOACTIVATE | SWP_NOOWNERZORDER)
                 : nullptr;
  };
  HDWP batch = position(BeginDeferWindowPos(2), g_overlay.fill, preview.dragged, g_settings.dragPreviewFillOpacity);
  batch = position(batch, g_overlay.outline, preview.affected, g_settings.dragPreviewOutlineOpacity);
  if (!batch || !EndDeferWindowPos(batch)) {
    FadeTo(0);
    return;
  }
  InvalidateRect(g_overlay.fill, nullptr, FALSE);
  InvalidateRect(g_overlay.outline, nullptr, FALSE);
  g_overlay.shown = preview;
  FadeTo(255, changedTarget);
}

static void End(HWND source = nullptr) {
  if (source && (!g_drag || g_drag->source != source)) return;
  g_drag.reset();
  FadeTo(0);
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
        Show(*preview);
        return;
      }
    }
    // A failed or empty drop returns a movable window to its tile. A ghost
    // window cannot be moved back by MultiWM.
    const WindowRecord* source = drag.workspace.Find(drag.source);
    if (drag.displayedWindow == drag.source && source && source->canMove && !IsRectEmpty(&drag.start)) {
      Show({drag.start, drag.start});
      return;
    }
  }
  FadeTo(0);
}

static void OnWindowEvent(DWORD event, HWND hwnd) {
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
    FadeTo(0);
    return;
  }

  UpdateDropPreview(drag, cursor);
}

static void Shutdown() {
  g_drag.reset();
  if (g_overlay.fill) DestroyWindow(g_overlay.fill);
  if (g_overlay.outline) DestroyWindow(g_overlay.outline);
  if (g_overlay.windowClass) UnregisterClassW(kPreviewClassName, GetModuleHandleW(nullptr));
  g_overlay = {};
}

}  // namespace DragPreview
