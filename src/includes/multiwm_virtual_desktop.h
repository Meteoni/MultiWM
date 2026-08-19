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
    profile.usesHMonitor = false;
  } else if (build < 22000) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win10_20348;
    profile.usesHMonitor = true;
  } else if (build < 22483) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_22000;
    profile.usesHMonitor = true;
  } else if (build >= 22621 && build < 26100) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_22621;
    profile.usesHMonitor = false;
  } else if (build >= 26100) {
    profile.managerInternal = IID_IVirtualDesktopManagerInternal_Win11_26100;
    profile.usesHMonitor = false;
  } else {
    // The old manual table did not claim a manager ABI for Insider builds
    // 22483..22620. Keep that gap explicit rather than silently guessing.
    return false;
  }

  // Mirrors Windhawk's Taskbar Desktop Indicator notification ABI table.
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
  if (!g_wm.threadId) return;

  // COM can emit redundant desktop notifications. Collapse them before they
  // reach the WM core, just as FancyWM coalesces layout invalidations.
  if (InterlockedExchange(&g_vd.changeQueued, 1) != 0) return;

  if (!PostThreadMessage(g_wm.threadId, WM_APP_VIRTUAL_DESKTOP_CHANGED, 0, 0)) {
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
    VirtualDesktopNotificationObject*) {
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
  if (!g_vd.serviceProvider || !g_vd.abiResolved) return false;

  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  if (config.methodCount == 0) {
    Wh_Log(L"Native VD notifications unavailable for Explorer build %lu.%lu; using WinEvent settle fallback",
           g_vd.explorerBuild, g_vd.explorerRevision);
    return false;
  }

  // Never carry an old cookie/sink into a new shell registration. Explorer can
  // reuse notification cookies after a restart. Normal reinitialization already
  // clears this state; this is a defensive cleanup for partial/stale attempts.
  if (g_vd.notificationService || g_vd.notificationObject ||
      g_vd.notificationCookie) {
    HWND shell = GetShellWindow();
    ClearVirtualDesktopNotificationState(shell && IsWindow(shell));
  }

  HRESULT hr = g_vd.serviceProvider->QueryService(
      SID_VirtualDesktopNotificationService, IID_IVirtualDesktopNotificationService,
      reinterpret_cast<void**>(&g_vd.notificationService));
  if (FAILED(hr) || !g_vd.notificationService) {
    Wh_Log(L"Failed to get VirtualDesktopNotificationService: 0x%08X", hr);
    g_vd.notificationService = nullptr;
    return false;
  }

  g_vd.notificationObject = CreateVirtualDesktopNotificationObject();
  if (!g_vd.notificationObject) {
    Wh_Log(L"Failed to create virtual desktop notification sink");
    SAFE_RELEASE(g_vd.notificationService);
    return false;
  }

  DWORD cookie = 0;
  hr = g_vd.notificationService->Register(
      reinterpret_cast<IUnknown*>(g_vd.notificationObject), &cookie);
  if (FAILED(hr) || cookie == 0) {
    Wh_Log(L"Virtual desktop notification Register failed: 0x%08X cookie=%lu", hr, cookie);
    VirtualDesktopNotification_Release(g_vd.notificationObject);
    g_vd.notificationObject = nullptr;
    SAFE_RELEASE(g_vd.notificationService);
    return false;
  }

  g_vd.notificationCookie = cookie;
  g_vd.notificationsRegistered = true;
  Wh_Log(L"Native virtual desktop notifications registered (cookie=%lu)", cookie);
  return true;
}

// Acquires the COM interfaces needed for virtual-desktop queries. The outer
// lifecycle wrapper owns the initialized flag and notification registration.
bool InitializeVirtualDesktopAPIOnce() {
  ++Diagnostics::g_runtime.counters.vdApiInitAttempts;
  if (!ResolveVirtualDesktopAbiProfile()) {
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
                                (void**)&g_vd.serviceProvider);
  if (FAILED(hr) || !g_vd.serviceProvider) {
    Wh_Log(L"Failed to create ImmersiveShell: 0x%08X", hr);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  hr = g_vd.serviceProvider->QueryService(
      CLSID_VirtualDesktopManagerInternal, g_vd.abi.managerInternal,
      reinterpret_cast<void**>(&g_vd.managerInternal));
  if (FAILED(hr) || !g_vd.managerInternal) {
    Wh_Log(L"Failed to get VirtualDesktopManagerInternal: 0x%08X", hr);
    SAFE_RELEASE(g_vd.serviceProvider);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager,
                        (void**)&g_vd.desktopManager);
  if (FAILED(hr) || !g_vd.desktopManager) {
    Wh_Log(L"Failed to create VirtualDesktopManager: 0x%08X", hr);
    SAFE_RELEASE(g_vd.managerInternal);
    SAFE_RELEASE(g_vd.serviceProvider);
    ++Diagnostics::g_runtime.counters.vdApiInitFailures;
    return false;
  }

  ++Diagnostics::g_runtime.counters.vdApiInitSuccesses;
  return true;
}

// Releases virtual-desktop COM state while Explorer is responsive. During shell
// teardown, pointers are intentionally dropped without Release to avoid hangs.
void CleanupVirtualDesktopAPI() {
  HWND hShell = GetShellWindow();
  const bool shellAvailable = hShell && IsWindow(hShell);

  ClearVirtualDesktopNotificationState(shellAvailable);
  g_vd.abi = {};
  g_vd.abiResolved = false;
  g_vd.explorerBuild = 0;
  g_vd.explorerRevision = 0;
  g_vd.lastKnownDesktop = {};
  g_vd.lastKnownDesktopValid = false;

  if (!shellAvailable) {
    Wh_Log(L"Explorer not available, skipping COM cleanup to avoid hang");
    g_vd.desktopManager = nullptr;
    g_vd.managerInternal = nullptr;
    g_vd.serviceProvider = nullptr;
    g_vd.initialized = false;
    return;
  }

  SAFE_RELEASE(g_vd.desktopManager);
  SAFE_RELEASE(g_vd.managerInternal);
  SAFE_RELEASE(g_vd.serviceProvider);
  g_vd.initialized = false;
}

bool InitializeVirtualDesktopAPI() {
  if (g_vd.initialized) return true;
  if (InitializeVirtualDesktopAPIOnce()) {
    g_vd.initialized = true;
    return true;
  }
  return false;
}

bool ReinitializeVirtualDesktopAPI() {
  ++Diagnostics::g_runtime.counters.vdApiReinitializations;
  Diagnostics::RecordEvent(L"virtual desktop API reinitialization requested");
  CleanupVirtualDesktopAPI();
  if (!InitializeVirtualDesktopAPI()) return false;
  RegisterVirtualDesktopNotifications();
  return true;
}

// Invokes a private manager slot using the Windows-version-specific signature.
// A failed call forces one API reinitialization and retry before being returned.
template <typename TResult>
HRESULT CallManagerInternal(int vtableIndex, TResult* outResult) {
  if (UsesHMonitorParameter()) {
    auto pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, TResult*)>(g_vd.managerInternal,
                                                                                         vtableIndex);
    HRESULT hr = pfn(g_vd.managerInternal, nullptr, outResult);
    if (FAILED(hr) && ReinitializeVirtualDesktopAPI()) {
      pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, TResult*)>(g_vd.managerInternal,
                                                                                      vtableIndex);
      hr = pfn(g_vd.managerInternal, nullptr, outResult);
    }
    return hr;
  } else {
    auto pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, TResult*)>(g_vd.managerInternal, vtableIndex);
    HRESULT hr = pfn(g_vd.managerInternal, outResult);
    if (FAILED(hr) && ReinitializeVirtualDesktopAPI()) {
      pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, TResult*)>(g_vd.managerInternal, vtableIndex);
      hr = pfn(g_vd.managerInternal, outResult);
    }
    return hr;
  }
}

bool GetCurrentDesktopId(GUID* outGuid) {
  if (!outGuid) return false;
  if (!g_vd.managerInternal) {
    g_vd.lastKnownDesktopValid = false;
    return false;
  }

  IVirtualDesktop* desktop = nullptr;
  HRESULT hr = CallManagerInternal(VTABLE_GET_CURRENT_DESKTOP, &desktop);
  if (FAILED(hr) || !desktop) {
    g_vd.lastKnownDesktopValid = false;
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
  g_vd.lastKnownDesktopValid = false;
  return false;
}

bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid) {
  if (!outGuid) return false;
  if (!InitializeVirtualDesktopAPI() || !g_vd.desktopManager) return false;
  return SUCCEEDED(g_vd.desktopManager->GetWindowDesktopId(hwnd, outGuid));
}


}  // namespace Platform::VirtualDesktop

