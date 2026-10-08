//=============================================================================
// Windows platform adapter: WinEvent observation + queueing
//=============================================================================
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
  const HMONITOR observed = GetWindowPhysicalMonitor(hwnd);
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

// Out-of-context WinEvents are delivered on this WM thread. Keep the callback
// observation-only and post accepted work so reentrant delivery can't recursively
// mutate workspace state.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  AssertWmThread(L"WinEventProc");
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
      break;
    default:
      return;
  }

  // Retain foreground state on the serialized WM thread. Unknown HWNDs
  // deliberately do not clear managed focus: a just-created window can become
  // foreground before Automatic discovery admits it, matching FancyWM's
  // race-safe focus semantics.
  if (event == EVENT_SYSTEM_FOREGROUND) {
    const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
    if (threadId) {
      PostThreadMessage(
          threadId, WM_APP_FOREGROUND_CHANGED,
          reinterpret_cast<WPARAM>(hwnd), 0);
    }
    return;
  }

  // For window-level events only.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  // Opt-in rules can bridge custom frame/helper HWNDs to an already-managed root
  // owner for native move/size boundaries. Lifecycle events retain exact identity.
  if ((event == EVENT_SYSTEM_MOVESIZESTART ||
       event == EVENT_SYSTEM_MOVESIZEEND) &&
      !IsWindowTrackedInAnyState(hwnd) &&
      FindMatchingWindowRule(
          hwnd, WindowRuleTreatment::TraceToOwner)) {
    HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    if (rootOwner && IsWindowTrackedInAnyState(rootOwner)) {
      hwnd = rootOwner;
    }
  }

  const bool tracked = IsWindowTrackedInAnyState(hwnd);
  if (event == EVENT_OBJECT_DESTROY) {
    // Retire lifetime metadata when destruction is observed, before a delayed
    // model message could mistake a reused HWND for the old routed window.
    g_wm.overflowWindows.erase(hwnd);
    g_wm.pendingWindowOverflow.erase(
        std::remove_if(g_wm.pendingWindowOverflow.begin(), g_wm.pendingWindowOverflow.end(),
                       [hwnd](const PendingWindowOverflow& pending) { return pending.hwnd == hwnd; }),
        g_wm.pendingWindowOverflow.end());
    if (g_wm.pendingOverflowSwitch.hwnd == hwnd) g_wm.pendingOverflowSwitch = {};
    const bool routingKnown = g_wm.monitorRoutingEvaluated.erase(hwnd) != 0;
    const bool routingPending = g_wm.pendingMonitorRoutes.erase(hwnd) != 0;
    if (tracked || routingKnown || routingPending) QueueWindowEvent(event, hwnd);
    return;
  }

  // A tiled LOCATIONCHANGE is classified later on the WM actor. Expected placement
  // echoes are cheap no-ops there; unexpected same-monitor drift opens a bounded
  // conformance lease instead of being silently discarded.
  if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    if (!tracked) {
      if (g_wm.pendingMonitorRoutes.count(hwnd)) {
        QueueWindowEvent(event, hwnd);
        return;
      }
      // A newly created window can already be minimized, maximized, or fullscreen
      // before the ordinary discovery snapshot sees it. Queue those states for the
      // inactive-admission path.
      const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
      if ((reason != SuspensionReason::Minimized &&
           reason != SuspensionReason::Maximized) ||
          !WindowCanBeManaged(hwnd)) {
        return;
      }
      QueueWindowEvent(event, hwnd);
      return;
    }

    const bool overflowPending = std::any_of(
        g_wm.pendingWindowOverflow.begin(), g_wm.pendingWindowOverflow.end(),
        [hwnd](const PendingWindowOverflow& pending) { return pending.hwnd == hwnd; });
    const bool stateOrLeaseNeedsObservation =
        overflowPending || g_workspaces.IsSuspendedMaximized(hwnd) ||
        HasActiveConformanceLease(hwnd) ||
        g_workspaces.IsTiled(hwnd);
    if (!stateOrLeaseNeedsObservation &&
        !HasTrackedMonitorOwnershipMismatch(hwnd)) {
      return;
    }

    QueueWindowEvent(event, hwnd);
    return;
  }

  // Unknown top-level lifecycle events are only discovery prompts. Do not apply
  // the full eligibility policy inside the callback: a newly shown framework
  // window can still be acquiring its final styles, owner, title, or desktop
  // registration. The settled snapshot remains authoritative for admission, and
  // Manual mode still blocks admission at the model boundary.
  if (!tracked) {
    const bool discoveryEvent =
        event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED ||
        event == EVENT_OBJECT_STATECHANGE ||
        event == EVENT_SYSTEM_MINIMIZESTART ||
        event == EVENT_SYSTEM_MINIMIZEEND ||
        ((event == EVENT_SYSTEM_MOVESIZESTART ||
          event == EVENT_SYSTEM_MOVESIZEEND) &&
         (g_wm.monitorRoutingReady || IsAutomaticMode()));
    if (!discoveryEvent || GetAncestor(hwnd, GA_ROOT) != hwnd) {
      return;
    }
  }

  // Explicit user manipulation always wins over a transient placement lease.
  // Lease cancellation is actor-local; workspace mutation remains deferred until
  // normal message dispatch.
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    g_wm.overflowWindows.erase(hwnd);
    g_wm.pendingWindowOverflow.erase(
        std::remove_if(g_wm.pendingWindowOverflow.begin(), g_wm.pendingWindowOverflow.end(),
                       [hwnd](const PendingWindowOverflow& pending) { return pending.hwnd == hwnd; }),
        g_wm.pendingWindowOverflow.end());
    if (g_wm.pendingOverflowSwitch.hwnd == hwnd) g_wm.pendingOverflowSwitch = {};
    if (g_wm.monitorRoutingReady) {
      g_wm.monitorRoutingEvaluated.insert(hwnd);
      g_wm.pendingMonitorRoutes.erase(hwnd);
    }
    CancelConformanceLease(hwnd);
  }

  // Untracked drags only cancel initial routing. They must not reach the
  // managed-window gesture handler or leave samples for an unrelated workspace.
  if (!tracked && (event == EVENT_SYSTEM_MOVESIZESTART ||
                   event == EVENT_SYSTEM_MOVESIZEEND)) {
    if (event == EVENT_SYSTEM_MOVESIZEEND) QueueWindowEvent(EVENT_OBJECT_SHOW, hwnd);
    return;
  }

  // Cache user move/resize boundaries. The callback only records facts; model
  // mutation and layout work happen later on the WM thread.
  if (event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND) {
    RECT r{};
    if (GetWindowFrameRect(hwnd, &r)) {
      if (event == EVENT_SYSTEM_MOVESIZESTART) {
        g_moveSize.startRects[hwnd] = r;
      } else {
        g_moveSize.endRects[hwnd] = r;
        POINT pt{};
        if (GetCursorPos(&pt)) g_moveSize.endPoints[hwnd] = pt;
      }
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND) OnWindowMoveSizeEnd(hwnd);
    return;
  }

  // Accepted lifecycle events are serialized on the WM thread.
  // PostThreadMessage keeps model mutation out of the WinEvent callback.
  if (event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED) {
    // Older notification ABIs rely on the settled cloak/uncloak fallback to
    // discover desktop switches. Until that pass completes, preserve the current
    // behavior by making active-desktop checks query Explorer directly.
    if (g_wm.reconciledDesktopState != ReconciledDesktopState::Unknown) {
      g_wm.reconciledDesktopState =
          ReconciledDesktopState::TransitionPending;
    }
  }
  QueueWindowEvent(event, hwnd);
}

bool AllWinEventHooksInstalled() {
  return g_hooks.foreground && g_hooks.locationChange && g_hooks.moveSize &&
         g_hooks.minimize && g_hooks.hideDestroy && g_hooks.cloak && g_hooks.state;
}

bool InstallWinEventHooks() {
  AssertWmThread(L"Platform::WindowEvents::InstallWinEventHooks");
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

