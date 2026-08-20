//=============================================================================
// Notification-area layout indicator + layout flyout
//=============================================================================
//
// UI boundary for the tool mod. This subsystem is Win32/GDI-only: no Explorer
// hooks, XAML, WinUI, or Windows App SDK dependency. The tray uses two-letter
// mode abbreviations that are fitted to the full icon canvas. The flyout is a
// small non-activating card shown only after an explicit layout change.

namespace TrayUi {

//-----------------------------------------------------------------------------
// Settings + naming
//-----------------------------------------------------------------------------

constexpr wchar_t kTrayWindowClassName[] = L"WindhawkTilingHelperTrayWindow";
constexpr wchar_t kFlyoutWindowClassName[] = L"WindhawkTilingHelperStatusFlyout";
constexpr UINT kTrayCallbackMessage = WM_APP + 20;
constexpr UINT kIconId = 1;
constexpr UINT kContextTileWorkspace = 41001;
constexpr UINT kContextToggleMode = 41002;
constexpr UINT kContextLayoutBase = 41100;
constexpr int kIconCanvasSize = 32;
constexpr int kTrayFontHeight = 26;
constexpr UINT_PTR kFlyoutTimerId = 1;
constexpr DWORD kFlyoutHoldMs = 850;
constexpr DWORD kFlyoutFadeMs = 180;

static HWND g_trayWindow = nullptr;
static ATOM g_trayWindowClass = 0;
static ATOM g_flyoutWindowClass = 0;
static UINT g_taskbarCreatedMessage = 0;
static bool g_iconAdded = false;
static bool g_systemLightTheme = false;
static bool g_flyoutTop = false;
static int g_flyoutOffsetX = 0;
static int g_flyoutOffsetY = 0;
static TileLayout g_displayedLayout = TileLayout::COUNT;
static GUID g_displayedDesktopId{};
static bool g_haveDisplayedDesktopId = false;
static HMONITOR g_displayedMonitor = nullptr;

struct FlyoutInstance {
  HWND hwnd = nullptr;
  HMONITOR monitor = nullptr;
  std::wstring text;
  DWORD shownTick = 0;
};

static std::vector<FlyoutInstance*> g_flyouts;
static NOTIFYICONDATAW g_notifyData{};
static std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> g_icons{};
static std::array<std::wstring, static_cast<size_t>(TileLayout::COUNT)>
    g_customIconPaths{};

static const wchar_t* LayoutDisplayName(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"Master + Stack (Vertical)";
    case TileLayout::MasterStackH: return L"Master + Stack (Horizontal)";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    default: return L"Unknown";
  }
}

static const wchar_t* LayoutTrayText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"MV";
    case TileLayout::MasterStackH: return L"MH";
    case TileLayout::Columns: return L"CL";
    case TileLayout::Rows: return L"RW";
    case TileLayout::BSP: return L"BS";
    case TileLayout::Monocle: return L"MN";
    case TileLayout::Floating: return L"FL";
    default: return L"??";
  }
}

static const wchar_t* LayoutFlyoutText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"Master + Stack V";
    case TileLayout::MasterStackH: return L"Master + Stack H";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    default: return L"Unknown";
  }
}

static void LoadSettings() {
  using WindhawkUtils::StringSetting;
  auto position = StringSetting::make(L"FlyoutPosition");
  g_flyoutTop = _wcsicmp(position.get(), L"top") == 0;
  g_flyoutOffsetX = Wh_GetIntSetting(L"FlyoutOffsetX");
  g_flyoutOffsetY = Wh_GetIntSetting(L"FlyoutOffsetY");

  for (auto& path : g_customIconPaths) path.clear();
  for (int i = 0; i < 32; ++i) {
    auto layoutName = StringSetting::make(L"CustomIcons[%d].Layout", i);
    if (!*layoutName.get()) break;
    auto path = StringSetting::make(L"CustomIcons[%d].Path", i);
    size_t index = static_cast<size_t>(ParseLayoutSetting(layoutName.get()));
    if (*path.get() && index < g_customIconPaths.size() &&
        g_customIconPaths[index].empty()) {
      g_customIconPaths[index] = path.get();
    }
  }
}

//-----------------------------------------------------------------------------
// Theme-aware tray icons
//-----------------------------------------------------------------------------

static bool ReadSystemLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
          &size) == ERROR_SUCCESS) {
    return value != 0;
  }
  COLORREF fallback = GetSysColor(COLOR_WINDOW);
  return GetRValue(fallback) + GetGValue(fallback) + GetBValue(fallback) >= 384;
}

static COLORREF ForegroundColor() {
  return g_systemLightTheme ? RGB(24, 24, 24) : RGB(255, 255, 255);
}

static COLORREF FlyoutBackgroundColor() {
  return g_systemLightTheme ? RGB(243, 243, 243) : RGB(36, 36, 36);
}

static HICON CreateTextIcon(TileLayout layout) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = kIconCanvasSize;
  bmi.bmiHeader.biHeight = -kIconCanvasSize;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
  HDC dc = color ? CreateCompatibleDC(nullptr) : nullptr;
  if (!color || !dc || !bits) {
    if (dc) DeleteDC(dc);
    if (color) DeleteObject(color);
    return nullptr;
  }

  HGDIOBJ oldBitmap = SelectObject(dc, color);
  RECT rect{0, 0, kIconCanvasSize, kIconCanvasSize};
  FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  HFONT font = CreateFontW(-kTrayFontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  DrawTextW(dc, LayoutTrayText(layout), -1, &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);

  auto* pixels = static_cast<uint32_t*>(bits);
  COLORREF fg = ForegroundColor();
  uint32_t r = GetRValue(fg), g = GetGValue(fg), b = GetBValue(fg);
  for (int i = 0; i < kIconCanvasSize * kIconCanvasSize; ++i) {
    uint32_t p = pixels[i];
    uint32_t a = std::max({p & 0xFFu, (p >> 8) & 0xFFu,
                           (p >> 16) & 0xFFu});
    pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }

  SelectObject(dc, oldBitmap);
  DeleteDC(dc);

  constexpr int maskStride = ((kIconCanvasSize + 31) / 32) * 4;
  std::array<BYTE, maskStride * kIconCanvasSize> maskBits{};
  HBITMAP mask = CreateBitmap(kIconCanvasSize, kIconCanvasSize, 1, 1,
                              maskBits.data());
  ICONINFO info{TRUE, 0, 0, mask, color};
  HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
  if (mask) DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

// Returns an owned icon, preferring the configured file and falling back to
// generated text. The icon must eventually be released with DestroyIcon.
static HICON CreateLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_customIconPaths.size() && !g_customIconPaths[index].empty()) {
    HICON icon = static_cast<HICON>(LoadImageW(
        nullptr, g_customIconPaths[index].c_str(), IMAGE_ICON,
        kIconCanvasSize, kIconCanvasSize, LR_LOADFROMFILE));
    if (icon) return icon;
    Wh_Log(L"Failed to load custom tray icon, using text fallback: %s",
           g_customIconPaths[index].c_str());
  }
  return CreateTextIcon(layout);
}

static void DestroyIconSet(
    std::array<HICON, static_cast<size_t>(TileLayout::COUNT)>& icons) {
  for (HICON& icon : icons) {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
  }
}

static HICON GetLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_icons.size() && g_icons[index]) return g_icons[index];
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
    tooltip = L"Tiling Helper - ";
    tooltip += LayoutDisplayName(layout);
  } else {
    GUID desktopId{};
    bool haveDesktop = GetCurrentDesktopId(&desktopId);
    if (!haveDesktop && g_haveDisplayedDesktopId) {
      desktopId = g_displayedDesktopId;
      haveDesktop = true;
    }

    if (!haveDesktop) {
      tooltip = L"Tiling Helper - ";
      tooltip += LayoutDisplayName(layout);
    } else {
      tooltip = L"Tiling Helper";
      for (size_t i = 0; i < monitors.size(); ++i) {
        const HMONITOR monitor = monitors[i];
        TileLayout monitorLayout = g_settings.defaultLayout;
        Workspace state;
        DesktopMonitorKey key{};
        if (DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key) &&
            g_workspaces.Load(key, &state)) {
          monitorLayout = state.Layout();
        }

        tooltip += L"\n";
        // Reserve the same visual marker column for every row. A figure space
        // is much closer to the triangle glyph width than two proportional spaces.
        tooltip += monitor == g_displayedMonitor ? L"\x25B8 " : L"\u2007 ";
        tooltip += MonitorTooltipLabel(monitor, i);
        tooltip += L": ";
        tooltip += LayoutFlyoutText(monitorLayout);
      }
    }
  }

  wcsncpy(g_notifyData.szTip, tooltip.c_str(), ARRAYSIZE(g_notifyData.szTip) - 1);
  g_notifyData.szTip[ARRAYSIZE(g_notifyData.szTip) - 1] = L'\0';
}

static bool AddIconToShell() {
  if (!g_trayWindow) return false;
  if (g_displayedLayout == TileLayout::COUNT) g_displayedLayout = g_settings.defaultLayout;

  g_notifyData = {};
  g_notifyData.cbSize = sizeof(g_notifyData);
  g_notifyData.hWnd = g_trayWindow;
  g_notifyData.uID = kIconId;
  g_notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_notifyData.uCallbackMessage = kTrayCallbackMessage;
  g_notifyData.hIcon = GetLayoutIcon(g_displayedLayout);
  SetTooltip(g_displayedLayout);
  if (!Shell_NotifyIconW(NIM_ADD, &g_notifyData)) {
    g_iconAdded = false;
    return false;
  }
  g_iconAdded = true;
  g_notifyData.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_notifyData);
  return true;
}

static void UpdateIcon(TileLayout layout, HMONITOR monitor) {
  if (!g_trayWindow || layout == TileLayout::COUNT) return;
  g_displayedLayout = layout;
  if (monitor) g_displayedMonitor = monitor;
  if (!g_iconAdded) {
    AddIconToShell();
    return;
  }
  g_notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  g_notifyData.hIcon = GetLayoutIcon(layout);
  SetTooltip(layout);
  if (!Shell_NotifyIconW(NIM_MODIFY, &g_notifyData)) {
    g_iconAdded = false;
    AddIconToShell();
  }
}

// Repoints the live tray entry to a complete new icon set before destroying the
// old handles, so the shell never observes an HICON after it has been released.
static void RebuildIcons() {
  std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> icons{};
  for (size_t i = 0; i < icons.size(); ++i) {
    icons[i] = CreateLayoutIcon(static_cast<TileLayout>(i));
  }
  auto old = g_icons;
  g_icons = icons;
  if (g_displayedLayout != TileLayout::COUNT) UpdateIcon(g_displayedLayout, g_displayedMonitor);
  DestroyIconSet(old);
}

//-----------------------------------------------------------------------------
// Compact layout flyout
//-----------------------------------------------------------------------------

static int ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

static HFONT CreateFlyoutFont(UINT dpi) {
  return CreateFontW(-ScaleForDpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static FlyoutInstance* GetFlyoutInstance(HWND hwnd) {
  return reinterpret_cast<FlyoutInstance*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static LRESULT CALLBACK FlyoutWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    auto* instance = create
        ? reinterpret_cast<FlyoutInstance*>(create->lpCreateParams)
        : nullptr;
    if (instance) {
      instance->hwnd = hwnd;
      SetWindowLongPtrW(
          hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    }
  }

  FlyoutInstance* instance = GetFlyoutInstance(hwnd);
  switch (message) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      HBRUSH background = CreateSolidBrush(FlyoutBackgroundColor());
      FillRect(dc, &client, background);
      DeleteObject(background);

      HFONT font = CreateFlyoutFont(GetDpiForWindow(hwnd));
      HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, ForegroundColor());
      const wchar_t* text = instance ? instance->text.c_str() : L"";
      DrawTextW(dc, text, -1, &client,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (oldFont) SelectObject(dc, oldFont);
      if (font) DeleteObject(font);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wParam == kFlyoutTimerId && instance) {
        DWORD elapsed = GetTickCount() - instance->shownTick;
        if (elapsed >= kFlyoutHoldMs + kFlyoutFadeMs) {
          KillTimer(hwnd, kFlyoutTimerId);
          ShowWindow(hwnd, SW_HIDE);
          return 0;
        }
        BYTE alpha = 255;
        if (elapsed > kFlyoutHoldMs) {
          alpha = static_cast<BYTE>(255 - std::min<DWORD>(
              255, ((elapsed - kFlyoutHoldMs) * 255) / kFlyoutFadeMs));
        }
        SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
        return 0;
      }
      break;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

static FlyoutInstance* FindFlyoutForMonitor(HMONITOR monitor) {
  for (FlyoutInstance* instance : g_flyouts) {
    if (instance && instance->monitor == monitor && instance->hwnd) {
      return instance;
    }
  }
  return nullptr;
}

static FlyoutInstance* GetOrCreateFlyoutForMonitor(HMONITOR monitor) {
  if (!monitor) return nullptr;
  if (FlyoutInstance* existing = FindFlyoutForMonitor(monitor)) return existing;

  auto* instance = new (std::nothrow) FlyoutInstance;
  if (!instance) return nullptr;
  instance->monitor = monitor;

  HINSTANCE module = GetModuleHandleW(nullptr);
  instance->hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      kFlyoutWindowClassName, L"", WS_POPUP, 0, 0, 0, 0,
      nullptr, nullptr, module, instance);
  if (!instance->hwnd) {
    delete instance;
    return nullptr;
  }

  g_flyouts.push_back(instance);
  return instance;
}

static void ShowStatusFlyout(const wchar_t* text, HMONITOR monitor) {
  if (!text || !*text) return;
  if (!monitor) monitor = GetWorkspaceCommandMonitor();
  RECT workArea{};
  if (!monitor || !GetMonitorWorkArea(monitor, &workArea)) return;

  FlyoutInstance* instance = GetOrCreateFlyoutForMonitor(monitor);
  if (!instance || !instance->hwnd) return;
  instance->text = text;

  UINT dpi = GetMonitorEffectiveDpi(monitor);
  HFONT font = CreateFlyoutFont(dpi);
  HDC dc = GetDC(instance->hwnd);
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SIZE textSize{};
  GetTextExtentPoint32W(dc, instance->text.c_str(),
                        static_cast<int>(instance->text.size()), &textSize);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);
  ReleaseDC(instance->hwnd, dc);

  int width = std::max(ScaleForDpi(128, dpi),
                       static_cast<int>(textSize.cx) + ScaleForDpi(48, dpi));
  int height = ScaleForDpi(48, dpi);
  int x = workArea.left + ((workArea.right - workArea.left) - width) / 2 +
          ScaleForDpi(g_flyoutOffsetX, dpi);
  int edgeGap = ScaleForDpi(16, dpi);
  int y = g_flyoutTop ? workArea.top + edgeGap
                      : workArea.bottom - height - edgeGap;
  y += ScaleForDpi(g_flyoutOffsetY, dpi);

  int radius = ScaleForDpi(8, dpi);
  HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                   radius * 2, radius * 2);
  if (region && !SetWindowRgn(instance->hwnd, region, TRUE)) DeleteObject(region);

  instance->shownTick = GetTickCount();
  SetLayeredWindowAttributes(instance->hwnd, 0, 255, LWA_ALPHA);
  SetWindowPos(instance->hwnd, HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(instance->hwnd, nullptr, TRUE);
  UpdateWindow(instance->hwnd);
  if (!SetTimer(instance->hwnd, kFlyoutTimerId, 16, nullptr)) {
    // A cosmetic timer failure must not leave a permanently visible overlay.
    ShowWindow(instance->hwnd, SW_HIDE);
  }
}

static void ShowLayoutFlyout(TileLayout layout, HMONITOR monitor) {
  if (layout == TileLayout::COUNT) return;
  ShowStatusFlyout(LayoutFlyoutText(layout), monitor);
}

static void ShowManagementModeFlyout(bool automatic) {
  ShowStatusFlyout(
      automatic ? L"Automatic Mode" : L"Manual Mode",
      GetWorkspaceCommandMonitor());
}

static void ShowDesktopSwitchFlyouts(const GUID& desktopId) {
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);
  for (HMONITOR monitor : monitors) {
    TileLayout layout = g_settings.defaultLayout;
    Workspace state;
    DesktopMonitorKey key{};
    if (DesktopMonitorKey::FromHMonitor(desktopId, monitor, &key) &&
        g_workspaces.Load(key, &state)) {
      layout = state.Layout();
    }
    ShowLayoutFlyout(layout, monitor);
  }
}

//-----------------------------------------------------------------------------
// Standard tray context menu
//-----------------------------------------------------------------------------

static HMONITOR GetContextMenuMonitor() {
  MONITORINFO info{sizeof(info)};
  if (g_displayedMonitor && GetMonitorInfoW(g_displayedMonitor, &info)) {
    return g_displayedMonitor;
  }

  HMONITOR monitor = GetForegroundMonitor();
  if (!monitor) monitor = GetCursorMonitor();
  return monitor;
}

static TileLayout GetMonitorLayoutOrDefault(HMONITOR monitor) {
  TileLayout layout = g_settings.defaultLayout;
  DesktopMonitorKey key{};
  Workspace state;
  if (monitor && GetCurrentWorkspaceKey(monitor, &key) &&
      g_workspaces.Load(key, &state)) {
    layout = state.Layout();
  }
  return layout;
}

static void ShowContextMenu() {
  if (!g_trayWindow) return;

  const HMONITOR monitor = GetContextMenuMonitor();
  if (!monitor) return;

  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  AppendMenuW(menu, MF_STRING, kContextTileWorkspace, L"Tile Workspace");
  AppendMenuW(
      menu, MF_STRING, kContextToggleMode,
      IsAutomaticMode() ? L"Switch to Manual Mode" : L"Switch to Automatic Mode");

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

  // Required for reliable dismissal of notification-area popup menus.
  SetForegroundWindow(g_trayWindow);
  const UINT command = TrackPopupMenuEx(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
      point.x, point.y, g_trayWindow, nullptr);
  PostMessageW(g_trayWindow, WM_NULL, 0, 0);
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
    const ManagementMode mode = IsAutomaticMode()
                                    ? ManagementMode::Manual
                                    : ManagementMode::Automatic;
    PostThreadMessageW(
        threadId, WM_APP_MANAGEMENT_MODE_SET,
        static_cast<WPARAM>(mode), 0);
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

//-----------------------------------------------------------------------------
// Lifecycle / shell integration
//-----------------------------------------------------------------------------

// Updates the single shell notification icon for one explicit monitor boundary.
// The icon is intentionally a view onto one workspace, while the tooltip can
// summarize all current monitor workspaces when multiple displays are connected.
static void RefreshForMonitor(HMONITOR monitor) {
  if (!monitor) return;

  TileLayout layout = g_settings.defaultLayout;
  DesktopMonitorKey key{};
  Workspace state;
  const bool haveKey = GetCurrentWorkspaceKey(monitor, &key);
  if (haveKey && g_workspaces.Load(key, &state)) layout = state.Layout();

  if (haveKey) {
    g_displayedDesktopId = key.desktopId;
    g_haveDisplayedDesktopId = true;
  }
  UpdateIcon(layout, monitor);
}

static void RefreshDisplayedMonitor() {
  HMONITOR monitor = g_displayedMonitor;
  MONITORINFO info{sizeof(info)};
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    monitor = GetForegroundMonitor();
    if (!monitor) monitor = GetCursorMonitor();
  }
  if (monitor) RefreshForMonitor(monitor);
}

// Foreground changes deliberately retarget the one physical tray icon. Explicit
// workspace commands call RefreshForMonitor directly with their cursor-selected
// monitor instead of going through this focus-driven path.
static void RefreshForCurrentWorkspace() {
  HMONITOR monitor = GetForegroundMonitor();
  if (!monitor) monitor = GetCursorMonitor();
  if (monitor) RefreshForMonitor(monitor);
}

static void HandleThemeChanged() {
  bool light = ReadSystemLightTheme();
  if (light == g_systemLightTheme) return;
  g_systemLightTheme = light;
  RebuildIcons();
  for (FlyoutInstance* instance : g_flyouts) {
    if (instance && instance->hwnd && IsWindowVisible(instance->hwnd)) {
      InvalidateRect(instance->hwnd, nullptr, TRUE);
    }
  }
}

static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
  if (g_taskbarCreatedMessage && message == g_taskbarCreatedMessage) {
    g_iconAdded = false;
    AddIconToShell();
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
      // Emit the exact same action as the layout hotkey; layout mutation stays
      // serialized on the WM thread and is implemented in one place only.
      const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
      if (threadId) {
        // The tray lives on the primary taskbar, but its icon represents the
        // active/last-targeted monitor. Preserve that semantic when clicked
        // instead of letting the cursor's primary-monitor location retarget it.
        PostThreadMessageW(
            threadId, WM_APP_LAYOUT_CYCLE,
            reinterpret_cast<WPARAM>(g_displayedMonitor), 0);
      }
    }
    return 0;
  }

  switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      RuntimeLifecycle::NotifyDisplayTopologyChanged();
      return 0;

    case WM_SETTINGCHANGE:
      HandleThemeChanged();
      if (wParam == SPI_SETWORKAREA) {
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
  g_systemLightTheme = ReadSystemLightTheme();
  RebuildIcons();

  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW trayClass{};
  trayClass.lpfnWndProc = TrayWindowProc;
  trayClass.hInstance = instance;
  trayClass.lpszClassName = kTrayWindowClassName;
  g_trayWindowClass = RegisterClassW(&trayClass);
  if (!g_trayWindowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  WNDCLASSW flyoutClass{};
  flyoutClass.lpfnWndProc = FlyoutWindowProc;
  flyoutClass.hInstance = instance;
  flyoutClass.lpszClassName = kFlyoutWindowClassName;
  g_flyoutWindowClass = RegisterClassW(&flyoutClass);
  const bool haveFlyoutClass = g_flyoutWindowClass != 0 ||
                               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

  g_trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClassName, L"",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 instance, nullptr);
  if (!g_trayWindow) return false;

  if (!haveFlyoutClass) {
    Wh_Log(L"Status flyout window class unavailable; tray indicator remains active");
  }

  g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
  g_displayedLayout = g_settings.defaultLayout;
  RefreshForCurrentWorkspace();
  return g_iconAdded;
}

static void Shutdown() {
  if (g_iconAdded && g_trayWindow) {
    g_notifyData.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &g_notifyData);
  }
  g_iconAdded = false;
  for (FlyoutInstance* instance : g_flyouts) {
    if (!instance) continue;
    if (instance->hwnd) {
      KillTimer(instance->hwnd, kFlyoutTimerId);
      DestroyWindow(instance->hwnd);
      instance->hwnd = nullptr;
    }
    delete instance;
  }
  g_flyouts.clear();
  if (g_trayWindow) {
    DestroyWindow(g_trayWindow);
    g_trayWindow = nullptr;
  }
  DestroyIconSet(g_icons);

  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (g_flyoutWindowClass) UnregisterClassW(kFlyoutWindowClassName, instance);
  if (g_trayWindowClass) UnregisterClassW(kTrayWindowClassName, instance);
  g_flyoutWindowClass = g_trayWindowClass = 0;
  g_taskbarCreatedMessage = 0;
  g_displayedLayout = TileLayout::COUNT;
  g_displayedDesktopId = {};
  g_haveDisplayedDesktopId = false;
  g_displayedMonitor = nullptr;
  g_notifyData = {};
}

}  // namespace TrayUi


namespace Diagnostics {

