//=============================================================================
// Window observation + eligibility
//=============================================================================

// Win32 observation lives here, outside Model. These helpers translate live HWND
// properties/geometry into value objects consumed by Workspace and reconciliation.

constexpr UINT kConstraintProbeTimeoutMs = 40;

static int GetSystemMetricForDpiCompat(int index, UINT dpi) {
  using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
  static GetSystemMetricsForDpiFn fn = [] {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<GetSystemMetricsForDpiFn>(
                        GetProcAddress(user32, "GetSystemMetricsForDpi"))
                  : nullptr;
  }();
  return fn ? fn(index, dpi ? dpi : 96) : GetSystemMetrics(index);
}

static void GetFrameDecorationSpan(HWND hwnd, LONG* width, LONG* height) {
  if (width) *width = 0;
  if (height) *height = 0;
  RECT windowRect{}, frame{};
  if (!hwnd || !GetWindowRect(hwnd, &windowRect) ||
      FAILED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
    return;
  }
  if (width) {
    *width = std::max<LONG>(
        0, (frame.left - windowRect.left) +
               (windowRect.right - frame.right));
  }
  if (height) {
    *height = std::max<LONG>(
        0, (frame.top - windowRect.top) +
               (windowRect.bottom - frame.bottom));
  }
}

// WM_GETMINMAXINFO is synchronous application code, so never allow a hung client
// to stall the WM actor indefinitely. The returned tracking dimensions describe
// the outer HWND rectangle; convert them to our DWM-frame geometry before storing.
static WindowConstraints ProbeWindowConstraints(HWND hwnd, bool canResize) {
  ++Diagnostics::g_runtime.counters.constraintProbeAttempts;

  WindowConstraints constraints;
  HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
  constraints.dpi = GetMonitorEffectiveDpi(monitor);

  const LONG defaultMinWidth =
      std::max<LONG>(1, GetSystemMetricForDpiCompat(SM_CXMINTRACK, constraints.dpi));
  const LONG defaultMinHeight =
      std::max<LONG>(1, GetSystemMetricForDpiCompat(SM_CYMINTRACK, constraints.dpi));
  const LONG defaultMaxWidth =
      std::max<LONG>(defaultMinWidth,
                     GetSystemMetricForDpiCompat(SM_CXMAXTRACK, constraints.dpi));
  const LONG defaultMaxHeight =
      std::max<LONG>(defaultMinHeight,
                     GetSystemMetricForDpiCompat(SM_CYMAXTRACK, constraints.dpi));

  MINMAXINFO mmi{};
  mmi.ptMinTrackSize = {defaultMinWidth, defaultMinHeight};
  mmi.ptMaxTrackSize = {defaultMaxWidth, defaultMaxHeight};

  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  const LRESULT delivered = hwnd && IsWindow(hwnd)
      ? SendMessageTimeoutW(
            hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mmi),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            kConstraintProbeTimeoutMs, &result)
      : 0;
  if (!delivered) {
    DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
    constraints.lastProbeError = error;
    constraints.lastProbeTimedOut = error == ERROR_TIMEOUT;
    if (constraints.lastProbeTimedOut) {
      ++Diagnostics::g_runtime.counters.constraintProbeTimeouts;
    } else {
      ++Diagnostics::g_runtime.counters.constraintProbeFailures;
    }
    // Even when client code doesn't respond, Windows' ordinary minimum tracking
    // size remains a safe floor. Do not invent a restrictive maximum.
    mmi.ptMinTrackSize = {defaultMinWidth, defaultMinHeight};
    mmi.ptMaxTrackSize = {defaultMaxWidth, defaultMaxHeight};
  } else {
    constraints.valid = true;
    ++Diagnostics::g_runtime.counters.constraintProbeSuccesses;
  }

  LONG decorationWidth = 0, decorationHeight = 0;
  GetFrameDecorationSpan(hwnd, &decorationWidth, &decorationHeight);
  constraints.minWidth =
      std::max<LONG>(1, std::max<LONG>(defaultMinWidth, mmi.ptMinTrackSize.x) -
                            decorationWidth);
  constraints.minHeight =
      std::max<LONG>(1, std::max<LONG>(defaultMinHeight, mmi.ptMinTrackSize.y) -
                            decorationHeight);

  // The system default max-track values are implementation limits, not useful
  // application intent. Honor only a client-reported more restrictive maximum.
  constexpr LONG kTrackComparisonTolerance = 2;
  if (mmi.ptMaxTrackSize.x > 0 &&
      mmi.ptMaxTrackSize.x + kTrackComparisonTolerance < defaultMaxWidth) {
    constraints.maxWidthBounded = true;
    constraints.maxWidth =
        std::max<LONG>(constraints.minWidth, mmi.ptMaxTrackSize.x - decorationWidth);
  }
  if (mmi.ptMaxTrackSize.y > 0 &&
      mmi.ptMaxTrackSize.y + kTrackComparisonTolerance < defaultMaxHeight) {
    constraints.maxHeightBounded = true;
    constraints.maxHeight =
        std::max<LONG>(constraints.minHeight, mmi.ptMaxTrackSize.y - decorationHeight);
  }

  // A non-resizable tracked window is effectively fixed-size. Normally new
  // non-sizeable windows are excluded before admission, but preserving this fact
  // makes migration/legacy records deterministic.
  if (!canResize) {
    RECT frame{};
    if (GetWindowFrameRect(hwnd, &frame) &&
        frame.right > frame.left && frame.bottom > frame.top) {
      constraints.minWidth = constraints.maxWidth = frame.right - frame.left;
      constraints.minHeight = constraints.maxHeight = frame.bottom - frame.top;
      constraints.maxWidthBounded = true;
      constraints.maxHeightBounded = true;
    }
  }

  return constraints;
}

static bool SameConstraintGeometry(
    const WindowConstraints& a, const WindowConstraints& b) {
  return a.minWidth == b.minWidth && a.minHeight == b.minHeight &&
         a.maxWidth == b.maxWidth && a.maxHeight == b.maxHeight &&
         a.dpi == b.dpi &&
         a.maxWidthBounded == b.maxWidthBounded &&
         a.maxHeightBounded == b.maxHeightBounded;
}

static WindowConstraints MergeConstraintProbeWithCache(
    const WindowConstraints& cached, WindowConstraints observed) {
  if (!observed.valid && cached.valid) {
    const bool timedOut = observed.lastProbeTimedOut;
    const DWORD error = observed.lastProbeError;
    observed = cached;
    observed.lastProbeTimedOut = timedOut;
    observed.lastProbeError = error;
  }
  return observed;
}

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
    record.constraints = ProbeWindowConstraints(hwnd, record.canResize);

    RECT frame{};
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    // Only a restored window exposes meaningful natural/floating geometry.
    // Minimized/maximized admission should fall back to the configured size
    // rather than remembering an iconic or maximized rectangle.
    if (monitor && IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd) &&
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

// Best-effort bridge for apps that replace one top-level HWND with another.
// Candidates must share the PID and class; an exact title match wins, otherwise
// the first PID/class match is returned from the caller's scoped candidate set.
HWND ResolveEquivalentWindow(HWND hwnd, const std::vector<HWND>& candidates) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  wchar_t className[128] = {};
  GetClassNameW(hwnd, className, ARRAYSIZE(className));

  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, ARRAYSIZE(title));

  HWND bestMatch = nullptr;
  for (HWND candidate : candidates) {
    if (!candidate || candidate == hwnd || !IsWindow(candidate)) continue;

    DWORD candPid = 0;
    GetWindowThreadProcessId(candidate, &candPid);
    if (candPid != pid) continue;

    wchar_t candClass[128] = {};
    GetClassNameW(candidate, candClass, ARRAYSIZE(candClass));
    if (_wcsicmp(candClass, className) != 0) continue;

    wchar_t candTitle[256] = {};
    GetWindowTextW(candidate, candTitle, ARRAYSIZE(candTitle));

    if (_wcsicmp(candTitle, title) == 0 || (candTitle[0] == 0 && title[0] == 0)) {
      return candidate;
    }

    if (!bestMatch) bestMatch = candidate;
  }

  return bestMatch;
}

// Maps an event or foreground HWND to its active tile, preferring exact and
// ownership relationships before falling back to the looser equivalence check.
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

  return ResolveEquivalentWindow(hwnd, candidates);
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

static bool IsWindowExcludedByRules(HWND hwnd) {
  if (!hwnd || g_settings.exclusionRules.empty()) return false;

  std::wstring processName;
  std::wstring className;
  std::wstring title;
  bool haveProcess = false;
  bool haveClass = false;
  bool haveTitle = false;

  for (const auto& rule : g_settings.exclusionRules) {
    switch (rule.match) {
      case ExclusionMatch::Process:
        if (!haveProcess) {
          processName = GetWindowProcessName(hwnd);
          haveProcess = true;
        }
        if (!processName.empty() && EqualsInsensitive(processName, rule.value)) return true;
        break;

      case ExclusionMatch::Class:
        if (!haveClass) {
          wchar_t buffer[256]{};
          if (GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer))) className = buffer;
          haveClass = true;
        }
        if (!className.empty() && EqualsInsensitive(className, rule.value)) return true;
        break;

      case ExclusionMatch::Title:
        if (!haveTitle) {
          wchar_t buffer[512]{};
          if (GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer))) title = buffer;
          haveTitle = true;
        }
        if (!title.empty() && ContainsInsensitive(title, rule.value)) return true;
        break;
    }
  }
  return false;
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

  // FancyWM only tiles windows in the Restored state. Maximized windows remain
  // managed logically but do not participate until Windows reports Restored.
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return nullptr;
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

  RECT frameRect{};
  if (FAILED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect))) &&
      !GetWindowRect(hwnd, &frameRect)) {
    return nullptr;
  }

  if (frameRect.right <= frameRect.left || frameRect.bottom <= frameRect.top) {
    return nullptr;
  }
  return MonitorFromRect(&frameRect, MONITOR_DEFAULTTONULL);
}

// Determines whether an HWND can actively participate on targetMonitor now.
bool IsCurrentlyManagedWindow(HWND hwnd, HMONITOR targetMonitor) {
  return targetMonitor && GetCurrentManagedWindowMonitor(hwnd) == targetMonitor;
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
  AcquireSRWLockExclusive(&g_moveSize.lock);
  g_moveSize.startRects.erase(hwnd);
  g_moveSize.endRects.erase(hwnd);
  g_moveSize.endPoints.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSize.lock);
}

static void ClearAllMoveSizeSamples() {
  AcquireSRWLockExclusive(&g_moveSize.lock);
  g_moveSize.startRects.clear();
  g_moveSize.endRects.clear();
  g_moveSize.endPoints.clear();
  ReleaseSRWLockExclusive(&g_moveSize.lock);
}





