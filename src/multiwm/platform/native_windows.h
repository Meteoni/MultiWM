// Native window observations, eligibility and checked placement.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

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

  const RECT margins = SharedGeometry::FrameMargins(windowRect, frame);
  if (outWidth) *outWidth = std::max<LONG>(0, margins.left + margins.right);
  if (outHeight) *outHeight = std::max<LONG>(0, margins.top + margins.bottom);
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

  // The window answers in its own DPI units, and Windows does not convert the
  // reply for a sender with different awareness (measured: a DPI-unaware window
  // on a 200% monitor reports half its physical limits, to any sender). Offer
  // the defaults in the window's units too, as Windows does when it sends this
  // message, then scale everything to the monitor's physical pixels. For a
  // per-monitor-aware window both DPIs are equal and nothing is scaled.
  const UINT reportedWindowDpi = GetDpiForWindow(hwnd);
  const UINT windowDpi = reportedWindowDpi ? reportedWindowDpi : constraints.dpi;
  const SharedGeometry::TrackingDefaults defaults = SharedGeometry::DefaultTrackingLimits(windowDpi);
  MINMAXINFO mmi{};
  mmi.ptMinTrackSize = defaults.minimum;
  mmi.ptMaxTrackSize = defaults.maximum;

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

  const auto frame = SharedGeometry::TrackingLimitsToFrame(
      mmi, defaults.minimum, defaults.maximum, windowDpi, constraints.dpi, decorationWidth, decorationHeight);
  constraints.minWidth = frame.minWidth;
  constraints.minHeight = frame.minHeight;
  constraints.maxWidth = frame.maxWidth;
  constraints.maxHeight = frame.maxHeight;

  return constraints;
}

// True when `hwnd`'s window rectangle covers its whole monitor: how Chromium and
// similar apps look in fullscreen while Windows still reports them as restored.
static bool CoversItsMonitor(HWND hwnd) {
  RECT windowRect{};
  if (!GetWindowRect(hwnd, &windowRect)) return false;
  HMONITOR monitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONULL);
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  return monitor && GetMonitorInfoW(monitor, &monitorInfo) &&
         SharedGeometry::CoversMonitor(windowRect, monitorInfo.rcMonitor);
}

static SuspensionReason GetPhysicalSuspensionReason(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return SuspensionReason::None;
  if (IsIconic(hwnd)) return SuspensionReason::Minimized;
  // IsWindowVisible reads user-mode state; check it before the DWM cloak query so
  // the many invisible top-level HWNDs seen during EnumWindows skip that call.
  if (!IsWindowVisible(hwnd)) return SuspensionReason::Hidden;
  // Fail open on purpose: a failed query (DWM restarting, or the window being
  // destroyed right now) reads as "not app-cloaked". Treating failure as Hidden
  // would suspend every tiled window while DWM is unavailable and collapse each
  // layout, then rebuild it. A briefly app-cloaked window being tiled is the
  // smaller harm, and the next settled pass corrects it. The overflow
  // destination scan (window_routing.h) differs deliberately: it is a trial
  // that can simply be retried, so there a failure means "incomplete".
  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if (cloaked & DWM_CLOAKED_APP) return SuspensionReason::Hidden;
  if (IsZoomed(hwnd)) return SuspensionReason::Maximized;

  // Fullscreen and native maximize have identical suspend/save-slot/restore
  // semantics here.
  return CoversItsMonitor(hwnd) ? SuspensionReason::Maximized : SuspensionReason::None;
}

// True when `hwnd` can't take part in a layout right now: it is minimized,
// hidden, maximized or fullscreen (GetPhysicalSuspensionReason), or cloaked for
// any reason, which is how windows on other desktops look. One cloak query; a
// failed one reads as not cloaked, as in GetPhysicalSuspensionReason.
static bool IsSuspendedOrCloaked(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) return true;
  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  return cloaked || IsZoomed(hwnd) || CoversItsMonitor(hwnd);
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
    // No floating frame yet. The frame a window has when it is first recorded
    // is often a placeholder (shown at the system minimum, sized afterwards) or,
    // with the companion, already its tile. Its first float uses an
    // override-size rule or the configured default (RepairFloatingGeometry).
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

// CollectTileWindowsForMonitors for one monitor.
static std::vector<HWND> CollectTileWindows(HMONITOR monitor, const GUID& desktopId) {
  if (!monitor) return {};
  return std::move(CollectTileWindowsForMonitors({monitor}, desktopId).front());
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

static bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle) {
  return SharedRules::ContainsInsensitive(text, needle);
}

static std::wstring GetWindowProcessName(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return {};

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  std::wstring result = SharedRules::ProcessImageFileName(process);
  CloseHandle(process);
  return result;
}

// The first configured rule with `treatment` that matches `hwnd`, or nullptr.
// The matching itself is shared with the companion mod (shared/window_rules.h),
// so both mods classify a window the same way.
// This overload takes the window's facts: reuse one LazyWindowFacts for several
// treatments of the same window so its class, title and process name (which can
// cost an OpenProcess) are each read at most once.
static const WindowRule* FindMatchingWindowRule(
    SharedRules::LazyWindowFacts& facts, WindowRuleTreatment treatment) {
  if (!facts.Window() || g_settings.windowRules.empty()) return nullptr;
  return SharedRules::FindFirstMatchingRule(g_settings.windowRules, treatment, facts);
}

static const WindowRule* FindMatchingWindowRule(
    HWND hwnd, WindowRuleTreatment treatment) {
  SharedRules::LazyWindowFacts facts(hwnd, GetWindowProcessName);
  return FindMatchingWindowRule(facts, treatment);
}

// Discovery waits for initial placement before assigning workspace ownership. Explicit
// admission bypasses this policy and overrides pending initial placement.
static bool IsWindowAwaitingInitialPlacement(HWND hwnd) {
  return g_wm.routing.ready &&
         (g_wm.routing.pending.count(hwnd) ||
          (!g_wm.routing.evaluated.count(hwnd) &&
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

static bool ShouldWindowStartFloating(SharedRules::LazyWindowFacts& facts, bool* alwaysFloating) {
  const bool always = FindMatchingWindowRule(facts, WindowRuleTreatment::AlwaysFloating) != nullptr;
  if (alwaysFloating) *alwaysFloating = always;
  return always || FindMatchingWindowRule(facts, WindowRuleTreatment::DefaultToFloating) != nullptr;
}

static bool ShouldWindowStartFloating(HWND hwnd, bool* alwaysFloating = nullptr) {
  SharedRules::LazyWindowFacts facts(hwnd, GetWindowProcessName);
  return ShouldWindowStartFloating(facts, alwaysFloating);
}

// Applies stable structural and exclusion policy only; visibility, show state,
// desktop, and monitor participation are checked separately. Tracked windows
// tolerate a temporarily missing resize style so transient app UI cannot evict them.
static bool WindowCanBeManaged(HWND hwnd, bool allowFixedSize) {
  if (!hwnd) return false;

  // Reject structurally ineligible HWNDs before evaluating configurable exclusion
  // rules. Process-name exclusions can require OpenProcess/QueryFullProcessImageName,
  // so there is no reason to pay that cost for tool/owned/child windows.
  if (SharedRules::RejectNonTopLevel(hwnd)) return false;
  SharedRules::LazyWindowFacts facts(hwnd, GetWindowProcessName);

  // window could temporarily lose its WS_SizeBox.
  // (example: Office save/discard dialog)
  // A Default-to-floating rule is also an explicit Automatic-mode admission
  // policy for naturally fixed-size top-level windows. Always-floating is also
  // eligible during explicit Manual admission; centering never resizes either.
  if (!allowFixedSize && !(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_SIZEBOX) &&
      !IsWindowTrackedInAnyState(hwnd)) {
    bool alwaysFloating = false;
    const bool startsFloating = ShouldWindowStartFloating(facts, &alwaysFloating);
    if (!alwaysFloating && !(RuntimeLifecycle::IsAutomaticMode() && startsFloating)) return false;
  }

  if (SharedRules::RejectToolOrShellWindow(hwnd)) return false;
  return !FindMatchingWindowRule(facts, WindowRuleTreatment::Exclude);
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
  if (IsSuspendedOrCloaked(hwnd)) {
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
