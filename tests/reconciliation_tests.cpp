#include "../src/multiwm/multiwm.cpp"
#include <cstdio>
#include <future>
#include <thread>

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

  // Both switch paths decide "was this a switch" by one rule.
  {
    const HWND leased = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5e11));
    auto observe = [&](const GUID& desktop, bool notified) {
      g_wm.desktop.pendingSwitchFlyouts = false;
      TestLeases()[leased] = Placement::ConformanceLease{};
      return Reconcile::ObserveCurrentDesktop(desktop, notified);
    };
    auto sideEffects = [&](bool expected) {
      return g_wm.desktop.pendingSwitchFlyouts == expected && TestLeases().count(leased) == (expected ? 0u : 1u) &&
             g_wm.desktop.state == ReconciledDesktopState::Settled;
    };
    RuntimeLifecycle::ForgetReconciledDesktop();
    RuntimeLifecycle::MarkDesktopTransitionPending();
    check(g_wm.desktop.state == ReconciledDesktopState::Unknown, "nothing goes stale before a first observation");
    check(!observe(source.desktopId, false) && sideEffects(false), "a settled pass's first observation is no switch");
    RuntimeLifecycle::ForgetReconciledDesktop();
    check(observe(source.desktopId, true) && sideEffects(true), "a native notification's first observation is a switch");
    check(!observe(source.desktopId, true) && sideEffects(false), "a duplicate notification is no switch");
    RuntimeLifecycle::MarkDesktopTransitionPending();
    check(g_wm.desktop.state == ReconciledDesktopState::TransitionPending, "a possible switch makes the desktop stale");
    check(!observe(source.desktopId, false) && sideEffects(false), "a settled pass on the same desktop is no switch");
    check(observe(target.desktopId, false) && sideEffects(true) &&
              IsEqualGUID(g_wm.desktop.reconciled, target.desktopId),
          "a settled pass on another desktop is a switch");
    check(observe(source.desktopId, true) && sideEffects(true), "a notification of another desktop is a switch");
    TestLeases().clear();
    g_wm.desktop.pendingSwitchFlyouts = false;
    RuntimeLifecycle::ForgetReconciledDesktop();
  }
  std::printf("Passed %d desktop reconciliation checks.\n", checks);
}

// Test callbacks run on the WM thread; app and ghost windows need a
// different owner thread with a message pump.
class WinEventTestThread {
 public:
  explicit WinEventTestThread(std::function<void()> createWindows) {
    std::promise<DWORD> ready;
    auto threadId = ready.get_future();
    thread_ = std::thread([create = std::move(createWindows), ready = std::move(ready)]() mutable {
      MSG message{};
      PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
      create();
      ready.set_value(GetCurrentThreadId());
      while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    });
    threadId_ = threadId.get();
  }

  ~WinEventTestThread() {
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
  }

 private:
  DWORD threadId_ = 0;
  std::thread thread_;
};

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

  const DWORD previousThread = g_wm.threadId.exchange(GetCurrentThreadId(), std::memory_order_acq_rel);
  drain();
  const HWND ownWindow = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"STATIC", L"",
      WS_POPUP, 0, 0, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(ownWindow != nullptr, "create an own drag-preview window");
  send(EVENT_OBJECT_SHOW, ownWindow);
  send(EVENT_OBJECT_LOCATIONCHANGE, ownWindow);
  send(EVENT_SYSTEM_FOREGROUND, ownWindow);
  check(g_wm.lifecycle.dirtyWindows.empty() && !g_wm.timers.lifecycle && drain().empty(),
        "own preview events queue no lifecycle pass or foreground message");
  DestroyWindow(ownWindow);

  HWND tracked = nullptr, untracked = nullptr, child = nullptr;
  WinEventTestThread appThread([&] {
    tracked = makeWindow(WS_OVERLAPPEDWINDOW, nullptr);
    untracked = makeWindow(WS_OVERLAPPEDWINDOW, nullptr);
    child = makeWindow(WS_CHILD, untracked);
  });
  check(tracked && untracked && child, "create WinEvent test windows on another thread");

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
  drain();
  g_wm.routing.evaluated.clear();
  g_wm.routing.pending.clear();
  g_wm.routing.ready = false;
  g_wm.desktop.state = ReconciledDesktopState::Unknown;
  g_wm.threadId.store(previousThread, std::memory_order_release);
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

// Counts passed checks; the first failure ends the test run.
struct CheckCounter {
  unsigned passed = 0;
  void operator()(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++passed;
  }
};

// Real windows that are invisible to the user (transparent, click-through,
// never activated, tool windows that a live MultiWM ignores) on the primary
// monitor, owned by one fake virtual desktop. Floating placement tests use it;
// everything it changes is put back when it goes out of scope.
class FloatingWindowFixture {
 public:
  explicit FloatingWindowFixture(CheckCounter& check)
      : check_(check),
        previousDpi_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)),
        savedSettings_(g_settings) {
    g_settings.conformanceLeaseMs = 3000;
    g_settings.conformanceRepairIntervalMs = 75;
    g_settings.resizeStepPercent = 5;
    GUID desktop{};
    desktop.Data1 = 0xF10A7;
    windows_.desktop = desktop;
    manager_.desktop.id = desktop;
    g_vd.initialized = true;
    g_vd.desktopManager = &windows_;
    g_vd.managerInternal = &manager_;
    g_wm.desktop.reconciled = desktop;
    g_wm.desktop.state = ReconciledDesktopState::Settled;
    g_wm.managementMode = ManagementMode::Manual;
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    check_(DesktopMonitorKey::FromHMonitor(desktop, monitor, &key) &&
               Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics),
           "resolve the test monitor");
    workArea = metrics.workArea;
    center = SharedGeometry::RectCenter(workArea);
    hwnd = CreateTestWindow(nullptr);
  }

  ~FloatingWindowFixture() {
    // Newest first: destroying an owner also destroys the windows it owns.
    for (auto it = created_.rbegin(); it != created_.rend(); ++it) {
      Reconcile::ForgetManagedWindow(*it);
      DestroyWindow(*it);
    }
    ClearLeases();
    g_settings = savedSettings_;
    g_wm.managementMode = ManagementMode::Manual;
    g_wm.desktop.state = ReconciledDesktopState::Unknown;
    g_vd.desktopManager = nullptr;
    g_vd.managerInternal = nullptr;
    g_vd.initialized = false;
    SetThreadDpiAwarenessContext(previousDpi_);
  }

  // A 400x300 test window off-center in the work area, optionally owned.
  HWND CreateTestWindow(HWND owner) {
    const HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, L"STATIC",
        L"MultiWM floating test", WS_POPUP, workArea.left + 20, workArea.top + 20, 400, 300, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    check_(window != nullptr, "create an invisible test window");
    SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    created_.push_back(window);
    return window;
  }

  // A workspace in `layout` whose only member is `hwnd`, replacing any other.
  void Seed(TileLayout layout) {
    Workspace workspace;
    workspace.SetLayout(layout);
    workspace.AdmitTiled(Record(hwnd));
    workspace.ClearInsertionPlacement(hwnd);
    g_workspaces.Save(key, workspace);
  }

  static WindowRecord Record(HWND window) {
    WindowRecord record;
    record.hwnd = window;
    record.pid = GetCurrentProcessId();
    record.canMove = record.canResize = true;
    return record;
  }

  Workspace Load() {
    Workspace workspace;
    check_(g_workspaces.Load(key, &workspace), "load the test workspace");
    return workspace;
  }

  static RECT Frame(HWND window) {
    RECT rect{};
    Platform::Windows::GetWindowFrameRect(window, &rect);
    return rect;
  }

  // The app moves or resizes `window` itself.
  static void AppMove(HWND window, LONG left, LONG top, LONG width, LONG height) {
    SetWindowPos(window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // MultiWM places `window` with `intent` and saves the workspace.
  bool Place(HWND window, Placement::FloatingPlacementIntent intent) {
    Workspace workspace = Load();
    Placement::FloatingPlacementHint hint;
    hint.intent = intent;
    if (!Placement::RepairFloatingGeometry(key, workspace, window, hint)) return false;
    g_workspaces.Save(key, workspace);
    return true;
  }

  // One window-size step on a floating window; saves the workspace.
  bool Step(HWND window, int direction) {
    Workspace workspace = Load();
    if (!Placement::StepFloatingWindowSize(key, workspace, window, direction)) return false;
    g_workspaces.Save(key, workspace);
    return true;
  }

  // Where centering a window of `widthDip` x `heightDip` puts it.
  RECT Centered(LONG widthDip, LONG heightDip) const {
    return SharedGeometry::ClampToArea(
        SharedGeometry::CenteredIn(workArea, SharedGeometry::ScaleDip(widthDip, metrics.dpi),
                                   SharedGeometry::ScaleDip(heightDip, metrics.dpi)),
        workArea);
  }

  static Placement::ConformanceLease* Lease(HWND window) {
    auto it = TestLeases().find(window);
    return it == TestLeases().end() ? nullptr : &it->second;
  }

  static void ClearLeases() {
    TestLeases().clear();
    if (g_wm.timers.conformance) KillTimer(nullptr, g_wm.timers.conformance);
    g_wm.timers.conformance = 0;
  }

  // The desktop Windows reports for every test window (GUID_NULL: none yet).
  void SetWindowDesktop(const GUID& desktop) { windows_.desktop = desktop; }

  DesktopMonitorKey key;
  Platform::Monitors::WorkspaceMetrics metrics;
  RECT workArea{};
  POINT center{};
  HWND hwnd = nullptr;  // The first test window.

 private:
  CheckCounter& check_;
  DPI_AWARENESS_CONTEXT previousDpi_;
  SettingsState savedSettings_;
  TestWindowDesktops windows_;
  TestManager manager_;
  std::vector<HWND> created_;
};

// The overflow planner judges a destination by each member's cached size
// limits and probes only the windows it has no usable report for: none cached,
// cached at another DPI, or still settling into its slot. Real invisible test
// windows on a fake desktop, planned through the new-desktop path so no other
// window in the session is enumerated or probed. Nothing is placed or saved.
static void RunOverflowDestinationProbeTests() {
  CheckCounter check;
  FloatingWindowFixture fixture(check);
  const HWND cachedMember = fixture.hwnd;
  const HWND unknownMember = fixture.CreateTestWindow(nullptr);
  const HWND incomingWindow = fixture.CreateTestWindow(nullptr);
  const UINT dpi = fixture.metrics.dpi;
  auto record = [](HWND window, LONG minWidth, UINT constraintDpi, bool valid) {
    WindowRecord result = FloatingWindowFixture::Record(window);
    result.constraints.valid = valid;
    result.constraints.minWidth = minWidth;
    result.constraints.minHeight = 1;
    result.constraints.dpi = constraintDpi;
    return result;
  };
  const WindowRecord incoming = record(incomingWindow, 1, dpi, true);
  // Plans `incoming` into a Columns workspace of two members: cachedMember,
  // whose cached report says `cachedMinimum` wide at `cachedDpi` (the window
  // itself accepts any reasonable width), and unknownMember with no report.
  auto plan = [&](LONG cachedMinimum, UINT cachedDpi, bool settled, uint64_t* probes) {
    Workspace initial;
    initial.SetLayout(TileLayout::Columns);
    initial.AdmitTiled(record(cachedMember, cachedMinimum, cachedDpi, true));
    if (settled) initial.ClearInsertionPlacement(cachedMember);
    initial.AdmitTiled(record(unknownMember, 1, dpi, false));
    initial.ClearInsertionPlacement(unknownMember);
    const uint64_t before = Diagnostics::g_runtime.counters.constraintProbeAttempts;
    const auto destination = Reconcile::PlanOverflowDestination(
        fixture.key, incoming, FloatingWindowFixture::Frame(incomingWindow), &initial);
    *probes = Diagnostics::g_runtime.counters.constraintProbeAttempts - before;
    return destination.has_value();
  };

  uint64_t probes = 0;
  check(plan(1, dpi, true, &probes) && probes == 1,
        "a settled member's cached limits are used; only the member without a report is probed");
  check(plan(1, dpi, false, &probes) && probes == 2, "a member still settling into its slot is probed");
  check(plan(1, dpi * 2, true, &probes) && probes == 2, "a report cached at another DPI is probed");
  // A cached minimum as wide as the work area leaves no room for two more columns.
  const LONG workAreaWide = Layout::RectWidth(fixture.workArea);
  check(!plan(workAreaWide, dpi, true, &probes) && probes == 1,
        "the destination is judged by the cached minimum, not a fresh probe");
  check(plan(workAreaWide, dpi * 2, true, &probes) && probes == 2,
        "a fresh probe replaces a minimum cached at another DPI");
  std::printf("Passed %u overflow destination probe checks.\n", check.passed);
}

// Overflow and arrangement agree on which Dwindle destinations fit, including
// when the incoming window needs another insertion point. Seeded minimum sizes
// exercise the new-desktop path with real invisible windows.
static void RunOverflowDestinationFitTests() {
  CheckCounter check;
  FloatingWindowFixture fixture(check);
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;
  std::vector<HWND> windows{fixture.hwnd};
  for (int i = 0; i < 5; ++i) windows.push_back(fixture.CreateTestWindow(nullptr));
  const HWND incomingWindow = windows.back();
  const RECT& area = fixture.workArea;
  const LONG gap = fixture.metrics.gap;
  const LONG width = Layout::RectWidth(area);
  const LONG height = Layout::RectHeight(area);
  uint32_t seed = 0x5eed1234u;
  auto next = [&](uint32_t bound) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % bound;
  };
  // A minimum span: none, or a share of the work area from 15% to 70%.
  auto span = [&](LONG whole) { return next(3) == 0 ? 1L : whole * static_cast<LONG>(15 + next(56)) / 100; };
  auto record = [&](HWND window, SIZE minimum) {
    WindowRecord result = FloatingWindowFixture::Record(window);
    result.constraints.valid = true;
    result.constraints.dpi = fixture.metrics.dpi;
    result.constraints.minWidth = minimum.cx;
    result.constraints.minHeight = minimum.cy;
    return result;
  };

  size_t repositioned = 0;
  size_t compared = 0;
  for (int attempt = 0; attempt < 20000 && (compared < 300 || repositioned < 20); ++attempt) {
    Workspace initial;
    initial.SetLayout(TileLayout::Dwindle);
    const size_t memberCount = 2 + next(4);
    for (size_t i = 0; i < memberCount; ++i) {
      initial.AdmitTiled(record(windows[i], {span(width), span(height)}));
      initial.ClearInsertionPlacement(windows[i]);
    }
    const WindowRecord incoming = record(incomingWindow, {span(width), span(height)});

    // What arrangement does with the saved destination: plan it, else recover.
    Workspace expected = initial;
    expected.AdmitTiled(incoming);
    std::vector<RECT> rects;
    const bool fits = Layout::BuildWorkspaceLayoutPlan(expected, area, gap, &rects) == Layout::LayoutPlanStatus::Success;
    Layout::AdmissionRecoveryPlan recovery;
    const bool recovered = !fits && Layout::TryRecoverPendingAdmission(
        expected, area, gap, Placement::OrderRemovalCandidates(expected, nullptr), nullptr, &recovery);
    if (!recovered && compared >= 300) continue;
    ++compared;
    repositioned += recovered ? 1 : 0;
    const auto destination = Reconcile::PlanOverflowDestination(
        fixture.key, incoming, FloatingWindowFixture::Frame(incomingWindow), &initial);
    check(destination.has_value() == (fits || recovered),
          "overflow accepts a destination exactly when arrangement would tile the window there");
  }
  check(repositioned > 0, "some destinations fit only at an alternate insertion point");
  std::printf("Passed %u overflow destination fit checks (%zu destinations, %zu needing repositioning).\n",
              check.passed, compared, repositioned);
}

static bool IsCenteredIn(const RECT& rect, POINT center) {
  const POINT actual = SharedGeometry::RectCenter(rect);
  return std::abs(actual.x - center.x) <= 1 && std::abs(actual.y - center.y) <= 1;
}

// Floating leases: in the Floating layout, a window MultiWM placed is put back
// if the app snaps away, and keeps the app's frame at the deadline.
static void RunFloatingLeaseTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  auto lease = [&] { return FloatingWindowFixture::Lease(hwnd); };
  auto remembered = [&] {
    const WindowRecord* record = f.Load().Find(hwnd);
    return record && record->hasFloatingRect ? record->floatingRect : RECT{};
  };
  auto appShift = [&](LONG dx) {
    const RECT r = FloatingWindowFixture::Frame(hwnd);
    FloatingWindowFixture::AppMove(hwnd, r.left + dx, r.top, Layout::RectWidth(r), Layout::RectHeight(r));
  };

  f.Seed(TileLayout::Floating);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::NewWindowCenter), "place the window");
  const RECT placed = FloatingWindowFixture::Frame(hwnd);
  check(lease() && lease()->kind == Placement::ConformanceLeaseKind::Floating &&
            Layout::RectsNear(lease()->expectedRect, placed) && Layout::RectsNear(remembered(), placed),
        "a Floating-layout placement opens a floating lease at its frame");
  Placement::ArrangeWorkspace(f.key);
  check(lease() != nullptr, "arranging the Floating layout keeps its floating leases");

  appShift(41);
  check(Placement::HandleTiledWindowLocationChange(hwnd) &&
            Layout::RectsNear(FloatingWindowFixture::Frame(hwnd), placed) && lease() && lease()->attempts == 1,
        "a snap-back inside the lease is put back, and the attempt counted");
  appShift(41);
  check(Placement::HandleTiledWindowLocationChange(hwnd) && lease() && lease()->repairDueTickMs,
        "a second snap-back right away is deferred, not dropped");

  const uint64_t adoptedBefore = Diagnostics::g_runtime.counters.conformanceLeaseExpiredAdopted;
  const RECT appFrame = FloatingWindowFixture::Frame(hwnd);
  lease()->expiresAtTickMs = 1;
  Placement::ProcessConformanceTimer();
  check(!lease() && Layout::RectsNear(FloatingWindowFixture::Frame(hwnd), appFrame) &&
            Layout::RectsNear(remembered(), appFrame) &&
            Diagnostics::g_runtime.counters.conformanceLeaseExpiredAdopted == adoptedBefore + 1,
        "at the deadline the window keeps the app's frame, which is remembered");
  check(f.Load().Layout() == TileLayout::Floating && f.Load().IsTiled(hwnd),
        "a floating deadline never floats the window or changes the layout");

  check(f.Step(hwnd, 1) && lease() && Layout::RectsNear(lease()->expectedRect, FloatingWindowFixture::Frame(hwnd)),
        "a size step opens a floating lease too");
  f.Seed(TileLayout::Columns);
  check(!Placement::HandleTiledWindowLocationChange(hwnd) && !lease(),
        "outside the Floating layout a floating lease is dropped");
  std::printf("Passed %u floating lease checks.\n", check.passed);
}

// Window-size commands on floating windows: Floating-layout members and
// per-window floating windows step around their center and reset as if newly
// admitted; a per-window floating window is its own target, never its owner.
static void RunFloatingSizeCommandTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  auto frame = [](HWND window) { return FloatingWindowFixture::Frame(window); };
  const LONG stepX = MulDiv(Layout::RectWidth(f.workArea), 5, 100);
  const LONG stepY = MulDiv(Layout::RectHeight(f.workArea), 5, 100);

  f.Seed(TileLayout::Floating);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::NewWindowCenter) &&
            Layout::RectsNear(frame(hwnd), f.Centered(960, 640), 1),
        "reset centers the window at the default floating size");
  const RECT start = frame(hwnd);
  check(f.Step(hwnd, 1) && Layout::RectWidth(frame(hwnd)) == Layout::RectWidth(start) + stepX &&
            Layout::RectHeight(frame(hwnd)) == Layout::RectHeight(start) + stepY,
        "grow adds the step percentage of the work area to each dimension");
  check(f.Step(hwnd, -1) && Layout::RectsNear(frame(hwnd), start, 0), "shrink undoes grow");
  {
    // Limits cached on another monitor's DPI that would forbid any shrink here.
    Workspace workspace = f.Load();
    WindowConstraints otherDpi;
    otherDpi.valid = true;
    otherDpi.dpi = f.metrics.dpi * 2;
    otherDpi.minWidth = otherDpi.minHeight = 4000;
    workspace.UpdateConstraints(hwnd, otherDpi);
    g_workspaces.Save(f.key, workspace);
  }
  check(f.Step(hwnd, -1) && Layout::RectWidth(frame(hwnd)) == Layout::RectWidth(start) - stepX,
        "shrink uses the window's limits at this DPI, not ones cached at another");
  check(f.Load().Find(hwnd)->constraints.dpi == f.metrics.dpi, "the limits at this DPI are kept");
  check(f.Step(hwnd, 1) && Layout::RectsNear(frame(hwnd), start, 0), "grow undoes shrink");
  for (int i = 0; i < 100 && f.Step(hwnd, 1); ++i) {}
  check(!f.Step(hwnd, 1) && Layout::RectWidth(frame(hwnd)) <= Layout::RectWidth(f.workArea) &&
            Layout::RectHeight(frame(hwnd)) <= Layout::RectHeight(f.workArea),
        "growth stops at the work area");
  FloatingWindowFixture::ClearLeases();

  // A per-window floating window owned by a tiled one.
  f.Seed(TileLayout::Columns);
  const HWND owned = f.CreateTestWindow(hwnd);
  check(Commands::ResolveSizeCommandTarget(f.Load(), owned) == hwnd,
        "an unmanaged owned window resolves to its tiled owner");
  Workspace workspace = f.Load();
  check(workspace.AdmitFloating(FloatingWindowFixture::Record(owned)), "float the owned window");
  g_workspaces.Save(f.key, workspace);
  check(Commands::ResolveSizeCommandTarget(f.Load(), owned) == owned,
        "a per-window floating window is its own target");
  const RECT ownedStart = frame(owned);
  check(f.Step(owned, 1) && Layout::RectWidth(frame(owned)) == Layout::RectWidth(ownedStart) + stepX &&
            !FloatingWindowFixture::Lease(owned),
        "it grows by one step, without a floating lease outside the Floating layout");
  FloatingWindowFixture::AppMove(owned, f.workArea.left + 20, f.workArea.top + 20, 400, 300);
  check(f.Place(owned, Placement::FloatingPlacementIntent::NewWindowCenter) &&
            Layout::RectsNear(frame(owned), f.Centered(960, 640), 1),
        "without a size rule, reset centers it at the default floating size");

  WindowRule sizeRule;
  sizeRule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
  sizeRule.classNames = {L"Static"};
  sizeRule.preserveFloatingSize = false;
  sizeRule.floatingSizeDip = {489, 384};
  g_settings.windowRules = {sizeRule};
  g_wm.managementMode = ManagementMode::Automatic;
  check(f.Place(owned, Placement::FloatingPlacementIntent::NewWindowCenter) &&
            Layout::RectsNear(frame(owned), f.Centered(489, 384), 1),
        "with an override-size rule, reset centers it at the rule's size");
  std::printf("Passed %u floating size command checks.\n", check.passed);
}

// A window's first floating size never comes from the frame it had when it was
// first recorded (often a placeholder, or with the companion already its tile):
// it is an override-size rule's size or the configured default. Floating frames
// then take the window's size limits around their centre, because MultiWM's
// SWP_NOSENDCHANGING placement skips Windows' own clamp.
static void RunFirstFloatingSizeTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  auto frame = [&] { return FloatingWindowFixture::Frame(hwnd); };
  const LONG defaultWidthDip = g_settings.floatingDefaultSizeDip.width;
  const LONG defaultHeightDip = g_settings.floatingDefaultSizeDip.height;
  const RECT defaultFrame = f.Centered(defaultWidthDip, defaultHeightDip);

  WindowRecord record = Platform::Windows::MakeWindowRecord(hwnd);
  check(!record.hasFloatingRect, "a newly recorded window has no floating frame");
  record.canResize = true;  // The invisible test window has no sizing border.
  Workspace workspace;
  workspace.SetLayout(TileLayout::Columns);
  workspace.AdmitTiled(record);
  check(workspace.Float(hwnd), "float the tiled window");
  g_workspaces.Save(f.key, workspace);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::CenterWithFloatingSize) &&
            Layout::RectsNear(frame(), defaultFrame, 1),
        "Float Focused floats a never-floated window at the default size, centered");
  const WindowRecord* placed = f.Load().Find(hwnd);
  check(placed && placed->constraints.valid && placed->constraints.dpi == f.metrics.dpi,
        "the window's size limits are probed when none are cached at the target DPI");

  // Cached limits, as the window reported them at this DPI.
  auto withLimits = [&](LONG minWidth, LONG minHeight, LONG maxWidth, LONG maxHeight) {
    Workspace current = f.Load();
    WindowConstraints limits;
    limits.valid = true;
    limits.dpi = f.metrics.dpi;
    limits.minWidth = minWidth;
    limits.minHeight = minHeight;
    limits.maxWidth = maxWidth;
    limits.maxHeight = maxHeight;
    current.UpdateConstraints(hwnd, limits);
    g_workspaces.Save(f.key, current);
  };
  auto centeredAt = [&](LONG width, LONG height) {
    return SharedGeometry::CenteredClampedIn(f.metrics.workArea, width, height);
  };
  withLimits(1, 1, 300, 200);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::NewWindowCenter) &&
            Layout::RectsNear(frame(), centeredAt(300, 200), 1) && Layout::RectWidth(frame()) == 300 &&
            Layout::RectHeight(frame()) == 200,
        "a maximum shrinks the default frame around its centre");
  const LONG wider = Layout::RectWidth(defaultFrame) + 40;
  const LONG taller = Layout::RectHeight(defaultFrame) + 30;
  withLimits(wider, taller, 0, 0);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::NewWindowCenter) &&
            Layout::RectsNear(frame(), centeredAt(wider, taller), 1) && Layout::RectWidth(frame()) == wider &&
            Layout::RectHeight(frame()) == taller,
        "a minimum grows the default frame around its centre");
  withLimits(1, 1, 0, 0);

  // An override-size rule names the first floating size instead (Automatic mode).
  WindowRule sizeRule;
  sizeRule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
  sizeRule.classNames = {L"Static"};
  sizeRule.preserveFloatingSize = false;
  sizeRule.floatingSizeDip = {489, 384};
  g_settings.windowRules = {sizeRule};
  g_wm.managementMode = ManagementMode::Automatic;
  // A fresh record: the placements above remembered a floating frame.
  Workspace fresh;
  fresh.SetLayout(TileLayout::Columns);
  fresh.AdmitTiled(FloatingWindowFixture::Record(hwnd));
  check(fresh.Float(hwnd), "float a fresh record of the window");
  g_workspaces.Save(f.key, fresh);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::CenterWithFloatingSize) &&
            Layout::RectsNear(frame(), f.Centered(489, 384), 1),
        "with an override-size rule, a never-floated window floats at the rule's size");

  // A failed tile (FailureCenter): a new window floats at the rule's or the
  // default size, centred; a window with a remembered frame goes back to it.
  auto floatFresh = [&] {
    Workspace again;
    again.SetLayout(TileLayout::Columns);
    again.AdmitTiled(FloatingWindowFixture::Record(hwnd));
    again.Float(hwnd);
    g_workspaces.Save(f.key, again);
  };
  floatFresh();
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 30, f.workArea.top + 40, 700, 500);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::FailureCenter) &&
            Layout::RectsNear(frame(), f.Centered(489, 384), 1),
        "a new window that can't be tiled floats at the rule's size, centred");
  g_settings.windowRules.clear();
  floatFresh();
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::FailureCenter) &&
            Layout::RectsNear(frame(), defaultFrame, 1),
        "without a rule, at the default size");
  // The user resizes the floating window (a drag end remembers the frame), then
  // a failed tile floats it again from wherever its tile put it.
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 30, f.workArea.top + 40, 700, 500);
  const RECT userFrame = frame();
  {
    Workspace current = f.Load();
    check(Placement::RememberCurrentFloatingGeometry(f.key, current, hwnd), "remember the user's frame");
    g_workspaces.Save(f.key, current);
  }
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left, f.workArea.top, 300, 200);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::FailureCenter) &&
            Layout::RectsNear(frame(), userFrame, 1),
        "a window with a remembered frame that can't be tiled goes back to it");

  // Tile Workspace remembers a floating window's current frame, however it got
  // it, before re-tiling it.
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 60, f.workArea.top + 50, 640, 420);
  const RECT appFrame = frame();
  {
    Workspace current = f.Load();
    check(Reconcile::ReadoptFloatingSnapshot(current, {hwnd}, f.metrics.dpi) && current.IsTiled(hwnd),
          "Tile Workspace re-tiles the floating window");
    const WindowRecord* readopted = current.Find(hwnd);
    check(readopted && readopted->hasFloatingRect && EqualRect(&readopted->floatingRect, &appFrame) &&
              readopted->floatingDpi == f.metrics.dpi,
          "Tile Workspace remembers the frame the app gave the window, not only a dragged one");
  }

  // Leaving the Floating layout remembers each member's frame the same way.
  f.Seed(TileLayout::Floating);
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 80, f.workArea.top + 70, 520, 360);
  const RECT floatingLayoutFrame = frame();
  Commands::SetCurrentWorkspaceLayout(TileLayout::Columns, f.key.ResolveMonitor());
  {
    const Workspace left = f.Load();
    const WindowRecord* member = left.Find(hwnd);
    check(left.Layout() == TileLayout::Columns && member && member->hasFloatingRect &&
              EqualRect(&member->floatingRect, &floatingLayoutFrame),
          "leaving the Floating layout remembers where the app had put the window");
  }
  FloatingWindowFixture::ClearLeases();
  std::printf("Passed %u first floating size checks.\n", check.passed);
}

// Tile Workspace learns Master + Stack and grid geometry from where the windows
// are, but never rebuilds a BSP tree (or Dwindle's ratios) from it, and adopting
// keeps what the other layouts remember. Real invisible windows, really tiled.
static void RunTileWorkspaceGeometryTests() {
  CheckCounter check;
  FloatingWindowFixture fixture(check);
  const HWND second = fixture.CreateTestWindow(nullptr);
  const HWND third = fixture.CreateTestWindow(nullptr);
  g_settings.adaptiveDwindleSlots = 0;
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;
  const HMONITOR monitor = fixture.key.ResolveMonitor();
  auto adopt = [&](Workspace& workspace) {
    return Reconcile::AdoptSnapshotGeometryIfEligible(
        workspace, workspace.TiledWindows(), fixture.workArea, fixture.metrics.gap, monitor);
  };

  fixture.Seed(TileLayout::Bsp);
  Workspace workspace = fixture.Load();
  for (HWND window : {second, third}) {
    check(workspace.AdmitTiled(FloatingWindowFixture::Record(window)), "admit a Tile Workspace test window");
    workspace.ClearInsertionPlacement(window);
  }
  // A split the user turned, which a rebuilt tree would put back.
  const size_t turned = workspace.Bsp().Parent(workspace.Bsp().FindLeaf(third));
  const Layout::Axis axis =
      Layout::DefaultBspSplitAxis(workspace.Bsp(), turned, fixture.workArea) == Layout::Axis::X ? Layout::Axis::Y
                                                                                              : Layout::Axis::X;
  workspace.SetBspSplitDirection(turned, axis);
  g_workspaces.Save(fixture.key, workspace);
  Placement::ArrangeWorkspace(fixture.key);
  workspace = fixture.Load();
  const BspTree arranged = workspace.Bsp();
  check(!adopt(workspace) && workspace.Bsp() == arranged, "Tile Workspace keeps a BSP tree as it is");
  Workspace dwindle = workspace;
  dwindle.SetLayout(TileLayout::Dwindle);
  const Workspace dwindleBefore = dwindle;
  check(!adopt(dwindle) && dwindle.HasSameLayoutStateAs(dwindleBefore), "Tile Workspace keeps Dwindle's ratios");

  // Adopting Master + Stack geometry builds a new workspace; it keeps what the
  // other layouts remember, so switching back finds the turned tree. Frames
  // are given rather than read back, so only the merge is under test.
  Workspace master = workspace;
  master.SetLayout(TileLayout::MasterStack);
  std::vector<RECT> frames;
  check(Layout::BuildWorkspaceLayoutPlan(master, fixture.workArea, fixture.metrics.gap, &frames) ==
            Layout::LayoutPlanStatus::Success,
        "plan the Master + Stack frames");
  std::vector<WorkspaceGeometryItem> items;
  for (size_t i = 0; i < master.TiledWindows().size(); ++i) {
    items.push_back({*master.Find(master.TiledWindows()[i]), frames[i]});
  }
  Workspace adopted = Workspace::AdoptGeometry(TileLayout::MasterStack, fixture.workArea, fixture.metrics.gap, 0.5,
                                               items, master.TiledWindows().front());
  adopted.MergeRememberedLayoutGeometryFrom(master);
  check(adopted.TiledWindows() == master.TiledWindows(), "adopting keeps the Master + Stack order");
  adopted.SetLayout(TileLayout::Bsp);
  check(adopted.Bsp() == arranged, "adopting keeps the tree BSP remembers");
  std::printf("Passed %u Tile Workspace geometry checks.\n", check.passed);
}

// An arrangement while the user drags a tiled window (another window opened, a
// size-limit retry, a lease deadline) leaves the dragged window where it is.
static void RunDraggedWindowPlacementTests() {
  CheckCounter check;
  FloatingWindowFixture fixture(check);
  const HWND other = fixture.CreateTestWindow(nullptr);
  fixture.Seed(TileLayout::MasterStack);
  Workspace workspace = fixture.Load();
  check(workspace.AdmitTiled(FloatingWindowFixture::Record(other)), "admit a second tiled window");
  workspace.ClearInsertionPlacement(other);
  g_workspaces.Save(fixture.key, workspace);
  Placement::ArrangeWorkspace(fixture.key);

  auto windowRect = [](HWND window) {
    RECT rect{};
    GetWindowRect(window, &rect);
    return rect;
  };
  // Mid-drag: the gesture has started and the window is under the cursor.
  Gestures::g_moveSize.startRects[fixture.hwnd] = windowRect(fixture.hwnd);
  FloatingWindowFixture::AppMove(fixture.hwnd, fixture.workArea.left + 300, fixture.workArea.top + 200, 500, 400);
  const RECT dragged = windowRect(fixture.hwnd);
  Placement::ArrangeWorkspace(fixture.key);
  const RECT duringDrag = windowRect(fixture.hwnd);
  check(EqualRect(&dragged, &duringDrag), "arranging leaves a window being dragged in place");
  check(fixture.Load().IsTiled(fixture.hwnd), "the dragged window keeps its slot");

  Gestures::ClearMoveSizeSamples(fixture.hwnd);
  Placement::ArrangeWorkspace(fixture.key);
  const RECT afterDrag = windowRect(fixture.hwnd);
  check(!EqualRect(&dragged, &afterDrag), "after the gesture the window returns to its tile");

  // A window that stops being tracked mid-drag takes its drop preview with it:
  // its end event may never reach the preview (Trace to owner).
  Gestures::g_moveSize.startRects[fixture.hwnd] = afterDrag;
  DragPreview::g_drag.emplace(DragPreview::DragSession{
      fixture.hwnd, fixture.hwnd, fixture.key.ResolveMonitor(), fixture.key, afterDrag, {}, {}, {}, {}});
  Reconcile::ForgetManagedWindow(other);
  check(DragPreview::g_drag.has_value(), "another window leaving keeps the drop preview");
  Reconcile::ForgetManagedWindow(fixture.hwnd);
  check(!DragPreview::g_drag, "the dragged window leaving ends its drop preview");
  Overlay::Shutdown();
  FloatingWindowFixture::ClearLeases();
  std::printf("Passed %u dragged window placement checks.\n", check.passed);
}

// Exercise completed BSP drags against actual displayed tiles, including
// the constraint preflight that restores the old layout after a rejected drop.
static void RunBspDragTests() {
  CheckCounter check;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  check(SUCCEEDED(com), "initialize COM for the retained preview tests");
  FloatingWindowFixture fixture(check);
  const HWND source = fixture.hwnd;
  const HWND target = fixture.CreateTestWindow(nullptr);
  const HWND sibling = fixture.CreateTestWindow(nullptr);
  const bool savedToasts = TrayUi::g_preferences.showToasts;
  TrayUi::g_preferences.showToasts = false;
  g_settings.mouseMoveBehavior = MouseMoveBehavior::Swap;
  g_settings.bspSplitEdgePercent = SettingsState{}.bspSplitEdgePercent;
  g_settings.adaptiveDwindleSlots = 0;
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;

  fixture.Seed(TileLayout::Bsp);
  Workspace initial = fixture.Load();
  for (HWND window : {target, sibling}) {
    check(initial.AdmitTiled(FloatingWindowFixture::Record(window)), "admit a BSP drag test window");
    initial.ClearInsertionPlacement(window);
  }
  g_workspaces.Save(fixture.key, initial);
  Placement::ArrangeWorkspace(fixture.key);
  initial = fixture.Load();
  const RECT originalSourceFrame = FloatingWindowFixture::Frame(source);
  const bool landscape = Layout::FirstSplitAxis(fixture.workArea) == Layout::Axis::X;
  using Zone = Commands::TileDropZone;
  const Zone splitEdge = landscape ? Zone::Bottom : Zone::Right;
  const Zone fittingEdge = landscape ? Zone::Right : Zone::Bottom;
  auto dropOnto = [&](HWND dragged, HWND destination, Zone zone = Zone::Center) {
    Workspace workspace = fixture.Load();
    const RECT frame = FloatingWindowFixture::Frame(destination);
    MoveSizeGesture gesture;
    gesture.hasDropPoint = true;
    gesture.dropPoint = SharedGeometry::RectCenter(frame);
    if (zone == Zone::Left) gesture.dropPoint.x = frame.left;
    if (zone == Zone::Right) gesture.dropPoint.x = frame.right - 1;
    if (zone == Zone::Top) gesture.dropPoint.y = frame.top;
    if (zone == Zone::Bottom) gesture.dropPoint.y = frame.bottom - 1;
    Commands::HandleUserMove(fixture.key, workspace, dragged, gesture, fixture.workArea);
    return fixture.Load();
  };

  const RECT tile{0, 0, 200, 100};
  check(Commands::PickTileDropZone(tile, {100, 50}, 25) == Zone::Center &&
            Commands::PickTileDropZone(tile, {10, 50}, 25) == Zone::Left &&
            Commands::PickTileDropZone(tile, {190, 50}, 25) == Zone::Right &&
            Commands::PickTileDropZone(tile, {100, 5}, 25) == Zone::Top &&
            Commands::PickTileDropZone(tile, {100, 95}, 25) == Zone::Bottom,
        "the BSP drag zone selects the nearest edge or the center");
  check(Commands::PickTileDropZone(tile, {10, 50}, 0) == Zone::Center &&
            Commands::PickTileDropZone(tile, {100, 50}, 50) != Zone::Center,
        "the edge-size slider can disable or fill the split zones");
  const Platform::Monitors::WorkspaceMetrics portrait{{0, 0, 600, 1000}, 96, 0};
  for (const Zone accepted : {Zone::Top, Zone::Bottom}) {
    Workspace vertical;
    vertical.SetLayout(TileLayout::Bsp);
    if (accepted == Zone::Top) {
      vertical.AdmitTiled(FloatingWindowFixture::Record(target));
      vertical.AdmitTiledAfter(FloatingWindowFixture::Record(source), target);
      vertical.AdmitTiledAfter(FloatingWindowFixture::Record(sibling), target);
    } else {
      vertical.AdmitTiled(FloatingWindowFixture::Record(source));
      vertical.AdmitTiledAfter(FloatingWindowFixture::Record(sibling), source);
      vertical.AdmitTiledAfter(FloatingWindowFixture::Record(target), sibling);
    }
    vertical.SetBspSplitDirection(vertical.Bsp().Parent(vertical.Bsp().FindLeaf(target)), Layout::Axis::Y);
    const Zone rejected = accepted == Zone::Top ? Zone::Bottom : Zone::Top;
    check(Commands::PlanTiledDropPreview(vertical, source, {target, accepted}, portrait) &&
              !Commands::PlanTiledDropPreview(vertical, source, {target, rejected}, portrait),
          "only the vertical side that overlaps the requested edge can split");
  }

  // Replay the LocalSend and Bitwarden diagnostic in physical pixels.
  Workspace adaptiveDrop;
  adaptiveDrop.SetLayout(TileLayout::Bsp);
  adaptiveDrop.AdmitTiled(FloatingWindowFixture::Record(sibling));
  adaptiveDrop.AdmitTiledAfter(FloatingWindowFixture::Record(target), sibling);
  adaptiveDrop.AdmitTiledAfter(FloatingWindowFixture::Record(source), sibling);
  adaptiveDrop.UpdateConstraints(sibling, {802, 542, 192, true});
  adaptiveDrop.UpdateConstraints(source, {778, 989, 192, true});
  adaptiveDrop.UpdateConstraints(target, {1178, 1190, 192, true});
  const Platform::Monitors::WorkspaceMetrics reportedMetrics{{14, 14, 3058, 1804}, 192, 14};
  g_settings.adaptiveDwindleSlots = 2;
  g_settings.adaptiveDwindlePercentage = 20;
  const auto leftDrop = Commands::PlanTiledDropPreview(adaptiveDrop, source, {target, Zone::Left}, reportedMetrics);
  const auto rightDrop = Commands::PlanTiledDropPreview(adaptiveDrop, source, {target, Zone::Right}, reportedMetrics);
  const RECT expectedLeft{1088, 14, 1866, 1804};
  const RECT expectedRight{2280, 14, 3058, 1804};
  check(leftDrop && rightDrop && EqualRect(&leftDrop->dragged, &expectedLeft) &&
            EqualRect(&rightDrop->dragged, &expectedRight) &&
            Layout::RectWidth(leftDrop->affected) == 1178 && Layout::RectWidth(rightDrop->affected) == 1178,
        "both edges can borrow space within the adaptive limit and preview the chosen side");
  g_settings.adaptiveDwindleSlots = 0;
  check(!Commands::PlanTiledDropPreview(adaptiveDrop, source, {target, Zone::Left}, reportedMetrics) &&
            !Commands::PlanTiledDropPreview(adaptiveDrop, source, {target, Zone::Right}, reportedMetrics),
        "an edge drop cannot borrow space beyond the configured adaptive range");

  Workspace offEdge;
  offEdge.SetLayout(TileLayout::Bsp);
  offEdge.AdmitTiled(FloatingWindowFixture::Record(source));
  offEdge.AdmitTiledAfter(FloatingWindowFixture::Record(target), source);
  offEdge.AdmitTiledAfter(FloatingWindowFixture::Record(sibling), target);
  offEdge.SetBspSplitDirection(offEdge.Bsp().Parent(offEdge.Bsp().FindLeaf(target)), Layout::Axis::X);
  const Platform::Monitors::WorkspaceMetrics offEdgeMetrics{{0, 0, 1000, 600}, 96, 0};
  check(offEdge.StepTiledSize(offEdgeMetrics.workArea, 0, offEdge.TiledIndex(source), false, 20),
        "put the original target at x=300 to x=650");
  Workspace offEdgeCandidate = offEdge;
  std::vector<RECT> offEdgeRects;
  check(Commands::PrepareTiledDrop(offEdgeCandidate, source, {target, Zone::Right}, offEdgeMetrics.workArea) ==
            Placement::UserLayoutMutationKind::BspMerge &&
            Layout::BuildWorkspaceLayoutPlan(offEdgeCandidate, offEdgeMetrics.workArea, 0, &offEdgeRects) ==
                Layout::LayoutPlanStatus::Success &&
            offEdgeRects[offEdgeCandidate.TiledIndex(source)].right == 500 &&
            !Commands::PlanTiledDropPreview(offEdge, source, {target, Zone::Right}, offEdgeMetrics),
        "a split that overlaps the original tile away from the requested edge is rejected");

  for (const Zone zone : {Zone::Left, Zone::Top}) {
    Workspace leadingSplit = initial;
    check(Commands::PrepareTiledDrop(leadingSplit, source, {target, zone}, fixture.workArea) ==
              Placement::UserLayoutMutationKind::BspMerge &&
              leadingSplit.TiledIndex(source) + 1 == leadingSplit.TiledIndex(target),
          "dropping on a leading edge puts the dragged window first in the split");
  }
  Workspace expectedCenterSwap = initial;
  check(expectedCenterSwap.SwapTiled(source, target), "prepare a center swap across BSP branches");
  const auto centerPreview = Commands::PlanTiledDropPreview(initial, source, {target, Zone::Center}, fixture.metrics);
  const Workspace centerSwapped = dropOnto(source, target);
  const RECT afterCenterSwap = FloatingWindowFixture::Frame(source);
  const RECT affectedAfterSwap = FloatingWindowFixture::Frame(target);
  check(centerSwapped.HasSameLayoutStateAs(expectedCenterSwap) && centerPreview &&
            EqualRect(&centerPreview->dragged, &afterCenterSwap) &&
            EqualRect(&centerPreview->affected, &affectedAfterSwap),
        "a center drop previews both sides of the swap across BSP branches");
  FloatingWindowFixture::ClearLeases();
  g_workspaces.Save(fixture.key, initial);
  Placement::ArrangeWorkspace(fixture.key);

  const auto offTargetRejections = Diagnostics::g_runtime.counters.rejectedOrderCommands;
  check(dropOnto(target, source, splitEdge).HasSameLayoutStateAs(initial) &&
            Diagnostics::g_runtime.counters.rejectedOrderCommands == offTargetRejections + 1,
        "removing the dragged leaf cannot move it to a different tile while merging");

  // The target fits its current half. Trying to divide it along the next axis
  // would leave it less than the minimum it now reports.
  static POINT reportedMinimum;
  static WNDPROC originalProcedure;
  const LONG sourceWidth = Layout::RectWidth(fixture.workArea);
  const LONG sourceHeight = Layout::RectHeight(fixture.workArea);
  reportedMinimum = landscape ? POINT{3 * sourceWidth / 8, 3 * sourceHeight / 4}
                            : POINT{3 * sourceWidth / 4, 3 * sourceHeight / 8};
  const WNDPROC reportMinimum = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
    if (message == WM_GETMINMAXINFO) {
      reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = reportedMinimum;
      return 0;
    }
    return CallWindowProcW(originalProcedure, window, message, wparam, lparam);
  };
  originalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(target, GWLP_WNDPROC,
                                                                  reinterpret_cast<LONG_PTR>(reportMinimum)));
  check(originalProcedure != nullptr, "install the target's minimum-size procedure");
  const auto rejectedOrders = Diagnostics::g_runtime.counters.rejectedOrderCommands;
  FloatingWindowFixture::AppMove(source, originalSourceFrame.left + 15, originalSourceFrame.top,
                                Layout::RectWidth(originalSourceFrame), Layout::RectHeight(originalSourceFrame));
  check(dropOnto(source, target, splitEdge).HasSameLayoutStateAs(initial) &&
            Diagnostics::g_runtime.counters.rejectedOrderCommands == rejectedOrders + 1 &&
            Layout::RectsNear(FloatingWindowFixture::Frame(source), originalSourceFrame),
        "an infeasible merge is rejected and the dragged window snaps back");
  if (landscape) reportedMinimum.x = 1;
  else reportedMinimum.y = 1;
  Workspace previewWorkspace = fixture.Load();
  previewWorkspace.UpdateConstraints(target, {reportedMinimum.x, reportedMinimum.y, fixture.metrics.dpi, true});
  for (const Zone blockedEdge : {splitEdge, landscape ? Zone::Top : Zone::Left}) {
    const auto rejectedBefore = Diagnostics::g_runtime.counters.rejectedOrderCommands;
    check(!Commands::PlanTiledDropPreview(previewWorkspace, source, {target, blockedEdge}, fixture.metrics) &&
              dropOnto(source, target, blockedEdge).HasSameLayoutStateAs(initial) &&
              Diagnostics::g_runtime.counters.rejectedOrderCommands == rejectedBefore + 1,
          "a blocked edge neither uses nor commits a perpendicular split");
  }

  const UINT savedFadeMs = g_settings.dragPreviewFadeMs;
  g_settings.dragPreviewFadeMs = 0;
  g_workspaces.Save(fixture.key, previewWorkspace);
  DragPreview::DragSession previewDrag{source, source, fixture.key.ResolveMonitor(), fixture.key,
                                      originalSourceFrame, {}, {}, {}, {}};
  DragPreview::UpdateDropPreview(previewDrag, SharedGeometry::RectCenter(originalSourceFrame));
  check(!previewDrag.target && Overlay::g_presenter.fadeTo == 255 && Overlay::g_presenter.shown &&
            IsEqualGUID(Overlay::g_presenter.shown->desktopId, fixture.key.desktopId) &&
            EqualRect(&Overlay::g_presenter.shown->preview.dragged, &originalSourceFrame) &&
            EqualRect(&Overlay::g_presenter.shown->preview.affected, &originalSourceFrame),
        "a drop away from other tiles previews both shapes at the source tile");
  Overlay::FadeTo(0);
  const RECT targetFrame = FloatingWindowFixture::Frame(target);
  POINT blockedPoint = SharedGeometry::RectCenter(targetFrame);
  if (landscape) blockedPoint.y = targetFrame.bottom - 1;
  else blockedPoint.x = targetFrame.right - 1;
  DragPreview::UpdateDropPreview(previewDrag, blockedPoint);
  const auto& snapBack = Overlay::g_presenter.shown;
  check(previewDrag.target == Commands::TiledDropTarget{target, splitEdge} &&
            snapBack && Overlay::g_presenter.previewHost->visible &&
            EqualRect(&snapBack->preview.dragged, &originalSourceFrame) &&
            EqualRect(&snapBack->preview.affected, &originalSourceFrame),
        "a rejected split displays both shapes at the source tile");
  DragPreview::Shutdown(); Overlay::Shutdown();
  g_settings.dragPreviewFadeMs = savedFadeMs;

  const auto splitPreview = Commands::PlanTiledDropPreview(previewWorkspace, source, {target, fittingEdge}, fixture.metrics);
  const Workspace merged = dropOnto(source, target, fittingEdge);
  SetWindowLongPtrW(target, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProcedure));
  const RECT afterSplit = FloatingWindowFixture::Frame(source);
  const RECT affectedAfterSplit = FloatingWindowFixture::Frame(target);
  check(splitPreview && EqualRect(&splitPreview->dragged, &afterSplit) &&
            EqualRect(&splitPreview->affected, &affectedAfterSplit),
        "the fitting edge previews both panes in the split");
  check(merged.TiledWindows() == std::vector<HWND>({sibling, target, source}) &&
            merged.Bsp().Parent(merged.Bsp().FindLeaf(source)) == merged.Bsp().Parent(merged.Bsp().FindLeaf(target)),
        "the BSP drag keeps the target first and puts the source beside it");
  const BspNode& mergedSplit = merged.Bsp().Nodes()[merged.Bsp().Parent(merged.Bsp().FindLeaf(source))];
  check(mergedSplit.direction == Layout::FirstSplitAxis(fixture.workArea) && mergedSplit.ratio == 0.5,
        "the merge saves the chosen edge direction and keeps the new divider at 50/50");
  const RECT sourceBeforeSwap = FloatingWindowFixture::Frame(source);
  const RECT targetBeforeSwap = FloatingWindowFixture::Frame(target);
  Workspace expectedSwap = merged;
  check(expectedSwap.SwapTiled(source, target), "prepare the expected sibling swap");
  const Workspace siblingSwap = dropOnto(source, target);
  const RECT sourceAfterSwap = FloatingWindowFixture::Frame(source);
  const RECT targetAfterSwap = FloatingWindowFixture::Frame(target);
  check(siblingSwap.HasSameLayoutStateAs(expectedSwap) &&
            EqualRect(&sourceAfterSwap, &targetBeforeSwap) && EqualRect(&targetAfterSwap, &sourceBeforeSwap),
        "siblings exchange their exact tiles with BSP split zones enabled");

  g_settings.bspSplitEdgePercent = 0;
  check(dropOnto(source, target, splitEdge).HasSameLayoutStateAs(merged), "at zero edge size every zone swaps");
  g_settings.bspSplitEdgePercent = 25;
  Commands::PromoteFocusedWindow(source);
  check(fixture.Load().HasSameLayoutStateAs(expectedSwap), "the keyboard shortcut still swaps BSP leaves");

  FloatingWindowFixture::ClearLeases();
  g_workspaces.Save(fixture.key, initial);
  Placement::ArrangeWorkspace(fixture.key);
  g_settings.adaptiveDwindleSlots = 2;
  g_settings.adaptiveDwindlePercentage = 20;
  Workspace late = dropOnto(source, target, splitEdge);
  const auto* pending = Placement::g_pendingProofs.FindLayoutMutation(fixture.key);
  check(pending && pending->bspDropEdge, "a successful merge retains its requested edge while native placement settles");
  late.UpdateConstraints(target, {reportedMinimum.x, reportedMinimum.y, fixture.metrics.dpi, true});
  Workspace perpendicular = late;
  perpendicular.SetBspSplitDirection(perpendicular.Bsp().Parent(perpendicular.Bsp().FindLeaf(source)),
                                     Layout::FirstSplitAxis(fixture.workArea));
  std::vector<RECT> lateRects;
  check(Layout::BuildWorkspaceLayoutPlan(perpendicular, fixture.workArea, fixture.metrics.gap, &lateRects) ==
            Layout::LayoutPlanStatus::Success,
        "a native minimum could be satisfied by turning the new split");
  g_workspaces.Save(fixture.key, late);
  Placement::ArrangeWorkspace(fixture.key);
  check(fixture.Load().HasSameLayoutStateAs(initial),
        "a native minimum reported during placement rolls back a merge that would turn");
  late = dropOnto(source, target, splitEdge);
  late.UpdateConstraints(source, {landscape ? sourceWidth * 3 / 5 : 1,
                                  landscape ? 1 : sourceHeight * 3 / 5, fixture.metrics.dpi, true});
  pending = Placement::g_pendingProofs.FindLayoutMutation(fixture.key);
  const auto originalCells = Model::PlanBspCells(initial, fixture.workArea, fixture.metrics.gap).rects;
  RECT overlap{};
  check(Layout::BuildWorkspaceLayoutPlan(late, fixture.workArea, fixture.metrics.gap, &lateRects) ==
            Layout::LayoutPlanStatus::Success &&
            IntersectRect(&overlap, &lateRects[late.TiledIndex(source)], &originalCells[initial.TiledIndex(target)]) &&
            !EqualRect(&overlap, &lateRects[late.TiledIndex(source)]) && pending && pending->bspDropEdge &&
            IntersectRect(&overlap, &lateRects[late.TiledIndex(source)], &*pending->bspDropEdge),
        "the raised minimum extends beyond the original tile and still overlaps the requested edge");
  g_workspaces.Save(fixture.key, late);
  Placement::ArrangeWorkspace(fixture.key);
  check(fixture.Load().HasSameLayoutStateAs(late),
        "a native minimum reported during placement can borrow space and keep the merge on its requested edge");
  FloatingWindowFixture::ClearLeases();
  fixture.Seed(TileLayout::Bsp);
  Workspace uneven = fixture.Load();
  check(uneven.AdmitTiled(FloatingWindowFixture::Record(target)), "admit a second tile under the root divider");
  uneven.ClearInsertionPlacement(target);
  check(uneven.StepTiledSize(fixture.workArea, fixture.metrics.gap, uneven.TiledIndex(target), true, 10),
        "make the target tile larger than the source tile");
  g_workspaces.Save(fixture.key, uneven);
  Placement::ArrangeWorkspace(fixture.key);
  uneven = fixture.Load();
  const RECT narrowTile = FloatingWindowFixture::Frame(source);
  const RECT wideTile = FloatingWindowFixture::Frame(target);
  const auto splitSpan = [landscape](const RECT& tile) {
    return landscape ? Layout::RectWidth(tile) : Layout::RectHeight(tile);
  };
  const LONG middleSpan = (splitSpan(narrowTile) + splitSpan(wideTile)) / 2;
  const WindowConstraints increasedMinimum{landscape ? middleSpan : 1, landscape ? 1 : middleSpan,
                                           fixture.metrics.dpi, true};
  const double originalRatio = uneven.Bsp().Nodes().front().ratio;
  g_settings.adaptiveDwindleSlots = 0;
  const auto rotatedAxis = landscape ? Layout::Axis::Y : Layout::Axis::X;
  const Zone leadingPerpendicular = landscape ? Zone::Top : Zone::Left;
  for (const Zone edge : {leadingPerpendicular, splitEdge}) {
    const auto preview = Commands::PlanTiledDropPreview(uneven, source, {target, edge}, fixture.metrics);
    const Workspace rotated = dropOnto(source, target, edge);
    const RECT sourceFrame = FloatingWindowFixture::Frame(source);
    const RECT targetFrame = FloatingWindowFixture::Frame(target);
    const bool leading = edge == leadingPerpendicular;
    check(preview && EqualRect(&preview->dragged, &sourceFrame) && EqualRect(&preview->affected, &targetFrame) &&
              rotated.Bsp().Nodes().front().direction == rotatedAxis &&
              rotated.TiledWindows().front() == (leading ? source : target) &&
              std::abs(rotated.Bsp().Nodes().front().ratio - (leading ? originalRatio : 1.0 - originalRatio)) < 1e-9,
          "both perpendicular edges rotate the sibling split, carry shares, and match the drop preview");
    FloatingWindowFixture::ClearLeases();
    g_workspaces.Save(fixture.key, uneven);
    Placement::ArrangeWorkspace(fixture.key);
  }
  Workspace alongAxis = dropOnto(source, target, fittingEdge);
  check(!alongAxis.Bsp().Nodes().front().direction && alongAxis.TiledWindows().front() == target &&
            std::abs(alongAxis.Bsp().Nodes().front().ratio - (1.0 - originalRatio)) < 1e-9,
        "an edge along the existing split performs the weighted swap");
  FloatingWindowFixture::ClearLeases();
  g_workspaces.Save(fixture.key, uneven);
  Placement::ArrangeWorkspace(fixture.key);

  Commands::ApplyLayoutAction(source);
  const Workspace rotatedByKey = fixture.Load();
  check(rotatedByKey.TiledWindows() == uneven.TiledWindows() &&
            rotatedByKey.Bsp().Nodes().front().direction == rotatedAxis &&
            rotatedByKey.Bsp().Nodes().front().ratio == originalRatio,
        "the layout action rotates the focused split without changing its order or ratio");
  Commands::ApplyLayoutAction(source);
  check(fixture.Load().HasSameLayoutStateAs(uneven), "the layout action toggles the split back");

  reportedMinimum = {landscape ? 1 : sourceWidth * 3 / 4, landscape ? sourceHeight * 3 / 4 : 1};
  Workspace tooTallToRotate = uneven;
  tooTallToRotate.UpdateConstraints(source, {reportedMinimum.x, reportedMinimum.y, fixture.metrics.dpi, true});
  g_workspaces.Save(fixture.key, tooTallToRotate);
  originalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(source, GWLP_WNDPROC,
                                                                  reinterpret_cast<LONG_PTR>(reportMinimum)));
  check(originalProcedure != nullptr, "install a minimum that fits only the current split direction");
  const auto rejectedRotations = Diagnostics::g_runtime.counters.rejectedOrderCommands;
  check(!Commands::PlanTiledDropPreview(tooTallToRotate, source, {target, splitEdge}, fixture.metrics) &&
            dropOnto(source, target, splitEdge).HasSameLayoutStateAs(uneven),
        "a perpendicular drop that cannot fit rejects the rotation");
  Commands::ApplyLayoutAction(source);
  check(fixture.Load().HasSameLayoutStateAs(uneven) &&
            Diagnostics::g_runtime.counters.rejectedOrderCommands == rejectedRotations + 2,
        "the layout action also rejects a rotation that cannot fit");
  SetWindowLongPtrW(source, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProcedure));
  FloatingWindowFixture::ClearLeases();
  g_workspaces.Save(fixture.key, uneven);
  Placement::ArrangeWorkspace(fixture.key);

  reportedMinimum = {increasedMinimum.minWidth, increasedMinimum.minHeight};
  originalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(target, GWLP_WNDPROC,
                                                                  reinterpret_cast<LONG_PTR>(reportMinimum)));
  check(originalProcedure != nullptr, "install a minimum that fits only the wider tile");
  Workspace weightedSwap = dropOnto(source, target);
  check(weightedSwap.TiledWindows() == std::vector<HWND>({target, source}) &&
            std::abs(weightedSwap.Bsp().Nodes().front().ratio - (1.0 - originalRatio)) < 1e-9 &&
            std::abs(splitSpan(FloatingWindowFixture::Frame(source)) - splitSpan(narrowTile)) <= 1 &&
            std::abs(splitSpan(FloatingWindowFixture::Frame(target)) - splitSpan(wideTile)) <= 1,
        "siblings carry their saved shares to the opposite side, even when the larger window needs its share");
  SetWindowLongPtrW(target, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProcedure));

  g_settings.adaptiveDwindleSlots = 2;
  Placement::RefreshWorkspaceNativeConstraints(weightedSwap, true, L"BSP drag test");
  weightedSwap.UpdateConstraints(source, increasedMinimum);
  g_workspaces.Save(fixture.key, weightedSwap);
  Placement::ArrangeWorkspace(fixture.key);
  const LONG adaptedSourceSpan = splitSpan(FloatingWindowFixture::Frame(source));
  const LONG adaptedTargetSpan = splitSpan(FloatingWindowFixture::Frame(target));
  check(adaptedSourceSpan > splitSpan(narrowTile), "show an adaptively shifted divider");
  originalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(source, GWLP_WNDPROC,
                                                                  reinterpret_cast<LONG_PTR>(reportMinimum)));
  check(originalProcedure != nullptr, "keep reporting the dragged window's minimum during the swap");
  const Workspace adaptedSwap = dropOnto(source, target);
  check(adaptedSwap.TiledWindows() == uneven.TiledWindows() &&
            std::abs(adaptedSwap.Bsp().Nodes().front().ratio - originalRatio) < 1e-9 &&
            std::abs(splitSpan(FloatingWindowFixture::Frame(source)) - adaptedSourceSpan) <= 1 &&
            std::abs(splitSpan(FloatingWindowFixture::Frame(target)) - adaptedTargetSpan) <= 1,
        "swapping preserves the saved shares while adaptive fitting continues on the opposite sides");
  SetWindowLongPtrW(source, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProcedure));

  g_settings.dragPreviewFillOpacity = 25;
  g_settings.dragPreviewOutlineOpacity = 100;
  g_settings.dragPreviewFadeMs = 96;
  const HWND previouslyFocused = GetForegroundWindow();
  DragPreview::Show({narrowTile, wideTile});
  auto& overlay = Overlay::g_presenter;
  const ULONGLONG fadeInStart = overlay.fadeStarted;
  Overlay::StepFade(fadeInStart + g_settings.dragPreviewFadeMs / 2);
  check(overlay.fadeAlpha == 128, "the preview fades toward the configured opacity");
  DragPreview::Show({wideTile, narrowTile});
  check(overlay.fadeStarted == fadeInStart, "changing targets does not restart a fade");
  Overlay::StepFade(fadeInStart + g_settings.dragPreviewFadeMs);
  const auto previewVisible = [&] {
    return overlay.previewHost && IsWindowVisible(overlay.previewHost->window);
  };
  check(previewVisible() && overlay.shown &&
            EqualRect(&overlay.shown->preview.dragged, &wideTile) &&
            EqualRect(&overlay.shown->preview.affected, &narrowTile) &&
            overlay.style.fillOpacity == 25 && overlay.style.outlineOpacity == 100 &&
            GetForegroundWindow() == previouslyFocused,
        "the retained preview uses the planned rectangles and opacity settings without taking focus");
  DragPreview::Show({wideTile, narrowTile});
  check(overlay.fadeAlpha == 255 && !overlay.fading, "the same preview rectangles do not start another fade");
  DragPreview::Show({narrowTile, wideTile});
  check(overlay.fadeAlpha == 128 && EqualRect(&overlay.shown->preview.dragged, &narrowTile),
        "a changed destination appears immediately at half opacity");
  const ULONGLONG targetFadeStart = overlay.fadeStarted;
  Overlay::StepFade(targetFadeStart + g_settings.dragPreviewFadeMs / 2);
  check(overlay.fadeAlpha == 192, "a changed destination fades toward full strength");
  Overlay::StepFade(targetFadeStart + g_settings.dragPreviewFadeMs);
  DragPreview::End();
  const ULONGLONG fadeOutStart = overlay.fadeStarted;
  Overlay::StepFade(fadeOutStart + g_settings.dragPreviewFadeMs / 2);
  check(previewVisible() && overlay.fadeAlpha == 127,
        "the retained preview stays visible while the fade is in progress");
  Overlay::StepFade(fadeOutStart + g_settings.dragPreviewFadeMs);
  check(!previewVisible() && !overlay.fading, "the completed fade hides the preview");
  g_settings.dragPreviewFillOpacity = 0;
  g_settings.dragPreviewFadeMs = 0;
  DragPreview::Show({wideTile, narrowTile});
  check(g_settings.DragPreviewEnabled() && previewVisible() && overlay.style.fillOpacity == 0 &&
            overlay.style.outlineOpacity == 100 && overlay.fadeAlpha == 255 && !overlay.fading,
        "an outline-only preview with zero fade duration shows immediately");
  DragPreview::End();
  check(!previewVisible() && !overlay.fading, "zero fade duration also hides the preview immediately");

  const HICON sourceIcon = LoadIconW(nullptr, IDI_INFORMATION);
  const HICON targetIcon = LoadIconW(nullptr, IDI_WARNING);
  SendMessageW(source, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sourceIcon));
  SendMessageW(target, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(targetIcon));
  auto waitForIcon = [&](auto&& condition, bool dispatchCompletions = true) {
    // Pump the icon replies, leaving fade steps under the test's control.
    const auto deadline = GetTickCount64() + 3000;
    do {
      MSG message{};
      PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      while (dispatchCompletions && PeekMessageW(&message, nullptr, Overlay::kBadgeIconReadyMessage,
                                                 Overlay::kBadgeIconReadyMessage, PM_REMOVE)) {
        DispatchMessageW(&message);
      }
      if (condition()) return true;
      const DWORD wake = dispatchCompletions ? QS_SENDMESSAGE | QS_POSTMESSAGE : QS_SENDMESSAGE;
      MsgWaitForMultipleObjectsEx(0, nullptr, 10, wake, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return false;
  };
  g_settings.dragPreviewFillOpacity = 25;
  g_settings.dragPreviewIconSizeDip = 24;
  g_settings.dragPreviewFadeMs = 96;
  const GUID& desktopId = fixture.key.desktopId;
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  const auto& draggedBadge = overlay.draggedBadge;
  const auto& affectedBadge = overlay.affectedBadge;
  check(previewVisible() && !draggedBadge.icon && !affectedBadge.icon,
        "the cold preview shows the shapes before either icon lookup finishes");
  auto& lookup = *overlay.iconLookup;
  for (const auto& [window, icon] : {std::pair{source, sourceIcon}, std::pair{target, targetIcon}}) {
    check(waitForIcon([&] { return lookup.state.load() == Overlay::BadgeIconLookup::State::Ready; }, false) &&
              lookup.key.window == window,
          "the icon worker completes each queued badge");
    // The test owns both apps and the renderer on this thread, so presenting
    // the first frame can delay its own WM_GETICON reply past the timeout.
    if (lookup.queryFailed) {
      lookup.icon = Overlay::PreviewIcon(CopyIcon(icon), DestroyIcon);
      lookup.queryFailed = false;
    }
    Overlay::UpdateBadgesAfterIconLookup();
  }
  auto& desktopIcons = overlay.iconsByDesktop.at(desktopId);
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs / 2);
  const POINT tileCenter = SharedGeometry::RectCenter(narrowTile);
  const POINT badgeCenter = SharedGeometry::RectCenter(draggedBadge.frame);
  check(previewVisible() && draggedBadge.icon && affectedBadge.icon && draggedBadge.icon != affectedBadge.icon &&
            desktopIcons.size() == 2 && draggedBadge.iconSize == 36 && Layout::RectWidth(draggedBadge.frame) == 48 &&
            badgeCenter.x == tileCenter.x && badgeCenter.y == tileCenter.y && overlay.fadeAlpha == 128,
        "badges use DPI-scaled sizes and remain centered during a fade");
  const auto cachedIcon = draggedBadge.icon;
  const bool savedLightTheme = TrayUi::g_session.systemLightTheme;
  const bool actualLightTheme = TrayUi::ReadSystemLightTheme();
  TrayUi::g_session.systemLightTheme = !actualLightTheme;
  const COLORREF oldBadgeColor = TrayUi::FlyoutBackgroundColor();
  const ULONGLONG badgeFadeStart = overlay.fadeStarted;
  TrayUi::TrayWindowProc(nullptr, WM_THEMECHANGED, 0, 0);
  check(TrayUi::g_session.systemLightTheme == actualLightTheme &&
            overlay.style.badgeAppearance.background != oldBadgeColor &&
            overlay.style.badgeAppearance.light == actualLightTheme &&
            draggedBadge.icon == cachedIcon && overlay.fadeStarted == badgeFadeStart && overlay.fadeAlpha == 128,
        "a theme change updates the badge appearance without restarting the fade or clearing the icons");
  TrayUi::g_session.systemLightTheme = savedLightTheme;
  SendMessageW(source, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(targetIcon));
  DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
  check(draggedBadge.icon == cachedIcon && desktopIcons.size() == 2,
        "moving the preview keeps the cached window icons");
  DragPreview::Show({wideTile, wideTile, source}, true, 144, desktopId);
  check(draggedBadge.icon && !affectedBadge.icon,
        "a snap-back shows only the dragged app badge");
  DragPreview::End();
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  check(!previewVisible() && !draggedBadge.icon, "the badge hides and releases its icon after the fade");
  Overlay::HandleOverlayMessage(MSG{.message = Overlay::kNewDragMessage});
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(draggedBadge.icon == cachedIcon && affectedBadge.icon && desktopIcons.size() == 2,
        "the next drag reuses both cached icons");
  const auto cachedSource = std::ranges::find_if(desktopIcons, [source](const auto& entry) {
    return entry.key.window == source;
  });
  // Deliver a timed-out refresh without depending on scheduler timing.
  lookup.key = cachedSource->key;
  lookup.desktopId = desktopId;
  lookup.queryFailed = true;
  lookup.state.store(Overlay::BadgeIconLookup::State::Ready);
  Overlay::UpdateBadgesAfterIconLookup();
  check(cachedSource->icon == cachedIcon && cachedSource->retry && draggedBadge.icon == cachedIcon,
        "a failed refresh keeps the last good icon visible and retries on a later drag");

  for (auto& entry : desktopIcons) entry.loadedAt -= 60'000;
  SendMessageW(target, WM_SETICON, ICON_SMALL, 0);
  DragPreview::End();
  Overlay::HandleOverlayMessage(MSG{.message = Overlay::kNewDragMessage});
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(draggedBadge.icon == cachedIcon && affectedBadge.icon,
        "aging icons remain visible while a refresh runs");
  // The cache must apply successful results independently of the native
  // window-message timeout, which has its own coverage above.
  for (HWND window : {source, target}) {
    check(waitForIcon([&] { return lookup.state.load() == Overlay::BadgeIconLookup::State::Ready; }, false) &&
              lookup.key.window == window,
          "the aged badge is queued for a new lookup");
    lookup.icon = window == source ? Overlay::PreviewIcon(CopyIcon(targetIcon), DestroyIcon)
                                   : Overlay::PreviewIcon{};
    lookup.queryFailed = false;
    Overlay::UpdateBadgesAfterIconLookup();
  }
  check(draggedBadge.icon && draggedBadge.icon != cachedIcon && !affectedBadge.icon,
        "a later drag refreshes the changed icon and clears the one the app removed");
  DragPreview::OnWindowEvent(EVENT_OBJECT_DESTROY, target);
  check(desktopIcons.size() == 1, "destroy notification evicts the target icon");
  g_settings.dragPreviewIconSizeDip = 0;
  DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
  check(!draggedBadge.icon && !affectedBadge.icon && desktopIcons.size() == 1,
        "a zero icon size disables badges and icon queries");

  g_settings.dragPreviewIconSizeDip = 24;
  DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
  check(waitForIcon([&] {
          return desktopIcons.size() == 2 && overlay.iconLookup &&
              overlay.iconLookup->state.load() == Overlay::BadgeIconLookup::State::Idle;
        }) && !affectedBadge.icon,
        "a missing icon leaves the affected badge empty");
  SendMessageW(target, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(targetIcon));
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(overlay.iconLookup->state.load() == Overlay::BadgeIconLookup::State::Idle && !affectedBadge.icon,
        "another hover during the same drag does not repeat the miss");
  Overlay::HandleOverlayMessage(MSG{.message = Overlay::kNewDragMessage});
  DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
  check(waitForIcon([&] { return lookup.state.load() == Overlay::BadgeIconLookup::State::Ready; }, false) &&
            lookup.key.window == target,
        "the next drag queues the missing icon again");
  lookup.icon = Overlay::PreviewIcon(CopyIcon(targetIcon), DestroyIcon);
  lookup.queryFailed = false;
  Overlay::UpdateBadgesAfterIconLookup();
  check(affectedBadge.icon != nullptr,
        "the next drag retries the missing icon and shows it when available");
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  DragPreview::End();
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  check(!previewVisible() && !draggedBadge.icon && !affectedBadge.icon,
        "the completed fade clears the preview and both badges");
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  HWND previewHost = overlay.previewHost->window;
  Overlay::ResetHostSurface(*overlay.previewHost);
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(previewVisible() && overlay.previewHost->window == previewHost,
        "a preview update rebuilds a reset surface in the same host window");

  // A window that goes topmost while the host is still on screen (a preview
  // fading out, a toast) stays beneath the next preview.
  const HWND laterTopmost = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
                                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  auto isAbove = [](HWND upper, HWND lower) {
    for (HWND window = GetWindow(lower, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV)) {
      if (window == upper) return true;
    }
    return false;
  };
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  DragPreview::End();
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs / 2);
  check(laterTopmost && previewVisible() &&
            SetWindowPos(laterTopmost, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) &&
            isAbove(laterTopmost, previewHost),
        "another topmost window can rise above the fading host");
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(previewVisible() && isAbove(previewHost, laterTopmost),
        "a new preview raises its visible host above later topmost windows");
  DragPreview::End();
  Overlay::StepFade(overlay.fadeStarted + g_settings.dragPreviewFadeMs);
  check(!previewVisible() &&
            SetWindowPos(laterTopmost, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) &&
            isAbove(laterTopmost, previewHost),
        "another topmost window can rise above the hidden host");
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(previewVisible() && isAbove(previewHost, laterTopmost),
        "showing a hidden host raises it above later topmost windows");
  if (laterTopmost) DestroyWindow(laterTopmost);

  // A monitor that disappeared gives up its host window and surface.
  {
    const auto goneMonitor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x7ffe));
    auto& gone = overlay.hosts[goneMonitor];
    gone = std::make_unique<Overlay::MonitorHost>();
    gone->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, Overlay::kOverlayClassName, L"", WS_POPUP,
                                   0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), gone.get());
    const HWND goneWindow = gone->window;
    const size_t hosts = overlay.hosts.size();
    Overlay::NotifyDisplayChanged();
    check(goneWindow && !IsWindow(goneWindow) && !overlay.hosts.contains(goneMonitor) &&
              overlay.hosts.size() == hosts - 1 && previewVisible(),
          "a display change frees a vanished monitor's host and keeps the live preview");
  }

  GUID otherDesktopId = desktopId;
  otherDesktopId.Data1 ^= 1;
  const auto firstDesktopIcon = draggedBadge.icon;
  Overlay::HandleOverlayMessage(MSG{.message = Overlay::kNewDragMessage});
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, otherDesktopId);
  check(!draggedBadge.icon, "a cold desktop does not display the previous desktop's cached badge");
  check(waitForIcon([&] {
          const auto desktop = overlay.iconsByDesktop.find(otherDesktopId);
          return desktop != overlay.iconsByDesktop.end() && desktop->second.size() == 2 &&
              overlay.iconLookup->state.load() == Overlay::BadgeIconLookup::State::Idle;
        }), "the same preview on a different desktop completes its own lookups");

  auto& otherDesktopIcons = overlay.iconsByDesktop.at(otherDesktopId);
  auto oldestKey = otherDesktopIcons.front().key;
  oldestKey.size = 1000;
  for (LONG size = oldestKey.size; otherDesktopIcons.size() < 128; ++size) {
    auto key = oldestKey;
    key.size = size;
    otherDesktopIcons.push_back({.key = key});
  }
  SendMessageW(sibling, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sourceIcon));
  DragPreview::Show({wideTile, wideTile, sibling}, true, 144, otherDesktopId);
  check(waitForIcon([&] { return overlay.iconLookup->state.load() == Overlay::BadgeIconLookup::State::Idle; }) &&
            otherDesktopIcons.size() == 128 &&
            std::ranges::any_of(otherDesktopIcons, [sibling](const auto& entry) { return entry.key.window == sibling; }) &&
            std::ranges::find(otherDesktopIcons, oldestKey, &Overlay::CachedBadgeIcon::key) == otherDesktopIcons.end(),
        "a full desktop evicts its oldest entry to load a new badge");
  DragPreview::Show({narrowTile, wideTile, source, target}, true, 144, desktopId);
  check(draggedBadge.icon == firstDesktopIcon && desktopIcons.size() == 2,
        "filling another desktop does not evict this desktop's icons");
  DragPreview::OnWindowEvent(EVENT_OBJECT_DESTROY, target);
  check(desktopIcons.size() == 1 && otherDesktopIcons.size() == 127,
        "destroying a window removes its icons across desktops");
  Overlay::HandleOverlayMessage(MSG{.message = Overlay::kDesktopListMessage,
      .lParam = reinterpret_cast<LPARAM>(new std::vector<GUID>{desktopId})});
  check(overlay.iconsByDesktop.size() == 1 && overlay.iconsByDesktop.contains(desktopId),
        "a refreshed desktop list evicts the removed desktop's whole cache");
  DragPreview::Shutdown(); Overlay::Shutdown();

  // Use this thread's queue to make the publisher faster than the consumer.
  {
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    Overlay::g_overlayThread.id.store(GetCurrentThreadId());
    auto style = DragPreview::CurrentStyle();
    style.fadeMs = 0;
    Overlay::PublishPreview(Overlay::PresentRequest{{narrowTile, wideTile}, style});
    Overlay::PublishPreview(Overlay::PresentRequest{{wideTile, narrowTile}, style});
    check(Overlay::PostToOverlayThread(Overlay::kNewDragMessage), "queue a drag boundary");
    Overlay::PublishPreview(Overlay::PresentRequest{{narrowTile, wideTile}, style});
    Overlay::PublishPreview(std::nullopt);
    Overlay::g_overlayThread.id.store(0);

    const HWND threadMessages = reinterpret_cast<HWND>(-1);
    check(PeekMessageW(&message, threadMessages, Overlay::kPreviewUpdateMessage,
                       Overlay::kDesktopListMessage, PM_REMOVE) &&
              message.message == Overlay::kPreviewUpdateMessage,
          "the first batch queues one preview update");
    Overlay::HandleOverlayMessage(message);
    check(overlay.shown && EqualRect(&overlay.shown->preview.dragged, &wideTile),
          "the renderer takes the newest target in the batch");
    check(PeekMessageW(&message, threadMessages, Overlay::kPreviewUpdateMessage,
                       Overlay::kDesktopListMessage, PM_REMOVE) &&
              message.message == Overlay::kNewDragMessage,
          "a new drag separates pending preview updates");
    Overlay::HandleOverlayMessage(message);
    check(PeekMessageW(&message, threadMessages, Overlay::kPreviewUpdateMessage,
                       Overlay::kDesktopListMessage, PM_REMOVE) &&
              message.message == Overlay::kPreviewUpdateMessage,
          "the second batch queues one preview update");
    Overlay::HandleOverlayMessage(message);
    check(overlay.fadeTo == 0 && !previewVisible() &&
              !Overlay::g_overlayThread.pendingUpdate &&
              !PeekMessageW(&message, threadMessages, Overlay::kPreviewUpdateMessage,
                            Overlay::kDesktopListMessage, PM_REMOVE),
          "a pending hide supersedes a stale target and leaves the queue empty");
    DragPreview::Shutdown(); Overlay::Shutdown();
  }

  // A preview and solid toast share the same retained host on the renderer.
  {
    auto threadWindows = [](DWORD thread) {
      std::vector<HWND> windows;
      EnumThreadWindows(thread, [](HWND hwnd, LPARAM list) WINAPI -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(hwnd, className, ARRAYSIZE(className));
        if (std::wstring_view(className) == Overlay::kOverlayClassName) {
          reinterpret_cast<std::vector<HWND>*>(list)->push_back(hwnd);
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&windows));
      return windows;
    };
    auto waitFor = [](auto&& condition) {
      const ULONGLONG deadline = GetTickCount64() + 3000;
      while (!condition() && GetTickCount64() < deadline) {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
      }
      return condition();
    };
    auto noneVisible = [&](DWORD thread) {
      for (HWND window : threadWindows(thread)) {
        if (IsWindowVisible(window)) return false;
      }
      return true;
    };

    g_settings.dragPreviewFillOpacity = 25;
    g_settings.dragPreviewOutlineOpacity = 100;
    g_settings.dragPreviewIconSizeDip = 0;
    g_settings.dragPreviewFadeMs = 96;
    check(Overlay::EnsureOverlayThread(), "start the overlay thread");
    const DWORD overlayThread = Overlay::g_overlayThread.id.load();
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    check(GetMonitorInfoW(fixture.key.ResolveMonitor(), &monitorInfo), "resolve the test monitor's bounds");
    const RECT overlayWorkArea = SharedGeometry::KeepTilesOffWholeMonitor(monitorInfo.rcWork, monitorInfo.rcMonitor);
    HWND host = nullptr;
    auto findHost = [&] {
      for (HWND window : threadWindows(overlayThread)) {
        RECT frame{};
        if (IsWindowVisible(window) && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) &&
            GetWindowRect(window, &frame) && EqualRect(&frame, &overlayWorkArea)) {
          host = window;
          return true;
        }
      }
      return false;
    };
    DragPreview::Show({wideTile, narrowTile});
    check(waitFor(findHost), "the overlay thread shows a retained work-area host");
    check(GetPropW(host, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
          "the retained host opts out of the Shell's fullscreen detection");
    check(Overlay::IsOverlayThread(GetWindowThreadProcessId(host, nullptr)) &&
              Platform::WindowEvents::IsOwnWindow(host) && !Overlay::IsOverlayThread(GetCurrentThreadId()),
          "MultiWM treats the overlay thread's windows as its own");
    const POINT hitPoint{overlayWorkArea.left + 32, overlayWorkArea.top + 32};
    const HWND behindPreview = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"", WS_POPUP | SS_BLACKRECT | SS_NOTIFY, hitPoint.x - 10, hitPoint.y - 10, 20, 20,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(behindPreview && SetWindowPos(behindPreview, host, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE) &&
              WindowFromPoint(hitPoint) == behindPreview,
          "the overlay leaves hit testing to the window beneath it");
    DestroyWindow(behindPreview);
    g_settings.dragPreviewIconSizeDip = 24;
    DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
    DragPreview::Hide();
    check(waitFor([&] { return noneVisible(overlayThread); }), "the retained preview fades out on its thread");
    TrayUi::g_preferences.showToasts = true;
    const HWND savedTrayWindow = std::exchange(TrayUi::g_session.trayWindow, source);
    const int savedOffset = std::exchange(TrayUi::g_preferences.flyoutOffsetXDip, 100000);
    TrayUi::ShowStatusFlyout(L"Retained status", fixture.key.ResolveMonitor());
    TrayUi::g_session.trayWindow = savedTrayWindow;
    TrayUi::g_preferences.flyoutOffsetXDip = savedOffset;
    check(waitFor([&] { return IsWindowVisible(host); }) && threadWindows(overlayThread).size() == 1,
          "a toast with an out-of-bounds offset still uses the same preview host");
    DragPreview::Show({wideTile, narrowTile, source, target}, true, 144, desktopId);
    DragPreview::Hide();
    check(waitFor([&] { return noneVisible(overlayThread); }) && threadWindows(overlayThread).size() == 1,
          "the shared host hides after the preview and toast finish");
    DragPreview::Shutdown(); Overlay::Shutdown();
    check(!Overlay::g_overlayThread.handle && !Overlay::g_overlayThread.stopEvent &&
              !Overlay::IsOverlayThread(overlayThread) && threadWindows(overlayThread).empty(),
          "shutdown joins the thread, which destroys its windows");

  }
  TrayUi::g_preferences.showToasts = savedToasts;
  CoUninitialize();
  std::printf("Passed %u BSP drag checks.\n", check.passed);
}

// Stand-ins for user32's ghost pairing: `g_testGhost` is the ghost of `g_testHung`.
static HWND g_testHung = nullptr;
static HWND g_testGhost = nullptr;
static HWND WINAPI TestGhostFromHung(HWND hwnd) { return hwnd && hwnd == g_testHung ? g_testGhost : nullptr; }
static HWND WINAPI TestHungFromGhost(HWND hwnd) { return hwnd && hwnd == g_testGhost ? g_testHung : nullptr; }

// MultiWM can't move a real ghost (dwm.exe refuses), so it lays the other windows
// out around the ghost's fixed size and refuses user layout changes that would
// move the ghost's planned frame, unless the new frame is where the ghost is
// shown. The test "ghost" is an ordinary window, which the user's drags move.
static void CheckGhostLayoutAndCommands(CheckCounter& check, FloatingWindowFixture& f, HWND other) {
  const HWND hung = g_testHung;
  const HWND ghost = g_testGhost;
  const RECT originalFrame = FloatingWindowFixture::Frame(hung);
  const LONG width = std::min<LONG>(320, (Layout::RectWidth(f.workArea) - f.metrics.gap) / 4);
  const LONG height = std::min<LONG>(240, (Layout::RectHeight(f.workArea) - f.metrics.gap) / 4);
  auto planned = [&](const Workspace& workspace, HWND hwnd) {
    std::vector<RECT> plan;
    const size_t index = workspace.TiledIndex(hwnd);
    return Layout::BuildWorkspaceLayoutPlan(workspace, f.workArea, f.metrics.gap, &plan) ==
                   Layout::LayoutPlanStatus::Success && index < plan.size()
               ? plan[index] : RECT{};
  };

  FloatingWindowFixture::AppMove(ghost, f.workArea.left + 25, f.workArea.top + 30, width, height);
  const RECT ghostShown = FloatingWindowFixture::Frame(ghost);
  const HWND third = f.CreateTestWindow(nullptr);
  f.Seed(TileLayout::Columns);
  Workspace workspace = f.Load();
  workspace.AdmitTiled(FloatingWindowFixture::Record(other));
  workspace.AdmitTiled(FloatingWindowFixture::Record(third));
  workspace.UpdateConstraints(hung, WindowConstraints{1, 1, f.metrics.dpi, true});
  g_workspaces.Save(f.key, workspace);

  Reconcile::ProcessWindowLifecycleEvent(EVENT_OBJECT_STATECHANGE, hung);
  workspace = f.Load();
  check(workspace.Find(hung)->ghostSize.cx == width && workspace.Find(hung)->ghostSize.cy == height &&
            Layout::RectsNear(FloatingWindowFixture::Frame(ghost), ghostShown) &&
            Layout::RectsNear(FloatingWindowFixture::Frame(other), planned(workspace, other)),
        "a ghost's siblings tile around its fixed size; the ghost stays where it is");
  const RECT appFrame = FloatingWindowFixture::Frame(hung);
  check(EqualRect(&originalFrame, &appFrame) && workspace.Find(hung)->constraints.minWidth == 1,
        "ghost layout keeps the real window frame and native limits intact");

  // Splitters: one that would move the ghost's planned frame is refused, one
  // between two other windows is not.
  const Workspace beforeResize = workspace;
  MoveSizeGesture gesture;
  gesture.hasRects = true;
  gesture.start = gesture.end = FloatingWindowFixture::Frame(other);
  gesture.end.left -= Layout::RectWidth(f.workArea) / 20;
  Commands::HandleUserResize(f.key, workspace, other, f.workArea, f.metrics.gap, gesture);
  check(f.Load().HasSameLayoutStateAs(beforeResize), "a splitter drag that would move the ghost is refused");
  workspace = f.Load();
  gesture.start = gesture.end = FloatingWindowFixture::Frame(third);
  gesture.end.left -= Layout::RectWidth(f.workArea) / 20;
  Commands::HandleUserResize(f.key, workspace, third, f.workArea, f.metrics.gap, gesture);
  const RECT ghostFrameAfter = planned(f.Load(), hung);
  const RECT ghostFrameBefore = planned(beforeResize, hung);
  check(!f.Load().HasSameLayoutStateAs(beforeResize) && EqualRect(&ghostFrameAfter, &ghostFrameBefore),
        "a splitter drag that leaves the ghost's frame alone is accepted");

  // Order: refused while it would move the ghost, accepted once the user has
  // dragged the ghost to where the new order puts it.
  Commands::DemoteFocusedWindow(ghost);
  check(f.Load().TiledIndex(hung) == 0, "demoting the focused ghost is refused while it can't move");
  Workspace demoted = f.Load();
  demoted.SwapTiled(hung, other);
  const RECT demotedFrame = planned(demoted, hung);
  FloatingWindowFixture::AppMove(ghost, demotedFrame.left, demotedFrame.top, width, height);
  Commands::DemoteFocusedWindow(ghost);
  check(f.Load().TiledIndex(hung) == 1, "the same demotion is accepted once the ghost is where it lands");

  MoveSizeGesture drop;
  drop.hasDropPoint = true;
  drop.dropPoint = SharedGeometry::RectCenter(FloatingWindowFixture::Frame(ghost));
  workspace = f.Load();
  g_settings.mouseMoveBehavior = MouseMoveBehavior::Swap;
  Commands::HandleUserMove(f.key, workspace, other, drop, f.workArea);
  check(f.Load().TiledIndex(hung) == 1 && f.Load().TiledIndex(other) == 0,
        "dropping a sibling onto the ghost is refused: the ghost would have to move");

  g_testGhost = nullptr;
  Reconcile::ProcessWindowLifecycleEvent(EVENT_OBJECT_STATECHANGE, hung);
  workspace = f.Load();
  check(workspace.Find(hung)->ghostSize.cx == 0 && workspace.Find(hung)->ghostSize.cy == 0 &&
            Layout::RectWidth(FloatingWindowFixture::Frame(hung)) > width,
        "after recovery the actual app uses its tile without the ghost's fixed size");
  g_testGhost = ghost;
}

// While an app is hung, MultiWM reads its window's state from the ghost DWM shows
// for it, and the ghost's events reach the WM thread as the window's. Real
// visible test windows; the ghost pairing is faked.
static void RunGhostWindowTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hung = f.hwnd;
  const HWND ghost = f.CreateTestWindow(nullptr);
  const HWND other = f.CreateTestWindow(nullptr);
  Platform::Windows::GhostApi& api = Platform::Windows::Ghosts();
  const Platform::Windows::GhostApi saved = api;
  api = {TestGhostFromHung, TestHungFromGhost};
  g_testHung = hung;
  g_testGhost = ghost;
  using Platform::Windows::GetPhysicalSuspensionReason;
  using Platform::Windows::IsSuspendedOrCloaked;

  check(GetPhysicalSuspensionReason(hung) == SuspensionReason::None && !IsSuspendedOrCloaked(hung),
        "a hung window whose ghost is restored reads as restored");
  ShowWindow(ghost, SW_MINIMIZE);
  check(!IsIconic(hung) && GetPhysicalSuspensionReason(hung) == SuspensionReason::Minimized &&
            IsSuspendedOrCloaked(hung),
        "minimizing the ghost minimizes the hung window, which Windows itself still shows as restored");

  // The model follows: suspended with its slot kept, then tiled again (Automatic
  // mode; in Manual mode a minimize leaves the group, ghost or not).
  g_wm.managementMode = ManagementMode::Automatic;
  f.Seed(TileLayout::Columns);
  std::vector<DesktopMonitorKey> changed;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::Participation, &changed, hung);
  const WindowRecord* record = f.Load().Find(hung);
  check(record && record->state == ManageState::Suspended && record->suspensionReason == SuspensionReason::Minimized &&
            record->hasSavedSlot,
        "a hung window minimized through its ghost is suspended and keeps its slot");
  ShowWindow(ghost, SW_RESTORE);
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::Participation, &changed, hung);
  check(f.Load().IsTiled(hung), "restoring the ghost tiles the hung window again");

  CheckGhostLayoutAndCommands(check, f, other);

  // Events: a ghost's events are its hung window's; its destroy is not.
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
    return posted.size() == 1 && posted[0].message == message && posted[0].wParam == wParam &&
           posted[0].lParam == lParam;
  };
  auto stateChangeOf = [&](HWND hwnd) {
    return postedOnly(WM_APP_WINDOW_EVENT, EVENT_OBJECT_STATECHANGE, reinterpret_cast<LPARAM>(hwnd));
  };
  auto send = [](DWORD event, HWND hwnd) {
    Platform::WindowEvents::WinEventProc(nullptr, event, hwnd, OBJID_WINDOW, CHILDID_SELF, 0, 0);
  };
  HWND eventGhost = nullptr, eventWindow = nullptr;
  WinEventTestThread ghostThread([&] {
    eventGhost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                                 f.workArea.left, f.workArea.top, 400, 300,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    eventWindow = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  });
  check(eventGhost && eventWindow, "create foreign-thread windows for ghost routing");
  g_testGhost = eventGhost;
  g_wm.threadId.store(GetCurrentThreadId(), std::memory_order_release);
  drain();
  send(EVENT_OBJECT_LOCATIONCHANGE, eventGhost);
  check(drain().empty() && g_wm.ghosts.empty(), "a ghost isn't recognized from a location change alone");
  send(EVENT_SYSTEM_MINIMIZESTART, eventGhost);
  check(stateChangeOf(hung) && g_wm.ghosts.count(eventGhost) == 1,
        "minimizing a ghost has the WM thread look at its hung window again");
  send(EVENT_OBJECT_LOCATIONCHANGE, eventGhost);
  check(stateChangeOf(hung), "a known ghost's other events count for its hung window too");
  send(EVENT_SYSTEM_FOREGROUND, eventGhost);
  check(postedOnly(WM_APP_FOREGROUND_CHANGED, reinterpret_cast<WPARAM>(hung), 0),
        "a ghost in front means its hung window is");
  send(EVENT_SYSTEM_MOVESIZESTART, eventGhost);
  check(Gestures::IsMoveSizeGestureInProgress(hung) &&
            postedOnly(WM_APP_MOVE_SIZE_START, reinterpret_cast<WPARAM>(hung), reinterpret_cast<LPARAM>(eventGhost)),
        "a ghost drag starts a preview for the hung window using the displayed ghost");
  const RECT ghostStart = FloatingWindowFixture::Frame(eventGhost);
  FloatingWindowFixture::AppMove(eventGhost, ghostStart.left + 30, ghostStart.top + 20,
                                 Layout::RectWidth(ghostStart), Layout::RectHeight(ghostStart));
  send(EVENT_SYSTEM_MOVESIZEEND, eventGhost);
  check(postedOnly(WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(hung), 0) &&
            Gestures::ConsumeMoveSizeGesture(hung).intent == MoveSizeIntent::Move,
        "a ghost drag uses its displayed frame and queues the hung window's move");
  send(EVENT_OBJECT_DESTROY, eventGhost);
  check(stateChangeOf(hung) && g_wm.ghosts.empty(),
        "a ghost's destroy, when the app recovers, is a state change, never the window's destroy");
  send(EVENT_SYSTEM_MINIMIZESTART, eventWindow);
  check(drain().size() == 1 && g_wm.ghosts.empty(), "an untracked window that is no ghost is handled as before");

  // Without the user32 exports there is no ghost to follow.
  api = {};
  ShowWindow(ghost, SW_MINIMIZE);
  check(GetPhysicalSuspensionReason(hung) == SuspensionReason::None, "without the exports, the window's own state counts");

  api = saved;
  g_testHung = g_testGhost = nullptr;
  g_wm.ghosts.clear();
  drain();
  g_wm.threadId.store(0, std::memory_order_release);
  FloatingWindowFixture::ClearLeases();
  std::printf("Passed %u ghost window checks.\n", check.passed);
}

// A real window on its own thread that is too busy to answer WM_GETMINMAXINFO
// while `slow` is set, as an app just after it appears can be.
struct SlowLimitsWindow {
  std::atomic<bool> slow{true};
  HWND hwnd = nullptr;
  HANDLE thread = nullptr;
  HANDLE ready = nullptr;
  RECT frame{};

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP) {
      SetEvent(reinterpret_cast<HANDLE>(wparam));
      WaitForSingleObject(reinterpret_cast<HANDLE>(lparam), 3000);
      return 0;
    }
    auto* self = reinterpret_cast<SlowLimitsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_GETMINMAXINFO && self && self->slow) {
      Sleep(3 * Platform::Windows::kAppResponseTimeoutMs);  // Longer than the query waits.
      return 0;
    }
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  static DWORD WINAPI Thread(LPVOID parameter) {
    auto* self = static_cast<SlowLimitsWindow*>(parameter);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = Proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MultiWMSlowLimitsTest";
    RegisterClassW(&windowClass);
    self->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                 windowClass.lpszClassName, L"MultiWM slow limits test", WS_POPUP,
                                 self->frame.left, self->frame.top, 400, 300, nullptr, nullptr,
                                 windowClass.hInstance, nullptr);
    if (self->hwnd) {
      SetWindowLongPtrW(self->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      SetLayeredWindowAttributes(self->hwnd, 0, 0, LWA_ALPHA);
      ShowWindow(self->hwnd, SW_SHOWNOACTIVATE);
    }
    SetEvent(self->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
  }

  explicit SlowLimitsWindow(const RECT& workArea) {
    frame = {workArea.left + 40, workArea.top + 40, 0, 0};
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
    WaitForSingleObject(ready, 5000);
  }
  ~SlowLimitsWindow() {
    slow = false;
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (thread) WaitForSingleObject(thread, 5000);
    if (thread) CloseHandle(thread);
    if (ready) CloseHandle(ready);
  }
};

// A tiled window whose size-limit query times out is asked again after the New
// Window Retry Delay, at most kMaxConstraintRetries times, rather than waiting
// for an unrelated arrangement; the answer, once it comes, is used at once.
static void RunConstraintRetryTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  g_settings.slowApplicationDelayMs = 250;
  SlowLimitsWindow slow(f.workArea);
  check(slow.hwnd != nullptr, "create the slow window");
  auto& retries = g_wm.lifecycle.constraintRetries;
  auto retry = [&]() -> PendingConstraintRetry* {
    for (auto& entry : retries) {
      if (entry.hwnd == slow.hwnd) return &entry;
    }
    return nullptr;
  };
  auto runDueRetry = [&] {
    if (PendingConstraintRetry* entry = retry()) entry->dueTickMs = 1;
    Placement::ProcessConstraintRetryTimer();
  };
  auto limitsKnown = [&] {
    const WindowRecord* record = f.Load().Find(slow.hwnd);
    return record && record->constraints.valid;
  };

  // The gap before retry `attempt`: the delay, doubled for each retry before it.
  // Placement and the timed-out query itself add up to a few hundred ms.
  auto gapIs = [&](ULONGLONG before, unsigned attempt) {
    const ULONGLONG expected = 250ull << (attempt - 1);
    const ULONGLONG gap = retry() ? retry()->dueTickMs - before : 0;
    return gap >= expected && gap <= expected + 1000;
  };

  Workspace workspace;
  workspace.SetLayout(TileLayout::Columns);
  workspace.AdmitTiled(FloatingWindowFixture::Record(f.hwnd));
  workspace.AdmitTiled(FloatingWindowFixture::Record(slow.hwnd));
  g_workspaces.Save(f.key, workspace);
  ULONGLONG before = GetTickCount64();
  Placement::ArrangeWorkspace(f.key);
  check(!limitsKnown() && retry() && retry()->attempts == 1 && gapIs(before, 1) &&
            g_wm.timers.constraintRetry != 0,
        "a window that doesn't answer is tiled, and asked again after the retry delay");
  check(f.Load().IsTiled(slow.hwnd), "it stays tiled meanwhile");

  for (unsigned attempt = 2; attempt <= Placement::kMaxConstraintRetries; ++attempt) {
    before = GetTickCount64();
    runDueRetry();
    check(retry() && retry()->attempts == attempt && gapIs(before, attempt),
          "each silent retry schedules the next, twice as far off");
  }
  runDueRetry();
  auto exhausted = [&] {
    return retry() && retry()->attempts == Placement::kMaxConstraintRetries && !retry()->dueTickMs &&
           g_wm.timers.constraintRetry == 0;
  };
  check(exhausted() && !limitsKnown(), "retries stop after kMaxConstraintRetries");

  Placement::ArrangeWorkspace(f.key);
  check(exhausted(), "a later arrangement that still gets no answer doesn't start the retries again");
  slow.slow = false;
  // Its thread may still be asleep from an earlier query; wait until it is idle.
  DWORD_PTR ignored = 0;
  SendMessageTimeoutW(slow.hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 5000, &ignored);
  Placement::ArrangeWorkspace(f.key);
  check(limitsKnown() && !retry() && g_wm.timers.constraintRetry == 0,
        "the arrangement that gets an answer records it and ends the retries");
  check(Placement::ConformanceRetryDelayMs(PlacementResult::Success, 3) == 75 &&
            Placement::ConformanceRetryDelayMs(PlacementResult::Unresponsive, 1) == 150 &&
            Placement::ConformanceRetryDelayMs(PlacementResult::Unresponsive, 9) == 75 * 16,
        "repairs of a window that doesn't answer back off, up to 16 times the interval");

  g_settings.slowApplicationDelayMs = 0;
  slow.slow = true;
  {
    Workspace current = f.Load();
    current.UpdateConstraints(slow.hwnd, WindowConstraints{});
    g_workspaces.Save(f.key, current);
  }
  Placement::ArrangeWorkspace(f.key);
  check(!retry(), "a New Window Retry Delay of 0 disables the retry");

  Reconcile::ForgetManagedWindow(slow.hwnd);
  FloatingWindowFixture::ClearLeases();
  std::printf("Passed %u size-limit retry checks.\n", check.passed);
}

static void RunUnresponsivePlacementTest() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  // The wait is kAppResponseTimeoutMs whatever Window Change Delay says.
  g_settings.reconcileDelayMs = 2000;
  SlowLimitsWindow paused(f.workArea);
  check(paused.hwnd != nullptr, "create the window to pause");
  paused.slow = false;
  f.Seed(TileLayout::Columns);
  Workspace before = f.Load();
  before.AdmitTiled(FloatingWindowFixture::Record(paused.hwnd));
  g_workspaces.Save(f.key, before);
  Placement::ArrangeWorkspace(f.key);
  before = f.Load();
  Workspace candidate = before;
  candidate.SetLayout(TileLayout::Rows);
  Placement::RememberPendingLayoutMutation(f.key, before, candidate, f.hwnd,
                                           Placement::UserLayoutMutationKind::Resize, L"Resize");
  g_workspaces.Save(f.key, candidate);

  const HANDLE pausedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const HANDLE resumeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(pausedEvent && resumeEvent &&
            PostMessageW(paused.hwnd, WM_APP, reinterpret_cast<WPARAM>(pausedEvent),
                         reinterpret_cast<LPARAM>(resumeEvent)) &&
            WaitForSingleObject(pausedEvent, 3000) == WAIT_OBJECT_0 &&
            !IsHungAppWindow(paused.hwnd),
        "the window has stopped pumping before Windows marks it hung");
  const ULONGLONG start = GetTickCount64();
  Placement::ArrangeWorkspace(f.key);
  const ULONGLONG elapsed = GetTickCount64() - start;
  // GetTickCount64 advances in steps of about 16 ms.
  check(elapsed + 16 >= Platform::Windows::kAppResponseTimeoutMs && elapsed < 1000,
        "retile waits the app response timeout, not Window Change Delay, and returns while the app is paused");
  const Workspace deferred = f.Load();
  check(deferred.Find(paused.hwnd)->lastPlacementResult == PlacementResult::Unresponsive &&
            deferred.Find(f.hwnd)->lastPlacementResult == PlacementResult::Success,
        "retile defers the unresponsive window and places its responsive sibling");
  for (auto& [window, lease] : TestLeases()) lease.expiresAtTickMs = 1;
  Placement::ProcessConformanceTimer();
  const Workspace settled = f.Load();
  check(settled.Layout() == TileLayout::Rows && settled.IsTiled(paused.hwnd) &&
            !Placement::g_pendingProofs.FindLayoutMutation(f.key),
        "the deadline keeps the resize and the unresponsive window's slot");

  SetEvent(resumeEvent);
  DWORD_PTR ignored = 0;
  check(SendMessageTimeoutW(paused.hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 3000, &ignored) != 0,
        "the paused app resumes message processing");
  CloseHandle(resumeEvent);
  CloseHandle(pausedEvent);
  Reconcile::ProcessWindowLifecycleEvent(EVENT_OBJECT_STATECHANGE, paused.hwnd);
  const Workspace recovered = f.Load();
  check(recovered.Find(paused.hwnd)->lastPlacementResult == PlacementResult::Success &&
            Layout::RectsNear(FloatingWindowFixture::Frame(paused.hwnd),
                              recovered.Find(paused.hwnd)->lastRequestedRect),
        "a recovery state event places the resumed window on its tile");

  Reconcile::ForgetManagedWindow(paused.hwnd);
  std::printf("Passed %u unresponsive placement checks.\n", check.passed);
}

// Local floats are reported with their reason. Routing only reports after the
// live window ends up floating.
static void RunCommandReleaseTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  g_wm.managementMode = ManagementMode::Automatic;
  const bool savedToasts = TrayUi::g_preferences.showToasts;
  TrayUi::g_preferences.showToasts = false;
  std::vector<SpaceFloat> localFloats;
  auto release = [&](SpaceFloatReason reason, TileLayout layout = TileLayout::Columns) {
    f.Seed(layout);
    Workspace workspace = f.Load();
    localFloats.clear();
    const bool floated = Placement::FloatAutomaticOverflowWindow(f.key, workspace, hwnd, reason, localFloats);
    g_workspaces.Save(f.key, workspace);
    Placement::ReportSpaceFloats(localFloats, f.key.ResolveMonitor());
    return floated;
  };
  check(g_wm.commandReleases == nullptr, "nothing collects releases outside a command");
  check(release(SpaceFloatReason::CannotFit) && localFloats.size() == 1,
        "an automatic release outside a command reports a local float");
  // Automatic overflow floats quietly outside a command, except a BSP window
  // that can't fit; the auto-tiling limit stays quiet in BSP too.
  check(!localFloats[0].announce, "outside BSP, an automatic float isn't toasted");
  check(release(SpaceFloatReason::CannotFit, TileLayout::Bsp) && localFloats.size() == 1 && localFloats[0].announce,
        "a BSP window that can't fit is toasted");
  check(release(SpaceFloatReason::CountLimit, TileLayout::Bsp) && localFloats.size() == 1 && !localFloats[0].announce,
        "a BSP window at the auto-tiling limit isn't");
  {
    Commands::CommandReleaseScope scope;
    check(release(SpaceFloatReason::CannotFit) && release(SpaceFloatReason::CountLimit),
          "release the window twice inside a command");
    const auto& released = scope.Released();
    check(released.size() == 2 && released[0].window == hwnd &&
              released[0].reason == SpaceFloatReason::CannotFit &&
              released[1].reason == SpaceFloatReason::CountLimit,
          "a command hears each release and its reason");
  }
  check(g_wm.commandReleases == nullptr, "the command's scope stops collecting when it ends");
  check(Placement::SpaceFloatMessage({{nullptr, SpaceFloatReason::CannotFit}}) ==
            L"A window floated because it needs more space",
        "an unnamed constraint float still gets a message");
  check(Placement::SpaceFloatMessage({{nullptr, SpaceFloatReason::CountLimit}, {nullptr, SpaceFloatReason::CannotFit}}) ==
            L"2 windows floated due to space or the auto-tiling limit",
        "one summary covers both release reasons");

  f.Seed(TileLayout::Columns);
  Workspace crowded = f.Load();
  for (int i = 0; i < 2; ++i) {
    const HWND window = f.CreateTestWindow(nullptr);
    check(crowded.AdmitTiled(FloatingWindowFixture::Record(window)), "admit another window before enforcing the limit");
  }
  g_workspaces.Save(f.key, crowded);
  g_settings.automaticWindowLimit = 1;
  {
    Commands::CommandReleaseScope scope;
    Placement::ArrangeWorkspace(f.key);
    check(scope.Released().size() == 2 &&
              Placement::SpaceFloatMessage(scope.Released()) == L"2 windows floated at the auto-tiling limit",
          "one arrangement reports both local count-limit floats together");
    Placement::ArrangeWorkspace(f.key);
    check(scope.Released().size() == 2, "ordinary reflows do not report already-floating windows again");
  }
  g_settings.overflowCreateDesktop = true;
  g_settings.overflowTryOtherMonitors = false;
  g_settings.automaticWindowLimit = 1;
  auto& hideDestroyHook = Platform::WindowEvents::g_hooks.hideDestroy;
  const HWINEVENTHOOK savedHideDestroyHook = hideDestroyHook;
  hideDestroyHook = reinterpret_cast<HWINEVENTHOOK>(1);
  f.Seed(TileLayout::Bsp);
  auto snapshot = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::BuildBody(f.key.desktopId, snapshot.get());
  check(snapshot->workspaceCount == 1 && snapshot->bspLocalFloats == 1 &&
            snapshot->workspaces[0].newWindowFloats && !(snapshot->flags & CompanionAbi::kFlagOverflowCentersFloat),
        "the companion predicts a local float for a BSP count limit while other layouts can create desktops");
  for (const auto reason : {SpaceFloatReason::CountLimit, SpaceFloatReason::CannotFit}) {
    FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 20, f.workArea.top + 20, 400, 300);
    check(release(reason, TileLayout::Bsp) && !g_wm.HasPendingOverflow(hwnd) &&
              IsCenteredIn(FloatingWindowFixture::Frame(hwnd), f.center),
          "BSP overflow floats in place when the only other option is a new desktop");
  }
  {
    Commands::CommandReleaseScope scope;
    check(release(SpaceFloatReason::CountLimit) && g_wm.HasPendingOverflow(hwnd) &&
              localFloats.empty() && scope.Released().empty(),
          "a routed overflow defers the notification even inside a command");
  }
  const auto queued = std::find_if(g_wm.overflow.queue.begin(), g_wm.overflow.queue.end(),
                                  [&](const PendingWindowOverflow& request) { return request.hwnd == hwnd; });
  check(queued != g_wm.overflow.queue.end() && queued->reason == SpaceFloatReason::CountLimit,
        "deferred overflow carries its release reason");
  const PendingWindowOverflow request = *queued;
  using Reconcile::OverflowOutcome;
  check(!Reconcile::FinishedOverflowFloatMonitor(request, {OverflowOutcome::Retry}) &&
            !Reconcile::FinishedOverflowFloatMonitor(request, {OverflowOutcome::Placed}),
        "incomplete routing and a successfully placed tile produce no float toast");
  check(Reconcile::FinishedOverflowFloatMonitor(request, {OverflowOutcome::NoDestination}) == f.key.ResolveMonitor(),
        "a routed window still floating when no destination fits can be reported");
  f.Seed(TileLayout::Columns);
  check(!Reconcile::FinishedOverflowFloatMonitor(request, {OverflowOutcome::Failed}),
        "a late routing failure cannot announce a window that has since been tiled");
  g_wm.CancelPendingOverflow(hwnd);

  if (GetSystemMetrics(SM_CMONITORS) != 1) {
    g_settings.overflowTryOtherMonitors = true;
    check(release(SpaceFloatReason::CountLimit, TileLayout::Bsp), "BSP can overflow to another monitor");
    const auto queued = std::find_if(g_wm.overflow.queue.begin(), g_wm.overflow.queue.end(),
                                    [&](const PendingWindowOverflow& request) { return request.hwnd == hwnd; });
    check(queued != g_wm.overflow.queue.end() && !queued->allowNewDesktop,
          "the queued BSP overflow cannot create a new desktop after rebasing");
    g_wm.CancelPendingOverflow(hwnd);
  }
  RuntimeLifecycle::CancelActorTimer(g_wm.timers.lifecycle);
  hideDestroyHook = savedHideDestroyHook;
  TrayUi::g_preferences.showToasts = savedToasts;
  std::printf("Passed %u command release checks.\n", check.passed);
}

// A window a floating rule admits is centered at its own size, unless an
// override-size rule names another size (Automatic mode only).
static void RunRuleFloatedSizeTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  f.Seed(TileLayout::Columns);
  g_wm.managementMode = ManagementMode::Automatic;
  auto placeRuleFloated = [&] {
    FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 10, f.workArea.top + 10, 400, 300);
    check(f.Place(hwnd, Placement::FloatingPlacementIntent::RuleFloatedCenter), "place the rule-floated window");
    const RECT frame = FloatingWindowFixture::Frame(hwnd);
    check(IsCenteredIn(frame, f.center), "a rule-floated window is centered in the work area");
    return frame;
  };

  RECT placed = placeRuleFloated();
  check(Layout::RectWidth(placed) == 400 && Layout::RectHeight(placed) == 300,
        "without a size rule it keeps its own size");
  WindowRule sizeRule;
  sizeRule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
  sizeRule.classNames = {L"Static"};
  sizeRule.preserveFloatingSize = false;
  sizeRule.floatingSizeDip = {489, 384};
  g_settings.windowRules = {sizeRule};
  placed = placeRuleFloated();
  check(Layout::RectsNear(placed, f.Centered(489, 384), 1), "an override-size rule sizes it");
  g_settings.windowRules[0].preserveFloatingSize = true;
  placed = placeRuleFloated();
  check(Layout::RectWidth(placed) == 400 && Layout::RectHeight(placed) == 300,
        "a preserve-size rule keeps its own size");
  std::printf("Passed %u rule-floated size checks.\n", check.passed);
}

// A hidden member of a Floating-layout workspace (a tray app) shown again on
// another desktop moves to that desktop's workspace before it is restored. It
// must never be restored at its old floating frame first: that was a visible
// jump before its new tile. Until Windows assigns it a desktop (GUID_NULL) it
// waits. Shown again on its own desktop, it returns to its floating frame.
static void RunHiddenArrivalTests() {
  CheckCounter check;
  FloatingWindowFixture f(check);
  const HWND hwnd = f.hwnd;
  g_wm.managementMode = ManagementMode::Automatic;
  f.Seed(TileLayout::Floating);
  check(f.Place(hwnd, Placement::FloatingPlacementIntent::CenterWithFloatingSize), "float it in the Floating layout");
  const RECT floatingFrame = FloatingWindowFixture::Frame(hwnd);
  Workspace source = f.Load();
  check(source.Suspend(hwnd, SuspensionReason::Hidden), "hide it to the tray");
  g_workspaces.Save(f.key, source);
  // The companion has already moved it onto its new tile while it was hidden.
  FloatingWindowFixture::AppMove(hwnd, f.workArea.left + 20, f.workArea.top + 20, 400, 300);
  const RECT shownFrame = FloatingWindowFixture::Frame(hwnd);

  auto stateIn = [&](const DesktopMonitorKey& key) {
    Workspace workspace;
    const WindowRecord* record = g_workspaces.Load(key, &workspace) ? workspace.Find(hwnd) : nullptr;
    return record ? std::optional<ManageState>(record->state) : std::nullopt;
  };
  auto staysPut = [&] {
    return Layout::RectsNear(FloatingWindowFixture::Frame(hwnd), shownFrame, 0);
  };
  const auto repairs = [] { return Diagnostics::g_runtime.counters.floatingGeometryRepairs; };
  const auto repairsBefore = repairs();
  std::vector<DesktopMonitorKey> changed;

  // Shown before Windows assigned its desktop: nothing to restore it into yet.
  f.SetWindowDesktop(GUID_NULL);
  g_wm.lifecycle.dirtyWindows.clear();
  Reconcile::ReconcileManagedWindowStateNow(hwnd, Reconcile::ReconcileScope::Participation |
                                                      Reconcile::ReconcileScope::Monitor);
  Reconcile::ReconcileSettledKnownWindows({hwnd}, false, &changed);
  check(stateIn(f.key) == ManageState::Suspended && g_workspaces.OwnersOf(hwnd).size() == 1,
        "without a desktop it is neither restored nor moved to a desktop-less workspace");
  check(Model::ContainsWindow(g_wm.lifecycle.dirtyWindows, hwnd), "the next settled pass looks again");
  check(g_wm.timers.lifecycle != 0, "and that pass is scheduled, not left for an unrelated event");
  for (unsigned i = 0; i < Reconcile::kMaxDesktopOwnershipWaits; ++i) {
    RuntimeLifecycle::CancelActorTimer(g_wm.timers.lifecycle);
    Reconcile::ReconcileSettledKnownWindows({hwnd}, false, &changed);
  }
  check(g_wm.timers.lifecycle == 0 && Model::ContainsWindow(g_wm.lifecycle.dirtyWindows, hwnd) &&
            stateIn(f.key) == ManageState::Suspended,
        "a window that never reports a desktop stops scheduling passes, and still waits");
  check(staysPut() && repairs() == repairsBefore, "it isn't placed at its old floating frame");

  // Assigned to the current desktop, not its owner's. The immediate pass waits;
  // the settled pass moves it there first and restores it as a tile.
  DesktopMonitorKey arrival = f.key;
  arrival.desktopId.Data1 = 0xA7717;
  Workspace destination;
  destination.SetLayout(TileLayout::Dwindle);
  g_workspaces.Save(arrival, destination);
  f.SetWindowDesktop(arrival.desktopId);
  Reconcile::ReconcileManagedWindowStateNow(hwnd, Reconcile::ReconcileScope::Participation |
                                                      Reconcile::ReconcileScope::Monitor);
  check(stateIn(f.key) == ManageState::Suspended && staysPut() && repairs() == repairsBefore,
        "the immediate pass leaves it hidden in its old workspace");
  const std::vector<HWND> dirty{hwnd};
  g_wm.lifecycle.dirtyWindows.clear();
  Reconcile::ReconcileSettledKnownWindows(dirty, false, &changed);
  check(!stateIn(f.key) && stateIn(arrival) == ManageState::Tiled,
        "the settled pass moves it to the current desktop's workspace, restored as a tile");
  check(staysPut() && repairs() == repairsBefore, "it is never placed at its old floating frame");

  // Hidden again and shown on its own desktop: back to its floating frame.
  Reconcile::ForgetManagedWindow(hwnd);
  f.SetWindowDesktop(f.key.desktopId);
  g_workspaces.Save(f.key, source);
  Reconcile::ReconcileSettledKnownWindows(dirty, false, &changed);
  check(stateIn(f.key) == ManageState::Tiled && repairs() == repairsBefore + 1 &&
            Layout::RectsNear(FloatingWindowFixture::Frame(hwnd), floatingFrame, 0),
        "shown on its own desktop it returns to its floating frame");
  g_workspaces.Save(arrival, Workspace{});
  g_wm.lifecycle.dirtyWindows.clear();
  std::printf("Passed %u hidden arrival checks.\n", check.passed);
}

// Constraint recovery for settled windows and new or returning members.
// Fake HWNDs only; toasts disabled; no native window is moved.
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
  std::vector<SpaceFloat> localFloats;
  auto recover = [&](Workspace& workspace) {
    std::vector<RECT> rects;
    Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects);
    g_workspaces.Save(key, workspace);
    Placement::ArrangementContext context{key, monitor, metrics, nullptr};
    const auto result = Placement::RecoverUnsatisfiableLayout(workspace, context, rects);
    localFloats = std::move(context.localFloats);
    return result;
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
    check(localFloats.size() == 1 && localFloats[0].window == a && localFloats[0].reason == SpaceFloatReason::CannotFit,
          "Manual constraint recovery reports the window it actually floated");
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

  {
    // Three windows fit. Restoring d cuts the last slot in half, so c no longer
    // fits at any insertion point that keeps a, b, c in order.
    Workspace workspace = makeWorkspace(TileLayout::Dwindle, {{a, 1}, {b, 400}, {c, 400}, {d, 1}});
    check(workspace.Suspend(d, SuspensionReason::Minimized) &&
              plan(workspace) == Layout::LayoutPlanStatus::Success, "Dwindle fits with d minimized");
    check(workspace.Restore(d) && plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable,
          "restoring d at its saved slot makes the layout too narrow for c");
    Workspace shuffled = workspace;
    check(shuffled.SwapTiled(a, c) && plan(shuffled) == Layout::LayoutPlanStatus::Success,
          "swapping established windows would make room for d");
    check(recover(workspace) == Placement::ArrangementStep::Replan &&
              workspace.Find(d) && workspace.Find(d)->state == ManageState::Floating,
          "Dwindle floats d when it cannot be restored in order");
    check(workspace.TiledWindows() == std::vector<HWND>({a, b, c}) &&
              plan(workspace) == Layout::LayoutPlanStatus::Success,
          "existing Dwindle order still fits");
  }
  {
    // d needs one of the first two tiles. Inserting it before b keeps a, b, c
    // in order and makes enough room for all four.
    Workspace workspace = makeWorkspace(TileLayout::Dwindle, {{a, 400}, {b, 1}, {c, 1}, {d, 400}});
    check(workspace.Suspend(d, SuspensionReason::Minimized) && workspace.Restore(d) &&
              plan(workspace) == Layout::LayoutPlanStatus::Unsatisfiable,
          "d cannot fit at its saved slot");
    check(recover(workspace) == Placement::ArrangementStep::Replan &&
              workspace.TiledWindows() == std::vector<HWND>({a, d, b, c}) &&
              plan(workspace) == Layout::LayoutPlanStatus::Success,
          "Dwindle restores d at an alternate insertion point");
  }
  {
    // BSP tries the configured tile (focused or largest) in both directions,
    // then the largest other tile once, then floats the arrival.
    g_wm.managementMode = ManagementMode::Automatic;
    g_settings.overflowCreateDesktop = true;
    auto& hideDestroyHook = Platform::WindowEvents::g_hooks.hideDestroy;
    const HWINEVENTHOOK savedHideDestroyHook = hideDestroyHook;
    hideDestroyHook = reinterpret_cast<HWINEVENTHOOK>(1);
    check(Placement::AutomaticOverflowRoutes(), "overflow routing is enabled");
    for (const bool splitFocused : {true, false}) {
      g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;
      Workspace established = makeWorkspace(TileLayout::Bsp, {{a, 1}, {b, 1}, {c, 1}});
      check(established.RememberFocusedWindow(a), "focus a smaller BSP tile");
      if (!splitFocused) established.UpdateConstraints(b, {350, 750, 96, true});
      check(plan(established) == Layout::LayoutPlanStatus::Success, "established BSP windows fit");

      WindowRecord newcomer;
      newcomer.hwnd = d;
      newcomer.pid = 901;
      newcomer.canMove = false;
      newcomer.constraints = {300, splitFocused ? 300 : 250, 96, true};
      if (splitFocused) g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;

      Workspace suspended = established;
      check(suspended.AdmitInitial(newcomer, SuspensionReason::Minimized), "reserve a BSP restore");
      const auto restoreHint = Placement::PredictRestoredFrame(suspended, d, metrics);

      Workspace workspace = established;
      if (splitFocused) {
        check(workspace.AdmitTiled(newcomer), "admit to the focused BSP tile");
      } else {
        Workspace away = makeWorkspace(TileLayout::Bsp, {{d, 1}});
        away.UpdateConstraints(d, newcomer.constraints);
        WindowRecord returning;
        check(away.ExtractForMigration(d, &returning), "extract a tiled window returning from another workspace");
        workspace.AdmitMigrated(std::move(returning));
      }
      std::vector<RECT> rects;
      check(Layout::PlanOrTurnNewBspSplit(workspace, metrics.workArea, metrics.gap, &rects) ==
                Layout::LayoutPlanStatus::Unsatisfiable, "neither direction of the selected BSP split fits");
      if (!splitFocused) {
        // The first try was already the largest tile, so there is no fallback.
        check(recover(workspace) == Placement::ArrangementStep::Replan && !tiled(workspace, d) &&
                  localFloats.size() == 1 && localFloats[0].reason == SpaceFloatReason::NoBspTileFits,
              "with the largest tile already tried, BSP floats the arrival");
        check(!restoreHint, "the companion predicts no tile for a restore that will float");
      } else {
        check(recover(workspace) == Placement::ArrangementStep::Replan && tiled(workspace, d) && localFloats.empty(),
              "BSP falls back to the largest tile and keeps the arrival tiled");
        check(Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects) ==
                  Layout::LayoutPlanStatus::Success &&
                  restoreHint && EqualRect(&*restoreHint, &rects[workspace.TiledIndex(d)]),
              "the companion predicts the same fallback tile on restore");
        const auto& tree = workspace.Bsp();
        const size_t divider = tree.Parent(tree.FindLeaf(d));
        check(divider == tree.Parent(tree.FindLeaf(b)) && tree.Nodes()[divider].ratio == 0.5,
              "the fallback splits the largest tile, the right half");
        check(workspace.Suspend(d, SuspensionReason::Minimized) &&
                  Placement::RestorePendingAdmissionRecoveryAfterSuspension(key, d, &workspace) &&
                  workspace.Bsp() == established.Bsp(),
              "suspending an unsettled BSP arrival restores the established tree");
      }

      newcomer.constraints = {900, 900, 96, true};
      workspace = established;
      check(workspace.AdmitTiled(newcomer), "admit a BSP window that can't fit any split");
      check(recover(workspace) == Placement::ArrangementStep::Replan &&
                localFloats.size() == 1 && localFloats[0].window == d &&
                localFloats[0].reason == SpaceFloatReason::NoBspTileFits &&
                Placement::SpaceFloatMessage(localFloats) == L"No BSP tile has enough space. Window floated.",
            "a BSP admission reports a single toast after every split fails");
      const WindowRecord* rejected = workspace.Find(d);
      check(rejected && rejected->state == ManageState::Floating && !g_wm.overflow.windows.contains(d) &&
                workspace.Bsp() == established.Bsp(),
            "an arrival with no fitting tile floats in place");
    }
    g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;
    Workspace focused = makeWorkspace(TileLayout::Bsp, {{a, 1}, {b, 1}, {c, 1}});
    focused.RememberFocusedWindow(a);
    focused.UpdateConstraints(a, {500, 500, 96, true});
    const auto admissionHint = Placement::PredictAutomaticAdmission(focused, metrics, false, true);
    check(admissionHint.tiledFrame && !admissionHint.newWindowFloats,
          "the companion predicts a different BSP tile when the focused tile is too small to split");
    hideDestroyHook = savedHideDestroyHook;
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
  // Fixed landscape and portrait geometries keep the digest display-independent.
  Platform::Monitors::WorkspaceMetrics metrics;
  metrics.gap = 12;
  metrics.dpi = 96;
  DesktopMonitorKey key;
  GUID desktop{};
  desktop.Data1 = 0xC0457;
  if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &key)) std::exit(1);
  const TileLayout layouts[] = {TileLayout::MasterStack, TileLayout::MasterStackH, TileLayout::Columns,
                                TileLayout::Rows, TileLayout::Dwindle, TileLayout::Monocle};

  size_t exercised = 0, replans = 0;
  for (int iteration = 0; iteration < 3000; ++iteration) {
    metrics.workArea = iteration % 2 == 0 ? RECT{0, 0, 1920, 1040} : RECT{0, 0, 1040, 1920};
    const LONG width = Layout::RectWidth(metrics.workArea);
    const LONG height = Layout::RectHeight(metrics.workArea);
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
    mix(!context.localFloats.empty() || !context.unresolvedConstraint.empty());
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
  // Dwindle starts stacked on portrait work areas.
  // Includes local space-float notices in both management modes.
  if (digest != 0xa125dd71df6ddffbull) {
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
  // Includes the adaptive split settings measured from saved dividers.
  // Intentional report format changes must update this value.
  if (digest != 0x9b009582af3d245cull) {
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

// A worker that doesn't stop in time must not leave MultiWM stopped: the
// restart waits for it to exit, and an uninit in between cancels the restart.
// The worker is a test-owned thread that ignores WM_QUIT until released, so no
// hotkeys, tray icon or hooks are involved.
namespace DeferredRestartFixture {
HANDLE g_release = nullptr;
HANDLE g_finished = nullptr;
LONG g_finishCalls = 0;

DWORD WINAPI StuckWorker(LPVOID ready) {
  MSG msg{};
  PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // A queue, so WM_QUIT posts.
  SetEvent(static_cast<HANDLE>(ready));
  WaitForSingleObject(g_release, INFINITE);
  return 0;
}

void Finish() {
  InterlockedIncrement(&g_finishCalls);
  SetEvent(g_finished);
}

// Installs a stuck worker as the WM thread, as StartHotkeyThread would.
void StartStuckWorker() {
  ResetEvent(g_release);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD id = 0;
  g_wm.thread = CreateThread(nullptr, 0, StuckWorker, ready, 0, &id);
  WaitForSingleObject(ready, INFINITE);
  CloseHandle(ready);
  g_wm.threadId.store(id, std::memory_order_release);
}
}  // namespace DeferredRestartFixture

static void RunDeferredRestartTests() {
  using namespace DeferredRestartFixture;
  using namespace RuntimeLifecycle;
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  check(!g_wm.thread, "no WM worker is running in the tests");
  const DWORD savedTimeout = g_stopTimeoutMs;
  g_stopTimeoutMs = 20;
  g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  // A reload that times out finishes once the worker exits.
  StartStuckWorker();
  {
    const std::scoped_lock lock(g_lifecycle.mutex);
    check(!StopHotkeyThread(), "a stuck worker times out");
    check(DeferUntilWorkerExits(Finish), "the restart waits for the worker");
    check(!DeferUntilWorkerExits(Finish), "only one restart waits at a time");
  }
  check(g_finishCalls == 0, "nothing restarts while the worker is alive");
  SetEvent(g_release);
  check(WaitForSingleObject(g_finished, 5000) == WAIT_OBJECT_0, "the restart runs once the worker exits");
  {
    const std::scoped_lock lock(g_lifecycle.mutex);  // The callback has returned.
    check(g_finishCalls == 1 && !g_wm.thread && !g_lifecycle.deferredWait,
          "the restart joined the old worker and ran once");
  }

  // Uninit cancels a waiting restart: nothing starts after the worker exits.
  StartStuckWorker();
  {
    const std::scoped_lock lock(g_lifecycle.mutex);
    check(!StopHotkeyThread() && DeferUntilWorkerExits(Finish), "a second restart waits");
  }
  CancelDeferredRestart();
  check(!g_lifecycle.deferredWait, "cancelling drops the waiting restart");
  SetEvent(g_release);
  check(WaitForSingleObject(g_wm.thread, 5000) == WAIT_OBJECT_0, "the second worker exits");
  check(WaitForSingleObject(g_finished, 0) == WAIT_OBJECT_0 && g_finishCalls == 1,
        "a cancelled restart never runs");
  check(StopHotkeyThread() && !g_wm.thread, "uninit then joins the exited worker");

  CloseHandle(g_release);
  CloseHandle(g_finished);
  g_stopTimeoutMs = savedTimeout;
  std::printf("Passed %u deferred restart checks.\n", checks);
}

// A setting problem whose text doesn't fit the toast line is cut off, never
// dropped, and a quoted user value is shortened so the reason stays visible.
static void RunSettingProblemTextTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const auto saved = g_settings.problems;
  g_settings.problems.clear();
  const std::wstring longValue(400, L'x');
  Configuration::NoteSettingProblem(L"%ls", longValue.c_str());
  check(g_settings.problems.size() == 1 && g_settings.problems.back().size() == 159,
        "a problem longer than the line is cut off, not dropped");
  const std::wstring quoted = Configuration::ShortenedSettingValue(longValue.c_str());
  check(quoted.size() == 24 && quoted.back() == L'…', "a long value is shortened with an ellipsis");
  check(Configuration::ShortenedSettingValue(L"ctrl+q") == L"ctrl+q", "a short value is quoted whole");
  Configuration::NoteSettingProblem(L"%ls hotkey off: \"%ls\" isn't a key", L"Float Focused Window", quoted.c_str());
  check(g_settings.problems.back().ends_with(L"isn't a key"), "the reason survives a long value");
  g_settings.problems = saved;
  std::printf("Passed %u setting problem text checks.\n", checks);
}

// The companion section is read in every process, elevated ones included, so a
// Low-integrity process of the same user must be able to read it but never
// write it or change its DACL, also when MultiWM reopens a section an older
// build created with a Low label. Uses a test-owned name, never the live one.
static void RunCompanionObjectSecurityTests() {
  unsigned checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const std::wstring name = L"Local\\MultiWM.Tests.CompanionSecurity." + std::to_wstring(GetCurrentProcessId());
  auto createWith = [&](const wchar_t* sddl) {
    return CompanionSnapshot::CreateNamedObject(sddl, [&](SECURITY_ATTRIBUTES* attributes) {
      return CreateFileMappingW(INVALID_HANDLE_VALUE, attributes, PAGE_READWRITE, 0, 4096, name.c_str());
    });
  };
  // What a Low-integrity token of this user can open the section with.
  struct LowAccess { bool read = false; bool write = false; bool writeDac = false; };
  auto probeAsLow = [&] {
    LowAccess access;
    HANDLE self = nullptr;
    HANDLE low = nullptr;
    PSID lowSid = nullptr;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self) &&
              DuplicateTokenEx(self, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenImpersonation, &low) &&
              ConvertStringSidToSidW(L"S-1-16-4096", &lowSid),
          "make a Low-integrity token");
    TOKEN_MANDATORY_LABEL label{{lowSid, SE_GROUP_INTEGRITY}};
    check(SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(lowSid)) &&
              ImpersonateLoggedOnUser(low),
          "impersonate the Low-integrity token");
    for (auto [rights, result] : {std::pair{DWORD{FILE_MAP_READ}, &access.read},
                                  std::pair{DWORD{FILE_MAP_WRITE}, &access.write},
                                  std::pair{DWORD{WRITE_DAC}, &access.writeDac}}) {
      if (HANDLE opened = OpenFileMappingW(rights, FALSE, name.c_str())) {
        *result = true;
        CloseHandle(opened);
      }
    }
    RevertToSelf();
    LocalFree(lowSid);
    CloseHandle(low);
    CloseHandle(self);
    return access;
  };

  HANDLE fresh = createWith(CompanionAbi::kSectionSddl);
  check(fresh != nullptr, "create the section");
  LowAccess access = probeAsLow();
  check(access.read, "Low-integrity readers can read a new section");
  check(!access.write && !access.writeDac, "Low-integrity processes can't write or re-permission a new section");
  CloseHandle(fresh);

  // An older build's descriptor, still held open by a reader.
  constexpr wchar_t kLowLabeledSddl[] =
      L"D:(A;;GA;;;OW)(A;;GA;;;SY)(A;;0x0005;;;WD)(A;;0x0005;;;AC)S:(ML;;NW;;;LW)";
  HANDLE older = createWith(kLowLabeledSddl);
  check(older != nullptr && probeAsLow().write, "the old Low label let Low-integrity processes write");
  HANDLE reopened = createWith(CompanionAbi::kSectionSddl);
  check(reopened != nullptr && GetLastError() == ERROR_ALREADY_EXISTS, "reopen the existing section");
  access = probeAsLow();
  check(access.read, "Low-integrity readers can still read a reopened section");
  check(!access.write && !access.writeDac, "reopening removes the old label's write access");
  CloseHandle(reopened);
  CloseHandle(older);

  // Only a whole, committed, writable section takes the header. A reserved
  // section maps writable but would fault on the first write.
  auto viewIsWritable = [&](DWORD protect, DWORD size) {
    const std::wstring sectionName = name + L".Usable";
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect, 0, size, sectionName.c_str());
    check(section != nullptr, "create a section to map");
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
    const bool writable = CompanionSnapshot::IsWritableSnapshotView(view);
    if (view) UnmapViewOfFile(view);
    CloseHandle(section);
    return writable;
  };
  constexpr DWORD kSnapshotSize = sizeof(CompanionAbi::SnapshotSection);
  check(viewIsWritable(PAGE_READWRITE, kSnapshotSize), "a committed section takes the header");
  check(!viewIsWritable(PAGE_READWRITE | SEC_RESERVE, kSnapshotSize), "a reserved section is refused");
  check(!viewIsWritable(PAGE_READWRITE, 4096), "a smaller section is refused");
  std::printf("Passed %u companion object security checks.\n", checks);
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
      const auto hint = CompanionSnapshot::MakeTrackedWindowHint(record, GUID{0xC001}, RECT{0, 0, 1920, 1080});
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
      const auto hint = CompanionSnapshot::MakeTrackedWindowHint(record, GUID{0xC001}, RECT{0, 0, 1920, 1080});
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
    check(CompanionSnapshot::MakeTrackedWindowHint(record, GUID{0xC001}, RECT{0, 0, 1920, 1080}).marginsDpi == 0,
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

// A host's work-area-sized surface is released once the host has been hidden
// for a while, never while it shows something, and the next toast re-attaches
// it. Overlays run on this thread (no overlay thread is started).
static void RunOverlaySurfaceReleaseTests() {
  CheckCounter check;
  const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HMONITOR monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
  RECT workArea{};
  check(Platform::Monitors::GetOverlayWorkArea(monitor, &workArea), "read the overlay work area");
  auto toast = [&] {
    Overlay::ShowToast(Overlay::ToastRequest{monitor, L"Surface release test", TrayUi::CurrentPopupAppearance(), 0, 0, false});
    return &Overlay::GetMonitorHost(monitor, workArea);
  };
  auto idleTimerFires = [](Overlay::MonitorHost& host) {
    SendMessageW(host.window, WM_TIMER, Overlay::kSurfaceReleaseTimer, 0);
  };

  Overlay::MonitorHost* host = toast();
  check(host->visible && host->surface.target, "a toast shows on an attached surface");
  idleTimerFires(*host);
  check(host->visible && host->surface.target, "a showing host keeps its surface");
  Overlay::ClearToast(*host);
  Overlay::DrawHost(*host);
  check(!host->visible && host->surface.target, "hiding keeps the surface for a while");
  idleTimerFires(*host);
  check(!host->surface.target && host->window, "an idle host releases its surface and keeps its window");
  host = toast();
  check(host->visible && host->surface.target, "the next toast re-attaches a surface");
  Overlay::Shutdown();
  SetThreadDpiAwarenessContext(previousDpi);
  std::printf("Passed %u overlay surface release checks.\n", check.passed);
}

int main() {
  // This thread goes long stretches without pumping messages, so Windows would
  // ghost its test windows as hung, and MultiWM never places a ghosted window.
  // The ghost tests install their own ghost lookups instead.
  DisableProcessWindowsGhosting();
  RunFrameMarginCacheTests();
  RunConstraintProbeTests();
  RunComRefOwnershipTests();
  RunCompanionObjectSecurityTests();
  RunSettingProblemTextTests();
  RunDeferredRestartTests();
  RunDesktopReconciliationTests();
  RunDiscoveryResultTests();
  RunWinEventRoutingTests();
  RunConformanceLocationTests();
  RunDiagnosticReportShapeTest();
  // After the report test: these record events the report lists.
  RunFloatingLeaseTests();
  RunFloatingSizeCommandTests();
  RunRuleFloatedSizeTests();
  RunFirstFloatingSizeTests();
  RunHiddenArrivalTests();
  RunOverflowDestinationProbeTests();
  RunOverflowDestinationFitTests();
  RunCommandReleaseTests();
  RunConstraintRetryTests();
  RunUnresponsivePlacementTest();
  RunBspDragTests();
  RunTileWorkspaceGeometryTests();
  RunDraggedWindowPlacementTests();
  RunOverlaySurfaceReleaseTests();
  RunGhostWindowTests();
  RunSettledWindowRecoveryTests();
  RunConstraintRecoveryDigest();
  RunOverflowFloatPredictionTests();
}
