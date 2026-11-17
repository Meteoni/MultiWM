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
static decltype(&SetWindowPlacement) g_unhookedSetWindowPlacement = nullptr;
static decltype(&CreateWindowExW) g_unhookedCreateWindowExW = nullptr;
static decltype(&CreateWindowExA) g_unhookedCreateWindowExA = nullptr;

// Show commands that bring up a restored window. Maximized and minimized shows
// are suspended by MultiWM rather than tiled, so they are left alone.
static bool IsRestoredShowCommand(int command) {
  if (command == SW_SHOWDEFAULT) {
    STARTUPINFOW startup{sizeof(startup)};
    GetStartupInfoW(&startup);
    command = (startup.dwFlags & STARTF_USESHOWWINDOW) ? startup.wShowWindow : SW_SHOWNORMAL;
  }
  return command == SW_SHOWNORMAL || command == SW_SHOW || command == SW_SHOWNA ||
         command == SW_SHOWNOACTIVATE || command == SW_RESTORE;
}

static BOOL WINAPI ShowWindowHook(HWND hwnd, int command) {
  if (hwnd && IsRestoredShowCommand(command) && !IsWindowVisible(hwnd)) {
    PlaceAtPredictedFrame(hwnd, "ShowWindow");
  }
  return g_unhookedShowWindow(hwnd, command);
}

static BOOL WINAPI ShowWindowAsyncHook(HWND hwnd, int command) {
  if (hwnd && IsRestoredShowCommand(command) && !IsWindowVisible(hwnd)) {
    // PlaceAtPredictedFrame rejects other-thread windows. Never turn an async
    // request into a synchronous cross-thread move or send it a minimum query.
    PlaceAtPredictedFrame(hwnd, "ShowWindowAsync");
  }
  return g_unhookedShowWindowAsync(hwnd, command);
}

// Geometry arguments use the caller's DPI coordinates. The conversion APIs use
// the window's context, so skip unusual mixed-context calls rather than guess.
// Per-monitor callers already use physical pixels, even for an unaware window.
static std::optional<RECT> ConvertScreenRect(
    HWND hwnd, const RECT& rect, BOOL(WINAPI* convert)(HWND, LPPOINT)) {
  const auto context = GetThreadDpiAwarenessContext();
  if (GetAwarenessFromDpiAwarenessContext(context) == DPI_AWARENESS_PER_MONITOR_AWARE) return rect;
  if (!AreDpiAwarenessContextsEqual(context, GetWindowDpiAwarenessContext(hwnd))) return std::nullopt;
  POINT topLeft{rect.left, rect.top};
  POINT bottomRight{rect.right, rect.bottom};
  if (!convert(hwnd, &topLeft) || !convert(hwnd, &bottomRight)) return std::nullopt;
  return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

// Both immediate and deferred placement take the same geometry. Rewrite it
// without committing a move; a deferred batch must remain deferred. Callers
// hold PlacementScope through the original API call to prevent nested hints.
static void AdjustPositionForHint(WINDOWPOS& position, const char* path) {
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
    }
  }
}

static BOOL WINAPI SetWindowPosHook(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (g_placing || !hwnd || !(flags & SWP_SHOWWINDOW) || IsWindowVisible(hwnd) || RejectStructurally(hwnd)) {
    return g_unhookedSetWindowPos(hwnd, after, x, y, cx, cy, flags);
  }
  PlacementScope placing;
  WINDOWPOS position{hwnd, after, x, y, cx, cy, flags};
  AdjustPositionForHint(position, "SetWindowPos");
  // Preserve the app's single native call, flags and result, including failure.
  return g_unhookedSetWindowPos(hwnd, after, position.x, position.y, position.cx, position.cy, position.flags);
}

static HDWP WINAPI DeferWindowPosHook(
    HDWP batch, HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (g_placing || !batch || !hwnd || !(flags & SWP_SHOWWINDOW) || IsWindowVisible(hwnd) || RejectStructurally(hwnd)) {
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
  if (g_placing || !hwnd || !placement || placement->length != sizeof(*placement) ||
      !IsRestoredShowCommand(static_cast<int>(placement->showCmd)) ||
      IsWindowVisible(hwnd) || RejectStructurally(hwnd)) {
    return g_unhookedSetWindowPlacement(hwnd, placement);
  }
  PlacementScope placing;
  // Non-tool top-level windows store rcNormalPosition in workspace coordinates.
  // Convert to screen coordinates before prediction and back at its destination.
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  RECT proposedScreen = placement->rcNormalPosition;
  if (GetMonitorInfoW(MonitorFromRect(&proposedScreen, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
    OffsetRect(&proposedScreen, monitorInfo.rcWork.left - monitorInfo.rcMonitor.left,
                monitorInfo.rcWork.top - monitorInfo.rcMonitor.top);
    const auto proposed = ConvertScreenRect(hwnd, proposedScreen, LogicalToPhysicalPointForPerMonitorDPI);
    const auto prediction = proposed ? PredictWindowPlacement(hwnd, *proposed, "SetWindowPlacement") : std::nullopt;
    const auto target = prediction ? ConvertScreenRect(hwnd, prediction->windowRect,
                                                       PhysicalToLogicalPointForPerMonitorDPI) : std::nullopt;
    if (target && GetMonitorInfoW(MonitorFromRect(&*target, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
      WINDOWPLACEMENT adjusted = *placement;
      adjusted.rcNormalPosition = *target;
      OffsetRect(&adjusted.rcNormalPosition, monitorInfo.rcMonitor.left - monitorInfo.rcWork.left,
                  monitorInfo.rcMonitor.top - monitorInfo.rcWork.top);
      LogDecision(hwnd, "SetWindowPlacement", "using MultiWM's predicted placement");
      return g_unhookedSetWindowPlacement(hwnd, &adjusted);
    }
  }
  return g_unhookedSetWindowPlacement(hwnd, placement);
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
  ok &= WindhawkUtils::SetFunctionHook(SetWindowPlacement, SetWindowPlacementHook, &g_unhookedSetWindowPlacement);
  ok &= WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExWHook, &g_unhookedCreateWindowExW);
  ok &= WindhawkUtils::SetFunctionHook(CreateWindowExA, CreateWindowExAHook, &g_unhookedCreateWindowExA);
  return ok;
}

}  // namespace Companion
