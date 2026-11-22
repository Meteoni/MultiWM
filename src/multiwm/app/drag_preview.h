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
// The preview is drawn with DirectComposition when it is available
// (preview_composition.h): one host window holds the fill, outline and badges,
// drawn on the GPU, and DWM runs the fades itself at the display's refresh
// rate. The preview thread starts with the WM session and attaches to the GPU
// then (about 120 ms, measured), so the first drag doesn't wait for it.
// Otherwise, and for the rest of the session after a composition failure, each
// part is a layered GDI window, faded once per composition (DwmFlush).
//
// If the thread can't start (and in tests that don't start it), the same code
// runs on the caller's thread, with a window timer for GDI fades.

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

// False keeps the preview on GDI windows (tests that inspect them).
static std::atomic<bool> g_allowComposition{true};

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
  // GDI: one layered window per part.
  HWND fill = nullptr;
  HWND outline = nullptr;
  IconBadge draggedBadge;
  IconBadge affectedBadge;
  // DirectComposition: the host window and its presenter, while it works.
  HWND host = nullptr;
  std::unique_ptr<PreviewComposition::Presenter> composition;
  bool compositionFailed = false;

  ATOM windowClass = 0;
  COLORREF accent = 0;
  COLORREF colorKey = RGB(1, 0, 1);
  PreviewStyle style;
  BYTE fadeAlpha = 0;
  BYTE fadeFrom = 0;
  BYTE fadeTo = 0;
  ULONGLONG fadeStarted = 0;
  UINT fadeMs = 0;      // The running fade's duration, fixed when it starts.
  bool fading = false;  // A GDI fade is being stepped.
  // The preview thread steps GDI fades itself, once per composition.
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

static bool Composed() { return g_overlay.composition && g_overlay.composition->Attached(); }

static bool SameRequest(const PresentRequest& a, const PresentRequest& b) {
  return a.preview.draggedWindow == b.preview.draggedWindow && a.preview.affectedWindow == b.preview.affectedWindow &&
         EqualRect(&a.preview.dragged, &b.preview.dragged) && EqualRect(&a.preview.affected, &b.preview.affected);
}

// --- Fades -------------------------------------------------------------------------

static bool ApplyAlpha(BYTE fadeAlpha) {
  g_overlay.fadeAlpha = fadeAlpha;
  const BYTE fillAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_overlay.style.fillOpacity, 100));
  const BYTE outlineAlpha = static_cast<BYTE>(MulDiv(fadeAlpha, g_overlay.style.outlineOpacity, 100));
  const BOOL fillSet = SetLayeredWindowAttributes(g_overlay.fill, 0, fillAlpha, LWA_ALPHA);
  const BOOL outlineSet = SetLayeredWindowAttributes(
      g_overlay.outline, g_overlay.colorKey, outlineAlpha, LWA_COLORKEY | LWA_ALPHA);
  for (const IconBadge* badge : {&g_overlay.draggedBadge, &g_overlay.affectedBadge}) {
    if (badge->icon) SetLayeredWindowAttributes(badge->window, 0, fadeAlpha, LWA_ALPHA);
  }
  return fillSet && outlineSet;
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

// Composition: DWM runs the fade, and this only finishes it, from the timer
// set for its end: a completed fade-out hides the host and frees its surfaces.
static void FinishComposedFade(ULONGLONG now) {
  const ULONGLONG end = g_overlay.fadeStarted + g_overlay.fadeMs;
  if (now < end) {
    SetTimer(g_overlay.host, kFadeTimer, static_cast<UINT>(end - now), nullptr);
    return;
  }
  KillTimer(g_overlay.host, kFadeTimer);
  g_overlay.fadeAlpha = g_overlay.fadeTo;
  if (g_overlay.fadeTo == 0) g_overlay.composition->Hide();
}

static void StepFade(ULONGLONG now) {
  if (Composed()) {
    FinishComposedFade(now);
    return;
  }
  const BYTE alpha = AlphaAt(now);
  if (alpha != g_overlay.fadeAlpha) ApplyAlpha(alpha);
  if (now - g_overlay.fadeStarted < g_overlay.fadeMs) return;

  g_overlay.fading = false;
  KillTimer(g_overlay.fill, kFadeTimer);
  if (alpha == 0) {
    ShowWindow(g_overlay.fill, SW_HIDE);
    ShowWindow(g_overlay.outline, SW_HIDE);
    g_overlay.draggedBadge.Hide();
    g_overlay.affectedBadge.Hide();
  }
}

static void DisableComposition(const wchar_t* why) {
  Wh_Log(L"Drag preview: %ls; drawing with GDI for the rest of the session", why);
  if (g_overlay.host) {
    KillTimer(g_overlay.host, kFadeTimer);
    ShowWindow(g_overlay.host, SW_HIDE);
  }
  g_overlay.composition.reset();
  g_overlay.compositionFailed = true;
}

// Composition: fades to `alpha` from `startAt` (else the current opacity),
// presenting `scene` in the same frame when given. Without `restart`, a fade
// already heading to `alpha` keeps its timing. False when composition failed
// and was turned off.
static bool ComposedFadeTo(BYTE alpha, bool restart, const PreviewComposition::Scene* scene,
                           std::optional<BYTE> startAt = std::nullopt) {
  // Called on every mouse move while nothing is targeted: only a change commits.
  if (!scene && !restart && g_overlay.fadeTo == alpha) return true;
  const ULONGLONG now = PreviewClockMs();
  const BYTE current = startAt.value_or(AlphaAt(now));
  if (restart || g_overlay.fadeTo != alpha) {
    g_overlay.fadeFrom = current;
    g_overlay.fadeTo = alpha;
    g_overlay.fadeStarted = now;
    g_overlay.fadeMs = g_overlay.style.fadeMs;
  }
  const ULONGLONG end = g_overlay.fadeStarted + g_overlay.fadeMs;
  const UINT remaining = now < end ? static_cast<UINT>(end - now) : 0;
  g_overlay.fadeAlpha = current;
  auto& presenter = *g_overlay.composition;
  const bool shown = scene ? presenter.Present(*scene, current / 255.0f, alpha / 255.0f, remaining)
                           : presenter.Fade(current / 255.0f, alpha / 255.0f, remaining);
  if (!shown) {
    DisableComposition(L"DirectComposition failed (device lost?)");
    return false;
  }
  FinishComposedFade(remaining ? now : end);
  return true;
}

// GDI: unpaced, a window timer runs only during the transition.
static void FadeTo(BYTE alpha, bool restart = false) {
  if (Composed()) {
    ComposedFadeTo(alpha, restart, nullptr);
    return;
  }
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

static void PaintOutline(HWND hwnd, HDC dc, const PAINTSTRUCT& paint, HBRUSH accent) {
  HBRUSH transparent = CreateSolidBrush(g_overlay.colorKey);
  if (!transparent) return;
  FillRect(dc, &paint.rcPaint, transparent);
  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  const UINT dpi = GetDpiForWindow(hwnd);
  const int width = std::max(1, MulDiv(g_overlay.style.outlineWidthDip, dpi, 96));
  const int radius = std::min<LONG>(MulDiv(g_overlay.style.outlineRadiusDip, dpi, 96),
                                   std::min(Layout::RectWidth(bounds), Layout::RectHeight(bounds)) / 2);
  PaintRoundedRect(dc, bounds, radius, accent);
  InflateRect(&bounds, -width, -width);
  PaintRoundedRect(dc, bounds, radius - width, transparent);
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
  if (message == WM_TIMER && (hwnd == g_overlay.fill || hwnd == g_overlay.host) && wparam == kFadeTimer) {
    StepFade(PreviewClockMs());
    return 0;
  }
  if (message != WM_PAINT || hwnd == g_overlay.host) return DefWindowProcW(hwnd, message, wparam, lparam);

  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (hwnd == g_overlay.draggedBadge.window || hwnd == g_overlay.affectedBadge.window) {
    PaintBadge(dc, paint, hwnd == g_overlay.draggedBadge.window ? g_overlay.draggedBadge : g_overlay.affectedBadge);
  } else if (HBRUSH accent = CreateSolidBrush(g_overlay.accent)) {
    if (hwnd == g_overlay.outline) {
      PaintOutline(hwnd, dc, paint, accent);
    } else {
      FillRect(dc, &paint.rcPaint, accent);
    }
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

// Where `window`'s badge goes on `tile`, with its icon and sizes. Nothing when
// badges are off, the tile is too small, or the window has no icon.
struct BadgePlan {
  RECT frame;
  PreviewIcon icon;
  LONG iconSize;
  LONG badgeSize;
};

static std::optional<BadgePlan> PlanBadge(HWND window, const RECT& tile, UINT dpi) {
  const LONG iconSize = SharedGeometry::ScaleDip(g_overlay.style.iconSizeDip, dpi);
  const LONG badgeSize = iconSize + SharedGeometry::ScaleDip(8, dpi);
  if (!window || iconSize <= 0 || badgeSize > Layout::RectWidth(tile) || badgeSize > Layout::RectHeight(tile)) {
    return std::nullopt;
  }
  auto [entry, firstVisit] = g_overlay.icons.try_emplace(window);
  if (firstVisit) entry->second = LoadBadgeIcon(window, dpi, iconSize);
  if (!entry->second) return std::nullopt;
  const POINT center = SharedGeometry::RectCenter(tile);
  const LONG left = center.x - badgeSize / 2;
  const LONG top = center.y - badgeSize / 2;
  return BadgePlan{{left, top, left + badgeSize, top + badgeSize}, entry->second, iconSize, badgeSize};
}

// --- Windows -----------------------------------------------------------------------

constexpr DWORD kGdiPreviewExStyle =
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;

static bool EnsureWindowClass() {
  if (g_overlay.windowClass) return true;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = PreviewWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kPreviewClassName;
  g_overlay.windowClass = RegisterClassW(&windowClass);
  return g_overlay.windowClass != 0;
}

static HWND CreatePreviewWindow(DWORD exStyle) {
  return CreateWindowExW(exStyle, kPreviewClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}

// Attaches DirectComposition to its host window once. False when it isn't
// allowed or available; the preview then uses GDI windows.
static bool EnsureComposition() {
  if (Composed()) return true;
  if (!g_allowComposition.load() || g_overlay.compositionFailed || !EnsureWindowClass()) return false;
  if (!g_overlay.host) g_overlay.host = CreatePreviewWindow(PreviewComposition::kHostExStyle);
  auto presenter = std::make_unique<PreviewComposition::Presenter>();
  if (!g_overlay.host || !presenter->Attach(g_overlay.host)) {
    DisableComposition(L"DirectComposition is unavailable");
    return false;
  }
  g_overlay.composition = std::move(presenter);
  return true;
}

// --- Composed preview --------------------------------------------------------------

// The host window's frame for `preview`: the monitor's tiling area, which
// holds every tile and never covers the whole monitor (so nothing reads the
// host as a fullscreen window), stretched to any preview frame outside it.
static RECT CompositionHostFrame(const Commands::TiledDropPreview& preview) {
  RECT area{};
  UnionRect(&area, &preview.dragged, &preview.affected);
  MONITORINFO monitor{sizeof(monitor)};
  if (GetMonitorInfoW(MonitorFromRect(&preview.dragged, MONITOR_DEFAULTTONEAREST), &monitor)) {
    const RECT tiling = SharedGeometry::KeepTilesOffWholeMonitor(monitor.rcWork, monitor.rcMonitor);
    UnionRect(&area, &area, &tiling);
  }
  return area;
}

static std::optional<PreviewComposition::BadgeScene> ComposedBadge(HWND window, const RECT& tile, UINT dpi) {
  const auto plan = PlanBadge(window, tile, dpi);
  if (!plan) return std::nullopt;
  // The GDI badge uses DWM's rounded corners, 8 px at 96 DPI.
  return PreviewComposition::BadgeScene{plan->frame, plan->icon.get(), plan->iconSize,
                                        SharedGeometry::ScaleDip(8, dpi), g_overlay.style.badgeBackground};
}

static PreviewComposition::Scene ComposedScene(const PresentRequest& request) {
  const Commands::TiledDropPreview& preview = request.preview;
  const PreviewStyle& style = g_overlay.style;
  const UINT dpi = request.dpi;
  PreviewComposition::Scene scene;
  scene.host = CompositionHostFrame(preview);
  scene.accent = g_overlay.accent;
  scene.fill = preview.dragged;
  scene.fillOpacity = style.fillOpacity / 100.0f;
  // The GDI fill uses DWM's rounded corners, 8 px at 96 DPI.
  scene.fillRadius = SharedGeometry::ScaleDip(8, dpi);
  scene.outline = preview.affected;
  scene.outlineOpacity = style.outlineOpacity / 100.0f;
  scene.outlineWidth = std::max(1, MulDiv(style.outlineWidthDip, dpi, 96));
  scene.outlineRadius = std::min<LONG>(MulDiv(style.outlineRadiusDip, dpi, 96),
                                      std::min(Layout::RectWidth(preview.affected),
                                               Layout::RectHeight(preview.affected)) / 2);
  if (!request.badges) return scene;
  const bool sameTile = EqualRect(&preview.dragged, &preview.affected);
  if (style.fillOpacity > 0 || sameTile) scene.badges[0] = ComposedBadge(preview.draggedWindow, preview.dragged, dpi);
  if (!sameTile && style.outlineOpacity > 0) {
    scene.badges[1] = ComposedBadge(preview.affectedWindow, preview.affected, dpi);
  }
  return scene;
}

// --- GDI preview -------------------------------------------------------------------

static void ShowBadge(IconBadge& badge, HWND window, const RECT& tile, UINT dpi) {
  const auto plan = PlanBadge(window, tile, dpi);
  if (!plan) {
    badge.Hide();
    return;
  }
  if (!badge.window) {
    badge.window = CreatePreviewWindow(kGdiPreviewExStyle);
    if (!badge.window) return;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(badge.window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  }
  badge.badgeSize = plan->badgeSize;
  badge.iconSize = plan->iconSize;
  // Retain our own copy while a badge is fading out.
  badge.icon = plan->icon;
  if (!SetLayeredWindowAttributes(badge.window, 0, g_overlay.fadeAlpha, LWA_ALPHA) ||
      !SetWindowPos(badge.window, HWND_TOPMOST, plan->frame.left, plan->frame.top, plan->badgeSize, plan->badgeSize,
                    SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
    badge.Hide();
    return;
  }
  InvalidateRect(badge.window, nullptr, FALSE);
}

static bool EnsureGdiWindows() {
  if (!EnsureWindowClass()) return false;
  for (HWND* window : {&g_overlay.fill, &g_overlay.outline}) {
    if (*window) continue;
    *window = CreatePreviewWindow(kGdiPreviewExStyle);
    if (!*window) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corners = window == &g_overlay.fill ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(*window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  }
  return true;
}

static void ShowWithGdi(const PresentRequest& request, bool changedTarget) {
  const Commands::TiledDropPreview& preview = request.preview;
  const PreviewStyle& style = g_overlay.style;
  if (!EnsureGdiWindows()) return;
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
  HDWP batch = position(BeginDeferWindowPos(2), g_overlay.fill, preview.dragged, style.fillOpacity);
  batch = position(batch, g_overlay.outline, preview.affected, style.outlineOpacity);
  if (!batch || !EndDeferWindowPos(batch)) {
    FadeTo(0);
    return;
  }
  InvalidateRect(g_overlay.fill, nullptr, FALSE);
  InvalidateRect(g_overlay.outline, nullptr, FALSE);

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
  g_overlay.shown = request;
  FadeTo(255, changedTarget);
}

// --- Presentation (the presenting thread) ------------------------------------------

static void Present(const PresentRequest& request) {
  if (g_overlay.fadeTo == 255 && g_overlay.shown && SameRequest(*g_overlay.shown, request)) return;
  g_overlay.style = request.style;

  DWORD accent = 0;
  BOOL opaqueBlend = FALSE;
  g_overlay.accent = SUCCEEDED(DwmGetColorizationColor(&accent, &opaqueBlend))
      ? RGB((accent >> 16) & 0xff, (accent >> 8) & 0xff, accent & 0xff)
      : GetSysColor(COLOR_HIGHLIGHT);
  g_overlay.colorKey = g_overlay.accent == RGB(1, 0, 1) ? RGB(0, 1, 0) : RGB(1, 0, 1);
  // Reveal the new destination immediately at half strength. If the hint
  // is already fading in, let that fade finish without a new opacity drop.
  // DWM runs a composed fade, so its opacity is read from the clock.
  const BYTE shownAlpha = Composed() ? AlphaAt(PreviewClockMs()) : g_overlay.fadeAlpha;
  const bool changedTarget = g_overlay.fadeTo == 255 && shownAlpha == 255 && g_overlay.style.fadeMs > 0;
  if (EnsureComposition()) {
    const auto scene = ComposedScene(request);
    if (ComposedFadeTo(255, changedTarget, &scene, changedTarget ? std::optional<BYTE>(128) : std::nullopt)) {
      g_overlay.shown = request;
      return;
    }
    // Composition just failed and was turned off: GDI from a fresh start.
    g_overlay.fadeAlpha = 0;
    g_overlay.fadeTo = 0;
  }
  ShowWithGdi(request, changedTarget);
}

// A theme change recolours the badges, keeping the fade.
static void RepaintBadges(COLORREF background) {
  g_overlay.style.badgeBackground = background;
  if (Composed()) {
    if (!g_overlay.shown || g_overlay.fadeTo == 0) return;
    g_overlay.shown->style.badgeBackground = background;
    const auto scene = ComposedScene(*g_overlay.shown);
    ComposedFadeTo(g_overlay.fadeTo, false, &scene);
    return;
  }
  for (const IconBadge* badge : {&g_overlay.draggedBadge, &g_overlay.affectedBadge}) {
    if (badge->window && IsWindowVisible(badge->window)) InvalidateRect(badge->window, nullptr, FALSE);
  }
}

static void DestroyOverlay() {
  g_overlay.composition.reset();
  for (HWND window : {g_overlay.fill, g_overlay.outline, g_overlay.draggedBadge.window,
                      g_overlay.affectedBadge.window, g_overlay.host}) {
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
  kWarmUpMessage,                // Attach to the GPU before the first drag.
};

struct PreviewThread {
  HANDLE handle = nullptr;
  std::atomic<DWORD> id{0};
};
static PreviewThread g_previewThread;

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
    case kWarmUpMessage:
      EnsureComposition();
      return;
    default:
      DispatchMessageW(&msg);
  }
}

// Handles every queued message. False once the thread should exit.
static bool DrainPreviewMessages() {
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) return false;
    HandlePreviewMessage(msg);
  }
  return true;
}

// Waits for messages while idle; while a GDI fade runs, steps it once per
// composition so every frame DWM shows has its own opacity. Composed fades
// need nothing per frame: DWM runs them.
static DWORD WINAPI PreviewThreadProc(LPVOID ready) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));  // WIC, for badge icons.
  MSG msg{};
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // Create the queue before anyone posts.
  g_overlay.paced = true;
  SetEvent(static_cast<HANDLE>(ready));
  for (;;) {
    if (!g_overlay.fading) {
      const BOOL received = GetMessageW(&msg, nullptr, 0, 0);
      if (received <= 0) break;
      HandlePreviewMessage(msg);
      continue;
    }
    if (!DrainPreviewMessages()) break;
    if (!g_overlay.fading) continue;
    StepFade(PreviewClockMs());
    if (g_overlay.fading && FAILED(DwmFlush())) Sleep(kFadeIntervalMs);
  }
  DestroyOverlay();
  if (com) CoUninitialize();
  return 0;
}

// Starts the preview thread once. False when it can't start; the preview then
// runs on the WM thread.
static bool EnsurePreviewThread() {
  if (g_previewThread.handle) return true;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready) return false;
  DWORD id = 0;
  HANDLE thread = CreateThread(nullptr, 0, PreviewThreadProc, ready, 0, &id);
  const bool started = thread && WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0;
  CloseHandle(ready);
  if (!started) {
    if (thread) {
      PostThreadMessageW(id, WM_QUIT, 0, 0);
      WaitForSingleObject(thread, 2000);
      CloseHandle(thread);
    }
    Wh_Log(L"Drag preview thread did not start; previews run on the WM thread");
    return false;
  }
  SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
  g_previewThread.handle = thread;
  g_previewThread.id.store(id, std::memory_order_release);
  return true;
}

// Sends a message to the preview thread. False when there is no thread, so the
// caller presents on its own thread instead.
static bool PostToPreviewThread(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
  const DWORD id = g_previewThread.id.load(std::memory_order_acquire);
  return id && PostThreadMessageW(id, message, wparam, lparam);
}

// --- WM thread API -----------------------------------------------------------------

// At the start of a WM session: start the preview thread and attach to the
// GPU there, so the first drag doesn't wait. Nothing while previews are off.
static void Start() {
  if (g_settings.DragPreviewEnabled() && EnsurePreviewThread()) PostToPreviewThread(kWarmUpMessage);
}

static void Show(const Commands::TiledDropPreview& preview, bool badges = false, UINT dpi = 96) {
  PresentRequest request{preview, CurrentStyle(), dpi, badges};
  if (!g_previewThread.id.load(std::memory_order_acquire)) {
    Present(request);
    return;
  }
  auto* posted = new PresentRequest(request);
  if (!PostToPreviewThread(kPresentMessage, 0, reinterpret_cast<LPARAM>(posted))) delete posted;
}

static void Hide() {
  if (!PostToPreviewThread(kFadeOutMessage)) FadeTo(0);
}

static void RefreshBadgeTheme() {
  const COLORREF background = TrayUi::FlyoutBackgroundColor();
  if (!PostToPreviewThread(kBadgeThemeMessage, background)) RepaintBadges(background);
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
  // Previews turned on mid-session start the thread with the first drag.
  if (!EnsurePreviewThread() || !PostToPreviewThread(kNewDragMessage)) g_overlay.icons.clear();
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
    PostThreadMessageW(g_previewThread.id.load(std::memory_order_acquire), WM_QUIT, 0, 0);
    // The thread only ever waits on DWM, the GPU or an 8 ms icon request.
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) Wh_Log(L"Drag preview thread did not exit");
    CloseHandle(thread);
    g_previewThread.handle = nullptr;
    g_previewThread.id.store(0, std::memory_order_release);
    return;
  }
  DestroyOverlay();
}

}  // namespace DragPreview
