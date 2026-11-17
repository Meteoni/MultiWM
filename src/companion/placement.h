// Decides whether a window about to be shown for the first time will be managed
// by MultiWM, and if so moves it to the frame MultiWM will give it.
// Part of the MultiWM Companion mod; see the include order in src/companion/multiwm-companion.cpp.
//
// One-shot: one decision when a window goes from hidden to shown, nothing after.
// No per-window state is kept. A window MultiWM already knows (say, hidden to the
// tray) is in the snapshot's tracked list and is left alone; one it doesn't know
// will be admitted as new, so the next slot is right for it on any show.
// MultiWM stays the authority: a wrong guess costs one move by MultiWM afterwards,
// the same as without the companion.

namespace Companion {

struct Settings {
  bool logDecisions = false;
};

static Settings g_settings;
// Set while the companion itself moves a window, so its own calls never re-enter.
static thread_local bool g_placing = false;

// Native placement and minimum-size queries can run application code. Nested
// show calls must pass through until the outer decision and native call finish.
class PlacementScope {
 public:
  PlacementScope() { g_placing = true; }
  ~PlacementScope() { g_placing = false; }
  PlacementScope(const PlacementScope&) = delete;
  PlacementScope& operator=(const PlacementScope&) = delete;
};

using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
// The original NtUserSetWindowPos, filled in when the hook (companion/show_hooks.h)
// is applied; null until then or when win32u doesn't export it.
static SetWindowPos_t g_unhookedSetWindowPos = nullptr;

static void LoadSettings() {
  g_settings.logDecisions = Wh_GetIntSetting(L"logDecisions") != 0;
}

// The process name of the current process: the companion only handles windows
// of its own process, so it never needs OpenProcess.
static std::wstring CurrentProcessName(HWND) {
  static const std::wstring name = [] {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    const wchar_t* base = path;
    for (DWORD i = 0; i < length; ++i) {
      if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    }
    return std::wstring(base);
  }();
  return name;
}

// Switches the calling thread to per-monitor DPI awareness for its lifetime.
// The snapshot is in physical pixels, and so are MultiWM's placements; without
// this, a DPI-unaware app's thread would see and set virtualized coordinates.
class PhysicalCoordinatesScope {
 public:
  PhysicalCoordinatesScope()
      : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~PhysicalCoordinatesScope() {
    if (previous_) SetThreadDpiAwarenessContext(previous_);
  }
  PhysicalCoordinatesScope(const PhysicalCoordinatesScope&) = delete;
  PhysicalCoordinatesScope& operator=(const PhysicalCoordinatesScope&) = delete;

 private:
  DPI_AWARENESS_CONTEXT previous_;
};

// --- Geometry ------------------------------------------------------------------------

static LONG Width(const RECT& rect) { return rect.right - rect.left; }
static LONG Height(const RECT& rect) { return rect.bottom - rect.top; }

static RECT CenteredIn(const RECT& area, LONG width, LONG height) {
  width = std::max<LONG>(1, width);
  height = std::max<LONG>(1, height);
  const LONG left = area.left + Width(area) / 2 - width / 2;
  const LONG top = area.top + Height(area) / 2 - height / 2;
  return {left, top, left + width, top + height};
}

// Mirrors MultiWM's ClampFloatingRectToWorkArea (placement/floating.h).
static RECT ClampToArea(RECT rect, const RECT& area) {
  const LONG width = std::min(std::max<LONG>(1, Width(rect)), std::max<LONG>(1, Width(area)));
  const LONG height = std::min(std::max<LONG>(1, Height(rect)), std::max<LONG>(1, Height(area)));
  LONG left = std::max(rect.left, area.left);
  LONG top = std::max(rect.top, area.top);
  if (left + width > area.right) left = area.right - width;
  if (top + height > area.bottom) top = area.bottom - height;
  return {left, top, left + width, top + height};
}

static LONG ScaleDip(LONG value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

// Use DWM's visible frame when available; hidden windows may have no frame yet.
static RECT FrameOf(HWND hwnd, const RECT& windowRect) {
  RECT frame{};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
      frame.right > frame.left && frame.bottom > frame.top) {
    return frame;
  }
  return windowRect;
}

// --- Decision -----------------------------------------------------------------------

// Cheap checks that need no snapshot: the structural part of MultiWM's
// WindowCanBeManaged, plus the companion's own limits. Returns nullptr when the
// window is a candidate, else the reason it isn't.
static const char* RejectStructurally(HWND hwnd) {
  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
  // Moving another thread's window would wait on that thread mid-show.
  if (processId != GetCurrentProcessId() || threadId != GetCurrentThreadId()) return "not shown by its own thread";
  if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER)) return "child or owned window";
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (style & WS_CHILD) return "child window";
  if (style & (WS_MINIMIZE | WS_MAXIMIZE)) return "minimized or maximized";
  if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return "tool window";
  wchar_t className[64];
  if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) && SharedRules::IsIgnoredWindowClass(className)) {
    return "shell window";
  }
  if (!IsWindowEnabled(hwnd)) return "disabled";
  return nullptr;
}

// The frame MultiWM will give `hwnd` (currently at `frame`), mirroring
// Automatic discovery for an unknown window. `*keepSize` is set when MultiWM
// would only move it. Returns nothing, with `*reason` set, when MultiWM won't
// manage it as a new window or the snapshot can't say where it goes.
static std::optional<RECT> PredictFrame(
    HWND hwnd, const RECT& frame, const HintInputs& inputs, bool* keepSize, const char** reason) {
  using namespace CompanionAbi;
  *keepSize = false;
  auto reject = [&](const char* why) -> std::optional<RECT> {
    *reason = why;
    return std::nullopt;
  };
  constexpr uint32_t kComplete = kFlagRunning | kFlagRulesComplete | kFlagTrackedComplete;
  if ((inputs.flags & kComplete) != kComplete || !inputs.rules) return reject("no complete snapshot");
  if (!(inputs.flags & kFlagAutomaticMode)) return reject("Manual mode");
  if (inputs.tracked) return reject("MultiWM already knows this window");
  if (!inputs.workspace) return reject("no workspace on this monitor");

  const std::vector<SnapshotRule>& rules = *inputs.rules;
  SharedRules::LazyWindowFacts facts(hwnd, CurrentProcessName);
  auto matches = [&](WindowRuleTreatment treatment) {
    return SharedRules::FindFirstMatchingRule(rules, treatment, facts);
  };
  if (matches(WindowRuleTreatment::Exclude)) return reject("excluded by a rule");
  if (matches(WindowRuleTreatment::InitialPlacement)) return reject("routed by an Open-on-monitor rule");
  const bool floatingRule =
      matches(WindowRuleTreatment::AlwaysFloating) || matches(WindowRuleTreatment::DefaultToFloating);
  const bool resizable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_SIZEBOX) != 0;
  if (!resizable && !floatingRule) return reject("fixed-size window");

  const WorkspaceHint& workspace = *inputs.workspace;
  // Rule-floated windows keep the app's size and are centered without clamping
  // (CenterPreservingSize), in every layout.
  if (floatingRule) {
    *keepSize = true;
    return CenteredIn(workspace.workArea, Width(frame), Height(frame));
  }

  if (workspace.floatingLayout) {
    // NewWindowCenter: the default size unless a size rule says otherwise.
    const SnapshotRule* sizeRule = matches(WindowRuleTreatment::FloatingPlacementOverride);
    if (!sizeRule) {
      if (!workspace.hasFloatingFrame) return reject("no floating default");
      return workspace.floatingFrame;
    }
    LONG width = Width(frame);
    LONG height = Height(frame);
    if (!sizeRule->preserveFloatingSize) {
      width = sizeRule->useWorkspaceSize ? Width(workspace.workArea)
                                         : ScaleDip(sizeRule->floatingSizeDip.width, workspace.dpi);
      height = sizeRule->useWorkspaceSize ? Height(workspace.workArea)
                                          : ScaleDip(sizeRule->floatingSizeDip.height, workspace.dpi);
    }
    return ClampToArea(CenteredIn(workspace.workArea, width, height), workspace.workArea);
  }

  if (!workspace.hasTiledFrame) return reject("no free tiled slot");
  return workspace.tiledFrame;
}

// Mirrors the fullscreen test in MultiWM's GetPhysicalSuspensionReason: such a
// window is suspended as maximized, not tiled.
static bool CoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
  constexpr LONG kTolerancePx = 2;
  auto close = [](LONG a, LONG b) { return std::abs(a - b) <= kTolerancePx; };
  return close(windowRect.left, monitorRect.left) && close(windowRect.top, monitorRect.top) &&
         close(windowRect.right, monitorRect.right) && close(windowRect.bottom, monitorRect.bottom);
}

static void LogDecision(HWND hwnd, const char* path, const char* outcome) {
  if (!g_settings.logDecisions) return;
  wchar_t className[128] = L"";
  GetClassNameW(hwnd, className, ARRAYSIZE(className));
  Wh_Log(L"%S hwnd=%p class=%s: %S", path, hwnd, className, outcome);
}

struct PredictedPlacement {
  RECT windowRect;
  bool keepSize;
};

// Calculates without placing. `proposed` is the app's intended window rectangle
// in physical screen pixels, including resize borders. The current DWM margins
// translate it to a visible frame. Caller holds PlacementScope on the window's
// own thread, so application callbacks cannot recursively predict another show.
static std::optional<PredictedPlacement> PredictWindowPlacement(
    HWND hwnd, const RECT& proposed, const char* path) {
  PhysicalCoordinatesScope physical;
  RECT windowRect{};
  if (!GetWindowRect(hwnd, &windowRect)) return std::nullopt;
  const RECT currentFrame = FrameOf(hwnd, windowRect);
  const RECT margins{currentFrame.left - windowRect.left, currentFrame.top - windowRect.top,
                     windowRect.right - currentFrame.right, windowRect.bottom - currentFrame.bottom};
  const RECT frame{proposed.left + margins.left, proposed.top + margins.top,
                   proposed.right - margins.right, proposed.bottom - margins.bottom};

  // The monitor MultiWM will assign: the one holding the frame (GetWindowPhysicalMonitor).
  HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONULL);
  if (!monitor) monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
    LogDecision(hwnd, path, "off every monitor");
    return std::nullopt;
  }
  if (CoversMonitor(proposed, monitorInfo.rcMonitor)) {
    LogDecision(hwnd, path, "fullscreen");
    return std::nullopt;
  }

  HintInputs inputs;
  if (!ReadHintInputs(hwnd, monitorInfo.rcMonitor, &inputs)) {
    LogDecision(hwnd, path, "no snapshot (MultiWM not running?)");
    return std::nullopt;
  }
  bool keepSize = false;
  const char* reason = nullptr;
  const std::optional<RECT> target = PredictFrame(hwnd, frame, inputs, &keepSize, &reason);
  if (!target) {
    LogDecision(hwnd, path, reason);
    return std::nullopt;
  }

  // The snapshot holds visible frames; SetWindowPos takes the window rectangle,
  // which includes the invisible resize borders.
  RECT placement{target->left - margins.left, target->top - margins.top,
                  target->right + margins.right, target->bottom + margins.bottom};
  if (!keepSize) {
    // A fresh window may not fit the unconstrained hint. Keep the predicted
    // origin and clamp its size to the app's limits; MultiWM can refine the
    // layout later. Seed the same system defaults used by MultiWM's probe.
    const UINT dpi = inputs.workspace->dpi ? inputs.workspace->dpi : 96;
    MINMAXINFO limits{};
    limits.ptMinTrackSize = {GetSystemMetricsForDpi(SM_CXMINTRACK, dpi),
                             GetSystemMetricsForDpi(SM_CYMINTRACK, dpi)};
    limits.ptMaxTrackSize = {GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi),
                             GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi)};
    const POINT systemMinimum = limits.ptMinTrackSize;
    const bool wasVisible = IsWindowVisible(hwnd) != FALSE;
    SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));

    // The query executes app code. Abandon the hint if it destroyed, moved,
    // showed or otherwise made the window ineligible while answering.
    RECT afterQuery{};
    if (RejectStructurally(hwnd) || !GetWindowRect(hwnd, &afterQuery) ||
        !EqualRect(&windowRect, &afterQuery) || (IsWindowVisible(hwnd) != FALSE) != wasVisible) {
      return std::nullopt;
    }
    const LONG minWidth = std::max<LONG>(1, std::max(systemMinimum.x, limits.ptMinTrackSize.x));
    const LONG minHeight = std::max<LONG>(1, std::max(systemMinimum.y, limits.ptMinTrackSize.y));
    placement.right = placement.left + std::clamp(Width(placement), minWidth,
                                                  std::max(minWidth, limits.ptMaxTrackSize.x));
    placement.bottom = placement.top + std::clamp(Height(placement), minHeight,
                                                  std::max(minHeight, limits.ptMaxTrackSize.y));
  }
  return PredictedPlacement{placement, keepSize};
}

// Used by show and creation hooks that have no geometry arguments to rewrite.
static bool PlaceAtPredictedFrame(HWND hwnd, const char* path) {
  if (g_placing || !hwnd || RejectStructurally(hwnd)) return false;
  PlacementScope placing;
  PhysicalCoordinatesScope physical;
  RECT current{};
  if (!GetWindowRect(hwnd, &current)) return false;
  const auto prediction = PredictWindowPlacement(hwnd, current, path);
  if (!prediction) return false;

  const RECT& placement = prediction->windowRect;
  UINT flags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
  if (prediction->keepSize) flags |= SWP_NOSIZE;
  const auto setWindowPos = g_unhookedSetWindowPos ? g_unhookedSetWindowPos : SetWindowPos;
  const BOOL moved = setWindowPos(hwnd, nullptr, placement.left, placement.top,
                                  Width(placement), Height(placement), flags);
  LogDecision(hwnd, path, moved ? "placed at MultiWM's next frame" : "SetWindowPos failed");
  return moved != FALSE;
}

}  // namespace Companion
