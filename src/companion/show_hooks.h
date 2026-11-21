// Hooks on the ways a top-level window becomes visible.
// Part of the MultiWM Companion mod; see the include order in src/companion/multiwm-companion.cpp.
//
// The same show paths Window Centralizer and Mobile Open Animation cover: the
// win32u entry points (user32 and the dialog manager call them directly, so
// hooking ShowWindow alone misses most dialogs), deferred positioning,
// SetWindowPlacement (shows inside the kernel) and visible creation.

namespace Companion {

using NtUserShowWindow_t = BOOL(WINAPI*)(HWND, int);
static NtUserShowWindow_t g_unhookedShowWindow = nullptr;
static NtUserShowWindow_t g_unhookedShowWindowAsync = nullptr;
static decltype(&DeferWindowPos) g_unhookedDeferWindowPos = nullptr;
static decltype(&CreateWindowExW) g_unhookedCreateWindowExW = nullptr;
static decltype(&CreateWindowExA) g_unhookedCreateWindowExA = nullptr;
static decltype(&DefWindowProcW) g_unhookedDefWindowProcW = nullptr;
static decltype(&DefWindowProcA) g_unhookedDefWindowProcA = nullptr;

// ApplicationFrameHost keeps a spare UWP frame: it creates and shows the next
// frame, still cloaked, whenever an app takes the current one, so that show is
// often minutes before any app uses it. A launch then reuses the spare: it sets
// the title, sometimes moves the frame to where the app last closed, and
// uncloaks it, with no show call. (Traced on Microsoft Store: the jump at launch
// was the spare's stale position or the app's saved one.) Until it uncloaks,
// such a frame counts as not shown yet. MultiWM ignores cloaked windows, so an
// untracked one is still a new window to it.
//
// The launch is recognised by the spare's title going from empty to set. Other
// cloaked frames (an app on another virtual desktop, say) already have a title,
// so their title changes are left alone. ApplicationFrameHost's follow-up move
// to the app's saved position is held by settling (placement.h).
static bool IsCloakedUwpFrame(HWND hwnd) {
  if (!IsWindowVisible(hwnd) || !IsUwpFrame(hwnd)) return false;
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// Calls that need a hint: a hidden window's show.
static bool NeedsPositionHint(HWND hwnd, UINT flags) {
  return (flags & SWP_SHOWWINDOW) && !IsWindowVisible(hwnd);
}

// True for a WM_SETTEXT that may be an app taking a spare UWP frame. Read
// before the title changes.
static bool MayAttachUwpFrame(HWND hwnd, UINT message) {
  return message == WM_SETTEXT && !g_placing && IsCloakedUwpFrame(hwnd) && !HasTitle(hwnd);
}

// After the title is set (so title rules match): place the frame at the current
// prediction, which also starts settling it.
static void PlaceAttachingUwpFrame(HWND hwnd) {
  if (HasTitle(hwnd) && IsCloakedUwpFrame(hwnd)) PlaceAtPredictedFrame(hwnd, "UWP frame launch");
}

// Windows sets this WINDOWPOS flag (undocumented, seen as 0x8124 in traces) for
// a minimize, maximize or restore.
constexpr UINT kSwpStateChanged = 0x8000;

// Holds a settling window (placement.h) at the companion's frame. Every
// positioning call (SetWindowPos, MoveWindow, SetWindowPlacement, the end of a
// DeferWindowPos batch) sends WM_WINDOWPOSCHANGING, and changing its WINDOWPOS
// changes the move (documented), so this one place covers them all. Runs before
// DefWindowProc, after the window's own procedure. Settling ends:
//  - when MultiWM has taken the window: its snapshot entry changed, or its own
//    placement arrived (WM_WINDOWPOSCHANGED from another thread; MultiWM uses
//    SWP_NOSENDCHANGING, so its moves never reach the steering);
//  - when anything but the app's own thread positions it, the user moves or
//    resizes it, or it is minimized, maximized, hidden or destroyed.
// From then on MultiWM handles the app's moves as before.
static void SteerSettlingWindow(HWND hwnd, UINT message, LPARAM lparam) {
  if (!hwnd || hwnd != g_settling.hwnd || g_placing) return;
  if (message == WM_NCDESTROY) {
    EndSettling();
    return;
  }
  if (message != WM_WINDOWPOSCHANGING && message != WM_WINDOWPOSCHANGED) return;
  auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
  if (!position || position->hwnd != hwnd) return;
  const bool fromOtherThread = InSendMessageEx(nullptr) != ISMEX_NOSEND;
  if (message == WM_WINDOWPOSCHANGED) {
    if (fromOtherThread || (position->flags & SWP_NOSENDCHANGING)) EndSettling();
    return;
  }
  if ((position->flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE) &&
      !(position->flags & SWP_HIDEWINDOW)) {
    return;  // Z-order or activation only.
  }
  GUITHREADINFO gui{sizeof(gui)};
  WindowFacts facts;
  if (fromOtherThread || (position->flags & (SWP_HIDEWINDOW | kSwpStateChanged)) || IsIconic(hwnd) ||
      IsZoomed(hwnd) || (GetGUIThreadInfo(GetCurrentThreadId(), &gui) && (gui.flags & GUI_INMOVESIZE)) ||
      !ReadWindowFacts(hwnd, &facts) || !(facts == g_settling.facts)) {
    EndSettling();
    return;
  }
  const auto target = ConvertScreenRect(hwnd, g_settling.windowRect, PhysicalToLogicalPointForPerMonitorDPI);
  if (!target) {
    EndSettling();
    return;
  }
  position->x = target->left;
  position->y = target->top;
  position->cx = Width(*target);
  position->cy = Height(*target);
  position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
  LogDecision(hwnd, "WM_WINDOWPOSCHANGING", "held at the companion's frame until MultiWM takes the window");
}

// ShowWindow and ShowWindowAsync: a restore of a minimized or maximized member
// is prepared first; a hidden window being shown restored is placed while still
// hidden. `show` is the original entry point, read at call time because Windhawk
// fills it in after the hooks are applied. `path` names the call in logs.
// Structural eligibility rejects cross-thread windows (an async show from another
// thread) before any native probe.
static BOOL ShowWithHint(NtUserShowWindow_t show, HWND hwnd, int command, const char* path,
                         const char* restorePath) {
  if (PrepareRestorePlacement(hwnd, command, restorePath)) {
    PlacementScope placing;
    return show(hwnd, command);
  }
  if (hwnd && IsRestoredShowCommand(command) && !IsWindowVisible(hwnd)) PlaceAtPredictedFrame(hwnd, path);
  return show(hwnd, command);
}

static BOOL WINAPI ShowWindowHook(HWND hwnd, int command) {
  return ShowWithHint(g_unhookedShowWindow, hwnd, command, "ShowWindow", "ShowWindow restore");
}

static BOOL WINAPI ShowWindowAsyncHook(HWND hwnd, int command) {
  return ShowWithHint(g_unhookedShowWindowAsync, hwnd, command, "ShowWindowAsync", "ShowWindowAsync restore");
}

// DefWindowProcW and DefWindowProcA: default SC_RESTORE handling restores a
// member into its tile; every message passes through the settling and UWP-frame
// checks. `defWindowProc` is the original, read at call time.
static LRESULT DefWindowProcWithHint(
    decltype(&DefWindowProcW) defWindowProc, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_RESTORE &&
      PrepareRestorePlacement(hwnd, SW_RESTORE, "SC_RESTORE")) {
    PlacementScope placing;
    return defWindowProc(hwnd, message, wparam, lparam);
  }
  SteerSettlingWindow(hwnd, message, lparam);
  const bool attaching = MayAttachUwpFrame(hwnd, message);
  const LRESULT result = defWindowProc(hwnd, message, wparam, lparam);
  if (attaching) PlaceAttachingUwpFrame(hwnd);
  return result;
}

static LRESULT WINAPI DefWindowProcWHook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcWithHint(g_unhookedDefWindowProcW, hwnd, message, wparam, lparam);
}

static LRESULT WINAPI DefWindowProcAHook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcWithHint(g_unhookedDefWindowProcA, hwnd, message, wparam, lparam);
}

// Both immediate and deferred placement take the same geometry. Rewrite it
// without committing a move; a deferred batch must remain deferred. Callers
// hold PlacementScope through the original API call to prevent nested hints.
// Returns the frame to settle at (physical) when the hint set an exact frame.
static std::optional<RECT> AdjustPositionForHint(WINDOWPOS& position, const char* path) {
  const HWND hwnd = position.hwnd;
  RECT current{};
  if (GetWindowRect(hwnd, &current)) {
    const LONG left = (position.flags & SWP_NOMOVE) ? current.left : position.x;
    const LONG top = (position.flags & SWP_NOMOVE) ? current.top : position.y;
    const LONG width = (position.flags & SWP_NOSIZE) ? Width(current) : position.cx;
    const LONG height = (position.flags & SWP_NOSIZE) ? Height(current) : position.cy;
    const auto proposed = ConvertScreenRect(
        hwnd, {left, top, left + width, top + height}, LogicalToPhysicalPointForPerMonitorDPI);
    const auto prediction = proposed ? PredictWindowPlacement(hwnd, *proposed, path) : std::nullopt;
    const auto target = prediction ? ConvertScreenRect(hwnd, prediction->windowRect,
                                                       PhysicalToLogicalPointForPerMonitorDPI) : std::nullopt;
    if (target) {
      position.x = target->left;
      position.y = target->top;
      position.flags &= ~SWP_NOMOVE;
      if (!prediction->keepSize) {
        position.cx = Width(*target);
        position.cy = Height(*target);
        position.flags &= ~SWP_NOSIZE;
      }
      LogDecision(hwnd, path, "using MultiWM's predicted placement");
      if (!prediction->keepSize) return prediction->windowRect;
    }
  }
  return std::nullopt;
}

static BOOL WINAPI SetWindowPosHook(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (g_placing || !hwnd || !NeedsPositionHint(hwnd, flags) || RejectStructurally(hwnd)) {
    return g_unhookedSetWindowPos(hwnd, after, x, y, cx, cy, flags);
  }
  BOOL result = FALSE;
  std::optional<RECT> settleAt;
  {
    PlacementScope placing;
    WINDOWPOS position{hwnd, after, x, y, cx, cy, flags};
    settleAt = AdjustPositionForHint(position, "SetWindowPos");
    // Preserve the app's single native call, flags and result, including failure.
    result = g_unhookedSetWindowPos(hwnd, after, position.x, position.y, position.cx, position.cy, position.flags);
  }
  if (result && settleAt) BeginSettling(hwnd, *settleAt);
  return result;
}

static HDWP WINAPI DeferWindowPosHook(
    HDWP batch, HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (g_placing || !batch || !hwnd || !NeedsPositionHint(hwnd, flags) || RejectStructurally(hwnd)) {
    return g_unhookedDeferWindowPos(batch, hwnd, after, x, y, cx, cy, flags);
  }
  PlacementScope placing;
  WINDOWPOS position{hwnd, after, x, y, cx, cy, flags};
  AdjustPositionForHint(position, "DeferWindowPos");
  return g_unhookedDeferWindowPos(batch, hwnd, after,
                                 position.x, position.y, position.cx, position.cy, position.flags);
}

// Programs that restore a saved position (7-Zip's file manager, for one) show
// their window this way, with no show call to hook.
static BOOL WINAPI SetWindowPlacementHook(HWND hwnd, const WINDOWPLACEMENT* placement) {
  if (!g_placing && hwnd && placement && placement->length == sizeof(*placement) &&
      (IsIconic(hwnd) || IsZoomed(hwnd))) {
    PlacementScope placing;
    WINDOWPLACEMENT adjusted = *placement;
    if (AdjustRestorePlacement(hwnd, &adjusted, "SetWindowPlacement restore")) {
      return g_unhookedSetWindowPlacement(hwnd, &adjusted);
    }
    return g_unhookedSetWindowPlacement(hwnd, placement);
  }
  if (g_placing || !hwnd || !placement || placement->length != sizeof(*placement) ||
      !IsRestoredShowCommand(static_cast<int>(placement->showCmd)) ||
      IsWindowVisible(hwnd) || RejectStructurally(hwnd)) {
    return g_unhookedSetWindowPlacement(hwnd, placement);
  }
  std::optional<RECT> settleAt;
  BOOL result = FALSE;
  {
    PlacementScope placing;
    // Non-tool top-level windows store rcNormalPosition in workspace coordinates.
    // Convert to screen coordinates before prediction and back at its destination.
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    std::optional<WINDOWPLACEMENT> adjusted;
    if (GetMonitorInfoW(MonitorFromRect(&placement->rcNormalPosition, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
      const RECT proposedScreen = WorkspaceToScreen(placement->rcNormalPosition, monitorInfo);
      const auto proposed = ConvertScreenRect(hwnd, proposedScreen, LogicalToPhysicalPointForPerMonitorDPI);
      const auto prediction = proposed ? PredictWindowPlacement(hwnd, *proposed, "SetWindowPlacement") : std::nullopt;
      const auto target = prediction ? ConvertScreenRect(hwnd, prediction->windowRect,
                                                         PhysicalToLogicalPointForPerMonitorDPI) : std::nullopt;
      if (target && GetMonitorInfoW(MonitorFromRect(&*target, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        adjusted = *placement;
        adjusted->rcNormalPosition = ScreenToWorkspace(*target, monitorInfo);
        LogDecision(hwnd, "SetWindowPlacement", "using MultiWM's predicted placement");
        if (!prediction->keepSize) settleAt = prediction->windowRect;
      }
    }
    result = g_unhookedSetWindowPlacement(hwnd, adjusted ? &*adjusted : placement);
  }
  if (result && settleAt) BeginSettling(hwnd, *settleAt);
  return result;
}

// Visible creation can show inside the kernel. Adjust as soon as creation
// returns; apps that paint during creation may already have exposed a frame.
static void PlaceIfCreatedVisible(HWND hwnd, DWORD style) {
  if (hwnd && (style & WS_VISIBLE) && !(style & WS_CHILD)) PlaceAtPredictedFrame(hwnd, "CreateWindowEx");
}

static HWND WINAPI CreateWindowExWHook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x,
                                       int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                                       LPVOID param) {
  const HWND hwnd = g_unhookedCreateWindowExW(exStyle, className, windowName, style, x, y, width, height, parent,
                                              menu, instance, param);
  PlaceIfCreatedVisible(hwnd, style);
  return hwnd;
}

static HWND WINAPI CreateWindowExAHook(DWORD exStyle, LPCSTR className, LPCSTR windowName, DWORD style, int x,
                                       int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                                       LPVOID param) {
  const HWND hwnd = g_unhookedCreateWindowExA(exStyle, className, windowName, style, x, y, width, height, parent,
                                              menu, instance, param);
  PlaceIfCreatedVisible(hwnd, style);
  return hwnd;
}

// Registers every hook. Windhawk applies them after Wh_ModInit returns, and
// fills in the g_unhooked* pointers then. Returns false if a hook failed to
// register; the ones that registered still work.
static bool InstallShowHooks() {
  bool ok = true;
  if (HMODULE win32u = GetModuleHandleW(L"win32u.dll")) {
    if (auto showWindow = reinterpret_cast<NtUserShowWindow_t>(GetProcAddress(win32u, "NtUserShowWindow"))) {
      ok &= WindhawkUtils::SetFunctionHook(showWindow, ShowWindowHook, &g_unhookedShowWindow);
    }
    if (auto showWindowAsync = reinterpret_cast<NtUserShowWindow_t>(GetProcAddress(win32u, "NtUserShowWindowAsync"))) {
      ok &= WindhawkUtils::SetFunctionHook(showWindowAsync, ShowWindowAsyncHook, &g_unhookedShowWindowAsync);
    }
    if (auto setWindowPos = reinterpret_cast<SetWindowPos_t>(GetProcAddress(win32u, "NtUserSetWindowPos"))) {
      ok &= WindhawkUtils::SetFunctionHook(setWindowPos, SetWindowPosHook, &g_unhookedSetWindowPos);
    }
  }
  ok &= WindhawkUtils::SetFunctionHook(DeferWindowPos, DeferWindowPosHook, &g_unhookedDeferWindowPos);
  ok &= WindhawkUtils::SetFunctionHook(DefWindowProcW, DefWindowProcWHook, &g_unhookedDefWindowProcW);
  ok &= WindhawkUtils::SetFunctionHook(DefWindowProcA, DefWindowProcAHook, &g_unhookedDefWindowProcA);
  ok &= WindhawkUtils::SetFunctionHook(SetWindowPlacement, SetWindowPlacementHook, &g_unhookedSetWindowPlacement);
  ok &= WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExWHook, &g_unhookedCreateWindowExW);
  ok &= WindhawkUtils::SetFunctionHook(CreateWindowExA, CreateWindowExAHook, &g_unhookedCreateWindowExA);
  return ok;
}

}  // namespace Companion
