// Native window observations, eligibility and checked placement.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Platform::Windows {

bool GetWindowFrameRect(HWND hwnd, RECT* outRect) {
  if (!hwnd || !outRect) return false;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, outRect, sizeof(*outRect)))) {
    return true;
  }
  return GetWindowRect(hwnd, outRect) != FALSE;
}

// True when `hwnd` still names a live window owned by `processId`.
// HWND values are recycled by Windows, so a handle saved earlier may now belong to
// a different window. Re-check after any call that can run other code (COM, shell,
// SendMessage) before acting on a saved handle. Pure query; never pumps messages.
static bool IsSameLiveWindow(HWND hwnd, DWORD processId) {
  DWORD livePid = 0;
  GetWindowThreadProcessId(hwnd, &livePid);
  return IsWindow(hwnd) && livePid == processId;
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

static inline long long WindowAreaOnWorkArea(HWND hwnd, const RECT& workArea) {
  RECT r{};
  if (!GetWindowFrameRect(hwnd, &r)) return 0;
  RECT inter{};
  if (!IntersectRect(&inter, &r, &workArea)) return 0;
  return Layout::RectAreaLL(inter);
}

// Win32 observation lives here, outside Model. These helpers translate live HWND
// properties/geometry into value objects consumed by Workspace and reconciliation.

constexpr UINT kConstraintProbeTimeoutMs = 40;

static int GetSystemMetricForDpiCompat(int index, UINT dpi) {
  using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
  static GetSystemMetricsForDpiFn fn = [] {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32
        ? reinterpret_cast<GetSystemMetricsForDpiFn>(
              GetProcAddress(user32, "GetSystemMetricsForDpi"))
        : nullptr;
  }();
  return fn ? fn(index, dpi ? dpi : 96) : GetSystemMetrics(index);
}

static void GetFrameDecorationSpan(
    HWND hwnd, LONG* outWidth, LONG* outHeight) {
  if (outWidth) *outWidth = 0;
  if (outHeight) *outHeight = 0;

  RECT windowRect{};
  RECT frame{};
  if (!hwnd ||
      !GetWindowRect(hwnd, &windowRect) ||
      FAILED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
          &frame, sizeof(frame)))) {
    return;
  }

  if (outWidth) {
    *outWidth = std::max<LONG>(
        0,
        (frame.left - windowRect.left) +
        (windowRect.right - frame.right));
  }
  if (outHeight) {
    *outHeight = std::max<LONG>(
        0,
        (frame.top - windowRect.top) +
        (windowRect.bottom - frame.bottom));
  }
}

// WM_GETMINMAXINFO can execute target-process window procedure code. Bound the
// synchronous query so a hung client cannot block the serialized WM actor. A
// failed probe remains unconstrained; a previously valid observation is retained
// by the evidence-merge path.
static WindowConstraints ProbeWindowConstraints(HWND hwnd) {
  ++Diagnostics::g_runtime.counters.constraintProbeAttempts;
  WindowConstraints constraints;

  if (!hwnd || !IsWindow(hwnd)) {
    constraints.lastProbeError = ERROR_INVALID_WINDOW_HANDLE;
    ++Diagnostics::g_runtime.counters.constraintProbeFailures;
    return constraints;
  }
  if (IsHungAppWindow(hwnd)) {
    constraints.lastProbeError = ERROR_TIMEOUT;
    ++Diagnostics::g_runtime.counters.constraintProbeTimeouts;
    return constraints;
  }

  HMONITOR monitor =
      MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  constraints.dpi = Platform::Monitors::GetMonitorEffectiveDpi(monitor);

  const LONG defaultMinWidth = std::max<LONG>(
      1, GetSystemMetricForDpiCompat(SM_CXMINTRACK, constraints.dpi));
  const LONG defaultMinHeight = std::max<LONG>(
      1, GetSystemMetricForDpiCompat(SM_CYMINTRACK, constraints.dpi));
  const LONG defaultMaxWidth = std::max<LONG>(
      defaultMinWidth,
      GetSystemMetricForDpiCompat(SM_CXMAXTRACK, constraints.dpi));
  const LONG defaultMaxHeight = std::max<LONG>(
      defaultMinHeight,
      GetSystemMetricForDpiCompat(SM_CYMAXTRACK, constraints.dpi));

  MINMAXINFO mmi{};
  mmi.ptMinTrackSize = {defaultMinWidth, defaultMinHeight};
  mmi.ptMaxTrackSize = {defaultMaxWidth, defaultMaxHeight};

  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  const LRESULT delivered =
      hwnd && IsWindow(hwnd)
          ? SendMessageTimeoutW(
                hwnd, WM_GETMINMAXINFO, 0,
                reinterpret_cast<LPARAM>(&mmi),
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                kConstraintProbeTimeoutMs, &result)
          : 0;
  if (!delivered) {
    DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
    constraints.lastProbeError = error;
    if (error == ERROR_TIMEOUT) {
      ++Diagnostics::g_runtime.counters.constraintProbeTimeouts;
    } else {
      ++Diagnostics::g_runtime.counters.constraintProbeFailures;
    }
    return constraints;
  }

  constraints.valid = true;
  ++Diagnostics::g_runtime.counters.constraintProbeSuccesses;

  LONG decorationWidth = 0;
  LONG decorationHeight = 0;
  GetFrameDecorationSpan(
      hwnd, &decorationWidth, &decorationHeight);

  constraints.minWidth = std::max<LONG>(
      1,
      std::max<LONG>(defaultMinWidth, mmi.ptMinTrackSize.x) -
          decorationWidth);
  constraints.minHeight = std::max<LONG>(
      1,
      std::max<LONG>(defaultMinHeight, mmi.ptMinTrackSize.y) -
          decorationHeight);

  return constraints;
}

static SuspensionReason GetPhysicalSuspensionReason(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return SuspensionReason::None;
  if (IsIconic(hwnd)) return SuspensionReason::Minimized;
  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if (!IsWindowVisible(hwnd) || (cloaked & DWM_CLOAKED_APP)) return SuspensionReason::Hidden;
  if (IsZoomed(hwnd)) return SuspensionReason::Maximized;

  // Chromium and similar apps can cover the full monitor while Windows still
  // reports the HWND as restored. Fullscreen and native maximize have identical
  // suspend/save-slot/restore semantics here.
  RECT windowRect{};
  if (GetWindowRect(hwnd, &windowRect)) {
    HMONITOR monitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONULL);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    constexpr LONG kFullscreenTolerancePx = 2;
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo) &&
        !Layout::Differs(windowRect.left, monitorInfo.rcMonitor.left,
                 kFullscreenTolerancePx) &&
        !Layout::Differs(windowRect.top, monitorInfo.rcMonitor.top,
                 kFullscreenTolerancePx) &&
        !Layout::Differs(windowRect.right, monitorInfo.rcMonitor.right,
                 kFullscreenTolerancePx) &&
        !Layout::Differs(windowRect.bottom, monitorInfo.rcMonitor.bottom,
                 kFullscreenTolerancePx)) {
      return SuspensionReason::Maximized;
    }
  }
  return SuspensionReason::None;
}

// Workspace initialization needs a broader physical snapshot than normal layout
// participation. A visible top-level window can already belong to this workspace
// while minimized, maximized, or fullscreen, even though active collection
// correctly excludes it from placement.
static std::vector<HWND> CollectWorkspaceWindowsForInitialization(
    HMONITOR monitor, const GUID& desktopId) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    GUID desktopId;
    std::vector<HWND>* windowList;
  } context = {monitor, desktopId, &windows};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (!WindowCanBeManaged(hwnd)) return TRUE;
        if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        if (GetWindowPhysicalMonitor(hwnd) != ctx->targetMonitor) {
          return TRUE;
        }

        ctx->windowList->push_back(hwnd);
        ++Diagnostics::g_runtime.counters.tileCandidatesCollected;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
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
      record.floatingDpi = Platform::Monitors::GetMonitorEffectiveDpi(monitor);
      record.hasFloatingRect = true;
    }
  }

  return record;
}

// Maps an event or foreground HWND to its active tile using only unambiguous
// HWND identity and ownership relationships.
HWND ResolveToTiledWindow(HWND hwnd, const std::vector<HWND>& candidates) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  if (Model::ContainsWindow(candidates, hwnd)) {
    return hwnd;
  }

  HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
  if (rootOwner && Model::ContainsWindow(candidates, rootOwner)) {
    return rootOwner;
  }

  HWND owner = GetWindow(hwnd, GW_OWNER);
  while (owner) {
    if (Model::ContainsWindow(candidates, owner)) {
      return owner;
    }
    owner = GetWindow(owner, GW_OWNER);
  }

  return nullptr;
}

std::vector<HWND> CollectTileWindows(HMONITOR monitor, const GUID& desktopId) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    GUID desktopId;
    std::vector<HWND>* windowList;
  } context = {monitor, desktopId, &windows};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        if (GetCurrentManagedWindowMonitor(hwnd, &ctx->desktopId) ==
            ctx->targetMonitor) {
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
    const std::vector<HMONITOR>& monitors, const GUID& desktopId) {
  std::vector<std::vector<HWND>> snapshots(monitors.size());
  if (monitors.empty()) return snapshots;

  struct EnumContext {
    const std::vector<HMONITOR>* monitors;
    GUID desktopId;
    std::vector<std::vector<HWND>>* snapshots;
  } context = {&monitors, desktopId, &snapshots};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        const HMONITOR monitor =
            GetCurrentManagedWindowMonitor(hwnd, &ctx->desktopId);
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

    if (!rule.classNames.empty()) {
      if (!haveClass) {
        wchar_t buffer[256]{};
        if (GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer))) className = buffer;
        haveClass = true;
      }
      if (std::none_of(
              rule.classNames.begin(), rule.classNames.end(),
              [&](const std::wstring& candidate) {
                return EqualsInsensitive(className, candidate);
              })) {
        continue;
      }
    }

    if (!rule.titleFragments.empty()) {
      if (!haveTitle) {
        wchar_t buffer[512]{};
        if (GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer))) title = buffer;
        haveTitle = true;
      }
      if (std::none_of(
              rule.titleFragments.begin(), rule.titleFragments.end(),
              [&](const std::wstring& candidate) {
                return ContainsInsensitive(title, candidate);
              })) {
        continue;
      }
    }

    if (!rule.processNames.empty()) {
      if (!haveProcess) {
        processName = GetWindowProcessName(hwnd);
        haveProcess = true;
      }
      if (std::none_of(
              rule.processNames.begin(), rule.processNames.end(),
              [&](const std::wstring& candidate) {
                return EqualsInsensitive(processName, candidate);
              })) {
        continue;
      }
    }

    return &rule;
  }
  return nullptr;
}

// Discovery waits for initial placement before assigning workspace ownership. Explicit
// admission bypasses this policy and overrides pending initial placement.
static bool IsWindowAwaitingInitialPlacement(HWND hwnd) {
  return g_wm.windowRoutingReady &&
         (g_wm.pendingWindowRoutes.count(hwnd) ||
          (!g_wm.windowRoutingEvaluated.count(hwnd) &&
           FindMatchingWindowRule(hwnd, WindowRuleTreatment::InitialPlacement)));
}

static bool HasMonitorPlacementRule(HWND hwnd) {
  const WindowRule* rule = FindMatchingWindowRule(hwnd, WindowRuleTreatment::InitialPlacement);
  return rule && (rule->monitor != RuleMonitor::Any || !rule->monitorId.empty());
}

static bool IsWindowExcludedByRules(HWND hwnd) {
  return FindMatchingWindowRule(hwnd, WindowRuleTreatment::Exclude) != nullptr;
}

static bool ShouldWindowAlwaysFloat(HWND hwnd) {
  return FindMatchingWindowRule(
             hwnd, WindowRuleTreatment::AlwaysFloating) != nullptr;
}

static bool ShouldWindowStartFloating(HWND hwnd, bool* alwaysFloating = nullptr) {
  const bool always = ShouldWindowAlwaysFloat(hwnd);
  if (alwaysFloating) *alwaysFloating = always;
  return always || FindMatchingWindowRule(
                       hwnd, WindowRuleTreatment::DefaultToFloating) != nullptr;
}

// Applies stable structural and exclusion policy only; visibility, show state,
// desktop, and monitor participation are checked separately. Tracked windows
// tolerate a temporarily missing resize style so transient app UI cannot evict them.
static bool WindowCanBeManaged(HWND hwnd, bool allowFixedSize) {
  if (!hwnd) return false;

  // Reject structurally ineligible HWNDs before evaluating configurable exclusion
  // rules. Process-name exclusions can require OpenProcess/QueryFullProcessImageName,
  // so there is no reason to pay that cost for tool/owned/child windows.
  if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER)) return false;

  LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
  if (style & WS_CHILD) return false;

  // window could temporarily lose its WS_SizeBox.
  // (example: Office save/discard dialog)
  // A Default-to-floating rule is also an explicit Automatic-mode admission
  // policy for naturally fixed-size top-level windows. Always-floating is also
  // eligible during explicit Manual admission; centering never resizes either.
  if (!allowFixedSize && !(style & WS_SIZEBOX) &&
      !IsWindowTrackedInAnyState(hwnd)) {
    bool alwaysFloating = false;
    const bool startsFloating =
        ShouldWindowStartFloating(hwnd, &alwaysFloating);
    if (!alwaysFloating && !(RuntimeLifecycle::IsAutomaticMode() && startsFloating)) return false;
  }

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
static HMONITOR GetCurrentManagedWindowMonitor(
    HWND hwnd, const GUID* expectedDesktop) {
  // window might become temporarily disabled
  // (example: VSCode unstaged commit dialog)
  if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return nullptr;

  // Minimized, hidden, maximized, and fullscreen windows are not active layout
  // participants. Their logical records retain enough state to restore in place.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) return nullptr;
  if (!WindowCanBeManaged(hwnd)) return nullptr;
  if (IsWindowCloaked(hwnd)) return nullptr;

  // Virtual desktop filter: other desktops' windows may not yet be cloaked
  // at the moment we enumerate (race between uncloak/cloak during switch).
  // Fail closed when the desktop predicate is temporarily unavailable; the
  // centralized runtime-maintenance path will recover the COM service and retry.
  if (expectedDesktop) {
    // Bind enumeration to one desktop, rather than a moving "current" predicate.
    GUID windowDesktop{};
    if (!Platform::VirtualDesktop::GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
        !IsEqualGUID(windowDesktop, *expectedDesktop)) {
      return nullptr;
    }
  } else {
    BOOL onCurrent = FALSE;
    if (!Platform::VirtualDesktop::IsWindowOnCurrentDesktopSafe(hwnd, &onCurrent) || !onCurrent) {
      return nullptr;
    }
  }

  return GetWindowPhysicalMonitor(hwnd);
}

// Positions one observed HWND without mutating logical workspace state. The
// resulting value object is folded into Workspace separately.
// WM thread only. Input/output frames are physical screen pixels. A non-success
// result can follow an issued native placement; inspect placementIssued and the
// observed frame rather than assuming failure left the HWND untouched.
static PlacementObservation PlaceWindowChecked(
    HWND hwnd, bool canMove, const RECT& targetRect,
    bool positionOnly = false) {
  ++Diagnostics::g_runtime.counters.placementChecks;
  PlacementObservation observation;
  observation.requested = targetRect;

  auto finish = [&](PlacementResult result) {
    observation.result = result;
    switch (result) {
      case PlacementResult::Success:
        ++Diagnostics::g_runtime.counters.placementSuccess;
        break;
      case PlacementResult::SuppressedByPhysicalState:
        ++Diagnostics::g_runtime.counters.placementSuppressedByPhysicalState;
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
    if (result != PlacementResult::Success &&
        result != PlacementResult::SuppressedByPhysicalState) {
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
    if (Layout::RectsNear(currentFrame, targetRect)) {
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

  // Close the final physical-state race at the actual mutation boundary. The
  // application can maximize/fullscreen/minimize itself after arrangement's
  // earlier eligibility check but before this SetWindowPos. Treat that transition
  // as a benign suppressed placement, never as refusal/nonconformance.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd)) {
    ++Diagnostics::g_runtime.counters.placementPreflightStops;
    return finish(PlacementResult::SuppressedByPhysicalState);
  }

  SetLastError(ERROR_SUCCESS);
  ++Diagnostics::g_runtime.counters.setWindowPosCalls;
  observation.placementIssued = true;
  BOOL placed = SetWindowPos(
      hwnd, nullptr,
      targetRect.left - offsetLeft,
      targetRect.top - offsetTop,
      positionOnly
          ? 0
          : targetRect.right - targetRect.left + offsetLeft + offsetRight,
      positionOnly
          ? 0
          : targetRect.bottom - targetRect.top + offsetTop + offsetBottom,
      SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE |
          SWP_NOSENDCHANGING | (positionOnly ? SWP_NOSIZE : 0));
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
  return finish(Layout::RectsNear(observed, targetRect)
                    ? PlacementResult::Success
                    : PlacementResult::AdjustedByWindow);
}

// Uses aggregate work-area coverage as a coarse guard against learning arbitrary
// floating geometry as an authoritative tiled layout.
static bool SnapshotLooksTiled(const std::vector<HWND>& windows, const RECT& workArea) {
  if (windows.empty()) return false;
  const long long workAreaArea = Layout::RectAreaLL(workArea);
  long long sumArea = 0;
  for (HWND w : windows) sumArea += WindowAreaOnWorkArea(w, workArea);
  const long long lo = (workAreaArea * 85) / 100;
  const long long hi = (windows.size() <= 2) ? (workAreaArea * 115) / 100 : (workAreaArea * 105) / 100;
  return sumArea >= lo && sumArea <= hi;
}

} // namespace Platform::Windows
