//=============================================================================
// Settings parsing + serialized WM thread
//=============================================================================

UINT ParseHotkeyKey(PCWSTR str) {
  if (!str || !str[0]) return 0;

  static const std::pair<PCWSTR, UINT> kNamedKeys[] = {
      {L"Space", VK_SPACE},
      {L"Tab", VK_TAB},
      {L"Enter", VK_RETURN},
      {L"Return", VK_RETURN},
      {L"Esc", VK_ESCAPE},
      {L"Escape", VK_ESCAPE},
      {L"Backspace", VK_BACK},
      {L"Insert", VK_INSERT},
      {L"Ins", VK_INSERT},
      {L"Delete", VK_DELETE},
      {L"Del", VK_DELETE},
      {L"Home", VK_HOME},
      {L"End", VK_END},
      {L"PageUp", VK_PRIOR},
      {L"PgUp", VK_PRIOR},
      {L"PageDown", VK_NEXT},
      {L"PgDn", VK_NEXT},
      {L"Left", VK_LEFT},
      {L"Right", VK_RIGHT},
      {L"Up", VK_UP},
      {L"Down", VK_DOWN},
  };
  for (const auto& namedKey : kNamedKeys) {
    if (_wcsicmp(str, namedKey.first) == 0) return namedKey.second;
  }

  if ((str[0] == L'F' || str[0] == L'f') && str[1]) {
    wchar_t* end = nullptr;
    long functionKey = std::wcstol(str + 1, &end, 10);
    if (*end == L'\0' && functionKey >= 1 && functionKey <= 12) {
      return VK_F1 + static_cast<UINT>(functionKey - 1);
    }
  }

  if (str[1] != L'\0') return 0;
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

static bool ParseWorkspaceInsetsSetting(PCWSTR text, WorkspaceInsets* outInsets) {
  if (!text || !outInsets) return false;

  LONG values[4]{};
  const wchar_t* cursor = text;

  for (size_t i = 0; i < 4; ++i) {
    while (*cursor && std::iswspace(*cursor)) ++cursor;

    wchar_t* end = nullptr;
    long value = std::wcstol(cursor, &end, 10);
    if (end == cursor || value < 0 || value > 500) return false;
    values[i] = static_cast<LONG>(value);
    cursor = end;

    while (*cursor && std::iswspace(*cursor)) ++cursor;
    if (i < 3) {
      if (*cursor != L',') return false;
      ++cursor;
    }
  }

  while (*cursor && std::iswspace(*cursor)) ++cursor;
  if (*cursor != L'\0') return false;

  *outInsets = {values[0], values[1], values[2], values[3]};
  return true;
}

static bool ParseFloatingDefaultSizeSetting(
    PCWSTR text, FloatingDefaultSize* outSize) {
  if (!text || !outSize) return false;

  LONG values[2]{};
  const wchar_t* cursor = text;
  for (size_t i = 0; i < 2; ++i) {
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    wchar_t* end = nullptr;
    long value = std::wcstol(cursor, &end, 10);
    if (end == cursor || value < 100 || value > 4000) return false;
    values[i] = static_cast<LONG>(value);
    cursor = end;
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    if (i == 0) {
      if (*cursor != L',') return false;
      ++cursor;
    }
  }
  while (*cursor && std::iswspace(*cursor)) ++cursor;
  if (*cursor != L'\0') return false;
  *outSize = {values[0], values[1]};
  return true;
}

void LoadSettings() {
  using WindhawkUtils::StringSetting;

  g_settings.tilingModifiers =
      ReadModifierSetting(L"hotkeys.TilingModifier", MOD_ALT);

  auto insets = StringSetting::make(L"workspace.WorkspaceInsets");
  WorkspaceInsets parsedInsets{};
  if (ParseWorkspaceInsetsSetting(insets.get(), &parsedInsets)) {
    g_settings.insetsDip = parsedInsets;
  } else {
    g_settings.insetsDip = {};
    Wh_Log(L"Invalid WorkspaceInsets setting; using 6, 6, 6, 6");
  }
  g_settings.gapDip = Wh_GetIntSetting(L"workspace.TileGap");
  if (g_settings.gapDip < 0 || g_settings.gapDip > 100) g_settings.gapDip = 6;

  auto floatingSize =
      StringSetting::make(L"windowBehavior.FloatingDefaultSize");
  FloatingDefaultSize parsedFloatingSize{};
  if (ParseFloatingDefaultSizeSetting(floatingSize.get(), &parsedFloatingSize)) {
    g_settings.floatingDefaultSizeDip = parsedFloatingSize;
  } else {
    g_settings.floatingDefaultSizeDip = {};
    Wh_Log(L"Invalid FloatingDefaultSize setting; using 960, 640");
  }
  g_settings.masterPercent = Wh_GetIntSetting(L"workspace.MasterPercent");
  if (g_settings.masterPercent < 1 || g_settings.masterPercent > 99) g_settings.masterPercent = 50;

  auto layout = StringSetting::make(L"workspace.DefaultLayout");
  g_settings.defaultLayout = ParseLayoutSetting(layout.get());

  g_settings.layoutCycle.clear();
  for (int i = 0; i < 32; ++i) {
    auto cycleEntry =
        StringSetting::make(L"workspace.LayoutCycle[%d]", i);
    if (!*cycleEntry.get()) break;

    TileLayout cycleLayout{};
    if (!TryParseLayoutSetting(cycleEntry.get(), &cycleLayout)) {
      Wh_Log(L"Ignoring invalid LayoutCycle[%d] entry: %s", i, cycleEntry.get());
      continue;
    }
    if (std::find(g_settings.layoutCycle.begin(), g_settings.layoutCycle.end(),
                  cycleLayout) == g_settings.layoutCycle.end()) {
      g_settings.layoutCycle.push_back(cycleLayout);
    }
  }
  if (g_settings.layoutCycle.empty()) {
    g_settings.layoutCycle = MakeBuiltInLayoutCycle();
    Wh_Log(L"LayoutCycle is empty or invalid; using the built-in layout sequence");
  }

  auto mouseBehavior =
      StringSetting::make(L"windowBehavior.MouseMoveBehavior");
  g_settings.mouseMoveBehavior =
      _wcsicmp(mouseBehavior.get(), L"swap") == 0
          ? MouseMoveBehavior::Swap
          : MouseMoveBehavior::Float;

  auto automaticNewWindowPosition =
      StringSetting::make(L"general.AutomaticNewWindowPosition");
  g_settings.automaticNewWindowPosition =
      _wcsicmp(automaticNewWindowPosition.get(), L"after_focused") == 0
          ? AutomaticNewWindowPosition::AfterFocused
          : AutomaticNewWindowPosition::LastSlot;

  auto managementMode =
      StringSetting::make(L"general.DefaultWindowManagementMode");
  g_wm.managementMode =
      _wcsicmp(managementMode.get(), L"manual") == 0
          ? ManagementMode::Manual
          : ManagementMode::Automatic;

  int conformanceLease = Wh_GetIntSetting(L"advanced.ConformanceLeaseMs");
  if (conformanceLease < 0 || conformanceLease > 10000) conformanceLease = 3000;
  g_settings.conformanceLeaseMs = static_cast<UINT>(conformanceLease);

  int conformanceRepairInterval =
      Wh_GetIntSetting(L"advanced.ConformanceRepairIntervalMs");
  if (conformanceRepairInterval < 20 || conformanceRepairInterval > 2000) {
    conformanceRepairInterval = 75;
  }
  g_settings.conformanceRepairIntervalMs =
      static_cast<UINT>(conformanceRepairInterval);

  int reconcileDelay = Wh_GetIntSetting(L"advanced.ReconcileDelayMs");
  if (reconcileDelay < 20 || reconcileDelay > 2000) reconcileDelay = 50;
  g_settings.reconcileDelayMs = static_cast<UINT>(reconcileDelay);

  g_settings.windowRules.clear();
  for (int i = 0; i < 64; ++i) {
    auto process =
        StringSetting::make(L"windowRules.Rules[%d].Process", i);
    auto className =
        StringSetting::make(L"windowRules.Rules[%d].Class", i);
    auto titleContains =
        StringSetting::make(L"windowRules.Rules[%d].TitleContains", i);
    auto treatment =
        StringSetting::make(L"windowRules.Rules[%d].Treatment", i);
    auto size = StringSetting::make(L"windowRules.Rules[%d].Size", i);
    if (!*process.get() && !*className.get() && !*titleContains.get()) {
      Wh_Log(
          L"Window rule %d is blank; later rules will not be loaded",
          i + 1);
      break;
    }

    WindowRule rule;
    if (_wcsicmp(treatment.get(), L"trace_to_owner") == 0) {
      rule.treatment = WindowRuleTreatment::TraceToOwner;
    } else if (_wcsicmp(
                   treatment.get(), L"preserve_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      rule.preserveFloatingSize = true;
    } else if (_wcsicmp(
                   treatment.get(), L"override_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      if (!ParseFloatingDefaultSizeSetting(
              size.get(), &rule.floatingSizeDip)) {
        Wh_Log(
            L"Ignoring invalid centered size override at rule %d: %s",
            i + 1, size.get());
        continue;
      }
      rule.preserveFloatingSize = false;
    } else {
      rule.treatment = WindowRuleTreatment::Exclude;
    }
    rule.process = process.get();
    rule.className = className.get();
    rule.titleContains = titleContains.get();
    g_settings.windowRules.push_back(std::move(rule));
  }

  g_settings.tileKey =
      ReadHotkeySetting<UINT>(L"hotkeys.TileKey", ParseHotkeyKey);
  g_settings.layoutKey =
      ReadHotkeySetting<UINT>(L"hotkeys.LayoutKey", ParseHotkeyKey);
  g_settings.swapMasterKey =
      ReadHotkeySetting<UINT>(L"hotkeys.SwapMasterKey", ParseHotkeyKey);
  g_settings.promoteWindowKey =
      ReadHotkeySetting<UINT>(L"hotkeys.PromoteWindowKey", ParseHotkeyKey);
  g_settings.demoteWindowKey =
      ReadHotkeySetting<UINT>(L"hotkeys.DemoteWindowKey", ParseHotkeyKey);
  g_settings.managementModeToggleKey =
      ReadHotkeySetting<UINT>(L"hotkeys.ManagementModeToggleKey", ParseHotkeyKey);
  g_settings.floatFocusedKey =
      ReadHotkeySetting<UINT>(L"hotkeys.FloatFocusedKey", ParseHotkeyKey);
  g_settings.diagnosticDumpKey =
      ReadHotkeySetting<UINT>(L"hotkeys.DiagnosticDumpKey", ParseHotkeyKey);

  auto diagnosticsPath =
      StringSetting::make(L"diagnostics.DiagnosticsOutputPath");
  if (*diagnosticsPath.get()) {
    g_settings.diagnosticsOutputPath = diagnosticsPath.get();
  } else {
    g_settings.diagnosticsOutputPath =
        L"%USERPROFILE%\\Documents\\MultiWMDiagnostics";
  }

  TrayUi::LoadSettings();
}

static void RegisterConfiguredHotkeys() {
  auto registerOne = [](
      int id, UINT modifiers, UINT key, const wchar_t* name,
      bool allowRepeat = false) {
    if (!key) return;
    const UINT registrationModifiers =
        allowRepeat ? modifiers : modifiers | MOD_NOREPEAT;
    if (!RegisterHotKey(nullptr, id, registrationModifiers, key)) {
      Wh_Log(L"Failed to register %s hotkey (id=%d error=%lu)",
             name, id, GetLastError());
    }
  };

  registerOne(HK_TILE, g_settings.tilingModifiers, g_settings.tileKey, L"Tile");
  registerOne(HK_SWAP_MASTER, g_settings.tilingModifiers,
              g_settings.swapMasterKey, L"Swap master");
  registerOne(HK_PROMOTE_WINDOW, g_settings.tilingModifiers,
              g_settings.promoteWindowKey, L"Promote window", true);
  registerOne(HK_DEMOTE_WINDOW, g_settings.tilingModifiers,
              g_settings.demoteWindowKey, L"Demote window", true);
  registerOne(HK_MANAGEMENT_MODE_TOGGLE, g_settings.tilingModifiers,
              g_settings.managementModeToggleKey, L"Management mode");
  registerOne(HK_FLOAT_FOCUSED, g_settings.tilingModifiers,
              g_settings.floatFocusedKey, L"Float focused");
  registerOne(HK_LAYOUT, g_settings.tilingModifiers,
              g_settings.layoutKey, L"Layout cycle");
  registerOne(HK_DIAGNOSTIC_DUMP, g_settings.tilingModifiers,
              g_settings.diagnosticDumpKey, L"Diagnostic dump");
}

static void UnregisterConfiguredHotkeys() {
  UnregisterHotKey(nullptr, HK_TILE);
  UnregisterHotKey(nullptr, HK_SWAP_MASTER);
  UnregisterHotKey(nullptr, HK_PROMOTE_WINDOW);
  UnregisterHotKey(nullptr, HK_DEMOTE_WINDOW);
  UnregisterHotKey(nullptr, HK_MANAGEMENT_MODE_TOGGLE);
  UnregisterHotKey(nullptr, HK_FLOAT_FOCUSED);
  UnregisterHotKey(nullptr, HK_LAYOUT);
  UnregisterHotKey(nullptr, HK_DIAGNOSTIC_DUMP);
}

static void RememberManagedForeground(HWND hwnd) {
  AssertWmThread(L"RememberManagedForeground");
  if (!hwnd) return;

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  for (const auto& key : owners) {
    Workspace workspace;
    if (!g_workspaces.Load(key, &workspace)) continue;
    if (!workspace.RememberFocusedWindow(hwnd)) continue;
    g_workspaces.Save(key, workspace);
  }
}

static void InitializeCurrentDesktopWorkspaces() {
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);
  for (HMONITOR monitor : monitors) {
    Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
  }
  RememberManagedForeground(GetForegroundWindow());

  Wh_Log(
      L"Window management mode at startup: %s (%zu connected monitor%s)",
      IsManualMode() ? L"Manual" : L"Automatic", monitors.size(),
      monitors.size() == 1 ? L"" : L"s");
}

static void SetWindowManagementMode(ManagementMode mode) {
  if (g_wm.managementMode == mode) return;
  g_wm.managementMode = mode;
  const bool automatic = IsAutomaticMode();
  Wh_Log(L"Window management mode: %s", automatic ? L"Automatic" : L"Manual");
  Diagnostics::RecordEvent(
      L"management mode changed to %ls", automatic ? L"Automatic" : L"Manual");
  TrayUi::ShowManagementModeFlyout(automatic);

  // Entering Automatic mode runs the normal discovery path for every monitor.
  // Known Floating records remain managed-but-excluded and are never re-adopted.
  if (automatic) {
    std::vector<HMONITOR> monitors;
    Reconcile::EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      Reconcile::DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
    RememberManagedForeground(GetForegroundWindow());
    Reconcile::ScheduleLifecycleReconcile(nullptr);
  }
}

static void ToggleWindowManagementMode() {
  SetWindowManagementMode(
      IsAutomaticMode() ? ManagementMode::Manual : ManagementMode::Automatic);
}

static void HandleMoveSizeEndMessage(HWND hwnd) {
  ++Diagnostics::g_runtime.counters.moveSizeEndMessages;
  // MOVESIZEEND is an authoritative monitor-transition boundary. Repair monitor
  // ownership before interpreting final geometry, without querying desktop
  // ownership for unrelated lifecycle noise.
  std::vector<DesktopMonitorKey> changedKeys;
  Reconcile::ReconcileKnownWindows(Reconcile::ReconcileScope::Monitor, &changedKeys, hwnd);
  Commands::ApplyUserMoveSize(hwnd);
  for (const auto& key : changedKeys) ArrangeWorkspace(key);
  Reconcile::ScheduleLifecycleReconcile(hwnd);
}

// Converts one serialized thread message into a domain command. Messages not
// owned by the WM are explicitly returned to Windows so STA COM's hidden window
// continues receiving cross-apartment calls.
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg) {
  switch (msg.message) {
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

    case WM_APP_FOREGROUND_CHANGED:
      RememberManagedForeground(reinterpret_cast<HWND>(msg.wParam));
      ++Diagnostics::g_runtime.counters.trayRefreshMessages;
      TrayUi::RefreshForCurrentWorkspace();
      return WmMessageDisposition::Handled;

    case WM_APP_LAYOUT_CYCLE:
      Commands::CycleCurrentWorkspaceLayout(
          reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_TILE_WORKSPACE:
      Commands::TileWindows(reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_LAYOUT_SET:
      Commands::SetCurrentWorkspaceLayout(
          static_cast<TileLayout>(msg.lParam),
          reinterpret_cast<HMONITOR>(msg.wParam));
      return WmMessageDisposition::Handled;

    case WM_APP_MANAGEMENT_MODE_SET:
      if (msg.wParam == static_cast<WPARAM>(ManagementMode::Manual) ||
          msg.wParam == static_cast<WPARAM>(ManagementMode::Automatic)) {
        SetWindowManagementMode(static_cast<ManagementMode>(msg.wParam));
      }
      return WmMessageDisposition::Handled;

    case WM_APP_RECONCILE_NOW:
      Reconcile::ReconcileDeferredLifecycle();
      return WmMessageDisposition::Handled;

    case WM_TIMER:
      // Actor timers are created with SetTimer(nullptr, ...). A window timer can
      // use the same numeric ID and must still reach its WndProc.
      if (msg.hwnd) return WmMessageDisposition::DispatchToWindows;

      if (g_wm.lifecycleTimer &&
          static_cast<UINT_PTR>(msg.wParam) == g_wm.lifecycleTimer) {
        Reconcile::ReconcileDeferredLifecycle();
        return WmMessageDisposition::Handled;
      }
      if (g_wm.maintenanceTimer &&
          static_cast<UINT_PTR>(msg.wParam) == g_wm.maintenanceTimer) {
        RuntimeLifecycle::RunMaintenanceNow();
        return WmMessageDisposition::Handled;
      }
      if (g_wm.conformanceTimer &&
          static_cast<UINT_PTR>(msg.wParam) == g_wm.conformanceTimer) {
        ProcessConformanceTimer();
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
        case HK_PROMOTE_WINDOW:
          Commands::PromoteFocusedWindow();
          break;
        case HK_DEMOTE_WINDOW:
          Commands::DemoteFocusedWindow();
          break;
        case HK_FLOAT_FOCUSED:
          Commands::FloatFocusedWindow();
          break;
        case HK_DIAGNOSTIC_DUMP:
          Diagnostics::WriteDiagnosticReport();
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
  // Standard Windhawk tool-mod worker pattern: one ordinary Windows message loop
  // owns thread messages, timers, the tray windows, and STA COM dispatch.
  MSG msg{};
  for (;;) {
    const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
    if (result == 0) return;  // WM_QUIT
    if (result == -1) {
      Wh_Log(L"WM worker GetMessage failed: %lu", GetLastError());
      return;
    }

    switch (HandleWmThreadMessage(msg)) {
      case WmMessageDisposition::Handled:
        break;

      case WmMessageDisposition::DispatchToWindows:
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        break;
    }
  }
}

static void CleanupWmThread() {
  // Unhook WinEvent delivery before clearing transient callback observations so
  // no reentrant delivery can repopulate the next settings-reload session.
  UnregisterConfiguredHotkeys();
  TrayUi::Shutdown();
  Platform::WindowEvents::RemoveWinEventHooks();
  CleanupVirtualDesktopAPI();

  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }
  if (g_wm.maintenanceTimer) {
    KillTimer(nullptr, g_wm.maintenanceTimer);
    g_wm.maintenanceTimer = 0;
  }
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }
  g_wm.lifecycleDirtyWindows.clear();
  g_wm.pendingDesktopArranges.clear();
  g_wm.maintenanceAttempts = 0;
  g_wm.maintenanceRunning = false;
  g_wm.lifecycleRetryAfterPlatformRecovery = false;
  g_wm.forceMonitorReconcile = false;
  g_wm.reconciledDesktop = {};
  g_wm.reconciledDesktopState = ReconciledDesktopState::Unknown;
  g_wm.pendingDesktopSwitchFlyouts = false;
  ClearAllConformanceLeases();
  ClearAllMoveSizeSamples();
  ClearMonitorIdentityCache();
  InterlockedExchange(&g_vd.changeQueued, 0);
}

// Owns the STA apartment and delegates startup, serialized dispatch, and cleanup
// to narrow helpers. All workspace mutations reachable from this thread remain
// serialized; foreign STA messages are dispatched back to Windows.
DWORD WINAPI HotkeyThreadProc(LPVOID) {
  SetThreadDpiAwarenessContext(
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  const DWORD threadId = GetCurrentThreadId();
  g_wm.threadId.store(threadId, std::memory_order_release);
  Diagnostics::BeginWmSession();
  Wh_Log(L"Hotkey thread started, thread ID: %lu", threadId);

  // Readiness means the worker exists and owns a Windows message queue. Signal it
  // before any shell/COM acquisition so the controller can always request WM_QUIT
  // without waiting on Explorer readiness.
  MSG msg{};
  PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);
  if (g_wm.readyEvent) SetEvent(g_wm.readyEvent);

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_wm.comInitialized = SUCCEEDED(coHr);
  Wh_Log(L"CoInitializeEx result: 0x%08X", coHr);

  if (g_wm.comInitialized) {
    RuntimeLifecycle::RunMaintenanceNow();
  } else {
    Platform::WindowEvents::InstallWinEventHooks();
    Wh_Log(L"COM initialization failed; virtual desktop integration unavailable for this WM session");
  }

  Reconcile::ScheduleLifecycleReconcile(nullptr);
  RegisterConfiguredHotkeys();
  InitializeCurrentDesktopWorkspaces();

  if (!TrayUi::Initialize()) {
    Wh_Log(L"Tiling layout tray icon unavailable; window management remains active");
  }

  RunWmMessageLoop();

  // Stop callbacks from queueing new work as soon as the actor stops consuming
  // messages. Cleanup then unhooks delivery before clearing callback observations.
  g_wm.threadId.store(0, std::memory_order_release);
  CleanupWmThread();

  if (g_wm.comInitialized) CoUninitialize();
  g_wm.comInitialized = false;
  return 0;
}

bool StartHotkeyThread() {
  if (g_wm.thread) {
    Wh_Log(L"Refusing to start a second WM actor");
    return false;
  }

  g_wm.readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!g_wm.readyEvent) {
    Wh_Log(L"Failed to create WM readiness event: %lu", GetLastError());
    return false;
  }

  g_wm.thread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);
  if (!g_wm.thread) {
    Wh_Log(L"Failed to create WM thread: %lu", GetLastError());
    CloseHandle(g_wm.readyEvent);
    g_wm.readyEvent = nullptr;
    return false;
  }

  HANDLE waits[] = {g_wm.readyEvent, g_wm.thread};
  DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE);
  const bool ready = waitResult == WAIT_OBJECT_0;
  if (!ready) {
    Wh_Log(L"WM thread exited before signaling readiness (wait=%lu)", waitResult);
  }

  CloseHandle(g_wm.readyEvent);
  g_wm.readyEvent = nullptr;

  if (!ready) {
    WaitForSingleObject(g_wm.thread, INFINITE);
    CloseHandle(g_wm.thread);
    g_wm.thread = nullptr;
    g_wm.threadId.store(0, std::memory_order_release);
    return false;
  }
  return true;
}

bool StopHotkeyThread() {
  if (!g_wm.thread) return true;

  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (threadId && !PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
    Wh_Log(L"Failed to post WM_QUIT to WM thread: %lu", GetLastError());
  }

  const DWORD waitResult = WaitForSingleObject(g_wm.thread, 5000);
  if (waitResult != WAIT_OBJECT_0) {
    // During settings reload, retaining these handles prevents a second WM owner.
    // During WhTool_ModUninit, the standard tool-mod wrapper exits the dedicated
    // process immediately after the callback returns.
    Wh_Log(L"WM thread did not stop cleanly (wait=%lu); leaving the existing actor intact", waitResult);
    return false;
  }

  CloseHandle(g_wm.thread);
  g_wm.thread = nullptr;
  g_wm.threadId.store(0, std::memory_order_release);
  if (g_wm.readyEvent) {
    CloseHandle(g_wm.readyEvent);
    g_wm.readyEvent = nullptr;
  }
  return true;
}

