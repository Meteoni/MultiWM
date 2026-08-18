//=============================================================================
// Settings parsing + serialized WM thread
//=============================================================================

// Parse single character to virtual key code
UINT ParseSingleCharKey(PCWSTR str) {
  if (!str || !str[0]) return 0;
  wchar_t c = str[0];

  if (c >= L'A' && c <= L'Z') return c;
  if (c >= L'a' && c <= L'z') return c - L'a' + L'A';
  if (c >= L'0' && c <= L'9') return c;

  if (c == L'!') return '1';
  if (c == L'@') return '2';
  if (c == L'#') return '3';
  if (c == L'$') return '4';
  if (c == L'%') return '5';
  if (c == L'^') return '6';
  if (c == L'&') return '7';
  if (c == L'*') return '8';
  if (c == L'(') return '9';
  if (c == L')') return '0';

  if (c == L'`' || c == L'~') return VK_OEM_3;
  if (c == L'-' || c == L'_') return VK_OEM_MINUS;
  if (c == L'=' || c == L'+') return VK_OEM_PLUS;
  if (c == L'[' || c == L'{') return VK_OEM_4;
  if (c == L']' || c == L'}') return VK_OEM_6;
  if (c == L'\\' || c == L'|') return VK_OEM_5;
  if (c == L';' || c == L':') return VK_OEM_1;
  if (c == L'\'' || c == L'"') return VK_OEM_7;
  if (c == L',' || c == L'<') return VK_OEM_COMMA;
  if (c == L'.' || c == L'>') return VK_OEM_PERIOD;
  if (c == L'/' || c == L'?') return VK_OEM_2;
  if (c == L' ') return VK_SPACE;

  return 0;
}

void LoadSettings() {
  PCWSTR version = Wh_GetStringSetting(L"WindowsVersion");
  g_settings.windowsVersionIndex = ParseWindowsVersion(version);
  Wh_FreeStringSetting(version);

  g_settings.tilingModifiers = ReadModifierSetting(L"TilingModifier", MOD_ALT);

  auto readInset = [](PCWSTR name) -> LONG {
    int value = Wh_GetIntSetting(name);
    return (value >= 0 && value <= 500) ? static_cast<LONG>(value) : 4;
  };
  g_settings.insets.left = readInset(L"WorkspaceInsetLeft");
  g_settings.insets.top = readInset(L"WorkspaceInsetTop");
  g_settings.insets.right = readInset(L"WorkspaceInsetRight");
  g_settings.insets.bottom = readInset(L"WorkspaceInsetBottom");
  g_settings.gap = Wh_GetIntSetting(L"TileGap");
  if (g_settings.gap < 0 || g_settings.gap > 100) g_settings.gap = 4;
  g_settings.masterPercent = Wh_GetIntSetting(L"MasterPercent");
  if (g_settings.masterPercent < 1 || g_settings.masterPercent > 99) g_settings.masterPercent = 50;

  PCWSTR layout = Wh_GetStringSetting(L"DefaultLayout");
  g_settings.defaultLayout = ParseLayoutSetting(layout);
  Wh_FreeStringSetting(layout);

  PCWSTR mouseBehavior = Wh_GetStringSetting(L"MouseMoveBehavior");
  g_settings.mouseMoveBehavior = mouseBehavior && _wcsicmp(mouseBehavior, L"swap") == 0
                            ? MouseMoveBehavior::Swap
                            : MouseMoveBehavior::Float;
  Wh_FreeStringSetting(mouseBehavior);

  g_settings.enabled = Wh_GetIntSetting(L"EnableTiling") != 0;

  PCWSTR managementMode = Wh_GetStringSetting(L"DefaultWindowManagementMode");
  g_wm.managementMode =
      managementMode && _wcsicmp(managementMode, L"automatic") == 0
          ? ManagementMode::Automatic
          : ManagementMode::Manual;
  Wh_FreeStringSetting(managementMode);

  int reconcileDelay = Wh_GetIntSetting(L"ReconcileDelayMs");
  if (reconcileDelay < 20 || reconcileDelay > 2000) reconcileDelay = 80;
  g_settings.reconcileDelayMs = static_cast<UINT>(reconcileDelay);

  g_settings.exclusionRules.clear();
  using WindhawkUtils::StringSetting;
  for (int i = 0; i < 64; ++i) {
    auto match = StringSetting::make(L"Exclusions[%d].Match", i);
    auto value = StringSetting::make(L"Exclusions[%d].Value", i);
    if (!*match.get() && !*value.get()) break;
    if (!*value.get()) continue;

    ExclusionRule rule;
    if (_wcsicmp(match.get(), L"class") == 0) {
      rule.match = ExclusionMatch::Class;
    } else if (_wcsicmp(match.get(), L"title") == 0) {
      rule.match = ExclusionMatch::Title;
    } else {
      rule.match = ExclusionMatch::Process;
    }
    rule.value = value.get();
    g_settings.exclusionRules.push_back(std::move(rule));
  }

  g_settings.tileKey = ReadHotkeySetting(L"TileKey", ParseSingleCharKey, (UINT)'D');
  g_settings.layoutKey = ReadHotkeySetting(L"LayoutKey", ParseSingleCharKey, (UINT)'L');
  g_settings.swapMasterKey = ReadHotkeySetting(L"SwapMasterKey", ParseSingleCharKey, (UINT)'M');
  g_settings.managementModeToggleKey = ReadHotkeySetting(L"ManagementModeToggleKey", ParseSingleCharKey, (UINT)'U');
  g_settings.floatFocusedKey = ReadHotkeySetting(L"FloatFocusedKey", ParseSingleCharKey, (UINT)'F');
  g_settings.diagnosticDumpKey = ReadHotkeySetting(L"DiagnosticDumpKey", ParseSingleCharKey, (UINT)'I');

  TrayUi::LoadSettings();
}

static void RegisterConfiguredHotkeys() {
  if (g_settings.enabled) {
    if (g_settings.tileKey) {
      RegisterHotKey(nullptr, HK_TILE, g_settings.tilingModifiers, g_settings.tileKey);
    }
    if (g_settings.swapMasterKey) {
      RegisterHotKey(
          nullptr, HK_SWAP_MASTER, g_settings.tilingModifiers,
          g_settings.swapMasterKey);
    }
    if (g_settings.managementModeToggleKey) {
      RegisterHotKey(
          nullptr, HK_MANAGEMENT_MODE_TOGGLE, g_settings.tilingModifiers,
          g_settings.managementModeToggleKey);
    }
    if (g_settings.floatFocusedKey) {
      RegisterHotKey(
          nullptr, HK_FLOAT_FOCUSED, g_settings.tilingModifiers,
          g_settings.floatFocusedKey);
    }
    if (g_settings.layoutKey) {
      RegisterHotKey(nullptr, HK_LAYOUT, g_settings.tilingModifiers, g_settings.layoutKey);
    }
  }

  if (g_settings.diagnosticDumpKey) {
    RegisterHotKey(
        nullptr, HK_DIAGNOSTIC_DUMP, g_settings.tilingModifiers,
        g_settings.diagnosticDumpKey);
  }
  Wh_Log(L"Hotkeys registered");
}

static void UnregisterConfiguredHotkeys() {
  if (g_settings.enabled) {
    UnregisterHotKey(nullptr, HK_TILE);
    UnregisterHotKey(nullptr, HK_SWAP_MASTER);
    UnregisterHotKey(nullptr, HK_MANAGEMENT_MODE_TOGGLE);
    UnregisterHotKey(nullptr, HK_FLOAT_FOCUSED);
    UnregisterHotKey(nullptr, HK_LAYOUT);
  }
  UnregisterHotKey(nullptr, HK_DIAGNOSTIC_DUMP);
}

static void InitializeCurrentWorkspace() {
  if (!g_settings.enabled) return;

  HMONITOR monitor = GetTargetMonitor();
  if (monitor) {
    DesktopMonitorKey key{};
    Workspace existingState;
    if (GetCurrentWorkspaceKey(monitor, &key) &&
        g_workspaces.Load(key, &existingState)) {
      ArrangeWorkspace(key);
    } else {
      Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
  }

  Wh_Log(
      L"Window management mode at startup: %s",
      IsManualMode() ? L"Manual" : L"Automatic");
}

static void ToggleWindowManagementMode() {
  g_wm.managementMode =
      IsAutomaticMode() ? ManagementMode::Manual : ManagementMode::Automatic;
  const bool automatic = IsAutomaticMode();
  Wh_Log(L"Window management mode: %s", automatic ? L"Automatic" : L"Manual");
  TrayUi::ShowManagementModeFlyout(automatic);

  // Entering Automatic mode runs the normal discovery path for every monitor.
  // Known Floating records remain managed-but-excluded and are never re-adopted.
  if (automatic) {
    std::vector<HMONITOR> monitors;
    Reconcile::EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
    Reconcile::ScheduleLifecycleReconcile(nullptr);
  }
}

static void HandleMoveSizeEndMessage(HWND hwnd) {
  // MOVESIZEEND is an authoritative monitor-transition boundary. Repair monitor
  // ownership before interpreting final geometry, without querying desktop
  // ownership for unrelated lifecycle noise.
  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::Monitor, &changedKeys, hwnd);
  Commands::ApplyUserMoveSize(hwnd);
  for (const auto& key : changedKeys) ArrangeWorkspace(key);
  Reconcile::ScheduleLifecycleReconcile(hwnd);
}

enum class WmMessageDisposition {
  Handled,
  DispatchToWindows,
  Quit,
};

// Converts one serialized thread message into a domain command. Messages not
// owned by the WM are explicitly returned to Windows so STA COM's hidden window
// continues receiving cross-apartment calls.
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg) {
  switch (msg.message) {
    case WM_QUIT:
      return WmMessageDisposition::Quit;

    case WM_APP_MOVE_SIZE_END:
      HandleMoveSizeEndMessage(reinterpret_cast<HWND>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_WINDOW_EVENT:
      Reconcile::ProcessWindowLifecycleEvent(
          static_cast<DWORD>(msg.wParam), reinterpret_cast<HWND>(msg.lParam));
      return WmMessageDisposition::Handled;

    case WM_APP_VIRTUAL_DESKTOP_CHANGED:
      Reconcile::HandleVirtualDesktopChanged();
      return WmMessageDisposition::Handled;

    case WM_APP_TRAY_REFRESH:
      TrayUi::RefreshForCurrentWorkspace();
      return WmMessageDisposition::Handled;

    case WM_APP_LAYOUT_CYCLE:
      Commands::CycleCurrentWorkspaceLayout();
      return WmMessageDisposition::Handled;

    case WM_TIMER:
      if (g_wm.lifecycleTimer &&
          static_cast<UINT_PTR>(msg.wParam) == g_wm.lifecycleTimer) {
        Reconcile::ReconcileDeferredLifecycle();
        return WmMessageDisposition::Handled;
      }
      return WmMessageDisposition::DispatchToWindows;

    case WM_HOTKEY:
      switch (static_cast<UINT>(msg.wParam)) {
        case HK_TILE:
          Commands::TileWindows();
          break;
        case HK_LAYOUT:
          Commands::CycleCurrentWorkspaceLayout();
          break;
        case HK_SWAP_MASTER:
          Commands::SwapMaster();
          break;
        case HK_FLOAT_FOCUSED:
          Commands::FloatFocusedWindow();
          break;
        case HK_DIAGNOSTIC_DUMP:
          Diagnostics::DumpSavedBookkeeping();
          break;
        case HK_MANAGEMENT_MODE_TOGGLE:
          ToggleWindowManagementMode();
          break;
      }
      return WmMessageDisposition::Handled;
  }

  return WmMessageDisposition::DispatchToWindows;
}

static void RunWmMessageLoop() {
  MSG msg{};
  while (!g_wm.stopRequested) {
    DWORD waitResult =
        MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);
    if (waitResult != WAIT_OBJECT_0) continue;

    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      switch (HandleWmThreadMessage(msg)) {
        case WmMessageDisposition::Quit:
          return;

        case WmMessageDisposition::Handled:
          break;

        case WmMessageDisposition::DispatchToWindows:
          TranslateMessage(&msg);
          DispatchMessage(&msg);
          break;
      }
    }
  }
}

static void CleanupWmThread() {
  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }
  g_wm.lifecycleDirtyWindows.clear();
  g_wm.pendingDesktopArranges.clear();
  InterlockedExchange(&g_vd.changeQueued, 0);

  UnregisterConfiguredHotkeys();
  TrayUi::Shutdown();
  Platform::WindowEvents::RemoveWinEventHooks();
  CleanupVirtualDesktopAPI();
}

// Owns the STA apartment and delegates startup, serialized dispatch, and cleanup
// to narrow helpers. All workspace mutations reachable from this thread remain
// serialized; foreign STA messages are dispatched back to Windows.
DWORD WINAPI HotkeyThreadProc(LPVOID) {
  g_wm.threadId = GetCurrentThreadId();
  Wh_Log(L"Hotkey thread started, thread ID: %lu", g_wm.threadId);

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Wh_Log(L"CoInitializeEx result: 0x%08X", coHr);

  // Force creation of this STA's message queue before callbacks can arrive.
  MSG msg{};
  PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

  if (g_settings.enabled && SUCCEEDED(coHr) && InitializeVirtualDesktopAPI()) {
    if (!RegisterVirtualDesktopNotifications()) {
      Wh_Log(L"Using settled WinEvent reconciliation for virtual desktop changes");
    }
  }

  SetEvent(g_wm.readyEvent);

  if (g_settings.enabled) {
    Platform::WindowEvents::InstallWinEventHooks();
    Reconcile::ScheduleLifecycleReconcile(nullptr);
  }
  RegisterConfiguredHotkeys();
  InitializeCurrentWorkspace();

  if (g_settings.enabled && !TrayUi::Initialize()) {
    Wh_Log(L"Tiling layout tray icon unavailable; window management remains active");
  }

  RunWmMessageLoop();
  CleanupWmThread();

  if (SUCCEEDED(coHr)) CoUninitialize();
  return 0;
}

bool StartHotkeyThread() {
  g_wm.readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  g_wm.thread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);

  if (!g_wm.thread || WaitForSingleObject(g_wm.readyEvent, 5000) != WAIT_OBJECT_0) {
    Wh_Log(L"Failed to start hotkey thread");
    if (g_wm.thread) {
      CloseHandle(g_wm.thread);
      g_wm.thread = nullptr;
    }
    CloseHandle(g_wm.readyEvent);
    g_wm.readyEvent = nullptr;
    return false;
  }

  CloseHandle(g_wm.readyEvent);
  g_wm.readyEvent = nullptr;
  return true;
}

void StopHotkeyThread() {
  g_wm.stopRequested = true;

  g_vd.initialized = false;

  if (g_wm.threadId) {
    PostThreadMessage(g_wm.threadId, WM_QUIT, 0, 0);
  }

  if (g_wm.thread) {
    DWORD waitResult = WaitForSingleObject(g_wm.thread, 5000);
    if (waitResult == WAIT_TIMEOUT) {
      Wh_Log(L"WARNING: Hotkey thread cleanup timeout, thread may be stuck in CoUninitialize");
    }
    CloseHandle(g_wm.thread);
    g_wm.thread = nullptr;
  }

  g_wm.threadId = 0;
  g_wm.stopRequested = false;
}

