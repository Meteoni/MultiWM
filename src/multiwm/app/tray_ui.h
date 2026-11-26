//
// UI boundary for the tool mod: no Explorer hooks, XAML, WinUI, or Windows App
// SDK dependency. The tray uses two-letter mode abbreviations that are fitted to
// the full icon canvas. Status toasts are what Overlay (app/overlay.h)
// draws; this chapter decides their text, monitor, position and theme.

namespace TrayUi {

constexpr wchar_t kTrayWindowClassName[] = L"WindhawkMultiWMTrayWindow";
constexpr UINT kTrayCallbackMessage = WM_APP + 20;
constexpr UINT kIconId = 1;
constexpr UINT kContextTileWorkspace = 41001;
constexpr UINT kContextToggleMode = 41002;
constexpr UINT kContextLayoutBase = 41100;
// The tray text's height for an icon kTrayTextDesignSize pixels square; it
// scales with the icon.
constexpr int kTrayTextDesignSize = 32;
constexpr int kTrayFontHeight = 26;
// Each icon pixel averages this many aliased samples per axis (TextCoverage).
constexpr int kTraySupersample = 8;
constexpr DWORD kToastHoldMs = 850;
constexpr wchar_t kPersonalizeSettings[] = LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
using LayoutIconSet = std::array<HICON, static_cast<size_t>(TileLayout::COUNT)>;

// User choices read by LoadSettings. They survive Shutdown so a settings reload
// applies them to the next session.
struct TrayPreferences {
  bool showToasts = true;
  bool toastAtTop = false;
  bool clickOpensMenu = false;
  int toastOffsetXDip = 0;
  int toastOffsetYDip = 0;
  std::array<std::wstring, static_cast<size_t>(TileLayout::COUNT)> customIconPaths{};
};

// Windows, shell registration and display state owned between Initialize and
// Shutdown. Shutdown releases the native resources, then resets the whole
// session at once, so a later Initialize never inherits stale state.
struct TraySession {
  HWND trayWindow = nullptr;
  ATOM trayWindowClass = 0;
  UINT taskbarCreatedMessage = 0;
  bool iconAdded = false;
  bool systemLightTheme = false;
  TileLayout displayedLayout = TileLayout::COUNT;
  GUID displayedDesktopId{};
  bool haveDisplayedDesktopId = false;
  HMONITOR displayedMonitor = nullptr;
  NOTIFYICONDATAW notifyData{};
  LayoutIconSet icons{};
  int iconSize = 0;  // The icons' size in pixels (TrayIconSize when built).
};

static TrayPreferences g_preferences;
static TraySession g_session;

static const wchar_t* LayoutDisplayName(TileLayout layout) {
  const LayoutNames* names = FindLayoutNames(layout);
  return names ? names->display : L"Unknown";
}

static const wchar_t* LayoutTrayText(TileLayout layout) {
  const LayoutNames* names = FindLayoutNames(layout);
  return names ? names->trayIcon : L"??";
}

static const wchar_t* LayoutToastText(TileLayout layout) {
  const LayoutNames* names = FindLayoutNames(layout);
  return names ? names->toast : L"Unknown";
}

// The layout `key`'s workspace uses, or the one it will start with.
static TileLayout LayoutShownFor(const DesktopMonitorKey& key) {
  if (const std::optional<TileLayout> layout = g_workspaces.LayoutOf(key)) return *layout;
  return Reconcile::ResolveInitialWorkspaceLayout(key);
}

static void LoadSettings() {
  using WindhawkUtils::StringSetting;
  auto trayClickAction = StringSetting::make(L"general.TrayClickAction");
  g_preferences.clickOpensMenu = _wcsicmp(trayClickAction.get(), L"context_menu") == 0;
  g_preferences.showToasts = Wh_GetIntSetting(L"appearance.ShowToasts") != 0;
  auto position = StringSetting::make(L"appearance.FlyoutPosition");
  g_preferences.toastAtTop = _wcsicmp(position.get(), L"top") == 0;
  g_preferences.toastOffsetXDip = Wh_GetIntSetting(L"appearance.FlyoutOffsetX");
  g_preferences.toastOffsetYDip = Wh_GetIntSetting(L"appearance.FlyoutOffsetY");

  for (auto& path : g_preferences.customIconPaths) path.clear();
  for (int i = 0; i < 32; ++i) {
    auto layoutName =
        StringSetting::make(L"appearance.CustomIcons[%d].Layout", i);
    if (!*layoutName.get()) break;
    auto path = StringSetting::make(L"appearance.CustomIcons[%d].Path", i);
    size_t index = static_cast<size_t>(Configuration::ParseLayoutSetting(layoutName.get()));
    if (*path.get() && index < g_preferences.customIconPaths.size() &&
        g_preferences.customIconPaths[index].empty()) {
      g_preferences.customIconPaths[index] = path.get();
    }
  }
}

static bool ReadSystemLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(
          HKEY_CURRENT_USER,
          kPersonalizeSettings,
          L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
          &size) == ERROR_SUCCESS) {
    return value != 0;
  }
  COLORREF fallback = GetSysColor(COLOR_WINDOW);
  return GetRValue(fallback) + GetGValue(fallback) + GetBValue(fallback) >= 384;
}

static COLORREF ToastBackgroundColor() {
  return g_session.systemLightTheme ? RGB(243, 243, 243) : RGB(36, 36, 36);
}

// The colors of toasts and drag-preview badge cards for the current theme.
static Overlay::PopupAppearance CurrentPopupAppearance() {
  return {ToastBackgroundColor(), g_session.systemLightTheme};
}

// The tray's icon size: the small-icon size at the taskbar's DPI, so the shell
// shows the icon as drawn rather than resizing it.
static int TrayIconSize() {
  const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
  UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
  if (!dpi) dpi = Platform::Monitors::GetMonitorEffectiveDpi(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY));
  const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
  return size > 0 ? size : 16;
}

// How much of each pixel of a `size`-pixel square icon `text` covers, 0..255,
// row by row; empty on failure. GDI draws the text aliased on a canvas
// kTraySupersample times larger and each pixel averages its block, so the
// edges are smooth whatever the system's font smoothing (which GDI's own
// antialiasing follows; with it off, as in many VMs, the icon was pixel art).
static std::vector<BYTE> TextCoverage(const wchar_t* text, int size) {
  const int canvas = size * kTraySupersample;
  BITMAPINFO bmi{};
  bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), canvas, -canvas, 1, 32, BI_RGB};
  void* bits = nullptr;
  const HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  const HDC dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
  if (!bitmap || !dc || !bits) {
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    return {};
  }

  const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
  RECT rect{0, 0, canvas, canvas};
  FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  const HFONT font = CreateFontW(-MulDiv(kTrayFontHeight, canvas, kTrayTextDesignSize), 0, 0, 0, FW_BOLD, FALSE,
                                 FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
  const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);
  GdiFlush();

  std::vector<BYTE> coverage(static_cast<size_t>(size) * size);
  const auto* samples = static_cast<const uint32_t*>(bits);
  constexpr int kSamples = kTraySupersample * kTraySupersample;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      int covered = 0;
      for (int dy = 0; dy < kTraySupersample; ++dy) {
        const uint32_t* row = samples + static_cast<size_t>(y * kTraySupersample + dy) * canvas + x * kTraySupersample;
        for (int dx = 0; dx < kTraySupersample; ++dx) covered += (row[dx] & 0xFF) >= 128;
      }
      coverage[static_cast<size_t>(y) * size + x] = static_cast<BYTE>((covered * 255 + kSamples / 2) / kSamples);
    }
  }
  SelectObject(dc, oldBitmap);
  DeleteDC(dc);
  DeleteObject(bitmap);
  return coverage;
}

// `layout`'s two letters as a `size`-pixel icon in the theme's text color.
static HICON CreateTextIcon(TileLayout layout, int size) {
  const std::vector<BYTE> coverage = TextCoverage(LayoutTrayText(layout), size);
  if (coverage.empty()) return nullptr;
  BITMAPINFO bmi{};
  bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
  void* bits = nullptr;
  const HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color || !bits) {
    if (color) DeleteObject(color);
    return nullptr;
  }
  // Every pixel carries the text color, the transparent ones too, so the shell
  // can't blend a fringe of another color into the edges.
  const COLORREF fg = Overlay::CardTextColor(g_session.systemLightTheme);
  const uint32_t rgb = (GetRValue(fg) << 16) | (GetGValue(fg) << 8) | GetBValue(fg);
  auto* pixels = static_cast<uint32_t*>(bits);
  for (size_t i = 0; i < coverage.size(); ++i) pixels[i] = (static_cast<uint32_t>(coverage[i]) << 24) | rgb;

  const std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16) * 2 * size);  // Word-aligned rows.
  const HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
  ICONINFO info{TRUE, 0, 0, mask, color};
  const HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
  if (mask) DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

// Returns an owned `size`-pixel icon, preferring the configured file (its
// closest frame) and falling back to generated text. The icon must eventually
// be released with DestroyIcon.
static HICON CreateLayoutIcon(TileLayout layout, int size) {
  size_t index = static_cast<size_t>(layout);
  const auto& customIconPaths = g_preferences.customIconPaths;
  if (index < customIconPaths.size() && !customIconPaths[index].empty()) {
    HICON icon = static_cast<HICON>(LoadImageW(
        nullptr, customIconPaths[index].c_str(), IMAGE_ICON,
        size, size, LR_LOADFROMFILE));
    if (icon) return icon;
    Wh_Log(L"Failed to load custom tray icon, using text fallback: %s",
           customIconPaths[index].c_str());
  }
  return CreateTextIcon(layout, size);
}

static void DestroyIconSet(LayoutIconSet& icons) {
  for (HICON& icon : icons) {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
  }
}

static HICON GetLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_session.icons.size() && g_session.icons[index]) return g_session.icons[index];
  return LoadIconW(nullptr, IDI_APPLICATION);
}

static std::wstring MonitorTooltipLabel(HMONITOR monitor, size_t fallbackIndex) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (monitor && GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) {
    std::wstring device = info.szDevice;
    constexpr wchar_t kDevicePrefix[] = L"\\\\.\\";
    if (device.rfind(kDevicePrefix, 0) == 0) device.erase(0, 4);
    if (!device.empty()) return device;
  }

  wchar_t fallback[32]{};
  swprintf(fallback, ARRAYSIZE(fallback), L"Monitor %zu", fallbackIndex + 1);
  return fallback;
}

static void SetTooltip(TileLayout layout) {
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);

  // Preserve the compact historical tooltip exactly on a single-display system.
  // The richer summary is useful only when there are multiple local workspaces to
  // distinguish and would otherwise be pure visual clutter.
  std::wstring tooltip;
  if (monitors.size() <= 1) {
    tooltip = L"MultiWM - ";
    tooltip += LayoutDisplayName(layout);
  } else {
    GUID desktopId{};
    bool haveDesktop = RuntimeLifecycle::GetActiveDesktopId(&desktopId);
    if (!haveDesktop && g_session.haveDisplayedDesktopId) {
      desktopId = g_session.displayedDesktopId;
      haveDesktop = true;
    }

    if (!haveDesktop) {
      tooltip = L"MultiWM - ";
      tooltip += LayoutDisplayName(layout);
    } else {
      tooltip = L"MultiWM";
      for (size_t i = 0; i < monitors.size(); ++i) {
        const HMONITOR monitor = monitors[i];
        TileLayout monitorLayout = g_settings.defaultLayout;
        DesktopMonitorKey key{};
        if (DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) monitorLayout = LayoutShownFor(key);

        tooltip += L"\n";
        // Reserve the same visual marker column for every row. A figure space
        // is much closer to the triangle glyph width than two proportional spaces.
        tooltip += monitor == g_session.displayedMonitor ? L"\x25B8 " : L"\u2007 ";
        tooltip += MonitorTooltipLabel(monitor, i);
        tooltip += L": ";
        tooltip += LayoutToastText(monitorLayout);
      }
    }
  }

  wcsncpy(g_session.notifyData.szTip, tooltip.c_str(), ARRAYSIZE(g_session.notifyData.szTip) - 1);
  g_session.notifyData.szTip[ARRAYSIZE(g_session.notifyData.szTip) - 1] = L'\0';
}

static bool AddIconToShell() {
  if (!g_session.trayWindow) return false;
  if (g_session.displayedLayout == TileLayout::COUNT) {
    g_session.displayedLayout = g_settings.defaultLayout;
  }

  g_session.notifyData = {};
  g_session.notifyData.cbSize = sizeof(g_session.notifyData);
  g_session.notifyData.hWnd = g_session.trayWindow;
  g_session.notifyData.uID = kIconId;
  g_session.notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_session.notifyData.uCallbackMessage = kTrayCallbackMessage;
  g_session.notifyData.hIcon = GetLayoutIcon(g_session.displayedLayout);
  SetTooltip(g_session.displayedLayout);
  if (!Shell_NotifyIconW(NIM_ADD, &g_session.notifyData)) {
    g_session.iconAdded = false;
    Wh_Log(L"Tray icon NIM_ADD failed");
    return false;
  }

  g_session.iconAdded = true;
  g_session.notifyData.uVersion = NOTIFYICON_VERSION_4;
  if (!Shell_NotifyIconW(NIM_SETVERSION, &g_session.notifyData)) {
    Wh_Log(L"Tray icon NIM_SETVERSION failed after NIM_ADD");
  }
  return true;
}

static void UpdateIcon(TileLayout layout, HMONITOR monitor) {
  if (!g_session.trayWindow || layout == TileLayout::COUNT) return;

  const TileLayout previousLayout = g_session.displayedLayout;
  const HMONITOR previousMonitor = g_session.displayedMonitor;
  const HICON previousIcon = g_session.notifyData.hIcon;
  const std::wstring previousTooltip = g_session.notifyData.szTip;

  g_session.displayedLayout = layout;
  if (monitor) g_session.displayedMonitor = monitor;

  if (!g_session.iconAdded) {
    if (AddIconToShell()) return;

    // TaskbarCreated can be broadcast even when Explorer kept the existing tray
    // entry. In that state NIM_ADD fails because the icon already exists, so try
    // updating that surviving entry instead of permanently wedging on NIM_ADD.
    g_session.notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_session.notifyData.hIcon = GetLayoutIcon(layout);
    SetTooltip(layout);
    if (Shell_NotifyIconW(NIM_MODIFY, &g_session.notifyData)) {
      g_session.iconAdded = true;
      g_session.notifyData.uVersion = NOTIFYICON_VERSION_4;
      if (!Shell_NotifyIconW(NIM_SETVERSION, &g_session.notifyData)) {
        Wh_Log(L"Tray icon NIM_SETVERSION failed after NIM_MODIFY recovery");
      }
      Wh_Log(L"Tray icon recovered by NIM_MODIFY after NIM_ADD failed");
      return;
    }

    Wh_Log(L"Tray icon recovery failed: both NIM_ADD and NIM_MODIFY failed");
    return;
  }

  g_session.notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  g_session.notifyData.hIcon = GetLayoutIcon(layout);
  SetTooltip(layout);
  if (previousLayout == g_session.displayedLayout &&
      previousMonitor == g_session.displayedMonitor &&
      previousIcon == g_session.notifyData.hIcon &&
      previousTooltip == g_session.notifyData.szTip) {
    return;
  }
  if (!Shell_NotifyIconW(NIM_MODIFY, &g_session.notifyData)) {
    // The shell may have discarded the entry while our local state still says it
    // exists. Reverse the recovery direction and recreate it with NIM_ADD.
    Wh_Log(L"Tray icon NIM_MODIFY failed; trying NIM_ADD");
    g_session.iconAdded = false;
    if (!AddIconToShell()) {
      Wh_Log(L"Tray icon recovery failed after NIM_MODIFY");
    }
  }
}

// TaskbarCreated is a shell/taskbar lifecycle hint, not proof that the old tray
// entry vanished. Remove any surviving entry first, then rebuild from current
// model state. UpdateIcon still has ADD/MODIFY recovery for shell-state races.
static void RecreateIconInShell() {
  if (!g_session.trayWindow) return;

  NOTIFYICONDATAW identity{};
  identity.cbSize = sizeof(identity);
  identity.hWnd = g_session.trayWindow;
  identity.uID = kIconId;
  Shell_NotifyIconW(NIM_DELETE, &identity);

  g_session.iconAdded = false;
  if (g_session.displayedLayout == TileLayout::COUNT) {
    g_session.displayedLayout = g_settings.defaultLayout;
  }
  UpdateIcon(g_session.displayedLayout, g_session.displayedMonitor);
}

// Repoints the live tray entry to a complete new icon set before destroying the
// old handles, so the shell never observes an HICON after it has been released.
// Builds them at the tray's current size (TrayIconSize).
static void RebuildIcons() {
  const int size = TrayIconSize();
  LayoutIconSet icons{};
  for (size_t i = 0; i < icons.size(); ++i) {
    icons[i] = CreateLayoutIcon(static_cast<TileLayout>(i), size);
  }
  auto old = g_session.icons;
  g_session.icons = icons;
  g_session.iconSize = size;
  if (g_session.displayedLayout != TileLayout::COUNT) {
    UpdateIcon(g_session.displayedLayout, g_session.displayedMonitor);
  }
  DestroyIconSet(old);
}

// After a display, DPI or taskbar change: rebuilds the icons if the tray's
// icon size changed, so they stay unresized.
static void RebuildIconsIfResized() {
  if (TrayIconSize() != g_session.iconSize) RebuildIcons();
}

// Shows `text` on `monitor` (else the command monitor), with the icons of
// `apps`, the windows of the apps it names, one each, before it.
static void ShowStatusToast(const wchar_t* text, HMONITOR monitor, const std::vector<HWND>& apps = {}) {
  if ((!g_session.trayWindowClass && !g_session.trayWindow) || !g_preferences.showToasts || !text || !*text) return;
  if (!monitor) monitor = Platform::Monitors::GetWorkspaceCommandMonitor();
  if (!monitor) return;
  Overlay::ToastRequest request{monitor, text, CurrentPopupAppearance(), g_preferences.toastOffsetXDip,
                                g_preferences.toastOffsetYDip, g_preferences.toastAtTop};
  // Allow roughly 75 ms per character, retaining the minimum for short hints.
  request.holdMs = static_cast<DWORD>(std::max<size_t>(
      kToastHoldMs, std::min<size_t>(request.text.size(), 1000000) * 75));
  for (HWND hwnd : apps) {
    if (request.apps.size() == Overlay::kMaxToastIcons) break;
    DWORD processId = 0;
    if (const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId)) {
      request.apps.push_back({.window = hwnd, .processId = processId, .threadId = threadId});
    }
  }
  Overlay::PublishToast(std::move(request));
}

static void ShowLayoutToast(TileLayout layout, HMONITOR monitor) {
  if (layout == TileLayout::COUNT) return;
  ShowStatusToast(LayoutToastText(layout), monitor);
}

static void ShowCommandFailureToast(
    const wchar_t* text, HMONITOR monitor) {
  if (!text || !*text) return;
  ShowStatusToast(text, monitor);
}

// A toast about apps (Placement::AppToast): its text after their icons.
static void ShowAppToast(const std::wstring& text, const std::vector<HWND>& apps, HMONITOR monitor) {
  ShowStatusToast(text.c_str(), monitor, apps);
}

static void ShowManagementModeToast(bool automatic) {
  ShowStatusToast(
      automatic ? L"Automatic Mode" : L"Manual Mode",
      Platform::Monitors::GetWorkspaceCommandMonitor());
}

static void ShowDesktopSwitchToasts(const GUID& desktopId) {
  if (!g_preferences.showToasts) return;
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);
  for (HMONITOR monitor : monitors) {
    TileLayout layout = g_settings.defaultLayout;
    DesktopMonitorKey key{};
    if (DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key)) layout = LayoutShownFor(key);
    ShowLayoutToast(layout, monitor);
  }
}

// The monitor the tray icon currently shows, while it is connected; else the
// foreground window's, else the cursor's.
static HMONITOR GetDisplayedOrFocusedMonitor() {
  if (Platform::Monitors::IsLiveMonitorHandle(g_session.displayedMonitor)) return g_session.displayedMonitor;
  return Platform::Monitors::GetForegroundOrCursorMonitor();
}

static TileLayout GetMonitorLayoutOrDefault(HMONITOR monitor) {
  DesktopMonitorKey key{};
  if (monitor && Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) return LayoutShownFor(key);
  return g_settings.defaultLayout;
}

// TrackPopupMenuEx runs a modal loop which otherwise retrieves and discards
// hwnd-less actor messages. Process only messages owned by the WM and leave all
// menu/system traffic to the next hook and the menu loop.
static LRESULT CALLBACK TrayMenuMessageFilterProc(
    int code, WPARAM wParam, LPARAM lParam) {
  if (code == MSGF_MENU && lParam) {
    const MSG& msg = *reinterpret_cast<const MSG*>(lParam);
    if (!msg.hwnd &&
        RuntimeLifecycle::HandleWmThreadMessage(msg) == WmMessageDisposition::Handled) {
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

static void ShowContextMenu() {
  if (!g_session.trayWindow) return;

  const HMONITOR monitor = GetDisplayedOrFocusedMonitor();
  if (!monitor) return;

  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  AppendMenuW(menu, MF_STRING, kContextTileWorkspace, L"Tile Workspace");
  const ManagementMode targetMode = RuntimeLifecycle::IsAutomaticMode()
                                        ? ManagementMode::Manual
                                        : ManagementMode::Automatic;
  AppendMenuW(
      menu, MF_STRING, kContextToggleMode,
      targetMode == ManagementMode::Manual ? L"Switch to Manual Mode"
                                           : L"Switch to Automatic Mode");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  const TileLayout currentLayout = GetMonitorLayoutOrDefault(monitor);
  for (size_t i = 0; i < g_settings.layoutCycle.size(); ++i) {
    const TileLayout layout = g_settings.layoutCycle[i];
    const UINT command = kContextLayoutBase + static_cast<UINT>(i);
    UINT flags = MF_STRING;
    if (layout == currentLayout) {
      // Only the active layout carries a marker. Disable it as selecting the
      // current layout would be a no-op, and let the native menu render it grey.
      flags |= MF_CHECKED | MF_GRAYED;
    }
    AppendMenuW(menu, flags, command, LayoutDisplayName(layout));
  }

  POINT point{};
  if (!GetCursorPos(&point)) {
    DestroyMenu(menu);
    return;
  }

  HHOOK menuMessageHook = SetWindowsHookExW(
      WH_MSGFILTER, TrayMenuMessageFilterProc, nullptr, GetCurrentThreadId());
  if (!menuMessageHook) {
    Wh_Log(L"Failed to install tray menu message filter: %lu", GetLastError());
    DestroyMenu(menu);
    return;
  }

  // Required for reliable dismissal of notification-area popup menus.
  SetForegroundWindow(g_session.trayWindow);
  const UINT command = TrackPopupMenuEx(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
      point.x, point.y, g_session.trayWindow, nullptr);
  if (!UnhookWindowsHookEx(menuMessageHook)) {
    Wh_Log(L"Failed to remove tray menu message filter: %lu", GetLastError());
  }
  PostMessageW(g_session.trayWindow, WM_NULL, 0, 0);
  DestroyMenu(menu);

  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (!threadId || !command) return;

  if (command == kContextTileWorkspace) {
    PostThreadMessageW(
        threadId, WM_APP_TILE_WORKSPACE,
        reinterpret_cast<WPARAM>(monitor), 0);
    return;
  }

  if (command == kContextToggleMode) {
    PostThreadMessageW(
        threadId, WM_APP_MANAGEMENT_MODE_SET,
        static_cast<WPARAM>(targetMode), 0);
    return;
  }

  if (command >= kContextLayoutBase) {
    const size_t index = static_cast<size_t>(command - kContextLayoutBase);
    if (index < g_settings.layoutCycle.size()) {
      PostThreadMessageW(
          threadId, WM_APP_LAYOUT_SET,
          reinterpret_cast<WPARAM>(monitor),
          static_cast<LPARAM>(g_settings.layoutCycle[index]));
    }
  }
}

// Updates the single shell notification icon for one explicit monitor boundary.
// The icon is intentionally a view onto one workspace, while the tooltip can
// summarize all current monitor workspaces when multiple displays are connected.
static void RefreshForMonitor(HMONITOR monitor) {
  if (!monitor) return;

  TileLayout layout = g_settings.defaultLayout;
  DesktopMonitorKey key{};
  if (Platform::Monitors::GetCurrentWorkspaceKey(monitor, &key)) {
    layout = LayoutShownFor(key);
    g_session.displayedDesktopId = key.desktopId;
    g_session.haveDisplayedDesktopId = true;
  }
  UpdateIcon(layout, monitor);
}

static void RefreshDisplayedMonitor() {
  if (HMONITOR monitor = GetDisplayedOrFocusedMonitor()) RefreshForMonitor(monitor);
}

// Foreground changes deliberately retarget the one physical tray icon. Explicit
// workspace commands call RefreshForMonitor directly with their cursor-selected
// monitor instead of going through this focus-driven path.
static void RefreshForCurrentWorkspace() {
  if (HMONITOR monitor = Platform::Monitors::GetForegroundOrCursorMonitor()) RefreshForMonitor(monitor);
}

static void HandleThemeChanged() {
  const bool light = ReadSystemLightTheme();
  if (light == g_session.systemLightTheme) return;
  g_session.systemLightTheme = light;
  RebuildIcons();
  Overlay::RefreshAppearance(CurrentPopupAppearance());
}

static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
  if (g_session.taskbarCreatedMessage && message == g_session.taskbarCreatedMessage) {
    RebuildIconsIfResized();  // The new taskbar may run at another DPI.
    RecreateIconInShell();
    RuntimeLifecycle::NotifyShellRestarted();
    return 0;
  }

  if (message == kTrayCallbackMessage) {
    const UINT event = LOWORD(lParam);
    const UINT iconId = HIWORD(lParam);
    // NOTIFYICON_VERSION_4 reports the semantic context-menu activation as
    // WM_CONTEXTMENU. Do not also react to the legacy WM_RBUTTONUP notification:
    // one physical right-click can otherwise open two menus back-to-back, and the
    // second menu can appear before the first menu's asynchronous WM command is
    // consumed by the WM actor.
    if (iconId == kIconId && event == WM_CONTEXTMENU) {
      ShowContextMenu();
      return 0;
    }
    if (iconId == kIconId &&
        (event == NIN_SELECT || event == NIN_KEYSELECT)) {
      if (g_preferences.clickOpensMenu) {
        ShowContextMenu();
        return 0;
      }
      // Emit the exact same action as the layout hotkey; layout mutation stays
      // serialized on the WM thread and is implemented in one place only.
      const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
      if (threadId) {
        // The tray lives on the primary taskbar, but its icon represents the
        // active/last-targeted monitor. Preserve that semantic when clicked
        // instead of letting the cursor's primary-monitor location retarget it.
        PostThreadMessageW(
            threadId, WM_APP_LAYOUT_CYCLE,
            reinterpret_cast<WPARAM>(g_session.displayedMonitor), 0);
      }
    }
    return 0;
  }

  switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      RebuildIconsIfResized();
      RuntimeLifecycle::NotifyDisplayTopologyChanged();
      return 0;

    case WM_SETTINGCHANGE:
      HandleThemeChanged();
      if (wParam == SPI_SETWORKAREA) {
        RebuildIconsIfResized();
        RuntimeLifecycle::NotifyDisplayTopologyChanged();
      }
      return 0;

    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      HandleThemeChanged();
      return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

static bool Initialize() {
  g_session.systemLightTheme = ReadSystemLightTheme();
  RebuildIcons();

  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW trayClass{};
  trayClass.lpfnWndProc = TrayWindowProc;
  trayClass.hInstance = instance;
  trayClass.lpszClassName = kTrayWindowClassName;
  g_session.trayWindowClass = RegisterClassW(&trayClass);
  if (!g_session.trayWindowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  g_session.trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClassName, L"",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 instance, nullptr);
  if (!g_session.trayWindow) return false;
  g_session.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
  g_session.displayedLayout = g_settings.defaultLayout;
  RefreshForCurrentWorkspace();
  return g_session.iconAdded;
}

static void Shutdown() {
  if (g_session.iconAdded && g_session.trayWindow) {
    g_session.notifyData.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &g_session.notifyData);
  }
  g_session.iconAdded = false;
  if (g_session.trayWindow) {
    DestroyWindow(g_session.trayWindow);
    g_session.trayWindow = nullptr;
  }
  DestroyIconSet(g_session.icons);

  HINSTANCE instance = GetModuleHandleW(nullptr);
  // Unregister by name even after recovering from ERROR_CLASS_ALREADY_EXISTS,
  // where RegisterClassW didn't return an ATOM for us to remember.
  UnregisterClassW(kTrayWindowClassName, instance);
  g_session = {};
}

}  // namespace TrayUi

