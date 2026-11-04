namespace Platform::WindowEvents {

void OnWindowMoveSizeEnd(HWND hwnd) {
  if (!IsWindow(hwnd)) return;
  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (!threadId) return;
  if (!PostThreadMessage(threadId, WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(hwnd), 0)) {
    Wh_Log(L"Failed to queue resize event");
  }
}

static void QueueWindowEvent(DWORD event, HWND hwnd) {
  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (!threadId || !hwnd) return;
  if (!PostThreadMessage(threadId, WM_APP_WINDOW_EVENT,
                         static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd))) {
    Wh_Log(L"Failed to queue lifecycle event %lu", event);
  }
}

static bool HasTrackedMonitorOwnershipMismatch(HWND hwnd) {
  const HMONITOR observed = Platform::Windows::GetWindowPhysicalMonitor(hwnd);
  Model::MonitorId observedId;
  if (!observed || !Model::MonitorId::FromHMonitor(observed, &observedId)) {
    return false;
  }

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  if (owners.empty()) return false;
  return std::any_of(
      owners.begin(), owners.end(),
      [&](const DesktopMonitorKey& owner) { return owner.monitor != observedId; });
}

// --- WinEvent callback -------------------------------------------------------
//
// Out-of-context WinEvents are delivered on the WM thread, possibly re-entrantly
// while other WM code is waiting on a native call. So the callback only observes
// and posts work; it never changes workspace state. The helpers below each handle
// one family of events and are called only from WinEventProc.

static bool IsSubscribedWinEvent(DWORD event) {
  switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
    case EVENT_SYSTEM_MOVESIZESTART:
    case EVENT_SYSTEM_MOVESIZEEND:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_STATECHANGE:
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
      return true;
    default:
      return false;
  }
}

static bool IsMoveSizeEvent(DWORD event) {
  return event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND;
}

// Unknown HWNDs deliberately do not clear managed focus: a just-created window
// can become foreground before Automatic discovery admits it, matching FancyWM's
// race-safe focus semantics. The WM thread decides what the change means.
static void PostForegroundChanged(HWND hwnd) {
  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (threadId) {
    PostThreadMessage(threadId, WM_APP_FOREGROUND_CHANGED, reinterpret_cast<WPARAM>(hwnd), 0);
  }
}

// A "Trace to owner" rule lets a custom title bar or helper window stand in for
// its managed root owner during a move/resize. Returns the HWND the gesture
// should be attributed to. Other events always keep their exact HWND.
static HWND ResolveMoveSizeTarget(DWORD event, HWND hwnd) {
  if (!IsMoveSizeEvent(event) || Platform::Windows::IsWindowTrackedInAnyState(hwnd) ||
      !Platform::Windows::FindMatchingWindowRule(hwnd, WindowRuleTreatment::TraceToOwner)) {
    return hwnd;
  }
  const HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
  return rootOwner && Platform::Windows::IsWindowTrackedInAnyState(rootOwner) ? rootOwner : hwnd;
}

// Retires per-HWND bookkeeping as soon as destruction is observed, before a
// delayed message could mistake a reused HWND value for the old window.
static void OnWindowDestroyed(HWND hwnd, bool tracked) {
  g_wm.CancelPendingWindowMoves(hwnd);
  const bool routingKnown = g_wm.windowRoutingEvaluated.erase(hwnd) != 0;
  const bool routingPending = g_wm.pendingWindowRoutes.erase(hwnd) != 0;
  if (tracked || routingKnown || routingPending) QueueWindowEvent(EVENT_OBJECT_DESTROY, hwnd);
}

// LOCATIONCHANGE fires constantly, so only forward the ones the WM thread needs.
// For tiled windows, expected placement echoes are cheap no-ops there, while
// unexpected drift opens a bounded conformance lease.
static void OnWindowLocationChanged(HWND hwnd, bool tracked) {
  if (!tracked) {
    if (g_wm.pendingWindowRoutes.count(hwnd)) {
      QueueWindowEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd);
      return;
    }
    // A newly created window can already be minimized, maximized, or fullscreen
    // before the ordinary discovery snapshot sees it. Queue those states for the
    // inactive-admission path.
    const SuspensionReason reason = Platform::Windows::GetPhysicalSuspensionReason(hwnd);
    if ((reason != SuspensionReason::Minimized && reason != SuspensionReason::Maximized) ||
        !Platform::Windows::WindowCanBeManaged(hwnd)) {
      return;
    }
    QueueWindowEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd);
    return;
  }

  const bool stateOrLeaseNeedsObservation =
      g_wm.HasPendingOverflow(hwnd) || g_workspaces.IsSuspendedMaximized(hwnd) ||
      Placement::HasActiveConformanceLease(hwnd) || g_workspaces.IsTiled(hwnd);
  if (!stateOrLeaseNeedsObservation && !HasTrackedMonitorOwnershipMismatch(hwnd)) return;
  QueueWindowEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd);
}

// For an untracked window, true when `event` should trigger discovery.
// Eligibility is deliberately not checked here: a newly shown framework window
// can still be acquiring its final styles, owner, title, or desktop. The settled
// snapshot decides admission, and Manual mode still blocks it in the model.
static bool IsDiscoveryPrompt(DWORD event, HWND hwnd) {
  const bool discoveryEvent =
      event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED ||
      event == EVENT_OBJECT_STATECHANGE ||
      event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND ||
      (IsMoveSizeEvent(event) && (g_wm.windowRoutingReady || RuntimeLifecycle::IsAutomaticMode()));
  return discoveryEvent && GetAncestor(hwnd, GA_ROOT) == hwnd;
}

// A user drag or resize always wins over pending automatic work for that window.
// For managed windows, records the start/end frames that Gestures classifies later.
static void OnMoveSize(DWORD event, HWND hwnd, bool tracked) {
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    g_wm.CancelPendingWindowMoves(hwnd);
    if (g_wm.windowRoutingReady) {
      g_wm.windowRoutingEvaluated.insert(hwnd);
      g_wm.pendingWindowRoutes.erase(hwnd);
    }
    Placement::CancelConformanceLease(hwnd);
  }

  // Untracked drags only cancel initial routing. They must not reach the
  // managed-window gesture handler or leave samples for an unrelated workspace.
  if (!tracked) {
    if (event == EVENT_SYSTEM_MOVESIZEEND) QueueWindowEvent(EVENT_OBJECT_SHOW, hwnd);
    return;
  }

  RECT frame{};
  if (Platform::Windows::GetWindowFrameRect(hwnd, &frame)) {
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
      Gestures::g_moveSize.startRects[hwnd] = frame;
    } else {
      Gestures::g_moveSize.endRects[hwnd] = frame;
      POINT cursor{};
      if (GetCursorPos(&cursor)) Gestures::g_moveSize.endPoints[hwnd] = cursor;
    }
  }
  if (event == EVENT_SYSTEM_MOVESIZEEND) OnWindowMoveSizeEnd(hwnd);
}

// Show/hide/cloak/minimize/state changes: forward to the WM thread.
static void OnLifecycleEvent(DWORD event, HWND hwnd) {
  if (event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED) {
    // Older notification ABIs rely on the settled cloak/uncloak fallback to
    // discover desktop switches. Until that pass completes, make active-desktop
    // checks query Explorer directly.
    if (g_wm.reconciledDesktopState != ReconciledDesktopState::Unknown) {
      g_wm.reconciledDesktopState = ReconciledDesktopState::TransitionPending;
    }
  }
  QueueWindowEvent(event, hwnd);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  RuntimeLifecycle::AssertWmThread(L"WinEventProc");
  if (!IsSubscribedWinEvent(event)) return;
  if (event == EVENT_SYSTEM_FOREGROUND) {
    PostForegroundChanged(hwnd);
    return;
  }
  // Only whole top-level-window events; ignore child objects such as scroll bars.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  hwnd = ResolveMoveSizeTarget(event, hwnd);
  const bool tracked = Platform::Windows::IsWindowTrackedInAnyState(hwnd);
  if (event == EVENT_OBJECT_DESTROY) {
    OnWindowDestroyed(hwnd, tracked);
  } else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    OnWindowLocationChanged(hwnd, tracked);
  } else if (!tracked && !IsDiscoveryPrompt(event, hwnd)) {
    return;  // Unknown window, and not an event that should trigger discovery.
  } else if (IsMoveSizeEvent(event)) {
    OnMoveSize(event, hwnd, tracked);
  } else {
    OnLifecycleEvent(event, hwnd);
  }
}
bool AllWinEventHooksInstalled() {
  return g_hooks.foreground && g_hooks.locationChange && g_hooks.moveSize &&
         g_hooks.minimize && g_hooks.hideDestroy && g_hooks.cloak && g_hooks.state;
}

bool InstallWinEventHooks() {
  RuntimeLifecycle::AssertWmThread(L"Platform::WindowEvents::InstallWinEventHooks");
  auto noteFailure = [](const wchar_t* name) {
    ++Diagnostics::g_runtime.counters.winEventHookInstallFailures;
    Wh_Log(L"Failed to install %s WinEvent hook: %lu", name, GetLastError());
  };

  if (!g_hooks.foreground) {
    g_hooks.foreground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.foreground) noteFailure(L"foreground");
  }

  if (!g_hooks.locationChange) {
    g_hooks.locationChange = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.locationChange) noteFailure(L"location-change");
  }

  if (!g_hooks.moveSize) {
    g_hooks.moveSize = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.moveSize) noteFailure(L"move/size");
  }

  if (!g_hooks.minimize) {
    g_hooks.minimize = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.minimize) noteFailure(L"minimization");
  }

  if (!g_hooks.hideDestroy) {
    g_hooks.hideDestroy = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hooks.hideDestroy) noteFailure(L"hide/destroy");
  }

  if (!g_hooks.cloak) {
    g_hooks.cloak = SetWinEventHook(
      EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hooks.cloak) noteFailure(L"cloak");
  }

  if (!g_hooks.state) {
    g_hooks.state = SetWinEventHook(
        EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE,
        nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.state) noteFailure(L"state-change");
  }

  return AllWinEventHooksInstalled();
}

void RemoveWinEventHooks() {
    if (g_hooks.foreground) {
        UnhookWinEvent(g_hooks.foreground);
        g_hooks.foreground = nullptr;
    }
    if (g_hooks.locationChange) {
        UnhookWinEvent(g_hooks.locationChange);
        g_hooks.locationChange = nullptr;
    }
    if (g_hooks.moveSize) {
        UnhookWinEvent(g_hooks.moveSize);
        g_hooks.moveSize = nullptr;
    }
    if (g_hooks.minimize) {
        UnhookWinEvent(g_hooks.minimize);
        g_hooks.minimize = nullptr;
    }
    if (g_hooks.hideDestroy) {
        UnhookWinEvent(g_hooks.hideDestroy);
        g_hooks.hideDestroy = nullptr;
    }
    if (g_hooks.cloak) {
        UnhookWinEvent(g_hooks.cloak);
        g_hooks.cloak = nullptr;
    }
    if (g_hooks.state) {
        UnhookWinEvent(g_hooks.state);
        g_hooks.state = nullptr;
    }
}

}  // namespace Platform::WindowEvents

