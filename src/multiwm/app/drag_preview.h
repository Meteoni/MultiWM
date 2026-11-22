// Accent-colored feedback during a managed tiled-window drag.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// The WM thread decides what to show (the drag session, its drop target and
// the planned frames). A dedicated preview thread owns the preview's windows,
// loads app icons and draws, so a WM thread busy with a rearrange or a slow
// app never delays the preview. The WM thread sends each change as a
// PresentRequest that carries everything needed, settings included, so the
// preview thread never reads shared state.
//
// The fill and outline paint matching corners in layered windows. DWM rounds
// the app badges and supplies their border and shadow, as on the toasts.
// The preview thread steps opacity once per composition (DwmFlush).
//
// If the thread can't start, the calling thread paints the preview and uses a
// window timer during fades.

namespace DragPreview {

constexpr wchar_t kPreviewClassName[] = L"WindhawkMultiWMDragPreview";

using PreviewIcon = std::shared_ptr<std::remove_pointer_t<HICON>>;

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

// The settings a preview is drawn with, read on the WM thread.
struct PreviewStyle {
  LONG fillOpacity = 0;     // Percent.
  LONG outlineOpacity = 0;  // Percent.
  LONG outlineWidthDip = 0;
  LONG outlineRadiusDip = 0;
  LONG iconSizeDip = 0;
  UINT fadeMs = 0;
  COLORREF badgeBackground = 0;
};

static PreviewStyle CurrentStyle() {
  return {g_settings.dragPreviewFillOpacity, g_settings.dragPreviewOutlineOpacity,
          g_settings.dragPreviewOutlineWidthDip, g_settings.dragPreviewOutlineRadiusDip,
          g_settings.dragPreviewIconSizeDip, g_settings.dragPreviewFadeMs, TrayUi::FlyoutBackgroundColor()};
}

// One change to the shown preview, from the WM thread.
struct PresentRequest {
  Commands::TiledDropPreview preview;
  PreviewStyle style;
  UINT dpi = 96;
  bool badges = false;  // Show app icons (a real drag, not a bare preview).
};

struct IconBadge {
  HWND window = nullptr;
  PreviewIcon icon;
  LONG iconSize = 0;
  LONG badgeSize = 0;

  void Hide() {
    if (window) ShowWindow(window, SW_HIDE);
    icon.reset();
  }
};

struct Overlay {
  HWND fill = nullptr;
  HWND outline = nullptr;
  IconBadge draggedBadge;
  IconBadge affectedBadge;
  ATOM windowClass = 0;
  COLORREF accent = 0;
  COLORREF colorKey = RGB(1, 0, 1);
  PreviewStyle style;
  BYTE fadeAlpha = 0;
  BYTE fadeFrom = 0;
  BYTE fadeTo = 0;
  ULONGLONG fadeStarted = 0;
  UINT fadeMs = 0;      // The running fade's duration, fixed when it starts.
  bool fading = false;  // A fade is being stepped.
  // The preview thread steps fades itself, once per composition.
  bool paced = false;
  std::optional<PresentRequest> shown;
  // App icons by window for the current drag. Even missed lookups are kept: a
  // window can change its icon or stop responding.
  std::unordered_map<HWND, PreviewIcon> icons;
};

// Owned by the thread that presents: the preview thread once started, else the caller.
static Overlay g_overlay;

// Milliseconds on a high-resolution clock. GetTickCount64 advances in ~15.6 ms
// steps, coarser than a frame, so a fade timed by it moves unevenly.
static ULONGLONG PreviewClockMs() {
  static const LONGLONG frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return frequency >= 1000 ? static_cast<ULONGLONG>(now.QuadPart / (frequency / 1000)) : GetTickCount64();
}

static bool SameRequest(const PresentRequest& a, const PresentRequest& b) {
  return a.preview.draggedWindow == b.preview.draggedWindow && a.preview.affectedWindow == b.preview.affectedWindow &&
         EqualRect(&a.preview.dragged, &b.preview.dragged) && EqualRect(&a.preview.affected, &b.preview.affected);
}

// --- Fades -------------------------------------------------------------------------

static bool ApplyAlpha(BYTE fadeAlpha) {
  g_overlay.fadeAlpha = fadeAlpha;
  const BYTE fillAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_overlay.style.fillOpacity, 100));
  const BYTE outlineAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_overlay.style.outlineOpacity, 100));
  const BOOL fillSet = SetLayeredWindowAttributes(
      g_overlay.fill, g_overlay.colorKey, fillAlpha, LWA_COLORKEY | LWA_ALPHA);
  const BOOL outlineSet = SetLayeredWindowAttributes(
      g_overlay.outline, g_overlay.colorKey, outlineAlpha, LWA_COLORKEY | LWA_ALPHA);
  if (!fillSet || !outlineSet) return false;
  for (IconBadge* badge : {&g_overlay.draggedBadge, &g_overlay.affectedBadge}) {
    if (badge->icon && !SetLayeredWindowAttributes(badge->window, 0, fadeAlpha, LWA_ALPHA)) badge->Hide();
  }
  return true;
}

// The fade's opacity at `now`.
static BYTE AlphaAt(ULONGLONG now) {
  const UINT duration = g_overlay.fadeMs;
  const ULONGLONG elapsed = now - g_overlay.fadeStarted;
  return elapsed >= duration
      ? g_overlay.fadeTo
      : static_cast<BYTE>(g_overlay.fadeFrom + MulDiv(g_overlay.fadeTo - g_overlay.fadeFrom, static_cast<int>(elapsed),
                                                     duration));
}

static void HideOverlay() {
  g_overlay.fading = false;
  g_overlay.fadeAlpha = 0;
  g_overlay.fadeTo = 0;
  if (g_overlay.fill) {
    KillTimer(g_overlay.fill, kFadeTimer);
    ShowWindow(g_overlay.fill, SW_HIDE);
  }
  if (g_overlay.outline) ShowWindow(g_overlay.outline, SW_HIDE);
  g_overlay.draggedBadge.Hide();
  g_overlay.affectedBadge.Hide();
}

static void StepFade(ULONGLONG now) {
  const BYTE alpha = AlphaAt(now);
  if (alpha != g_overlay.fadeAlpha && !ApplyAlpha(alpha)) {
    HideOverlay();
    return;
  }
  if (now - g_overlay.fadeStarted < g_overlay.fadeMs) return;
  if (alpha == 0) {
    HideOverlay();
    return;
  }
  g_overlay.fading = false;
  KillTimer(g_overlay.fill, kFadeTimer);
}

// The synchronous fallback uses a window timer only during a transition.
static void FadeTo(BYTE alpha, bool restart = false) {
  if (!g_overlay.fill || !g_overlay.outline || (!restart && g_overlay.fadeTo == alpha)) return;
  g_overlay.fadeFrom = g_overlay.fadeAlpha;
  g_overlay.fadeTo = alpha;
  g_overlay.fadeStarted = PreviewClockMs();
  g_overlay.fadeMs = g_overlay.style.fadeMs;
  g_overlay.fading = true;
  const bool immediate = g_overlay.fadeFrom == alpha || g_overlay.fadeMs == 0;
  if (immediate || (!g_overlay.paced && !SetTimer(g_overlay.fill, kFadeTimer, kFadeIntervalMs, nullptr))) {
    StepFade(g_overlay.fadeStarted + g_overlay.fadeMs);
  }
}

// --- GDI painting ------------------------------------------------------------------

static void PaintRoundedRect(HDC dc, const RECT& rect, int cornerRadius, HBRUSH brush) {
  if (IsRectEmpty(&rect)) return;
  if (cornerRadius > 0) {
    const int diameter = 2 * cornerRadius;
    // GDI drops the last row and column from the rounded region's bounds.
    if (HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, diameter, diameter)) {
      FillRgn(dc, region, brush);
      DeleteObject(region);
      return;
    }
  }
  FillRect(dc, &rect, brush);
}

static void PaintPreviewShape(HWND hwnd, HDC dc, const RECT& update, HBRUSH accent) {
  HBRUSH transparent = CreateSolidBrush(g_overlay.colorKey);
  if (!transparent) return;
  FillRect(dc, &update, transparent);
  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  const UINT dpi = GetDpiForWindow(hwnd);
  const LONG radius = std::min<LONG>(MulDiv(g_overlay.style.outlineRadiusDip, dpi, 96),
                                     std::min(Layout::RectWidth(bounds), Layout::RectHeight(bounds)) / 2);
  PaintRoundedRect(dc, bounds, radius, accent);
  if (hwnd == g_overlay.outline) {
    const int width = std::max(1, MulDiv(g_overlay.style.outlineWidthDip, dpi, 96));
    InflateRect(&bounds, -width, -width);
    PaintRoundedRect(dc, bounds, radius - width, transparent);
  }
  DeleteObject(transparent);
}

static void PaintBadge(HDC dc, const PAINTSTRUCT& paint, const IconBadge& badge) {
  if (!badge.icon) return;
  if (HBRUSH background = CreateSolidBrush(g_overlay.style.badgeBackground)) {
    FillRect(dc, &paint.rcPaint, background);
    DeleteObject(background);
  }
  const LONG inset = (badge.badgeSize - badge.iconSize) / 2;
  DrawIconEx(dc, inset, inset, badge.icon.get(), badge.iconSize, badge.iconSize, 0, nullptr, DI_NORMAL);
}

static LRESULT CALLBACK PreviewWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_TIMER && hwnd == g_overlay.fill && wparam == kFadeTimer) {
    if (g_overlay.fading) StepFade(PreviewClockMs());
    return 0;
  }
  if (message != WM_PAINT) return DefWindowProcW(hwnd, message, wparam, lparam);

  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (hwnd == g_overlay.draggedBadge.window || hwnd == g_overlay.affectedBadge.window) {
    PaintBadge(dc, paint, hwnd == g_overlay.draggedBadge.window ? g_overlay.draggedBadge : g_overlay.affectedBadge);
  } else if (HBRUSH accent = CreateSolidBrush(g_overlay.accent)) {
    PaintPreviewShape(hwnd, dc, paint.rcPaint, accent);
    DeleteObject(accent);
  }
  EndPaint(hwnd, &paint);
  return 0;
}

// --- App icons ---------------------------------------------------------------------

// The width in pixels of `icon`'s image, or 0 when it can't be read.
static LONG IconPixelSize(HICON icon) {
  ICONINFO info{};
  if (!icon || !GetIconInfo(icon, &info)) return 0;
  BITMAP bitmap{};
  const HBITMAP image = info.hbmColor ? info.hbmColor : info.hbmMask;
  const LONG size = image && GetObjectW(image, sizeof(bitmap), &bitmap) ? bitmap.bmWidth : 0;
  if (info.hbmColor) DeleteObject(info.hbmColor);
  if (info.hbmMask) DeleteObject(info.hbmMask);
  return size;
}

// `icon` at `size` pixels, reloaded from the executable resource it records
// (most apps' class icons do), so the badge needs no stretching. Null when it
// records none; a copy of an icon loses that record, so pass the app's own.
static HICON ReloadIconAtSize(HICON icon, LONG size) {
  ICONINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetIconInfoExW(icon, &info)) return nullptr;
  if (info.hbmColor) DeleteObject(info.hbmColor);
  if (info.hbmMask) DeleteObject(info.hbmMask);
  if (!info.szModName[0]) return nullptr;
  return static_cast<HICON>(CopyImage(icon, IMAGE_ICON, size, size, LR_COPYFROMRESOURCE));
}

// For a badge `size` pixels wide, true when an icon `candidate` pixels wide
// beats one `current` wide: the smallest that covers the badge, failing that
// the largest.
static bool IsBetterBadgeIcon(LONG candidate, LONG current, LONG size) {
  const bool covers = candidate >= size;
  if (covers != (current >= size)) return covers;
  return covers ? candidate < current : candidate > current;
}

// The window's icon for a badge `size` pixels wide, as an owned copy: reloaded
// at exactly that size from its app's resources when the icon records them,
// else the smallest of the window's icons at least that large (shrunk), else
// the largest (stretched). The window's own icons come before its class's. The
// big icon is asked for only when the small ones won't do, since each
// WM_GETICON may wait up to 8 ms for a busy app. Null when it has none.
static PreviewIcon LoadBadgeIcon(HWND window, UINT dpi, LONG size) {
  auto ask = [&](WPARAM which) {
    DWORD_PTR handle = 0;
    SendMessageTimeoutW(window, WM_GETICON, which, dpi, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 8, &handle);
    return reinterpret_cast<HICON>(handle);
  };
  auto classIcon = [&](int index) { return reinterpret_cast<HICON>(GetClassLongPtrW(window, index)); };
  HICON chosen = nullptr;
  LONG chosenSize = 0;
  for (int step = 0; step < 4; ++step) {
    if (step == 2 && chosenSize >= size) break;  // The small icons already cover the badge.
    const HICON icon = step == 0 ? ask(ICON_SMALL) : step == 1 ? classIcon(GCLP_HICONSM)
                     : step == 2 ? ask(ICON_BIG) : classIcon(GCLP_HICON);
    if (!icon) continue;
    if (HICON exact = ReloadIconAtSize(icon, size)) return PreviewIcon(exact, DestroyIcon);
    const LONG pixels = IconPixelSize(icon);
    if (pixels > 0 && (!chosen || IsBetterBadgeIcon(pixels, chosenSize, size))) {
      chosen = icon;
      chosenSize = pixels;
    }
  }
  HICON copy = chosen ? CopyIcon(chosen) : nullptr;
  return copy ? PreviewIcon(copy, DestroyIcon) : PreviewIcon{};
}

// --- Windows -----------------------------------------------------------------------

static bool EnsureWindowClass() {
  if (g_overlay.windowClass) return true;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = PreviewWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kPreviewClassName;
  g_overlay.windowClass = RegisterClassW(&windowClass);
  return g_overlay.windowClass != 0;
}

static HWND CreatePreviewWindow(DWM_WINDOW_CORNER_PREFERENCE corners) {
  HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                kPreviewClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  if (window) DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  return window;
}

// --- App badges --------------------------------------------------------------------

static void ShowBadge(IconBadge& badge, HWND window, const RECT& tile, UINT dpi) {
  const LONG iconSize = SharedGeometry::ScaleDip(g_overlay.style.iconSizeDip, dpi);
  const LONG badgeSize = iconSize + SharedGeometry::ScaleDip(8, dpi);
  if (!window || iconSize <= 0 || badgeSize > Layout::RectWidth(tile) || badgeSize > Layout::RectHeight(tile)) {
    badge.Hide();
    return;
  }

  auto [entry, firstVisit] = g_overlay.icons.try_emplace(window);
  if (firstVisit) entry->second = LoadBadgeIcon(window, dpi, iconSize);
  if (!entry->second) {
    badge.Hide();
    return;
  }

  // DWM rounds the badge and gives it its border and shadow, as on the toasts.
  if (!badge.window) badge.window = CreatePreviewWindow(DWMWCP_ROUND);
  if (!badge.window) return;
  badge.badgeSize = badgeSize;
  badge.iconSize = iconSize;
  // Retain our own copy while a badge is fading out.
  badge.icon = entry->second;
  const POINT center = SharedGeometry::RectCenter(tile);
  if (!SetLayeredWindowAttributes(badge.window, 0, g_overlay.fadeAlpha, LWA_ALPHA) ||
      !SetWindowPos(badge.window, HWND_TOPMOST, center.x - badgeSize / 2, center.y - badgeSize / 2,
                    badgeSize, badgeSize, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
    badge.Hide();
    return;
  }
  InvalidateRect(badge.window, nullptr, FALSE);
}

// The dragged window's badge on the fill, the other window's on the outline.
static void ShowBadges(const PresentRequest& request) {
  const Commands::TiledDropPreview& preview = request.preview;
  const PreviewStyle& style = g_overlay.style;
  const bool sameTile = EqualRect(&preview.dragged, &preview.affected);
  if (request.badges && (style.fillOpacity > 0 || sameTile)) {
    ShowBadge(g_overlay.draggedBadge, preview.draggedWindow, preview.dragged, request.dpi);
  } else {
    g_overlay.draggedBadge.Hide();
  }
  if (request.badges && !sameTile && style.outlineOpacity > 0) {
    ShowBadge(g_overlay.affectedBadge, preview.affectedWindow, preview.affected, request.dpi);
  } else {
    g_overlay.affectedBadge.Hide();
  }
}

// --- Presentation (the presenting thread) ------------------------------------------

static bool EnsurePreviewWindows() {
  if (!EnsureWindowClass()) return false;
  for (HWND* window : {&g_overlay.fill, &g_overlay.outline}) {
    if (!IsWindow(*window)) *window = CreatePreviewWindow(DWMWCP_DONOTROUND);
    if (!*window) return false;
  }
  return true;
}

static void Present(const PresentRequest& request) {
  if (g_overlay.fadeTo == 255 && g_overlay.shown && SameRequest(*g_overlay.shown, request)) return;
  if (!EnsurePreviewWindows()) {
    HideOverlay();
    return;
  }
  g_overlay.style = request.style;

  DWORD accent = 0;
  BOOL opaqueBlend = FALSE;
  g_overlay.accent = SUCCEEDED(DwmGetColorizationColor(&accent, &opaqueBlend))
      ? RGB((accent >> 16) & 0xff, (accent >> 8) & 0xff, accent & 0xff)
      : GetSysColor(COLOR_HIGHLIGHT);
  g_overlay.colorKey = g_overlay.accent == RGB(1, 0, 1) ? RGB(0, 1, 0) : RGB(1, 0, 1);
  // Reveal a changed target at half strength once the current fade has finished.
  const PreviewStyle& style = g_overlay.style;
  const bool changedTarget = g_overlay.fadeTo == 255 && g_overlay.fadeAlpha == 255 && style.fadeMs > 0;
  if (!ApplyAlpha(changedTarget ? 128 : g_overlay.fadeAlpha)) {
    HideOverlay();
    return;
  }

  const Commands::TiledDropPreview& preview = request.preview;
  auto position = [](HDWP batch, HWND window, const RECT& frame, LONG opacity) {
    const UINT visibility = opacity > 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    return batch ? DeferWindowPos(batch, window, HWND_TOPMOST, frame.left, frame.top, Layout::RectWidth(frame),
                                  Layout::RectHeight(frame), visibility | SWP_NOACTIVATE | SWP_NOOWNERZORDER)
                 : nullptr;
  };
  HDWP batch = position(BeginDeferWindowPos(2), g_overlay.fill, preview.dragged, style.fillOpacity);
  batch = position(batch, g_overlay.outline, preview.affected, style.outlineOpacity);
  if (!batch || !EndDeferWindowPos(batch)) {
    HideOverlay();
    return;
  }
  InvalidateRect(g_overlay.fill, nullptr, FALSE);
  InvalidateRect(g_overlay.outline, nullptr, FALSE);
  ShowBadges(request);
  g_overlay.shown = request;
  FadeTo(255, changedTarget);
}

// A theme change recolours the badges, keeping the fade.
static void RepaintBadges(COLORREF background) {
  g_overlay.style.badgeBackground = background;
  for (const IconBadge* badge : {&g_overlay.draggedBadge, &g_overlay.affectedBadge}) {
    if (badge->window && IsWindowVisible(badge->window)) InvalidateRect(badge->window, nullptr, FALSE);
  }
}

static void DestroyOverlay() {
  for (HWND window : {g_overlay.fill, g_overlay.outline, g_overlay.draggedBadge.window,
                      g_overlay.affectedBadge.window}) {
    if (window) DestroyWindow(window);
  }
  if (g_overlay.windowClass) UnregisterClassW(kPreviewClassName, GetModuleHandleW(nullptr));
  g_overlay = {};
}

// --- The preview thread ------------------------------------------------------------

enum PreviewMessage : UINT {
  kPresentMessage = WM_APP + 1,  // lParam: PresentRequest*, owned by the receiver.
  kFadeOutMessage,
  kBadgeThemeMessage,            // wParam: the new badge background.
  kNewDragMessage,               // Forget the last drag's icons.
};

struct PreviewThread {
  HANDLE handle = nullptr;
  HANDLE stopEvent = nullptr;
  std::atomic<DWORD> id{0};
};
static PreviewThread g_previewThread;

struct PreviewThreadStartup {
  HANDLE ready;
  HANDLE stop;
};

// True for the preview thread, whose windows MultiWM must ignore like its own.
static bool IsPreviewThread(DWORD threadId) {
  const DWORD id = g_previewThread.id.load(std::memory_order_acquire);
  return id && id == threadId;
}

static void HandlePreviewMessage(const MSG& msg) {
  switch (msg.message) {
    case kPresentMessage: {
      std::unique_ptr<PresentRequest> request(reinterpret_cast<PresentRequest*>(msg.lParam));
      if (request) Present(*request);
      return;
    }
    case kFadeOutMessage:
      FadeTo(0);
      return;
    case kBadgeThemeMessage:
      RepaintBadges(static_cast<COLORREF>(msg.wParam));
      return;
    case kNewDragMessage:
      g_overlay.icons.clear();
      return;
    default:
      DispatchMessageW(&msg);
  }
}

// Waits for messages while idle; while a fade runs, steps the windows' alpha
// once per composition, so every frame DWM shows has its own opacity.
static DWORD WINAPI PreviewThreadProc(LPVOID parameter) {
  const auto startup = *static_cast<const PreviewThreadStartup*>(parameter);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  MSG msg{};
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // Create the queue before anyone posts.
  g_overlay.paced = true;
  SetEvent(startup.ready);
  while (WaitForSingleObject(startup.stop, 0) == WAIT_TIMEOUT) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) break;
      HandlePreviewMessage(msg);
      continue;
    }
    if (g_overlay.fading) {
      StepFade(PreviewClockMs());
      if (g_overlay.fading && FAILED(DwmFlush())) WaitForSingleObject(startup.stop, kFadeIntervalMs);
    } else if (MsgWaitForMultipleObjectsEx(1, &startup.stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) !=
               WAIT_OBJECT_0 + 1) {
      break;
    }
  }
  // Discard pending present requests without drawing them during shutdown.
  while (PeekMessageW(&msg, reinterpret_cast<HWND>(-1), kPresentMessage, kPresentMessage, PM_REMOVE)) {
    if (msg.message == kPresentMessage) delete reinterpret_cast<PresentRequest*>(msg.lParam);
  }
  DestroyOverlay();
  return 0;
}

// Starts the preview thread once. False when it can't start; the preview then
// runs on the WM thread.
static bool EnsurePreviewThread() {
  if (g_previewThread.handle) return true;
  // A synchronous fallback already owns windows on the calling thread.
  if (g_overlay.windowClass) return false;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready) return false;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop) {
    CloseHandle(ready);
    return false;
  }
  PreviewThreadStartup startup{ready, stop};
  DWORD id = 0;
  HANDLE thread = CreateThread(nullptr, 0, PreviewThreadProc, &startup, 0, &id);
  const HANDLE waits[] = {ready, thread};
  const bool started = thread && WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE) == WAIT_OBJECT_0;
  if (thread && !started) {
    SetEvent(stop);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
  CloseHandle(ready);
  if (!started) {
    CloseHandle(stop);
    Wh_Log(L"Drag preview thread did not start; previews run on the WM thread");
    return false;
  }
  SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
  g_previewThread.handle = thread;
  g_previewThread.stopEvent = stop;
  g_previewThread.id.store(id, std::memory_order_release);
  return true;
}

// A failed post leaves the preview thread's ownership in place.
static bool PostToPreviewThread(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
  const DWORD id = g_previewThread.id.load(std::memory_order_acquire);
  return id && PostThreadMessageW(id, message, wparam, lparam);
}

// --- WM thread API -----------------------------------------------------------------

static void Show(const Commands::TiledDropPreview& preview, bool badges = false, UINT dpi = 96) {
  PresentRequest request{preview, CurrentStyle(), dpi, badges};
  if (!g_previewThread.handle) {
    Present(request);
    return;
  }
  auto* posted = new PresentRequest(request);
  if (!PostToPreviewThread(kPresentMessage, 0, reinterpret_cast<LPARAM>(posted))) delete posted;
}

static void Hide() {
  if (g_previewThread.handle) PostToPreviewThread(kFadeOutMessage);
  else FadeTo(0);
}

static void RefreshBadgeTheme() {
  const COLORREF background = TrayUi::FlyoutBackgroundColor();
  if (g_previewThread.handle) PostToPreviewThread(kBadgeThemeMessage, background);
  else RepaintBadges(background);
}

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
  if (EnsurePreviewThread()) PostToPreviewThread(kNewDragMessage);
  else g_overlay.icons.clear();
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
        Show(*preview, true, metrics.dpi);
        return;
      }
    }
    // A failed or empty drop returns a movable window to its tile. A ghost
    // window cannot be moved back by MultiWM.
    const WindowRecord* source = drag.workspace.Find(drag.source);
    if (drag.displayedWindow == drag.source && source && source->canMove && !IsRectEmpty(&drag.start)) {
      Show({drag.start, drag.start, drag.source}, true, metrics.dpi);
      return;
    }
  }
  Hide();
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
    Hide();
    return;
  }

  UpdateDropPreview(drag, cursor);
}

static void Shutdown() {
  g_drag.reset();
  if (HANDLE thread = g_previewThread.handle) {
    SetEvent(g_previewThread.stopEvent);
    // WM shutdown already keeps the session alive if this join takes too long.
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
      Wh_Log(L"Could not join drag preview thread: %lu", GetLastError());
      return;
    }
    CloseHandle(thread);
    CloseHandle(g_previewThread.stopEvent);
    g_previewThread.handle = nullptr;
    g_previewThread.stopEvent = nullptr;
    g_previewThread.id.store(0, std::memory_order_release);
    return;
  }
  DestroyOverlay();
}

}  // namespace DragPreview
