// Native monitor identity, DPI and workspace metrics.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Platform::Monitors {

static void ClearMonitorIdentityCache() {
  g_monitorIdentityCache.clear();
}

static bool IsLiveMonitorHandle(HMONITOR monitor) {
  if (!monitor) return false;
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  return GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
}


bool GetMonitorWorkArea(HMONITOR monitor, RECT* outRect) {
  if (!monitor || !outRect) return false;
  MONITORINFO monitorInfo = {sizeof(monitorInfo)};
  if (!GetMonitorInfoW(monitor, &monitorInfo)) return false;
  *outRect = monitorInfo.rcWork;
  return true;
}

static UINT GetMonitorEffectiveDpi(HMONITOR monitor) {
  if (!monitor || !IsLiveMonitorHandle(monitor)) return 96;

  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  using GetScaleFactorForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int*);
  struct DpiApis {
    GetDpiForMonitorFn getDpi = nullptr;
    GetScaleFactorForMonitorFn getScale = nullptr;
  };
  static const DpiApis apis = [] {
    DpiApis result;
    HMODULE shcore = LoadLibraryExW(
        L"Shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore) {
      result.getDpi = reinterpret_cast<GetDpiForMonitorFn>(
          GetProcAddress(shcore, "GetDpiForMonitor"));
      result.getScale = reinterpret_cast<GetScaleFactorForMonitorFn>(
          GetProcAddress(shcore, "GetScaleFactorForMonitor"));
    }
    return result;
  }();

  UINT dpiX = 96;
  UINT dpiY = 96;
  // MDT_EFFECTIVE_DPI == 0. Keep the enum types local so no Shcore import
  // library is required. DPI is observed fresh at the operation boundary.
  const bool haveDpi = apis.getDpi &&
      SUCCEEDED(apis.getDpi(monitor, 0, &dpiX, &dpiY)) && dpiX;

  // Also correct a virtualized 96-DPI result when the host process awareness
  // differs from this per-monitor-aware WM thread.
  int scalePercent = 100;
  const bool haveScale = apis.getScale &&
      SUCCEEDED(apis.getScale(monitor, &scalePercent)) && scalePercent > 0;
  if (haveScale && (!haveDpi || (dpiX == 96 && scalePercent != 100))) {
    return static_cast<UINT>(MulDiv(96, scalePercent, 100));
  }
  return haveDpi ? dpiX : 96;
}

static LONG ScaleDip(LONG value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

// Monitor targeting policy is intentionally explicit:
// - workspace-level user commands follow the cursor, because this mod doesn't
//   maintain a separate WM "focused monitor" state;
// - window-level commands derive ownership from the focused HWND itself;
// - passive UI refreshes use the foreground window's monitor.
static HMONITOR GetCursorMonitor() {
  POINT cursorPos{};
  if (!GetCursorPos(&cursorPos)) return nullptr;
  return MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
}

static HMONITOR GetForegroundMonitor() {
  HWND foregroundWindow = GetForegroundWindow();
  return foregroundWindow ? Platform::Windows::GetWindowPhysicalMonitor(foregroundWindow) : nullptr;
}

static HMONITOR GetWorkspaceCommandMonitor() {
  HMONITOR monitor = GetCursorMonitor();
  return monitor ? monitor : GetForegroundMonitor();
}

// Resolves a monitor on the current virtual desktop to its workspace key.
static bool GetCurrentWorkspaceKey(
    HMONITOR monitor, DesktopMonitorKey* outKey) {
  if (!outKey || !IsLiveMonitorHandle(monitor)) return false;
  GUID desktopId{};
  if (g_wm.reconciledDesktopState == ReconciledDesktopState::Settled) {
    desktopId = g_wm.reconciledDesktop;
  } else {
    // Never collapse separate desktops into a zero-GUID workspace when the
    // private API is temporarily unavailable; let the caller retry after recovery.
    if (!Platform::VirtualDesktop::InitializeVirtualDesktopAPI() || !Platform::VirtualDesktop::GetCurrentDesktopId(&desktopId)) {
      return false;
    }
  }
  return DesktopMonitorKey::FromHMonitor(desktopId, monitor, outKey);
}

struct WorkspaceMetrics {
  RECT workArea{};
  UINT dpi = 96;
  LONG gap = 0;
};

static bool GetWorkspaceMetrics(HMONITOR monitor, WorkspaceMetrics* out) {
  if (!monitor || !out) return false;

  RECT monitorWork{};
  if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;

  const UINT dpi = GetMonitorEffectiveDpi(monitor);
  const WorkspaceInsets insets{
      ScaleDip(g_settings.insetsDip.left, dpi),
      ScaleDip(g_settings.insetsDip.top, dpi),
      ScaleDip(g_settings.insetsDip.right, dpi),
      ScaleDip(g_settings.insetsDip.bottom, dpi)};
  const RECT workArea{
      monitorWork.left + insets.left,
      monitorWork.top + insets.top,
      monitorWork.right - insets.right,
      monitorWork.bottom - insets.bottom};
  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) {
    return false;
  }

  out->workArea = workArea;
  out->dpi = dpi;
  out->gap = ScaleDip(g_settings.gapDip, dpi);
  return true;
}

static bool GetWorkspaceWorkArea(HMONITOR monitor, RECT* outWorkArea) {
  if (!outWorkArea) return false;
  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return false;
  *outWorkArea = metrics.workArea;
  return true;
}

} // namespace Platform::Monitors

namespace Model {

bool MonitorId::FromHMonitor(HMONITOR monitor, MonitorId* out) {
  if (!out || !Platform::Monitors::IsLiveMonitorHandle(monitor)) return false;

  auto cached = std::find_if(
      Platform::Monitors::g_monitorIdentityCache.begin(), Platform::Monitors::g_monitorIdentityCache.end(),
      [&](const Platform::Monitors::MonitorIdentityCacheEntry& entry) {
        return entry.monitor == monitor;
      });
  if (cached != Platform::Monitors::g_monitorIdentityCache.end()) {
    *out = cached->id;
    return true;
  }

  MONITORINFOEXW monitorInfo{};
  monitorInfo.cbSize = sizeof(monitorInfo);
  if (!GetMonitorInfoW(
          monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo))) {
    return false;
  }

  // This is the same stable display-interface identity source used by FancyWM:
  // map the current GDI display name (\\.\DISPLAYn) to the monitor device
  // interface exposed by EnumDisplayDevices + EDD_GET_DEVICE_INTERFACE_NAME.
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  std::wstring identity;
  if (EnumDisplayDevicesW(
          monitorInfo.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) &&
      device.DeviceID[0]) {
    identity = L"IFACE:";
    identity += device.DeviceID;
  } else if (monitorInfo.szDevice[0]) {
    // Compatibility fallback only. Normal physical monitors should have an
    // interface ID; retaining the GDI name is preferable to disabling tiling on
    // unusual virtual/RDP display providers that don't expose one.
    identity = L"GDI:";
    identity += monitorInfo.szDevice;
  } else {
    return false;
  }

  Model::NormalizeMonitorIdentityToken(&identity);
  out->deviceId = std::move(identity);
  Platform::Monitors::g_monitorIdentityCache.push_back({monitor, *out});
  return true;
}

HMONITOR MonitorId::Resolve() const {
  if (deviceId.empty()) return nullptr;

  for (const auto& entry : Platform::Monitors::g_monitorIdentityCache) {
    if (entry.id == *this && Platform::Monitors::IsLiveMonitorHandle(entry.monitor)) {
      return entry.monitor;
    }
  }

  struct ResolveContext {
    const MonitorId* target = nullptr;
    HMONITOR result = nullptr;
  } context{this, nullptr};

  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM lParam) WINAPI -> BOOL {
        auto* context = reinterpret_cast<ResolveContext*>(lParam);
        MonitorId candidate;
        if (MonitorId::FromHMonitor(monitor, &candidate) &&
            candidate == *context->target) {
          context->result = monitor;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return context.result;
}

std::vector<HMONITOR> WorkspaceRepository::MonitorsOnDesktop(
    const GUID& desktopId) const {
  std::vector<MonitorId> monitorIds;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    if (!IsEqualGUID(kv.first.desktopId, desktopId)) continue;
    if (std::find(monitorIds.begin(), monitorIds.end(), kv.first.monitor) ==
        monitorIds.end()) {
      monitorIds.push_back(kv.first.monitor);
    }
  }
  ReleaseSRWLockShared(&lock_);

  // Repository identity survives topology churn. Only return identities that
  // currently resolve to a live HMONITOR; detached workspaces remain dormant.
  std::vector<HMONITOR> monitors;
  for (const auto& monitorId : monitorIds) {
    HMONITOR monitor = monitorId.Resolve();
    if (monitor && std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
      monitors.push_back(monitor);
    }
  }
  return monitors;
}

} // namespace Model
