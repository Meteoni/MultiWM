#include <windows.h>

// An unnamed liveness event and a heap snapshot keep this test independent of
// the installed MultiWM. Windows are app-cloaked on a private Win32 desktop;
// the input desktop is never switched and live window managers ignore them.
static HANDLE WINAPI OpenTestAliveEvent(DWORD, BOOL, LPCWSTR) {
  return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
#define OpenEventW OpenTestAliveEvent
#include "../src/companion/multiwm-companion.cpp"
#undef OpenEventW
#include <cstdio>

// Activating a window on this private, input-less desktop sometimes stalls
// inside Windows for about two seconds (measured: about one activating show in
// three, cloaked or not), which pushed the suite past its watchdog. Activation
// is not what these tests check, so the native calls behind the companion's
// hooks show without activating; the companion still receives the caller's
// original command. Each substitute leaves the window in the same state as the
// original from normal, minimized, maximized and hidden, except SW_SHOWNORMAL
// on a window minimized from maximized, which is passed through.
static int WithoutActivation(HWND hwnd, int command) {
  switch (command) {
    case SW_RESTORE: return SW_SHOWNOACTIVATE;
    case SW_SHOW: return SW_SHOWNA;
    case SW_SHOWNORMAL: {
      WINDOWPLACEMENT placement{sizeof(placement)};
      const bool restoresToMaximized = IsIconic(hwnd) && GetWindowPlacement(hwnd, &placement) &&
                                       (placement.flags & WPF_RESTORETOMAXIMIZED);
      return restoresToMaximized ? command : SW_SHOWNOACTIVATE;
    }
    default: return command;
  }
}
static BOOL WINAPI ShowWithoutActivation(HWND hwnd, int command) {
  return ShowWindow(hwnd, WithoutActivation(hwnd, command));
}
static BOOL WINAPI ShowAsyncWithoutActivation(HWND hwnd, int command) {
  return ShowWindowAsync(hwnd, WithoutActivation(hwnd, command));
}

static bool preparationPreservedState = true;
static int preparations = 0;
// The companion's restore preparation must change only the normal rectangle:
// never the window's visibility, minimized/maximized state or activation.
static BOOL WINAPI ObservePlacement(HWND hwnd, const WINDOWPLACEMENT* placement) {
  const bool minimized = IsIconic(hwnd) != FALSE;
  const bool maximized = IsZoomed(hwnd) != FALSE;
  const bool visible = IsWindowVisible(hwnd) != FALSE;
  const HWND active = GetActiveWindow();
  WINDOWPLACEMENT native = *placement;
  native.showCmd = WithoutActivation(hwnd, static_cast<int>(placement->showCmd));
  const BOOL result = SetWindowPlacement(hwnd, &native);
  const UINT command = placement->showCmd;
  if (command == SW_SHOWMINNOACTIVE || (command == SW_SHOWNA && visible) || command == SW_HIDE) {
    ++preparations;
    preparationPreservedState &= minimized == (IsIconic(hwnd) != FALSE) && maximized == (IsZoomed(hwnd) != FALSE) &&
                                 visible == (IsWindowVisible(hwnd) != FALSE) && active == GetActiveWindow();
  }
  return result;
}

static BOOL WINAPI ObservePosition(HWND hwnd, HWND after, int x, int y, int width, int height, UINT flags) {
  const bool visible = IsWindowVisible(hwnd) != FALSE;
  const HWND active = GetActiveWindow();
  const BOOL result = SetWindowPos(hwnd, after, x, y, width, height, flags);
  ++preparations;
  preparationPreservedState &= !visible && !IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd) &&
                               active == GetActiveWindow();
  return result;
}

struct HiddenRestoreObservation {
  bool sawVisible = false;
  RECT firstVisibleRect{};
};

static LRESULT CALLBACK HiddenRestoreProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
  }
  const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
  auto* observation = reinterpret_cast<HiddenRestoreObservation*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_WINDOWPOSCHANGED && observation && !observation->sawVisible && IsWindowVisible(hwnd)) {
    observation->sawVisible = true;
    GetWindowRect(hwnd, &observation->firstVisibleRect);
  }
  return result;
}

static DWORD WINAPI RunNativeRestoreTests(void* parameter) {
  if (!SetThreadDesktop(static_cast<HDESK>(parameter))) return 1;
  Companion::PhysicalCoordinatesScope physical;
  Companion::g_unhookedSetWindowPlacement = ObservePlacement;
  Companion::g_unhookedShowWindow = ShowWithoutActivation;
  Companion::g_unhookedShowWindowAsync = ShowAsyncWithoutActivation;
  Companion::g_unhookedSetWindowPos = SetWindowPos;
  Companion::g_setWindowPosForOwnMoves = ObservePosition;
  Companion::g_unhookedDeferWindowPos = DeferWindowPos;
  Companion::g_unhookedDefWindowProcW = DefWindowProcW;
  Companion::g_unhookedDefWindowProcA = DefWindowProcA;
  auto section = std::make_unique<CompanionAbi::SnapshotSection>();
  section->magic = CompanionAbi::kMagic;
  section->abiMajor = CompanionAbi::kAbiMajor;
  Companion::g_reader.view = section.get();
  Companion::g_reader.body = std::make_unique<CompanionAbi::SnapshotBody>();
  auto& body = section->body;
  body.desktopId = GUID{0xC001};
  body.flags = CompanionAbi::kFlagRunning | CompanionAbi::kFlagAutomaticMode |
               CompanionAbi::kFlagRulesComplete | CompanionAbi::kFlagTrackedComplete;
  body.workspaceCount = body.trackedCount = body.restoreWindowCount = 1;
  auto& workspace = body.workspaces[0];
  RECT restoreTile{};
  int checks = 0;

  for (int state = 0; state < 4; ++state) {
    const bool maximized = (state & 1) != 0;
    const bool arrival = state >= 2;
    for (int path = 0; path < 5; ++path) {
      HWND hwnd = CreateWindowExW(0, L"STATIC", L"MultiWM restore test", WS_OVERLAPPEDWINDOW,
                                   100, 100, 600, 400, nullptr, nullptr, nullptr, nullptr);
      if (!hwnd) return 2;
      BOOL cloak = TRUE;
      if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return 3;
      MONITORINFO info{sizeof(info)};
      if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return 4;
      workspace.monitorRect = info.rcMonitor;
      workspace.workArea = info.rcWork;
      workspace.dpi = GetDpiForWindow(hwnd);
      // The tile MultiWM publishes for this window's restore.
      restoreTile = {info.rcWork.left + 110, info.rcWork.top + 110, info.rcWork.left + 610, info.rcWork.top + 460};
      // What MultiWM publishes: the borders measured while the window is restored.
      RECT restoredWindow{}, restoredFrame{};
      GetWindowRect(hwnd, &restoredWindow);
      if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &restoredFrame, sizeof(restoredFrame)))) {
        return 13;
      }
      const RECT margins{restoredFrame.left - restoredWindow.left, restoredFrame.top - restoredWindow.top,
                         restoredWindow.right - restoredFrame.right, restoredWindow.bottom - restoredFrame.bottom};
      body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd)), GetCurrentProcessId(),
                                arrival ? GUID{0xC002} : body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled,
                                margins, workspace.dpi};
      body.restoreWindows[0] = {body.trackedWindows[0].hwnd, GetCurrentProcessId(), 0, 1, restoreTile};
      body.restoreWindowCount = arrival ? 0 : 1;
      workspace.hasTiledFrame = 1;
      workspace.tiledFrame = restoreTile;
      ++body.generation;  // A new publish; the reader reuses its copy otherwise.

      ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWMINNOACTIVE);
      if (maximized ? !IsZoomed(hwnd) : !IsIconic(hwnd)) return 5;
      WINDOWPLACEMENT requested{sizeof(requested)};
      GetWindowPlacement(hwnd, &requested);
      requested.showCmd = SW_RESTORE;
      preparations = 0;
      switch (path) {
        case 0: Companion::ShowWindowHook(hwnd, SW_RESTORE); break;
        case 1: Companion::ShowWindowAsyncHook(hwnd, SW_SHOWNORMAL); break;
        case 2: Companion::SetWindowPlacementHook(hwnd, &requested); break;
        case 3: Companion::DefWindowProcWHook(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0); break;
        case 4: Companion::DefWindowProcAHook(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0); break;
      }
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      RECT expected = restoreTile;
      expected.left -= margins.left;
      expected.top -= margins.top;
      expected.right += margins.right;
      expected.bottom += margins.bottom;
      RECT actual{}, actualFrame{};
      GetWindowRect(hwnd, &actual);
      DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &actualFrame, sizeof(actualFrame));
      // The point of the measured borders: the visible frame lands exactly on the slot.
      if (IsIconic(hwnd) || IsZoomed(hwnd) || !EqualRect(&actual, &expected) ||
          !EqualRect(&actualFrame, &restoreTile) ||
          !preparationPreservedState || preparations != (path == 2 ? 0 : 1)) {
        std::fprintf(stderr, "FAILED: restore path=%d maximized=%d actual=(%ld,%ld,%ld,%ld) expected=(%ld,%ld,%ld,%ld)\n",
                     path, maximized, actual.left, actual.top, actual.right, actual.bottom,
                     expected.left, expected.top, expected.right, expected.bottom);
        return 6;
      }
      ++checks;
      DestroyWindow(hwnd);
    }
  }
  // A hidden minimized or maximized member (hidden to the tray, say) being shown
  // again: the preparation must keep it hidden, so it never flashes at full size,
  // and the caller's show then restores it straight into the slot.
  body.restoreWindowCount = 1;
  for (bool maximized : {false, true}) {
    HWND hidden = CreateWindowExW(0, L"STATIC", L"Hidden restore test", WS_OVERLAPPEDWINDOW,
                                   100, 100, 600, 400, nullptr, nullptr, nullptr, nullptr);
    if (!hidden) return 14;
    BOOL cloak = TRUE;
    if (FAILED(DwmSetWindowAttribute(hidden, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return 15;
    RECT restoredWindow{}, restoredFrame{};
    GetWindowRect(hidden, &restoredWindow);
    if (FAILED(DwmGetWindowAttribute(hidden, DWMWA_EXTENDED_FRAME_BOUNDS, &restoredFrame, sizeof(restoredFrame)))) {
      return 16;
    }
    const RECT margins{restoredFrame.left - restoredWindow.left, restoredFrame.top - restoredWindow.top,
                       restoredWindow.right - restoredFrame.right, restoredWindow.bottom - restoredFrame.bottom};
    body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hidden)), GetCurrentProcessId(),
                              body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled,
                              margins, workspace.dpi};
    body.restoreWindows[0] = {body.trackedWindows[0].hwnd, GetCurrentProcessId(), 0, 1, restoreTile};
    ++body.generation;
    ShowWindow(hidden, maximized ? SW_SHOWMAXIMIZED : SW_SHOWMINNOACTIVE);
    ShowWindow(hidden, SW_HIDE);
    if (IsWindowVisible(hidden) || (maximized ? !IsZoomed(hidden) : !IsIconic(hidden))) return 17;
    preparations = 0;
    preparationPreservedState = true;
    Companion::ShowWindowHook(hidden, SW_SHOWNORMAL);
    RECT frame{};
    DwmGetWindowAttribute(hidden, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
    if (preparations != 1 || !preparationPreservedState || !IsWindowVisible(hidden) || IsIconic(hidden) ||
        IsZoomed(hidden) || !EqualRect(&frame, &restoreTile)) {
      std::fprintf(stderr, "FAILED: hidden restore maximized=%d preparations=%d preserved=%d\n", maximized,
                   preparations, preparationPreservedState);
      return 18;
    }
    ++checks;
    DestroyWindow(hidden);
  }

  // Tray apps can hide a normal window without minimizing it. Every supported
  // show path must use its own restore tile before the first visible position,
  // including commands that preserve the window's current normal state.
  WNDCLASSW hiddenClass{};
  hiddenClass.lpfnWndProc = HiddenRestoreProcedure;
  hiddenClass.hInstance = GetModuleHandleW(nullptr);
  hiddenClass.lpszClassName = L"MultiWMHiddenRestoreTest";
  if (!RegisterClassW(&hiddenClass)) return 19;
  for (int scenario = 0; scenario < 10; ++scenario) {
    const int path = scenario % 5;
    const bool arrival = scenario >= 5;
    for (int command : {SW_SHOWNORMAL, SW_SHOWNOACTIVATE, SW_SHOW, SW_SHOWNA, SW_RESTORE}) {
      // Immediate/deferred positioning have flags rather than show commands.
      if (path >= 3 && command != SW_SHOWNORMAL) continue;
      HiddenRestoreObservation observation;
      HWND hidden = CreateWindowExW(0, hiddenClass.lpszClassName, L"Hidden normal restore", WS_OVERLAPPEDWINDOW,
                                      100, 100, 600, 400, nullptr, nullptr, hiddenClass.hInstance, &observation);
      if (!hidden) return 20;
      BOOL cloak = TRUE;
      if (FAILED(DwmSetWindowAttribute(hidden, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return 21;
      RECT oldRect{}, oldFrame{};
      GetWindowRect(hidden, &oldRect);
      if (FAILED(DwmGetWindowAttribute(hidden, DWMWA_EXTENDED_FRAME_BOUNDS, &oldFrame, sizeof(oldFrame)))) return 22;
      const RECT margins{oldFrame.left - oldRect.left, oldFrame.top - oldRect.top,
                         oldRect.right - oldFrame.right, oldRect.bottom - oldFrame.bottom};
      body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hidden)), GetCurrentProcessId(),
                                arrival ? GUID{0xC002} : body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled,
                                margins, workspace.dpi};
      body.restoreWindows[0] = {body.trackedWindows[0].hwnd, GetCurrentProcessId(), 0, 1, restoreTile};
      body.restoreWindowCount = arrival ? 0 : 1;
      ++body.generation;
      preparations = 0;
      preparationPreservedState = true;
      const HWND active = GetActiveWindow();
      constexpr UINT flags = SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
      switch (path) {
        case 0: Companion::ShowWindowHook(hidden, command); break;
        case 1: Companion::ShowWindowAsyncHook(hidden, command); break;
        case 2: {
          WINDOWPLACEMENT requested{sizeof(requested)};
          GetWindowPlacement(hidden, &requested);
          requested.showCmd = command;
          Companion::SetWindowPlacementHook(hidden, &requested);
          break;
        }
        case 3: Companion::SetWindowPosHook(hidden, nullptr, 0, 0, 0, 0, flags); break;
        case 4: {
          HDWP batch = BeginDeferWindowPos(1);
          batch = Companion::DeferWindowPosHook(batch, hidden, nullptr, 0, 0, 0, 0, flags);
          RECT beforeCommit{};
          GetWindowRect(hidden, &beforeCommit);
          if (!batch || IsWindowVisible(hidden) || observation.sawVisible ||
              !EqualRect(&beforeCommit, &oldRect) || GetActiveWindow() != active) return 23;
          if (!EndDeferWindowPos(batch)) return 24;
          break;
        }
      }
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      RECT expected = restoreTile;
      expected.left -= margins.left;
      expected.top -= margins.top;
      expected.right += margins.right;
      expected.bottom += margins.bottom;
      RECT actual{}, frame{};
      GetWindowRect(hidden, &actual);
      DwmGetWindowAttribute(hidden, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
      if (!IsWindowVisible(hidden) || IsIconic(hidden) || IsZoomed(hidden) || !observation.sawVisible ||
          !EqualRect(&observation.firstVisibleRect, &expected) || !EqualRect(&actual, &expected) ||
          !EqualRect(&frame, &restoreTile) || !preparationPreservedState || preparations != (path < 2 ? 1 : 0)) {
        std::fprintf(stderr, "FAILED: hidden normal path=%d command=%d first=(%ld,%ld,%ld,%ld) expected=(%ld,%ld,%ld,%ld)\n",
                     path, command, observation.firstVisibleRect.left, observation.firstVisibleRect.top,
                     observation.firstVisibleRect.right, observation.firstVisibleRect.bottom,
                     expected.left, expected.top, expected.right, expected.bottom);
        return 25;
      }
      ++checks;
      DestroyWindow(hidden);
    }
  }

  // A minimized window that was maximized must return to maximized. Do not
  // replace its normal rectangle with a tile on that transition.
  HWND restoreMax = CreateWindowExW(0, L"STATIC", L"Restore to maximized", WS_OVERLAPPEDWINDOW,
                                     100, 100, 600, 400, nullptr, nullptr, nullptr, nullptr);
  if (!restoreMax) return 7;
  body.restoreWindowCount = 1;
  BOOL cloak = TRUE;
  if (FAILED(DwmSetWindowAttribute(restoreMax, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return 8;
  body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoreMax)), GetCurrentProcessId(),
                            body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled,
                            RECT{7, 0, 7, 7}, workspace.dpi};
  body.restoreWindows[0] = {body.trackedWindows[0].hwnd, GetCurrentProcessId(), 0, 1, restoreTile};
  ++body.generation;
  ShowWindow(restoreMax, SW_SHOWMAXIMIZED);
  ShowWindow(restoreMax, SW_SHOWMINNOACTIVE);
  preparations = 0;
  Companion::ShowWindowHook(restoreMax, SW_RESTORE);
  if (!IsZoomed(restoreMax) || preparations != 0) return 9;
  ++checks;
  DestroyWindow(restoreMax);
  Companion::g_reader.view = nullptr;
  Companion::g_reader.body.reset();
  std::printf("Passed %d native restoration checks.\n", checks);
  return 0;
}

int main() {
  wchar_t name[80];
  swprintf(name, ARRAYSIZE(name), L"MultiWMRestoreTests_%lu", GetCurrentProcessId());
  HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  if (!desktop) return 10;
  HANDLE worker = CreateThread(nullptr, 0, RunNativeRestoreTests, desktop, 0, nullptr);
  if (!worker) { CloseDesktop(desktop); return 11; }
  const DWORD wait = WaitForSingleObject(worker, 60000);
  if (wait != WAIT_OBJECT_0) return 12;
  DWORD result = 0;
  GetExitCodeThread(worker, &result);
  CloseHandle(worker);
  CloseDesktop(desktop);
  return static_cast<int>(result);
}
