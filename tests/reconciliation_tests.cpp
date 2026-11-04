#include "../src/multiwm.cpp"
#include <cstdio>

struct TestDesktop final : Platform::VirtualDesktop::IVirtualDesktop {
  GUID id{};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown*, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetId(GUID* result) override { *result = id; return S_OK; }
};
struct TestManager final : Platform::VirtualDesktop::IVirtualDesktopManagerInternal {
  TestDesktop desktop;
  GUID nextDesktop{};
  unsigned currentQueries = 0;
  unsigned switchAfter = UINT_MAX;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  virtual HRESULT STDMETHODCALLTYPE Reserved3() { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE Reserved4() { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE Reserved5() { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE Current(Platform::VirtualDesktop::IVirtualDesktop** result) {
    if (++currentQueries > switchAfter) desktop.id = nextDesktop;
    *result = &desktop;
    return S_OK;
  }
};
struct TestWindowDesktops final : IVirtualDesktopManager {
  GUID desktop{};
  bool fail = false;
  unsigned queries = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND, BOOL* current) override {
    *current = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND, GUID* result) override {
    ++queries;
    if (fail) return E_FAIL;
    *result = desktop;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND, REFGUID) override { return E_NOTIMPL; }
};

static void RunDesktopReconciliationTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  TestWindowDesktops windows;
  TestManager manager;
  g_vd.initialized = true;
  g_vd.desktopManager = &windows;
  g_vd.managerInternal = &manager;
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;
  DesktopMonitorKey source;
  source.desktopId.Data1 = 0xFF00065C;
  source.monitor.deviceId = L"TEST_MONITOR";
  DesktopMonitorKey target = source;
  target.desktopId.Data1 = 0x3E3FD57C;
  const std::vector<HWND> snapshot{reinterpret_cast<HWND>(1), reinterpret_cast<HWND>(2), reinterpret_cast<HWND>(3)};
  const std::vector<HWND> reordered{snapshot[2], snapshot[0], snapshot[1]};
  const RECT area{0, 0, 1000, 1000};
  for (TileLayout layout : {TileLayout::Dwindle, TileLayout::Columns, TileLayout::Rows,
                            TileLayout::MasterStack, TileLayout::MasterStackH}) {
    for (ManagementMode mode : {ManagementMode::Automatic, ManagementMode::Manual}) {
      g_wm.managementMode = mode;
      Workspace original;
      original.SetLayout(layout);
      for (HWND hwnd : snapshot) {
        WindowRecord record;
        record.hwnd = hwnd;
        record.pid = 123;
        record.canMove = false; // No real window positioning in this test.
        check(original.AdmitTiled(record), "seed original window");
      }
      std::vector<RECT> rects;
      check(Layout::BuildWorkspaceLayoutPlan(original, area, 0, &rects) == Layout::LayoutPlanStatus::Success, "plan original layout");
      MoveSizeGesture drag;
      drag.hasRects = true;
      drag.start = drag.end = rects[0];
      if (layout == TileLayout::Rows || layout == TileLayout::MasterStackH) drag.end.bottom += 100;
      else drag.end.right += 100;
      check(layout == TileLayout::Dwindle ? original.LearnDwindleResize(area, 0, 0, drag)
            : layout == TileLayout::Columns || layout == TileLayout::Rows ? original.LearnGridResize(area, 0, 0, drag)
            : original.LearnMasterStackResize(area, 0, 0, drag), "save custom ratio or weights");
      original.RememberFocusedWindow(snapshot[1]);
      check(original.Suspend(snapshot[2], SuspensionReason::Minimized), "preserve suspended slot");
      Workspace floating;
      floating.SetLayout(TileLayout::Floating);
      g_workspaces.Save(source, original);
      g_workspaces.Save(target, floating);
      windows.desktop = source.desktopId;
      windows.fail = false;
      windows.queries = 0;
      const auto migrations = Diagnostics::g_runtime.counters.workspaceMigrations;
      std::vector<DesktopMonitorKey> changed;
      check(!Reconcile::ReconcileTrackedSnapshotOwnership(target, snapshot, &changed), "reject stale destination");
      Workspace after;
      check(g_workspaces.Load(source, &after) && after.HasSameLayoutStateAs(original), "preserve weights, order, focus membership and saved slots");
      check(g_workspaces.Load(target, &after) && after.RecordCount() == 0, "floating destination remains empty");
      check(windows.queries == snapshot.size() && changed.empty(), "verify ownership before each possible migration");
      check(!Reconcile::ReconcileTrackedSnapshotOwnership(source, reordered, &changed), "correction is a no-op after rejected stale snapshot");
      check(g_workspaces.Load(source, &after) && after.HasSameLayoutStateAs(original), "correction preserves custom layout");
      check(Diagnostics::g_runtime.counters.workspaceMigrations == migrations, "no out-and-back migrations");
      windows.fail = true;
      check(!Reconcile::ReconcileTrackedSnapshotOwnership(target, snapshot, &changed), "query failure preserves ownership");
      check(g_workspaces.Load(source, &after) && after.HasSameLayoutStateAs(original), "query failure preserves saved layout");
      windows.fail = false;
      windows.desktop = target.desktopId;
      check(!Reconcile::ReconcileTrackedSnapshotOwnership(source, snapshot, &changed), "reject stale source after actual move");
      check(Reconcile::ReconcileTrackedSnapshotOwnership(target, snapshot, &changed), "accept verified actual desktop departure");
      check(g_workspaces.Load(source, &after) && after.RecordCount() == 0, "actual departure removes old owner");
      check(g_workspaces.Load(target, &after) && after.RecordCount() == (mode == ManagementMode::Automatic ? 3 : 0), "Automatic migrates and Manual forgets");
      for (HWND hwnd : snapshot) Reconcile::ForgetManagedWindow(hwnd);
    }
  }

  // Exercise the complete retry path with hidden windows owned by this test.
  // Manual discovery cannot adopt or move any other process's windows.
  g_wm.managementMode = ManagementMode::Manual;
  DesktopMonitorKey liveKey;
  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  check(DesktopMonitorKey::FromHMonitor(source.desktopId, monitor, &liveKey), "resolve test monitor");
  Workspace hidden;
  hidden.SetLayout(TileLayout::Dwindle);
  std::vector<HWND> hiddenWindows;
  for (int index = 0; index < 3; ++index) {
    const HWND hwnd = CreateWindowExW(0, L"STATIC", L"MultiWM reconciliation test", WS_OVERLAPPEDWINDOW,
                                     0, 0, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(hwnd != nullptr, "create hidden test window");
    hiddenWindows.push_back(hwnd);
    WindowRecord record;
    record.hwnd = hwnd;
    record.pid = GetCurrentProcessId();
    record.canMove = false;
    check(hidden.AdmitTiled(record), "seed hidden test record");
  }
  MoveSizeGesture hiddenDrag;
  hiddenDrag.start = {0, 0, 500, 1000};
  hiddenDrag.end = {0, 0, 600, 1000};
  hiddenDrag.hasRects = true;
  check(hidden.LearnDwindleResize(area, 0, 0, hiddenDrag), "save hidden test ratio");
  g_workspaces.Save(liveKey, hidden);
  Workspace expectedHidden = hidden;
  for (HWND hwnd : hiddenWindows) expectedHidden.Suspend(hwnd, SuspensionReason::Hidden);
  windows.desktop = source.desktopId;
  manager.desktop.id = source.desktopId;
  manager.nextDesktop = target.desktopId;
  manager.currentQueries = 0;
  manager.switchAfter = 1;
  g_wm.reconciledDesktop = source.desktopId;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  g_wm.lifecycleDirtyWindows = hiddenWindows;
  Reconcile::ReconcileDeferredLifecycle();
  check(g_wm.reconciledDesktopState == ReconciledDesktopState::TransitionPending,
        "whole pass detects desktop change during enumeration");
  for (HWND hwnd : hiddenWindows) {
    check(Model::ContainsWindow(g_wm.lifecycleDirtyWindows, hwnd), "retry retains dirty window");
  }
  check(g_wm.lifecycleTimer != 0, "whole pass schedules retry");
  check(std::any_of(g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) { return IsEqualGUID(pending.desktopId, source.desktopId) && pending.monitor == monitor; }),
        "retry retains arrangement for participation changes");
  Workspace retried;
  check(g_workspaces.Load(liveKey, &retried) && retried.HasSameLayoutStateAs(expectedHidden),
        "retry preserves logical slots and weights while updating physical suspension");

  // Desktop transitions can hide a window while old placement work is pending.
  // Neither lease expiry nor a late location event may float it or lose its slot.
  Placement::ConformanceLease hiddenLease;
  hiddenLease.generation = 42;
  hiddenLease.expiresAtTickMs = GetTickCount64() + 3000;
  hiddenLease.expectedRect = {0, 0, 500, 1000};
  Placement::FinalizeExpiredConformanceLease(hiddenWindows.front(), hiddenLease);
  check(g_workspaces.Load(liveKey, &retried) && retried.HasSameLayoutStateAs(expectedHidden),
        "lease expiry preserves hidden members and saved slots");
  Placement::g_conformanceLeases.leases[hiddenWindows.front()] = hiddenLease;
  check(!Placement::HandleTiledWindowLocationChange(hiddenWindows.front()), "hidden location event is ineligible for repair");
  check(!Placement::g_conformanceLeases.leases.count(hiddenWindows.front()), "hidden location event cancels stale placement work");
  check(g_workspaces.Load(liveKey, &retried) && retried.HasSameLayoutStateAs(expectedHidden),
        "late location event preserves hidden members and saved slots");
  for (HWND hwnd : hiddenWindows) {
    Reconcile::ForgetManagedWindow(hwnd);
    DestroyWindow(hwnd);
  }
  KillTimer(nullptr, g_wm.lifecycleTimer);
  g_wm.lifecycleTimer = 0;
  g_wm.lifecycleDirtyWindows.clear();
  g_wm.pendingDesktopArranges.clear();
  manager.switchAfter = UINT_MAX;

  manager.desktop.id = source.desktopId;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  check(Reconcile::ValidateSnapshotDesktop(source.desktopId), "stable snapshot accepted");
  check(g_wm.lifecycleTimer == 0, "stable snapshot does not poll");
  manager.desktop.id = target.desktopId;
  check(!Reconcile::ValidateSnapshotDesktop(source.desktopId), "desktop switch invalidates snapshot");
  check(g_wm.reconciledDesktopState == ReconciledDesktopState::TransitionPending && g_wm.lifecycleTimer != 0, "switch schedules settled retry");
  KillTimer(nullptr, g_wm.lifecycleTimer);
  g_wm.lifecycleTimer = 0;
  g_vd.managerInternal = nullptr;
  check(!Reconcile::ValidateSnapshotDesktop(source.desktopId), "missing current desktop invalidates snapshot");
  check(g_wm.lifecycleTimer != 0 && g_wm.lifecycleRetryAfterPlatformRecovery, "unavailable desktop retains recovery path");
  KillTimer(nullptr, g_wm.lifecycleTimer);
  KillTimer(nullptr, g_wm.maintenanceTimer);
  g_wm.lifecycleTimer = g_wm.maintenanceTimer = 0;
  g_vd.desktopManager = nullptr;
  g_vd.initialized = false;
  std::printf("Passed %d desktop reconciliation checks.\n", checks);
}

// WinEventProc must only observe and post. These checks pin which events reach
// the WM thread, as which message, and which per-window bookkeeping is cleared.
// Uses hidden windows owned by this test; no hooks are installed.
static void RunWinEventRoutingTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  struct Posted { UINT message; WPARAM wParam; LPARAM lParam; };
  auto drain = [] {
    std::vector<Posted> posted;
    MSG msg{};
    while (PeekMessageW(&msg, reinterpret_cast<HWND>(-1), WM_APP, WM_APP + 64, PM_REMOVE)) {
      posted.push_back({msg.message, msg.wParam, msg.lParam});
    }
    return posted;
  };
  auto postedOnly = [&](UINT message, WPARAM wParam, LPARAM lParam) {
    const auto posted = drain();
    return posted.size() == 1 && posted[0].message == message &&
        posted[0].wParam == wParam && posted[0].lParam == lParam;
  };
  auto windowEvent = [&](DWORD event, HWND hwnd) {
    return postedOnly(WM_APP_WINDOW_EVENT, static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd));
  };
  auto send = [](DWORD event, HWND hwnd, LONG idObject = OBJID_WINDOW) {
    Platform::WindowEvents::WinEventProc(nullptr, event, hwnd, idObject, CHILDID_SELF, 0, 0);
  };
  auto makeWindow = [](DWORD style, HWND parent) {
    return CreateWindowExW(0, L"STATIC", L"MultiWM WinEvent test", style, 0, 0, 300, 300,
                           parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  };

  g_wm.threadId.store(GetCurrentThreadId(), std::memory_order_release);
  drain();
  const HWND tracked = makeWindow(WS_OVERLAPPEDWINDOW, nullptr);
  const HWND untracked = makeWindow(WS_OVERLAPPEDWINDOW, nullptr);
  const HWND child = makeWindow(WS_CHILD, untracked);
  check(tracked && untracked && child, "create WinEvent test windows");

  DesktopMonitorKey key;
  GUID desktop{};
  desktop.Data1 = 0x5EE7E57;
  check(DesktopMonitorKey::FromHMonitor(desktop, MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &key),
        "resolve WinEvent test monitor");
  Workspace workspace;
  WindowRecord record;
  record.hwnd = tracked;
  record.pid = GetCurrentProcessId();
  record.canMove = false;
  check(workspace.AdmitTiled(record), "seed tracked WinEvent window");
  g_workspaces.Save(key, workspace);

  // Filtering before any window-specific work.
  send(EVENT_OBJECT_FOCUS, tracked);
  check(drain().empty(), "unsubscribed event is ignored");
  send(EVENT_SYSTEM_FOREGROUND, untracked);
  check(postedOnly(WM_APP_FOREGROUND_CHANGED, reinterpret_cast<WPARAM>(untracked), 0),
        "foreground change is forwarded for any window");
  send(EVENT_OBJECT_LOCATIONCHANGE, tracked, OBJID_CLIENT);
  check(drain().empty(), "non-window object event is ignored");

  // Untracked windows: only discovery prompts on top-level windows get through.
  g_wm.managementMode = ManagementMode::Manual;
  g_wm.windowRoutingReady = false;
  send(EVENT_OBJECT_SHOW, untracked);
  check(windowEvent(EVENT_OBJECT_SHOW, untracked), "untracked show prompts discovery");
  send(EVENT_OBJECT_SHOW, child);
  check(drain().empty(), "child window show is ignored");
  send(EVENT_SYSTEM_MOVESIZEEND, untracked);
  check(drain().empty(), "Manual untracked drag end is not a discovery prompt");
  g_wm.managementMode = ManagementMode::Automatic;
  send(EVENT_SYSTEM_MOVESIZEEND, untracked);
  check(windowEvent(EVENT_OBJECT_SHOW, untracked), "Automatic untracked drag end prompts discovery as SHOW");
  check(!Gestures::g_moveSize.endRects.count(untracked), "untracked drag records no gesture samples");
  send(EVENT_OBJECT_LOCATIONCHANGE, untracked);
  check(drain().empty(), "untracked restored-window location change is ignored");

  // Initial-placement routing state for untracked windows.
  g_wm.windowRoutingReady = true;
  g_wm.pendingWindowRoutes[untracked] = {};
  send(EVENT_OBJECT_LOCATIONCHANGE, untracked);
  check(windowEvent(EVENT_OBJECT_LOCATIONCHANGE, untracked), "location change of a routed window is forwarded");
  send(EVENT_SYSTEM_MOVESIZESTART, untracked);
  check(drain().empty(), "untracked drag start posts nothing");
  check(!g_wm.pendingWindowRoutes.count(untracked) && g_wm.windowRoutingEvaluated.count(untracked),
        "user drag cancels initial routing");
  send(EVENT_OBJECT_DESTROY, untracked);
  check(windowEvent(EVENT_OBJECT_DESTROY, untracked), "destroy of a routing-known window is forwarded");
  check(!g_wm.windowRoutingEvaluated.count(untracked), "destroy retires routing bookkeeping");
  send(EVENT_OBJECT_DESTROY, untracked);
  check(drain().empty(), "destroy of an unknown window is ignored");

  // Tracked windows.
  send(EVENT_OBJECT_LOCATIONCHANGE, tracked);
  check(windowEvent(EVENT_OBJECT_LOCATIONCHANGE, tracked), "tiled window location change is forwarded");
  Placement::g_conformanceLeases.leases[tracked] = Placement::ConformanceLease{};
  g_wm.overflowWindows.insert(tracked);
  g_wm.pendingWindowOverflow.push_back({tracked});
  send(EVENT_SYSTEM_MOVESIZESTART, tracked);
  check(drain().empty(), "tracked drag start posts nothing");
  check(!Placement::g_conformanceLeases.leases.count(tracked), "drag start cancels the conformance lease");
  check(!g_wm.HasPendingOverflow(tracked) && !g_wm.overflowWindows.count(tracked), "drag start cancels queued overflow");
  check(Gestures::g_moveSize.startRects.count(tracked) == 1, "drag start records the start frame");
  send(EVENT_SYSTEM_MOVESIZEEND, tracked);
  check(postedOnly(WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(tracked), 0), "drag end posts MOVE_SIZE_END");
  check(Gestures::g_moveSize.endRects.count(tracked) == 1, "drag end records the end frame");
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  send(EVENT_OBJECT_CLOAKED, tracked);
  check(windowEvent(EVENT_OBJECT_CLOAKED, tracked), "cloak is forwarded");
  check(g_wm.reconciledDesktopState == ReconciledDesktopState::TransitionPending, "cloak marks a possible desktop switch");
  send(EVENT_OBJECT_HIDE, tracked);
  check(windowEvent(EVENT_OBJECT_HIDE, tracked), "tracked hide is forwarded");
  g_wm.pendingOverflowSwitch.hwnd = tracked;
  send(EVENT_OBJECT_DESTROY, tracked);
  check(windowEvent(EVENT_OBJECT_DESTROY, tracked), "tracked destroy is forwarded");
  check(!g_wm.pendingOverflowSwitch.hwnd, "destroy cancels a pending desktop follow");

  Reconcile::ForgetManagedWindow(tracked);
  DestroyWindow(child);
  DestroyWindow(untracked);
  DestroyWindow(tracked);
  drain();
  g_wm.windowRoutingEvaluated.clear();
  g_wm.pendingWindowRoutes.clear();
  g_wm.windowRoutingReady = false;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Unknown;
  g_wm.threadId.store(0, std::memory_order_release);
  std::printf("Passed %u WinEvent routing checks.\n", checks);
}

// Characterizes Placement::HandleTiledWindowLocationChange: when a tiled window
// reports a new position, does MultiWM ignore it, open a conformance lease,
// defer, reinforce the tile, or reject the observation?
//
// Uses one real window that is visible to the API but invisible to the user:
// fully transparent, click-through, never activated, and a tool window (which
// MultiWM itself never manages, so a live MultiWM instance ignores it too).
static void RunConformanceLocationTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  uint64_t digest = 1469598103934665603ull;
  auto mix = [&](uint64_t value) { digest ^= value; digest *= 1099511628211ull; };
  auto mixRect = [&](const RECT& r) {
    mix(static_cast<uint32_t>(r.left)); mix(static_cast<uint32_t>(r.top));
    mix(static_cast<uint32_t>(r.right)); mix(static_cast<uint32_t>(r.bottom));
  };

  // MultiWM runs per-monitor DPI aware; without this, a scaled display makes
  // window rectangles (logical) and DWM frames (physical) disagree.
  const DPI_AWARENESS_CONTEXT previousDpiContext =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  TestWindowDesktops windows;
  TestManager manager;
  GUID desktop{};
  desktop.Data1 = 0xC0F0A11;
  windows.desktop = desktop;
  manager.desktop.id = desktop;
  g_vd.initialized = true;
  g_vd.desktopManager = &windows;
  g_vd.managerInternal = &manager;
  g_wm.reconciledDesktop = desktop;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  g_wm.managementMode = ManagementMode::Manual;

  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  DesktopMonitorKey key;
  check(DesktopMonitorKey::FromHMonitor(desktop, monitor, &key), "resolve conformance test monitor");
  const HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
      L"STATIC", L"MultiWM conformance test", WS_POPUP, 0, 0, 300, 300,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(hwnd != nullptr, "create transparent test window");
  SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  const HWND neighbour = reinterpret_cast<HWND>(0x7100004);

  auto seed = [&](TileLayout layout, DWORD pid) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (HWND member : {hwnd, neighbour}) {
      WindowRecord record;
      record.hwnd = member;
      record.pid = member == hwnd ? pid : 4242;
      record.canMove = true;
      workspace.AdmitTiled(record);
      workspace.ClearInsertionPlacement(member);
    }
    g_workspaces.Save(key, workspace);
    return workspace;
  };
  auto authoritativeRect = [&] {
    Workspace workspace;
    RECT rect{};
    check(g_workspaces.Load(key, &workspace) &&
          Placement::GetCurrentAuthoritativeTiledRect(key, workspace, hwnd, &rect),
          "plan authoritative tile");
    return rect;
  };
  auto moveTo = [&](const RECT& r) {
    SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  };
  auto offset = [](RECT r, LONG dx) { r.left += dx; r.right += dx; return r; };
  auto leaseOf = [&]() -> const Placement::ConformanceLease* {
    auto it = Placement::g_conformanceLeases.leases.find(hwnd);
    return it == Placement::g_conformanceLeases.leases.end() ? nullptr : &it->second;
  };
  // One step: optional setup, call the handler, record the observable outcome.
  auto step = [&](const char* name) {
    const bool handled = Placement::HandleTiledWindowLocationChange(hwnd);
    const Placement::ConformanceLease* lease = leaseOf();
    RECT frame{};
    Platform::Windows::GetWindowFrameRect(hwnd, &frame);
    Workspace workspace;
    const bool loaded = g_workspaces.Load(key, &workspace);
    const WindowRecord* record = loaded ? workspace.Find(hwnd) : nullptr;
    mix(std::hash<std::string>{}(name));
    mix(handled ? 1 : 0);
    mix(lease ? 1 : 0);
    if (lease) {
      mixRect(lease->expectedRect);
      mix(lease->attempts);
      mix(lease->repairDueTickMs ? 1 : 0);
      mix(lease->lastRepairTickMs ? 1 : 0);
    }
    mixRect(frame);
    mix(record ? static_cast<uint64_t>(record->state) + 1 : 0);
    mix(loaded ? static_cast<uint64_t>(workspace.Layout()) : 99);
    return handled;
  };
  auto resetLease = [&] {
    Placement::g_conformanceLeases.leases.clear();
    if (g_wm.conformanceTimer) KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  };

  const DWORD pid = GetCurrentProcessId();
  seed(TileLayout::Columns, pid);
  const RECT tile = authoritativeRect();
  check(Layout::RectWidth(tile) > 0 && Layout::RectHeight(tile) > 0, "tile has an area");

  // 1. Already on its tile: just our own placement echo.
  moveTo(tile);
  check(step("on tile") && !leaseOf(), "window on its tile needs no lease");

  // 2. Drifted off the tile: open a lease aimed at the tile.
  moveTo(offset(tile, 37));
  check(step("drift opens lease"), "drift is handled");
  check(leaseOf() && Layout::RectsNear(leaseOf()->expectedRect, tile), "drift opens a lease for the tile");

  // 3. Repair already scheduled: wait for it.
  check(step("repair scheduled"), "scheduled repair defers");

  // 4. Repair due now: reinforce the tile.
  Placement::g_conformanceLeases.leases[hwnd].repairDueTickMs = 0;
  Placement::g_conformanceLeases.leases[hwnd].lastRepairTickMs = 0;
  check(step("repair due"), "due repair is handled");

  // 5. Recently repaired: rate-limited into one deferred re-check.
  moveTo(offset(tile, 51));
  if (Placement::g_conformanceLeases.leases.count(hwnd)) {
    Placement::g_conformanceLeases.leases[hwnd].repairDueTickMs = 0;
    Placement::g_conformanceLeases.leases[hwnd].lastRepairTickMs = GetTickCount64();
  }
  step("rate limited");

  // 6. Lease aimed at an old tile: replaced by one for the current tile.
  if (Placement::g_conformanceLeases.leases.count(hwnd)) {
    Placement::g_conformanceLeases.leases[hwnd].expectedRect = offset(tile, 400);
  }
  step("stale target replaced");

  // 7. Lease already past its deadline: leave it to the expiry timer.
  if (Placement::g_conformanceLeases.leases.count(hwnd)) {
    Placement::g_conformanceLeases.leases[hwnd].expiresAtTickMs = 1;
  }
  check(step("expired lease"), "expired lease is left to the timer");
  resetLease();

  // 8-10. Ineligible observations are rejected and cancel any lease.
  Placement::BeginConformanceLease(hwnd, tile);
  seed(TileLayout::Floating, pid);
  check(!step("floating layout"), "floating layout rejects the observation");
  resetLease();
  Placement::BeginConformanceLease(hwnd, tile);
  seed(TileLayout::Columns, pid + 1);
  check(!step("pid mismatch"), "reused HWND (pid mismatch) rejects the observation");
  check(!leaseOf(), "rejection cancels the lease");
  resetLease();
  seed(TileLayout::Columns, pid);
  ShowWindow(hwnd, SW_HIDE);
  Placement::BeginConformanceLease(hwnd, tile);
  check(!step("hidden"), "hidden window rejects the observation");
  check(!leaseOf(), "hidden window cancels the lease");
  resetLease();

  Reconcile::ForgetManagedWindow(hwnd);
  Reconcile::ForgetManagedWindow(neighbour);
  DestroyWindow(hwnd);
  g_wm.reconciledDesktopState = ReconciledDesktopState::Unknown;
  g_vd.desktopManager = nullptr;
  g_vd.managerInternal = nullptr;
  g_vd.initialized = false;
  SetThreadDpiAwarenessContext(previousDpiContext);
  // Recorded from the implementation as of 2026-11-04 (commit after 59f0168).
  check(digest == 0x19b1f6837e1683fcull, "conformance location trace matches recorded behavior");
  std::printf("Passed %u conformance location checks (trace digest %016llx).\n",
              checks, static_cast<unsigned long long>(digest));
}

static void RunDiscoveryResultTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  using Reconcile::WorkspaceDiscoveryStatus;
  TestWindowDesktops windows;
  TestManager manager;
  GUID current{17}, previous{23};
  windows.desktop = current; manager.desktop.id = current;
  g_vd.initialized = true;
  g_vd.desktopManager = &windows; g_vd.managerInternal = &manager;
  g_wm.managementMode = ManagementMode::Manual;
  g_wm.reconciledDesktop = current;
  g_wm.reconciledDesktopState = ReconciledDesktopState::Settled;
  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  DesktopMonitorKey key;
  check(DesktopMonitorKey::FromHMonitor(current, monitor, &key), "resolve live monitor");
  Workspace empty; empty.SetLayout(TileLayout::Dwindle);
  g_workspaces.Save(key, empty);
  auto result = Reconcile::DiscoverWorkspaceWindows(nullptr, current, {});
  check(result.status == WorkspaceDiscoveryStatus::Unavailable, "invalid monitor is Unavailable");
  check(!result.key.monitor && result.ownershipChanges.empty(), "Unavailable has no resolved key or ownership changes");
  result = Reconcile::DiscoverWorkspaceWindows(monitor, current, {});
  check(result.status == WorkspaceDiscoveryStatus::Unchanged, "existing empty workspace is Unchanged");
  check(Model::DesktopMonitorKeyEqual{}(result.key, key), "Unchanged returns the resolved key");
  check(result.ownershipChanges.empty(), "Unchanged reports no incidental ownership changes");
  DesktopMonitorKey oldKey = key; oldKey.desktopId = previous;
  HWND phantom = reinterpret_cast<HWND>(0x70000001);
  Workspace old;
  WindowRecord record; record.hwnd = phantom; record.pid = 100;
  old.AdmitTiled(record); g_workspaces.Save(oldKey, old);
  const auto saves = Diagnostics::g_runtime.counters.workspaceSaves;
  result = Reconcile::DiscoverWorkspaceWindows(monitor, current, {phantom});
  check(result.status == WorkspaceDiscoveryStatus::Saved, "verified Manual departure saves requested workspace");
  check(result.ownershipChanges.size() == 1 && Model::DesktopMonitorKeyEqual{}(result.ownershipChanges.front(), oldKey),
        "result reports repaired source ownership");
  check(g_workspaces.OwnersOf(phantom).empty(), "Manual policy still forgets cross-desktop membership");
  check(Diagnostics::g_runtime.counters.workspaceSaves == saves + 2, "exact source and requested workspace saves");
  manager.desktop.id = previous;
  result = Reconcile::DiscoverWorkspaceWindows(monitor, current, {});
  check(result.status == WorkspaceDiscoveryStatus::DesktopChanged, "stale snapshot is DesktopChanged");
  check(Model::DesktopMonitorKeyEqual{}(result.key, key), "DesktopChanged retains requested key");
  check(result.ownershipChanges.empty(), "stale snapshot makes no ownership repairs");
  check(g_wm.lifecycleTimer != 0 && g_wm.reconciledDesktopState == ReconciledDesktopState::TransitionPending,
        "DesktopChanged retains deferred retry effects");
  KillTimer(nullptr, g_wm.lifecycleTimer); g_wm.lifecycleTimer = 0;
  g_vd.desktopManager = nullptr; g_vd.managerInternal = nullptr; g_vd.initialized = false;
  std::printf("Passed %u discovery result contract checks.\n", checks);
}

int main() {
  RunDesktopReconciliationTests();
  RunDiscoveryResultTests();
  RunWinEventRoutingTests();
  RunConformanceLocationTests();
}
