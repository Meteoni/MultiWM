// WinEvent hooks and the callback that turns their events into WM-thread messages.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

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

// --- Ghosts of hung windows ---------------------------------------------------
//
// An event on a ghost is about the hung window it stands in for, whose state is
// read from the ghost (Platform::Windows::GhostWindowOf). Known ghosts are
// remembered; a new one is recognized when it is shown, takes the foreground,
// is minimized or restored, or starts a move/resize gesture.

static bool CanIntroduceGhost(DWORD event) {
  return event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_FOREGROUND ||
         event == EVENT_OBJECT_STATECHANGE || event == EVENT_SYSTEM_MINIMIZESTART ||
         event == EVENT_SYSTEM_MINIMIZEEND || IsMoveSizeEvent(event);
}

// The managed hung window `hwnd` is the ghost of, or nullptr. A known ghost is
// forgotten when it is destroyed, which happens when its app recovers.
static HWND ResolveGhost(DWORD event, HWND hwnd) {
  const auto known = g_wm.ghosts.find(hwnd);
  if (known != g_wm.ghosts.end()) {
    const HWND hung = known->second;
    if (event == EVENT_OBJECT_DESTROY) g_wm.ghosts.erase(known);
    return hung;
  }
  if (!hwnd || !CanIntroduceGhost(event) || Platform::Windows::IsWindowTrackedInAnyState(hwnd)) return nullptr;
  const HWND hung = Platform::Windows::HungWindowOfGhost(hwnd);
  if (!hung || !Platform::Windows::IsWindowTrackedInAnyState(hung)) return nullptr;
  g_wm.ghosts.emplace(hwnd, hung);
  return hung;
}

// Retires per-HWND bookkeeping as soon as destruction is observed, before a
// delayed message could mistake a reused HWND value for the old window.
static void OnWindowDestroyed(HWND hwnd, bool tracked) {
  g_wm.CancelPendingWindowMoves(hwnd);
  std::erase_if(g_wm.ghosts, [hwnd](const auto& ghost) { return ghost.second == hwnd; });
  const bool routingKnown = g_wm.routing.evaluated.erase(hwnd) != 0;
  const bool routingPending = g_wm.routing.pending.erase(hwnd) != 0;
  if (tracked || routingKnown || routingPending) QueueWindowEvent(EVENT_OBJECT_DESTROY, hwnd);
}

// LOCATIONCHANGE fires constantly, so only forward the ones the WM thread needs.
// For tiled windows, expected placement echoes are cheap no-ops there, while
// unexpected drift opens a bounded conformance lease.
static void OnWindowLocationChanged(HWND hwnd, bool tracked) {
  if (!tracked) {
    if (g_wm.routing.pending.count(hwnd)) {
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
      (IsMoveSizeEvent(event) && (g_wm.routing.ready || RuntimeLifecycle::IsAutomaticMode()));
  return discoveryEvent && GetAncestor(hwnd, GA_ROOT) == hwnd;
}

// A user drag or resize always wins over pending automatic work for that window.
// For managed windows, records the start/end frames that Gestures classifies later.
static void OnMoveSize(DWORD event, HWND hwnd, bool tracked, HWND frameWindow) {
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    g_wm.CancelPendingWindowMoves(hwnd);
    if (g_wm.routing.ready) {
      g_wm.routing.evaluated.insert(hwnd);
      g_wm.routing.pending.erase(hwnd);
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
  const bool hasFrame = Platform::Windows::GetWindowFrameRect(frameWindow, &frame);
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    if (!hasFrame) return;
    Gestures::g_moveSize.startRects[hwnd] = frame;
    if (g_settings.DragPreviewEnabled()) {
      if (const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire)) {
        PostThreadMessageW(threadId, WM_APP_MOVE_SIZE_START, reinterpret_cast<WPARAM>(hwnd),
                           reinterpret_cast<LPARAM>(frameWindow));
      }
    }
    return;
  }
  if (hasFrame) {
    Gestures::g_moveSize.endRects[hwnd] = frame;
    POINT cursor{};
    if (GetCursorPos(&cursor)) Gestures::g_moveSize.endPoints[hwnd] = cursor;
  }
  OnWindowMoveSizeEnd(hwnd);
}

// Show/hide/cloak/minimize/state changes: forward to the WM thread.
static void OnLifecycleEvent(DWORD event, HWND hwnd) {
  if (event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED) {
    // Older notification ABIs rely on the settled cloak/uncloak fallback to
    // discover desktop switches. Until that pass completes, make active-desktop
    // checks query Explorer directly.
    RuntimeLifecycle::MarkDesktopTransitionPending();
  }
  QueueWindowEvent(event, hwnd);
}

// The tray and toasts belong to the WM thread, the drag preview to its own.
static bool IsOwnWindow(HWND hwnd) {
  const DWORD wmThread = g_wm.threadId.load(std::memory_order_acquire);
  const DWORD owner = GetWindowThreadProcessId(hwnd, nullptr);
  return owner && ((wmThread && owner == wmThread) || DragPreview::IsPreviewThread(owner));
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  RuntimeLifecycle::AssertWmThread(L"WinEventProc");
  if (!IsSubscribedWinEvent(event)) return;
  if (hwnd && IsOwnWindow(hwnd)) return;
  if (event == EVENT_SYSTEM_FOREGROUND) {
    // A ghost in front means its hung window is.
    const HWND hung = ResolveGhost(event, hwnd);
    PostForegroundChanged(hung ? hung : hwnd);
    return;
  }
  // Only whole top-level-window events; ignore child objects such as scroll bars.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  // Anything that happens to a ghost may change its hung window's state: have the
  // WM thread look again. Its destroy, when the app recovers, is not the window's.
  if (const HWND hung = ResolveGhost(event, hwnd)) {
    if (IsMoveSizeEvent(event)) {
      OnMoveSize(event, hung, true, hwnd);
    } else {
      QueueWindowEvent(EVENT_OBJECT_STATECHANGE, hung);
    }
    return;
  }

  hwnd = ResolveMoveSizeTarget(event, hwnd);
  const bool tracked = Platform::Windows::IsWindowTrackedInAnyState(hwnd);
  if (event == EVENT_OBJECT_DESTROY) {
    OnWindowDestroyed(hwnd, tracked);
  } else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    OnWindowLocationChanged(hwnd, tracked);
  } else if (!tracked && !IsDiscoveryPrompt(event, hwnd)) {
    return;  // Unknown window, and not an event that should trigger discovery.
  } else if (IsMoveSizeEvent(event)) {
    OnMoveSize(event, hwnd, tracked, hwnd);
  } else {
    OnLifecycleEvent(event, hwnd);
  }
}
// One SetWinEventHook registration per WinEventHooks member, in installation and
// removal order. All are out-of-context, so callbacks arrive on the WM thread.
struct WinEventHookSpec {
  HWINEVENTHOOK WinEventHooks::*slot;
  DWORD firstEvent;
  DWORD lastEvent;
  const wchar_t* name;
};

static constexpr WinEventHookSpec kWinEventHookSpecs[] = {
    {&WinEventHooks::foreground, EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, L"foreground"},
    {&WinEventHooks::locationChange, EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, L"location-change"},
    {&WinEventHooks::moveSize, EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, L"move/size"},
    {&WinEventHooks::minimize, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, L"minimization"},
    {&WinEventHooks::hideDestroy, EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, L"hide/destroy"},
    {&WinEventHooks::cloak, EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, L"cloak"},
    {&WinEventHooks::state, EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE, L"state-change"},
};

bool AllWinEventHooksInstalled() {
  return std::all_of(std::begin(kWinEventHookSpecs), std::end(kWinEventHookSpecs),
                     [](const WinEventHookSpec& spec) { return g_hooks.*spec.slot != nullptr; });
}

// Installs every hook that is missing; already installed ones are kept.
bool InstallWinEventHooks() {
  RuntimeLifecycle::AssertWmThread(L"Platform::WindowEvents::InstallWinEventHooks");
  for (const WinEventHookSpec& spec : kWinEventHookSpecs) {
    HWINEVENTHOOK& hook = g_hooks.*spec.slot;
    if (hook) continue;
    hook = SetWinEventHook(spec.firstEvent, spec.lastEvent, nullptr, WinEventProc, 0, 0,
                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) {
      ++Diagnostics::g_runtime.counters.winEventHookInstallFailures;
      Wh_Log(L"Failed to install %s WinEvent hook: %lu", spec.name, GetLastError());
    }
  }
  return AllWinEventHooksInstalled();
}

void RemoveWinEventHooks() {
  for (const WinEventHookSpec& spec : kWinEventHookSpecs) {
    HWINEVENTHOOK& hook = g_hooks.*spec.slot;
    if (!hook) continue;
    UnhookWinEvent(hook);
    hook = nullptr;
  }
}

}  // namespace Platform::WindowEvents

