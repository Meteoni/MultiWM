//=============================================================================
// Window observation + eligibility
//=============================================================================

// Win32 observation lives here, outside Model. These helpers translate live HWND
// properties/geometry into value objects consumed by Workspace and reconciliation.

static WindowRecord MakeWindowRecord(HWND hwnd, ManageState state = ManageState::Tiled) {
  WindowRecord record;
  record.hwnd = hwnd;
  record.state = state;

  if (hwnd) {
    GetWindowThreadProcessId(hwnd, &record.pid);
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    record.canMove = (GetAncestor(hwnd, GA_ROOT) == hwnd) && !(style & WS_CHILD);
    record.canResize = (style & WS_SIZEBOX) != 0;
    record.topmost = (exStyle & WS_EX_TOPMOST) != 0;

    RECT frame{};
    HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
    // Only a participating restored window exposes meaningful natural/floating
    // geometry. Suspended admission should fall back to the configured size.
    if (monitor &&
        GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
        GetWindowFrameRect(hwnd, &frame) &&
        frame.right > frame.left && frame.bottom > frame.top) {
      record.floatingRect = frame;
      record.floatingMonitor = monitor;
      record.floatingDpi = GetMonitorEffectiveDpi(monitor);
      record.hasFloatingRect = true;
    }
  }

  return record;
}

// Maps an event or foreground HWND to its active tile using only unambiguous
// HWND identity and ownership relationships.
HWND ResolveToTiledWindow(HWND hwnd, const std::vector<HWND>& candidates) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  if (ContainsWindow(candidates, hwnd)) {
    return hwnd;
  }

  HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
  if (rootOwner && ContainsWindow(candidates, rootOwner)) {
    return rootOwner;
  }

  HWND owner = GetWindow(hwnd, GW_OWNER);
  while (owner) {
    if (ContainsWindow(candidates, owner)) {
      return owner;
    }
    owner = GetWindow(owner, GW_OWNER);
  }

  return nullptr;
}

std::vector<HWND> CollectTileWindows(HMONITOR monitor) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    std::vector<HWND>* windowList;
  } context = {monitor, &windows};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        if (GetCurrentManagedWindowMonitor(hwnd) == ctx->targetMonitor) {
          ctx->windowList->push_back(hwnd);
          ++Diagnostics::g_runtime.counters.tileCandidatesCollected;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

// Deferred reconciliation often needs snapshots for several monitors at once.
// Enumerate top-level HWNDs once and bucket each eligible window by monitor instead
// of repeating EnumWindows (and its DWM/VD checks) once per monitor.
static std::vector<std::vector<HWND>> CollectTileWindowsForMonitors(
    const std::vector<HMONITOR>& monitors) {
  std::vector<std::vector<HWND>> snapshots(monitors.size());
  if (monitors.empty()) return snapshots;

  struct EnumContext {
    const std::vector<HMONITOR>* monitors;
    std::vector<std::vector<HWND>>* snapshots;
  } context = {&monitors, &snapshots};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        const HMONITOR monitor = GetCurrentManagedWindowMonitor(hwnd);
        if (!monitor) return TRUE;

        auto it = std::find(ctx->monitors->begin(), ctx->monitors->end(), monitor);
        if (it == ctx->monitors->end()) return TRUE;

        const size_t index = static_cast<size_t>(it - ctx->monitors->begin());
        (*ctx->snapshots)[index].push_back(hwnd);
        ++Diagnostics::g_runtime.counters.tileCandidatesCollected;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return snapshots;
}

// Captures Win32 geometry/capability observations before the pure Workspace
// importer interprets them as ordering and layout weights.
static std::vector<WorkspaceGeometryItem> CaptureWorkspaceGeometry(
    const std::vector<HWND>& windows, HMONITOR monitor, const RECT& workArea) {
  std::vector<WorkspaceGeometryItem> items;
  items.reserve(windows.size());
  for (HWND hwnd : windows) {
    RECT rect{};
    if (!GetWindowFrameRect(hwnd, &rect) ||
        MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != monitor) {
      rect = workArea;
    }
    items.push_back({MakeWindowRecord(hwnd, ManageState::Tiled), rect});
  }
  return items;
}

static const wchar_t* kIgnoredWindowClasses[] = {
  L"Progman",
  L"WorkerW",
  L"Shell_TrayWnd",
  L"Shell_SecondaryTrayWnd",
  L"Windows.UI.Core.CoreWindow",
  L"OperationStatusWindow",
};

bool IsWindowCloaked(HWND hwnd) {
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  return cloaked;
}

static bool IsWindowTrackedInAnyState(HWND hwnd) {
  // Exact HWND identity only; child/owned lifecycle events do not inherit the
  // managed state of their top-level/root-owner window.
  return g_workspaces.IsTracked(hwnd);
}

static bool EqualsInsensitive(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty()) return false;
  return std::search(
             text.begin(), text.end(), needle.begin(), needle.end(),
             [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end();
}

static std::wstring GetWindowProcessName(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return {};

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};

  wchar_t path[32768]{};
  DWORD chars = ARRAYSIZE(path);
  std::wstring result;
  if (QueryFullProcessImageNameW(process, 0, path, &chars) && chars) {
    const wchar_t* base = path;
    for (DWORD i = 0; i < chars; ++i) {
      if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    }
    result.assign(base, static_cast<size_t>((path + chars) - base));
  }
  CloseHandle(process);
  return result;
}

static const WindowRule* FindMatchingWindowRule(
    HWND hwnd, WindowRuleTreatment treatment) {
  if (!hwnd || g_settings.windowRules.empty()) return nullptr;

  std::wstring processName;
  std::wstring className;
  std::wstring title;
  bool haveProcess = false;
  bool haveClass = false;
  bool haveTitle = false;

  for (const auto& rule : g_settings.windowRules) {
    if (rule.treatment != treatment) continue;

    if (!rule.className.empty()) {
      if (!haveClass) {
        wchar_t buffer[256]{};
        if (GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer))) className = buffer;
        haveClass = true;
      }
      if (!EqualsInsensitive(className, rule.className)) continue;
    }

    if (!rule.titleContains.empty()) {
      if (!haveTitle) {
        wchar_t buffer[512]{};
        if (GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer))) title = buffer;
        haveTitle = true;
      }
      if (!ContainsInsensitive(title, rule.titleContains)) continue;
    }

    if (!rule.process.empty()) {
      if (!haveProcess) {
        processName = GetWindowProcessName(hwnd);
        haveProcess = true;
      }
      if (!EqualsInsensitive(processName, rule.process)) continue;
    }

    return &rule;
  }
  return nullptr;
}

static bool IsWindowExcludedByRules(HWND hwnd) {
  return FindMatchingWindowRule(hwnd, WindowRuleTreatment::Exclude) != nullptr;
}

// Applies stable structural and exclusion policy only; visibility, show state,
// desktop, and monitor participation are checked separately. Tracked windows
// tolerate a temporarily missing resize style so transient app UI cannot evict them.
static bool WindowCanBeManaged(HWND hwnd) {
  if (!hwnd) return false;

  // Reject structurally ineligible HWNDs before evaluating configurable exclusion
  // rules. Process-name exclusions can require OpenProcess/QueryFullProcessImageName,
  // so there is no reason to pay that cost for tool/owned/child windows.
  if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER)) return false;

  LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
  if (style & WS_CHILD) return false;

  // window could temporarily lose its WS_SizeBox.
  // (example: Office save/discard dialog)
  if (!(style & WS_SIZEBOX) && !IsWindowTrackedInAnyState(hwnd)) return false;

  LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;

  wchar_t className[64];
  if (GetClassNameW(hwnd, className, ARRAYSIZE(className))) {
    for (const auto* ignoredClass : kIgnoredWindowClasses) {
      if (_wcsicmp(className, ignoredClass) == 0) return false;
    }
  }

  return !IsWindowExcludedByRules(hwnd);
}

// Returns the monitor on which an HWND can actively participate right now.
// A null result means the window is not currently tile-eligible.
static HMONITOR GetCurrentManagedWindowMonitor(HWND hwnd) {
  // window might become temporarily disabled
  // (example: VSCode unstaged commit dialog)
  if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return nullptr;

  // FancyWM only tiles windows in the Restored state. Minimized, maximized and
  // native-fullscreen windows remain managed logically until they are restored.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) return nullptr;
  if (!WindowCanBeManaged(hwnd)) return nullptr;
  if (IsWindowCloaked(hwnd)) return nullptr;

  // Virtual desktop filter: other desktops' windows may not yet be cloaked
  // at the moment we enumerate (race between uncloak/cloak during switch).
  // Fail closed when the desktop predicate is temporarily unavailable; the
  // centralized runtime-maintenance path will recover the COM service and retry.
  BOOL onCurrent = FALSE;
  if (!IsWindowOnCurrentDesktopSafe(hwnd, &onCurrent) || !onCurrent) {
    return nullptr;
  }

  return GetWindowPhysicalMonitor(hwnd);
}

static bool RectsNear(const RECT& a, const RECT& b, LONG tolerance = Model::kPlacementTolerancePx) {
  return !Differs(a.left, b.left, tolerance) &&
         !Differs(a.top, b.top, tolerance) &&
         !Differs(a.right, b.right, tolerance) &&
         !Differs(a.bottom, b.bottom, tolerance);
}

// Positions one observed HWND without mutating logical workspace state. The
// resulting value object is folded into Workspace separately.
static PlacementObservation PlaceWindowChecked(
    HWND hwnd, bool canMove, const RECT& targetRect) {
  ++Diagnostics::g_runtime.counters.placementChecks;
  PlacementObservation observation;
  observation.requested = targetRect;

  auto finish = [&](PlacementResult result) {
    observation.result = result;
    switch (result) {
      case PlacementResult::Success:
        ++Diagnostics::g_runtime.counters.placementSuccess;
        break;
      case PlacementResult::AdjustedByWindow:
        ++Diagnostics::g_runtime.counters.placementAdjusted;
        break;
      case PlacementResult::AccessDenied:
        ++Diagnostics::g_runtime.counters.placementAccessDenied;
        break;
      case PlacementResult::Refused:
        ++Diagnostics::g_runtime.counters.placementRefused;
        break;
      case PlacementResult::Dead:
        ++Diagnostics::g_runtime.counters.placementDead;
        break;
    }
    if (result != PlacementResult::Success) {
      Diagnostics::RecordEvent(
          L"placement result=%d hwnd=%p target=[%ld,%ld,%ld,%ld]",
          static_cast<int>(result), hwnd, targetRect.left, targetRect.top,
          targetRect.right, targetRect.bottom);
    }
    return observation;
  };

  if (!hwnd || !IsWindow(hwnd)) {
    ++Diagnostics::g_runtime.counters.placementPreflightStops;
    return finish(PlacementResult::Dead);
  }
  if (!canMove) {
    ++Diagnostics::g_runtime.counters.placementPreflightStops;
    return finish(PlacementResult::Refused);
  }
  if (IsHungAppWindow(hwnd)) {
    ++Diagnostics::g_runtime.counters.placementPreflightStops;
    return finish(PlacementResult::Refused);
  }

  RECT currentFrame{};
  if (GetWindowFrameRect(hwnd, &currentFrame)) {
    observation.observed = currentFrame;
    if (RectsNear(currentFrame, targetRect)) {
      ++Diagnostics::g_runtime.counters.placementNoOps;
      return finish(PlacementResult::Success);
    }
  }

  RECT windowRect{}, extendedFrame{};
  LONG offsetLeft = 0, offsetTop = 0, offsetRight = 0, offsetBottom = 0;
  if (GetWindowRect(hwnd, &windowRect) &&
      SUCCEEDED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extendedFrame, sizeof(extendedFrame)))) {
    offsetLeft = extendedFrame.left - windowRect.left;
    offsetTop = extendedFrame.top - windowRect.top;
    offsetRight = windowRect.right - extendedFrame.right;
    offsetBottom = windowRect.bottom - extendedFrame.bottom;
  }

  SetLastError(ERROR_SUCCESS);
  ++Diagnostics::g_runtime.counters.setWindowPosCalls;
  observation.placementIssued = true;
  BOOL placed = SetWindowPos(
      hwnd, nullptr,
      targetRect.left - offsetLeft,
      targetRect.top - offsetTop,
      targetRect.right - targetRect.left + offsetLeft + offsetRight,
      targetRect.bottom - targetRect.top + offsetTop + offsetBottom,
      SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
  if (!placed) {
    const DWORD error = GetLastError();
    return finish(error == ERROR_ACCESS_DENIED
                      ? PlacementResult::AccessDenied
                      : PlacementResult::Refused);
  }

  RECT observed{};
  if (!GetWindowFrameRect(hwnd, &observed)) {
    return finish(IsWindow(hwnd) ? PlacementResult::Refused
                                 : PlacementResult::Dead);
  }

  observation.observed = observed;
  return finish(RectsNear(observed, targetRect)
                    ? PlacementResult::Success
                    : PlacementResult::AdjustedByWindow);
}

static void ArrangeWorkspace(const DesktopMonitorKey& key);

static void ClearMoveSizeSamples(HWND hwnd) {
  AssertWmThread(L"ClearMoveSizeSamples");
  g_moveSize.startRects.erase(hwnd);
  g_moveSize.endRects.erase(hwnd);
  g_moveSize.endPoints.erase(hwnd);
}

static void ClearAllMoveSizeSamples() {
  AssertWmThread(L"ClearAllMoveSizeSamples");
  g_moveSize.startRects.clear();
  g_moveSize.endRects.clear();
  g_moveSize.endPoints.clear();
}





