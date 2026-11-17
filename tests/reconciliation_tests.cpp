#include "../src/multiwm/multiwm.cpp"
#include <cstdio>

// Conformance tests drive lease timing directly (friend of PendingProofs).
namespace Placement {
struct PendingProofsTestAccess {
  static auto& Leases() { return g_pendingProofs.leases_; }
  static auto& AdmissionRecoveries() { return g_pendingProofs.recoveries_; }
};
}  // namespace Placement
static auto& TestLeases() { return Placement::PendingProofsTestAccess::Leases(); }
static auto& TestAdmissionRecoveries() {
  return Placement::PendingProofsTestAccess::AdmissionRecoveries();
}

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
  g_wm.desktop.reconciled = source.desktopId;
  g_wm.desktop.state = ReconciledDesktopState::Settled;
  g_wm.lifecycle.dirtyWindows = hiddenWindows;
  Reconcile::ReconcileDeferredLifecycle();
  check(g_wm.desktop.state == ReconciledDesktopState::TransitionPending,
        "whole pass detects desktop change during enumeration");
  for (HWND hwnd : hiddenWindows) {
    check(Model::ContainsWindow(g_wm.lifecycle.dirtyWindows, hwnd), "retry retains dirty window");
  }
  check(g_wm.timers.lifecycle != 0, "whole pass schedules retry");
  check(std::any_of(g_wm.lifecycle.pendingArranges.begin(), g_wm.lifecycle.pendingArranges.end(),
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
  TestLeases()[hiddenWindows.front()] = hiddenLease;
  check(!Placement::HandleTiledWindowLocationChange(hiddenWindows.front()), "hidden location event is ineligible for repair");
  check(!TestLeases().count(hiddenWindows.front()), "hidden location event cancels stale placement work");
  check(g_workspaces.Load(liveKey, &retried) && retried.HasSameLayoutStateAs(expectedHidden),
        "late location event preserves hidden members and saved slots");
  for (HWND hwnd : hiddenWindows) {
    Reconcile::ForgetManagedWindow(hwnd);
    DestroyWindow(hwnd);
  }
  KillTimer(nullptr, g_wm.timers.lifecycle);
  g_wm.timers.lifecycle = 0;
  g_wm.lifecycle.dirtyWindows.clear();
  g_wm.lifecycle.pendingArranges.clear();
  manager.switchAfter = UINT_MAX;

  manager.desktop.id = source.desktopId;
  g_wm.desktop.state = ReconciledDesktopState::Settled;
  check(Reconcile::ValidateSnapshotDesktop(source.desktopId), "stable snapshot accepted");
  check(g_wm.timers.lifecycle == 0, "stable snapshot does not poll");
  manager.desktop.id = target.desktopId;
  check(!Reconcile::ValidateSnapshotDesktop(source.desktopId), "desktop switch invalidates snapshot");
  check(g_wm.desktop.state == ReconciledDesktopState::TransitionPending && g_wm.timers.lifecycle != 0, "switch schedules settled retry");
  KillTimer(nullptr, g_wm.timers.lifecycle);
  g_wm.timers.lifecycle = 0;
  g_vd.managerInternal = nullptr;
  check(!Reconcile::ValidateSnapshotDesktop(source.desktopId), "missing current desktop invalidates snapshot");
  check(g_wm.timers.lifecycle != 0 && g_wm.lifecycle.retryAfterPlatformRecovery, "unavailable desktop retains recovery path");
  KillTimer(nullptr, g_wm.timers.lifecycle);
  KillTimer(nullptr, g_wm.timers.maintenance);
  g_wm.timers.lifecycle = g_wm.timers.maintenance = 0;
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
  g_wm.routing.ready = false;
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
  g_wm.routing.ready = true;
  g_wm.routing.pending[untracked] = {};
  send(EVENT_OBJECT_LOCATIONCHANGE, untracked);
  check(windowEvent(EVENT_OBJECT_LOCATIONCHANGE, untracked), "location change of a routed window is forwarded");
  send(EVENT_SYSTEM_MOVESIZESTART, untracked);
  check(drain().empty(), "untracked drag start posts nothing");
  check(!g_wm.routing.pending.count(untracked) && g_wm.routing.evaluated.count(untracked),
        "user drag cancels initial routing");
  send(EVENT_OBJECT_DESTROY, untracked);
  check(windowEvent(EVENT_OBJECT_DESTROY, untracked), "destroy of a routing-known window is forwarded");
  check(!g_wm.routing.evaluated.count(untracked), "destroy retires routing bookkeeping");
  send(EVENT_OBJECT_DESTROY, untracked);
  check(drain().empty(), "destroy of an unknown window is ignored");

  // Tracked windows.
  send(EVENT_OBJECT_LOCATIONCHANGE, tracked);
  check(windowEvent(EVENT_OBJECT_LOCATIONCHANGE, tracked), "tiled window location change is forwarded");
  TestLeases()[tracked] = Placement::ConformanceLease{};
  g_wm.overflow.windows.insert(tracked);
  g_wm.overflow.queue.push_back({tracked});
  send(EVENT_SYSTEM_MOVESIZESTART, tracked);
  check(drain().empty(), "tracked drag start posts nothing");
  check(!TestLeases().count(tracked), "drag start cancels the conformance lease");
  check(!g_wm.HasPendingOverflow(tracked) && !g_wm.overflow.windows.count(tracked), "drag start cancels queued overflow");
  check(Gestures::g_moveSize.startRects.count(tracked) == 1, "drag start records the start frame");
  send(EVENT_SYSTEM_MOVESIZEEND, tracked);
  check(postedOnly(WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(tracked), 0), "drag end posts MOVE_SIZE_END");
  check(Gestures::g_moveSize.endRects.count(tracked) == 1, "drag end records the end frame");
  g_wm.desktop.state = ReconciledDesktopState::Settled;
  send(EVENT_OBJECT_CLOAKED, tracked);
  check(windowEvent(EVENT_OBJECT_CLOAKED, tracked), "cloak is forwarded");
  check(g_wm.desktop.state == ReconciledDesktopState::TransitionPending, "cloak marks a possible desktop switch");
  send(EVENT_OBJECT_HIDE, tracked);
  check(windowEvent(EVENT_OBJECT_HIDE, tracked), "tracked hide is forwarded");
  g_wm.overflow.follow.hwnd = tracked;
  send(EVENT_OBJECT_DESTROY, tracked);
  check(windowEvent(EVENT_OBJECT_DESTROY, tracked), "tracked destroy is forwarded");
  check(!g_wm.overflow.follow.hwnd, "destroy cancels a pending desktop follow");

  Reconcile::ForgetManagedWindow(tracked);
  DestroyWindow(child);
  DestroyWindow(untracked);
  DestroyWindow(tracked);
  drain();
  g_wm.routing.evaluated.clear();
  g_wm.routing.pending.clear();
  g_wm.routing.ready = false;
  g_wm.desktop.state = ReconciledDesktopState::Unknown;
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
  g_wm.desktop.reconciled = desktop;
  g_wm.desktop.state = ReconciledDesktopState::Settled;
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
    auto it = TestLeases().find(hwnd);
    return it == TestLeases().end() ? nullptr : &it->second;
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
    TestLeases().clear();
    if (g_wm.timers.conformance) KillTimer(nullptr, g_wm.timers.conformance);
    g_wm.timers.conformance = 0;
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
  TestLeases()[hwnd].repairDueTickMs = 0;
  TestLeases()[hwnd].lastRepairTickMs = 0;
  check(step("repair due"), "due repair is handled");

  // 5. Recently repaired: rate-limited into one deferred re-check.
  moveTo(offset(tile, 51));
  if (TestLeases().count(hwnd)) {
    TestLeases()[hwnd].repairDueTickMs = 0;
    TestLeases()[hwnd].lastRepairTickMs = GetTickCount64();
  }
  step("rate limited");

  // 6. Lease aimed at an old tile: replaced by one for the current tile.
  if (TestLeases().count(hwnd)) {
    TestLeases()[hwnd].expectedRect = offset(tile, 400);
  }
  step("stale target replaced");

  // 7. Lease already past its deadline: leave it to the expiry timer.
  if (TestLeases().count(hwnd)) {
    TestLeases()[hwnd].expiresAtTickMs = 1;
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
  g_wm.desktop.state = ReconciledDesktopState::Unknown;
  g_vd.desktopManager = nullptr;
  g_vd.managerInternal = nullptr;
  g_vd.initialized = false;
  SetThreadDpiAwarenessContext(previousDpiContext);
  // Recorded from the implementation as of 2026-11-04 (commit after 59f0168).
  check(digest == 0x19b1f6837e1683fcull, "conformance location trace matches recorded behavior");
  std::printf("Passed %u conformance location checks (trace digest %016llx).\n",
              checks, static_cast<unsigned long long>(digest));
}

// Sequences in which a window that fitted must not be floated later: a sibling
// leaving changes the remaining slots' shape, or another window raises its
// minimum. Fake HWNDs only; toasts disabled; no native window is moved.
static void RunSettledWindowRecoveryTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const SettingsState savedSettings = g_settings;
  const bool savedToasts = TrayUi::g_preferences.showToasts;
  const ManagementMode savedMode = g_wm.managementMode;
  g_settings.adaptiveDwindleSlots = 0;  // Fixed Dwindle geometry: only order can help.
  g_settings.reverseDwindle = false;
  TrayUi::g_preferences.showToasts = false;
  g_wm.managementMode = ManagementMode::Manual;

  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  Platform::Monitors::WorkspaceMetrics metrics;
  metrics.workArea = {0, 0, 1000, 1000};
  metrics.gap = 0;
  metrics.dpi = 96;
  DesktopMonitorKey key;
  GUID desktop{};
  desktop.Data1 = 0x5E77ED;
  check(DesktopMonitorKey::FromHMonitor(desktop, monitor, &key), "resolve test monitor");
  const HWND a = reinterpret_cast<HWND>(0x7400000);
  const HWND b = reinterpret_cast<HWND>(0x7400004);
  const HWND c = reinterpret_cast<HWND>(0x7400008);
  const HWND d = reinterpret_cast<HWND>(0x740000C);

  auto makeWorkspace = [&](TileLayout layout, std::initializer_list<std::pair<HWND, LONG>> members) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (const auto& [hwnd, minWidth] : members) {
      WindowRecord record;
      record.hwnd = hwnd;
      record.pid = 901;
      record.canMove = false;
      workspace.AdmitTiled(record);
      if (minWidth > 1) workspace.UpdateConstraints(hwnd, {minWidth, 150, 96, true});
      workspace.ClearInsertionPlacement(hwnd);
    }
    return workspace;
  };
  auto plan = [&](const Workspace& workspace) {
    std::vector<RECT> rects;
    return Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects);
  };
  auto recover = [&](Workspace& workspace) {
    std::vector<RECT> rects;
    Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects);
    g_workspaces.Save(key, workspace);
    Placement::ArrangementContext context{key, monitor, metrics, nullptr};
    return Placement::RecoverUnsatisfiableLayout(workspace, context, rects);
  };
  auto tiled = [](const Workspace& workspace, HWND hwnd) { return workspace.IsTiled(hwnd); };

  {
    // Dwindle: a user-made 0.2 divider on slot 1 suits b (300 wide, 150 tall)
    // there. Closing a moves b to slot 0, whose split runs the other way: 200 wide.
    Workspace workspace = makeWorkspace(TileLayout::Dwindle, {{a, 1}, {b, 300}, {c, 1}});
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "Dwindle siblings fit");
    MoveSizeGesture gesture;
    gesture.hasRects = true;
    gesture.intent = MoveSizeIntent::Resize;
    gesture.start = gesture.end = rects[1];
    gesture.end.bottom -= 300;
    check(workspace.LearnDwindleResize(metrics.workArea, 0, 1, gesture) &&
              std::abs(workspace.DwindleRatios()[1] - 0.2) < 1e-9, "learn slot 1 divider");
    check(plan(workspace) == Layout::LayoutPlanStatus::Success, "b fits its short slot");
    check(workspace.Forget(a), "close first window");
    check(plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable, "shifted slots no longer fit b");
    check(recover(workspace) == Placement::ArrangementStep::Replan, "recovery replans");
    check(tiled(workspace, b) && tiled(workspace, c), "closing a sibling floats nobody");
    check(workspace.TiledWindows() == std::vector<HWND>({c, b}), "settled windows trade slots instead");
    check(plan(workspace) == Layout::LayoutPlanStatus::Success, "reordered Dwindle fits");
  }
  {
    // Master + Stack: b and d each need 600 of 1000 pixels and both fit in the
    // stack. Closing master a would promote b beside d; promoting c fits instead.
    Workspace workspace = makeWorkspace(TileLayout::MasterStack, {{a, 1}, {b, 600}, {c, 1}, {d, 600}});
    check(plan(workspace) == Layout::LayoutPlanStatus::Success, "wide stack members fit");
    check(workspace.Forget(a), "close master");
    check(plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable, "promoted master no longer fits");
    check(recover(workspace) == Placement::ArrangementStep::Replan, "recovery replans");
    check(tiled(workspace, b) && tiled(workspace, c) && tiled(workspace, d), "closing master floats nobody");
    check(workspace.TiledWindows() == std::vector<HWND>({c, b, d}), "a narrow window becomes master");
  }
  {
    // A settled window raising its own minimum floats itself, not the newest sibling.
    Workspace workspace = makeWorkspace(TileLayout::Columns, {{a, 300}, {b, 300}, {c, 300}});
    check(plan(workspace) == Layout::LayoutPlanStatus::Success, "three columns fit");
    workspace.UpdateConstraints(a, {500, 150, 96, true});
    check(plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable, "raised minimum overflows");
    check(recover(workspace) == Placement::ArrangementStep::Replan, "recovery floats one window");
    check(!tiled(workspace, a) && tiled(workspace, b) && tiled(workspace, c), "the window that grew is floated");
  }
  {
    // An unsettled admission that fitted is not blamed for a later raised minimum.
    Workspace workspace = makeWorkspace(TileLayout::Columns, {{a, 300}, {b, 300}});
    WindowRecord record;
    record.hwnd = c;
    record.pid = 901;
    record.canMove = false;
    workspace.AdmitTiled(record);
    workspace.UpdateConstraints(c, {300, 150, 96, true});
    check(plan(workspace) == Layout::LayoutPlanStatus::Success, "pending admission fits");
    workspace.UpdateConstraints(b, {500, 150, 96, true});
    check(recover(workspace) == Placement::ArrangementStep::Replan, "recovery floats one window");
    check(!tiled(workspace, b) && tiled(workspace, a) && tiled(workspace, c), "growth, not pending admission, is floated");
    check(workspace.Find(c)->pendingInsertionPlacement, "admission keeps settling");
  }
  {
    // A newcomer that does not fit is still rejected rather than reordering or
    // floating settled windows.
    Workspace workspace = makeWorkspace(TileLayout::Columns, {{a, 400}, {b, 400}});
    WindowRecord record;
    record.hwnd = c;
    record.pid = 901;
    record.canMove = false;
    workspace.AdmitTiled(record);
    workspace.UpdateConstraints(c, {400, 150, 96, true});
    check(plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable, "newcomer conflicts");
    check(recover(workspace) == Placement::ArrangementStep::Replan, "recovery floats one window");
    check(!tiled(workspace, c) && tiled(workspace, a) && tiled(workspace, b), "newcomer is rejected");
  }

  g_workspaces.Save(key, Workspace{});
  g_settings = savedSettings;
  TrayUi::g_preferences.showToasts = savedToasts;
  g_wm.managementMode = savedMode;
  std::printf("Passed %d settled window recovery checks.\n", checks);
}

// Characterizes Placement::RecoverUnsatisfiableLayout: given a workspace whose
// windows cannot all fit, which window is floated, or how is it reordered?
// Fake HWNDs only; toasts disabled; no native window is moved.
static void RunConstraintRecoveryDigest() {
  uint64_t digest = 1469598103934665603ull;
  auto mix = [&](uint64_t value) { digest ^= value; digest *= 1099511628211ull; };
  uint32_t seed = 4111;
  auto random = [&](uint32_t bound) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % bound; };

  const bool savedToasts = TrayUi::g_preferences.showToasts;
  const ManagementMode savedMode = g_wm.managementMode;
  TrayUi::g_preferences.showToasts = false;
  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  // Fixed geometry so the digest does not depend on this machine's displays.
  Platform::Monitors::WorkspaceMetrics metrics;
  metrics.workArea = {0, 0, 1920, 1040};
  metrics.gap = 12;
  metrics.dpi = 96;
  DesktopMonitorKey key;
  GUID desktop{};
  desktop.Data1 = 0xC0457;
  if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &key)) std::exit(1);
  const LONG width = Layout::RectWidth(metrics.workArea);
  const LONG height = Layout::RectHeight(metrics.workArea);
  const TileLayout layouts[] = {TileLayout::MasterStack, TileLayout::MasterStackH, TileLayout::Columns,
                                TileLayout::Rows, TileLayout::Dwindle, TileLayout::Monocle};

  size_t exercised = 0, replans = 0;
  for (int iteration = 0; iteration < 3000; ++iteration) {
    g_wm.managementMode = random(2) ? ManagementMode::Automatic : ManagementMode::Manual;
    Workspace workspace;
    workspace.SetLayout(layouts[random(std::size(layouts))]);
    const size_t count = 2 + random(5);
    const size_t pending = random(count + 2);  // >= count: no pending admission.
    std::vector<HWND> hwnds;
    for (size_t i = 0; i < count; ++i) {
      const HWND hwnd = reinterpret_cast<HWND>(0x7300000 + i * 4);
      hwnds.push_back(hwnd);
      WindowRecord record;
      record.hwnd = hwnd;
      record.pid = 900;
      record.canMove = false;
      workspace.AdmitTiled(record);
      if (random(4) != 0) {
        WindowConstraints constraints;
        constraints.valid = true;
        constraints.dpi = metrics.dpi;
        // Mostly plausible minimums, occasionally larger than the whole monitor.
        constraints.minWidth = 1 + static_cast<LONG>(random(static_cast<uint32_t>(width + width / 4)));
        constraints.minHeight = 1 + static_cast<LONG>(random(static_cast<uint32_t>(height + height / 4)));
        workspace.UpdateConstraints(hwnd, constraints);
      }
      if (i != pending) workspace.ClearInsertionPlacement(hwnd);
    }
    std::vector<RECT> rects;
    if (Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects) !=
        Layout::LayoutPlanStatus::Unsatisfiable) {
      continue;
    }
    ++exercised;
    g_workspaces.Save(key, workspace);
    const HWND preferred = random(3) == 0 ? hwnds[random(static_cast<uint32_t>(count))] : nullptr;
    Placement::ArrangementContext context{key, monitor, metrics, preferred};
    const auto step = Placement::RecoverUnsatisfiableLayout(workspace, context, rects);
    replans += step == Placement::ArrangementStep::Replan;

    auto mixWorkspace = [&](const Workspace& state) {
      mix(static_cast<uint64_t>(state.Layout()));
      for (HWND hwnd : state.TiledWindows()) mix(reinterpret_cast<uintptr_t>(hwnd));
      for (HWND hwnd : hwnds) {
        const WindowRecord* record = state.Find(hwnd);
        mix(record ? 1 + static_cast<uint64_t>(record->state) : 0);
        mix(record && record->pendingInsertionPlacement);
      }
    };
    mix(iteration);
    mix(static_cast<uint64_t>(step));
    mix(context.constraintFailureReported);
    mixWorkspace(workspace);
    Workspace saved;
    mix(g_workspaces.Load(key, &saved));
    mixWorkspace(saved);
    mix(g_wm.overflow.queue.size());
    mix(Placement::g_pendingProofs.AdmissionRecoveryCount());

    g_workspaces.Save(key, Workspace{});
    TestAdmissionRecoveries().clear();
    TestLeases().clear();
    g_wm.overflow.queue.clear();
    g_wm.overflow.windows.clear();
  }
  if (g_wm.timers.overflow) KillTimer(nullptr, g_wm.timers.overflow);
  if (g_wm.timers.conformance) KillTimer(nullptr, g_wm.timers.conformance);
  g_wm.timers.overflow = g_wm.timers.conformance = 0;
  TrayUi::g_preferences.showToasts = savedToasts;
  g_wm.managementMode = savedMode;
  if (exercised < 500 || replans == 0 || replans == exercised) {
    std::fprintf(stderr, "FAILED: recovery digest inputs do not exercise all outcomes (%zu/%zu)\n", replans, exercised);
    std::exit(1);
  }
  // Recorded from the implementation as of 2026-11-16: settled-window conflicts
  // reorder or float a window from the conflict instead of an uninvolved
  // pending admission (168 of 2467 outcomes changed vs. 2026-11-04).
  if (digest != 0x59ab62281bd3f20cull) {
    std::fprintf(stderr, "FAILED: constraint recovery digest %016llx changed\n", static_cast<unsigned long long>(digest));
    std::exit(1);
  }
  std::printf("Constraint recovery digest %016llx over %zu unsatisfiable layouts (%zu replans).\n",
              static_cast<unsigned long long>(digest), exercised, replans);
}

// Writes a real diagnostic report and fingerprints its shape: every line with
// digits and hex runs masked and spacing collapsed, so timings, PIDs and HWND
// values do not matter but
// section order, labels and PASS/FAIL verdicts do.
static void RunDiagnosticReportShapeTest() {
  wchar_t tempPath[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempPath);
  const std::wstring directory = std::wstring(tempPath) + L"multiwm_report_test";
  CreateDirectoryW(directory.c_str(), nullptr);
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW((directory + L"\\*.txt").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do { DeleteFileW((directory + L"\\" + found.cFileName).c_str()); } while (FindNextFileW(search, &found));
    FindClose(search);
  }

  DesktopMonitorKey key;
  key.desktopId.Data1 = 0xD1A6;
  key.monitor.deviceId = L"IFACE:TEST_REPORT_MONITOR";
  Workspace workspace;
  workspace.SetLayout(TileLayout::Columns);
  for (int i = 0; i < 3; ++i) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(0x7200000 + i * 4);
    record.pid = 777;
    record.canMove = false;
    if (i == 2) workspace.AdmitFloating(record); else workspace.AdmitTiled(record);
  }
  g_workspaces.Save(key, workspace);
  g_wm.lifecycle.dirtyWindows = {reinterpret_cast<HWND>(0x7200000)};

  const std::wstring savedPath = g_settings.diagnosticsOutputPath;
  g_settings.diagnosticsOutputPath = directory;
  Diagnostics::WriteDiagnosticReport();
  g_settings.diagnosticsOutputPath = savedPath;
  g_wm.lifecycle.dirtyWindows.clear();
  for (const auto& entry : workspace.Records()) Reconcile::ForgetManagedWindow(entry.first);

  search = FindFirstFileW((directory + L"\\*.txt").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "FAILED: diagnostic report not written\n"); std::exit(1); }
  FindClose(search);
  const std::wstring file = directory + L"\\" + found.cFileName;
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  std::string bytes(GetFileSize(handle, nullptr), '\0');
  DWORD read = 0;
  ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(handle);
  DeleteFileW(file.c_str());

  uint64_t digest = 1469598103934665603ull;
  size_t lines = 0;
  bool inRun = false;
  char previous = 0;
  for (char ch : bytes) {
    // Column padding depends on number widths; treat any run of spaces as one.
    if (ch == ' ' && previous == ' ') continue;
    previous = ch;
    const bool maskable = std::isxdigit(static_cast<unsigned char>(ch)) != 0;
    if (ch == '\n') ++lines;
    // Mask runs containing a digit: words like "Dead" survive, numbers/hex do not.
    if (maskable && (std::isdigit(static_cast<unsigned char>(ch)) || inRun)) {
      if (!inRun) { digest ^= '#'; digest *= 1099511628211ull; }
      inRun = true;
      continue;
    }
    inRun = false;
    digest ^= static_cast<unsigned char>(ch);
    digest *= 1099511628211ull;
  }
  // Recorded from the implementation as of 2026-11-17 (companion snapshot
  // section/counters, maximum sizes, ABI and liveness-event state). Intentional
  // report format changes must update this value.
  if (digest != 0xb0ca6b0a8a727976ull) {
    std::fprintf(stderr, "FAILED: diagnostic report shape changed (%016llx)\n", static_cast<unsigned long long>(digest));
    std::exit(1);
  }
  std::printf("Diagnostic report shape digest %016llx over %zu lines.\n",
              static_cast<unsigned long long>(digest), lines);
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
  g_wm.desktop.reconciled = current;
  g_wm.desktop.state = ReconciledDesktopState::Settled;
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
  check(g_wm.timers.lifecycle != 0 && g_wm.desktop.state == ReconciledDesktopState::TransitionPending,
        "DesktopChanged retains deferred retry effects");
  KillTimer(nullptr, g_wm.timers.lifecycle); g_wm.timers.lifecycle = 0;
  g_vd.desktopManager = nullptr; g_vd.managerInternal = nullptr; g_vd.initialized = false;
  std::printf("Passed %u discovery result contract checks.\n", checks);
}

// Local COM references must be released exactly once on every path, and an
// out-parameter slot must not leak a reference it already holds.
static void RunComRefOwnershipTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  struct CountingObject final : IUnknown {
    unsigned releases = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return 0; }
  };
  using Platform::VirtualDesktop::ComRef;
  CountingObject first, second;
  {
    ComRef<IUnknown> ref;
    *ref.Put() = &first;
    *ref.Put() = &second;
    check(first.releases == 1, "Put releases the previously held reference");
    ComRef<IUnknown> moved = std::move(ref);
    check(!ref && moved.Get() == &second, "move transfers ownership");
  }
  check(second.releases == 1, "scope exit releases the moved reference once");
  {
    ComRef<IUnknown> ref;
    *ref.Put() = &first;
    ref.Reset();
    ref.Reset();
  }
  check(first.releases == 2, "Reset releases once and leaves nothing for scope exit");
  std::printf("Passed %u COM reference ownership checks.\n", checks);
}

// Exercise the publisher's real border cache across native state transitions.
// A private desktop keeps the shown test window away from live window managers.
static void RunFrameMarginCacheTests() {
  wchar_t desktopName[80];
  swprintf(desktopName, ARRAYSIZE(desktopName), L"MultiWMFrameCacheTests_%lu", GetCurrentProcessId());
  HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  if (!desktop) std::exit(1);
  HANDLE worker = CreateThread(nullptr, 0, [](void* parameter) -> DWORD {
    int checks = 0;
    auto check = [&](bool passed, const char* message) {
      if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
      ++checks;
    };
    check(SetThreadDesktop(static_cast<HDESK>(parameter)), "select border-cache test desktop");
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"MultiWM border cache test", WS_OVERLAPPEDWINDOW,
                                  100, 100, 600, 400, nullptr, nullptr, nullptr, nullptr);
    check(hwnd != nullptr, "create border-cache test window");
    BOOL cloak = TRUE;
    check(SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak))), "cloak border-cache test window");
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    WindowRecord record;
    record.hwnd = hwnd;
    record.pid = GetCurrentProcessId();
    Workspace workspace;
    check(workspace.AdmitTiled(record), "admit border-cache test window");
    const UINT dpi = GetDpiForWindow(hwnd);
    CompanionSnapshot::RememberFrameMargins(workspace, dpi);
    check(CompanionSnapshot::g_publisher.frameMargins.contains(hwnd), "measure visible restored borders");
    const auto measured = CompanionSnapshot::g_publisher.frameMargins.at(hwnd);

    for (int command : {SW_HIDE, SW_SHOWMINNOACTIVE, SW_HIDE, SW_SHOWMAXIMIZED, SW_HIDE, SW_SHOWNOACTIVATE}) {
      ShowWindow(hwnd, command);
      CompanionSnapshot::RememberFrameMargins(workspace, dpi);
      const auto hint = CompanionSnapshot::MakeRestoreWindowHint(record, 0, RECT{10, 10, 500, 400});
      const auto cached = CompanionSnapshot::g_publisher.frameMargins.find(hwnd);
      check(cached != CompanionSnapshot::g_publisher.frameMargins.end() &&
                cached->second.style == measured.style && EqualRect(&cached->second.margins, &measured.margins),
            "show-state transitions retain the restored measurement without remeasuring");
      check(hint.marginsDpi == dpi && EqualRect(&hint.frameMargins, &measured.margins),
            "hidden, minimized and maximized windows publish restored borders");
    }
    for (bool topmost : {true, false}) {
      check(SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE), "change stacking order");
      EnableWindow(hwnd, !topmost);
      RECT windowRect{}, frame{};
      GetWindowRect(hwnd, &windowRect);
      check(SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))),
            "read borders after stacking and enabled-state changes");
      const RECT margins{frame.left - windowRect.left, frame.top - windowRect.top,
                         windowRect.right - frame.right, windowRect.bottom - frame.bottom};
      check(EqualRect(&margins, &measured.margins) && CompanionSnapshot::FrameInputsUnchanged(hwnd, measured),
            "stacking and enabled state preserve actual borders and cache validity");
    }

    ShowWindow(hwnd, SW_HIDE);
    // Actual frame-style changes still invalidate the cache while hidden, when
    // it cannot be safely replaced with a fresh measurement.
    for (int change = 0; change < 3; ++change) {
      SetWindowLongPtrW(hwnd, GWL_STYLE, measured.style & ~WS_VISIBLE);
      SetWindowLongPtrW(hwnd, GWL_EXSTYLE, measured.exStyle);
      if (change < 2) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, (measured.style & ~WS_VISIBLE) ^
                                         (change == 0 ? WS_THICKFRAME : WS_CAPTION));
      } else {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, measured.exStyle ^ WS_EX_CLIENTEDGE);
      }
      CompanionSnapshot::g_publisher.frameMargins[hwnd] = measured;
      const auto hint = CompanionSnapshot::MakeRestoreWindowHint(record, 0, RECT{10, 10, 500, 400});
      check(hint.marginsDpi == 0, "changed frame styles suppress stale published borders");
      CompanionSnapshot::RememberFrameMargins(workspace, dpi);
      check(!CompanionSnapshot::g_publisher.frameMargins.contains(hwnd), "unmeasurable stale borders are discarded");
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, measured.style & ~WS_VISIBLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, measured.exStyle);
    auto stale = measured;
    ++stale.windowDpi;
    check(!CompanionSnapshot::FrameInputsUnchanged(hwnd, stale), "window-DPI changes invalidate cached borders");
    stale = measured;
    ++stale.processId;
    CompanionSnapshot::g_publisher.frameMargins[hwnd] = stale;
    check(CompanionSnapshot::MakeRestoreWindowHint(record, 0, RECT{10, 10, 500, 400}).marginsDpi == 0,
          "another process cannot inherit cached borders");
    CompanionSnapshot::g_publisher.frameMargins.clear();
    DestroyWindow(hwnd);
    std::printf("Passed %d border-cache checks.\n", checks);
    return 0;
  }, desktop, 0, nullptr);
  if (!worker || WaitForSingleObject(worker, 30000) != WAIT_OBJECT_0) std::exit(1);
  DWORD result = 0;
  GetExitCodeThread(worker, &result);
  CloseHandle(worker);
  CloseDesktop(desktop);
  if (result) std::exit(1);
}

// The probe reads both tracking limits from WM_GETMINMAXINFO in frame geometry.
// A maximum is recorded only when the app lowered it below the system default.
// Hidden, never-shown test windows: no window manager or mod reacts to them.
static void RunConstraintProbeTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  static POINT reportedMinimum{};
  static POINT reportedMaximum{};
  static bool overrideLimits = false;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
    if (message == WM_GETMINMAXINFO && overrideLimits) {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize = reportedMinimum;
      info->ptMaxTrackSize = reportedMaximum;
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  };
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"MultiWMConstraintProbeTest";
  check(RegisterClassW(&windowClass) != 0, "register probe test class");
  // Probe the way MultiWM's WM thread does: from a per-monitor-aware thread. A
  // window created under that context is per-monitor aware too.
  const DPI_AWARENESS_CONTEXT previousContext =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"MultiWM probe test", WS_OVERLAPPEDWINDOW,
                                    100, 100, 800, 600, nullptr, nullptr, windowClass.hInstance, nullptr);
  check(hwnd != nullptr, "create hidden probe test window");
  LONG decorationWidth = 0;
  LONG decorationHeight = 0;
  Platform::Windows::GetFrameDecorationSpan(hwnd, &decorationWidth, &decorationHeight);

  overrideLimits = false;
  const WindowConstraints defaults = Platform::Windows::ProbeWindowConstraints(hwnd);
  check(defaults.valid && defaults.maxWidth == 0 && defaults.maxHeight == 0,
        "the system default maximum is recorded as no maximum");

  overrideLimits = true;
  reportedMinimum = {300, 200};
  reportedMaximum = {500, 400};
  const WindowConstraints capped = Platform::Windows::ProbeWindowConstraints(hwnd);
  check(capped.valid && capped.maxWidth == 500 - decorationWidth && capped.maxHeight == 400 - decorationHeight,
        "a lowered maximum is recorded in frame geometry");
  check(capped.minWidth == std::max<LONG>(300, GetSystemMetricsForDpi(SM_CXMINTRACK, capped.dpi)) - decorationWidth,
        "the minimum is read as before");

  // A maximum below the minimum is not a real limit on the window's size.
  reportedMaximum = {100, 100};
  const WindowConstraints inverted = Platform::Windows::ProbeWindowConstraints(hwnd);
  check(inverted.maxWidth == inverted.minWidth && inverted.maxHeight == inverted.minHeight,
        "a maximum is never below the minimum");

  // A DPI-unaware window answers in its own 96-DPI units, unconverted. The probe
  // must scale them to the monitor's physical pixels, or MultiWM plans tiles the
  // window refuses on a scaled display.
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
  const HWND unaware = CreateWindowExW(0, windowClass.lpszClassName, L"MultiWM unaware probe test",
                                       WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, nullptr, nullptr,
                                       windowClass.hInstance, nullptr);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  check(unaware != nullptr && GetDpiForWindow(unaware) == 96, "create hidden DPI-unaware test window");
  LONG unawareDecorationWidth = 0;
  LONG unawareDecorationHeight = 0;
  Platform::Windows::GetFrameDecorationSpan(unaware, &unawareDecorationWidth, &unawareDecorationHeight);
  overrideLimits = false;
  const WindowConstraints unawareDefaults = Platform::Windows::ProbeWindowConstraints(unaware);
  check(unawareDefaults.valid && unawareDefaults.maxWidth == 0 && unawareDefaults.maxHeight == 0,
        "an unaware window's default maximum is still recorded as no maximum");
  overrideLimits = true;
  reportedMinimum = {300, 200};
  reportedMaximum = {500, 400};
  const WindowConstraints scaled = Platform::Windows::ProbeWindowConstraints(unaware);
  const int monitorDpi = static_cast<int>(scaled.dpi);
  check(scaled.minWidth == MulDiv(std::max<LONG>(300, GetSystemMetricsForDpi(SM_CXMINTRACK, 96)), monitorDpi, 96) -
                               unawareDecorationWidth &&
            scaled.minHeight == MulDiv(std::max<LONG>(200, GetSystemMetricsForDpi(SM_CYMINTRACK, 96)), monitorDpi, 96) -
                                    unawareDecorationHeight,
        "an unaware window's minimum is scaled to physical pixels");
  check(scaled.maxWidth == MulDiv(500, monitorDpi, 96) - unawareDecorationWidth &&
            scaled.maxHeight == MulDiv(400, monitorDpi, 96) - unawareDecorationHeight,
        "an unaware window's maximum is scaled to physical pixels");

  DestroyWindow(unaware);
  DestroyWindow(hwnd);
  SetThreadDpiAwarenessContext(previousContext);
  UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
  std::printf("Passed %d constraint probe checks.\n", checks);
}

// The companion moves a new window to the centred floating frame when MultiWM
// predicts that overflow floats it (newWindowFloats). Check the prediction
// against what discovery's admission plus arrangement's real recovery do, over
// random settled workspaces, with and without a preferred window, and with the
// newcomer reporting a larger minimum than the prediction assumed.
static void RunOverflowFloatPredictionTests() {
  uint32_t seed = 9157;
  auto random = [&](uint32_t bound) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % bound; };
  const bool savedToasts = TrayUi::g_preferences.showToasts;
  const ManagementMode savedMode = g_wm.managementMode;
  TrayUi::g_preferences.showToasts = false;
  g_wm.managementMode = ManagementMode::Automatic;
  const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  Platform::Monitors::WorkspaceMetrics metrics;
  metrics.workArea = {0, 0, 1920, 1040};
  metrics.gap = 12;
  metrics.dpi = 96;
  DesktopMonitorKey key;
  GUID desktop{};
  desktop.Data1 = 0x0F10A7;
  if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &key)) std::exit(1);
  const TileLayout layouts[] = {TileLayout::MasterStack, TileLayout::MasterStackH, TileLayout::Columns,
                                TileLayout::Rows, TileLayout::Dwindle, TileLayout::Monocle};
  const HWND incoming = reinterpret_cast<HWND>(0x7FFF0000);

  // missedFloats: the predicted minimal window floated without a prediction.
  size_t predictedFloats = 0, predictedTiles = 0, unpredictedFloats = 0, missedFloats = 0;
  for (int iteration = 0; iteration < 8000; ++iteration) {
    Workspace workspace;
    workspace.SetLayout(layouts[random(std::size(layouts))]);
    const size_t count = 1 + random(6);
    // Half the workspaces are crowded: each window needs 70-100% of an equal
    // share on both axes, so the windows fit but often leave no room.
    const bool crowded = random(2) == 0;
    const LONG shareWidth = (1920 - metrics.gap * static_cast<LONG>(count - 1)) / static_cast<LONG>(count);
    const LONG shareHeight = (1040 - metrics.gap * static_cast<LONG>(count - 1)) / static_cast<LONG>(count);
    std::vector<HWND> hwnds;
    for (size_t i = 0; i < count; ++i) {
      const HWND hwnd = reinterpret_cast<HWND>(0x7400000 + i * 4);
      hwnds.push_back(hwnd);
      WindowRecord record;
      record.hwnd = hwnd;
      record.pid = 901;
      record.canMove = false;
      workspace.AdmitTiled(record);
      WindowConstraints constraints;
      constraints.valid = true;
      constraints.dpi = metrics.dpi;
      // Often wide or tall windows, so a newcomer often doesn't fit.
      if (crowded) {
        constraints.minWidth = shareWidth * static_cast<LONG>(70 + random(31)) / 100;
        constraints.minHeight = shareHeight * static_cast<LONG>(70 + random(31)) / 100;
      } else {
        constraints.minWidth = 1 + static_cast<LONG>(random(random(2) ? 1900 : 600));
        constraints.minHeight = 1 + static_cast<LONG>(random(random(2) ? 1030 : 400));
      }
      workspace.UpdateConstraints(hwnd, constraints);
      workspace.ClearInsertionPlacement(hwnd);
    }
    std::vector<RECT> rects;
    // Only settled workspaces that fit: arrangement keeps them that way.
    if (Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects) !=
        Layout::LayoutPlanStatus::Success) {
      continue;
    }
    const auto prediction = Placement::PredictAutomaticAdmission(workspace, metrics, false, false);

    Workspace admitted = workspace;
    if (!Reconcile::AdmitUntrackedSnapshot(admitted, {incoming}, true, true)) std::exit(1);
    const bool largerMinimum = random(2) == 0;
    if (largerMinimum) {
      admitted.UpdateConstraints(incoming, {1 + static_cast<LONG>(random(1000)), 1 + static_cast<LONG>(random(600)),
                                            metrics.dpi, true});
    }
    const uint32_t preferredChoice = random(3);
    const HWND preferred = preferredChoice == 0 ? nullptr : preferredChoice == 1 ? incoming : hwnds[random(count)];
    g_workspaces.Save(key, admitted);
    Placement::ArrangementContext context{key, monitor, metrics, preferred};
    for (int pass = 0; pass < 8; ++pass) {
      if (Layout::BuildWorkspaceLayoutPlan(admitted, metrics.workArea, metrics.gap, &rects) !=
          Layout::LayoutPlanStatus::Unsatisfiable) {
        break;
      }
      if (Placement::RecoverUnsatisfiableLayout(admitted, context, rects) != Placement::ArrangementStep::Replan) break;
    }
    const WindowRecord* newcomer = admitted.Find(incoming);
    const bool floated = newcomer && newcomer->state == ManageState::Floating;
    bool othersKept = true;
    for (HWND hwnd : hwnds) othersKept &= admitted.IsTiled(hwnd);

    if (prediction.newWindowFloats) {
      ++predictedFloats;
      if (!floated || !othersKept) {
        std::fprintf(stderr, "FAILED: predicted overflow float, but floated=%d othersKept=%d (iteration %d)\n",
                     floated, othersKept, iteration);
        std::exit(1);
      }
    } else if (prediction.tiledFrame && !largerMinimum) {
      ++predictedTiles;
      if (floated) {
        std::fprintf(stderr, "FAILED: predicted a tiled slot, but the window floated (iteration %d)\n", iteration);
        std::exit(1);
      }
    } else if (floated) {
      ++unpredictedFloats;  // Allowed: a larger real minimum, or no prediction.
      missedFloats += !largerMinimum;
    }

    g_workspaces.Save(key, Workspace{});
    TestAdmissionRecoveries().clear();
    TestLeases().clear();
    g_wm.overflow.queue.clear();
    g_wm.overflow.windows.clear();
  }
  if (g_wm.timers.overflow) KillTimer(nullptr, g_wm.timers.overflow);
  if (g_wm.timers.conformance) KillTimer(nullptr, g_wm.timers.conformance);
  g_wm.timers.overflow = g_wm.timers.conformance = 0;
  TrayUi::g_preferences.showToasts = savedToasts;
  g_wm.managementMode = savedMode;
  // Fixed seed: 98 predicted floats and 3016 predicted tiles as of 2026-11-17.
  if (predictedFloats < 80 || predictedTiles < 1000) {
    std::fprintf(stderr, "FAILED: overflow prediction inputs too narrow (%zu floats, %zu tiles)\n", predictedFloats,
                 predictedTiles);
    std::exit(1);
  }
  std::printf("Passed overflow float prediction parity: %zu predicted floats, %zu predicted tiles, "
              "%zu other floats (%zu without a larger minimum).\n",
              predictedFloats, predictedTiles, unpredictedFloats, missedFloats);
}

int main() {
  RunFrameMarginCacheTests();
  RunConstraintProbeTests();
  RunComRefOwnershipTests();
  RunDesktopReconciliationTests();
  RunDiscoveryResultTests();
  RunWinEventRoutingTests();
  RunConformanceLocationTests();
  RunDiagnosticReportShapeTest();
  RunSettledWindowRecoveryTests();
  RunConstraintRecoveryDigest();
  RunOverflowFloatPredictionTests();
}
