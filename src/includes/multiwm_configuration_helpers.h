//=============================================================================
// Configuration parsing + shared core helpers
//=============================================================================

UINT ParseModifiers(PCWSTR str) {
  UINT modifiers = 0;
  if (wcsstr(str, L"alt")) modifiers |= MOD_ALT;
  if (wcsstr(str, L"ctrl")) modifiers |= MOD_CONTROL;
  if (wcsstr(str, L"shift")) modifiers |= MOD_SHIFT;
  return modifiers;
}

static bool TryParseLayoutSetting(PCWSTR str, TileLayout* outLayout) {
  if (!str || !outLayout) return false;
  static const std::pair<PCWSTR, TileLayout> kLayoutMap[] = {
      {L"master_stack", TileLayout::MasterStack},
      {L"master_stack_h", TileLayout::MasterStackH},
      {L"columns", TileLayout::Columns},
      {L"rows", TileLayout::Rows},
      {L"bsp", TileLayout::BSP},
      {L"monocle", TileLayout::Monocle},
      {L"floating", TileLayout::Floating},
  };
  for (const auto& entry : kLayoutMap) {
    if (wcscmp(str, entry.first) == 0) {
      *outLayout = entry.second;
      return true;
    }
  }
  return false;
}

TileLayout ParseLayoutSetting(PCWSTR str) {
  TileLayout layout = TileLayout::MasterStack;
  TryParseLayoutSetting(str, &layout);
  return layout;
}

template <typename T, typename Parser>
T ReadHotkeySetting(PCWSTR name, Parser parser) {
  auto value = WindhawkUtils::StringSetting::make(name);
  if (!*value.get()) return T{};  // Blank hotkey = unused.

  T result = parser(value.get());
  if (!result) {
    Wh_Log(
        L"Invalid %ls hotkey setting '%ls'; hotkey disabled", name,
        value.get());
  }
  return result;
}

UINT ReadModifierSetting(PCWSTR name, UINT defaultVal) {
  auto value = WindhawkUtils::StringSetting::make(name);
  UINT result = ParseModifiers(value.get());
  return result ? result : defaultVal;
}

inline double ClampDouble(double value, double minVal, double maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}

bool GetWindowFrameRect(HWND hwnd, RECT* outRect) {
  if (!hwnd || !outRect) return false;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, outRect, sizeof(*outRect)))) {
    return true;
  }
  return GetWindowRect(hwnd, outRect) != FALSE;
}

// One authoritative HWND -> physical-monitor observation for routing workspace
// ownership. Restored/maximized windows prefer the DWM frame so every caller uses
// the same monitor-boundary rule. Minimized windows deliberately fall back to
// MonitorFromWindow(), which Windows resolves from the pre-minimize rectangle.
static HMONITOR GetWindowPhysicalMonitor(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  if (!IsIconic(hwnd)) {
    RECT frame{};
    if (GetWindowFrameRect(hwnd, &frame) &&
        frame.right > frame.left && frame.bottom > frame.top) {
      HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONULL);
      if (monitor) return monitor;
    }
  }

  return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
}

static bool IsLiveMonitorHandle(HMONITOR monitor) {
  if (!monitor) return false;
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  return GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
}

static void NormalizeMonitorIdentityToken(std::wstring* value) {
  if (!value) return;
  std::transform(
      value->begin(), value->end(), value->begin(),
      [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
}

bool Model::MonitorId::FromHMonitor(HMONITOR monitor, MonitorId* out) {
  if (!out || !IsLiveMonitorHandle(monitor)) return false;

  auto cached = std::find_if(
      g_monitorIdentityCache.begin(), g_monitorIdentityCache.end(),
      [&](const MonitorIdentityCacheEntry& entry) {
        return entry.monitor == monitor;
      });
  if (cached != g_monitorIdentityCache.end()) {
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

  NormalizeMonitorIdentityToken(&identity);
  out->deviceId = std::move(identity);
  g_monitorIdentityCache.push_back({monitor, *out});
  return true;
}

HMONITOR Model::MonitorId::Resolve() const {
  if (deviceId.empty()) return nullptr;

  for (const auto& entry : g_monitorIdentityCache) {
    if (entry.id == *this && IsLiveMonitorHandle(entry.monitor)) {
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

std::vector<double> DefaultWeights(size_t count) {
  return std::vector<double>(count, 1.0);
}

static HMONITOR GetCurrentManagedWindowMonitor(HWND hwnd);
static SuspensionReason GetPhysicalSuspensionReason(HWND hwnd);
static bool WindowCanBeManaged(HWND hwnd);
static bool IsWindowTrackedInAnyState(HWND hwnd);
bool ContainsWindow(const std::vector<HWND>& windows, HWND hwnd) {
  for (HWND w : windows) {
    if (w == hwnd) return true;
  }
  return false;
}

bool Workspace::HasRecord(HWND hwnd) const {
  return hwnd && records_.find(hwnd) != records_.end();
}

const WindowRecord* Workspace::Find(HWND hwnd) const {
  auto it = records_.find(hwnd);
  return it == records_.end() ? nullptr : &it->second;
}

WindowRecord* Workspace::FindMutable(HWND hwnd) {
  auto it = records_.find(hwnd);
  return it == records_.end() ? nullptr : &it->second;
}

bool Workspace::IsTiled(HWND hwnd) const {
  const WindowRecord* record = Find(hwnd);
  return record && record->state == ManageState::Tiled &&
         std::find(windows_.begin(), windows_.end(), hwnd) != windows_.end();
}

size_t Workspace::TiledIndex(HWND hwnd) const {
  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  return it == windows_.end() ? static_cast<size_t>(-1)
                              : static_cast<size_t>(it - windows_.begin());
}

size_t Workspace::LogicalIndex(HWND hwnd) const {
  const WindowRecord* record = Find(hwnd);
  if (!record) return static_cast<size_t>(-1);
  if (record->state == ManageState::Suspended && record->hasSavedSlot) {
    return record->savedSlot.index;
  }
  if (record->state == ManageState::Tiled) {
    const size_t activeIndex = TiledIndex(hwnd);
    if (activeIndex != static_cast<size_t>(-1)) {
      return ActiveIndexToLogicalIndex(activeIndex);
    }
  }
  return static_cast<size_t>(-1);
}

bool Workspace::RememberFocusedWindow(HWND hwnd) {
  AssertWmThread(L"Workspace::RememberFocusedWindow");
  if (!hwnd || !HasRecord(hwnd) || lastFocusedWindow_ == hwnd) return false;
  lastFocusedWindow_ = hwnd;
  return true;
}

bool Workspace::HasSuspended() const {
  return std::any_of(
      records_.begin(), records_.end(), [](const auto& kv) {
        return kv.second.state == ManageState::Suspended;
      });
}

bool Workspace::AllTiledVisible(const std::vector<HWND>& snapshot) const {
  return std::all_of(
      windows_.begin(), windows_.end(),
      [&](HWND hwnd) { return ContainsWindow(snapshot, hwnd); });
}

WindowRecord& Workspace::UpsertRecord(WindowRecord record) {
  AssertWmThread(L"Workspace::UpsertRecord");
  const HWND hwnd = record.hwnd;
  auto [it, inserted] = records_.emplace(hwnd, std::move(record));
  if (!inserted) {
    // Preserve accumulated runtime bookkeeping when merely refreshing observed
    // capabilities/identity for an already-known record.
    WindowRecord& existing = it->second;
    const DWORD pid = it->second.pid;
    const PlacementResult lastResult = existing.lastPlacementResult;
    const RECT lastRequested = existing.lastRequestedRect;
    const RECT lastObserved = existing.lastObservedRect;
    const RECT floatingRect = existing.floatingRect;
    const HMONITOR floatingMonitor = existing.floatingMonitor;
    const UINT floatingDpi = existing.floatingDpi;
    const bool hasFloatingRect = existing.hasFloatingRect;
    const ManageState state = existing.state;
    const SuspensionReason suspension = existing.suspensionReason;
    const SuspendedSlot slot = existing.savedSlot;
    const bool hasSlot = existing.hasSavedSlot;

    existing = std::move(record);
    existing.pid = existing.pid ? existing.pid : pid;
    existing.lastPlacementResult = lastResult;
    existing.lastRequestedRect = lastRequested;
    existing.lastObservedRect = lastObserved;
    if (hasFloatingRect) {
      existing.floatingRect = floatingRect;
      existing.floatingMonitor = floatingMonitor;
      existing.floatingDpi = floatingDpi;
      existing.hasFloatingRect = true;
    }
    existing.state = state;
    existing.suspensionReason = suspension;
    existing.savedSlot = slot;
    existing.hasSavedSlot = hasSlot;
  }
  return it->second;
}

bool Workspace::ActivateTiled(HWND hwnd) {
  AssertWmThread(L"Workspace::ActivateTiled");
  WindowRecord* record = FindMutable(hwnd);
  if (!record) return false;

  bool changed = false;
  if (record->state != ManageState::Tiled || record->hasSavedSlot ||
      record->suspensionReason != SuspensionReason::None) {
    record->state = ManageState::Tiled;
    record->suspensionReason = SuspensionReason::None;
    record->savedSlot = {};
    record->hasSavedSlot = false;
    changed = true;
  }
  if (!ContainsWindow(windows_, hwnd)) {
    AppendTiledWithDefaultWeight(hwnd);
    changed = true;
  }
  if (changed) DebugValidateMutation(L"Workspace::ActivateTiled");
  return changed;
}

bool Workspace::AdmitTiled(WindowRecord record) {
  AssertWmThread(L"Workspace::AdmitTiled");
  if (!record.hwnd) return false;
  const HWND hwnd = record.hwnd;
  const bool existed = HasRecord(hwnd);
  UpsertRecord(std::move(record));
  const bool activated = ActivateTiled(hwnd);
  return !existed || activated;
}

// Fresh Automatic admission can insert relative to the retained managed focus.
// Shift suspended logical slots first so every member keeps a unique position.
bool Workspace::AdmitTiledAfter(WindowRecord record, HWND anchor) {
  AssertWmThread(L"Workspace::AdmitTiledAfter");
  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd) || !IsTiled(anchor)) {
    return AdmitTiled(std::move(record));
  }

  const size_t anchorActiveIndex = TiledIndex(anchor);
  if (anchorActiveIndex == static_cast<size_t>(-1)) {
    return AdmitTiled(std::move(record));
  }

  const size_t insertionLogicalIndex =
      ActiveIndexToLogicalIndex(anchorActiveIndex) + 1;
  for (auto& kv : records_) {
    WindowRecord& existing = kv.second;
    if (existing.state == ManageState::Suspended &&
        existing.hasSavedSlot &&
        existing.savedSlot.index >= insertionLogicalIndex) {
      ++existing.savedSlot.index;
    }
  }

  EnsureWeights();
  const size_t insertionActiveIndex =
      std::min(anchorActiveIndex + 1, windows_.size());
  windows_.insert(windows_.begin() + insertionActiveIndex, hwnd);

  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    gridWeights_.insert(
        gridWeights_.begin() + std::min(insertionActiveIndex, gridWeights_.size()),
        1.0);
  } else if ((layout_ == TileLayout::MasterStack ||
              layout_ == TileLayout::MasterStackH) &&
             insertionActiveIndex > 0) {
    const size_t stackIndex =
        std::min(insertionActiveIndex - 1, stackWeights_.size());
    stackWeights_.insert(stackWeights_.begin() + stackIndex, 1.0);
  }

  record.state = ManageState::Tiled;
  record.suspensionReason = SuspensionReason::None;
  record.savedSlot = {};
  record.hasSavedSlot = false;
  records_.emplace(hwnd, std::move(record));
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitTiledAfter");
  return true;
}

bool Workspace::RememberFloatingGeometry(
    HWND hwnd, const RECT& frame, HMONITOR monitor, UINT dpi) {
  AssertWmThread(L"Workspace::RememberFloatingGeometry");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !monitor || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }

  const UINT normalizedDpi = dpi ? dpi : 96;
  if (record->hasFloatingRect &&
      EqualRect(&record->floatingRect, &frame) &&
      record->floatingMonitor == monitor &&
      record->floatingDpi == normalizedDpi) {
    return false;
  }

  record->floatingRect = frame;
  record->floatingMonitor = monitor;
  record->floatingDpi = normalizedDpi;
  record->hasFloatingRect = true;
  DebugValidateMutation(L"Workspace::RememberFloatingGeometry");
  return true;
}

// Pure model import: interprets an already-observed geometry snapshot as
// authoritative workspace ordering and resize weights. No Win32 queries happen
// inside Workspace.
Workspace Workspace::AdoptGeometry(
    TileLayout layout, const RECT& workArea, LONG gap,
    double fallbackMasterRatio,
    const std::vector<WorkspaceGeometryItem>& items, HWND preferredFirstWin) {
  AssertWmThread(L"Workspace::AdoptGeometry");
  Workspace workspace;
  workspace.layout_ = layout;
  for (const auto& item : items) {
    workspace.windows_.push_back(item.record.hwnd);
    workspace.records_.emplace(item.record.hwnd, item.record);
  }
  if (items.empty()) return workspace;

  if (layout == TileLayout::MasterStack || layout == TileLayout::MasterStackH) {
    const bool horizontal = layout == TileLayout::MasterStackH;
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(items.size());
    for (const auto& item : items) infos.push_back({item.record.hwnd, item.frame});

    struct Candidate {
      long long score;
      long long axisPos;
      size_t index;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(infos.size());

    for (size_t i = 0; i < infos.size(); ++i) {
      const RECT& r = infos[i].rect;
      long long d[4] = {
          std::llabs(static_cast<long long>(r.left) - workArea.left),
          std::llabs(static_cast<long long>(r.top) - workArea.top),
          std::llabs(static_cast<long long>(r.right) - workArea.right),
          std::llabs(static_cast<long long>(r.bottom) - workArea.bottom),
      };
      std::sort(d, d + 4);
      Candidate candidate{
          d[0] + d[1] + d[2],
          horizontal
              ? std::llabs(static_cast<long long>(r.top) - workArea.top)
              : std::llabs(static_cast<long long>(r.left) - workArea.left),
          i};
      candidates.push_back(candidate);
    }

    std::stable_sort(
        candidates.begin(), candidates.end(),
        [preferredFirstWin, &infos](const Candidate& a, const Candidate& b) {
          const bool aPreferred = preferredFirstWin && infos[a.index].hwnd == preferredFirstWin;
          const bool bPreferred = preferredFirstWin && infos[b.index].hwnd == preferredFirstWin;
          if (aPreferred != bPreferred) return aPreferred;
          if (a.score != b.score) return a.score < b.score;
          return a.axisPos < b.axisPos;
        });

    const size_t masterIndex = candidates.front().index;
    const WindowInfo master = infos[masterIndex];
    std::vector<WindowInfo> stack;
    stack.reserve(infos.size() - 1);
    for (size_t i = 0; i < infos.size(); ++i) {
      if (i != masterIndex) stack.push_back(infos[i]);
    }
    std::stable_sort(
        stack.begin(), stack.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
          return horizontal ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
        });

    workspace.windows_.clear();
    workspace.windows_.push_back(master.hwnd);
    for (const auto& item : stack) workspace.windows_.push_back(item.hwnd);

    const LONG totalSize = horizontal ? workArea.bottom - workArea.top
                                      : workArea.right - workArea.left;
    if (totalSize > gap + 1) {
      LONG masterSize = horizontal ? master.rect.bottom - master.rect.top
                                   : master.rect.right - master.rect.left;
      if (masterSize < 1) masterSize = 1;
      workspace.masterRatio_ = ClampDouble(
          static_cast<double>(masterSize) / static_cast<double>(totalSize - gap),
          0.1, 0.9);
    } else {
      workspace.masterRatio_ = ClampDouble(fallbackMasterRatio, 0.1, 0.9);
    }

    workspace.stackWeights_.clear();
    for (const auto& item : stack) {
      LONG size = horizontal ? item.rect.right - item.rect.left
                             : item.rect.bottom - item.rect.top;
      if (size < 1) size = 1;
      workspace.stackWeights_.push_back(static_cast<double>(size));
    }
  } else if (layout == TileLayout::Columns || layout == TileLayout::Rows) {
    const bool horizontal = layout == TileLayout::Rows;
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(items.size());
    for (const auto& item : items) infos.push_back({item.record.hwnd, item.frame});

    std::stable_sort(
        infos.begin(), infos.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
          return horizontal ? a.rect.top < b.rect.top : a.rect.left < b.rect.left;
        });

    workspace.windows_.clear();
    workspace.gridWeights_.clear();
    for (const auto& item : infos) {
      workspace.windows_.push_back(item.hwnd);
      LONG size = horizontal ? item.rect.bottom - item.rect.top
                             : item.rect.right - item.rect.left;
      if (size < 1) size = 1;
      workspace.gridWeights_.push_back(static_cast<double>(size));
    }
  }

  workspace.EnsureWeights();
  workspace.DebugValidateMutation(L"Workspace::AdoptGeometry");
  return workspace;
}

