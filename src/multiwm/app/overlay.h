// MultiWM's on-screen overlays: the drag preview's shapes and badges, and toasts.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// Everything here belongs to the thread that presents: a dedicated overlay
// thread once started, else the calling thread, which then uses window timers
// during fades. Callers on the WM thread use the API at the end: DragPreview
// says what preview to show, TrayUi which toasts.
//
// One retained Direct2D surface per monitor, composed by DirectComposition,
// draws the fill, outline, badges and toasts. GDI layered windows measured far
// slower for these overlays; the retained surface follows PowerToys
// FancyZones. The overlay thread steps opacity at the compositor's refresh
// cadence and can wake early for a new target. Thread-pool workers load app
// icons, for the preview's badges and toasts, so drawing never waits on an app.

#include <mutex>

namespace Overlay {

// A toast or badge card's colors, from the system theme (TrayUi reads it).
struct PopupAppearance {
  COLORREF background = 0;
  bool light = false;
};

constexpr DWORD kToastFadeMs = 180;
constexpr UINT kToastFadeIntervalMs = 16;
// A toast's app icons: at most this many, each the small-icon size (16 DIP:
// 16, 20, 24 or 32 pixels at the common scales, sizes apps ship) beside 14 DIP
// text, as in Windows' own lists.
constexpr size_t kMaxToastIcons = 3;
constexpr LONG kToastIconDip = 16;

// A window whose icon is wanted, identified so a reused handle isn't mistaken
// for it, and the DPI and size in pixels to load the icon at.
struct BadgeIconKey {
  HWND window = nullptr;
  DWORD processId = 0;
  DWORD threadId = 0;
  UINT dpi = 0;
  LONG size = 0;
  bool operator==(const BadgeIconKey&) const = default;

  bool IsLive() const {
    DWORD liveProcessId = 0;
    return GetWindowThreadProcessId(window, &liveProcessId) == threadId && liveProcessId == processId;
  }
};

// One status toast, centred on `monitor`'s work area unless offset.
struct ToastRequest {
  HMONITOR monitor = nullptr;
  std::wstring text;
  PopupAppearance appearance;
  int xOffsetDip = 0;
  int yOffsetDip = 0;
  bool atTop = false;
  DWORD holdMs = 0;  // Fully shown this long, then it fades over kToastFadeMs.
  // The apps the text names, one window each, for their icons before the text
  // (at most kMaxToastIcons are shown). ShowToast sets the DPI and size.
  std::vector<BadgeIconKey> apps;
};

// Text on a card of the light or dark theme.
static COLORREF CardTextColor(bool light) {
  return light ? RGB(24, 24, 24) : RGB(255, 255, 255);
}
constexpr wchar_t kOverlayClassName[] = L"WindhawkMultiWMOverlay";

using PreviewIcon = std::shared_ptr<std::remove_pointer_t<HICON>>;

constexpr UINT_PTR kFadeTimer = 1;
constexpr UINT_PTR kToastTimer = 2;
// Releases an idle host's surface: a work-area-sized BGRA surface (about 32 MB
// of GPU memory at 4K) isn't kept for the whole session. The device and window
// stay, so the next show re-attaches in a few milliseconds (measured 4-7 ms more
// than a warm toast at 3066x1914), not the ~170 ms of a cold start.
constexpr UINT_PTR kSurfaceReleaseTimer = 3;
constexpr UINT kIdleSurfaceReleaseMs = 30'000;
constexpr UINT kFadeIntervalMs = 16;
constexpr UINT kBadgeIconReadyMessage = WM_APP + 32;
constexpr UINT kToastIconsReadyMessage = WM_APP + 33;  // wParam: ToastIconJob::id.

// The settings a preview is drawn with, read on the WM thread.
struct PreviewStyle {
  LONG fillOpacity = 0;     // Percent.
  LONG outlineOpacity = 0;  // Percent.
  LONG outlineWidthDip = 0;
  LONG outlineRadiusDip = 0;
  LONG iconSizeDip = 0;
  UINT fadeMs = 0;
  PopupAppearance badgeAppearance;
};

// One change to the shown preview, from the WM thread.
struct PresentRequest {
  Commands::TiledDropPreview preview;
  PreviewStyle style;
  UINT dpi = 96;
  bool badges = false;  // Show app icons (a real drag, not a bare preview).
  GUID desktopId{};
};

struct IconBadge {
  PreviewIcon icon;
  LONG iconSize = 0;
  LONG badgeSize = 0;
  RECT frame{};

  void Hide() { icon.reset(); }
};

struct CachedBadgeIcon {
  BadgeIconKey key;
  PreviewIcon icon;
  ULONGLONG loadedAt = 0;
  uint64_t lastDrag = 0;
  bool retry = true;
};

struct BadgeIconLookup {
  // The presenter publishes the request, the worker publishes its result.
  enum class State { Idle, Queued, Ready };
  PTP_WORK work = nullptr;
  std::atomic<State> state{State::Idle};
  BadgeIconKey key;
  GUID desktopId{};
  HWND notifyWindow = nullptr;
  PreviewIcon icon;
  bool queryFailed = false;

  ~BadgeIconLookup() {
    if (work) {
      WaitForThreadpoolWorkCallbacks(work, TRUE);
      CloseThreadpoolWork(work);
    }
  }
};

// A toast waiting for its app icons. Asking an app for its icon can wait on the
// app, so a thread-pool worker loads them, sets `done` and posts
// kToastIconsReadyMessage to `notifyWindow`; the presenter then shows the toast
// (ShowToastWithIcons). Owned by the presenter; destroying a job waits for its
// worker.
struct ToastIconJob {
  PTP_WORK work = nullptr;
  uint64_t id = 0;
  HWND notifyWindow = nullptr;
  ToastRequest request;
  std::vector<PreviewIcon> icons;  // The worker's result, one per request.apps entry.
  std::atomic<bool> posted{false};  // The worker's message went out (before `done`).
  std::atomic<bool> done{false};

  ~ToastIconJob() {
    if (work) {
      WaitForThreadpoolWorkCallbacks(work, TRUE);
      CloseThreadpoolWork(work);
    }
  }
};

struct MonitorHost {
  HWND window = nullptr;
  RECT workArea{};
  bool visible = false;
  // Raise above other topmost windows at the next draw: a new toast or preview
  // must not appear behind one that went on top while this host was showing
  // something else (a toast, a fading preview).
  bool raise = false;
  PopupSurface::Surface surface;
  struct BadgeBitmap {
    PreviewIcon icon;
    PopupSurface::ComPtr<ID2D1Bitmap> bitmap;
  };
  BadgeBitmap badges[2];
  std::optional<ToastRequest> toast;
  PopupSurface::ComPtr<IDWriteTextLayout> toastText;
  D2D1_RECT_F toastFrame{};
  D2D1_POINT_2F toastTextOrigin{};
  struct ToastIcon {
    PopupSurface::ComPtr<ID2D1Bitmap> bitmap;
    D2D1_RECT_F rect{};
  };
  std::vector<ToastIcon> toastIcons;
  UINT toastDpi = 96;
  ULONGLONG toastStarted = 0;
  float toastAlpha = 1.0f;
};

struct Presenter {
  IconBadge draggedBadge;
  IconBadge affectedBadge;
  ATOM windowClass = 0;
  COLORREF accent = 0;
  PreviewStyle style;
  BYTE fadeAlpha = 0;
  BYTE fadeFrom = 0;
  BYTE fadeTo = 0;
  ULONGLONG fadeStarted = 0;
  UINT fadeMs = 0;      // The running fade's duration, fixed when it starts.
  bool fading = false;  // A fade is being stepped.
  // The overlay thread steps fades itself.
  bool paced = false;
  std::optional<PresentRequest> shown;
  // Retain owned icons across drags. A miss or provisional fallback gets at
  // most one lookup per drag.
  std::unordered_map<GUID, std::vector<CachedBadgeIcon>, Model::GuidHash, Model::GuidEqual> iconsByDesktop;
  uint64_t iconDrag = 1;
  std::unique_ptr<BadgeIconLookup> iconLookup;
  std::vector<std::unique_ptr<ToastIconJob>> toastIconJobs;
  // The latest toast for each monitor; a job for an older one shows nothing.
  std::unordered_map<HMONITOR, uint64_t> latestToasts;
  uint64_t lastToastId = 0;
  PopupSurface::Device device;
  std::unordered_map<HMONITOR, std::unique_ptr<MonitorHost>> hosts;
  MonitorHost* previewHost = nullptr;
};

// Owned by the thread that presents: the overlay thread once started, else the caller.
static Presenter g_presenter;
static bool DrawHost(MonitorHost& host);
static void ReleaseHostSurface(MonitorHost& host);
static void StepToast(MonitorHost& host);

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
  return a.dpi == b.dpi && a.badges == b.badges && a.style.iconSizeDip == b.style.iconSizeDip &&
         IsEqualGUID(a.desktopId, b.desktopId) &&
         a.preview.draggedWindow == b.preview.draggedWindow && a.preview.affectedWindow == b.preview.affectedWindow &&
         EqualRect(&a.preview.dragged, &b.preview.dragged) && EqualRect(&a.preview.affected, &b.preview.affected);
}

// --- Fades -------------------------------------------------------------------------

// The fade's opacity at `now`.
static BYTE AlphaAt(ULONGLONG now) {
  const UINT duration = g_presenter.fadeMs;
  const ULONGLONG elapsed = now - g_presenter.fadeStarted;
  return elapsed >= duration
      ? g_presenter.fadeTo
      : static_cast<BYTE>(g_presenter.fadeFrom + MulDiv(g_presenter.fadeTo - g_presenter.fadeFrom, static_cast<int>(elapsed),
                                                     duration));
}

static void ClearPreview() {
  g_presenter.fading = false;
  g_presenter.fadeAlpha = 0;
  g_presenter.fadeTo = 0;
  g_presenter.draggedBadge.Hide();
  g_presenter.affectedBadge.Hide();
  g_presenter.shown.reset();
  if (auto* host = std::exchange(g_presenter.previewHost, nullptr)) {
    if (!g_presenter.paced) KillTimer(host->window, kFadeTimer);
    DrawHost(*host);
  }
}

static void StepFade(ULONGLONG now) {
  if (!g_presenter.fading) return;
  const BYTE alpha = AlphaAt(now);
  if (alpha != g_presenter.fadeAlpha) {
    g_presenter.fadeAlpha = alpha;
    if (alpha && !DrawHost(*g_presenter.previewHost)) {
      ClearPreview();
      return;
    }
  }
  if (now - g_presenter.fadeStarted < g_presenter.fadeMs) return;
  if (alpha == 0) {
    ClearPreview();
    return;
  }
  g_presenter.fading = false;
  if (!g_presenter.paced) KillTimer(g_presenter.previewHost->window, kFadeTimer);
}

// Use the same surface with a window timer if the overlay thread cannot start.
static void FadeTo(BYTE alpha, bool restart = false) {
  if (!g_presenter.previewHost || (!restart && g_presenter.fadeTo == alpha)) return;
  g_presenter.fadeFrom = g_presenter.fadeAlpha;
  g_presenter.fadeTo = alpha;
  g_presenter.fadeStarted = PreviewClockMs();
  g_presenter.fadeMs = g_presenter.style.fadeMs;
  g_presenter.fading = true;
  if (g_presenter.fadeFrom == alpha || g_presenter.fadeMs == 0 ||
      (!g_presenter.paced && !SetTimer(g_presenter.previewHost->window, kFadeTimer, kFadeIntervalMs, nullptr))) {
    StepFade(g_presenter.fadeStarted + g_presenter.fadeMs);
  }
}

static void UpdateBadgesAfterIconLookup();
static void FinishToastIconJob(uint64_t id);

static LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    // Tell the Shell to exempt this overlay from fullscreen detection.
    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
  }
  if (message == WM_NCDESTROY) RemovePropW(hwnd, L"NonRudeHWND");
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_ERASEBKGND) return 1;
  if (message == kBadgeIconReadyMessage) {
    UpdateBadgesAfterIconLookup();
    return 0;
  }
  if (message == kToastIconsReadyMessage) {
    FinishToastIconJob(static_cast<uint64_t>(wparam));
    return 0;
  }
  auto* host = reinterpret_cast<MonitorHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (host && message == WM_TIMER && wparam == kToastTimer) {
    StepToast(*host);
    return 0;
  }
  if (host && message == WM_TIMER && wparam == kSurfaceReleaseTimer) {
    KillTimer(hwnd, kSurfaceReleaseTimer);
    if (!host->visible && host->surface.target) ReleaseHostSurface(*host);
    return 0;
  }
  if (host && message == WM_PAINT) {
    ValidateRect(hwnd, nullptr);
    return 0;
  }
  if (message == WM_TIMER && wparam == kFadeTimer) {
    if (g_presenter.fading && host == g_presenter.previewHost) StepFade(PreviewClockMs());
    else KillTimer(hwnd, kFadeTimer);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
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

// The icon handle LoadBadgeIcon tries at `step`, in order: the window's small
// icon, its class's small icon, the window's big icon, its class's big icon.
// Not owned. Asking the window sets `queryFailed` when the send fails.
static HICON BadgeIconCandidate(HWND window, UINT dpi, int step, bool& queryFailed) {
  constexpr UINT kBadgeIconQueryTimeoutMs = 8;  // Move on quickly when an app is busy.
  if (step == 1 || step == 3) {
    return reinterpret_cast<HICON>(GetClassLongPtrW(window, step == 1 ? GCLP_HICONSM : GCLP_HICON));
  }
  DWORD_PTR handle = 0;
  if (!SendMessageTimeoutW(window, WM_GETICON, step == 0 ? ICON_SMALL : ICON_BIG, dpi,
                           SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, kBadgeIconQueryTimeoutMs, &handle)) {
    queryFailed = true;
  }
  return reinterpret_cast<HICON>(handle);
}

// The window's icon for a badge `size` pixels wide, as an owned copy: reloaded
// at exactly that size from its app's resources when the icon records them,
// else the smallest of the window's icons at least that large (shrunk), else
// the largest (stretched). The window's own icons come before its class's. The
// big icon is asked for only when the small ones won't do. A failed send
// skips further app calls and leaves any class icon eligible for a retry.
static PreviewIcon LoadBadgeIcon(HWND window, UINT dpi, LONG size, bool& queryFailed) {
  HICON chosen = nullptr;
  LONG chosenSize = 0;
  bool foundIconHandle = false;
  for (int step = 0; step < 4; ++step) {
    if (step == 2 && chosenSize >= size) break;  // The small icons already cover the badge.
    if (step == 2 && queryFailed) continue;
    const HICON icon = BadgeIconCandidate(window, dpi, step, queryFailed);
    if (!icon) continue;
    foundIconHandle = true;
    if (HICON exact = ReloadIconAtSize(icon, size)) return PreviewIcon(exact, DestroyIcon);
    const LONG pixels = IconPixelSize(icon);
    if (pixels > 0 && (!chosen || IsBetterBadgeIcon(pixels, chosenSize, size))) {
      chosen = icon;
      chosenSize = pixels;
    }
  }
  HICON copy = chosen ? CopyIcon(chosen) : nullptr;
  if (foundIconHandle && !copy) queryFailed = true;
  return copy ? PreviewIcon(copy, DestroyIcon) : PreviewIcon{};
}

static void CALLBACK RunBadgeIconLookup(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
  auto& lookup = *static_cast<BadgeIconLookup*>(context);
  if (lookup.state.load(std::memory_order_acquire) != BadgeIconLookup::State::Queued) return;
  const HWND notifyWindow = lookup.notifyWindow;
  const auto key = lookup.key;
  bool queryFailed = false;
  auto icon = key.IsLive()
      ? LoadBadgeIcon(key.window, key.dpi, key.size, queryFailed) : PreviewIcon{};
  lookup.queryFailed = queryFailed;
  lookup.icon = std::move(icon);
  lookup.state.store(BadgeIconLookup::State::Ready, std::memory_order_release);
  PostMessageW(notifyWindow, kBadgeIconReadyMessage, 0, 0);
}

static PreviewIcon BadgeIconForPreview(const GUID& desktopId, const BadgeIconKey& key) {
  constexpr size_t kMaxCachedBadgeIconsPerDesktop = 128;
  // Refresh an icon on a later drag if the app has had time to change it.
  constexpr ULONGLONG kBadgeIconRefreshMs = 30'000;
  auto& icons = g_presenter.iconsByDesktop[desktopId];
  auto entry = std::ranges::find(icons, key, &CachedBadgeIcon::key);
  if (entry != icons.end() && (entry->lastDrag == g_presenter.iconDrag ||
      (!entry->retry && GetTickCount64() - entry->loadedAt < kBadgeIconRefreshMs))) {
    entry->lastDrag = g_presenter.iconDrag;
    return entry->icon;
  }

  auto& lookup = g_presenter.iconLookup;
  if (!lookup) {
    lookup = std::make_unique<BadgeIconLookup>();
    lookup->work = CreateThreadpoolWork(RunBadgeIconLookup, lookup.get(), nullptr);
  }
  // When the worker is busy, its completion will check the current preview
  // again. Rapidly changing drop targets never build a queue of old requests.
  if (!lookup->work || lookup->state.load(std::memory_order_acquire) != BadgeIconLookup::State::Idle) {
    return entry == icons.end() ? PreviewIcon{} : entry->icon;
  }
  if (entry == icons.end()) {
    if (icons.size() < kMaxCachedBadgeIconsPerDesktop) {
      icons.push_back({.key = key});
      entry = std::prev(icons.end());
    } else {
      entry = std::ranges::min_element(icons, {}, &CachedBadgeIcon::lastDrag);
      if (entry->lastDrag == g_presenter.iconDrag) return {};
      *entry = {.key = key};
    }
  }
  entry->lastDrag = g_presenter.iconDrag;
  lookup->key = key;
  lookup->desktopId = desktopId;
  lookup->notifyWindow = g_presenter.previewHost->window;
  lookup->state.store(BadgeIconLookup::State::Queued, std::memory_order_release);
  SubmitThreadpoolWork(lookup->work);
  return entry->icon;
}

static bool CollectBadgeIcon() {
  const auto& lookup = g_presenter.iconLookup;
  if (!lookup || lookup->state.load(std::memory_order_acquire) != BadgeIconLookup::State::Ready) return false;

  auto desktop = g_presenter.iconsByDesktop.find(lookup->desktopId);
  if (desktop != g_presenter.iconsByDesktop.end()) {
    auto& icons = desktop->second;
    const auto entry = std::ranges::find(icons, lookup->key, &CachedBadgeIcon::key);
    if (entry != icons.end()) {
      if (!lookup->key.IsLive()) {
        icons.erase(entry);
        if (icons.empty()) g_presenter.iconsByDesktop.erase(desktop);
      } else {
        // An incomplete lookup may supply a fallback, but cannot clear or replace an existing icon.
        if (!lookup->queryFailed || !entry->icon) entry->icon = std::move(lookup->icon);
        entry->retry = lookup->queryFailed || !entry->icon;
        if (!entry->retry) entry->loadedAt = GetTickCount64();
      }
    }
  }
  lookup->icon.reset();
  lookup->state.store(BadgeIconLookup::State::Idle, std::memory_order_release);
  return true;
}

// --- Windows -----------------------------------------------------------------------

static bool EnsureWindowClass() {
  if (g_presenter.windowClass) return true;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = OverlayWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kOverlayClassName;
  g_presenter.windowClass = RegisterClassW(&windowClass);
  return g_presenter.windowClass != 0;
}

static void ClearToast(MonitorHost& host) {
  if (host.window) KillTimer(host.window, kToastTimer);
  host.toast.reset();
  host.toastText.Reset();
  host.toastIcons.clear();
}

// Hides the host and releases its surface; the window stays for the next attach.
// The commit detaches the released target from the window now rather than at
// the next unrelated draw.
static void ReleaseHostSurface(MonitorHost& host) {
  if (host.window) ShowWindow(host.window, SW_HIDE);
  host.visible = false;
  for (auto& badge : host.badges) badge = {};
  host.surface = {};
  ClearToast(host);
  if (auto* composition = g_presenter.device.composition.Get()) composition->Commit();
}

// After a drawing failure: a lost device takes every host's surface with it,
// otherwise only `host` starts over.
static void ResetHostSurface(MonitorHost& host) {
  BOOL valid = TRUE;
  auto* composition = g_presenter.device.composition.Get();
  if (composition && (FAILED(composition->CheckDeviceState(&valid)) || !valid)) {
    for (auto& [monitor, each] : g_presenter.hosts) ReleaseHostSurface(*each);
    g_presenter.device = {};
  } else {
    ReleaseHostSurface(host);
  }
}

static void ClearPreview();

// Frees the hosts of monitors that are gone. Monitor handles change with the
// display setup, so without this every reconnect would keep another
// monitor-sized window and surface until shutdown.
static void PruneDisconnectedMonitors() {
  std::erase_if(g_presenter.hosts, [](auto& entry) {
    auto& [monitor, host] = entry;
    if (Platform::Monitors::IsLiveMonitorHandle(monitor)) return false;
    g_presenter.latestToasts.erase(monitor);  // Its waiting toast, if any, is dropped.
    if (host.get() == g_presenter.previewHost) ClearPreview();
    if (host->window) KillTimer(host->window, kFadeTimer);
    ReleaseHostSurface(*host);
    if (host->window) DestroyWindow(host->window);
    return true;
  });
}

static MonitorHost& GetMonitorHost(HMONITOR monitor, const RECT& workArea) {
  // A handle not seen before can mean the display setup changed.
  if (!g_presenter.hosts.contains(monitor)) PruneDisconnectedMonitors();
  auto& entry = g_presenter.hosts[monitor];
  if (!entry) entry = std::make_unique<MonitorHost>();
  auto& host = *entry;
  if (host.surface.target && EqualRect(&host.workArea, &workArea)) return host;

  if (!host.window && EnsureWindowClass()) {
    // Layered and transparent keeps the monitor-sized host out of hit testing.
    host.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
        WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, kOverlayClassName, L"", WS_POPUP, workArea.left, workArea.top,
        Layout::RectWidth(workArea), Layout::RectHeight(workArea), nullptr, nullptr, GetModuleHandleW(nullptr), &host);
    if (!host.window || !SetLayeredWindowAttributes(host.window, 0, 255, LWA_ALPHA)) {
      if (host.window) DestroyWindow(std::exchange(host.window, nullptr));
      return host;
    }
    host.workArea = workArea;
  }
  if (!host.window || (!EqualRect(&host.workArea, &workArea) &&
      !SetWindowPos(host.window, nullptr, workArea.left, workArea.top,
                    Layout::RectWidth(workArea), Layout::RectHeight(workArea),
                    SWP_NOZORDER | SWP_NOACTIVATE)) ||
      !host.surface.Attach(g_presenter.device, host.window,
          D2D1::SizeU(Layout::RectWidth(workArea), Layout::RectHeight(workArea)))) {
    ResetHostSurface(host);
    return host;
  }
  host.workArea = workArea;
  return host;
}

// --- Overlay geometry (pure) -------------------------------------------------------

// `rect` (physical screen pixels) in the coordinates of a host whose surface
// starts at `origin`.
static D2D1_RECT_F ToHostRect(const RECT& rect, const RECT& origin) {
  return D2D1::RectF(static_cast<float>(rect.left - origin.left), static_cast<float>(rect.top - origin.top),
                     static_cast<float>(rect.right - origin.left), static_cast<float>(rect.bottom - origin.top));
}

// The preview's opacities, 0..1, at fade opacity `fadeAlpha` (0..255): the fill
// and outline each at their own configured share, the badges at the fade's.
struct PreviewOpacities {
  float fill = 0;
  float outline = 0;
  float badge = 0;
};

static PreviewOpacities PreviewOpacitiesAt(const PreviewStyle& style, BYTE fadeAlpha) {
  const float alpha = fadeAlpha / 255.0f;
  return {alpha * style.fillOpacity / 100.0f, alpha * style.outlineOpacity / 100.0f, alpha};
}

// A badge's square card and icon size for a tile, physical pixels.
struct BadgeGeometry {
  RECT frame{};  // Centred on the tile.
  LONG iconSize = 0;
  LONG badgeSize = 0;  // The card: the icon plus a 4 DIP margin each side.
};

// The badge for `tile` at `iconSizeDip`, or nothing when icons are off or the
// card doesn't fit inside the tile.
static std::optional<BadgeGeometry> BadgeGeometryFor(const RECT& tile, LONG iconSizeDip, UINT dpi) {
  const LONG iconSize = SharedGeometry::ScaleDip(iconSizeDip, dpi);
  const LONG badgeSize = iconSize + SharedGeometry::ScaleDip(8, dpi);
  if (iconSize <= 0 || badgeSize > Layout::RectWidth(tile) || badgeSize > Layout::RectHeight(tile)) {
    return std::nullopt;
  }
  const POINT center = SharedGeometry::RectCenter(tile);
  const LONG left = center.x - badgeSize / 2;
  const LONG top = center.y - badgeSize / 2;
  return BadgeGeometry{{left, top, left + badgeSize, top + badgeSize}, iconSize, badgeSize};
}

// Where the icon sits in a badge card drawn at `card`: centred.
static D2D1_RECT_F BadgeIconRect(const D2D1_RECT_F& card, LONG badgeSize, LONG iconSize) {
  const float inset = (badgeSize - iconSize) / 2.0f;
  return D2D1::RectF(card.left + inset, card.top + inset, card.left + inset + iconSize, card.top + inset + iconSize);
}

// Where a toast whose content (icons and text) is `contentWidth` pixels wide
// goes on a host the size of `workSize` (the work area), in the host's
// coordinates: 48 DIP tall, as wide as its content plus 48 DIP (at least 128
// DIP, at most the work area), centred, 16 DIP from the top or bottom edge,
// moved by the request's offsets but kept inside the work area. Nothing when
// the work area is shorter than a toast.
static std::optional<D2D1_RECT_F> ToastFrame(const ToastRequest& request, SIZE workSize, UINT dpi, float contentWidth) {
  const float available = static_cast<float>(workSize.cx);
  const float workHeight = static_cast<float>(workSize.cy);
  const float height = static_cast<float>(SharedGeometry::ScaleDip(48, dpi));
  if (height > workHeight) return std::nullopt;
  const float width = std::min(available, std::max<float>(
      SharedGeometry::ScaleDip(128, dpi), std::ceil(contentWidth) + SharedGeometry::ScaleDip(48, dpi)));
  const float left = (available - width) / 2 + SharedGeometry::ScaleDip(request.xOffsetDip, dpi);
  const float gap = static_cast<float>(SharedGeometry::ScaleDip(16, dpi));
  const float top = (request.atTop ? gap : workHeight - height - gap) +
                    SharedGeometry::ScaleDip(request.yOffsetDip, dpi);
  const float x = std::floor(std::clamp(left, 0.0f, available - width));
  const float y = std::clamp(top, 0.0f, workHeight - height);
  return D2D1::RectF(x, y, x + width, y + height);
}

// The width of a toast's icons and the gap before its text: `iconCount` icons
// `iconSize` pixels wide, 4 DIP apart, then 8 DIP. 0 without icons.
static float ToastIconsWidth(size_t iconCount, LONG iconSize, UINT dpi) {
  if (iconCount == 0) return 0;
  const LONG count = static_cast<LONG>(iconCount);
  return static_cast<float>(count * iconSize + (count - 1) * SharedGeometry::ScaleDip(4, dpi) +
                            SharedGeometry::ScaleDip(8, dpi));
}

// Where a toast's icons and text go inside its frame, in host coordinates.
struct ToastContentLayout {
  std::vector<D2D1_RECT_F> icons;
  float textLeft = 0;
  float textWidth = 0;  // The text box, at most the room left in the frame.
};

// Lays out `iconCount` icons (ToastIconsWidth) and then text `textWidth` wide
// as one row centred in `frame`. Each icon sits on whole pixels, so one drawn
// at its own size isn't resampled, centred on `iconMiddle` pixels below the
// frame's top (the middle of the text's capitals, so it lines up with the
// words rather than with their descenders).
static ToastContentLayout LayOutToastContent(const D2D1_RECT_F& frame, size_t iconCount, LONG iconSize, UINT dpi,
                                             float textWidth, float iconMiddle) {
  ToastContentLayout layout;
  const float iconsWidth = ToastIconsWidth(iconCount, iconSize, dpi);
  const float frameWidth = frame.right - frame.left;
  const float left = frame.left + std::floor(std::max(0.0f, (frameWidth - iconsWidth - std::ceil(textWidth)) / 2));
  const float top = std::round(frame.top + iconMiddle - iconSize / 2.0f);
  const float step = static_cast<float>(iconSize + SharedGeometry::ScaleDip(4, dpi));
  for (size_t i = 0; i < iconCount; ++i) {
    const float iconLeft = left + step * i;
    layout.icons.push_back(D2D1::RectF(iconLeft, top, iconLeft + iconSize, top + iconSize));
  }
  layout.textLeft = left + iconsWidth;
  layout.textWidth = std::max(0.0f, std::min(std::ceil(textWidth), frame.right - layout.textLeft));
  return layout;
}

// --- Drawing -----------------------------------------------------------------------

static bool DrawHost(MonitorHost& host) {
  if (!host.surface.target) return false;
  const auto* request = g_presenter.previewHost == &host && g_presenter.shown ? &*g_presenter.shown : nullptr;
  const bool showPreview = request && (g_presenter.fadeAlpha || g_presenter.fadeTo);
  const bool visible = showPreview || host.toastText;
  if (!visible && !host.visible) return true;
  if (visible && (!host.visible || host.raise)) {
    // An already visible host keeps its old place among topmost windows.
    SetWindowPos(host.window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    if (!IsWindowVisible(host.window)) {
      ResetHostSurface(host);
      return false;
    }
    host.visible = true;
    host.raise = false;
    KillTimer(host.window, kSurfaceReleaseTimer);
  }
  auto& surface = host.surface;
  if (!surface.BeginDraw()) {
    ResetHostSurface(host);
    return false;
  }
  auto& target = *surface.target.Get();
  target.Clear(D2D1::ColorF(0, 0.0f));
  const auto drawSolidCard = [&](const D2D1_RECT_F& frame, UINT dpi,
                                 const PopupAppearance& appearance, float alpha) {
    const float radius = SharedGeometry::ScaleDip(8, dpi);
    surface.Fill(frame, radius, appearance.background, alpha);
    // The native dark popup has a light rim against its dark fill.
    const COLORREF strokeColor = appearance.light ? RGB(0, 0, 0) : RGB(255, 255, 255);
    const float strokeOpacity = (appearance.light ? 0.23f : 0.13f) * alpha;
    surface.Outline(frame, radius, SharedGeometry::ScaleDip(1, dpi), strokeColor, strokeOpacity);
  };
  if (showPreview) {
    const auto& style = g_presenter.style;
    const auto& preview = request->preview;
    const PreviewOpacities opacity = PreviewOpacitiesAt(style, g_presenter.fadeAlpha);
    const float radius = SharedGeometry::ScaleDip(style.outlineRadiusDip, request->dpi);
    const float width = std::max<LONG>(1, SharedGeometry::ScaleDip(style.outlineWidthDip, request->dpi));
    surface.Fill(ToHostRect(preview.dragged, host.workArea), radius, g_presenter.accent, opacity.fill);
    surface.Outline(ToHostRect(preview.affected, host.workArea), radius, width, g_presenter.accent, opacity.outline);
    size_t index = 0;
    for (const auto* badge : {&g_presenter.draggedBadge, &g_presenter.affectedBadge}) {
      auto& rendered = host.badges[index++];
      if (!badge->icon) continue;
      if (rendered.icon != badge->icon) {
        rendered.icon = badge->icon;
        rendered.bitmap = g_presenter.device.IconBitmap(target, badge->icon.get());
      }
      if (!rendered.bitmap) continue;
      const auto card = ToHostRect(badge->frame, host.workArea);
      drawSolidCard(card, request->dpi, style.badgeAppearance, opacity.badge);
      target.DrawBitmap(rendered.bitmap.Get(), BadgeIconRect(card, badge->badgeSize, badge->iconSize), opacity.badge);
    }
  }
  if (host.toastText) {
    const auto& toast = *host.toast;
    drawSolidCard(host.toastFrame, host.toastDpi, toast.appearance, host.toastAlpha);
    for (const auto& icon : host.toastIcons) {
      // At its own size the cubic filter changes nothing; it is for an app
      // that has no icon of that size.
      target.DrawBitmap(icon.bitmap.Get(), &icon.rect, host.toastAlpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    surface.brush->SetColor(PopupSurface::Color(CardTextColor(toast.appearance.light), host.toastAlpha));
    target.DrawTextLayout(host.toastTextOrigin, host.toastText.Get(), surface.brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (!surface.EndDraw(g_presenter.device)) {
    ResetHostSurface(host);
    return false;
  }
  if (!visible) {
    ShowWindow(host.window, SW_HIDE);
    host.visible = false;
    SetTimer(host.window, kSurfaceReleaseTimer, kIdleSurfaceReleaseMs, nullptr);
  }
  return true;
}

// The middle of `text`'s capitals, in pixels below the top of its layout box;
// the box's middle when the line can't be measured.
static float TextCapMiddle(IDWriteTextLayout& text, const DWRITE_TEXT_METRICS& metrics, float fontSize) {
  DWRITE_LINE_METRICS line{};
  UINT32 lines = 0;
  if (FAILED(text.GetLineMetrics(&line, 1, &lines)) || lines != 1) return metrics.top + metrics.height / 2;
  return metrics.top + line.baseline - g_presenter.device.CapHeightRatio() * fontSize / 2;
}

// Lays out and shows `request` on its monitor, replacing the toast there, with
// `icons` (loaded for request.apps) before the text. Missing icons, and any
// Direct2D can't take, are left out.
static void ShowToastWithIcons(ToastRequest request, const std::vector<PreviewIcon>& icons) {
  RECT workArea{};
  if (!Platform::Monitors::GetOverlayWorkArea(request.monitor, &workArea)) return;
  const UINT dpi = Platform::Monitors::GetMonitorEffectiveDpi(request.monitor);
  auto& host = GetMonitorHost(request.monitor, workArea);
  if (!host.surface.target) return;
  std::vector<MonitorHost::ToastIcon> shownIcons;
  for (const PreviewIcon& icon : icons) {
    if (shownIcons.size() == kMaxToastIcons) break;
    if (!icon) continue;
    if (auto bitmap = g_presenter.device.IconBitmap(*host.surface.target.Get(), icon.get())) {
      shownIcons.push_back({std::move(bitmap)});
    }
  }
  const SIZE workSize{Layout::RectWidth(workArea), Layout::RectHeight(workArea)};
  const float fontSize = static_cast<float>(SharedGeometry::ScaleDip(14, dpi));
  auto text = g_presenter.device.LayoutText(request.text, fontSize, static_cast<float>(workSize.cx),
                                            static_cast<float>(SharedGeometry::ScaleDip(48, dpi)));
  DWRITE_TEXT_METRICS metrics{};
  if (!text || FAILED(text->GetMetrics(&metrics))) return;
  const LONG iconSize = SharedGeometry::ScaleDip(kToastIconDip, dpi);
  const auto frame =
      ToastFrame(request, workSize, dpi, ToastIconsWidth(shownIcons.size(), iconSize, dpi) + metrics.width);
  if (!frame) return;
  const ToastContentLayout content = LayOutToastContent(*frame, shownIcons.size(), iconSize, dpi, metrics.width,
                                                        TextCapMiddle(*text.Get(), metrics, fontSize));
  if (FAILED(text->SetMaxWidth(content.textWidth))) return;
  for (size_t i = 0; i < shownIcons.size(); ++i) shownIcons[i].rect = content.icons[i];
  host.toast = std::move(request);
  host.toastText = std::move(text);
  host.toastIcons = std::move(shownIcons);
  host.toastFrame = *frame;
  host.toastTextOrigin = D2D1::Point2F(content.textLeft, frame->top);
  host.toastDpi = dpi;
  host.toastStarted = GetTickCount64();
  host.toastAlpha = 1.0f;
  host.raise = true;
  if (DrawHost(host) &&
      !SetTimer(host.window, kToastTimer, host.toast->holdMs, nullptr)) {
    ClearToast(host);
    DrawHost(host);
  }
}

// Thread pool: loads a ToastIconJob's icons, then tells the presenter.
static void CALLBACK LoadToastIcons(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
  auto& job = *static_cast<ToastIconJob*>(context);
  for (const BadgeIconKey& app : job.request.apps) {
    bool queryFailed = false;
    job.icons.push_back(app.IsLive() ? LoadBadgeIcon(app.window, app.dpi, app.size, queryFailed) : PreviewIcon{});
  }
  job.posted.store(PostMessageW(job.notifyWindow, kToastIconsReadyMessage, static_cast<WPARAM>(job.id), 0) != FALSE,
                   std::memory_order_relaxed);
  job.done.store(true, std::memory_order_release);
}

// Shows `request` on its monitor, replacing the toast there. A toast naming
// apps first waits for their icons (ToastIconJob), typically a millisecond or
// two; without a worker it shows at once, without them.
static void ShowToast(ToastRequest request) {
  const uint64_t id = ++g_presenter.lastToastId;
  g_presenter.latestToasts[request.monitor] = id;
  // Drop jobs that will show nothing: replaced, or unable to say they're done.
  std::erase_if(g_presenter.toastIconJobs, [](const std::unique_ptr<ToastIconJob>& job) {
    if (!job->done.load(std::memory_order_acquire)) return false;
    const auto latest = g_presenter.latestToasts.find(job->request.monitor);
    return !job->posted.load(std::memory_order_relaxed) || latest == g_presenter.latestToasts.end() ||
           latest->second != job->id;
  });
  RECT workArea{};
  if (request.apps.empty() || !Platform::Monitors::GetOverlayWorkArea(request.monitor, &workArea)) {
    return ShowToastWithIcons(std::move(request), {});
  }
  const HWND notifyWindow = GetMonitorHost(request.monitor, workArea).window;
  if (!notifyWindow) return;
  const UINT dpi = Platform::Monitors::GetMonitorEffectiveDpi(request.monitor);
  if (request.apps.size() > kMaxToastIcons) request.apps.resize(kMaxToastIcons);
  for (BadgeIconKey& app : request.apps) {
    app.dpi = dpi;
    app.size = SharedGeometry::ScaleDip(kToastIconDip, dpi);
  }
  auto job = std::make_unique<ToastIconJob>();
  job->id = id;
  job->notifyWindow = notifyWindow;
  job->request = std::move(request);
  job->work = CreateThreadpoolWork(LoadToastIcons, job.get(), nullptr);
  if (!job->work) return ShowToastWithIcons(std::move(job->request), {});
  SubmitThreadpoolWork(job->work);
  g_presenter.toastIconJobs.push_back(std::move(job));
}

// kToastIconsReadyMessage: shows job `id`'s toast with its icons, unless a
// later toast on that monitor replaced it, and drops the job.
static void FinishToastIconJob(uint64_t id) {
  auto& jobs = g_presenter.toastIconJobs;
  const auto entry = std::ranges::find(jobs, id, [](const std::unique_ptr<ToastIconJob>& job) { return job->id; });
  if (entry == jobs.end()) return;
  const std::unique_ptr<ToastIconJob> job = std::move(*entry);
  jobs.erase(entry);
  WaitForThreadpoolWorkCallbacks(job->work, FALSE);  // The worker posted just before returning.
  const auto latest = g_presenter.latestToasts.find(job->request.monitor);
  if (latest == g_presenter.latestToasts.end() || latest->second != id) return;
  ShowToastWithIcons(std::move(job->request), job->icons);
}

static void StepToast(MonitorHost& host) {
  if (!host.toast) return;
  const ULONGLONG elapsed = GetTickCount64() - host.toastStarted;
  const DWORD hold = host.toast->holdMs;
  if (elapsed < hold) {
    if (SetTimer(host.window, kToastTimer, static_cast<UINT>(hold - elapsed), nullptr)) return;
    ClearToast(host);
  } else if (elapsed >= hold + kToastFadeMs) {
    ClearToast(host);
  } else {
    if (host.toastAlpha == 1.0f &&
        !SetTimer(host.window, kToastTimer, kToastFadeIntervalMs, nullptr)) {
      ClearToast(host);
    } else {
      host.toastAlpha = 1.0f - (elapsed - hold) / static_cast<float>(kToastFadeMs);
    }
  }
  DrawHost(host);
}

// --- App badges --------------------------------------------------------------------

static void ShowBadge(IconBadge& badge, HWND window, const RECT& tile, const PresentRequest& request) {
  const UINT dpi = request.dpi;
  const auto geometry = BadgeGeometryFor(tile, g_presenter.style.iconSizeDip, dpi);
  if (!window || !geometry) {
    badge.Hide();
    return;
  }

  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(window, &processId);
  auto icon = threadId
      ? BadgeIconForPreview(request.desktopId, {window, processId, threadId, dpi, geometry->iconSize})
      : PreviewIcon{};
  if (!icon) {
    badge.Hide();
    return;
  }

  badge.badgeSize = geometry->badgeSize;
  badge.iconSize = geometry->iconSize;
  // Retain our own copy while a badge is fading out.
  badge.icon = std::move(icon);
  badge.frame = geometry->frame;
}

// The dragged window's badge on the fill, the other window's on the outline.
static void ShowBadges(const PresentRequest& request) {
  const Commands::TiledDropPreview& preview = request.preview;
  const PreviewStyle& style = g_presenter.style;
  const bool sameTile = EqualRect(&preview.dragged, &preview.affected);
  if (request.badges && (style.fillOpacity > 0 || sameTile)) {
    ShowBadge(g_presenter.draggedBadge, preview.draggedWindow, preview.dragged, request);
  } else {
    g_presenter.draggedBadge.Hide();
  }
  if (request.badges && !sameTile && style.outlineOpacity > 0) {
    ShowBadge(g_presenter.affectedBadge, preview.affectedWindow, preview.affected, request);
  } else {
    g_presenter.affectedBadge.Hide();
  }
}

static void UpdateBadgesAfterIconLookup() {
  if (CollectBadgeIcon() && g_presenter.fadeTo == 255 && g_presenter.shown) {
    ShowBadges(*g_presenter.shown);
    if (!DrawHost(*g_presenter.previewHost)) ClearPreview();
  }
}

// --- Presentation (the presenting thread) ------------------------------------------

static void Present(const PresentRequest& request) {
  const bool iconReady = CollectBadgeIcon();
  if (g_presenter.fadeTo == 255 && g_presenter.shown && g_presenter.previewHost->surface.target &&
      SameRequest(*g_presenter.shown, request)) {
    if (iconReady) {
      ShowBadges(request);
      if (!DrawHost(*g_presenter.previewHost)) ClearPreview();
    }
    return;
  }

  const HMONITOR monitor = MonitorFromRect(&request.preview.dragged, MONITOR_DEFAULTTONEAREST);
  RECT workArea{};
  if (!Platform::Monitors::GetOverlayWorkArea(monitor, &workArea)) {
    ClearPreview();
    return;
  }
  auto& host = GetMonitorHost(monitor, workArea);
  if (!host.surface.target) {
    ClearPreview();
    return;
  }
  if (g_presenter.fadeTo != 255) host.raise = true;  // Not already shown: a new preview.
  if (auto* previous = std::exchange(g_presenter.previewHost, &host); previous && previous != &host) {
    if (!g_presenter.paced && g_presenter.fading) {
      KillTimer(previous->window, kFadeTimer);
      SetTimer(host.window, kFadeTimer, kFadeIntervalMs, nullptr);
    }
    DrawHost(*previous);
  }
  g_presenter.style = request.style;

  DWORD accent = 0;
  BOOL opaqueBlend = FALSE;
  g_presenter.accent = SUCCEEDED(DwmGetColorizationColor(&accent, &opaqueBlend))
      ? RGB((accent >> 16) & 0xff, (accent >> 8) & 0xff, accent & 0xff)
      : GetSysColor(COLOR_HIGHLIGHT);
  // Reveal a changed target at half strength once the current fade has finished.
  const bool changedTarget = g_presenter.fadeTo == 255 && g_presenter.fadeAlpha == 255 && request.style.fadeMs > 0;
  if (changedTarget) g_presenter.fadeAlpha = 128;
  const BYTE alpha = g_presenter.fadeAlpha;
  g_presenter.shown = request;
  ShowBadges(request);
  FadeTo(255, changedTarget);
  // An immediate opacity change already drew the new scene in StepFade.
  if (g_presenter.previewHost && g_presenter.fadeAlpha == alpha && !DrawHost(host)) ClearPreview();
}

static void RepaintCards(PopupAppearance appearance) {
  g_presenter.style.badgeAppearance = appearance;
  for (auto& [monitor, host] : g_presenter.hosts) {
    if (host->toast) host->toast->appearance = appearance;
    if (host->visible && !DrawHost(*host) && host.get() == g_presenter.previewHost) {
      ClearPreview();
    }
  }
}

static void DestroyHosts() {
  // Join the icon workers before destroying the windows they notify.
  g_presenter.iconLookup.reset();
  g_presenter.toastIconJobs.clear();
  for (auto& [monitor, host] : g_presenter.hosts) {
    if (host->window) DestroyWindow(host->window);
  }
  if (g_presenter.windowClass) UnregisterClassW(kOverlayClassName, GetModuleHandleW(nullptr));
  g_presenter = {};
}

// --- The overlay thread ------------------------------------------------------------

enum OverlayMessage : UINT {
  kPreviewUpdateMessage = WM_APP + 1,  // lParam: PreviewUpdate*, owned by the receiver.
  kCardThemeMessage,                   // wParam: background color. lParam: light theme.
  kNewDragMessage,                     // Allow another attempt at missing or outdated icons.
  kWindowDestroyedMessage,             // wParam: HWND whose icons must be evicted.
  kDesktopListMessage,                 // lParam: vector<GUID>*, owned by the receiver.
  kToastMessage,                       // lParam: ToastRequest*, owned by the receiver.
  kWarmOverlayMessage,                 // wParam: HMONITOR.
  kDisplayChangedMessage,              // Free the hosts of monitors that are gone.
  kOverlayMessageEnd,                  // One past the last; the shutdown drain covers the range.
};

using PreviewUpdate = std::optional<PresentRequest>;  // An empty update hides the preview.

struct OverlayThread {
  HANDLE handle = nullptr;
  HANDLE stopEvent = nullptr;
  std::atomic<DWORD> id{0};
  std::mutex updateMutex;
  // The queued update can be replaced until the renderer takes it or a
  // control message seals it. Rendering never holds the mutex.
  PreviewUpdate* pendingUpdate = nullptr;
  std::unordered_map<HMONITOR, ToastRequest*> pendingToasts;
};
static OverlayThread g_overlayThread;

struct OverlayThreadStartup {
  HANDLE ready;
  HANDLE stop;
};

// True for the overlay thread, whose windows MultiWM must ignore like its own.
static bool IsOverlayThread(DWORD threadId) {
  const DWORD id = g_overlayThread.id.load(std::memory_order_acquire);
  return id && id == threadId;
}

// kPreviewUpdateMessage: takes ownership of the update, unseals the queue slot
// it held, and shows it (an empty update fades the preview out).
static void TakePreviewUpdate(LPARAM payload) {
  const std::unique_ptr<PreviewUpdate> update(reinterpret_cast<PreviewUpdate*>(payload));
  {
    const std::scoped_lock lock(g_overlayThread.updateMutex);
    if (g_overlayThread.pendingUpdate == update.get()) g_overlayThread.pendingUpdate = nullptr;
  }
  if (*update) Present(**update);
  else FadeTo(0);
}

// kToastMessage: as TakePreviewUpdate, for one monitor's pending toast.
static void TakeToast(LPARAM payload) {
  const std::unique_ptr<ToastRequest> request(reinterpret_cast<ToastRequest*>(payload));
  {
    const std::scoped_lock lock(g_overlayThread.updateMutex);
    const auto entry = g_overlayThread.pendingToasts.find(request->monitor);
    if (entry != g_overlayThread.pendingToasts.end() && entry->second == request.get()) {
      g_overlayThread.pendingToasts.erase(entry);
    }
  }
  ShowToast(std::move(*request));
}

// kWindowDestroyedMessage: drops `window`'s cached icons on every desktop and
// its badge from the shown preview.
static void ForgetDestroyedWindow(HWND window) {
  for (auto& [desktopId, icons] : g_presenter.iconsByDesktop) {
    std::erase_if(icons, [window](const CachedBadgeIcon& entry) { return entry.key.window == window; });
  }
  std::erase_if(g_presenter.iconsByDesktop, [](const auto& desktop) { return desktop.second.empty(); });
  if (!g_presenter.shown) return;
  auto& preview = g_presenter.shown->preview;
  const bool dragged = preview.draggedWindow == window;
  const bool affected = preview.affectedWindow == window;
  if (!dragged && !affected) return;
  if (dragged) {
    g_presenter.draggedBadge.Hide();
    preview.draggedWindow = nullptr;
  }
  if (affected) {
    g_presenter.affectedBadge.Hide();
    preview.affectedWindow = nullptr;
  }
  if (g_presenter.previewHost && !DrawHost(*g_presenter.previewHost)) ClearPreview();
}

// kDesktopListMessage: keeps only the icon caches of the listed desktops.
static void KeepDesktopIcons(LPARAM payload) {
  const std::unique_ptr<std::vector<GUID>> desktops(reinterpret_cast<std::vector<GUID>*>(payload));
  std::erase_if(g_presenter.iconsByDesktop, [&](const auto& desktop) {
    return std::ranges::none_of(*desktops, [&](const GUID& id) { return IsEqualGUID(id, desktop.first); });
  });
}

// Warms the device, window and surface for the first preview or toast. An
// unused surface is released like any idle one; the device stays warm.
static void WarmMonitorHost(HMONITOR monitor) {
  RECT workArea{};
  if (!Platform::Monitors::GetOverlayWorkArea(monitor, &workArea)) return;
  MonitorHost& host = GetMonitorHost(monitor, workArea);
  if (host.window && host.surface.target && !host.visible) {
    SetTimer(host.window, kSurfaceReleaseTimer, kIdleSurfaceReleaseMs, nullptr);
  }
}

static void HandleOverlayMessage(const MSG& msg) {
  if (msg.hwnd) {
    DispatchMessageW(&msg);
    return;
  }
  switch (msg.message) {
    case kPreviewUpdateMessage: return TakePreviewUpdate(msg.lParam);
    case kCardThemeMessage: return RepaintCards({static_cast<COLORREF>(msg.wParam), msg.lParam != 0});
    case kNewDragMessage: ++g_presenter.iconDrag; return;
    case kWindowDestroyedMessage: return ForgetDestroyedWindow(reinterpret_cast<HWND>(msg.wParam));
    case kDesktopListMessage: return KeepDesktopIcons(msg.lParam);
    case kToastMessage: return TakeToast(msg.lParam);
    case kWarmOverlayMessage: return WarmMonitorHost(reinterpret_cast<HMONITOR>(msg.wParam));
    case kDisplayChangedMessage: return PruneDisconnectedMonitors();
    default: return;
  }
}

// Steps the overlay thread's fades at the compositor's refresh period. The
// thread waits on this timer and its message queue together, so a new target
// can be drawn before the next scheduled opacity step.
struct FadePacer {
  HANDLE timer = nullptr;  // High-resolution waitable timer; null falls back to a wait timeout.
  LONGLONG frequency = 0;  // QueryPerformanceFrequency.
  LONGLONG nextStep = 0;   // QPC time of the next opacity step; 0 while no fade runs.
  LONGLONG interval = 0;   // QPC ticks between steps, read when a fade starts.

  // One refresh period, or kFadeIntervalMs when DWM can't say.
  LONGLONG RefreshInterval() const {
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    return SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod
        ? static_cast<LONGLONG>(timing.qpcRefreshPeriod) : frequency * kFadeIntervalMs / 1000;
  }

  // Steps a running fade when due, then schedules the next step: arms `timer`
  // (`*armed` true) or returns the wait timeout in milliseconds. INFINITE when
  // no fade runs.
  DWORD StepAndSchedule(bool* armed) {
    *armed = false;
    if (!g_presenter.fading) {
      nextStep = 0;
      return INFINITE;
    }
    if (!nextStep) interval = RefreshInterval();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= nextStep) {
      StepFade(PreviewClockMs());
      nextStep = now.QuadPart + interval;
      QueryPerformanceCounter(&now);
    }
    if (!g_presenter.fading) {
      nextStep = 0;
      return INFINITE;
    }
    const LONGLONG remaining = std::max<LONGLONG>(0, nextStep - now.QuadPart);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(1, remaining * 10'000'000 / frequency);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      *armed = true;
      return INFINITE;
    }
    return static_cast<DWORD>((-due.QuadPart + 9'999) / 10'000);
  }
};

// At thread exit: frees the payloads of messages the overlay thread never
// handled. Holding the mutex keeps publishers from replacing one meanwhile.
static void DiscardUndeliveredPayloads() {
  const std::scoped_lock lock(g_overlayThread.updateMutex);
  g_overlayThread.pendingUpdate = nullptr;
  g_overlayThread.pendingToasts.clear();
  MSG msg{};
  while (PeekMessageW(&msg, reinterpret_cast<HWND>(-1), kPreviewUpdateMessage, kOverlayMessageEnd - 1, PM_REMOVE)) {
    if (msg.message == kPreviewUpdateMessage) delete reinterpret_cast<PreviewUpdate*>(msg.lParam);
    if (msg.message == kDesktopListMessage) delete reinterpret_cast<std::vector<GUID>*>(msg.lParam);
    if (msg.message == kToastMessage) delete reinterpret_cast<ToastRequest*>(msg.lParam);
  }
}

static DWORD WINAPI OverlayThreadProc(LPVOID parameter) {
  const auto startup = *static_cast<const OverlayThreadStartup*>(parameter);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  MSG msg{};
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // Create the queue before anyone posts.
  g_presenter.paced = true;
  FadePacer pacer;
  pacer.timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  pacer.frequency = frequency.QuadPart;
  const HANDLE waits[] = {startup.stop, pacer.timer};
  SetEvent(startup.ready);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (WaitForSingleObject(startup.stop, 0) == WAIT_TIMEOUT) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) break;
      HandleOverlayMessage(msg);
    }
    bool timerArmed = false;
    const DWORD timeout = pacer.StepAndSchedule(&timerArmed);
    if (MsgWaitForMultipleObjectsEx(timerArmed ? 2 : 1, waits, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
        WAIT_FAILED) {
      break;
    }
  }
  if (pacer.timer) CloseHandle(pacer.timer);
  DiscardUndeliveredPayloads();
  DestroyHosts();
  if (SUCCEEDED(com)) CoUninitialize();
  return 0;
}

// Starts the overlay thread once. False when it can't start; overlays then
// run on the WM thread.
static bool EnsureOverlayThread() {
  if (g_overlayThread.handle) return true;
  // A synchronous fallback already owns windows on the calling thread.
  if (g_presenter.windowClass) return false;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready) return false;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop) {
    CloseHandle(ready);
    return false;
  }
  OverlayThreadStartup startup{ready, stop};
  DWORD id = 0;
  HANDLE thread = CreateThread(nullptr, 0, OverlayThreadProc, &startup, 0, &id);
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
    Wh_Log(L"Overlay thread did not start; overlays run on the WM thread");
    return false;
  }
  SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
  g_overlayThread.handle = thread;
  g_overlayThread.stopEvent = stop;
  g_overlayThread.id.store(id, std::memory_order_release);
  return true;
}

// Preserve control-message ordering. A later target needs its own queue slot.
static bool PostToOverlayThread(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
  const DWORD id = g_overlayThread.id.load(std::memory_order_acquire);
  const std::scoped_lock lock(g_overlayThread.updateMutex);
  if (!id || !PostThreadMessageW(id, message, wparam, lparam)) return false;
  g_overlayThread.pendingUpdate = nullptr;
  g_overlayThread.pendingToasts.clear();
  return true;
}

static void PublishPreview(PreviewUpdate update) {
  const std::scoped_lock lock(g_overlayThread.updateMutex);
  if (g_overlayThread.pendingUpdate) {
    *g_overlayThread.pendingUpdate = std::move(update);
    return;
  }
  const DWORD id = g_overlayThread.id.load(std::memory_order_acquire);
  if (!id) return;
  auto posted = std::make_unique<PreviewUpdate>(std::move(update));
  if (PostThreadMessageW(id, kPreviewUpdateMessage, 0, reinterpret_cast<LPARAM>(posted.get()))) {
    g_overlayThread.pendingUpdate = posted.release();
  }
}

// --- WM thread API -----------------------------------------------------------------

// Shows `request`'s preview, fading it in, or moves a shown one to it.
static void ShowPreview(PresentRequest request) {
  if (g_overlayThread.handle) PublishPreview(std::move(request));
  else Present(request);
}

static void HidePreview() {
  if (g_overlayThread.handle) PublishPreview(std::nullopt);
  else FadeTo(0);
}

// Toasts start the overlay thread if it isn't running yet.
static void PublishToast(ToastRequest request) {
  if (!g_overlayThread.handle && !EnsureOverlayThread()) {
    ShowToast(std::move(request));
    return;
  }
  const std::scoped_lock lock(g_overlayThread.updateMutex);
  const HMONITOR monitor = request.monitor;
  if (const auto entry = g_overlayThread.pendingToasts.find(monitor); entry != g_overlayThread.pendingToasts.end()) {
    *entry->second = std::move(request);
    return;
  }
  const DWORD id = g_overlayThread.id.load(std::memory_order_acquire);
  if (!id) return;
  auto posted = std::make_unique<ToastRequest>(std::move(request));
  if (PostThreadMessageW(id, kToastMessage, 0, reinterpret_cast<LPARAM>(posted.get()))) {
    g_overlayThread.pendingToasts[monitor] = posted.release();
  }
}

// Repaints shown cards for a theme change.
static void RefreshAppearance(PopupAppearance appearance) {
  if (g_overlayThread.handle) {
    PostToOverlayThread(kCardThemeMessage, appearance.background, appearance.light);
  } else {
    RepaintCards(appearance);
  }
}

// Starts the overlay thread and builds the primary monitor's host ahead of the
// first preview or toast, so neither waits for the GPU.
static void WarmPrimaryMonitor() {
  if (!EnsureOverlayThread()) return;
  const HMONITOR primary = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
  PostToOverlayThread(kWarmOverlayMessage, reinterpret_cast<WPARAM>(primary));
}

// A new drag: icons missing or outdated in the last one may be looked up again.
static void BeginIconDrag() {
  if (EnsureOverlayThread()) PostToOverlayThread(kNewDragMessage);
  else ++g_presenter.iconDrag;
}

// Evicts a destroyed window's cached icons and badges.
static void ForgetWindow(HWND hwnd) {
  const auto window = reinterpret_cast<WPARAM>(hwnd);
  if (g_overlayThread.handle) PostToOverlayThread(kWindowDestroyedMessage, window);
  else HandleOverlayMessage(MSG{.message = kWindowDestroyedMessage, .wParam = window});
}

// Called from RuntimeLifecycle::NotifyDisplayTopologyChanged.
static void NotifyDisplayChanged() {
  if (g_overlayThread.handle) PostToOverlayThread(kDisplayChangedMessage);
  else PruneDisconnectedMonitors();
}

// Drops the icon caches of desktops that no longer exist.
static void PruneDesktopIcons() {
  if (!g_overlayThread.handle && g_presenter.iconsByDesktop.empty()) return;
  auto desktops = std::make_unique<std::vector<GUID>>();
  if (!Platform::VirtualDesktop::GetDesktopIdsInOrder(desktops.get())) return;
  if (!g_overlayThread.handle) {
    HandleOverlayMessage(MSG{.message = kDesktopListMessage, .lParam = reinterpret_cast<LPARAM>(desktops.release())});
  } else if (PostToOverlayThread(kDesktopListMessage, 0, reinterpret_cast<LPARAM>(desktops.get()))) {
    desktops.release();
  }
}

// Joins the overlay thread, which destroys its windows, or destroys the
// calling thread's.
static void Shutdown() {
  if (HANDLE thread = g_overlayThread.handle) {
    SetEvent(g_overlayThread.stopEvent);
    // WM shutdown already keeps the session alive if this join takes too long.
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
      Wh_Log(L"Could not join the overlay thread: %lu", GetLastError());
      return;
    }
    CloseHandle(thread);
    CloseHandle(g_overlayThread.stopEvent);
    g_overlayThread.handle = nullptr;
    g_overlayThread.stopEvent = nullptr;
    g_overlayThread.id.store(0, std::memory_order_release);
    return;
  }
  DestroyHosts();
}

}  // namespace Overlay