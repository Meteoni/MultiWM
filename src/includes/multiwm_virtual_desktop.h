//=============================================================================
// Windows platform adapters: private virtual desktops
//=============================================================================
//
// Everything below this point is shell/COM adaptation. The model, layout engine,
// reconciliation policy, commands, and WinEvent adapter above depend only on the
// narrow declarations near the top of the source.

namespace Platform::VirtualDesktop {

template <typename T>
T GetVTableFunction(void* pInterface, int index) {
  return reinterpret_cast<T>((*reinterpret_cast<void***>(pInterface))[index]);
}


//=============================================================================
// COM CLSIDs and IIDs for Virtual Desktop API (undocumented)
//=============================================================================

static const CLSID CLSID_ImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};

static const CLSID CLSID_VirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// Private shell service used for virtual-desktop change notifications.
// These identifiers and ABI generations mirror Windhawk's Taskbar Desktop
// Indicator, while the WM consumes them through its own message queue.
static const GUID SID_VirtualDesktopNotificationService = {
    0xA501FDEC, 0x4A09, 0x464C, {0xAE, 0x4E, 0x1B, 0x9C, 0x21, 0xB8, 0x49, 0x18}};

static const IID IID_IVirtualDesktopNotificationService = {
    0x0CD45E71, 0xD927, 0x4F15, {0x8B, 0x0A, 0x8F, 0xEF, 0x52, 0x53, 0x37, 0xBF}};

static const IID IID_IVirtualDesktopNotification_Old = {
    0xCD403E52, 0xDEED, 0x4C13, {0xB4, 0x37, 0xB9, 0x83, 0x80, 0xF2, 0xB1, 0xE8}};

static const IID IID_IVirtualDesktopNotification_Intermediate = {
    0xB287FA1C, 0x7771, 0x471A, {0xA2, 0xDF, 0x9B, 0x6B, 0x21, 0xF0, 0xD6, 0x75}};

static const IID IID_IVirtualDesktopNotification_Current = {
    0xB9E5E94D, 0x233E, 0x49AB, {0xAF, 0x5C, 0x2B, 0x45, 0x41, 0xC3, 0xAA, 0xDE}};

//=============================================================================
// Virtual Desktop private ABI identifiers
//=============================================================================

// Manager interface generations. Selection is automatic from explorer.exe's
// build; there is no user-facing Windows-version override.
static const IID IID_IVirtualDesktopManagerInternal_Win10Old = {
    0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}};
static const IID IID_IVirtualDesktopManagerInternal_Win10_20348 = {
    0x094AFE11, 0x44F2, 0x4BA0, {0x97, 0x6F, 0x29, 0xA9, 0x7E, 0x26, 0x3E, 0xE0}};
static const IID IID_IVirtualDesktopManagerInternal_Win11_22000 = {
    0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}};
static const IID IID_IVirtualDesktopManagerInternal_Win11_22621 = {
    0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}};
static const IID IID_IVirtualDesktopManagerInternal_Win11_26100 = {
    0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};

//=============================================================================
// COM Interface Definitions (undocumented, reverse-engineered)
//=============================================================================
struct IVirtualDesktop : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown*, BOOL*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetId(GUID*) = 0;
};

struct IVirtualDesktopManagerInternal : public IUnknown {};

MIDL_INTERFACE("0CD45E71-D927-4F15-8B0A-8FEF525337BF")
IVirtualDesktopNotificationService : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE Register(IUnknown* notification, DWORD* cookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE Unregister(DWORD cookie) = 0;
};

struct VirtualDesktopNotificationObject {
  void** vtable = nullptr;
  LONG refCount = 1;
};

bool UsesHMonitorParameter() { return g_vd.abi.usesHMonitor; }


//=============================================================================
// Virtual Desktop API + native change notifications
//=============================================================================

DesktopMetadata ReadDesktopMetadata(const GUID& desktopId) {
  // Read only at the initial-layout boundary. Missing shell metadata leaves the
  // corresponding condition unmatched; it never changes existing workspaces.
  DesktopMetadata desktop;
  DWORD sessionId = 0;
  const bool haveSession = ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
  const std::wstring registryPaths[] = {
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
      haveSession
          ? L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\" +
                std::to_wstring(sessionId) + L"\\VirtualDesktops"
          : std::wstring{}};

  for (const auto& path : registryPaths) {
    if (path.empty()) continue;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"VirtualDesktopIDs",
                     RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || bytes > 65536 || bytes % sizeof(GUID) != 0) {
      continue;
    }
    std::vector<GUID> ids(bytes / sizeof(GUID));
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"VirtualDesktopIDs",
                     RRF_RT_REG_BINARY, nullptr, ids.data(), &bytes) != ERROR_SUCCESS ||
        bytes % sizeof(GUID) != 0) {
      continue;
    }
    ids.resize(bytes / sizeof(GUID));
    auto found = std::find_if(ids.begin(), ids.end(), [&](const GUID& id) {
      return IsEqualGUID(id, desktopId);
    });
    if (found != ids.end()) {
      desktop.number = static_cast<int>(found - ids.begin()) + 1;
      break;
    }
  }

  wchar_t desktopIdText[39]{};
  if (!StringFromGUID2(desktopId, desktopIdText, ARRAYSIZE(desktopIdText))) {
    return desktop;
  }
  for (const auto& path : registryPaths) {
    if (path.empty()) continue;
    const std::wstring namePath = path + L"\\Desktops\\" + desktopIdText;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, namePath.c_str(), L"Name",
                     RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
      continue;
    }
    std::vector<wchar_t> name(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, namePath.c_str(), L"Name",
                     RRF_RT_REG_SZ, nullptr, name.data(), &bytes) == ERROR_SUCCESS &&
        name.front() != L'\0') {
      desktop.name = name.data();
      break;
    }
  }
  return desktop;
}

static const int VTABLE_GET_CURRENT_DESKTOP = 6;

static const NotificationInterfaceConfig& GetNotificationInterfaceConfig() {
  return g_vd.abi.notification;
}

// Resolves every private VD ABI choice from one observed Explorer version.
// Known gaps are rejected instead of guessing an undocumented manager IID.
static bool SelectVirtualDesktopAbiProfile(
    DWORD build, DWORD revision, VirtualDesktopAbiProfile* outProfile) {
  if (!outProfile) return false;

  VirtualDesktopAbiProfile profile{};

  if (build < 20348) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win10Old;
    profile.virtualDesktop = {0xFF72FFDD, 0xBE7E, 0x43FC, {0x9C, 0x03, 0xAD, 0x81, 0x68, 0x1E, 0x88, 0xE4}};
    profile.usesHMonitor = false;
  } else if (build < 22000) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win10_20348;
    profile.virtualDesktop = {0x62FDF88B, 0x11CA, 0x4AFB, {0x8B, 0xD8, 0x22, 0x96, 0xDF, 0xAE, 0x49, 0xE2}};
    profile.usesHMonitor = true;
  } else if (build < 22483 || (build == 22621 && revision < 2215)) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_22000;
    profile.virtualDesktop = {0x536D3495, 0xB208, 0x4CC9, {0xAE, 0x26, 0xDE, 0x81, 0x11, 0x27, 0x5B, 0xF8}};
    profile.usesHMonitor = true;
    if (build >= 22449) {
      profile.getDesktopsIndex = 8;
      profile.switchDesktopIndex = 10;
    }
  } else if (build >= 22621 && build < 26100) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_22621;
    profile.virtualDesktop = {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
    profile.usesHMonitor = false;
  } else if (build >= 26100) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_26100;
    profile.virtualDesktop = {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
    profile.usesHMonitor = false;
    profile.animatedSwitchIndex = 22;
    profile.animationWaitIndex = 24;
  } else {
    // The old manual table did not claim a manager ABI for Insider builds
    // 22483..22620. Keep that gap explicit rather than silently guessing.
    return false;
  }

  if (build >= 22000) {
    profile.createDesktopIndex = profile.switchDesktopIndex + 1;
    if (build >= 26100) ++profile.createDesktopIndex;
    profile.moveDesktopIndex = profile.createDesktopIndex + 1;
    profile.removeDesktopIndex = profile.createDesktopIndex + 2;
  }

  // Mirrors Windhawk's Taskbar Desktop Indicator notification ABI table. The
  // architecture metadata guarantees a 64-bit caller-cleanup ABI; retain the
  // compile guard as a belt-and-braces constraint around the manual sink.
#if defined(_WIN64)
  if (build >= 22000) {
    if (build < 22483 ||
        (build == 22621 && revision < 2215)) {
      profile.notification = {IID_IVirtualDesktopNotification_Old, 13, 11, true};
    } else if (build < 22631 ||
               (build == 22631 && revision < 3085)) {
      profile.notification = {
          IID_IVirtualDesktopNotification_Intermediate, 14, 10, false};
    } else {
      profile.notification = {IID_IVirtualDesktopNotification_Current, 14, 10, false};
    }
  }
#endif

  *outProfile = profile;
  return true;
}

static bool ReadExplorerVersion(DWORD* outBuild, DWORD* outRevision) {
  if (!outBuild || !outRevision) return false;

  wchar_t explorerPath[MAX_PATH]{};
  UINT length = GetWindowsDirectoryW(explorerPath, ARRAYSIZE(explorerPath));
  static constexpr wchar_t suffix[] = L"\\explorer.exe";
  if (!length || length >= ARRAYSIZE(explorerPath) ||
      length + ARRAYSIZE(suffix) > ARRAYSIZE(explorerPath)) {
    Wh_Log(L"Failed to resolve explorer.exe path for VD ABI detection");
    return false;
  }
  memcpy(explorerPath + length, suffix, sizeof(suffix));

  DWORD ignored = 0;
  DWORD versionSize = GetFileVersionInfoSizeW(explorerPath, &ignored);
  if (!versionSize) {
    Wh_Log(L"GetFileVersionInfoSize(explorer.exe) failed: %lu", GetLastError());
    return false;
  }

  std::vector<BYTE> versionData(versionSize);
  if (!GetFileVersionInfoW(explorerPath, 0, versionSize, versionData.data())) {
    Wh_Log(L"GetFileVersionInfo(explorer.exe) failed: %lu", GetLastError());
    return false;
  }

  VS_FIXEDFILEINFO* fixedInfo = nullptr;
  UINT fixedInfoSize = 0;
  if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&fixedInfo),
                      &fixedInfoSize) || !fixedInfo || fixedInfoSize < sizeof(*fixedInfo)) {
    Wh_Log(L"VerQueryValue(explorer.exe) failed");
    return false;
  }

  *outBuild = HIWORD(fixedInfo->dwFileVersionLS);
  *outRevision = LOWORD(fixedInfo->dwFileVersionLS);
  return true;
}

static bool ResolveVirtualDesktopAbiProfile() {
  DWORD build = 0;
  DWORD revision = 0;
  if (!ReadExplorerVersion(&build, &revision)) return false;

  VirtualDesktopAbiProfile profile{};
  if (!SelectVirtualDesktopAbiProfile(build, revision, &profile)) {
    Wh_Log(
        L"Unsupported Explorer virtual-desktop ABI: build=%lu revision=%lu",
        build, revision);
    return false;
  }

  g_vd.explorerBuild = build;
  g_vd.explorerRevision = revision;
  g_vd.abi = profile;
  g_vd.abiResolved = true;

  Wh_Log(
      L"Explorer virtual-desktop ABI resolved: build=%lu revision=%lu hmonitor=%d notifications=%d",
      build, revision, profile.usesHMonitor ? 1 : 0,
      profile.notification.methodCount);
  return true;
}

static bool IsCurrentNotificationInterface(REFIID riid) {
  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  return config.methodCount > 0 && InlineIsEqualGUID(riid, config.iid);
}

static void QueueVirtualDesktopChangedNotification() {
  const DWORD threadId = g_wm.threadId.load(std::memory_order_acquire);
  if (!threadId) return;

  // The callback is the transition boundary. Keep using the live shell query until
  // the queued handler has reconciled the new desktop into actor-owned state.
  if (g_wm.reconciledDesktopState != ReconciledDesktopState::Unknown) {
    g_wm.reconciledDesktopState =
        ReconciledDesktopState::TransitionPending;
  }

  // COM can emit redundant desktop notifications. Collapse them before they
  // reach the WM core, just as FancyWM coalesces layout invalidations.
  if (InterlockedExchange(&g_vd.changeQueued, 1) != 0) return;

  if (!PostThreadMessage(threadId, WM_APP_VIRTUAL_DESKTOP_CHANGED, 0, 0)) {
    InterlockedExchange(&g_vd.changeQueued, 0);
    Wh_Log(L"Failed to queue virtual desktop changed notification: %lu", GetLastError());
  }
}

static HRESULT STDMETHODCALLTYPE VirtualDesktopNotification_QueryInterface(
    VirtualDesktopNotificationObject* self, REFIID riid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;

  if (InlineIsEqualGUID(riid, IID_IUnknown) || IsCurrentNotificationInterface(riid)) {
    *object = self;
    InterlockedIncrement(&self->refCount);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE VirtualDesktopNotification_AddRef(
    VirtualDesktopNotificationObject* self) {
  return static_cast<ULONG>(InterlockedIncrement(&self->refCount));
}

static ULONG STDMETHODCALLTYPE VirtualDesktopNotification_Release(
    VirtualDesktopNotificationObject* self) {
  LONG refCount = InterlockedDecrement(&self->refCount);
  if (refCount == 0) {
    delete[] self->vtable;
    delete self;
  }
  return static_cast<ULONG>(std::max<LONG>(refCount, 0));
}

static HRESULT STDMETHODCALLTYPE VirtualDesktopNotification_NoOp() {
  return S_OK;
}

// Newer notification ABIs pass old/new IVirtualDesktop pointers after `self`.
// We deliberately don't consume those private-interface parameters here: the
// callback is the authoritative *signal*, then the WM thread queries the current
// GUID through its already-versioned manager interface. This follows Windhawk's
// compatibility pattern and avoids COM lifetime/cross-thread pointer hazards.
static HRESULT STDMETHODCALLTYPE VirtualDesktopNotification_CurrentChanged(
    VirtualDesktopNotificationObject*, void*, void*) {
  QueueVirtualDesktopChangedNotification();
  return S_OK;
}

// The oldest supported Windows 11 notification ABI inserts a monitor collection
// before old/new desktop arguments. The values are callback-owned COM inputs and
// are intentionally ignored rather than AddRef/Release'd.
static HRESULT STDMETHODCALLTYPE VirtualDesktopNotification_CurrentChangedWithMonitors(
    VirtualDesktopNotificationObject*, void*, void*, void*) {
  QueueVirtualDesktopChangedNotification();
  return S_OK;
}

// Builds the minimal COM sink required by the selected private ABI: IUnknown,
// one desktop-change callback, and no-op implementations for unrelated slots.
static VirtualDesktopNotificationObject* CreateVirtualDesktopNotificationObject() {
  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  if (config.methodCount <= 0 || config.currentChangedIndex < 3 ||
      config.currentChangedIndex >= config.methodCount) {
    return nullptr;
  }

  auto* object = new (std::nothrow) VirtualDesktopNotificationObject();
  if (!object) return nullptr;

  object->vtable = new (std::nothrow) void*[config.methodCount];
  if (!object->vtable) {
    delete object;
    return nullptr;
  }

  for (int i = 0; i < config.methodCount; ++i) {
    object->vtable[i] = reinterpret_cast<void*>(&VirtualDesktopNotification_NoOp);
  }
  object->vtable[0] = reinterpret_cast<void*>(&VirtualDesktopNotification_QueryInterface);
  object->vtable[1] = reinterpret_cast<void*>(&VirtualDesktopNotification_AddRef);
  object->vtable[2] = reinterpret_cast<void*>(&VirtualDesktopNotification_Release);
  if (config.currentChangedHasMonitors) {
    object->vtable[config.currentChangedIndex] =
        reinterpret_cast<void*>(&VirtualDesktopNotification_CurrentChangedWithMonitors);
  } else {
    object->vtable[config.currentChangedIndex] =
        reinterpret_cast<void*>(&VirtualDesktopNotification_CurrentChanged);
  }
  return object;
}

// Clears local notification ownership and, when Explorer is responsive, also
// unregisters and releases the shell service. Teardown still discards the stale
// cookie when COM calls are deliberately skipped to avoid an Explorer hang.
static void ClearVirtualDesktopNotificationState(bool unregisterFromShell) {
  g_vd.notificationsRegistered = false;
  InterlockedExchange(&g_vd.changeQueued, 0);

  if (g_vd.notificationService) {
    if (unregisterFromShell && g_vd.notificationCookie) {
      HRESULT hr = g_vd.notificationService->Unregister(g_vd.notificationCookie);
      if (FAILED(hr)) {
        Wh_Log(L"Virtual desktop notification Unregister failed: 0x%08X", hr);
      }
    }

    if (unregisterFromShell) {
      g_vd.notificationService->Release();
    }
    // If Explorer is unavailable, avoid a potentially blocking COM call/release.
    // The old server is going away; importantly, discard its cookie *before* a
    // new registration so a reused cookie can never unregister the new sink.
    g_vd.notificationService = nullptr;
  }

  if (g_vd.notificationObject) {
    VirtualDesktopNotification_Release(g_vd.notificationObject);
    g_vd.notificationObject = nullptr;
  }

  g_vd.notificationCookie = 0;
}

bool RegisterVirtualDesktopNotifications() {
  if (g_vd.notificationsRegistered) return true;
  if (!g_vd.abiResolved) return false;

  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  if (config.methodCount == 0) {
    // Win10 has no selected native notification ABI and deliberately uses settled
    // cloak/uncloak reconciliation instead.
    return true;
  }

  ++Diagnostics::g_runtime.counters.vdNotificationRegisterAttempts;
  g_vd.notificationLastAttemptTickMs = GetTickCount64();

  // Each attempt obtains a fresh ImmersiveShell provider, matching the robust
  // Taskbar Desktop Indicator pattern. Notification-service readiness can lag the
  // core VD manager during logon, so don't pin registration to the provider that
  // happened to initialize the manager first.
  IServiceProvider* notificationProvider = nullptr;
  HRESULT hr = CoCreateInstance(
      CLSID_ImmersiveShell, nullptr,
      CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
      reinterpret_cast<void**>(&notificationProvider));
  if (FAILED(hr) || !notificationProvider) {
    if (notificationProvider) notificationProvider->Release();
    g_vd.notificationLastHr = FAILED(hr) ? hr : E_NOINTERFACE;
    ++Diagnostics::g_runtime.counters.vdNotificationRegisterFailures;
    Wh_Log(L"VD notification registration: ImmersiveShell unavailable: 0x%08X", hr);
    return false;
  }

  if (g_vd.notificationService || g_vd.notificationObject ||
      g_vd.notificationCookie) {
    ClearVirtualDesktopNotificationState(true);
  }

  hr = notificationProvider->QueryService(
      SID_VirtualDesktopNotificationService, IID_IVirtualDesktopNotificationService,
      reinterpret_cast<void**>(&g_vd.notificationService));
  notificationProvider->Release();
  if (FAILED(hr) || !g_vd.notificationService) {
    SAFE_RELEASE(g_vd.notificationService);
    g_vd.notificationLastHr = FAILED(hr) ? hr : E_NOINTERFACE;
    ++Diagnostics::g_runtime.counters.vdNotificationRegisterFailures;
    Wh_Log(L"VD notification registration: service unavailable: 0x%08X", hr);
    return false;
  }

  g_vd.notificationObject = CreateVirtualDesktopNotificationObject();
  if (!g_vd.notificationObject) {
    g_vd.notificationLastHr = E_OUTOFMEMORY;
    ++Diagnostics::g_runtime.counters.vdNotificationRegisterFailures;
    SAFE_RELEASE(g_vd.notificationService);
    Wh_Log(L"VD notification registration: failed to create sink");
    return false;
  }

  DWORD cookie = 0;
  hr = g_vd.notificationService->Register(
      reinterpret_cast<IUnknown*>(g_vd.notificationObject), &cookie);
  if (FAILED(hr) || cookie == 0) {
    g_vd.notificationLastHr = FAILED(hr) ? hr : E_FAIL;
    ++Diagnostics::g_runtime.counters.vdNotificationRegisterFailures;
    Wh_Log(L"VD notification registration failed: 0x%08X cookie=%lu", hr, cookie);
    VirtualDesktopNotification_Release(g_vd.notificationObject);
    g_vd.notificationObject = nullptr;
    SAFE_RELEASE(g_vd.notificationService);
    return false;
  }

  g_vd.notificationCookie = cookie;
  g_vd.notificationsRegistered = true;
  if (!g_vd.shellPid) {
    HWND shell = GetShellWindow();
    if (shell && IsWindow(shell)) GetWindowThreadProcessId(shell, &g_vd.shellPid);
  }
  g_vd.notificationLastHr = S_OK;
  g_vd.notificationLastSuccessTickMs = GetTickCount64();
  ++Diagnostics::g_runtime.counters.vdNotificationRegisterSuccesses;
  Diagnostics::RecordEvent(
      L"virtual desktop notifications registered cookie=%lu afterAttempt=%llu",
      cookie,
      static_cast<unsigned long long>(
          Diagnostics::g_runtime.counters.vdNotificationRegisterAttempts));
  Wh_Log(L"Native virtual desktop notifications registered (cookie=%lu)", cookie);
  return true;
}

// Acquires the COM interfaces needed for virtual-desktop queries. Notification
// subscription is deliberately owned by the centralized runtime-maintenance path.
bool InitializeVirtualDesktopAPIOnce() {
  ++Diagnostics::g_runtime.counters.vdApiInitAttempts;
  if (!ResolveVirtualDesktopAbiProfile()) {
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  HRESULT hr = CoCreateInstance(
      CLSID_ImmersiveShell, nullptr,
      CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
      reinterpret_cast<void**>(&g_vd.serviceProvider));
  if (FAILED(hr) || !g_vd.serviceProvider) {
    SAFE_RELEASE(g_vd.serviceProvider);
    Wh_Log(L"Failed to create ImmersiveShell: 0x%08X", hr);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  if (g_vd.explorerBuild >= 22621) {
    hr = g_vd.serviceProvider->QueryService(
        CLSID_VirtualDesktopManagerInternal, IID_IVirtualDesktopManagerInternal_Win11_26100,
        reinterpret_cast<void**>(&g_vd.managerInternal));
    if (SUCCEEDED(hr) && g_vd.managerInternal) {
      g_vd.abi.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_26100;
      g_vd.abi.virtualDesktop = {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
      g_vd.abi.usesHMonitor = false;
      g_vd.abi.getDesktopsIndex = 7;
      g_vd.abi.switchDesktopIndex = 9;
      // MScholtes supplies separate 23H2/24H2 definitions for this same IID:
      // 24H2 inserts SwitchDesktopAndMoveForegroundView at slot 10.
      g_vd.abi.animatedSwitchIndex = g_vd.explorerBuild >= 26100 ? 22 : 21;
      g_vd.abi.animationWaitIndex = g_vd.abi.animatedSwitchIndex + 2;
      g_vd.abi.createDesktopIndex = g_vd.explorerBuild >= 26100 ? 11 : 10;
      g_vd.abi.moveDesktopIndex = g_vd.abi.createDesktopIndex + 1;
      g_vd.abi.removeDesktopIndex = g_vd.abi.createDesktopIndex + 2;
    }
  }
  if (!g_vd.managerInternal) {
    hr = g_vd.serviceProvider->QueryService(
        CLSID_VirtualDesktopManagerInternal, g_vd.abi.managerInternal,
        reinterpret_cast<void**>(&g_vd.managerInternal));
  }
  if (FAILED(hr) || !g_vd.managerInternal) {
    Wh_Log(L"Failed to get VirtualDesktopManagerInternal: 0x%08X", hr);
    SAFE_RELEASE(g_vd.managerInternal);
    SAFE_RELEASE(g_vd.serviceProvider);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  hr = CoCreateInstance(
      CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
      IID_IVirtualDesktopManager,
      reinterpret_cast<void**>(&g_vd.desktopManager));
  if (FAILED(hr) || !g_vd.desktopManager) {
    Wh_Log(L"Failed to create VirtualDesktopManager: 0x%08X", hr);
    SAFE_RELEASE(g_vd.desktopManager);
    SAFE_RELEASE(g_vd.managerInternal);
    SAFE_RELEASE(g_vd.serviceProvider);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  HWND shell = GetShellWindow();
  g_vd.shellPid = 0;
  if (shell && IsWindow(shell)) {
    GetWindowThreadProcessId(shell, &g_vd.shellPid);
  }

  ++Diagnostics::g_runtime.counters.vdApiInitSuccesses;
  return true;
}

static void ResetVirtualDesktopMetadata() {
  g_vd.abi = {};
  g_vd.abiResolved = false;
  g_vd.explorerBuild = 0;
  g_vd.explorerRevision = 0;
  g_vd.shellPid = 0;
  g_vd.lastKnownDesktop = {};
  g_vd.lastKnownDesktopValid = false;
  g_vd.notificationLastHr = S_OK;
  g_vd.notificationLastAttemptTickMs = 0;
  g_vd.notificationLastSuccessTickMs = 0;
  g_vd.initialized = false;
}

// Releases virtual-desktop COM state while Explorer is responsive. During shell
// teardown, pointers are intentionally dropped without Release to avoid hangs.
void CleanupVirtualDesktopAPI() {
  HWND hShell = GetShellWindow();
  DWORD currentShellPid = 0;
  const bool shellAvailable = hShell && IsWindow(hShell);
  if (shellAvailable) GetWindowThreadProcessId(hShell, &currentShellPid);
  const bool sameShell =
      shellAvailable && g_vd.shellPid != 0 && currentShellPid == g_vd.shellPid;

  // A replacement Explorer can already have a valid ShellWindow while our COM
  // proxies still point at the dead predecessor. Never call Unregister/Release on
  // those stale proxies merely because *some* shell is now visible.
  ClearVirtualDesktopNotificationState(sameShell);
  if (!sameShell) {
    if (g_vd.initialized || g_vd.serviceProvider || g_vd.managerInternal ||
        g_vd.desktopManager) {
      Wh_Log(L"Explorer identity changed/unavailable, abandoning stale VD COM proxies");
    }
    g_vd.desktopManager = nullptr;
    g_vd.managerInternal = nullptr;
    g_vd.serviceProvider = nullptr;
    ResetVirtualDesktopMetadata();
    return;
  }

  SAFE_RELEASE(g_vd.desktopManager);
  SAFE_RELEASE(g_vd.managerInternal);
  SAFE_RELEASE(g_vd.serviceProvider);
  ResetVirtualDesktopMetadata();
}

// TaskbarCreated means the Explorer instance backing these private proxies has
// changed. Never call stale shell proxies merely because the replacement shell is
// already visible; discard them and let the one maintenance owner reacquire all.
void AbandonVirtualDesktopAPIForShellRestart() {
  ClearVirtualDesktopNotificationState(false);
  g_vd.desktopManager = nullptr;
  g_vd.managerInternal = nullptr;
  g_vd.serviceProvider = nullptr;
  ResetVirtualDesktopMetadata();
}

bool InitializeVirtualDesktopAPI() {
  if (g_vd.initialized) return true;
  if (InitializeVirtualDesktopAPIOnce()) {
    g_vd.initialized = true;
    return true;
  }
  RuntimeLifecycle::RequestMaintenance(false);
  return false;
}

bool ReinitializeVirtualDesktopAPI() {
  ++Diagnostics::g_runtime.counters.vdApiReinitializations;
  Diagnostics::RecordEvent(L"virtual desktop core API reinitialization requested");
  CleanupVirtualDesktopAPI();
  const bool ready = InitializeVirtualDesktopAPI();
  // A previously-live COM path failed, so give the centralized recovery owner a
  // fresh retry burst. Notification registration remains owned there.
  RuntimeLifecycle::RequestMaintenance(true);
  return ready;
}

// Per-window IVirtualDesktopManager calls can legitimately reject or lose an
// individual HWND. Rebuild the shared COM stack only when the proxy/server itself
// is no longer usable.
static bool IsDeadVirtualDesktopProxy(HRESULT hr) {
  return hr == RPC_E_DISCONNECTED ||
         hr == RPC_E_CONNECTION_TERMINATED ||
         hr == RPC_E_SERVER_DIED ||
         hr == RPC_E_SERVER_DIED_DNE ||
         hr == RPC_E_INVALID_IPID ||
         hr == RPC_E_INVALID_OBJECT ||
         hr == CO_E_OBJNOTCONNECTED ||
         hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
}

// Invokes a private manager slot using the Windows-version-specific signature.
// A failed call forces one core-API reinitialization and retry before returning.
template <typename TResult>
HRESULT CallManagerInternal(int vtableIndex, TResult* outResult, bool enumerateDesktops = false) {
  HRESULT hr = E_NOINTERFACE;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!g_vd.managerInternal) return hr;
    const int slot = enumerateDesktops ? g_vd.abi.getDesktopsIndex : vtableIndex;
    if (UsesHMonitorParameter()) {
      const auto function = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, TResult*)>(g_vd.managerInternal, slot);
      hr = function(g_vd.managerInternal, nullptr, outResult);
    } else {
      const auto function = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, TResult*)>(g_vd.managerInternal, slot);
      hr = function(g_vd.managerInternal, outResult);
    }
    if (SUCCEEDED(hr) || attempt != 0 || !ReinitializeVirtualDesktopAPI()) break;
  }
  return hr;
}

// Enumeration/movement follow the shell interfaces used by Virtual Desktop
// Helper and VD.ahk. Animation and its versioned slots follow MScholtes'
// separate VirtualDesktop11.cs and VirtualDesktop11-24H2.cs definitions.
bool GetDesktopIdsInOrder(std::vector<GUID>* desktopIds) {
  if (!desktopIds) return false;
  desktopIds->clear();
  if (!InitializeVirtualDesktopAPI()) return false;
  IObjectArray* desktops = nullptr;
  HRESULT hr = CallManagerInternal(g_vd.abi.getDesktopsIndex, &desktops, true);
  if (FAILED(hr) || !desktops) {
    SAFE_RELEASE(desktops);
    return false;
  }
  UINT count = 0;
  hr = desktops->GetCount(&count);
  for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index) {
    IVirtualDesktop* desktop = nullptr;
    hr = desktops->GetAt(index, g_vd.abi.virtualDesktop, reinterpret_cast<void**>(&desktop));
    GUID id{};
    if (SUCCEEDED(hr) && desktop) hr = desktop->GetId(&id);
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    SAFE_RELEASE(desktop);
    if (SUCCEEDED(hr)) desktopIds->push_back(id);
  }
  desktops->Release();
  if (FAILED(hr)) desktopIds->clear();
  return SUCCEEDED(hr);
}

// Resolve a GUID against a fresh shell array at each mutation boundary. Holding
// an ordinal or a desktop pointer across a dynamic-desktop update is unsafe.
static IVirtualDesktop* FindLiveDesktop(const GUID& desktopId) {
  if (!InitializeVirtualDesktopAPI()) return nullptr;
  IObjectArray* desktops = nullptr;
  HRESULT hr = CallManagerInternal(g_vd.abi.getDesktopsIndex, &desktops, true);
  IUnknown* const manager = g_vd.managerInternal;
  if (FAILED(hr) || !desktops) {
    SAFE_RELEASE(desktops);
    return nullptr;
  }
  UINT count = 0;
  if (FAILED(desktops->GetCount(&count))) count = 0;
  IVirtualDesktop* found = nullptr;
  for (UINT index = 0; index < count; ++index) {
    IVirtualDesktop* desktop = nullptr;
    GUID id{};
    if (SUCCEEDED(desktops->GetAt(index, g_vd.abi.virtualDesktop, reinterpret_cast<void**>(&desktop))) &&
        desktop && SUCCEEDED(desktop->GetId(&id)) && IsEqualGUID(id, desktopId)) {
      found = desktop;
      break;
    }
    SAFE_RELEASE(desktop);
  }
  desktops->Release();
  if (g_vd.managerInternal != manager) SAFE_RELEASE(found);
  return found;
}

// Read shell views rather than guessing occupancy from cloaked top-level HWNDs.
// An unregistered ApplicationFrameWindow is absent here and cannot block routing.
bool IsDesktopEmptyExcept(const GUID& desktopId, HWND allowedWindow) {
  if (!InitializeVirtualDesktopAPI()) return false;
  IUnknown* const manager = g_vd.managerInternal;
  const IID collectionIid =
      {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
  const IID viewIid =
      {0x372E1D3B, 0x38D3, 0x42E4, {0xA1, 0x5B, 0x8A, 0xB2, 0xB1, 0x78, 0xF5, 0x13}};
  IUnknown* collection = nullptr;
  IObjectArray* views = nullptr;
  IUnknown* allowedIdentity = nullptr;
  HRESULT hr = g_vd.serviceProvider->QueryService(
      collectionIid, collectionIid, reinterpret_cast<void**>(&collection));
  if (SUCCEEDED(hr) && collection && allowedWindow) {
    IUnknown* view = nullptr;
    hr = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HWND, IUnknown**)>(collection, 6)(
        collection, allowedWindow, &view);
    if (SUCCEEDED(hr) && view) hr = view->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&allowedIdentity));
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    SAFE_RELEASE(view);
  }
  if (SUCCEEDED(hr) && collection) {
    hr = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IObjectArray**)>(collection, 3)(collection, &views);
  } else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
  UINT count = 0;
  if (SUCCEEDED(hr) && views) hr = views->GetCount(&count);
  else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
  bool empty = SUCCEEDED(hr);
  for (UINT index = 0; empty && index < count; ++index) {
    IUnknown* view = nullptr;
    IUnknown* identity = nullptr;
    GUID owner{};
    hr = views->GetAt(index, viewIid, reinterpret_cast<void**>(&view));
    if (SUCCEEDED(hr) && view) {
      // IApplicationView derives from IInspectable; GetVirtualDesktopId is slot 25.
      hr = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, GUID*)>(view, 25)(view, &owner);
      if (SUCCEEDED(hr) && IsEqualGUID(owner, desktopId)) {
        hr = view->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity));
        empty = SUCCEEDED(hr) && allowedIdentity && identity == allowedIdentity;
      }
    } else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    if (FAILED(hr)) empty = false;
    SAFE_RELEASE(identity);
    SAFE_RELEASE(view);
  }
  SAFE_RELEASE(allowedIdentity);
  SAFE_RELEASE(views);
  SAFE_RELEASE(collection);
  return empty && g_vd.managerInternal == manager;
}

bool CreateDesktopAfter(const GUID& sourceDesktop, GUID* desktopId, int* desktopNumber) {
  if (!desktopId || !desktopNumber) return false;
  *desktopId = {};
  *desktopNumber = 0;
  std::vector<GUID> order;
  GUID current{};
  if (!GetDesktopIdsInOrder(&order) || !GetCurrentDesktopId(&current) ||
      !IsEqualGUID(current, sourceDesktop)) return false;
  auto source = std::find_if(order.begin(), order.end(),
      [&](const GUID& id) { return IsEqualGUID(id, sourceDesktop); });
  if (source == order.end() ||
      (g_vd.abi.moveDesktopIndex < 0 && source + 1 != order.end())) return false;
  IUnknown* const manager = g_vd.managerInternal;
  IVirtualDesktop* created = nullptr;
  // Creation is a mutation: never retry it after a lost COM response.
  HRESULT hr = UsesHMonitorParameter()
      ? GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, IVirtualDesktop**)>(manager, g_vd.abi.createDesktopIndex)(manager, nullptr, &created)
      : GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop**)>(manager, g_vd.abi.createDesktopIndex)(manager, &created);
  if (created) created->GetId(desktopId);
  SAFE_RELEASE(created);
  if (IsEqualGUID(*desktopId, GUID_NULL)) {
    Diagnostics::RecordEvent(L"overflow desktop creation returned no identity hr=%08X", hr);
    if (IsDeadVirtualDesktopProxy(hr)) RuntimeLifecycle::RequestMaintenance(true);
    return false;
  }
  if (!GetDesktopIdsInOrder(&order) || g_vd.managerInternal != manager ||
      !GetCurrentDesktopId(&current) || !IsEqualGUID(current, sourceDesktop)) return false;
  source = std::find_if(order.begin(), order.end(),
      [&](const GUID& id) { return IsEqualGUID(id, sourceDesktop); });
  auto target = std::find_if(order.begin(), order.end(),
      [&](const GUID& id) { return IsEqualGUID(id, *desktopId); });
  if (source == order.end() || target == order.end()) return false;
  if (target != source + 1) {
    if (g_vd.abi.moveDesktopIndex < 0 || !IsDesktopEmptyExcept(*desktopId) ||
        g_vd.managerInternal != manager || !GetCurrentDesktopId(&current) ||
        !IsEqualGUID(current, sourceDesktop)) return false;
    const int index = static_cast<int>(source - order.begin()) + (target < source ? 0 : 1);
    created = FindLiveDesktop(*desktopId);
    if (!created || g_vd.managerInternal != manager) { SAFE_RELEASE(created); return false; }
    hr = UsesHMonitorParameter()
        ? GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop*, HMONITOR, int)>(manager, g_vd.abi.moveDesktopIndex)(manager, created, nullptr, index)
        : GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop*, int)>(manager, g_vd.abi.moveDesktopIndex)(manager, created, index);
    created->Release();
  }
  if (!GetDesktopIdsInOrder(&order) || g_vd.managerInternal != manager) return false;
  source = std::find_if(order.begin(), order.end(),
      [&](const GUID& id) { return IsEqualGUID(id, sourceDesktop); });
  target = std::find_if(order.begin(), order.end(),
      [&](const GUID& id) { return IsEqualGUID(id, *desktopId); });
  const bool inserted = source != order.end() && target != order.end() && target == source + 1;
  if (inserted) *desktopNumber = static_cast<int>(target - order.begin()) + 1;
  Diagnostics::RecordEvent(L"overflow desktop creation target=%08X hr=%08X inserted=%d", desktopId->Data1, hr, inserted ? 1 : 0);
  if (IsDeadVirtualDesktopProxy(hr)) RuntimeLifecycle::RequestMaintenance(true);
  return inserted;
}

// Only callers holding the GUID returned by their own creation may use this.
// Never delete a selected desktop or one containing any shell application view.
bool RemoveEmptyOverflowDesktop(const GUID& desktopId, const GUID& fallbackDesktop) {
  if (IsEqualGUID(desktopId, GUID_NULL) || IsEqualGUID(desktopId, fallbackDesktop)) return false;
  IVirtualDesktop* desktop = FindLiveDesktop(desktopId);
  IUnknown* const manager = g_vd.managerInternal;
  IVirtualDesktop* fallback = FindLiveDesktop(fallbackDesktop);
  GUID current{};
  bool removed = false;
  if (desktop && fallback && IsDesktopEmptyExcept(desktopId) &&
      GetCurrentDesktopId(&current) && !IsEqualGUID(current, desktopId) &&
      g_vd.managerInternal == manager) {
    HRESULT hr = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop*, IVirtualDesktop*)>(
        manager, g_vd.abi.removeDesktopIndex)(manager, desktop, fallback);
    removed = SUCCEEDED(hr);
    Diagnostics::RecordEvent(L"overflow empty desktop cleanup target=%08X hr=%08X", desktopId.Data1, hr);
    if (IsDeadVirtualDesktopProxy(hr)) RuntimeLifecycle::RequestMaintenance(true);
  }
  SAFE_RELEASE(desktop);
  SAFE_RELEASE(fallback);
  return removed;
}

bool MoveWindowToDesktop(HWND hwnd, DWORD processId, const GUID& sourceDesktop, const GUID& desktopId) {
  IVirtualDesktop* desktop = FindLiveDesktop(desktopId);
  if (!desktop) return false;
  IUnknown* const manager = g_vd.managerInternal;
  const IID viewCollectionIid =
      {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
  IUnknown* collection = nullptr;
  IUnknown* view = nullptr;
  HRESULT hr = g_vd.serviceProvider->QueryService(
      viewCollectionIid, viewCollectionIid, reinterpret_cast<void**>(&collection));
  if (SUCCEEDED(hr) && collection) {
    const auto getView = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HWND, IUnknown**)>(collection, 6);
    hr = getView(collection, hwnd, &view);
  } else if (SUCCEEDED(hr)) {
    hr = E_NOINTERFACE;
  }
  IUnknown* pinnedApps = nullptr;
  const CLSID pinnedAppsService =
      {0xB5A399E7, 0x1C87, 0x46B8, {0x88, 0xE9, 0xFC, 0x57, 0x47, 0xB1, 0x71, 0xBD}};
  const IID pinnedAppsIid =
      {0x4CE81583, 0x1E4C, 0x4632, {0xA6, 0x21, 0x07, 0xA5, 0x35, 0x43, 0x14, 0x8F}};
  BOOL pinned = TRUE;
  if (SUCCEEDED(hr) && view) {
    hr = g_vd.managerInternal == manager && g_vd.serviceProvider
        ? g_vd.serviceProvider->QueryService(pinnedAppsService, pinnedAppsIid, reinterpret_cast<void**>(&pinnedApps))
        : E_ABORT;
    if (SUCCEEDED(hr) && pinnedApps) {
      const auto isPinned = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, BOOL*)>(pinnedApps, 6);
      hr = isPinned(pinnedApps, view, &pinned);
    }
    if (SUCCEEDED(hr) && pinned) hr = E_ACCESSDENIED;
  }
  SAFE_RELEASE(pinnedApps);
  BOOL canMove = FALSE;
  if (SUCCEEDED(hr) && view && g_vd.managerInternal == manager) {
    const auto canMoveView = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, BOOL*)>(g_vd.managerInternal, 5);
    hr = canMoveView(g_vd.managerInternal, view, &canMove);
  }
  DWORD livePid = 0;
  GUID owner{};
  GetWindowThreadProcessId(hwnd, &livePid);
  if (SUCCEEDED(hr) && view && canMove && IsWindow(hwnd) && livePid == processId &&
      g_wm.overflowWindows.count(hwnd) &&
      GetWindowDesktopIdSafe(hwnd, &owner) && IsEqualGUID(owner, sourceDesktop) &&
      g_vd.managerInternal == manager) {
    const auto moveView = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, IVirtualDesktop*)>(g_vd.managerInternal, 4);
    hr = moveView(g_vd.managerInternal, view, desktop);
  } else if (SUCCEEDED(hr)) {
    hr = E_ACCESSDENIED;
  }
  SAFE_RELEASE(view);
  SAFE_RELEASE(collection);
  desktop->Release();
  GUID observed{};
  // A mutation can complete despite a lost COM response. Never blindly repeat it.
  GetWindowThreadProcessId(hwnd, &livePid);
  const bool moved = livePid == processId && g_wm.overflowWindows.count(hwnd) &&
                     GetWindowDesktopIdSafe(hwnd, &observed) &&
                     IsEqualGUID(observed, desktopId);
  Diagnostics::RecordEvent(L"overflow desktop move hwnd=%p target=%08X hr=%08X observedSuccess=%d",
                           hwnd, desktopId.Data1, hr, moved ? 1 : 0);
  if (IsDeadVirtualDesktopProxy(hr)) RuntimeLifecycle::RequestMaintenance(true);
  return moved;
}

bool SwitchToDesktop(HWND hwnd, DWORD processId, const GUID& desktopId, const GUID& sourceDesktop, DWORD inputTickMs) {
  IVirtualDesktop* desktop = FindLiveDesktop(desktopId);
  if (!desktop) return false;
  IUnknown* const manager = g_vd.managerInternal;
  auto canFollow = [&] {
    LASTINPUTINFO input{sizeof(input)};
    GUID current{}, owner{};
    DWORD livePid = 0;
    GetWindowThreadProcessId(hwnd, &livePid);
    return IsWindow(hwnd) && livePid == processId && g_wm.overflowWindows.count(hwnd) &&
           GetLastInputInfo(&input) && input.dwTime == inputTickMs &&
           GetWindowDesktopIdSafe(hwnd, &owner) && IsEqualGUID(owner, desktopId) &&
           GetCurrentDesktopId(&current) && IsEqualGUID(current, sourceDesktop) &&
           g_vd.managerInternal == manager;
  };
  if (!canFollow()) {
    desktop->Release();
    return false;
  }
  HRESULT hr = E_NOINTERFACE;
  bool animated = false;
  if (g_vd.abi.animatedSwitchIndex >= 0) {
    // The reference waits before a new animation to avoid crashing Explorer
    // when another switch is still animating. Wait for completion before
    // ordinary workspace reconciliation resumes.
    const auto waitForAnimation = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*)>(
        g_vd.managerInternal, g_vd.abi.animationWaitIndex);
    hr = waitForAnimation(g_vd.managerInternal);
    // An existing animation can outlast the user's decision to stay elsewhere.
    if (!canFollow()) {
      desktop->Release();
      return false;
    }
    if (SUCCEEDED(hr)) {
      const auto switchAnimated = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop*)>(
          g_vd.managerInternal, g_vd.abi.animatedSwitchIndex);
      hr = switchAnimated(g_vd.managerInternal, desktop);
      animated = SUCCEEDED(hr);
      if (animated && g_vd.managerInternal == manager) {
        const HRESULT waitHr = waitForAnimation(g_vd.managerInternal);
        if (FAILED(waitHr)) Wh_Log(L"Desktop animation completion wait failed: 0x%08X", waitHr);
      }
    }
  }
  if (g_vd.managerInternal == manager &&
      (g_vd.abi.animatedSwitchIndex < 0 || hr == E_NOTIMPL || hr == E_NOINTERFACE) && canFollow()) {
    if (g_vd.abi.usesHMonitor) {
      const auto switchDesktop = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, IVirtualDesktop*)>(
          g_vd.managerInternal, g_vd.abi.switchDesktopIndex);
      hr = switchDesktop(g_vd.managerInternal, nullptr, desktop);
    } else {
      const auto switchDesktop = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, IVirtualDesktop*)>(
          g_vd.managerInternal, g_vd.abi.switchDesktopIndex);
      hr = switchDesktop(g_vd.managerInternal, desktop);
    }
  }
  desktop->Release();
  GUID current{};
  const bool accepted = SUCCEEDED(hr) || (GetCurrentDesktopId(&current) && IsEqualGUID(current, desktopId));
  Diagnostics::RecordEvent(L"overflow desktop switch target=%08X hr=%08X accepted=%d animated=%d", desktopId.Data1, hr, accepted ? 1 : 0, animated ? 1 : 0);
  if (IsDeadVirtualDesktopProxy(hr)) RuntimeLifecycle::RequestMaintenance(true);
  return accepted;
}

bool GetCurrentDesktopId(GUID* outGuid) {
  if (!outGuid) return false;
  if (!g_vd.managerInternal) {
    g_vd.lastKnownDesktopValid = false;
    RuntimeLifecycle::RequestMaintenance(false);
    return false;
  }

  // GetCurrentDesktop itself is already retried once by CallManagerInternal. A
  // stale IVirtualDesktop can still fail at GetId after Explorer replacement, so
  // allow one full core reacquisition for that distinct failure boundary too.
  for (int attempt = 0; attempt < 2; ++attempt) {
    IVirtualDesktop* desktop = nullptr;
    HRESULT hr = CallManagerInternal(VTABLE_GET_CURRENT_DESKTOP, &desktop);
    if (FAILED(hr) || !desktop) {
      g_vd.lastKnownDesktopValid = false;
      RuntimeLifecycle::RequestMaintenance(false);
      Wh_Log(L"GetCurrentDesktop failed: 0x%08X", hr);
      return false;
    }

    hr = desktop->GetId(outGuid);
    desktop->Release();
    if (SUCCEEDED(hr)) {
      g_vd.lastKnownDesktop = *outGuid;
      g_vd.lastKnownDesktopValid = true;
      return true;
    }

    if (attempt == 0 && ReinitializeVirtualDesktopAPI()) continue;
    Wh_Log(L"Current virtual desktop GetId failed after recovery: 0x%08X", hr);
  }

  g_vd.lastKnownDesktopValid = false;
  RuntimeLifecycle::RequestMaintenance(false);
  return false;
}

bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid) {
  if (!hwnd || !outGuid) return false;
  if (!g_vd.initialized || !g_vd.desktopManager) {
    RuntimeLifecycle::RequestMaintenance(false);
    return false;
  }

  HRESULT hr = g_vd.desktopManager->GetWindowDesktopId(hwnd, outGuid);
  if (FAILED(hr) && IsDeadVirtualDesktopProxy(hr) &&
      ReinitializeVirtualDesktopAPI() && g_vd.desktopManager) {
    hr = g_vd.desktopManager->GetWindowDesktopId(hwnd, outGuid);
  }
  if (FAILED(hr) && IsDeadVirtualDesktopProxy(hr)) {
    RuntimeLifecycle::RequestMaintenance(false);
  }
  return SUCCEEDED(hr);
}

bool IsWindowOnCurrentDesktopSafe(HWND hwnd, BOOL* onCurrent) {
  if (!hwnd || !onCurrent) return false;
  if (!g_vd.initialized || !g_vd.desktopManager) {
    RuntimeLifecycle::RequestMaintenance(false);
    return false;
  }

  HRESULT hr = g_vd.desktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, onCurrent);
  if (FAILED(hr) && IsDeadVirtualDesktopProxy(hr) &&
      ReinitializeVirtualDesktopAPI() && g_vd.desktopManager) {
    hr = g_vd.desktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, onCurrent);
  }
  if (FAILED(hr) && IsDeadVirtualDesktopProxy(hr)) {
    RuntimeLifecycle::RequestMaintenance(false);
  }
  return SUCCEEDED(hr);
}



}  // namespace Platform::VirtualDesktop

