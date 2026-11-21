// Places a hidden window at the frame MultiWM will give it: the admission frame
// for an unknown window, or its published restore frame for a suspended member.
// Part of the MultiWM Companion mod; see the include order in src/companion/multiwm-companion.cpp.
//
// One-shot: one decision when a window goes from hidden to shown, nothing after.
// No per-window state is kept. A window MultiWM already knows (say, hidden to the
// tray) uses its own restore frame or a destination arrival frame. restoration.h
// separately prepares the normal rectangle for minimized/maximized restores.
// Unknown windows will be admitted as new, so the next slot is right on any show.
// MultiWM stays the authority: a wrong guess costs one move by MultiWM afterwards,
// the same as without the companion.

namespace Companion {

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
// The companion's own moves go through the public user32 SetWindowPos, so other
// mods' SetWindowPos hooks see them. Mobile Open Animation, for one, retargets its
// panel when an animating window moves; an unhooked call would leave the panel at
// the old rectangle. The companion's own NtUserSetWindowPos hook passes the call
// through because the caller holds PlacementScope. Tests redirect this pointer.
static SetWindowPos_t g_setWindowPosForOwnMoves = SetWindowPos;

// The process name of the current process: the companion only handles windows
// of its own process, so it never needs OpenProcess.
static std::wstring CurrentProcessName(HWND) {
  // Read as MultiWM reads other processes' names, so process rules match alike.
  static const std::wstring name = SharedRules::ProcessImageFileName(GetCurrentProcess());
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

using SharedGeometry::Height;
using SharedGeometry::Width;

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
static const char* RejectStructurally(HWND hwnd, bool allowSuspended = false) {
  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
  // Moving another thread's window would wait on that thread mid-show.
  if (processId != GetCurrentProcessId() || threadId != GetCurrentThreadId()) return "not shown by its own thread";
  if (const char* reason = SharedRules::RejectNonTopLevel(hwnd)) return reason;
  if (!allowSuspended && (GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_MINIMIZE | WS_MAXIMIZE))) {
    return "minimized or maximized";
  }
  if (const char* reason = SharedRules::RejectToolOrShellWindow(hwnd)) return reason;
  if (!IsWindowEnabled(hwnd)) return "disabled";
  return nullptr;
}

// Asks the window for its tracking limits and converts them to frame geometry
// using `margins` (the invisible resize borders). Both admission and restoration
// use this. Caller holds PlacementScope and uses physical coordinates. Returns
// nothing when the callback changed the window's geometry or physical state
// while answering, since the decision would then be about a different window.
static std::optional<SharedGeometry::FrameLimits> QueryFrameLimits(HWND hwnd, UINT dpi, const RECT& margins) {
  RECT before{};
  if (!GetWindowRect(hwnd, &before)) return std::nullopt;
  const bool wasVisible = IsWindowVisible(hwnd) != FALSE;
  const LONG_PTR showState = GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_MINIMIZE | WS_MAXIMIZE);
  // The window answers in its own DPI units even to this per-monitor-aware
  // scope (measured: a DPI-unaware window on a 200% monitor reports half its
  // physical limits). Offer the defaults in those units and scale the reply to
  // `dpi`, the monitor's physical DPI, exactly as MultiWM's probe does.
  const UINT reportedWindowDpi = GetDpiForWindow(hwnd);
  const UINT windowDpi = reportedWindowDpi ? reportedWindowDpi : dpi;
  const SharedGeometry::TrackingDefaults defaults = SharedGeometry::DefaultTrackingLimits(windowDpi);
  MINMAXINFO limits{};
  limits.ptMinTrackSize = defaults.minimum;
  limits.ptMaxTrackSize = defaults.maximum;
  SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
  RECT after{};
  if (RejectStructurally(hwnd, true) || !GetWindowRect(hwnd, &after) || !EqualRect(&before, &after) ||
      (IsWindowVisible(hwnd) != FALSE) != wasVisible ||
      (GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_MINIMIZE | WS_MAXIMIZE)) != showState) {
    return std::nullopt;
  }
  return SharedGeometry::TrackingLimitsToFrame(limits, defaults.minimum, defaults.maximum,
      windowDpi, dpi, margins.left + margins.right, margins.top + margins.bottom);
}

// One axis of FitSlotToLimits. `start`/`end` are the slot's edges, `areaStart`/
// `areaEnd` the work area's. Returns false when the minimum is larger than the
// whole work area on this axis.
static bool FitSlotAxis(LONG& start, LONG& end, LONG areaStart, LONG areaEnd, LONG minimum, LONG maximum) {
  const LONG size = end - start;
  // Capped below the slot: MultiWM centres it in its cell (CenterWithinMaximumSizes).
  if (SharedGeometry::CenterWithinMaximumSize(start, end, maximum)) return true;
  if (size >= minimum) return true;
  if (minimum > areaEnd - areaStart) return false;

  // Too small: MultiWM's constrained step takes the space from neighbouring
  // windows, so an edge on the work-area boundary stays and the edges facing
  // other windows move. With neighbours on both sides, MultiWM splits the growth
  // in proportion to their weights; growing from the centre approximates that.
  constexpr LONG kBoundaryTolerancePx = 1;
  const bool atStart = std::abs(start - areaStart) <= kBoundaryTolerancePx;
  const bool atEnd = std::abs(end - areaEnd) <= kBoundaryTolerancePx;
  if (atStart && !atEnd) {
    end = start + minimum;
  } else if (atEnd && !atStart) {
    start = end - minimum;
  } else {
    start -= (minimum - size) / 2;
    end = start + minimum;
  }
  if (start < areaStart) {
    end += areaStart - start;
    start = areaStart;
  }
  if (end > areaEnd) {
    start -= end - areaEnd;
    end = areaEnd;
  }
  return true;
}

// Where MultiWM will put a window whose size limits don't match the tiled
// `slot` it was predicted (frames, physical pixels):
//  - larger than its maximum: centred in the slot at the maximum, as MultiWM's
//    planner does, so the hint is exact;
//  - smaller than its minimum: grown at the edges that face other windows, an
//    approximation of MultiWM's constrained step, which MultiWM then refines.
// Returns nothing when the minimum doesn't fit the work area: MultiWM can't tile
// that window, so the companion gives no hint. Pure.
static std::optional<RECT> FitSlotToLimits(
    const RECT& slot, const RECT& workArea, const SharedGeometry::FrameLimits& limits) {
  RECT fitted = slot;
  if (!FitSlotAxis(fitted.left, fitted.right, workArea.left, workArea.right, limits.minWidth, limits.maxWidth) ||
      !FitSlotAxis(fitted.top, fitted.bottom, workArea.top, workArea.bottom, limits.minHeight, limits.maxHeight)) {
    return std::nullopt;
  }
  return fitted;
}

// How MultiWM treats the frame PredictFrame returns once it reads the window's
// size limits.
enum class TargetKind {
  Tile,      // A tiled slot, fitted to the limits as FitSlotToLimits does.
  Float,     // A floating frame, fitted by SharedGeometry::FitFloatingPlacementToLimits.
  KeepSize,  // Only moved; the app keeps its size.
};

// Where MultiWM will centre a new floating window, by the rule both mods share
// (SharedGeometry::PlanFloatingPlacement, as RepairFloatingGeometry uses it):
// sized by `source` from the window's matching size rule (`sizeRule`, may be
// null), its own frame `frame`, and the workspace's default floating frame. A
// preserve-size rule is ignored when `ignorePreserveSize` (FailureCenter, for a
// window overflow floats). Nothing when the size it needs is unknown.
static std::optional<SharedGeometry::FloatingPlacement> PredictFloatingPlacement(
    const CompanionAbi::WorkspaceHint& workspace, const CompanionAbi::SnapshotRule* sizeRule, const RECT& frame,
    bool resizable, SharedGeometry::FloatingSizeSource source, bool ignorePreserveSize) {
  SharedGeometry::FloatingRequest request;
  request.source = source;
  request.workArea = workspace.workArea;
  // The published frame is the default size centred and clamped in the work area.
  if (workspace.hasFloatingFrame) {
    request.defaultSize = SIZE{Width(workspace.floatingFrame), Height(workspace.floatingFrame)};
  }
  request.current = SIZE{Width(frame), Height(frame)};
  request.canResize = resizable;
  if (sizeRule) request.rule = SharedGeometry::ResolveFloatingSizeRule(*sizeRule, workspace.workArea, workspace.dpi);
  request.honorPreserveSize = !ignorePreserveSize;
  return SharedGeometry::PlanFloatingPlacement(request);
}

// The frame MultiWM will give `hwnd` (currently at `frame`), using its restore
// hint when known or Automatic discovery otherwise, and how MultiWM applies the
// window's size limits to it (`*kind`). `*floating` is the floating placement
// behind a floating frame, which the limits are fitted to. For a tiled slot it
// is where overflow would centre the window if its minimum turns out larger
// than the work area (MultiWM then floats it); nothing when overflow routes
// such a window instead. Returns nothing, with `*reason` set, when MultiWM
// won't manage it or the snapshot can't say where it goes.
static std::optional<RECT> PredictFrame(HWND hwnd, const RECT& frame, const HintInputs& inputs, TargetKind* kind,
                                        std::optional<SharedGeometry::FloatingPlacement>* floating,
                                        const char** reason) {
  using namespace CompanionAbi;
  *kind = TargetKind::Tile;
  floating->reset();
  auto reject = [&](const char* why) -> std::optional<RECT> {
    *reason = why;
    return std::nullopt;
  };
  constexpr uint32_t kComplete = kFlagRunning | kFlagRulesComplete | kFlagTrackedComplete;
  if ((inputs.flags & kComplete) != kComplete || !inputs.rules) return reject("no complete snapshot");
  if (!(inputs.flags & kFlagAutomaticMode)) return reject("Manual mode");
  if (!inputs.workspace) return reject("no workspace on this monitor");

  const std::vector<SnapshotRule>& rules = *inputs.rules;
  SharedRules::LazyWindowFacts facts(hwnd, CurrentProcessName);
  auto matches = [&](WindowRuleTreatment treatment) {
    return SharedRules::FindFirstMatchingRule(rules, treatment, facts);
  };
  if (matches(WindowRuleTreatment::Exclude)) return reject("excluded by a rule");
  const WorkspaceHint& workspace = *inputs.workspace;
  if (inputs.tracked) {
    if (inputs.intent == PlacementIntent::KeepFloating) return reject("retaining this window's floating treatment");
    if (inputs.intent != PlacementIntent::Restore && inputs.intent != PlacementIntent::Arrival) {
      return reject("known window has no usable ownership or treatment");
    }
    if (workspace.floatingLayout) return reject("known window's destination uses the Floating layout");
    if (!inputs.restoreFrameMargins || inputs.restoreMarginsDpi != workspace.dpi) {
      return reject("no current restore border measurement");
    }
    if (inputs.intent == PlacementIntent::Arrival) {
      // Migration retains treatment and uses the same destination insertion as
      // admission. Never rerun opening rules or guess how overflow will route it.
      if (!workspace.hasTiledFrame || workspace.newWindowFloats ||
          Width(workspace.tiledFrame) <= 0 || Height(workspace.tiledFrame) <= 0) {
        return reject("no tiled arrival hint for the destination workspace");
      }
      *reason = "using MultiWM's destination arrival tile";
      return workspace.tiledFrame;
    }
    if (!inputs.restoreFrame || Width(*inputs.restoreFrame) <= 0 || Height(*inputs.restoreFrame) <= 0) {
      return reject("no restore tile for this workspace member");
    }
    *reason = "using MultiWM's restore tile";
    return inputs.restoreFrame;
  }
  if (matches(WindowRuleTreatment::InitialPlacement)) return reject("routed by an Open-on-monitor rule");
  const bool floatingRule =
      matches(WindowRuleTreatment::AlwaysFloating) || matches(WindowRuleTreatment::DefaultToFloating);
  const bool resizable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_SIZEBOX) != 0;
  if (!resizable && !floatingRule) return reject("fixed-size window");

  // Rule-floated windows are centered (RuleFloatedCenter), in every layout: at
  // the size a matching override-size rule names, else at the app's own size
  // without clamping. A window that can't resize keeps its size either way.
  using SharedGeometry::FloatingSizeSource;
  const SnapshotRule* sizeRule = matches(WindowRuleTreatment::FloatingPlacementOverride);
  if (floatingRule) {
    *floating = PredictFloatingPlacement(workspace, sizeRule, frame, resizable, FloatingSizeSource::Own, false);
    if (!*floating) return reject("no frame to centre");
    *kind = (*floating)->moveOnly ? TargetKind::KeepSize : TargetKind::Float;
    if (!(*floating)->moveOnly) *reason = "rule-floated at its size rule's size";
    return (*floating)->frame;
  }

  if (workspace.floatingLayout) {
    // NewWindowCenter: the default size unless a size rule says otherwise.
    *kind = TargetKind::Float;
    *floating = PredictFloatingPlacement(workspace, sizeRule, frame, resizable, FloatingSizeSource::Fallback, false);
    if (!*floating) return reject("no floating default");
    return (*floating)->frame;
  }

  // Automatic overflow floats a window it can't tile and, unless it routes the
  // window elsewhere, centres it (FailureCenter).
  if (inputs.flags & kFlagOverflowCentersFloat) {
    *floating = PredictFloatingPlacement(workspace, sizeRule, frame, resizable, FloatingSizeSource::Fallback, true);
  }
  if (!workspace.hasTiledFrame) {
    if (!workspace.newWindowFloats) return reject("no free tiled slot");
    if (!*floating) return reject("overflow routes the window");
    *kind = TargetKind::Float;
    *reason = "overflow floats it";
    return (*floating)->frame;
  }
  return workspace.tiledFrame;
}

static std::wstring ClassNameOf(HWND hwnd) {
  wchar_t className[128] = L"";
  GetClassNameW(hwnd, className, ARRAYSIZE(className));
  return className;
}

// Windhawk's per-mod logging switch gates this: with logging off, Wh_Log skips
// its arguments, so not even the class name is read.
static void LogDecision(HWND hwnd, const char* path, const char* outcome) {
  Wh_Log(L"%S hwnd=%p class=%s: %S", path, hwnd, ClassNameOf(hwnd).c_str(), outcome);
}

struct PredictedPlacement {
  RECT windowRect;
  bool keepSize;
};

// Calculates without placing. `proposed` is the app's intended window rectangle
// in physical screen pixels, including resize borders. DWM margins translate
// new windows; known members use the published measured borders and restore tile.
// Caller holds PlacementScope on the window's
// own thread, so application callbacks cannot recursively predict another show.
static std::optional<PredictedPlacement> PredictWindowPlacement(
    HWND hwnd, const RECT& proposed, const char* path) {
  PhysicalCoordinatesScope physical;
  RECT windowRect{};
  if (!GetWindowRect(hwnd, &windowRect)) return std::nullopt;
  const RECT currentFrame = FrameOf(hwnd, windowRect);
  RECT margins = SharedGeometry::FrameMargins(windowRect, currentFrame);
  const RECT frame = SharedGeometry::RemoveFrameMargins(proposed, margins);

  // The monitor MultiWM will assign: the one holding the frame (GetWindowPhysicalMonitor).
  HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONULL);
  if (!monitor) monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
    LogDecision(hwnd, path, "off every monitor");
    return std::nullopt;
  }
  if (SharedGeometry::CoversMonitor(proposed, monitorInfo.rcMonitor)) {
    LogDecision(hwnd, path, "fullscreen");
    return std::nullopt;
  }

  HintInputs inputs;
  if (!ReadHintInputs(hwnd, monitorInfo.rcMonitor, &inputs)) {
    LogDecision(hwnd, path, "no snapshot (MultiWM not running?)");
    return std::nullopt;
  }
  if (inputs.tracked && IsWindowVisible(hwnd)) return std::nullopt;
  // Known hidden members use the same measured borders as minimized restores;
  // a hidden window's current DWM bounds can be missing or stale.
  if (inputs.tracked && inputs.restoreFrameMargins) margins = *inputs.restoreFrameMargins;
  TargetKind kind = TargetKind::Tile;
  std::optional<SharedGeometry::FloatingPlacement> floating;
  const char* reason = nullptr;
  const std::optional<RECT> target = PredictFrame(hwnd, frame, inputs, &kind, &floating, &reason);
  if (!target) {
    LogDecision(hwnd, path, reason);
    return std::nullopt;
  }

  // MultiWM predicted the slot without knowing this window's size limits. Apply
  // them the way MultiWM will once it reads them: tiles by FitSlotToLimits,
  // floating frames by the rule both mods share.
  RECT frameTarget = *target;
  if (kind != TargetKind::KeepSize) {
    const UINT dpi = inputs.workspace->dpi ? inputs.workspace->dpi : 96;
    const auto limits = QueryFrameLimits(hwnd, dpi, margins);
    if (!limits) return std::nullopt;
    if (kind == TargetKind::Float) {
      frameTarget = SharedGeometry::FitFloatingPlacementToLimits(*floating, *limits);
    } else if (const auto fitted = FitSlotToLimits(frameTarget, inputs.workspace->workArea, *limits)) {
      frameTarget = *fitted;
    } else if (floating) {
      // A minimum larger than the work area: MultiWM floats the window
      // (IntrinsicMinimum) and overflow centres it.
      frameTarget = SharedGeometry::FitFloatingPlacementToLimits(*floating, *limits);
      reason = "minimum larger than the work area: overflow floats it";
    } else {
      LogDecision(hwnd, path, "minimum larger than the work area; overflow routes it");
      return std::nullopt;
    }
  }
  if (reason) LogDecision(hwnd, path, reason);
  // The snapshot holds visible frames; SetWindowPos takes the window rectangle,
  // which includes the invisible resize borders.
  const RECT placement = SharedGeometry::AddFrameMargins(frameTarget, margins);
  return PredictedPlacement{placement, kind == TargetKind::KeepSize};
}

// --- Settling ----------------------------------------------------------------------
//
// After the companion places a window at its show, the app may still move it
// before MultiWM gets to it: WeChat restores its pre-hide position 64 ms after
// showing, ApplicationFrameHost the app's last position just before uncloaking
// (traced). MultiWM would move it back, so the move is a visible jump. Until
// MultiWM takes the window over, the app's own moves are kept at the frame the
// companion chose (show_hooks.h, SteerSettlingWindow).
//
// "Taken over" is read from MultiWM itself, never guessed: its snapshot entry
// for the window changes once it has processed the show (WindowFacts), or its
// own placement arrives. One window per thread: the last one placed.
struct SettlingWindow {
  HWND hwnd = nullptr;
  RECT windowRect{};  // Physical screen pixels, including resize borders.
  WindowFacts facts;  // MultiWM's view of the window when it was placed.
};
static thread_local SettlingWindow g_settling;

static bool IsUwpFrame(HWND hwnd) {
  wchar_t className[32];
  return GetClassNameW(hwnd, className, ARRAYSIZE(className)) && wcscmp(className, L"ApplicationFrameWindow") == 0;
}

// Reads the title without sending the window a message.
static bool HasTitle(HWND hwnd) {
  wchar_t first[2];
  return InternalGetWindowText(hwnd, first, ARRAYSIZE(first)) > 0;
}

static void EndSettling() { g_settling = {}; }

// Starts settling `hwnd` at `windowRect`, replacing any earlier window on this
// thread. A spare UWP frame (no title yet) is not settled: no app uses it yet,
// and the launch that takes it places it again (show_hooks.h).
static void BeginSettling(HWND hwnd, const RECT& windowRect) {
  WindowFacts facts;
  if ((IsUwpFrame(hwnd) && !HasTitle(hwnd)) || !ReadWindowFacts(hwnd, &facts)) {
    EndSettling();
    return;
  }
  g_settling = {hwnd, windowRect, facts};
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
  const BOOL moved = g_setWindowPosForOwnMoves(hwnd, nullptr, placement.left, placement.top,
                                               Width(placement), Height(placement), flags);
  LogDecision(hwnd, path, moved ? "placed at MultiWM's frame" : "SetWindowPos failed");
  // A window MultiWM only centres (keepSize) has no exact frame to hold.
  if (moved && !prediction->keepSize) BeginSettling(hwnd, placement);
  return moved != FALSE;
}

}  // namespace Companion
