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
  // The companion's own moves use the public SetWindowPos; record them the same way.
  Companion::g_setWindowPosForOwnMoves = RecordPosition;
  Companion::g_unhookedSetWindowPlacement = RecordPlacement;
  Companion::g_unhookedDeferWindowPos = RecordDeferred;
  Companion::g_unhookedShowWindow = RecordShow;
  Companion::g_unhookedShowWindowAsync = RecordAsync;

  auto section = std::make_unique<CompanionAbi::SnapshotSection>();
  section->magic = CompanionAbi::kMagic;
  section->abiMajor = CompanionAbi::kAbiMajor;
  auto& body = section->body;
  body.desktopId = GUID{0xC001};
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
  // A slot in the work area's top-left corner: a window that must grow keeps
  // that corner (it is on the boundary), one that is capped is centred.
  workspace.tiledFrame = {monitorInfo.rcWork.left, monitorInfo.rcWork.top,
                           monitorInfo.rcWork.left + 500, monitorInfo.rcWork.top + 400};
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
    body.trackedWindows[0] = {.hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd))};
    std::vector<CompanionAbi::SnapshotRule> rules;
    if (policy == 2) rules.emplace_back();  // An unrestricted Exclude rule.
    bool complete = false;
    body.rulesBytes = CompanionAbi::SerializeRules(rules, body.rules, CompanionAbi::kRulesCapacity, &complete);
    // Each fixture change is a new publish, as MultiWM's would be; the reader
    // reuses its copy while the generation is unchanged.
    ++body.generation;
    positionCalls.clear();
    Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
    check(positionCalls.size() == 1 && positionCalls[0].x == 210 && positionCalls[0].width == 710 &&
              positionCalls[0].flags == showFlags && limits.probes == 0,
          "ineligible windows keep the app's geometry and flags");
  }
  body.flags = completeFlags;
  body.trackedCount = 0;
  body.rulesBytes = 0;
  ++body.generation;

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

  // A real hint rewrites the original call. A minimum grows the window from the
  // slot's corner on the work-area boundary; a maximum centres it in the slot,
  // as MultiWM's planner will. The app's other arguments and flags survive.
  RECT current{}, currentFrame{};
  GetWindowRect(hwnd, &current);
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &currentFrame, sizeof(currentFrame))) ||
      currentFrame.right <= currentFrame.left || currentFrame.bottom <= currentFrame.top) currentFrame = current;
  const RECT testMargins{currentFrame.left - current.left, currentFrame.top - current.top,
                         current.right - currentFrame.right, current.bottom - currentFrame.bottom};
  const LONG hintedLeft = workspace.tiledFrame.left - testMargins.left;
  const LONG hintedTop = workspace.tiledFrame.top - testMargins.top;
  for (bool enforceMinimum : {true, false}) {
    limits.minimum = enforceMinimum ? POINT{700, 600} : POINT{100, 100};
    limits.maximum = enforceMinimum ? POINT{2000, 2000} : POINT{400, 300};
    positionCalls.clear();
    check(!Companion::SetWindowPosHook(hwnd, HWND_TOPMOST, 210, 220, 710, 510, showFlags | SWP_NOSIZE),
          "hint preserves the original native failure");
    check(positionCalls.size() == 1, "hint makes exactly one native positioning call");
    const auto& call = positionCalls[0];
    // Capped: the visible frame (window size minus borders) is centred in the 500x400 slot.
    const LONG cappedFrameWidth = 400 - testMargins.left - testMargins.right;
    const LONG cappedFrameHeight = 300 - testMargins.top - testMargins.bottom;
    const LONG expectedX = enforceMinimum ? hintedLeft : hintedLeft + (500 - cappedFrameWidth) / 2;
    const LONG expectedY = enforceMinimum ? hintedTop : hintedTop + (400 - cappedFrameHeight) / 2;
    check(call.x == expectedX && call.y == expectedY &&
              call.width == (enforceMinimum ? 700 : 400) && call.height == (enforceMinimum ? 600 : 300) &&
              call.after == HWND_TOPMOST && call.flags == showFlags,
          "hint applies size limits as MultiWM will, in one native call");
  }

  // Overflow floats: a window MultiWM won't tile goes where overflow centres it
  // (the published floating frame), only while overflow centres rather than routes.
  {
    using Companion::Width;
    using Companion::Height;
    const RECT& work = monitorInfo.rcWork;
    // MultiWM publishes the default floating size centred in the work area.
    const RECT floatFrame = SharedGeometry::CenteredClampedIn(work, 600, 400);
    workspace.floatingFrame = floatFrame;
    workspace.hasFloatingFrame = 1;
    limits.minimum = {100, 100};
    limits.maximum = {2000, 2000};
    // A hinted window is centred in the work area at its fitted size and kept
    // inside it (SharedGeometry::FitFloatingPlacementToLimits).
    auto hinted = [&](const char* description, bool expectHint, LONG expectedWidth, LONG expectedHeight) {
      ++body.generation;
      positionCalls.clear();
      Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
      check(positionCalls.size() == 1, description);
      const auto& call = positionCalls[0];
      if (expectHint) {
        const RECT visible = SharedGeometry::MoveInsideArea(
            SharedGeometry::CenteredIn(work, expectedWidth - testMargins.left - testMargins.right,
                                       expectedHeight - testMargins.top - testMargins.bottom),
            work);
        check(call.x == visible.left - testMargins.left && call.y == visible.top - testMargins.top &&
                  call.width == expectedWidth && call.height == expectedHeight && call.flags == showFlags,
              description);
      } else {
        check(call.x == 210 && call.y == 220 && call.width == 710 && call.height == 510, description);
      }
    };
    const LONG floatWidth = Width(floatFrame) + testMargins.left + testMargins.right;
    const LONG floatHeight = Height(floatFrame) + testMargins.top + testMargins.bottom;

    // The window limit is reached (or no slot fits a minimal window).
    workspace.hasTiledFrame = 0;
    workspace.newWindowFloats = 1;
    body.flags = completeFlags | CompanionAbi::kFlagOverflowCentersFloat;
    hinted("an overflow float goes to the centred floating frame", true, floatWidth, floatHeight);
    body.flags = completeFlags;
    hinted("overflow that routes the window gives no hint", false, 0, 0);
    workspace.newWindowFloats = 0;
    body.flags = completeFlags | CompanionAbi::kFlagOverflowCentersFloat;
    hinted("no slot and no predicted float gives no hint", false, 0, 0);

    // A minimum wider than the work area: MultiWM floats the window, centred at
    // its minimum height and starting at the work area's left edge.
    workspace.hasTiledFrame = 1;
    limits.minimum = {Width(work) + 200, 600};
    hinted("a minimum wider than the work area is floated, centred, from the left edge", true, Width(work) + 200,
           std::max<LONG>(600, floatHeight));
    body.flags = completeFlags;
    hinted("a minimum wider than the work area gives no hint while overflow routes", false, 0, 0);

    limits.minimum = {100, 100};
    workspace.hasFloatingFrame = 0;
    workspace.floatingFrame = {};
    ++body.generation;
  }

  // Rule-floated windows are centred at their own size (RuleFloatedCenter),
  // unless an override-size rule names another size.
  {
    using Companion::Width;
    using Companion::Height;
    const RECT& work = monitorInfo.rcWork;
    auto rules = std::make_shared<std::vector<CompanionAbi::SnapshotRule>>(1);
    (*rules)[0].treatment = WindowRuleTreatment::AlwaysFloating;
    (*rules)[0].classNames = {windowClass.lpszClassName};
    Companion::HintInputs inputs;
    inputs.flags = completeFlags;
    inputs.workspace = workspace;
    inputs.rules = rules;
    const RECT frame{work.left + 10, work.top + 10, work.left + 410, work.top + 310};
    auto predict = [&](Companion::TargetKind* kind) {
      std::optional<SharedGeometry::FloatingPlacement> floating;
      const char* reason = nullptr;
      return Companion::PredictFrame(hwnd, frame, inputs, kind, &floating, &reason);
    };
    const POINT workCenter = SharedGeometry::RectCenter(work);
    Companion::TargetKind kind{};
    auto predicted = predict(&kind);
    check(predicted && kind == Companion::TargetKind::KeepSize && Width(*predicted) == 400 &&
              Height(*predicted) == 300 && SharedGeometry::RectCenter(*predicted).x == workCenter.x,
          "a rule-floated window is centred at its own size");

    CompanionAbi::SnapshotRule sizeRule;
    sizeRule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
    sizeRule.classNames = {windowClass.lpszClassName};
    sizeRule.preserveFloatingSize = false;
    sizeRule.floatingSizeDip = {489, 384};
    rules->push_back(sizeRule);
    predicted = predict(&kind);
    const RECT expected = SharedGeometry::ClampToArea(
        SharedGeometry::CenteredIn(work, SharedGeometry::ScaleDip(489, workspace.dpi),
                                   SharedGeometry::ScaleDip(384, workspace.dpi)),
        work);
    check(predicted && kind == Companion::TargetKind::Float && EqualRect(&*predicted, &expected),
          "an override-size rule sizes a rule-floated window");

    rules->back().preserveFloatingSize = true;
    predicted = predict(&kind);
    check(predicted && kind == Companion::TargetKind::KeepSize && Width(*predicted) == 400,
          "a preserve-size rule keeps a rule-floated window's own size");

    rules->back().preserveFloatingSize = false;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_SIZEBOX);
    predicted = predict(&kind);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    check(predicted && kind == Companion::TargetKind::KeepSize && Width(*predicted) == 400,
          "a window that can't resize keeps its size despite a size rule");
  }

  // Settling: after the companion places a window at its show, the app's own
  // moves are held at that frame until MultiWM takes the window over (its
  // snapshot entry changes, or its own SWP_NOSENDCHANGING placement arrives).
  // UWP launches reuse a spare ApplicationFrameWindow shown, cloaked, long
  // before; the launch sets its empty title, which places and settles it.
  {
    WNDCLASSW frameClass = windowClass;
    frameClass.lpszClassName = L"ApplicationFrameWindow";
    check(RegisterClassW(&frameClass) != 0, "register a UWP frame class");
    auto makeFrame = [&](const wchar_t* title) {
      const HWND frame = CreateWindowExW(0, frameClass.lpszClassName, title, WS_OVERLAPPEDWINDOW, 100, 100, 800,
                                         600, nullptr, nullptr, frameClass.hInstance, &limits);
      BOOL cloak = TRUE;
      check(frame && SUCCEEDED(DwmSetWindowAttribute(frame, DWMWA_CLOAK, &cloak, sizeof(cloak))),
            "create a cloaked UWP frame");
      ShowWindow(frame, SW_SHOWNOACTIVATE);
      check(IsWindowVisible(frame) != FALSE, "the spare frame is shown while cloaked");
      return frame;
    };
    Companion::g_unhookedDefWindowProcW = DefWindowProcW;
    const BOOL savedResult = nativeResult;
    nativeResult = TRUE;  // The companion's own placement succeeds, so settling starts.
    constexpr UINT moveFlags = SWP_NOZORDER | SWP_NOACTIVATE;
    // The app's own move, as its WM_WINDOWPOSCHANGING reaches DefWindowProc.
    auto appMove = [&](HWND window, UINT flags = moveFlags) {
      WINDOWPOS position{window, nullptr, 900, 900, 600, 500, flags};
      Companion::DefWindowProcWHook(window, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&position));
      return position;
    };
    auto unchanged = [](const WINDOWPOS& position) {
      return position.x == 900 && position.y == 900 && position.cx == 600 && position.cy == 500;
    };
    const RECT& slot = workspace.tiledFrame;
    auto heldAt = [&](const WINDOWPOS& position, const RECT& margins) {
      return position.x == slot.left - margins.left && position.y == slot.top - margins.top &&
             position.cx == Companion::Width(slot) + margins.left + margins.right &&
             position.cy == Companion::Height(slot) + margins.top + margins.bottom &&
             !(position.flags & (SWP_NOMOVE | SWP_NOSIZE));
    };

    const HWND spare = makeFrame(L"");
    RECT window{}, visible{};
    GetWindowRect(spare, &window);
    if (FAILED(DwmGetWindowAttribute(spare, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) visible = window;
    const RECT margins{visible.left - window.left, visible.top - window.top, window.right - visible.right,
                       window.bottom - visible.bottom};
    ++body.generation;

    check(unchanged(appMove(spare)), "a spare frame no app has taken keeps its moves");
    positionCalls.clear();
    Companion::DefWindowProcWHook(spare, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Microsoft Store"));
    check(positionCalls.size() == 1 && positionCalls[0].x == slot.left - margins.left &&
              positionCalls[0].y == slot.top - margins.top,
          "taking a spare frame places it at the current slot");
    check(heldAt(appMove(spare), margins), "ApplicationFrameHost's move to the saved position is held at the slot");
    WINDOWPOS zOrder{spare, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE};
    Companion::DefWindowProcWHook(spare, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&zOrder));
    check(zOrder.x == 0 && zOrder.cx == 0 && zOrder.flags == (SWP_NOMOVE | SWP_NOSIZE),
          "a z-order-only change is unchanged");
    positionCalls.clear();
    Companion::DefWindowProcWHook(spare, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Microsoft Store - Home"));
    check(positionCalls.empty(), "later title changes don't place the frame");
    check(heldAt(appMove(spare), margins), "an unrelated republish keeps the frame held");
    ++body.generation;
    check(heldAt(appMove(spare), margins), "a new generation alone doesn't end settling");

    // MultiWM takes the frame (it uncloaked and was admitted): its entry appears.
    body.trackedCount = 1;
    body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(spare)), GetCurrentProcessId(),
                              GUID{1}, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled, {}, 0};
    ++body.generation;
    check(unchanged(appMove(spare)), "once MultiWM tracks the frame, its moves are its own again");
    check(unchanged(appMove(spare)), "settling stays ended");
    body.trackedCount = 0;
    body.trackedWindows[0] = {};
    ++body.generation;
    DestroyWindow(spare);

    const HWND titled = makeFrame(L"Settings");
    positionCalls.clear();
    Companion::DefWindowProcWHook(titled, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Settings - Display"));
    check(positionCalls.empty(), "a titled cloaked frame's title change doesn't place it");
    check(unchanged(appMove(titled)), "a titled cloaked frame keeps its moves");
    DestroyWindow(titled);

    // WeChat: shown into its tile, then moves itself to its pre-hide position.
    auto showPlaced = [&] {
      positionCalls.clear();
      showCommands.clear();
      Companion::ShowWindowHook(hwnd, SW_SHOWNORMAL);
      check(positionCalls.size() == 1, "a shown window is placed and settles");
    };
    showPlaced();
    check(heldAt(appMove(hwnd), testMargins), "the app's own move after its show is held at the tile");
    // MultiWM's own placement arrives (SWP_NOSENDCHANGING): settling ends.
    WINDOWPOS multiwm{hwnd, nullptr, 1, 2, 3, 4, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING};
    Companion::DefWindowProcWHook(hwnd, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&multiwm));
    check(unchanged(appMove(hwnd)), "after MultiWM's own placement, the app's moves are its own");
    showPlaced();
    check(unchanged(appMove(hwnd, moveFlags | Companion::kSwpStateChanged)), "a maximize or minimize ends settling");
    check(unchanged(appMove(hwnd)), "settling stays ended after a state change");
    showPlaced();
    WINDOWPOS hide{hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW};
    Companion::DefWindowProcWHook(hwnd, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&hide));
    check(hide.x == 0 && hide.cx == 0 && unchanged(appMove(hwnd)), "hiding the window ends settling");

    Companion::EndSettling();
    nativeResult = savedResult;
    ++body.generation;
  }

  // Process rules match on the executable's file name, read without a path limit.
  check(_wcsicmp(Companion::CurrentProcessName(nullptr).c_str(), L"companion_tests.exe") == 0,
        "the process name is the executable's file name");

  // The fitting rule itself, with literal numbers. A 1200x1000 work area.
  {
    const RECT area{0, 0, 1200, 1000};
    auto fit = [&](RECT slot, SharedGeometry::FrameLimits frameLimits) {
      return Companion::FitSlotToLimits(slot, area, frameLimits);
    };
    auto equals = [](const std::optional<RECT>& rect, RECT expected) {
      return rect && EqualRect(&*rect, &expected);
    };
    check(equals(fit({0, 0, 400, 1000}, {600, 1, 0, 0}), {0, 0, 600, 1000}),
          "growth keeps the edge on the work-area boundary (start)");
    check(equals(fit({800, 0, 1200, 1000}, {600, 1, 0, 0}), {600, 0, 1200, 1000}),
          "growth keeps the edge on the work-area boundary (end)");
    check(equals(fit({400, 0, 800, 1000}, {600, 1, 0, 0}), {300, 0, 900, 1000}),
          "growth between neighbours spreads from the centre");
    check(equals(fit({100, 0, 500, 1000}, {900, 1, 0, 0}), {0, 0, 900, 1000}),
          "growth never leaves the work area");
    check(!fit({0, 0, 400, 1000}, {1300, 1, 0, 0}), "a minimum wider than the work area gives no hint");
    check(equals(fit({400, 0, 800, 1000}, {1, 1, 200, 0}), {500, 0, 700, 1000}),
          "a capped width is centred, as MultiWM's planner does");
    check(equals(fit({0, 0, 600, 1000}, {1, 1, 0, 400}), {0, 300, 600, 700}),
          "a capped height is centred on the cross axis");
    check(equals(fit({0, 0, 600, 1000}, {1, 1, 600, 1000}), {0, 0, 600, 1000}),
          "limits the slot already satisfies change nothing");
  }

  // A DPI-unaware window reports its limits in its own 96-DPI units, unconverted
  // even to this per-monitor-aware scope. They must come back in physical pixels,
  // matching MultiWM's probe, or hints on a scaled display are too small.
  {
    WindowLimits unawareLimits;
    unawareLimits.minimum = {400, 300};
    unawareLimits.maximum = {800, 600};
    const DPI_AWARENESS_CONTEXT scope = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    const HWND unaware = CreateWindowExW(0, windowClass.lpszClassName, L"Companion unaware test", WS_OVERLAPPEDWINDOW,
                                         100, 100, 800, 600, nullptr, nullptr, windowClass.hInstance, &unawareLimits);
    SetThreadDpiAwarenessContext(scope);
    check(unaware && GetDpiForWindow(unaware) == 96, "create hidden DPI-unaware window");
    RECT window{}, frame{};
    GetWindowRect(unaware, &window);
    if (FAILED(DwmGetWindowAttribute(unaware, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) frame = window;
    const RECT unawareMargins{frame.left - window.left, frame.top - window.top,
                              window.right - frame.right, window.bottom - frame.bottom};
    const UINT monitorDpi = workspace.dpi;
    std::optional<SharedGeometry::FrameLimits> read;
    {
      Companion::PlacementScope placing;
      read = Companion::QueryFrameLimits(unaware, monitorDpi, unawareMargins);
    }
    const LONG decorationWidth = unawareMargins.left + unawareMargins.right;
    const LONG decorationHeight = unawareMargins.top + unawareMargins.bottom;
    check(read && read->minWidth == MulDiv(std::max(400, GetSystemMetricsForDpi(SM_CXMINTRACK, 96)),
                                           static_cast<int>(monitorDpi), 96) - decorationWidth &&
              read->minHeight == MulDiv(std::max(300, GetSystemMetricsForDpi(SM_CYMINTRACK, 96)),
                                        static_cast<int>(monitorDpi), 96) - decorationHeight &&
              read->maxWidth == MulDiv(800, static_cast<int>(monitorDpi), 96) - decorationWidth &&
              read->maxHeight == MulDiv(600, static_cast<int>(monitorDpi), 96) - decorationHeight,
          "an unaware window's limits are scaled to physical pixels");
    DestroyWindow(unaware);
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

  // Ownership chooses the operation. A missing local restore tile never turns
  // into admission; a tiled owner elsewhere uses the destination insertion tile.
  {
    const RECT borders{9, 0, 9, 9};
    RECT saved = workspace.tiledFrame;
    OffsetRect(&saved, 600, 0);
    limits.minimum = {100, 100};
    limits.maximum = {2000, 2000};
    body.trackedCount = 1;
    for (int scenario = 0; scenario < 12; ++scenario) {
      auto& owner = body.trackedWindows[0];
      owner = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd)), GetCurrentProcessId(),
               body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled, borders, workspace.dpi};
      body.restoreWindowCount = 1;
      body.restoreWindows[0] = {owner.hwnd, GetCurrentProcessId(), 0, 1, saved};
      body.rulesBytes = 0;
      workspace.floatingLayout = 0;
      workspace.hasTiledFrame = 1;
      workspace.newWindowFloats = 0;
      Companion::PlacementIntent intent = Companion::PlacementIntent::Restore;
      bool hintExpected = scenario < 4;
      const RECT* target = &saved;
      if (scenario != 0 && scenario != 4) {
        owner.desktopId = GUID{0xC002};
        intent = Companion::PlacementIntent::Arrival;
        target = &workspace.tiledFrame;
        body.restoreWindowCount = 0;
      }
      if (scenario == 2) { owner.desktopId = body.desktopId; OffsetRect(&owner.monitorRect, 4000, 0); }
      if (scenario == 3) {
        // Opening rules do not reclassify a retained tiled member on arrival.
        std::vector<CompanionAbi::SnapshotRule> rules(2);
        rules[0].treatment = WindowRuleTreatment::DefaultToFloating;
        rules[1].treatment = WindowRuleTreatment::InitialPlacement;
        bool complete = false;
        body.rulesBytes = CompanionAbi::SerializeRules(rules, body.rules, CompanionAbi::kRulesCapacity, &complete);
        check(complete, "encode arrival rule fixture");
      }
      if (scenario == 4) body.restoreWindowCount = 0;
      if (scenario == 5) { owner.treatment = CompanionAbi::WindowTreatment::Floating; intent = Companion::PlacementIntent::KeepFloating; }
      if (scenario == 6) { ++owner.processId; intent = Companion::PlacementIntent::Unavailable; }
      if (scenario == 7) { owner.desktopId = {}; intent = Companion::PlacementIntent::Unavailable; }
      if (scenario == 8) { owner.treatment = CompanionAbi::WindowTreatment::Ignored; intent = Companion::PlacementIntent::Unavailable; }
      if (scenario == 9) owner.marginsDpi = 0;
      if (scenario == 10) workspace.floatingLayout = 1;
      if (scenario == 11) { workspace.hasTiledFrame = 0; workspace.newWindowFloats = 1; }
      ++body.generation;
      Companion::HintInputs inputs;
      check(Companion::ReadHintInputs(hwnd, workspace.monitorRect, &inputs) && inputs.tracked && inputs.intent == intent,
            "reader identifies restore, arrival, retained floating and unusable ownership");
      positionCalls.clear();
      limits.probes = 0;
      Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
      check(positionCalls.size() == 1, "known transition preserves one positioning call");
      const auto& call = positionCalls[0];
      check(hintExpected ? call.x == target->left - borders.left && call.y == target->top - borders.top &&
                             call.width == Companion::Width(*target) + borders.left + borders.right
                         : call.x == 210 && call.y == 220 && call.width == 710 && limits.probes == 0,
            "only an applicable restore or tiled arrival rewrites the geometry");
    }
    workspace.floatingLayout = workspace.newWindowFloats = 0;
    workspace.hasTiledFrame = 1;
    body.trackedCount = body.restoreWindowCount = body.rulesBytes = 0;
    ++body.generation;
    section->abiMajor = CompanionAbi::kAbiMajor - 1;
    positionCalls.clear();
    limits.probes = 0;
    Companion::SetWindowPosHook(hwnd, nullptr, 210, 220, 710, 510, showFlags);
    check(positionCalls.size() == 1 && positionCalls[0].x == 210 && positionCalls[0].width == 710 && limits.probes == 0,
          "an incompatible snapshot major passes through without probing or placing");
    section->abiMajor = CompanionAbi::kAbiMajor;
  }

  // Restoring uses the tile MultiWM published for this window, not the stale
  // normal rectangle. Changing only the hidden test window's style exercises the
  // decision without showing or moving it; real native restoration is checked
  // separately.
  const LONG_PTR normalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
  SetWindowLongPtrW(hwnd, GWL_STYLE, normalStyle | WS_MINIMIZE);
  check(IsIconic(hwnd), "hidden restore fixture reports minimized");
  limits.minimum = {100, 100};
  body.trackedCount = 1;
  body.restoreWindowCount = 1;
  // Borders as MultiWM measured them while the window was restored. Deliberately
  // unlike any system-metric estimate, so the test proves they are what is used.
  const RECT measuredMargins{9, 0, 9, 9};
  RECT restoreTile = workspace.tiledFrame;
  OffsetRect(&restoreTile, 600, 0);
  body.trackedWindows[0] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd)), GetCurrentProcessId(),
                            body.desktopId, workspace.monitorRect, CompanionAbi::WindowTreatment::Tiled,
                            measuredMargins, workspace.dpi};
  body.restoreWindows[0] = {body.trackedWindows[0].hwnd, GetCurrentProcessId(), 0, 1, restoreTile};
  ++body.generation;
  WINDOWPLACEMENT restoreRequest = original;
  restoreRequest.showCmd = SW_RESTORE;
  const LONG workspaceOffset = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
  const LONG workspaceTopOffset = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
  {
    Companion::PlacementScope placing;
    check(Companion::AdjustRestorePlacement(hwnd, &restoreRequest, "test"), "known minimized member gets a restore hint");
  }
  const RECT& slot = restoreTile;
  check(restoreRequest.rcNormalPosition.left == slot.left - measuredMargins.left - workspaceOffset &&
            restoreRequest.rcNormalPosition.top == slot.top - measuredMargins.top - workspaceTopOffset &&
            restoreRequest.rcNormalPosition.right == slot.right + measuredMargins.right - workspaceOffset &&
            restoreRequest.rcNormalPosition.bottom == slot.bottom + measuredMargins.bottom - workspaceTopOffset &&
            restoreRequest.showCmd == SW_RESTORE && restoreRequest.flags == original.flags,
        "MultiWM's restore tile is used, inflated by its measured borders, while show state and flags survive");

  // Without a measurement at this DPI the restored frame can't be exact, so no hint.
  for (UINT marginsDpi : {0u, workspace.dpi + 24}) {
    body.trackedWindows[0].marginsDpi = marginsDpi;
    ++body.generation;
    restoreRequest = original;
    restoreRequest.showCmd = SW_RESTORE;
    const auto unchanged = restoreRequest;
    Companion::PlacementScope placing;
    check(!Companion::AdjustRestorePlacement(hwnd, &restoreRequest, "test") &&
              std::memcmp(&restoreRequest, &unchanged, sizeof(unchanged)) == 0,
          "missing or other-DPI border measurement gives no restore hint");
  }
  body.trackedWindows[0].marginsDpi = workspace.dpi;
  for (int missing = 0; missing < 3; ++missing) {
    body.restoreWindows[0].processId = missing == 0 ? GetCurrentProcessId() + 1 : GetCurrentProcessId();
    // MultiWM publishes no tile when the restored layout would be unsatisfiable.
    body.restoreWindows[0].hasFrame = missing == 1 ? 0 : 1;
    ++body.generation;
    restoreRequest = original;
    restoreRequest.showCmd = SW_RESTORE;
    if (missing == 2) restoreRequest.flags |= WPF_RESTORETOMAXIMIZED;
    const auto unchanged = restoreRequest;
    Companion::PlacementScope placing;
    check(!Companion::AdjustRestorePlacement(hwnd, &restoreRequest, "test") &&
              std::memcmp(&restoreRequest, &unchanged, sizeof(unchanged)) == 0,
          "wrong identity, no published tile and restore-to-maximized are left alone");
  }
  SetWindowLongPtrW(hwnd, GWL_STYLE, normalStyle);
  body.trackedCount = 0;
  body.restoreWindowCount = 0;
  ++body.generation;

  // The reader reuses its copy while MultiWM's generation and process ID are
  // unchanged, and copies again when either changes. A restarted MultiWM counts
  // generations from zero again, so the process ID must invalidate on its own.
  {
    Companion::HintInputs inputs;
    check(Companion::ReadHintInputs(hwnd, workspace.monitorRect, &inputs) && inputs.flags == completeFlags,
          "read the current snapshot");
    body.flags = 0;  // Changed without a new generation: not a publish.
    check(Companion::ReadHintInputs(hwnd, workspace.monitorRect, &inputs) && inputs.flags == completeFlags,
          "an unchanged generation reuses the cached copy");
    section->writerPid += 1;
    check(Companion::ReadHintInputs(hwnd, workspace.monitorRect, &inputs) && inputs.flags == 0,
          "a new writer process invalidates the cached copy");
    body.flags = completeFlags;
    ++body.generation;
  }

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
