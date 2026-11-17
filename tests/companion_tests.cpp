#include <windows.h>

// Use an unnamed event and a test-owned snapshot. Never open or overwrite a
// running MultiWM instance's named objects, or install hooks in other apps.
static bool testPublisherAlive = false;
static HANDLE WINAPI OpenTestAliveEvent(DWORD, BOOL, LPCWSTR) {
  return testPublisherAlive ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
}
static STARTUPINFOW testStartup{sizeof(testStartup)};
static void WINAPI ReadTestStartupInfo(LPSTARTUPINFOW startup) { *startup = testStartup; }
#define OpenEventW OpenTestAliveEvent
#define GetStartupInfoW ReadTestStartupInfo
#include "../src/companion/multiwm-companion.cpp"
#undef OpenEventW
#undef GetStartupInfoW
#include <cstdio>

struct PositionCall {
  HWND hwnd;
  HWND after;
  int x, y, width, height;
  UINT flags;
};
static std::vector<PositionCall> positionCalls;
static std::vector<WINDOWPLACEMENT> placementCalls;
static std::vector<PositionCall> deferredCalls;
static std::vector<int> showCommands;
static std::vector<int> asyncCommands;
static HDWP receivedBatch;
static HDWP returnedBatch;
static BOOL nativeResult = FALSE;

static BOOL WINAPI RecordPosition(HWND hwnd, HWND after, int x, int y, int width, int height, UINT flags) {
  positionCalls.push_back({hwnd, after, x, y, width, height, flags});
  return nativeResult;
}

static BOOL WINAPI RecordPlacement(HWND, const WINDOWPLACEMENT* placement) {
  placementCalls.push_back(*placement);
  return nativeResult;
}

static HDWP WINAPI RecordDeferred(HDWP batch, HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
  receivedBatch = batch;
  deferredCalls.push_back({hwnd, after, x, y, cx, cy, flags});
  return returnedBatch;
}

static BOOL WINAPI RecordShow(HWND, int command) {
  showCommands.push_back(command);
  return nativeResult;
}

static BOOL WINAPI RecordAsync(HWND, int command) {
  asyncCommands.push_back(command);
  return nativeResult;
}

struct WindowLimits {
  POINT minimum{100, 100};
  POINT maximum{2000, 2000};
  int probes = 0;
  bool destroyDuringProbe = false;
};

static LRESULT CALLBACK TestWindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
  }
  auto* limits = reinterpret_cast<WindowLimits*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_GETMINMAXINFO && limits) {
    ++limits->probes;
    auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
    info->ptMinTrackSize = limits->minimum;
    info->ptMaxTrackSize = limits->maximum;
    if (limits->destroyDuringProbe) DestroyWindow(hwnd);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

int main() {
  int checks = 0;
  auto check = [&](bool passed, const char* description) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", description); std::exit(1); }
    ++checks;
  };
  Companion::PhysicalCoordinatesScope physical;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = TestWindowProcedure;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"MultiWMCompanionTest";
  check(RegisterClassW(&windowClass) != 0, "register test class");
  WindowLimits limits;
  const HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Companion test", WS_OVERLAPPEDWINDOW,
                                    100, 100, 800, 600, nullptr, nullptr, windowClass.hInstance, &limits);
  check(hwnd != nullptr, "create test-owned hidden window");
  limits.probes = 0;
  Companion::g_unhookedSetWindowPos = RecordPosition;
  Companion::g_unhookedSetWindowPlacement = RecordPlacement;
  Companion::g_unhookedDeferWindowPos = RecordDeferred;
  Companion::g_unhookedShowWindow = RecordShow;
  Companion::g_unhookedShowWindowAsync = RecordAsync;

  auto section = std::make_unique<CompanionAbi::SnapshotSection>();
  section->magic = CompanionAbi::kMagic;
  section->abiMajor = CompanionAbi::kAbiMajor;
  auto& body = section->body;
  constexpr uint32_t completeFlags = CompanionAbi::kFlagRunning | CompanionAbi::kFlagAutomaticMode |
                                      CompanionAbi::kFlagRulesComplete | CompanionAbi::kFlagTrackedComplete;
  body.flags = completeFlags;
  body.workspaceCount = 1;
  auto& workspace = body.workspaces[0];
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  check(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo) != FALSE,
        "read test window's monitor");
  workspace.monitorRect = monitorInfo.rcMonitor;
  workspace.workArea = monitorInfo.rcWork;
  workspace.dpi = GetDpiForWindow(hwnd);
  workspace.hasTiledFrame = 1;
  workspace.tiledFrame = {monitorInfo.rcWork.left + 100, monitorInfo.rcWork.top + 100,
                           monitorInfo.rcWork.left + 600, monitorInfo.rcWork.top + 500};
  Companion::g_reader.view = section.get();
  Companion::g_reader.body = std::make_unique<CompanionAbi::SnapshotBody>();
  constexpr UINT showFlags = SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE;

  // With no live publisher, an original failure and every argument survive.
  check(!Companion::SetWindowPosHook(hwnd, HWND_TOPMOST, 210, 220, 710, 510, showFlags),
        "no hint preserves SetWindowPos failure");
  check(positionCalls.size() == 1 && positionCalls[0].x == 210 && positionCalls[0].y == 220 &&
            positionCalls[0].width == 710 && positionCalls[0].height == 510 &&
            positionCalls[0].after == HWND_TOPMOST && positionCalls[0].flags == showFlags,
        "no hint preserves a single positioning call and all arguments");
  check(limits.probes == 0, "no hint does not execute an extra minimum-size query");
  WINDOWPLACEMENT placement{sizeof(placement)};
  placement.flags = WPF_SETMINPOSITION;
  placement.showCmd = SW_SHOWNORMAL;
  placement.ptMinPosition = {20, 30};
  placement.ptMaxPosition = {40, 50};
  placement.rcNormalPosition = {210, 220, 920, 730};
  const WINDOWPLACEMENT original = placement;
  check(!Companion::SetWindowPlacementHook(hwnd, &placement), "no hint preserves placement failure");
  check(placementCalls.size() == 1 && std::memcmp(&placementCalls[0], &original, sizeof(original)) == 0,
        "no hint passes through the original placement, including show command");

  testPublisherAlive = true;
  // Known, Manual-mode and rule-excluded windows also pass through untouched.
  for (int policy = 0; policy < 3; ++policy) {
    body.flags = policy == 0 ? completeFlags & ~CompanionAbi::kFlagAutomaticMode : completeFlags;
    body.trackedCount = policy == 1 ? 1 : 0;
    body.trackedWindows[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    std::vector<CompanionAbi::SnapshotRule> rules;
    if (policy == 2) rules.emplace_back();  // An unrestricted Exclude rule.
    bool complete = false;
    body.rulesBytes = CompanionAbi::SerializeRules(rules, body.rules, CompanionAbi::kRulesCapacity, &complete);
    positionCalls.clear();
    Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
    check(positionCalls.size() == 1 && positionCalls[0].x == 210 && positionCalls[0].width == 710 &&
              positionCalls[0].flags == showFlags && limits.probes == 0,
          "ineligible windows keep the app's geometry and flags");
  }
  body.flags = completeFlags;
  body.trackedCount = 0;
  body.rulesBytes = 0;

  // Restoring a hidden, unknown window is an eligible show. Startup defaults
  // are classified without changing the command passed to the original API.
  for (int command : {SW_SHOWNORMAL, SW_RESTORE, SW_SHOWDEFAULT}) {
    positionCalls.clear();
    showCommands.clear();
    check(!Companion::ShowWindowHook(hwnd, command), "show preserves the original return value");
    check(positionCalls.size() == 1 && showCommands == std::vector<int>{command},
          "restored show receives one hint and retains its original command");
  }
  testStartup.dwFlags = STARTF_USESHOWWINDOW;
  for (WORD command : {static_cast<WORD>(SW_SHOWMINIMIZED), static_cast<WORD>(SW_SHOWMAXIMIZED),
                       static_cast<WORD>(SW_HIDE)}) {
    testStartup.wShowWindow = command;
    positionCalls.clear();
    showCommands.clear();
    Companion::ShowWindowHook(hwnd, SW_SHOWDEFAULT);
    check(positionCalls.empty() && showCommands == std::vector<int>{SW_SHOWDEFAULT},
          "non-restored startup default is passed through without a hint");
  }
  testStartup.wShowWindow = SW_SHOWNORMAL;
  positionCalls.clear();
  showCommands.clear();
  Companion::ShowWindowHook(hwnd, SW_SHOWDEFAULT);
  check(positionCalls.size() == 1 && showCommands == std::vector<int>{SW_SHOWDEFAULT},
        "restored startup default receives a hint");
  testStartup.dwFlags = 0;
  positionCalls.clear();
  asyncCommands.clear();
  check(!Companion::ShowWindowAsyncHook(hwnd, SW_RESTORE), "async show retains the original return value");
  check(positionCalls.size() == 1 && asyncCommands == std::vector<int>{SW_RESTORE},
        "owning-thread async show receives one hint");

  // A queued request from another thread must not synchronously move or probe
  // the owner's window. The owner waits instead of processing sent messages.
  positionCalls.clear();
  asyncCommands.clear();
  limits.probes = 0;
  HANDLE caller = CreateThread(nullptr, 0, [](void* parameter) -> DWORD {
    Companion::ShowWindowAsyncHook(static_cast<HWND>(parameter), SW_RESTORE);
    return 0;
  }, hwnd, 0, nullptr);
  check(caller && WaitForSingleObject(caller, 2000) == WAIT_OBJECT_0, "cross-thread async show remains nonblocking");
  CloseHandle(caller);
  check(positionCalls.empty() && limits.probes == 0 && asyncCommands == std::vector<int>{SW_RESTORE},
        "cross-thread async show passes through without placement or probing");

  // A real hint rewrites the original call. Minimums expand it and maximums
  // shrink it, while its visible origin and the app's other flags survive.
  RECT current{}, currentFrame{};
  GetWindowRect(hwnd, &current);
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &currentFrame, sizeof(currentFrame))) ||
      currentFrame.right <= currentFrame.left || currentFrame.bottom <= currentFrame.top) currentFrame = current;
  const LONG hintedLeft = workspace.tiledFrame.left - (currentFrame.left - current.left);
  const LONG hintedTop = workspace.tiledFrame.top - (currentFrame.top - current.top);
  for (bool enforceMinimum : {true, false}) {
    limits.minimum = enforceMinimum ? POINT{700, 600} : POINT{100, 100};
    limits.maximum = enforceMinimum ? POINT{2000, 2000} : POINT{400, 300};
    positionCalls.clear();
    check(!Companion::SetWindowPosHook(hwnd, HWND_TOPMOST, 210, 220, 710, 510, showFlags | SWP_NOSIZE),
          "hint preserves the original native failure");
    check(positionCalls.size() == 1, "hint makes exactly one native positioning call");
    const auto& call = positionCalls[0];
    check(call.x == hintedLeft && call.y == hintedTop &&
              call.width == (enforceMinimum ? 700 : 400) && call.height == (enforceMinimum ? 600 : 300) &&
              call.after == HWND_TOPMOST && call.flags == showFlags,
          "hint clamps size limits at the predicted origin in one native call");
  }

  limits.minimum = {700, 600};
  limits.maximum = {2000, 2000};
  placementCalls.clear();
  nativeResult = TRUE;
  check(Companion::SetWindowPlacementHook(hwnd, &placement) != FALSE, "hint preserves placement success");
  RECT expectedNormal{hintedLeft, hintedTop, hintedLeft + 700, hintedTop + 600};
  OffsetRect(&expectedNormal, monitorInfo.rcMonitor.left - monitorInfo.rcWork.left,
               monitorInfo.rcMonitor.top - monitorInfo.rcWork.top);
  check(placementCalls.size() == 1 && EqualRect(&placementCalls[0].rcNormalPosition, &expectedNormal) &&
            placementCalls[0].showCmd == original.showCmd && placementCalls[0].flags == original.flags &&
            placementCalls[0].ptMinPosition.x == original.ptMinPosition.x &&
            placementCalls[0].ptMaxPosition.y == original.ptMaxPosition.y,
        "placement changes only restored geometry and retains the show request");
  check(std::memcmp(&placement, &original, sizeof(original)) == 0, "the caller's placement remains unchanged");

  // Deferred hints change only the queued operation. They never call immediate
  // positioning, and preserve the replacement HDWP (or failure) from Windows.
  const HDWP batch = reinterpret_cast<HDWP>(0x1000);
  const HDWP nextBatch = reinterpret_cast<HDWP>(0x2000);
  for (bool available : {false, true}) {
    for (HDWP result : {static_cast<HDWP>(nullptr), nextBatch}) {
      testPublisherAlive = available;
      positionCalls.clear();
      deferredCalls.clear();
      returnedBatch = result;
      check(Companion::DeferWindowPosHook(batch, hwnd, HWND_TOPMOST, 210, 220, 710, 510, showFlags) == result,
            "deferred call preserves the returned batch or failure");
      check(positionCalls.empty() && deferredCalls.size() == 1 && receivedBatch == batch,
            "deferred operation makes no immediate placement");
      const auto& queued = deferredCalls[0];
      check(queued.x == (available ? hintedLeft : 210) && queued.y == (available ? hintedTop : 220) &&
                queued.width == (available ? 700 : 710) && queued.height == (available ? 600 : 510) &&
                queued.after == HWND_TOPMOST && queued.flags == showFlags,
            "deferred geometry is hinted only when a snapshot applies");
    }
  }
  testPublisherAlive = true;

  limits.destroyDuringProbe = true;
  positionCalls.clear();
  Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
  check(!IsWindow(hwnd) && positionCalls.size() == 1 && positionCalls[0].x == 210 && positionCalls[0].width == 710,
        "destroyed during minimum query: abandon the hint and retain the original call");
  check(!Companion::g_placing, "recursion guard is released after callbacks");
  Companion::g_reader.view = nullptr;  // The test body is heap memory, not a mapping.
  Companion::g_reader.body.reset();
  UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
  std::printf("Passed %d companion placement checks.\n", checks);
}
