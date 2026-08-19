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
T ReadHotkeySetting(PCWSTR name, Parser parser, T defaultVal) {
  PCWSTR str = Wh_GetStringSetting(name);
  if (!str || !*str) {
    Wh_FreeStringSetting(str);
    return T{};  // Blank hotkey = unused.
  }
  T result = parser(str);
  Wh_FreeStringSetting(str);
  return result ? result : defaultVal;
}

UINT ReadModifierSetting(PCWSTR name, UINT defaultVal) {
  PCWSTR str = Wh_GetStringSetting(name);
  UINT result = ParseModifiers(str);
  Wh_FreeStringSetting(str);
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

bool GetMonitorWorkArea(HMONITOR monitor, RECT* outRect) {
  if (!monitor || !outRect) return false;
  MONITORINFO monitorInfo = {sizeof(monitorInfo)};
  if (!GetMonitorInfoW(monitor, &monitorInfo)) return false;
  *outRect = monitorInfo.rcWork;
  return true;
}

static UINT GetMonitorEffectiveDpi(HMONITOR monitor) {
  if (!monitor) return 96;

  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  using GetScaleFactorForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int*);
  struct DpiApis {
    GetDpiForMonitorFn getDpi = nullptr;
    GetScaleFactorForMonitorFn getScale = nullptr;
  };
  static const DpiApis apis = [] {
    DpiApis result;
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
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
  // library is required. GetScaleFactorForMonitor also guards against hosts
  // whose DPI-awareness context makes GetDpiForMonitor report 96.
  const bool haveDpi = apis.getDpi &&
      SUCCEEDED(apis.getDpi(monitor, 0, &dpiX, &dpiY)) && dpiX;

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

bool AreSameWindows(const std::vector<HWND>& a, const std::vector<HWND>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

std::vector<double> DefaultWeights(size_t count) {
  return std::vector<double>(count, 1.0);
}

bool IsCurrentlyManagedWindow(HWND hwnd, HMONITOR targetMonitor);
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
    const int failures = existing.placementFailures;
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
    existing.placementFailures = failures;
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

bool Workspace::ActivateTiled(HWND hwnd, bool resetPlacementFailures) {
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
  if (resetPlacementFailures && record->placementFailures != 0) {
    record->placementFailures = 0;
    changed = true;
  }
  if (!ContainsWindow(windows_, hwnd)) {
    AppendTiledWithDefaultWeight(hwnd);
    changed = true;
  }
  if (changed) DebugValidateMutation(L"Workspace::ActivateTiled");
  return changed;
}

bool Workspace::AdmitTiled(WindowRecord record, bool resetPlacementFailures) {
  AssertWmThread(L"Workspace::AdmitTiled");
  if (!record.hwnd) return false;
  const HWND hwnd = record.hwnd;
  const bool existed = HasRecord(hwnd);
  UpsertRecord(std::move(record));
  const bool activated = ActivateTiled(hwnd, resetPlacementFailures);
  return !existed || activated;
}

bool Workspace::ResetPlacementFailures(HWND hwnd) {
  AssertWmThread(L"Workspace::ResetPlacementFailures");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || record->placementFailures == 0) return false;
  record->placementFailures = 0;
  DebugValidateMutation(L"Workspace::ResetPlacementFailures");
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

