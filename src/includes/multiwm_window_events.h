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

// Treats out-of-context WinEvents as edge-triggered observations: filter noise,
// cache move/resize boundaries, and post accepted work to the hotkey thread.
// Workspace state is never mutated from this callback.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
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

  // Foreground changes only affect which per-monitor workspace the tray icon
  // represents. Keep that UI refresh serialized on the WM thread as well.
  if (event == EVENT_SYSTEM_FOREGROUND) {
    const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
    if (threadId) PostThreadMessage(threadId, WM_APP_TRAY_REFRESH, 0, 0);
    return;
  }

  // For window-level events only.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  const bool tracked = IsWindowTrackedInAnyState(hwnd);

  // Normal layout placement itself produces LOCATIONCHANGE, so keep this path
  // narrow. Besides maximized->restored detection, a short conformance lease may
  // opt a tiled HWND into location observation while an app finishes startup.
  if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    if (!tracked) return;

    const bool stateOrLeaseNeedsObservation =
        IsWindowSuspendedMaximized(hwnd) ||
        HasActiveConformanceLease(hwnd);
    if (!stateOrLeaseNeedsObservation &&
        !HasTrackedMonitorOwnershipMismatch(hwnd)) {
      return;
    }
    QueueWindowEvent(event, hwnd);
    return;
  }

  // Keep shell/framework noise out of the WM queue. Untracked HWNDs still flow
  // through the normal discovery pipeline in both modes, but only after they
  // resemble real windows; Manual mode blocks admission at the model boundary.
  if (!tracked) {
    const bool discoveryEvent =
        event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED ||
        event == EVENT_OBJECT_STATECHANGE;
    const bool restoredCandidate =
        event != EVENT_OBJECT_STATECHANGE ||
        (IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd));
    if (!discoveryEvent || !restoredCandidate || !WindowCanBeManaged(hwnd)) {
      return;
    }
  }

  // Explicit user manipulation always wins over a transient placement lease.
  // Cancellation is lock-protected because this callback is outside the model
  // mutation boundary.
  if (event == EVENT_SYSTEM_MOVESIZESTART) {
    CancelConformanceLease(hwnd);
  }

  // Cache user move/resize boundaries. The callback only records facts; model
  // mutation and layout work happen later on the WM thread.
  if (event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND) {
    AcquireSRWLockExclusive(&g_moveSize.lock);
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
    ReleaseSRWLockExclusive(&g_moveSize.lock);

    if (event == EVENT_SYSTEM_MOVESIZEEND) OnWindowMoveSizeEnd(hwnd);
    return;
  }

  // Accepted lifecycle events are serialized on the WM thread.
  // PostThreadMessage keeps model mutation out of the WinEvent callback.
  QueueWindowEvent(event, hwnd);
}

bool AllWinEventHooksInstalled() {
  return g_hooks.foreground && g_hooks.locationChange && g_hooks.moveSize &&
         g_hooks.minimize && g_hooks.hideDestroy && g_hooks.cloak && g_hooks.state;
}

bool InstallWinEventHooks() {
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

