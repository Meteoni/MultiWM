// ==WindhawkMod==
// @id              tiling-helper-experimental
// @name            Tiling Helper Experimental
// @description     Tile windows on the current monitor with customizable layouts and hotkeys
// @version         1.7.3
// @author          u2x1, meteoni
// @github          https://github.com/u2x1
// @include         windhawk.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -luuid -ldwmapi -lversion -lgdi32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Tiling Helper

A focused window tiling tool for Windows 10/11. Tile windows on the current monitor, cycle layouts, and keep
resize-adjusted ratios per virtual desktop.

## Features
- Manual tiling hotkey
- Layout cycle hotkey
- 7 per-workspace modes: Master+Stack (Vertical/Horizontal), Columns, Rows, BSP, Monocle, Floating
- Adjustable margin, gap, and master size
- Remembers layout mode, ordering, and resize adjustments per virtual desktop + monitor
- Uses native virtual-desktop notifications when available, with coalesced visibility/cloak reconciliation as a safety fallback
- Checked placement with per-window failure tracking and automatic floating for repeatedly non-compliant windows
- User move-only drags float that window without forgetting it
- Theme-aware notification-area text icon shows the current workspace layout (MV/MH/CL/RW/BS/MN/FL)
- Compact Win32 layout flyout appears briefly when the layout is changed
- Optional capture of current window sizes when manually tiling

## Default Hotkeys
- Tile windows: Alt + D
- Cycle layout: Alt + L

## Notes
- Layout mode, ordering, and resize state are stored per virtual desktop + monitor.
- Floating exists both as a workspace mode and as a per-window managed state.
- Native virtual-desktop notifications are selected from the actual Explorer build/revision; unsupported builds fall back to settled WinEvent reconciliation.
- If Virtual Desktop APIs are unavailable, tiling still works but per-desktop memory is disabled.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- WindowsVersion: win11_26100
  $name: Windows Version
  $description: Select your Windows version for correct COM interface IDs. Run "winver" to check your build number.
  $options:
    - win10_old: Windows 10 (Build < 20348)
    - win10_20348: Windows 10 (Build 20348 - 21999)
    - win11_22000: Windows 11 (Build 22000 - 22482)
    - win11_22621: Windows 11 (Build 22621/22631/23H2)
    - win11_26100: Windows 11 (Build 26100+ / 24H2)

- EnableTiling: true
  $name: '[Tiling] Enable'
  $description: Enable hotkey to tile windows on current monitor

- EnableTileNewWin: false
  $name: '[Tiling] Full-auto Tiling'
  $description: Automatically retile when a new window is created (only applies when tiling is enabled)

- TileNewWinToggleKey: "U"
  $name: '[Tiling] Tile New Windows Toggle Key'
  $description: 'Key to pause/resume Auto-tiling at runtime.'

- SwapMasterKey: "M"
  $name: '[Tiling] Switch Master Window Key'
  $description: 'Key to set another tiled window as the master.'

- CaptureLayoutOnTile: true
  $name: '[Tiling] Capture Layout On Tile'
  $description: Use current window sizes as layout weights when manually tiling

- TilingModifier: alt
  $name: '[Tiling] Modifier'
  $description: Modifier keys for tiling hotkeys
  $options:
    - alt: Alt
    - ctrl: Ctrl
    - alt+shift: Alt + Shift
    - ctrl+alt: Ctrl + Alt
    - ctrl+shift: Ctrl + Shift

- TileKey: "D"
  $name: '[Tiling] Key'
  $description: 'Key to tile all windows on the current monitor. Examples: D, T, Space, `, -'

- DefaultLayout: master_stack
  $name: '[Tiling] Default Layout'
  $description: The initial tiling layout when the mod starts (only applies when tiling is enabled)
  $options:
    - master_stack: Master + Stack (Vertical)
    - master_stack_h: Master + Stack (Horizontal)
    - columns: Columns
    - rows: Rows
    - bsp: BSP (Binary Space Partition)
    - monocle: Monocle (Fullscreen)
    - floating: Floating (managed, no automatic placement)

- TileMargin: 4
  $name: '[Tiling] Margin (pixels)'
  $description: Gap between tiled windows and screen edges (0-100, only applies when tiling is enabled)

- TileGap: 4
  $name: '[Tiling] Gap (pixels)'
  $description: Gap between adjacent tiled windows (0-100, only applies when tiling is enabled)

- MasterPercent: 50
  $name: '[Tiling] Master Size (%)'
  $description: Size percentage of the master window in Master+Stack layouts (1-99, only applies when tiling is enabled)

- EnableLayoutCycle: true
  $name: '[Tiling] Enable Layout Cycle'
  $description: Enable hotkey to cycle through tiling layouts (only works when tiling hotkey is enabled)

- LayoutKey: "L"
  $name: '[Tiling] Layout Cycle Key'
  $description: 'Key to cycle through tiling layouts. Examples: L, Tab, =, ], /'
*/
// ==/WindhawkModSettings==

#include <dwmapi.h>
#include <initguid.h>
#include <objbase.h>
#include <objectarray.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windhawk_utils.h>
#include <windows.h>
#include <winver.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <new>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#define SAFE_RELEASE(p) \
  do {                  \
    if (p) {            \
      (p)->Release();   \
      (p) = nullptr;    \
    }                   \
  } while (0)

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
// Windows Version-Specific Interface IDs
//=============================================================================
struct VersionIIDs {
  IID managerInternal;  // IID for IVirtualDesktopManagerInternal
  IID virtualDesktop;   // IID for IVirtualDesktop
  bool usesHMonitor;    // Whether API methods require HMONITOR parameter
};

static const VersionIIDs g_versionIIDs[] = {
    // [0] Windows 10 (Build < 20348)
    {{0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}},
     {0xFF72FFDD, 0xBE7E, 0x43FC, {0x9C, 0x03, 0xAD, 0x81, 0x68, 0x1E, 0x88, 0xE4}},
     false},

    // [1] Windows 10 (Build 20348 - 21999)
    {{0x094AFE11, 0x44F2, 0x4BA0, {0x97, 0x6F, 0x29, 0xA9, 0x7E, 0x26, 0x3E, 0xE0}},
     {0x62FDF88B, 0x11CA, 0x4AFB, {0x8B, 0xD8, 0x22, 0x96, 0xDF, 0xAE, 0x49, 0xE2}},
     true},

    // [2] Windows 11 (Build 22000 - 22482)
    {{0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}},
     {0x536D3495, 0xB208, 0x4CC9, {0xAE, 0x26, 0xDE, 0x81, 0x11, 0x27, 0x5B, 0xF8}},
     true},

    // [3] Windows 11 (Build 22621/22631/23H2)
    {{0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}},
     {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}},
     false},

    // [4] Windows 11 (Build 26100+ / 24H2)
    {{0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}},
     {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}},
     false},
};

static int g_windowsVersionIndex = 4;

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

struct NotificationInterfaceConfig {
  IID iid{};
  int methodCount = 0;
  int currentChangedIndex = -1;
  bool currentChangedHasMonitors = false;
};

struct VirtualDesktopNotificationObject {
  void** vtable = nullptr;
  LONG refCount = 1;
};

struct PendingWorkspaceArrange {
  GUID desktopId{};
  HMONITOR monitor = nullptr;
};

//=============================================================================
// Global State
//=============================================================================
static IServiceProvider* g_pServiceProvider = nullptr;
static IVirtualDesktopManagerInternal* g_pDesktopManagerInternal = nullptr;
static IVirtualDesktopManager* g_pDesktopManager = nullptr;
static IVirtualDesktopNotificationService* g_pDesktopNotificationService = nullptr;
static VirtualDesktopNotificationObject* g_virtualDesktopNotificationObject = nullptr;
static DWORD g_virtualDesktopNotificationCookie = 0;
static bool g_virtualDesktopNotificationsRegistered = false;
static DWORD g_explorerBuildNumber = 0;
static DWORD g_explorerRevisionNumber = 0;
static LONG g_virtualDesktopChangeQueued = 0;
static bool g_bInitialized = false;

static HANDLE g_hThread = nullptr;
static DWORD g_threadId = 0;
static HANDLE g_hReadyEvent = nullptr;
static volatile bool g_stopHotkeyThread = false;

// Hotkey IDs
enum HotkeyIds {
  HK_TILE = 1,
  HK_LAYOUT = 2,
  HK_SWAP_MASTER = 3,
  HK_TILENEWWIN_TOGGLE = 4,
};

// Messages to hotkey thread
constexpr UINT WM_APP_MOVE_SIZE_END = WM_APP + 1;
constexpr UINT WM_APP_WINDOW_EVENT = WM_APP + 2;
constexpr UINT WM_APP_VIRTUAL_DESKTOP_CHANGED = WM_APP + 3;
constexpr UINT WM_APP_TRAY_REFRESH = WM_APP + 4;

// Visibility/cloak state often changes in bursts, especially during virtual
// desktop switches. Reconcile final state only after the burst has settled.
constexpr UINT kLifecycleReconcileDelayMs = 80;
static UINT_PTR g_lifecycleReconcileTimer = 0;
static std::vector<HWND> g_lifecycleDirtyWindows;
// Native VD transitions may create/change a workspace while its windows are
// still shell-cloaked. Defer placement for only those monitors until the final
// settled visibility pass.
static std::vector<PendingWorkspaceArrange> g_pendingDesktopArranges;

static UINT g_tilingModifiers = MOD_ALT;
static UINT g_tileKey = 'D';
static UINT g_layoutKey = 'L';
static UINT g_swapMasterKey = 'M';
static UINT g_tileNewWinToggleKey = 'U';

static LONG g_tileMargin = 4;
static LONG g_tileGap = 4;
static LONG g_masterPercent = 50;

enum class TileLayout { MasterStack, Columns, Rows, MasterStackH, BSP, Monocle, Floating, COUNT };
// Used only when a desktop+monitor workspace is first created. Each workspace
// persists its own current layout in TilingState::layout.
static TileLayout g_defaultLayout = TileLayout::MasterStack;

static bool g_enableTiling = true;
static bool g_captureLayoutOnTile = true;
static bool g_enableLayoutCycle = true;
static bool g_enableTileNewWin = false;
static bool g_tileNewWinSuspended = false;

// Per-desktop + monitor state
struct GuidHash {
  size_t operator()(const GUID& guid) const {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(&guid);
    size_t hash = 0;
    for (int i = 0; i < 4; ++i) {
      hash ^= std::hash<uint32_t>{}(data[i]) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

struct GuidEqual {
  bool operator()(const GUID& a, const GUID& b) const { return IsEqualGUID(a, b); }
};

struct DesktopMonitorKey {
  GUID desktopId;
  HMONITOR monitor;
};

struct DesktopMonitorKeyHash {
  size_t operator()(const DesktopMonitorKey& key) const {
    size_t hash = GuidHash{}(key.desktopId);
    hash ^= std::hash<void*>{}(key.monitor) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct DesktopMonitorKeyEqual {
  bool operator()(const DesktopMonitorKey& a, const DesktopMonitorKey& b) const {
    return IsEqualGUID(a.desktopId, b.desktopId) && a.monitor == b.monitor;
  }
};

struct SuspendedSlot {
  size_t index = 0;
  double weight = 1.0;
  bool hasWeight = false;
  bool wasMaster = false;
};

enum class ManageState {
  Tiled,
  Floating,
  Suspended,
  Ignored,
};

enum class PlacementResult {
  Success,
  AdjustedByWindow,
  AccessDenied,
  Refused,
  Dead,
};

struct WindowRecord {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  ManageState state = ManageState::Tiled;

  // Capabilities are observations, not hard policy yet. They give later rule/
  // constraint work somewhere stable to live without re-querying every branch.
  bool canMove = true;
  bool canResize = true;
  bool topmost = false;

  int placementFailures = 0;
  PlacementResult lastPlacementResult = PlacementResult::Success;
  RECT lastRequestedRect{};
  RECT lastObservedRect{};

  // Only meaningful while state == Suspended. It preserves the tiled logical
  // position/weight across minimize, hide, and app-cloak transitions.
  SuspendedSlot savedSlot{};
  bool hasSavedSlot = false;
};

struct TilingState {
  TileLayout layout = TileLayout::MasterStack;

  // Active tiled participants only, in logical layout order. Membership/state
  // for every known window lives in records; this vector is just the ordering
  // consumed by the layout algorithms.
  std::vector<HWND> windows;
  std::unordered_map<HWND, WindowRecord> records;

  double masterRatio = 0.5;
  std::vector<double> stackWeights;
  std::vector<double> gridWeights;
};

constexpr int kPlacementFailureLimit = 3;
constexpr LONG kPlacementTolerancePx = 4;

static std::unordered_map<DesktopMonitorKey, TilingState, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>
    g_tilingStateMap;
static SRWLOCK g_tilingStateLock = SRWLOCK_INIT;
// Rect cache thread safety lock
static SRWLOCK g_moveSizeRectsLock = SRWLOCK_INIT;
static HWINEVENTHOOK g_hMoveSizeHook = nullptr;
static HWINEVENTHOOK g_hMinimizeHook = nullptr;
static HWINEVENTHOOK g_hHideDestroyHook = nullptr;
static HWINEVENTHOOK g_hCloakHook = nullptr;
static HWINEVENTHOOK g_hForegroundHook = nullptr;

//=============================================================================
//Cache Rects
//=============================================================================

static std::unordered_map<HWND, RECT> g_moveSizeStartRects;
static std::unordered_map<HWND, RECT> g_moveSizeEndRects;


//=============================================================================
//Move detection
//=============================================================================



enum class RectChange {
    None,
    MoveOnly,
    ResizeOnly,
    MoveAndResize
};

static inline LONG RectW(const RECT& r) { return r.right - r.left; }
static inline LONG RectH(const RECT& r) { return r.bottom - r.top; }

static inline bool Differs(LONG a, LONG b, LONG tol = 1) {
    return (a > b) ? (a - b > tol) : (b - a > tol);
}

RectChange ClassifyRectChange(const RECT& before, const RECT& after, LONG tol = 1) {
    bool xChanged = Differs(before.left,  after.left,  tol);
    bool yChanged = Differs(before.top,   after.top,   tol);
    bool wChanged = Differs(RectW(before), RectW(after), tol);
    bool hChanged = Differs(RectH(before), RectH(after), tol);

    bool posChanged  = xChanged || yChanged;
    bool sizeChanged = wChanged || hChanged;

    if (!posChanged && !sizeChanged) {Wh_Log(L"None change"); return RectChange::None;}
    if ( posChanged && !sizeChanged) {Wh_Log(L"Move-only change"); return RectChange::MoveOnly;}
    if (!posChanged &&  sizeChanged) {Wh_Log(L"Resize-only change"); return RectChange::ResizeOnly;}
    Wh_Log(L"Both changed"); return RectChange::MoveAndResize;
}






//=============================================================================
// Helper Functions
//=============================================================================

template <typename T>
T GetVTableFunction(void* pInterface, int index) {
  return reinterpret_cast<T>((*reinterpret_cast<void***>(pInterface))[index]);
}

bool UsesHMonitorParameter() { return g_versionIIDs[g_windowsVersionIndex].usesHMonitor; }

UINT ParseModifiers(PCWSTR str) {
  UINT modifiers = 0;
  if (wcsstr(str, L"alt")) modifiers |= MOD_ALT;
  if (wcsstr(str, L"ctrl")) modifiers |= MOD_CONTROL;
  if (wcsstr(str, L"shift")) modifiers |= MOD_SHIFT;
  return modifiers;
}

template <typename T>
T LookupTable(PCWSTR str, const std::pair<PCWSTR, T>* table, size_t count, T defaultVal) {
  for (size_t i = 0; i < count; ++i) {
    if (wcscmp(str, table[i].first) == 0) return table[i].second;
  }
  return defaultVal;
}

TileLayout ParseLayoutSetting(PCWSTR str) {
  static const std::pair<PCWSTR, TileLayout> kLayoutMap[] = {{L"master_stack", TileLayout::MasterStack},
                                                             {L"master_stack_h", TileLayout::MasterStackH},
                                                             {L"columns", TileLayout::Columns},
                                                             {L"rows", TileLayout::Rows},
                                                             {L"bsp", TileLayout::BSP},
                                                             {L"monocle", TileLayout::Monocle},
                                                             {L"floating", TileLayout::Floating}};
  return LookupTable(str, kLayoutMap, _countof(kLayoutMap), TileLayout::MasterStack);
}

int ParseWindowsVersion(PCWSTR str) {
  static const std::pair<PCWSTR, int> kVersionMap[] = {
      {L"win10_old", 0}, {L"win10_20348", 1}, {L"win11_22000", 2}, {L"win11_22621", 3}, {L"win11_26100", 4}};
  return LookupTable(str, kVersionMap, _countof(kVersionMap), 4);
}

template <typename T, typename Parser>
T ReadStringSetting(PCWSTR name, Parser parser, T defaultVal) {
  PCWSTR str = Wh_GetStringSetting(name);
  T result = parser(str);
  Wh_FreeStringSetting(str);
  return result ? result : defaultVal;
}

UINT ReadModifierSetting(PCWSTR name, UINT defaultVal) {
  PCWSTR str = Wh_GetStringSetting(name);
  UINT result = ParseModifiers(str);
  Wh_FreeStringSetting(str);
  return result ? result : defaultVal;
}

inline double ClampDouble(double value, double minVal, double maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}

bool GetWindowFrameRect(HWND hwnd, RECT* outRect) {
  if (!hwnd || !outRect) return false;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, outRect, sizeof(*outRect)))) {
    return true;
  }
  return GetWindowRect(hwnd, outRect) != FALSE;
}

bool GetMonitorWorkArea(HMONITOR monitor, RECT* outRect) {
  if (!monitor || !outRect) return false;
  MONITORINFO monitorInfo = {sizeof(monitorInfo)};
  if (!GetMonitorInfoW(monitor, &monitorInfo)) return false;
  *outRect = monitorInfo.rcWork;
  return true;
}

bool AreSameWindows(const std::vector<HWND>& a, const std::vector<HWND>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

std::vector<double> DefaultWeights(size_t count) {
  return std::vector<double>(count, 1.0);
}

bool IsTileEligible(HWND hwnd, HMONITOR targetMonitor);
static bool CouldBeTileEligible(HWND hwnd);
static bool IsWindowTrackedInAnyState(HWND hwnd);
bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid);

bool ContainsWindow(const std::vector<HWND>& windows, HWND hwnd) {
  for (HWND w : windows) {
    if (w == hwnd) return true;
  }
  return false;
}

static WindowRecord MakeWindowRecord(HWND hwnd, ManageState state = ManageState::Tiled) {
  WindowRecord record;
  record.hwnd = hwnd;
  record.state = state;

  if (hwnd) {
    GetWindowThreadProcessId(hwnd, &record.pid);
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    record.canMove = (GetAncestor(hwnd, GA_ROOT) == hwnd) && !(style & WS_CHILD);
    record.canResize = (style & WS_SIZEBOX) != 0;
    record.topmost = (exStyle & WS_EX_TOPMOST) != 0;
  }

  return record;
}

static WindowRecord& EnsureWindowRecord(
    TilingState& state, HWND hwnd, ManageState initialState = ManageState::Tiled) {
  auto [it, inserted] = state.records.emplace(hwnd, MakeWindowRecord(hwnd, initialState));
  if (!inserted) {
    it->second.hwnd = hwnd;
  }
  return it->second;
}

static WindowRecord* FindWindowRecord(TilingState& state, HWND hwnd) {
  auto it = state.records.find(hwnd);
  return it == state.records.end() ? nullptr : &it->second;
}

HWND ResolveEquivalentWindow(HWND hwnd, const std::vector<HWND>& candidates) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  wchar_t className[128] = {};
  GetClassNameW(hwnd, className, ARRAYSIZE(className));

  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, ARRAYSIZE(title));

  HWND bestMatch = nullptr;
  for (HWND candidate : candidates) {
    if (!candidate || candidate == hwnd || !IsWindow(candidate)) continue;

    DWORD candPid = 0;
    GetWindowThreadProcessId(candidate, &candPid);
    if (candPid != pid) continue;

    wchar_t candClass[128] = {};
    GetClassNameW(candidate, candClass, ARRAYSIZE(candClass));
    if (_wcsicmp(candClass, className) != 0) continue;

    wchar_t candTitle[256] = {};
    GetWindowTextW(candidate, candTitle, ARRAYSIZE(candTitle));

    if (_wcsicmp(candTitle, title) == 0 || (candTitle[0] == 0 && title[0] == 0)) {
      return candidate;
    }

    if (!bestMatch) bestMatch = candidate;
  }

  return bestMatch;
}

HWND ResolveToTiledWindow(HWND hwnd, const std::vector<HWND>& candidates) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;

  if (ContainsWindow(candidates, hwnd)) {
    return hwnd;
  }

  HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
  if (rootOwner && ContainsWindow(candidates, rootOwner)) {
    return rootOwner;
  }

  HWND owner = GetWindow(hwnd, GW_OWNER);
  while (owner) {
    if (ContainsWindow(candidates, owner)) {
      return owner;
    }
    owner = GetWindow(owner, GW_OWNER);
  }

  return ResolveEquivalentWindow(hwnd, candidates);
}

std::vector<HWND> CollectTileWindows(HMONITOR monitor) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    std::vector<HWND>* windowList;
  } context = {monitor, &windows};

  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        if (IsTileEligible(hwnd, ctx->targetMonitor)) {
          ctx->windowList->push_back(hwnd);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

TilingState BuildStateFromWindows(TileLayout layout, const RECT& workArea, const std::vector<HWND>& windows,
                                  HMONITOR monitor, HWND preferredFirstWin = nullptr) {
  TilingState state;
  state.layout = layout;
  state.windows = windows;
  if (windows.empty()) return state;
  /*
  // prefer current master window
  if (preferredFirstWin){
    auto itWin = std::find(state.windows.begin(), state.windows.end(), preferredFirstWin);
    if (itWin != state.windows.end()) {
      std::iter_swap(itWin, state.windows.begin());
    }
  }
  */

  if (layout == TileLayout::MasterStack || layout == TileLayout::MasterStackH) {
    bool horizontal = (layout == TileLayout::MasterStackH);
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(state.windows.size());
    for (HWND w : state.windows) {
      RECT rect = {};
      if (!GetWindowFrameRect(w, &rect)) {
        rect = workArea;
      }
      if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != monitor) {
        rect = workArea;
      }
      infos.push_back({w, rect});
    }


    // choose master by "closest to 3 workarea edges" score
    
    struct Candidate {
      long long score;  // Closeness to three edges
      long long axisPos;     // Closeness to left & top (tie-break)
      size_t index;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(infos.size());

    for (size_t i = 0; i < infos.size(); ++i) {
      const RECT& r = infos[i].rect;

      long long d[4] = {
        (long long)std::llabs((long long)r.left   - (long long)workArea.left),
        (long long)std::llabs((long long)r.top    - (long long)workArea.top),
        (long long)std::llabs((long long)r.right  - (long long)workArea.right),
        (long long)std::llabs((long long)r.bottom - (long long)workArea.bottom),
      };

      std::sort(d, d + 4);
      long long score = d[0] + d[1] + d[2]; // "distance from being master"
      
      Candidate c;
      c.score = score;
      c.axisPos = horizontal 
        ? (long long)std::llabs((long long)r.top  - (long long)workArea.top)
        : (long long)std::llabs((long long)r.left - (long long)workArea.left);

      c.index = i;
      candidates.push_back(c);
      Wh_Log(L"Index %zu rect: L=%ld T=%ld R=%ld B=%ld",
       i, r.left, r.top, r.right, r.bottom);
      Wh_Log(L"Index: %zu, Score: %lld, axisPos: %lld", c.index, c.score, c.axisPos);
    }

    
    std::stable_sort(candidates.begin(), candidates.end(),
      [preferredFirstWin, &infos](const auto& a, const auto& b) {
        // first comparison key: existing master always goes first
        bool aPreferred = preferredFirstWin && infos[a.index].hwnd == preferredFirstWin;
        bool bPreferred = preferredFirstWin && infos[b.index].hwnd == preferredFirstWin;

        if (aPreferred != bPreferred)
          return aPreferred;

        // later comparisons use closeness score
        if (a.score != b.score)
          return a.score < b.score;
        return a.axisPos < b.axisPos;
      });

    size_t masterIndex = candidates.empty() ? 0 : candidates.front().index;

    Wh_Log(L"Master identified: Score: %lld, axisPos: %lld", candidates.front().score, candidates.front().axisPos);


    // old logic uses maximum sizes
    /*
    size_t masterIndex = 0;
    LONG bestSize = -1;
    for (size_t i = 0; i < infos.size(); ++i) {
      LONG size = horizontal ? (infos[i].rect.bottom - infos[i].rect.top) : (infos[i].rect.right - infos[i].rect.left);
      if (size > bestSize) {
        bestSize = size;
        masterIndex = i;
      }
    }
    */

    WindowInfo master = infos[masterIndex];
    std::vector<WindowInfo> stack;
    stack.reserve(infos.size() - 1);
    for (size_t i = 0; i < infos.size(); ++i) {
      if (i == masterIndex) continue;
      stack.push_back(infos[i]);
    }

    std::stable_sort(stack.begin(), stack.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
      return horizontal ? (a.rect.left < b.rect.left) : (a.rect.top < b.rect.top);
    });

    state.windows.clear();
    state.windows.push_back(master.hwnd);
    for (const auto& item : stack) state.windows.push_back(item.hwnd);

    LONG totalSize = horizontal ? (workArea.bottom - workArea.top) : (workArea.right - workArea.left);
    if (totalSize > g_tileGap + 1) {
      LONG masterSize = horizontal ? (master.rect.bottom - master.rect.top) : (master.rect.right - master.rect.left);
      if (masterSize < 1) masterSize = 1;
      state.masterRatio = ClampDouble((double)masterSize / (double)(totalSize - g_tileGap), 0.1, 0.9);
    } else {
      state.masterRatio = ClampDouble(g_masterPercent / 100.0, 0.1, 0.9);
    }

    state.stackWeights.clear();
    for (const auto& item : stack) {
      LONG size = horizontal ? (item.rect.right - item.rect.left) : (item.rect.bottom - item.rect.top);
      if (size < 1) size = 1;
      state.stackWeights.push_back(static_cast<double>(size));
    }
  } else if (layout == TileLayout::Columns || layout == TileLayout::Rows) {
    bool horizontal = (layout == TileLayout::Rows);
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(state.windows.size());
    for (HWND w : state.windows) {
      RECT rect = {};
      if (!GetWindowFrameRect(w, &rect)) {
        rect = workArea;
      }
      if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != monitor) {
        rect = workArea;
      }
      infos.push_back({w, rect});
    }

    std::stable_sort(infos.begin(), infos.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
      return horizontal ? (a.rect.top < b.rect.top) : (a.rect.left < b.rect.left);
    });

    state.windows.clear();
    state.gridWeights.clear();
    for (const auto& item : infos) {
      state.windows.push_back(item.hwnd);
      LONG size = horizontal ? (item.rect.bottom - item.rect.top) : (item.rect.right - item.rect.left);
      if (size < 1) size = 1;
      state.gridWeights.push_back(static_cast<double>(size));
    }
  }

  state.records.clear();
  for (HWND hwnd : state.windows) {
    state.records.emplace(hwnd, MakeWindowRecord(hwnd, ManageState::Tiled));
  }

  return state;
}

std::vector<LONG> ComputeWeightedSizes(LONG totalSize, LONG gap, const std::vector<double>& weights) {
  size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (count == 0) return sizes;
  LONG available = totalSize - gap * (LONG)(count - 1);
  if (available <= 0) return sizes;

  double sum = 0.0;
  for (double w : weights) sum += (w > 0.0 ? w : 0.0);
  if (sum <= 0.0) sum = static_cast<double>(count);

  LONG used = 0;
  double remainingSum = sum;
  for (size_t i = 0; i < count; ++i) {
    double w = weights[i];
    if (w <= 0.0) w = 1.0;
    LONG remainingSlots = (LONG)(count - i - 1);
    LONG size = 0;
    if (i == count - 1) {
      size = available - used;

    } else {
      double ratio = w / remainingSum;
      size = static_cast<LONG>(std::llround(static_cast<double>(available - used) * ratio));
      if (size < 1) size = 1;
      LONG maxSize = available - used - remainingSlots;
      if (size > maxSize) size = maxSize;
    }
    sizes[i] = size;
    used += size;
    remainingSum -= w;
    if (remainingSum <= 0.0) remainingSum = 1.0;
  }
  return sizes;
}

std::vector<LONG> ComputeWeightedSizesWithFixed(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    size_t fixedIndex, LONG fixedSize) {

  const size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (count == 0) return sizes;
  if (fixedIndex >= count) return ComputeWeightedSizes(totalSize, gap, weights);

  const LONG available = totalSize - gap * (LONG)(count - 1);
  if (available <= 0) return sizes;

  // Clamp fixed size so remaining windows can still be >=1
  const LONG minFixed = 1;
  const LONG maxFixed = std::max<LONG>(1, available - (LONG)(count - 1));
  fixedSize = std::clamp(fixedSize, minFixed, maxFixed);

  sizes[fixedIndex] = fixedSize;

  LONG remaining = available - fixedSize;
  if (count == 1) return sizes;

  // Sum weights for non-fixed slots
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    if (i == fixedIndex) continue;
    sum += (weights[i] > 0.0 ? weights[i] : 1.0);
  }
  if (sum <= 0.0) sum = (double)(count - 1);

  LONG used = 0;
  double remainingSum = sum;

  // Distribute remaining across non-fixed indices (no gap math here)
  for (size_t i = 0; i < count; ++i) {
    if (i == fixedIndex) continue;

    // how many non-fixed slots remain after i?
    LONG slotsLeft = 0;
    for (size_t j = i + 1; j < count; ++j) if (j != fixedIndex) ++slotsLeft;

    double w = (weights[i] > 0.0 ? weights[i] : 1.0);
    LONG size = 0;

    if (slotsLeft == 0) {
      size = remaining - used;
    } else {
      const double ratio = w / remainingSum;
      size = (LONG)std::llround((double)(remaining - used) * ratio);
      if (size < 1) size = 1;

      const LONG maxSize = (remaining - used) - slotsLeft; // leave >=1 for each remaining slot
      if (size > maxSize) size = maxSize;
    }

    sizes[i] = size;
    used += size;
    remainingSum -= w;
    if (remainingSum <= 0.0) remainingSum = 1.0;
  }

  return sizes;
}

void LayoutGridWeighted(const RECT& area, size_t windowCount, std::vector<RECT>& outRects, bool horizontal,
                        const std::vector<double>& weights) {
  outRects.resize(windowCount);
  if (windowCount == 0) return;

  LONG totalSize = horizontal ? (area.bottom - area.top) : (area.right - area.left);
  std::vector<LONG> sizes = ComputeWeightedSizes(totalSize, g_tileGap, weights);
  LONG position = horizontal ? area.top : area.left;

  for (size_t i = 0; i < windowCount; ++i) {
    LONG size = sizes[i];
    LONG end = (i == windowCount - 1) ? (horizontal ? area.bottom : area.right) : position + size;
    outRects[i] = horizontal ? RECT{area.left, position, area.right, end} : RECT{position, area.top, end, area.bottom};
    position = end + g_tileGap;
  }
}

void LayoutMasterStackWeighted(const RECT& area, size_t windowCount, std::vector<RECT>& outRects, bool horizontal,
                               double masterRatio, const std::vector<double>& stackWeights) {
  outRects.resize(windowCount);
  if (windowCount == 0) return;
  if (windowCount == 1) {
    outRects[0] = area;
    return;
  }

  LONG totalSize = horizontal ? (area.bottom - area.top) : (area.right - area.left);
  LONG masterSize = static_cast<LONG>(std::llround((totalSize - g_tileGap) * masterRatio));
  if (masterSize < 1) masterSize = 1;
  if (masterSize > totalSize - g_tileGap - 1) masterSize = totalSize - g_tileGap - 1;

  size_t stackCount = windowCount - 1;
  std::vector<double> weights = stackWeights;
  if (weights.size() != stackCount) weights = DefaultWeights(stackCount);

  if (horizontal) {
    outRects[0] = {area.left, area.top, area.right, area.top + masterSize};
    LONG stackTop = area.top + masterSize + g_tileGap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.right - area.left, g_tileGap, weights);
    LONG x = area.left;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.right : x + sizes[i];
      outRects[i + 1] = {x, stackTop, end, area.bottom};
      x = end + g_tileGap;
    }
  } else {
    outRects[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    LONG stackLeft = area.left + masterSize + g_tileGap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.bottom - area.top, g_tileGap, weights);
    LONG y = area.top;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.bottom : y + sizes[i];
      outRects[i + 1] = {stackLeft, y, area.right, end};
      y = end + g_tileGap;
    }
  }
}

void LayoutBSP(const RECT& area, size_t startIndex, size_t count, int depth, std::vector<RECT>& outRects) {
  if (count == 0) return;
  if (count == 1) {
    outRects[startIndex] = area;
    return;
  }

  bool splitVertical = (depth % 2 == 0);
  LONG mid = splitVertical ? area.left + (area.right - area.left - g_tileGap) / 2
                           : area.top + (area.bottom - area.top - g_tileGap) / 2;

  outRects[startIndex] =
      splitVertical ? RECT{area.left, area.top, mid, area.bottom} : RECT{area.left, area.top, area.right, mid};

  RECT remaining = splitVertical ? RECT{mid + g_tileGap, area.top, area.right, area.bottom}
                                 : RECT{area.left, mid + g_tileGap, area.right, area.bottom};

  LayoutBSP(remaining, startIndex + 1, count - 1, depth + 1, outRects);
}

//=============================================================================
// Virtual Desktop API + native change notifications
//=============================================================================

static const int VTABLE_GET_CURRENT_DESKTOP = 6;

static NotificationInterfaceConfig GetNotificationInterfaceConfig() {
  if (g_explorerBuildNumber < 22000) return {};

  // Mirrors Windhawk's Taskbar Desktop Indicator ABI table. The private
  // notification interface changed both IID and callback layout within the
  // Windows 11 22H2/23H2 servicing lifetime, so build number alone is not enough.
  if (g_explorerBuildNumber < 22483 ||
      (g_explorerBuildNumber == 22621 && g_explorerRevisionNumber < 2215)) {
    return {IID_IVirtualDesktopNotification_Old, 13, 11, true};
  }

  if (g_explorerBuildNumber < 22631 ||
      (g_explorerBuildNumber == 22631 && g_explorerRevisionNumber < 3085)) {
    return {IID_IVirtualDesktopNotification_Intermediate, 14, 10, false};
  }

  return {IID_IVirtualDesktopNotification_Current, 14, 10, false};
}

static bool ReadExplorerVersion() {
  wchar_t explorerPath[MAX_PATH]{};
  UINT length = GetWindowsDirectoryW(explorerPath, ARRAYSIZE(explorerPath));
  static constexpr wchar_t suffix[] = L"\\explorer.exe";
  if (!length || length >= ARRAYSIZE(explorerPath) ||
      length + ARRAYSIZE(suffix) > ARRAYSIZE(explorerPath)) {
    Wh_Log(L"Failed to resolve explorer.exe path for VD notification ABI detection");
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

  g_explorerBuildNumber = HIWORD(fixedInfo->dwFileVersionLS);
  g_explorerRevisionNumber = LOWORD(fixedInfo->dwFileVersionLS);
  Wh_Log(L"Explorer version for VD notifications: build=%lu revision=%lu",
         g_explorerBuildNumber, g_explorerRevisionNumber);
  return true;
}

static bool IsCurrentNotificationInterface(REFIID riid) {
  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  return config.methodCount > 0 && InlineIsEqualGUID(riid, config.iid);
}

static void QueueVirtualDesktopChangedNotification() {
  if (!g_threadId || g_stopHotkeyThread) return;

  // COM can emit redundant desktop notifications. Collapse them before they
  // reach the WM core, just as FancyWM coalesces layout invalidations.
  if (InterlockedExchange(&g_virtualDesktopChangeQueued, 1) != 0) return;

  if (!PostThreadMessage(g_threadId, WM_APP_VIRTUAL_DESKTOP_CHANGED, 0, 0)) {
    InterlockedExchange(&g_virtualDesktopChangeQueued, 0);
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

static void ClearVirtualDesktopNotificationState(bool unregisterFromShell) {
  g_virtualDesktopNotificationsRegistered = false;
  InterlockedExchange(&g_virtualDesktopChangeQueued, 0);

  if (g_pDesktopNotificationService) {
    if (unregisterFromShell && g_virtualDesktopNotificationCookie) {
      HRESULT hr = g_pDesktopNotificationService->Unregister(g_virtualDesktopNotificationCookie);
      if (FAILED(hr)) {
        Wh_Log(L"Virtual desktop notification Unregister failed: 0x%08X", hr);
      }
    }

    if (unregisterFromShell) {
      g_pDesktopNotificationService->Release();
    }
    // If Explorer is unavailable, avoid a potentially blocking COM call/release.
    // The old server is going away; importantly, discard its cookie *before* a
    // new registration so a reused cookie can never unregister the new sink.
    g_pDesktopNotificationService = nullptr;
  }

  if (g_virtualDesktopNotificationObject) {
    VirtualDesktopNotification_Release(g_virtualDesktopNotificationObject);
    g_virtualDesktopNotificationObject = nullptr;
  }

  g_virtualDesktopNotificationCookie = 0;
}

static bool RegisterVirtualDesktopNotifications() {
  if (g_virtualDesktopNotificationsRegistered) return true;
  if (!g_pServiceProvider) return false;
  if (!ReadExplorerVersion()) return false;

  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  if (config.methodCount == 0) {
    Wh_Log(L"Native VD notifications unavailable for Explorer build %lu.%lu; using WinEvent settle fallback",
           g_explorerBuildNumber, g_explorerRevisionNumber);
    return false;
  }

  // Never carry an old cookie/sink into a new shell registration. Explorer can
  // reuse notification cookies after a restart. Normal reinitialization already
  // clears this state; this is a defensive cleanup for partial/stale attempts.
  if (g_pDesktopNotificationService || g_virtualDesktopNotificationObject ||
      g_virtualDesktopNotificationCookie) {
    HWND shell = GetShellWindow();
    ClearVirtualDesktopNotificationState(shell && IsWindow(shell));
  }

  HRESULT hr = g_pServiceProvider->QueryService(
      SID_VirtualDesktopNotificationService, IID_IVirtualDesktopNotificationService,
      reinterpret_cast<void**>(&g_pDesktopNotificationService));
  if (FAILED(hr) || !g_pDesktopNotificationService) {
    Wh_Log(L"Failed to get VirtualDesktopNotificationService: 0x%08X", hr);
    g_pDesktopNotificationService = nullptr;
    return false;
  }

  g_virtualDesktopNotificationObject = CreateVirtualDesktopNotificationObject();
  if (!g_virtualDesktopNotificationObject) {
    Wh_Log(L"Failed to create virtual desktop notification sink");
    SAFE_RELEASE(g_pDesktopNotificationService);
    return false;
  }

  DWORD cookie = 0;
  hr = g_pDesktopNotificationService->Register(
      reinterpret_cast<IUnknown*>(g_virtualDesktopNotificationObject), &cookie);
  if (FAILED(hr) || cookie == 0) {
    Wh_Log(L"Virtual desktop notification Register failed: 0x%08X cookie=%lu", hr, cookie);
    VirtualDesktopNotification_Release(g_virtualDesktopNotificationObject);
    g_virtualDesktopNotificationObject = nullptr;
    SAFE_RELEASE(g_pDesktopNotificationService);
    return false;
  }

  g_virtualDesktopNotificationCookie = cookie;
  g_virtualDesktopNotificationsRegistered = true;
  Wh_Log(L"Native virtual desktop notifications registered (cookie=%lu)", cookie);
  return true;
}

bool InitializeVirtualDesktopAPIOnce() {
  HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
                                (void**)&g_pServiceProvider);
  if (FAILED(hr) || !g_pServiceProvider) {
    Wh_Log(L"Failed to create ImmersiveShell: 0x%08X", hr);
    return false;
  }

  hr = g_pServiceProvider->QueryService(CLSID_VirtualDesktopManagerInternal,
                                        g_versionIIDs[g_windowsVersionIndex].managerInternal,
                                        (void**)&g_pDesktopManagerInternal);
  if (FAILED(hr) || !g_pDesktopManagerInternal) {
    Wh_Log(L"Failed to get VirtualDesktopManagerInternal: 0x%08X", hr);
    SAFE_RELEASE(g_pServiceProvider);
    return false;
  }

  hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager,
                        (void**)&g_pDesktopManager);
  if (FAILED(hr) || !g_pDesktopManager) {
    Wh_Log(L"Failed to create VirtualDesktopManager: 0x%08X", hr);
    SAFE_RELEASE(g_pDesktopManagerInternal);
    SAFE_RELEASE(g_pServiceProvider);
    return false;
  }

  return true;
}

void CleanupVirtualDesktopAPI() {
  HWND hShell = GetShellWindow();
  const bool shellAvailable = hShell && IsWindow(hShell);

  ClearVirtualDesktopNotificationState(shellAvailable);

  if (!shellAvailable) {
    Wh_Log(L"Explorer not available, skipping COM cleanup to avoid hang");
    g_pDesktopManager = nullptr;
    g_pDesktopManagerInternal = nullptr;
    g_pServiceProvider = nullptr;
    g_bInitialized = false;
    return;
  }

  SAFE_RELEASE(g_pDesktopManager);
  SAFE_RELEASE(g_pDesktopManagerInternal);
  SAFE_RELEASE(g_pServiceProvider);
  g_bInitialized = false;
}

bool InitializeVirtualDesktopAPI() {
  if (g_bInitialized) return true;
  if (InitializeVirtualDesktopAPIOnce()) {
    g_bInitialized = true;
    return true;
  }
  return false;
}

bool ReinitializeVirtualDesktopAPI() {
  CleanupVirtualDesktopAPI();
  if (!InitializeVirtualDesktopAPI()) return false;
  if (g_enableTiling) RegisterVirtualDesktopNotifications();
  return true;
}

template <typename TResult>
HRESULT CallManagerInternal(int vtableIndex, TResult* outResult) {
  if (UsesHMonitorParameter()) {
    auto pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, TResult*)>(g_pDesktopManagerInternal,
                                                                                         vtableIndex);
    HRESULT hr = pfn(g_pDesktopManagerInternal, nullptr, outResult);
    if (FAILED(hr) && ReinitializeVirtualDesktopAPI()) {
      pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, HMONITOR, TResult*)>(g_pDesktopManagerInternal,
                                                                                      vtableIndex);
      hr = pfn(g_pDesktopManagerInternal, nullptr, outResult);
    }
    return hr;
  } else {
    auto pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, TResult*)>(g_pDesktopManagerInternal, vtableIndex);
    HRESULT hr = pfn(g_pDesktopManagerInternal, outResult);
    if (FAILED(hr) && ReinitializeVirtualDesktopAPI()) {
      pfn = GetVTableFunction<HRESULT(STDMETHODCALLTYPE*)(void*, TResult*)>(g_pDesktopManagerInternal, vtableIndex);
      hr = pfn(g_pDesktopManagerInternal, outResult);
    }
    return hr;
  }
}

bool GetCurrentDesktopId(GUID* outGuid) {
  if (!outGuid) return false;
  if (!g_pDesktopManagerInternal) return false;

  IVirtualDesktop* desktop = nullptr;
  HRESULT hr = CallManagerInternal(VTABLE_GET_CURRENT_DESKTOP, &desktop);
  if (FAILED(hr) || !desktop) {
    Wh_Log(L"GetCurrentDesktop failed: 0x%08X", hr);
    return false;
  }

  hr = desktop->GetId(outGuid);
  desktop->Release();
  return SUCCEEDED(hr);
}

bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid) {
  if (!outGuid) return false;
  if (!InitializeVirtualDesktopAPI() || !g_pDesktopManager) return false;
  return SUCCEEDED(g_pDesktopManager->GetWindowDesktopId(hwnd, outGuid));
}

//=============================================================================
// Window Tiling
//=============================================================================

static const wchar_t* kIgnoredWindowClasses[] = {
  L"Progman", 
  L"WorkerW", 
  L"Shell_TrayWnd", 
  L"Shell_SecondaryTrayWnd",
  L"Windows.UI.Core.CoreWindow", 
  L"OperationStatusWindow",
};

bool IsWindowCloaked(HWND hwnd) {
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  return cloaked;
}

static bool IsWindowTrackedInAnyState(HWND hwnd) {
  if (!hwnd) return false;

  // normalize to root owner so events from owned windows still match
  HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
  if (root) hwnd = root;

  bool tracked = false;

  AcquireSRWLockShared(&g_tilingStateLock);
  for (const auto& kv : g_tilingStateMap) {
    const TilingState& st = kv.second;
    if (st.records.find(hwnd) != st.records.end()) {
      tracked = true;
      break;
    }
  }
  ReleaseSRWLockShared(&g_tilingStateLock);

  return tracked;
}

static bool CouldBeTileEligible(HWND hwnd) {
  if (!hwnd) return false;

  if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER)) return false;

  LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
  if (style & WS_CHILD) return false;

  // window could temporarily lose its WS_SizeBox. 
  // (example: Office save/discard dialog)
  if (!(style & WS_SIZEBOX) && !IsWindowTrackedInAnyState(hwnd)) return false;

  LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;

  wchar_t className[64];
  if (GetClassNameW(hwnd, className, 64)) {
    for (const auto* ignoredClass : kIgnoredWindowClasses) {
      if (_wcsicmp(className, ignoredClass) == 0) return false;
    }
  }

  return true;
}

bool IsTileEligible(HWND hwnd, HMONITOR targetMonitor) {
  // window might become temporarily disabled
  // (example: VSCode unstaged commit dialog)
  if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return false;

  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
  if (!CouldBeTileEligible(hwnd)) return false;
  if (IsWindowCloaked(hwnd)) return false;

  // Virtual desktop filter: other desktops' windows may not yet be cloaked
  // at the moment we enumerate (race between uncloak/cloak during switch).
  if (g_pDesktopManager) {
    BOOL onCurrent = FALSE;
    if (SUCCEEDED(g_pDesktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) && !onCurrent)
      return false;
  }

  RECT frameRect;
  if (!SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect)))) {
    if (!GetWindowRect(hwnd, &frameRect)) return false;
  }

  return frameRect.right > frameRect.left && frameRect.bottom > frameRect.top &&
         MonitorFromRect(&frameRect, MONITOR_DEFAULTTONULL) == targetMonitor;
}

static bool RectsNear(const RECT& a, const RECT& b, LONG tolerance = kPlacementTolerancePx) {
  return !Differs(a.left, b.left, tolerance) &&
         !Differs(a.top, b.top, tolerance) &&
         !Differs(a.right, b.right, tolerance) &&
         !Differs(a.bottom, b.bottom, tolerance);
}

static PlacementResult PlaceWindowChecked(WindowRecord& record, const RECT& targetRect) {
  HWND hwnd = record.hwnd;
  record.lastRequestedRect = targetRect;

  auto finish = [&](PlacementResult result) {
    record.lastPlacementResult = result;
    return result;
  };

  if (!hwnd || !IsWindow(hwnd)) return finish(PlacementResult::Dead);
  if (!record.canMove) return finish(PlacementResult::Refused);

  if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);

  RECT windowRect{}, extendedFrame{};
  LONG offsetLeft = 0, offsetTop = 0, offsetRight = 0, offsetBottom = 0;

  if (GetWindowRect(hwnd, &windowRect) &&
      SUCCEEDED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extendedFrame, sizeof(extendedFrame)))) {
    offsetLeft = extendedFrame.left - windowRect.left;
    offsetTop = extendedFrame.top - windowRect.top;
    offsetRight = windowRect.right - extendedFrame.right;
    offsetBottom = windowRect.bottom - extendedFrame.bottom;
  }

  SetLastError(ERROR_SUCCESS);
  BOOL placed = SetWindowPos(
      hwnd, nullptr,
      targetRect.left - offsetLeft,
      targetRect.top - offsetTop,
      targetRect.right - targetRect.left + offsetLeft + offsetRight,
      targetRect.bottom - targetRect.top + offsetTop + offsetBottom,
      SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

  if (!placed) {
    DWORD error = GetLastError();
    return finish(error == ERROR_ACCESS_DENIED
                      ? PlacementResult::AccessDenied
                      : PlacementResult::Refused);
  }

  RECT observed{};
  if (!GetWindowFrameRect(hwnd, &observed)) {
    return finish(IsWindow(hwnd)
                      ? PlacementResult::Refused
                      : PlacementResult::Dead);
  }

  record.lastObservedRect = observed;
  return finish(RectsNear(observed, targetRect)
                    ? PlacementResult::Success
                    : PlacementResult::AdjustedByWindow);
}

static void ArrangeWorkspace(const DesktopMonitorKey& key);

void SwapMaster() {
  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
  if (!mon) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{desktopId, mon};
  bool didSwap = false;
  HWND oldMaster = nullptr;
  HWND resolved = nullptr;

  AcquireSRWLockExclusive(&g_tilingStateLock);
  auto it = g_tilingStateMap.find(key);
  if (it != g_tilingStateMap.end() && it->second.windows.size() >= 2) {
    TilingState& state = it->second;
    if (state.layout == TileLayout::MasterStack ||
        state.layout == TileLayout::MasterStackH ||
        state.layout == TileLayout::Columns ||
        state.layout == TileLayout::Rows) {
      resolved = ResolveToTiledWindow(fg, state.windows);
      auto resolvedIt = std::find(state.windows.begin(), state.windows.end(), resolved);
      if (resolved && resolvedIt != state.windows.end() && resolvedIt != state.windows.begin()) {
        oldMaster = state.windows.front();
        std::iter_swap(state.windows.begin(), resolvedIt);
        didSwap = true;
      }
    }
  }
  ReleaseSRWLockExclusive(&g_tilingStateLock);

  if (!didSwap) return;

  // Purge user move/size samples for both windows, then render the new logical
  // order through the same checked-placement path used by normal arrangements.
  AcquireSRWLockExclusive(&g_moveSizeRectsLock);
  g_moveSizeStartRects.erase(oldMaster);
  g_moveSizeEndRects.erase(oldMaster);
  g_moveSizeStartRects.erase(resolved);
  g_moveSizeEndRects.erase(resolved);
  ReleaseSRWLockExclusive(&g_moveSizeRectsLock);

  ArrangeWorkspace(key);
}


//=============================================================================
// Authoritative workspace model + arrangement
//=============================================================================

static inline long long RectAreaLL(const RECT& r) {
  long long w = (long long)r.right - (long long)r.left;
  long long h = (long long)r.bottom - (long long)r.top;
  if (w <= 0 || h <= 0) return 0;
  return w * h;
}

static inline long long WindowAreaOnWorkArea(HWND hwnd, const RECT& workArea) {
  RECT r{};
  if (!GetWindowFrameRect(hwnd, &r)) return 0;
  RECT inter{};
  if (!IntersectRect(&inter, &r, &workArea)) return 0;
  return RectAreaLL(inter);
}

static HMONITOR GetTargetMonitor() {
  HWND foregroundWindow = GetForegroundWindow();
  HMONITOR monitor = foregroundWindow ? MonitorFromWindow(foregroundWindow, MONITOR_DEFAULTTONULL) : nullptr;
  if (!monitor) {
    POINT cursorPos{};
    if (GetCursorPos(&cursorPos)) {
      monitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
    }
  }
  return monitor;
}

static bool GetCurrentWorkspaceKey(HMONITOR monitor, DesktopMonitorKey* outKey, RECT* outWorkArea = nullptr) {
  if (!monitor || !outKey) return false;
  GUID desktopId{};
  // Keep a zero-GUID fallback so manual tiling still works if the private
  // virtual-desktop API is temporarily unavailable. In that degraded mode,
  // desktop-specific separation is unavailable but the WM remains usable.
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&desktopId)) {
    return false;
  }
  outKey->desktopId = desktopId;
  outKey->monitor = monitor;

  if (outWorkArea) {
    RECT monitorWork{};
    if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;
    *outWorkArea = {monitorWork.left + g_tileMargin, monitorWork.top + g_tileMargin,
                    monitorWork.right - g_tileMargin, monitorWork.bottom - g_tileMargin};
    if (outWorkArea->right <= outWorkArea->left || outWorkArea->bottom <= outWorkArea->top) return false;
  }
  return true;
}

static void SaveTilingState(const DesktopMonitorKey& key, const TilingState& state) {
  AcquireSRWLockExclusive(&g_tilingStateLock);
  g_tilingStateMap[key] = state;
  ReleaseSRWLockExclusive(&g_tilingStateLock);
}

static bool LoadTilingState(const DesktopMonitorKey& key, TilingState* state) {
  if (!state) return false;
  bool found = false;
  AcquireSRWLockShared(&g_tilingStateLock);
  auto it = g_tilingStateMap.find(key);
  if (it != g_tilingStateMap.end()) {
    *state = it->second;
    found = true;
  }
  ReleaseSRWLockShared(&g_tilingStateLock);
  return found;
}


//=============================================================================
// Notification-area layout indicator + layout flyout
//=============================================================================
//
// UI boundary for the tool mod. This subsystem is Win32/GDI-only: no Explorer
// hooks, XAML, WinUI, or Windows App SDK dependency. The tray uses two-letter
// mode abbreviations that are fitted to the full icon canvas. The flyout is a
// small non-activating card shown only after an explicit layout change.

namespace TrayUi {

//-----------------------------------------------------------------------------
// Shared UI state / naming
//-----------------------------------------------------------------------------

constexpr wchar_t kTrayWindowClassName[] = L"WindhawkTilingHelperTrayWindow";
constexpr wchar_t kFlyoutWindowClassName[] = L"WindhawkTilingHelperLayoutFlyout";
constexpr UINT kIconId = 1;
constexpr int kIconCanvasSize = 32;
constexpr UINT_PTR kFlyoutTimerId = 1;
constexpr DWORD kFlyoutHoldMs = 850;
constexpr DWORD kFlyoutFadeMs = 180;

static HWND g_trayWindow = nullptr;
static HWND g_flyoutWindow = nullptr;
static ATOM g_trayWindowClass = 0;
static ATOM g_flyoutWindowClass = 0;
static UINT g_taskbarCreatedMessage = 0;
static bool g_iconAdded = false;
static bool g_systemLightTheme = false;
static TileLayout g_displayedLayout = TileLayout::COUNT;
static GUID g_displayedDesktopId{};
static bool g_haveDisplayedDesktopId = false;
static TileLayout g_flyoutLayout = TileLayout::COUNT;
static DWORD g_flyoutShownTick = 0;
static NOTIFYICONDATAW g_notifyData{};
static std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> g_icons{};

static const wchar_t* LayoutDisplayName(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack:
      return L"Master + Stack (Vertical)";
    case TileLayout::MasterStackH:
      return L"Master + Stack (Horizontal)";
    case TileLayout::Columns:
      return L"Columns";
    case TileLayout::Rows:
      return L"Rows";
    case TileLayout::BSP:
      return L"BSP";
    case TileLayout::Monocle:
      return L"Monocle";
    case TileLayout::Floating:
      return L"Floating";
    case TileLayout::COUNT:
    default:
      return L"Unknown";
  }
}

static const wchar_t* LayoutTrayText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack:
      return L"M│";
    case TileLayout::MasterStackH:
      return L"M─";
    case TileLayout::Columns:
      return L"CL";
    case TileLayout::Rows:
      return L"RW";
    case TileLayout::BSP:
      return L"BS";
    case TileLayout::Monocle:
      return L"MN";
    case TileLayout::Floating:
      return L"FL";
    case TileLayout::COUNT:
    default:
      return L"??";
  }
}

static const wchar_t* LayoutFlyoutText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack:
      return L"Master + Stack V";
    case TileLayout::MasterStackH:
      return L"Master + Stack H";
    case TileLayout::Columns:
      return L"Columns";
    case TileLayout::Rows:
      return L"Rows";
    case TileLayout::BSP:
      return L"BSP";
    case TileLayout::Monocle:
      return L"Monocle";
    case TileLayout::Floating:
      return L"Floating";
    case TileLayout::COUNT:
    default:
      return L"Unknown";
  }
}

static bool ReadSystemLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (status == ERROR_SUCCESS) return value != 0;

  COLORREF fallback = GetSysColor(COLOR_WINDOW);
  return (GetRValue(fallback) + GetGValue(fallback) + GetBValue(fallback)) >= 384;
}

static COLORREF TrayForegroundColor() {
  return g_systemLightTheme ? RGB(24, 24, 24) : RGB(255, 255, 255);
}

static COLORREF FlyoutBackgroundColor() {
  return g_systemLightTheme ? RGB(243, 243, 243) : RGB(36, 36, 36);
}

static COLORREF FlyoutForegroundColor() {
  return g_systemLightTheme ? RGB(24, 24, 24) : RGB(255, 255, 255);
}

//-----------------------------------------------------------------------------
// Theme-aware text tray icons
//-----------------------------------------------------------------------------

static HICON CreateTextIcon(TileLayout layout) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = kIconCanvasSize;
  bmi.bmiHeader.biHeight = -kIconCanvasSize;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* dibBits = nullptr;
  HBITMAP colorBitmap =
      CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
  if (!colorBitmap || !dibBits) {
    if (colorBitmap) DeleteObject(colorBitmap);
    return nullptr;
  }

  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) {
    DeleteObject(colorBitmap);
    return nullptr;
  }

  HGDIOBJ oldBitmap = SelectObject(dc, colorBitmap);
  RECT canvas{0, 0, kIconCanvasSize, kIconCanvasSize};
  HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(dc, &canvas, black);
  DeleteObject(black);

  SetBkMode(dc, OPAQUE);
  SetBkColor(dc, RGB(0, 0, 0));
  SetTextColor(dc, RGB(255, 255, 255));

  const wchar_t* text = LayoutTrayText(layout);
  const int textLength = static_cast<int>(wcslen(text));
  constexpr int kTargetWidth = kIconCanvasSize - 2;
  constexpr int kTargetHeight = kIconCanvasSize - 2;

  HFONT chosenFont = nullptr;
  SIZE naturalSize{};

  // All modes use the same maximum cap-height. Width is normalized separately
  // below, so wide pairs such as RW no longer force a smaller font than CL/FL.
  for (int pixelHeight = kIconCanvasSize; pixelHeight >= 8; --pixelHeight) {
    HFONT font = CreateFontW(-pixelHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) continue;

    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(dc, text, textLength, &textSize);
    SelectObject(dc, oldFont);

    if (textSize.cy <= kTargetHeight) {
      chosenFont = font;
      naturalSize = textSize;
      break;
    }
    DeleteObject(font);
  }

  if (!chosenFont) {
    chosenFont = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, chosenFont);
    GetTextExtentPoint32W(dc, text, textLength, &naturalSize);
    SelectObject(dc, oldFont);
  }

  HGDIOBJ oldFont = SelectObject(dc, chosenFont);
  int previousGraphicsMode = SetGraphicsMode(dc, GM_ADVANCED);

  // Preserve one common vertical size, then normalize each label to the same
  // visual width. The shell therefore receives consistently large tray glyphs
  // instead of shrinking labels merely because one contains a W/M.
  float xScale = naturalSize.cx > 0
                     ? static_cast<float>(kTargetWidth) /
                           static_cast<float>(naturalSize.cx)
                     : 1.0f;
  XFORM transform{xScale, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  bool transformed = previousGraphicsMode != 0 &&
                     SetWorldTransform(dc, &transform) != FALSE;

  int outputWidth = transformed
                        ? kTargetWidth
                        : std::min(kTargetWidth, static_cast<int>(naturalSize.cx));
  int deviceX = (kIconCanvasSize - outputWidth) / 2;
  int logicalX = transformed
                     ? static_cast<int>(std::lround(deviceX / xScale))
                     : deviceX;
  int y = (kIconCanvasSize - naturalSize.cy) / 2;
  TextOutW(dc, logicalX, y, text, textLength);

  if (transformed) {
    ModifyWorldTransform(dc, nullptr, MWT_IDENTITY);
  }
  if (previousGraphicsMode != 0) {
    SetGraphicsMode(dc, previousGraphicsMode);
  }
  SelectObject(dc, oldFont);
  DeleteObject(chosenFont);

  // GDI doesn't populate alpha in a 32bpp DIB. Render white-on-black, use the
  // antialiased intensity as coverage, then recolor that coverage for the
  // current taskbar theme. This preserves smooth edges with a transparent BG.
  auto* pixels = static_cast<uint32_t*>(dibBits);
  COLORREF foreground = TrayForegroundColor();
  const uint32_t fr = GetRValue(foreground);
  const uint32_t fg = GetGValue(foreground);
  const uint32_t fb = GetBValue(foreground);
  for (int i = 0; i < kIconCanvasSize * kIconCanvasSize; ++i) {
    uint32_t pixel = pixels[i];
    uint32_t coverage = std::max({pixel & 0xFFu,
                                  (pixel >> 8) & 0xFFu,
                                  (pixel >> 16) & 0xFFu});
    pixels[i] = (coverage << 24) | (fr << 16) | (fg << 8) | fb;
  }

  SelectObject(dc, oldBitmap);
  DeleteDC(dc);

  constexpr int kMaskStride = ((kIconCanvasSize + 31) / 32) * 4;
  std::array<BYTE, kMaskStride * kIconCanvasSize> maskBits{};
  HBITMAP maskBitmap = CreateBitmap(kIconCanvasSize, kIconCanvasSize, 1, 1,
                                    maskBits.data());
  if (!maskBitmap) {
    DeleteObject(colorBitmap);
    return nullptr;
  }

  ICONINFO iconInfo{};
  iconInfo.fIcon = TRUE;
  iconInfo.hbmColor = colorBitmap;
  iconInfo.hbmMask = maskBitmap;
  HICON icon = CreateIconIndirect(&iconInfo);

  DeleteObject(maskBitmap);
  DeleteObject(colorBitmap);
  return icon;
}

static HICON GetLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_icons.size() && g_icons[index]) return g_icons[index];
  return LoadIconW(nullptr, IDI_APPLICATION);
}

static void DestroyIconSet(
    std::array<HICON, static_cast<size_t>(TileLayout::COUNT)>& icons) {
  for (HICON& icon : icons) {
    if (icon) {
      DestroyIcon(icon);
      icon = nullptr;
    }
  }
}

static void SetTooltip(TileLayout layout) {
  std::wstring tooltip = L"Tiling Helper — ";
  tooltip += LayoutDisplayName(layout);
  wcsncpy(g_notifyData.szTip, tooltip.c_str(), ARRAYSIZE(g_notifyData.szTip) - 1);
  g_notifyData.szTip[ARRAYSIZE(g_notifyData.szTip) - 1] = L'\0';
}

static bool AddIconToShell() {
  if (!g_trayWindow) return false;
  if (g_displayedLayout == TileLayout::COUNT) g_displayedLayout = g_defaultLayout;

  g_notifyData = {};
  g_notifyData.cbSize = sizeof(g_notifyData);
  g_notifyData.hWnd = g_trayWindow;
  g_notifyData.uID = kIconId;
  g_notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  g_notifyData.hIcon = GetLayoutIcon(g_displayedLayout);
  SetTooltip(g_displayedLayout);

  if (!Shell_NotifyIconW(NIM_ADD, &g_notifyData)) {
    g_iconAdded = false;
    Wh_Log(L"Failed to add tiling layout tray icon: %lu", GetLastError());
    return false;
  }

  g_iconAdded = true;
  g_notifyData.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_notifyData);
  return true;
}

static void UpdateIcon(TileLayout layout) {
  if (!g_trayWindow || layout == TileLayout::COUNT) return;
  g_displayedLayout = layout;

  if (!g_iconAdded) {
    AddIconToShell();
    return;
  }

  g_notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  g_notifyData.hIcon = GetLayoutIcon(layout);
  SetTooltip(layout);
  if (!Shell_NotifyIconW(NIM_MODIFY, &g_notifyData)) {
    g_iconAdded = false;
    AddIconToShell();
  }
}

static void RebuildIconsForTheme() {
  std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> newIcons{};
  for (size_t i = 0; i < newIcons.size(); ++i) {
    newIcons[i] = CreateTextIcon(static_cast<TileLayout>(i));
  }

  auto oldIcons = g_icons;
  g_icons = newIcons;
  if (g_displayedLayout != TileLayout::COUNT) UpdateIcon(g_displayedLayout);
  DestroyIconSet(oldIcons);
}

//-----------------------------------------------------------------------------
// Compact layout flyout
//-----------------------------------------------------------------------------

static int ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

static HFONT CreateFlyoutFont(UINT dpi) {
  return CreateFontW(-ScaleForDpi(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static LRESULT CALLBACK FlyoutWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
  switch (message) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);

      HBRUSH background = CreateSolidBrush(FlyoutBackgroundColor());
      FillRect(dc, &client, background);
      DeleteObject(background);

      UINT dpi = GetDpiForWindow(hwnd);
      HFONT font = CreateFlyoutFont(dpi);
      HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, FlyoutForegroundColor());
      DrawTextW(dc, LayoutFlyoutText(g_flyoutLayout), -1, &client,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (oldFont) SelectObject(dc, oldFont);
      if (font) DeleteObject(font);

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_TIMER:
      if (wParam == kFlyoutTimerId) {
        DWORD elapsed = GetTickCount() - g_flyoutShownTick;
        if (elapsed >= kFlyoutHoldMs + kFlyoutFadeMs) {
          KillTimer(hwnd, kFlyoutTimerId);
          ShowWindow(hwnd, SW_HIDE);
          return 0;
        }

        BYTE alpha = 255;
        if (elapsed > kFlyoutHoldMs) {
          DWORD fadeElapsed = elapsed - kFlyoutHoldMs;
          alpha = static_cast<BYTE>(255 -
              std::min<DWORD>(255, (fadeElapsed * 255) / kFlyoutFadeMs));
        }
        SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
        return 0;
      }
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void ShowLayoutFlyout(TileLayout layout) {
  if (!g_flyoutWindow || layout == TileLayout::COUNT) return;

  HMONITOR monitor = GetTargetMonitor();
  RECT workArea{};
  if (!monitor || !GetMonitorWorkArea(monitor, &workArea)) return;

  UINT dpi = GetDpiForWindow(g_flyoutWindow);
  if (!dpi) dpi = 96;
  HFONT font = CreateFlyoutFont(dpi);
  HDC dc = GetDC(g_flyoutWindow);
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SIZE textSize{};
  const wchar_t* text = LayoutFlyoutText(layout);
  GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &textSize);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);
  ReleaseDC(g_flyoutWindow, dc);

  const int horizontalPadding = ScaleForDpi(24, dpi);
  const int width = std::max(ScaleForDpi(128, dpi),
                             static_cast<int>(textSize.cx) + horizontalPadding * 2);
  const int height = ScaleForDpi(48, dpi);
  const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
  const int y = workArea.bottom - height - ScaleForDpi(16, dpi);

  const int radius = ScaleForDpi(8, dpi);
  HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                   radius * 2, radius * 2);
  if (region && !SetWindowRgn(g_flyoutWindow, region, TRUE)) {
    DeleteObject(region);
  }

  g_flyoutLayout = layout;
  g_flyoutShownTick = GetTickCount();
  SetLayeredWindowAttributes(g_flyoutWindow, 0, 255, LWA_ALPHA);
  SetWindowPos(g_flyoutWindow, HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(g_flyoutWindow, nullptr, TRUE);
  UpdateWindow(g_flyoutWindow);
  SetTimer(g_flyoutWindow, kFlyoutTimerId, 16, nullptr);
}

//-----------------------------------------------------------------------------
// Tray/flyout lifecycle and shell/theme integration
//-----------------------------------------------------------------------------

static void RefreshForCurrentWorkspace() {
  TileLayout layout = g_defaultLayout;
  HMONITOR monitor = GetTargetMonitor();
  DesktopMonitorKey key{};
  TilingState state;
  bool haveWorkspaceKey = monitor && GetCurrentWorkspaceKey(monitor, &key);
  if (haveWorkspaceKey && LoadTilingState(key, &state)) {
    layout = state.layout;
  }

  bool desktopChanged =
      haveWorkspaceKey && g_haveDisplayedDesktopId &&
      !IsEqualGUID(key.desktopId, g_displayedDesktopId);
  if (haveWorkspaceKey) {
    g_displayedDesktopId = key.desktopId;
    g_haveDisplayedDesktopId = true;
  }

  UpdateIcon(layout);
  if (desktopChanged) {
    ShowLayoutFlyout(layout);
  }
}

static void HandleThemeChanged() {
  bool lightTheme = ReadSystemLightTheme();
  if (lightTheme == g_systemLightTheme) return;

  g_systemLightTheme = lightTheme;
  RebuildIconsForTheme();
  if (g_flyoutWindow && IsWindowVisible(g_flyoutWindow)) {
    InvalidateRect(g_flyoutWindow, nullptr, TRUE);
  }
}

static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
  if (g_taskbarCreatedMessage && message == g_taskbarCreatedMessage) {
    g_iconAdded = false;
    AddIconToShell();
    return 0;
  }

  switch (message) {
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      HandleThemeChanged();
      return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

static bool Initialize() {
  g_systemLightTheme = ReadSystemLightTheme();
  for (size_t i = 0; i < g_icons.size(); ++i) {
    g_icons[i] = CreateTextIcon(static_cast<TileLayout>(i));
  }

  HINSTANCE instance = GetModuleHandleW(nullptr);

  WNDCLASSW trayClass{};
  trayClass.lpfnWndProc = TrayWindowProc;
  trayClass.hInstance = instance;
  trayClass.lpszClassName = kTrayWindowClassName;
  g_trayWindowClass = RegisterClassW(&trayClass);
  if (!g_trayWindowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Wh_Log(L"Failed to register tiling tray window class: %lu", GetLastError());
    return false;
  }

  WNDCLASSW flyoutClass{};
  flyoutClass.lpfnWndProc = FlyoutWindowProc;
  flyoutClass.hInstance = instance;
  flyoutClass.lpszClassName = kFlyoutWindowClassName;
  g_flyoutWindowClass = RegisterClassW(&flyoutClass);
  bool flyoutClassAvailable = g_flyoutWindowClass != 0;
  if (!flyoutClassAvailable) {
    DWORD classError = GetLastError();
    flyoutClassAvailable = classError == ERROR_CLASS_ALREADY_EXISTS;
    if (!flyoutClassAvailable) {
      Wh_Log(L"Failed to register tiling layout flyout class: %lu", classError);
    }
  }

  // Hidden top-level owner: Explorer broadcasts TaskbarCreated to top-level
  // windows after a shell restart, which lets us restore the notification icon.
  g_trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClassName, L"",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 instance, nullptr);
  if (!g_trayWindow) {
    Wh_Log(L"Failed to create tiling tray owner window: %lu", GetLastError());
    return false;
  }

  if (flyoutClassAvailable) {
    g_flyoutWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kFlyoutWindowClassName, L"", WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, instance, nullptr);
    if (!g_flyoutWindow) {
      Wh_Log(L"Failed to create tiling layout flyout window: %lu", GetLastError());
    }
  }

  g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
  g_displayedLayout = g_defaultLayout;
  RefreshForCurrentWorkspace();
  return g_iconAdded;
}

static void Shutdown() {
  if (g_iconAdded && g_trayWindow) {
    g_notifyData.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &g_notifyData);
  }
  g_iconAdded = false;

  if (g_flyoutWindow) {
    KillTimer(g_flyoutWindow, kFlyoutTimerId);
    DestroyWindow(g_flyoutWindow);
    g_flyoutWindow = nullptr;
  }
  if (g_trayWindow) {
    DestroyWindow(g_trayWindow);
    g_trayWindow = nullptr;
  }

  DestroyIconSet(g_icons);

  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (g_flyoutWindowClass) {
    UnregisterClassW(kFlyoutWindowClassName, instance);
    g_flyoutWindowClass = 0;
  }
  if (g_trayWindowClass) {
    UnregisterClassW(kTrayWindowClassName, instance);
    g_trayWindowClass = 0;
  }

  g_taskbarCreatedMessage = 0;
  g_displayedLayout = TileLayout::COUNT;
  g_displayedDesktopId = {};
  g_haveDisplayedDesktopId = false;
  g_flyoutLayout = TileLayout::COUNT;
  g_notifyData = {};
}

}  // namespace TrayUi


// Keep the active layout's weight vector aligned with its kind.
// A size mismatch resets that layout's weights to equal shares.
static void EnsureSaneWeightVector(TilingState& state) {
  if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    // windows[0] is the master and is sized by masterRatio. Remaining
    // windows are in the stack. N - 1 weights.
    size_t stackCount = state.windows.empty() ? 0 : state.windows.size() - 1;
    if (state.stackWeights.size() != stackCount) state.stackWeights = DefaultWeights(stackCount);
    // Grid weights are meaningful only for Columns and Rows layouts. Discard here.
    state.gridWeights.clear();
  } else if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    // There is no separate master in these layouts. Exactly N entries please.
    if (state.gridWeights.size() != state.windows.size()) state.gridWeights = DefaultWeights(state.windows.size());
    // Stack weights are meaningful only for the MasterStack layouts. Discard here.
    state.stackWeights.clear();
  } else {
    // BSP, Monocle, and Floating do not use either kind of weight.
    state.stackWeights.clear();
    state.gridWeights.clear();
  }
}

static void RemoveActiveWindowAt(TilingState& state, size_t index, SuspendedSlot* saved = nullptr) {
  if (index >= state.windows.size()) return;
  SuspendedSlot slot{};
  slot.index = index;

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    if (index < state.gridWeights.size()) {
      slot.weight = state.gridWeights[index];
      slot.hasWeight = true;
      state.gridWeights.erase(state.gridWeights.begin() + index);
    }
  } else if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    if (index == 0) {
      slot.wasMaster = true;
      // The temporary master has different stack semantics. Rebuild weights
      // rather than pretending the old stack vector still maps 1:1.
      state.stackWeights.clear();
    } else if (index - 1 < state.stackWeights.size()) {
      slot.weight = state.stackWeights[index - 1];
      slot.hasWeight = true;
      state.stackWeights.erase(state.stackWeights.begin() + (index - 1));
    }
  }

  state.windows.erase(state.windows.begin() + index);
  if (saved) *saved = slot;
  EnsureSaneWeightVector(state);
}

static size_t ActiveIndexToLogicalIndex(const TilingState& state, size_t activeIndex) {
  std::vector<size_t> suspendedIndices;
  for (const auto& kv : state.records) {
    const WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot) {
      suspendedIndices.push_back(record.savedSlot.index);
    }
  }
  std::sort(suspendedIndices.begin(), suspendedIndices.end());

  size_t logicalIndex = activeIndex;
  for (size_t suspendedIndex : suspendedIndices) {
    if (suspendedIndex <= logicalIndex) ++logicalIndex;
    else break;
  }
  return logicalIndex;
}

static size_t LogicalIndexToActiveIndex(
    const TilingState& state, size_t logicalIndex, HWND restoringHwnd) {
  size_t suspendedBefore = 0;
  for (const auto& kv : state.records) {
    const WindowRecord& record = kv.second;
    if (kv.first != restoringHwnd &&
        record.state == ManageState::Suspended &&
        record.hasSavedSlot &&
        record.savedSlot.index < logicalIndex) {
      ++suspendedBefore;
    }
  }
  return logicalIndex >= suspendedBefore ? logicalIndex - suspendedBefore : 0;
}

static void CloseLogicalGapAfterRemoval(
    TilingState& state, size_t removedLogicalIndex, HWND exceptHwnd = nullptr) {
  for (auto& kv : state.records) {
    WindowRecord& record = kv.second;
    if (kv.first != exceptHwnd &&
        record.state == ManageState::Suspended &&
        record.hasSavedSlot &&
        record.savedSlot.index > removedLogicalIndex) {
      --record.savedSlot.index;
    }
  }
}

static bool SuspendWindowInState(TilingState& state, HWND hwnd) {
  auto it = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (it == state.windows.end()) return false;

  WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Tiled);
  SuspendedSlot slot{};
  size_t activeIndex = static_cast<size_t>(it - state.windows.begin());
  size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
  RemoveActiveWindowAt(state, activeIndex, &slot);
  slot.index = logicalIndex;

  record.state = ManageState::Suspended;
  record.savedSlot = slot;
  record.hasSavedSlot = true;
  return true;
}

static bool RestoreWindowInState(TilingState& state, HWND hwnd) {
  WindowRecord* record = FindWindowRecord(state, hwnd);
  if (!record || record->state != ManageState::Suspended || !record->hasSavedSlot) {
    return false;
  }

  SuspendedSlot slot = record->savedSlot;
  size_t index = std::min(
      LogicalIndexToActiveIndex(state, slot.index, hwnd), state.windows.size());
  state.windows.insert(state.windows.begin() + index, hwnd);

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    if (state.gridWeights.size() + 1 == state.windows.size()) {
      state.gridWeights.insert(
          state.gridWeights.begin() + std::min(index, state.gridWeights.size()),
          slot.hasWeight ? slot.weight : 1.0);
    }
  } else if (state.layout == TileLayout::MasterStack ||
             state.layout == TileLayout::MasterStackH) {
    if (slot.wasMaster) {
      state.stackWeights.clear();
    } else if (index > 0 &&
               state.stackWeights.size() + 1 == state.windows.size() - 1) {
      size_t stackIndex = std::min(index - 1, state.stackWeights.size());
      state.stackWeights.insert(
          state.stackWeights.begin() + stackIndex,
          slot.hasWeight ? slot.weight : 1.0);
    }
  }

  record->state = ManageState::Tiled;
  record->savedSlot = {};
  record->hasSavedSlot = false;
  EnsureSaneWeightVector(state);
  return true;
}

static bool FloatWindowInState(TilingState& state, HWND hwnd) {
  WindowRecord* existing = FindWindowRecord(state, hwnd);
  if (existing && existing->state == ManageState::Floating) return false;

  auto activeIt = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (activeIt != state.windows.end()) {
    size_t activeIndex = static_cast<size_t>(activeIt - state.windows.begin());
    size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
    RemoveActiveWindowAt(state, activeIndex, nullptr);
    CloseLogicalGapAfterRemoval(state, logicalIndex);
  } else if (existing &&
             existing->state == ManageState::Suspended &&
             existing->hasSavedSlot) {
    CloseLogicalGapAfterRemoval(state, existing->savedSlot.index, hwnd);
  }

  WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Floating);
  record.state = ManageState::Floating;
  record.savedSlot = {};
  record.hasSavedSlot = false;
  EnsureSaneWeightVector(state);
  return true;
}

// Remove a window from management entirely. Floating/ignored windows have no
// logical tiled slot, while tiled/suspended windows close their layout gap.
static bool ForgetWindowInState(TilingState& state, HWND hwnd) {
  auto activeIt = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (activeIt != state.windows.end()) {
    size_t activeIndex = static_cast<size_t>(activeIt - state.windows.begin());
    size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
    RemoveActiveWindowAt(state, activeIndex, nullptr);
    CloseLogicalGapAfterRemoval(state, logicalIndex);
    state.records.erase(hwnd);
    return true;
  }

  auto recordIt = state.records.find(hwnd);
  if (recordIt == state.records.end()) return false;

  if (recordIt->second.state == ManageState::Suspended &&
      recordIt->second.hasSavedSlot) {
    size_t logicalIndex = recordIt->second.savedSlot.index;
    state.records.erase(recordIt);
    CloseLogicalGapAfterRemoval(state, logicalIndex);
  } else {
    state.records.erase(recordIt);
  }

  EnsureSaneWeightVector(state);
  return true;
}

static void ArrangeWorkspace(const DesktopMonitorKey& key) {
  // Never drive windows on an inactive virtual desktop. During a VD switch,
  // the inactive workspace keeps its logical membership unchanged; settled
  // visibility is reconciled only for the desktop that becomes current.
  GUID zeroGuid{};
  if (!IsEqualGUID(key.desktopId, zeroGuid)) {
    GUID currentDesktop{};
    if (GetCurrentDesktopId(&currentDesktop) &&
        !IsEqualGUID(currentDesktop, key.desktopId)) {
      return;
    }
  }

  TilingState state;
  if (!LoadTilingState(key, &state)) return;

  RECT monitorWork{};
  if (!GetMonitorWorkArea(key.monitor, &monitorWork)) return;
  RECT workArea = {
      monitorWork.left + g_tileMargin,
      monitorWork.top + g_tileMargin,
      monitorWork.right - g_tileMargin,
      monitorWork.bottom - g_tileMargin};
  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) return;

  // Repair missed destroy events from the authoritative record table.
  std::vector<HWND> invalidWindows;
  for (const auto& kv : state.records) {
    if (!IsWindow(kv.first)) invalidWindows.push_back(kv.first);
  }
  bool participationChanged = false;
  for (HWND hwnd : invalidWindows) {
    if (ForgetWindowInState(state, hwnd)) participationChanged = true;
  }

  // Defensive migration for state constructed by older helper paths: every active
  // tiled participant must have a Tiled WindowRecord.
  for (HWND hwnd : state.windows) {
    WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Tiled);
    if (record.state != ManageState::Tiled) {
      record.state = ManageState::Tiled;
      record.savedSlot = {};
      record.hasSavedSlot = false;
    }
  }

  EnsureSaneWeightVector(state);

  // Workspace-floating means records/order remain managed but geometry is wholly
  // user-controlled. Individual Floating records are already excluded from
  // state.windows and therefore don't consume layout slots.
  if (state.layout == TileLayout::Floating || state.windows.empty()) {
    SaveTilingState(key, state);
    return;
  }

  std::vector<RECT> windowRects(state.windows.size());
  switch (state.layout) {
    case TileLayout::MasterStack:
      LayoutMasterStackWeighted(
          workArea, state.windows.size(), windowRects, false,
          ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      break;
    case TileLayout::MasterStackH:
      LayoutMasterStackWeighted(
          workArea, state.windows.size(), windowRects, true,
          ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      break;
    case TileLayout::Columns:
      LayoutGridWeighted(
          workArea, state.windows.size(), windowRects, false, state.gridWeights);
      break;
    case TileLayout::Rows:
      LayoutGridWeighted(
          workArea, state.windows.size(), windowRects, true, state.gridWeights);
      break;
    case TileLayout::BSP:
      LayoutBSP(workArea, 0, state.windows.size(), 0, windowRects);
      break;
    case TileLayout::Monocle:
      windowRects.assign(state.windows.size(), workArea);
      break;
    case TileLayout::Floating:
    case TileLayout::COUNT:
      SaveTilingState(key, state);
      return;
  }

  std::vector<HWND> toFloat;
  std::vector<HWND> toForget;

  for (size_t i = 0; i < state.windows.size(); ++i) {
    HWND hwnd = state.windows[i];
    if (!IsWindow(hwnd)) {
      toForget.push_back(hwnd);
      continue;
    }

    // Rendering remains defensive against state transitions that occur after the
    // settled reconciliation snapshot.
    if (IsIconic(hwnd) || !IsWindowVisible(hwnd) || IsWindowCloaked(hwnd)) {
      continue;
    }

    WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Tiled);
    PlacementResult result = PlaceWindowChecked(record, windowRects[i]);

    switch (result) {
      case PlacementResult::Success:
        record.placementFailures = 0;
        break;

      case PlacementResult::AdjustedByWindow:
      case PlacementResult::AccessDenied:
      case PlacementResult::Refused:
        ++record.placementFailures;
        if (record.placementFailures >= kPlacementFailureLimit) {
          toFloat.push_back(hwnd);
        }
        break;

      case PlacementResult::Dead:
        toForget.push_back(hwnd);
        break;
    }
  }

  for (HWND hwnd : toForget) {
    if (ForgetWindowInState(state, hwnd)) participationChanged = true;
  }

  for (HWND hwnd : toFloat) {
    WindowRecord* record = FindWindowRecord(state, hwnd);
    if (!record || record->state != ManageState::Tiled) continue;

    Wh_Log(L"Window %p failed checked placement %d times (last result=%d); switching it to Floating",
           hwnd, record->placementFailures, static_cast<int>(record->lastPlacementResult));
    if (FloatWindowInState(state, hwnd)) participationChanged = true;
  }

  EnsureSaneWeightVector(state);
  SaveTilingState(key, state);

  // Reflow exactly once after dead/non-compliant windows stop consuming tiled
  // slots. The next pass cannot immediately float the same records because they
  // have already left state.windows.
  if (participationChanged && !state.windows.empty()) {
    ArrangeWorkspace(key);
  }
}

static bool SnapshotLooksTiled(const std::vector<HWND>& windows, const RECT& workArea) {
  if (windows.empty()) return false;
  const long long workAreaArea = RectAreaLL(workArea);
  long long sumArea = 0;
  for (HWND w : windows) sumArea += WindowAreaOnWorkArea(w, workArea);
  const long long lo = (workAreaArea * 85) / 100;
  const long long hi = (windows.size() <= 2) ? (workAreaArea * 115) / 100 : (workAreaArea * 105) / 100;
  return sumArea >= lo && sumArea <= hi;
}

static bool EnsureWorkspaceFromSnapshot(HMONITOR monitor, bool adoptCurrentGeometry) {
  DesktopMonitorKey key{};
  RECT workArea{};
  if (!GetCurrentWorkspaceKey(monitor, &key, &workArea)) return false;

  std::vector<HWND> snapshot = CollectTileWindows(monitor);
  TilingState state;
  bool hadState = LoadTilingState(key, &state);
  if (!hadState) {
    state.layout = g_defaultLayout;
    state.masterRatio = ClampDouble(g_masterPercent / 100.0, 0.1, 0.9);
  }

  // Restore known temporarily absent tiled windows which are visible again.
  for (HWND w : snapshot) {
    WindowRecord* record = FindWindowRecord(state, w);
    if (record && record->state == ManageState::Suspended) {
      RestoreWindowInState(state, w);
    }
  }

  // Windows no longer in the visible snapshot are suspended rather than
  // forgotten. Stable saved slots allow arbitrary restore order.
  std::vector<HWND> toSuspend;
  for (HWND w : state.windows) {
    if (!ContainsWindow(snapshot, w)) toSuspend.push_back(w);
  }
  for (HWND w : toSuspend) SuspendWindowInState(state, w);

  // Manual Tile explicitly re-adopts individually floated windows. Automatic
  // reconciliation never does this, so a user drag exclusion remains stable.
  if (adoptCurrentGeometry) {
    for (HWND w : snapshot) {
      WindowRecord* record = FindWindowRecord(state, w);
      if (record && record->state == ManageState::Floating) {
        record->state = ManageState::Tiled;
        record->placementFailures = 0;
        if (!ContainsWindow(state.windows, w)) state.windows.push_back(w);
      }
    }
  }

  // Preserve known membership/order and append truly new windows.
  for (HWND w : snapshot) {
    if (state.records.find(w) == state.records.end()) {
      state.windows.push_back(w);
      state.records.emplace(w, MakeWindowRecord(w, ManageState::Tiled));
    }
  }

  bool hasSuspended = false;
  for (const auto& kv : state.records) {
    if (kv.second.state == ManageState::Suspended) {
      hasSuspended = true;
      break;
    }
  }

  // Geometry capture remains explicit/manual. Rebuilding from current rectangles
  // also resets placement-failure history for the freshly adopted tiled set.
  if (adoptCurrentGeometry && g_captureLayoutOnTile &&
      state.layout != TileLayout::Floating && !hasSuspended &&
      SnapshotLooksTiled(state.windows, workArea)) {
    HWND preferredMaster = state.windows.empty() ? nullptr : state.windows.front();
    TilingState captured =
        BuildStateFromWindows(state.layout, workArea, state.windows, monitor, preferredMaster);

    // Keep non-participating managed records which aren't in the visible snapshot.
    for (const auto& kv : state.records) {
      if (kv.second.state != ManageState::Tiled) {
        captured.records[kv.first] = kv.second;
      }
    }
    state = std::move(captured);
  } else {
    EnsureSaneWeightVector(state);
  }

  SaveTilingState(key, state);
  ArrangeWorkspace(key);
  return true;
}

// Manual tile: reconcile membership and, when CaptureLayoutOnTile is enabled,
// explicitly adopt the user's current geometry. Automatic events never call this
// with adoptCurrentGeometry=true.
void TileWindows() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;
  if (!EnsureWorkspaceFromSnapshot(monitor, true)) return;
  TrayUi::RefreshForCurrentWorkspace();
  Wh_Log(L"Manual tile/reconcile completed");
}

static bool FindStateContainingWindow(HWND hwnd, DesktopMonitorKey* outKey, TilingState* outState) {
  bool found = false;
  AcquireSRWLockShared(&g_tilingStateLock);
  for (const auto& kv : g_tilingStateMap) {
    if (kv.second.records.find(hwnd) != kv.second.records.end()) {
      if (outKey) *outKey = kv.first;
      if (outState) *outState = kv.second;
      found = true;
      break;
    }
  }
  ReleaseSRWLockShared(&g_tilingStateLock);
  return found;
}

static void ForgetManagedWindow(HWND hwnd) {
  DesktopMonitorKey key{};
  TilingState state;
  if (!FindStateContainingWindow(hwnd, &key, &state)) return;
  if (ForgetWindowInState(state, hwnd)) {
    SaveTilingState(key, state); // Keep empty workspace state and its layout mode.
    ArrangeWorkspace(key);
  }

  AcquireSRWLockExclusive(&g_moveSizeRectsLock);
  g_moveSizeStartRects.erase(hwnd);
  g_moveSizeEndRects.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSizeRectsLock);
}

// If monit
static void AddUniqueMonitor(std::vector<HMONITOR>& monitors, HMONITOR monitor) {
  if (monitor && std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
    monitors.push_back(monitor);
  }
}

static void AddPendingWorkspaceArrange(const GUID& desktopId, HMONITOR monitor) {
  if (!monitor) return;
  // Only if not scheduled already
  auto it = std::find_if(
      g_pendingDesktopArranges.begin(), g_pendingDesktopArranges.end(),
      [&](const PendingWorkspaceArrange& pending) {
        return pending.monitor == monitor && IsEqualGUID(pending.desktopId, desktopId);
      });
  if (it == g_pendingDesktopArranges.end()) {
    g_pendingDesktopArranges.push_back({desktopId, monitor});
  }
}

static void ScheduleLifecycleReconcile(HWND hwnd);

// Reconcile one desktop+monitor workspace from a supplied participation snapshot.
// The caller controls whether that snapshot is cloak-sensitive (settled lifecycle)
// or cloak-insensitive (authoritative virtual-desktop transition).
static bool ReconcileSavedWindowsWithNewDesktop(
    HMONITOR monitor, const GUID& desktopId, const std::vector<HWND>& snapshot,
    bool discoverNew, DesktopMonitorKey* outKey) {
  if (!monitor) return false;

  DesktopMonitorKey key{desktopId, monitor};
  if (outKey) *outKey = key;

  TilingState state;
  bool hadState = LoadTilingState(key, &state);
  if (!hadState) {
    if (!discoverNew || snapshot.empty()) return false;
    state.layout = g_defaultLayout;
    state.masterRatio = ClampDouble(g_masterPercent / 100.0, 0.1, 0.9);
  }

  bool changed = !hadState;

  // Purge dead HWNDs from the authoritative record table.
  std::vector<HWND> deadWindows;
  for (const auto& kv : state.records) {
    if (!IsWindow(kv.first)) deadWindows.push_back(kv.first);
  }
  for (HWND w : deadWindows) {
    if (ForgetWindowInState(state, w)) changed = true;
  }

  // Visible snapshot members restore only if they were previously suspended.
  // Floating/ignored records remain deliberately outside the tiled order.
  for (HWND w : snapshot) {
    WindowRecord* record = FindWindowRecord(state, w);
    if (record && record->state == ManageState::Suspended) {
      if (RestoreWindowInState(state, w)) changed = true;
    }
  }

  std::vector<HWND> toSuspend;
  for (HWND w : state.windows) {
    if (!ContainsWindow(snapshot, w)) toSuspend.push_back(w);
  }
  for (HWND w : toSuspend) {
    if (SuspendWindowInState(state, w)) changed = true;
  }

  if (discoverNew) {
    for (HWND w : snapshot) {
      if (state.records.find(w) == state.records.end()) {
        state.windows.push_back(w);
        state.records.emplace(w, MakeWindowRecord(w, ManageState::Tiled));
        changed = true;
      }
    }
  }

  if (!changed) return false;

  EnsureSaneWeightVector(state);
  SaveTilingState(key, state);
  return true;
}

static void EnumerateCurrentMonitors(std::vector<HMONITOR>& monitors) {
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM lParam) WINAPI -> BOOL {
        auto* list = reinterpret_cast<std::vector<HMONITOR>*>(lParam);
        AddUniqueMonitor(*list, monitor);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
}

// DWM cloaking happens to all windows on VD switch. 
// Windows cloaks/uncloaks the incoming/outgoing desktop one HWND at a time. This
// Snapshot all eligible windows by desktop GUID, but ignores only the transient cloak bit. 
// Genuine app/shell cloaks will be handled later on the now-current desktop.
static std::vector<HWND> GetTileWindowsAfterDesktopSwitch(
    HMONITOR monitor, const GUID& desktopId) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    GUID desktopId;
    std::vector<HWND>* windowList;
  } context = {monitor, desktopId, &windows};

  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        if (!CouldBeTileEligible(hwnd)) return TRUE;
        if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        RECT frameRect{};
        if (!GetWindowFrameRect(hwnd, &frameRect) ||
            frameRect.right <= frameRect.left || frameRect.bottom <= frameRect.top) {
          return TRUE;
        }

        if (MonitorFromRect(&frameRect, MONITOR_DEFAULTTONULL) == ctx->targetMonitor) {
          ctx->windowList->push_back(hwnd);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

// Native CurrentVirtualDesktopChanged is the authoritative workspace transition
// signal. Reconcile membership immediately by desktop GUID while deliberately
// ignoring DWM cloak state, then let the existing settle timer make one final
// cloak-sensitive pass after Windows finishes its per-HWND visibility burst.
static void HandleVirtualDesktopChanged() {
  InterlockedExchange(&g_virtualDesktopChangeQueued, 0);

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    Wh_Log(L"Native VD change received but current desktop query failed");
    ScheduleLifecycleReconcile(nullptr);
    return;
  }

  std::vector<HMONITOR> monitors;
  EnumerateCurrentMonitors(monitors);

  const bool discoverNew = g_enableTileNewWin && !g_tileNewWinSuspended;
  for (HMONITOR monitor : monitors) {
    std::vector<HWND> snapshot = GetTileWindowsAfterDesktopSwitch(monitor, currentDesktop);
    DesktopMonitorKey key{};
    if (ReconcileSavedWindowsWithNewDesktop(
            monitor, currentDesktop, snapshot, discoverNew, &key)) {
      // Do not place yet: the notification can precede the final UNCLOAK events.
      // Remember only workspaces whose model actually changed; an ordinary switch
      // between already-known desktops therefore performs zero placement work.
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  // Short safety pass that distinguishes genuine current-desktop cloaks from the transient
  // shell cloaking that accompanies the already-known switch.
  ScheduleLifecycleReconcile(nullptr);
  TrayUi::RefreshForCurrentWorkspace();
}

// For this desktop, schedule a window lifecycle (suspended? active? dead?) 
// reconciliation pass
static void ScheduleLifecycleReconcile(HWND hwnd) {
  // HotkeyThreadProc owns this vector/timer; no additional lock is required.
  if (hwnd && std::find(g_lifecycleDirtyWindows.begin(), g_lifecycleDirtyWindows.end(), hwnd) ==
                  g_lifecycleDirtyWindows.end()) {
    g_lifecycleDirtyWindows.push_back(hwnd);
  }

  if (g_lifecycleReconcileTimer) {
    KillTimer(nullptr, g_lifecycleReconcileTimer);
    g_lifecycleReconcileTimer = 0;
  }

  g_lifecycleReconcileTimer = SetTimer(nullptr, 0, kLifecycleReconcileDelayMs, nullptr);
  if (!g_lifecycleReconcileTimer) {
    Wh_Log(L"Failed to schedule lifecycle reconciliation");
  }
}

static void ReconcileDeferredLifecycle() {
  if (g_lifecycleReconcileTimer) {
    KillTimer(nullptr, g_lifecycleReconcileTimer);
    g_lifecycleReconcileTimer = 0;
  }

  std::vector<HWND> dirtyWindows;
  dirtyWindows.swap(g_lifecycleDirtyWindows);

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    // Leave the authoritative model unchanged if the private VD API is unstable.
    // A later lifecycle event/manual reconcile will repair it.
    Wh_Log(L"Lifecycle reconcile skipped: current virtual desktop unavailable");
    return;
  }

  std::vector<HMONITOR> monitors;

  // FancyWM refreshes the current desktop as a unit. Do the same for every
  // monitor for which this mod already has current-desktop state.
  AcquireSRWLockShared(&g_tilingStateLock);
  for (const auto& kv : g_tilingStateMap) {
    if (IsEqualGUID(kv.first.desktopId, currentDesktop)) {
      AddUniqueMonitor(monitors, kv.first.monitor);
    }
  }
  ReleaseSRWLockShared(&g_tilingStateLock);

  // Also include monitors containing newly visible/untracked windows so auto
  // management can seed an unseen desktop+monitor workspace after the burst.
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd)) continue;
    GUID windowDesktop{};
    if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
        !IsEqualGUID(windowDesktop, currentDesktop)) {
      continue;
    }
    AddUniqueMonitor(monitors, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL));
  }

  const bool discoverNew = g_enableTileNewWin && !g_tileNewWinSuspended;
  for (HMONITOR monitor : monitors) {
    DesktopMonitorKey key{};
    std::vector<HWND> snapshot = CollectTileWindows(monitor);
    bool changed = ReconcileSavedWindowsWithNewDesktop(
        monitor, currentDesktop, snapshot, discoverNew, &key);
    bool pendingArrange = std::any_of(
        g_pendingDesktopArranges.begin(), g_pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == monitor &&
                 IsEqualGUID(pending.desktopId, currentDesktop);
        });
    if (changed || pendingArrange) {
      ArrangeWorkspace(key);
    }
  }

  g_pendingDesktopArranges.erase(
      std::remove_if(
          g_pendingDesktopArranges.begin(), g_pendingDesktopArranges.end(),
          [&](const PendingWorkspaceArrange& pending) {
            return IsEqualGUID(pending.desktopId, currentDesktop);
          }),
      g_pendingDesktopArranges.end());

  TrayUi::RefreshForCurrentWorkspace();
}

static void ProcessWindowLifecycleEvent(DWORD event, HWND hwnd) {
  switch (event) {
    case EVENT_OBJECT_DESTROY:
      // Destruction is final and should not wait for the visibility settle timer.
      ForgetManagedWindow(hwnd);
      break;

    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
      // These remain generic visibility invalidations. Native VD notifications
      // identify workspace switches explicitly; this settle pass is still needed
      // for genuine hide/minimize/app-cloak transitions and as an API fallback.
      ScheduleLifecycleReconcile(hwnd);
      break;
  }
}

static void CycleCurrentWorkspaceLayout() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;
  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return;

  TilingState state;
  if (!LoadTilingState(key, &state)) {
    if (!EnsureWorkspaceFromSnapshot(monitor, false) || !LoadTilingState(key, &state)) return;
  }

  state.layout = static_cast<TileLayout>((static_cast<int>(state.layout) + 1) % static_cast<int>(TileLayout::COUNT));
  for (auto& kv : state.records) {
    WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot) {
      record.savedSlot.weight = 1.0;
      record.savedSlot.hasWeight = false;
      record.savedSlot.wasMaster = false;
    }
  }
  EnsureSaneWeightVector(state);
  SaveTilingState(key, state);
  ArrangeWorkspace(key);
  TrayUi::UpdateIcon(state.layout);
  TrayUi::ShowLayoutFlyout(state.layout);
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(state.layout));
}

void ApplyUserMoveSize(HWND hwnd) {
  if (!g_enableTiling || !IsWindow(hwnd)) return;

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  RECT monitorWork{};
  if (!monitor || !GetMonitorWorkArea(monitor, &monitorWork)) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  TilingState state;
  if (!LoadTilingState(key, &state)) {
    // Tiling not set up for current desktop
    return;
  }
  if (state.layout == TileLayout::Floating) return;

  RECT workArea = {monitorWork.left + g_tileMargin, monitorWork.top + g_tileMargin,
                   monitorWork.right - g_tileMargin, monitorWork.bottom - g_tileMargin};
  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) return;

  HWND resizedHwnd = hwnd;
  if (!ContainsWindow(state.windows, resizedHwnd)) {
    resizedHwnd = ResolveToTiledWindow(resizedHwnd, state.windows);
    if (!resizedHwnd) {
      Wh_Log(L"Unresolved window");
      return;
    }
  }

  RectChange change = RectChange::None;
  AcquireSRWLockExclusive(&g_moveSizeRectsLock);
  auto startIt = g_moveSizeStartRects.find(hwnd);
  auto endIt = g_moveSizeEndRects.find(hwnd);
  if (startIt != g_moveSizeStartRects.end() && endIt != g_moveSizeEndRects.end()) {
    change = ClassifyRectChange(startIt->second, endIt->second, 1);
  }
  g_moveSizeStartRects.erase(hwnd);
  g_moveSizeEndRects.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSizeRectsLock);

  if (change == RectChange::MoveOnly) {
    if (FloatWindowInState(state, resizedHwnd)) {
      WindowRecord* record = FindWindowRecord(state, resizedHwnd);
      if (record) record->placementFailures = 0;
      SaveTilingState(key, state);
      ArrangeWorkspace(key);
      Wh_Log(L"User move floated managed window %p", resizedHwnd);
    }
    return;
  }

  constexpr LONG kMinLearnedSpan = 80;
  // Alter weight for resized window (MasterStacks)
  if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    // MasterStackH splits top/bottom; MasterStack splits left/right.
    const bool horizontal = state.layout == TileLayout::MasterStackH;
    const LONG totalSize = horizontal ? workArea.bottom - workArea.top : workArea.right - workArea.left;
    if (totalSize <= g_tileGap + 1) return;

    // Index zero identifies the master; later indices identify stack windows.
    auto resizedIt = std::find(state.windows.begin(), state.windows.end(), resizedHwnd);
    const size_t resizedIndex = static_cast<size_t>(resizedIt - state.windows.begin());

    // Read the geometry produced by the user's resize operation.
    RECT resizedRect{};
    if (!GetWindowFrameRect(resizedHwnd, &resizedRect)) return;

    // Use the resized span along the master/stack split axis.
    LONG resizedSize = horizontal ? resizedRect.bottom - resizedRect.top : resizedRect.right - resizedRect.left;
    // Prevent the learned span from becoming impractically small.
    resizedSize = std::max(resizedSize, kMinLearnedSpan);

    // When the master was resized, its new span is directly observable.
    LONG masterSize = resizedSize;
    if (resizedIndex != 0) {
      // A stack window's leading edge reveals the master boundary.
      const LONG candidateSize = horizontal ? resizedRect.top - workArea.top - g_tileGap
                                            : resizedRect.left - workArea.left - g_tileGap;
      // The candidate must leave at least one pixel for the stack.
      const LONG maxMaster = totalSize - g_tileGap - 1;
      if (candidateSize >= 1 && candidateSize <= maxMaster) {
        masterSize = candidateSize;
      } else {
        // Fall back to the current master span when the boundary is unusable.
        RECT masterRect{};
        if (!GetWindowFrameRect(state.windows[0], &masterRect)) return;
        masterSize = horizontal ? masterRect.bottom - masterRect.top : masterRect.right - masterRect.left;
        // Apply the same minimum-span policy to the fallback measurement.
        masterSize = std::max(masterSize, kMinLearnedSpan);
      }
    }

    state.masterRatio = ClampDouble(
        static_cast<double>(masterSize) / static_cast<double>(totalSize - g_tileGap), 0.1, 0.9);

    state.stackWeights.clear();
    state.stackWeights.reserve(state.windows.size() - 1);
    for (size_t i = 1; i < state.windows.size(); ++i) {
      RECT rect{};
      if (!GetWindowFrameRect(state.windows[i], &rect)) {
        state.stackWeights.push_back(1.0);
        continue;
      }
      LONG size = horizontal ? rect.right - rect.left : rect.bottom - rect.top;
      state.stackWeights.push_back(static_cast<double>(std::max<LONG>(size, 1)));
    }
  } else if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    const bool horizontal = state.layout == TileLayout::Rows;

    state.gridWeights.clear();
    state.gridWeights.reserve(state.windows.size());
    for (HWND window : state.windows) {
      RECT rect{};
      if (!GetWindowFrameRect(window, &rect)) {
        state.gridWeights.push_back(1.0);
        continue;
      }
      LONG size = horizontal ? rect.bottom - rect.top : rect.right - rect.left;
      state.gridWeights.push_back(static_cast<double>(std::max<LONG>(size, 1)));
    }
  } else if (state.layout == TileLayout::COUNT) {
    return;
  }

  EnsureSaneWeightVector(state);
  SaveTilingState(key, state);
  ArrangeWorkspace(key);
}

void OnWindowMoveSizeEnd(HWND hwnd) {
  if (!g_enableTiling || !IsWindow(hwnd)) return;
  if (!g_threadId) return;
  if (!PostThreadMessage(g_threadId, WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(hwnd), 0)) {
    Wh_Log(L"Failed to queue resize event");
  }
}

static void QueueWindowEvent(DWORD event, HWND hwnd) {
  if (!g_threadId || !hwnd) return;
  if (!PostThreadMessage(g_threadId, WM_APP_WINDOW_EVENT,
                         static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd))) {
    Wh_Log(L"Failed to queue lifecycle event %lu", event);
  }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
    case EVENT_SYSTEM_MOVESIZESTART:
    case EVENT_SYSTEM_MOVESIZEEND:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
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
    if (g_threadId) PostThreadMessage(g_threadId, WM_APP_TRAY_REFRESH, 0, 0);
    return;
  }

  // For window-level events only.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  const bool tracked = IsWindowTrackedInAnyState(hwnd);
  if (tracked && IsWindow(hwnd)) {
    HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    if (rootOwner && rootOwner != hwnd && IsWindowTrackedInAnyState(rootOwner)) hwnd = rootOwner;
  }

  // Cache user move/resize boundaries. The callback only records facts; model
  // mutation and layout work happen later on the WM thread.
  if (event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND) {
    if (!tracked) return;

    AcquireSRWLockExclusive(&g_moveSizeRectsLock);
    RECT r{};
    if (GetWindowFrameRect(hwnd, &r)) {
      if (event == EVENT_SYSTEM_MOVESIZESTART) {
        g_moveSizeStartRects[hwnd] = r;
      } else {
        g_moveSizeEndRects[hwnd] = r;
      }
    }
    ReleaseSRWLockExclusive(&g_moveSizeRectsLock);

    if (event == EVENT_SYSTEM_MOVESIZEEND) OnWindowMoveSizeEnd(hwnd);
    return;
  }

  // Lifecycle events are never dropped because another WM operation is active.
  // PostThreadMessage gives us a serialized event queue owned by HotkeyThreadProc.
  QueueWindowEvent(event, hwnd);
}

void InstallWinEventHooks() {
  if (!g_hForegroundHook) {
    g_hForegroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hForegroundHook) Wh_Log(L"Failed to install foreground hook");
  }

  if (!g_hMoveSizeHook) {
    g_hMoveSizeHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hMoveSizeHook) Wh_Log(L"Failed to install move/size hook");
  }

  if (!g_hMinimizeHook) {
    g_hMinimizeHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hMinimizeHook) Wh_Log(L"Failed to install minimization hook");
  }

  if (!g_hHideDestroyHook) {
    g_hHideDestroyHook = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hHideDestroyHook) Wh_Log(L"Failed to install hide/destroy hook");
  }

  if (!g_hCloakHook) {
    g_hCloakHook = SetWinEventHook(
      EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hCloakHook) Wh_Log(L"Failed to install uncloak hook");
  }
}

void RemoveWinEventHooks() {
    if (g_hForegroundHook) {
        UnhookWinEvent(g_hForegroundHook);
        g_hForegroundHook = nullptr;
    }
    if (g_hMoveSizeHook) {
        UnhookWinEvent(g_hMoveSizeHook);
        g_hMoveSizeHook = nullptr;
    }
    if (g_hMinimizeHook) {
        UnhookWinEvent(g_hMinimizeHook);
        g_hMinimizeHook = nullptr;
    }
    if (g_hHideDestroyHook) {
        UnhookWinEvent(g_hHideDestroyHook);
        g_hHideDestroyHook = nullptr;
    }
    if (g_hCloakHook) {
        UnhookWinEvent(g_hCloakHook);
        g_hCloakHook = nullptr;
    }
}

void ResetTilingStateMemory() {
  AcquireSRWLockExclusive(&g_tilingStateLock);
  g_tilingStateMap.clear();
  ReleaseSRWLockExclusive(&g_tilingStateLock);
}

//=============================================================================
// Hotkey Thread
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
  g_windowsVersionIndex = ParseWindowsVersion(version);
  Wh_FreeStringSetting(version);

  g_tilingModifiers = ReadModifierSetting(L"TilingModifier", MOD_ALT);

  g_tileMargin = Wh_GetIntSetting(L"TileMargin");
  if (g_tileMargin < 0 || g_tileMargin > 100) g_tileMargin = 4;
  g_tileGap = Wh_GetIntSetting(L"TileGap");
  if (g_tileGap < 0 || g_tileGap > 100) g_tileGap = 4;
  g_masterPercent = Wh_GetIntSetting(L"MasterPercent");
  if (g_masterPercent < 1 || g_masterPercent > 99) g_masterPercent = 50;

  PCWSTR layout = Wh_GetStringSetting(L"DefaultLayout");
  g_defaultLayout = ParseLayoutSetting(layout);
  Wh_FreeStringSetting(layout);

  g_enableTiling = Wh_GetIntSetting(L"EnableTiling") != 0;
  g_captureLayoutOnTile = Wh_GetIntSetting(L"CaptureLayoutOnTile") != 0;
  g_enableLayoutCycle = Wh_GetIntSetting(L"EnableLayoutCycle") != 0;
  // New! 
  g_enableTileNewWin = Wh_GetIntSetting(L"EnableTileNewWin") != 0;
  g_tileNewWinSuspended = false;


  g_tileKey = ReadStringSetting(L"TileKey", ParseSingleCharKey, (UINT)'D');
  g_layoutKey = ReadStringSetting(L"LayoutKey", ParseSingleCharKey, (UINT)'L');
  g_swapMasterKey = ReadStringSetting(L"SwapMasterKey", ParseSingleCharKey, (UINT)'M');
  g_tileNewWinToggleKey = ReadStringSetting(L"TileNewWinToggleKey", ParseSingleCharKey, (UINT)'U');
}

DWORD WINAPI HotkeyThreadProc(LPVOID) {
  g_threadId = GetCurrentThreadId();
  Wh_Log(L"Hotkey thread started, thread ID: %lu", g_threadId);

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Wh_Log(L"CoInitializeEx result: 0x%08X", coHr);

  MSG msg;
  PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

  if (g_enableTiling && SUCCEEDED(coHr) && InitializeVirtualDesktopAPI()) {
    if (!RegisterVirtualDesktopNotifications()) {
      Wh_Log(L"Using settled WinEvent reconciliation for virtual desktop changes");
    }
  }

  SetEvent(g_hReadyEvent);

  if (g_enableTiling) {
    InstallWinEventHooks();
  }

  if (g_enableTiling) {
    RegisterHotKey(nullptr, HK_TILE, g_tilingModifiers, g_tileKey);
    RegisterHotKey(nullptr, HK_SWAP_MASTER, g_tilingModifiers, g_swapMasterKey);
    RegisterHotKey(nullptr, HK_TILENEWWIN_TOGGLE, g_tilingModifiers, g_tileNewWinToggleKey);
    if (g_enableLayoutCycle) {
      RegisterHotKey(nullptr, HK_LAYOUT, g_tilingModifiers, g_layoutKey);
    }
  }
  Wh_Log(L"Hotkeys registered");


  if (g_enableTiling) {
      HMONITOR monitor = GetTargetMonitor();
      if (monitor) {
        DesktopMonitorKey key{};
        TilingState existingState;
        if (GetCurrentWorkspaceKey(monitor, &key) && LoadTilingState(key, &existingState)) {
          ArrangeWorkspace(key);
        } else if (g_enableTileNewWin) {
          EnsureWorkspaceFromSnapshot(monitor, false);
        }
      }
      if (g_enableTileNewWin) Wh_Log(L"TileNewWin enabled: reconciled current workspace on startup");
  }

  if (g_enableTiling) {
    if (!TrayUi::Initialize()) {
      Wh_Log(L"Tiling layout tray icon unavailable; window management remains active");
    }
  }

  while (!g_stopHotkeyThread) {
    DWORD waitResult = MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);

    if (waitResult == WAIT_OBJECT_0) {
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {

        switch (msg.message) {  
          case WM_QUIT: 
            goto cleanup;
          
          case WM_APP_MOVE_SIZE_END: {
            HWND resizedHwnd = reinterpret_cast<HWND>(msg.wParam);
            ApplyUserMoveSize(resizedHwnd);
            continue;
          }

          case WM_APP_WINDOW_EVENT: {
            DWORD event = static_cast<DWORD>(msg.wParam);
            HWND hwnd = reinterpret_cast<HWND>(msg.lParam);
            ProcessWindowLifecycleEvent(event, hwnd);
            continue;
          }

          case WM_APP_VIRTUAL_DESKTOP_CHANGED: {
            HandleVirtualDesktopChanged();
            continue;
          }

          case WM_APP_TRAY_REFRESH: {
            TrayUi::RefreshForCurrentWorkspace();
            continue;
          }

          case WM_TIMER: {
            if (g_lifecycleReconcileTimer &&
                static_cast<UINT_PTR>(msg.wParam) == g_lifecycleReconcileTimer) {
              ReconcileDeferredLifecycle();
              continue;
            }
            break;
          }

          case WM_HOTKEY: {
            UINT hotkeyId = static_cast<UINT>(msg.wParam);
            switch (hotkeyId) {
              case HK_TILE: {
                  TileWindows();
                  continue;
              }

              case HK_LAYOUT: {
                  CycleCurrentWorkspaceLayout();
                  continue;
              }

              case HK_SWAP_MASTER: {
                SwapMaster();
                continue;
              }

              case HK_TILENEWWIN_TOGGLE: {
                if (g_enableTileNewWin) {
                  g_tileNewWinSuspended = !g_tileNewWinSuspended;
                  Wh_Log(L"TileNewWin %s", g_tileNewWinSuspended ? L"suspended" : L"resumed");
                  
                  // Reconcile anything that appeared while auto-management was paused.
                  if (!g_tileNewWinSuspended) {
                    HMONITOR monitor = GetTargetMonitor();
                    if (monitor) EnsureWorkspaceFromSnapshot(monitor, false);
                  }
                  continue;
                };
                Wh_Log(L"TileNewWin is disabled from settings. ");
                continue;
              }

            }
            continue;
          }
        }

        // This is an STA COM apartment. Messages for COM's hidden
        // OleMainThreadWndClass must be dispatched, not merely removed from
        // the queue, or cross-apartment callbacks (including virtual desktop
        // notifications) can be silently lost. Our own thread messages use
        // `continue` above and never reach this point.
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }
  }

cleanup:
  if (g_lifecycleReconcileTimer) {
    KillTimer(nullptr, g_lifecycleReconcileTimer);
    g_lifecycleReconcileTimer = 0;
  }
  g_lifecycleDirtyWindows.clear();
  g_pendingDesktopArranges.clear();
  InterlockedExchange(&g_virtualDesktopChangeQueued, 0);

  if (g_enableTiling) {
    UnregisterHotKey(nullptr, HK_TILE);
    UnregisterHotKey(nullptr, HK_SWAP_MASTER);
    UnregisterHotKey(nullptr, HK_TILENEWWIN_TOGGLE);
    if (g_enableLayoutCycle) {
      UnregisterHotKey(nullptr, HK_LAYOUT);
    }
  }

  TrayUi::Shutdown();
  RemoveWinEventHooks();

  CleanupVirtualDesktopAPI();
  CoUninitialize();
  return 0;
}

bool StartHotkeyThread() {
  g_hReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  g_hThread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);

  if (!g_hThread || WaitForSingleObject(g_hReadyEvent, 5000) != WAIT_OBJECT_0) {
    Wh_Log(L"Failed to start hotkey thread");
    if (g_hThread) {
      CloseHandle(g_hThread);
      g_hThread = nullptr;
    }
    CloseHandle(g_hReadyEvent);
    g_hReadyEvent = nullptr;
    return false;
  }

  CloseHandle(g_hReadyEvent);
  g_hReadyEvent = nullptr;
  return true;
}

void StopHotkeyThread() {
  g_stopHotkeyThread = true;

  g_bInitialized = false;

  if (g_threadId) {
    PostThreadMessage(g_threadId, WM_QUIT, 0, 0);
  }

  if (g_hThread) {
    DWORD waitResult = WaitForSingleObject(g_hThread, 5000);
    if (waitResult == WAIT_TIMEOUT) {
      Wh_Log(L"WARNING: Hotkey thread cleanup timeout, thread may be stuck in CoUninitialize");
    }
    CloseHandle(g_hThread);
    g_hThread = nullptr;
  }

  g_threadId = 0;
  g_stopHotkeyThread = false;
}

//=============================================================================
// Windhawk Tool Mod Entry Points
//=============================================================================

BOOL WhTool_ModInit() {
  Wh_Log(L"Tiling Helper mod initializing...");
  LoadSettings();
  if (!StartHotkeyThread()) {
    Wh_Log(L"Failed to start hotkey thread");
    return FALSE;
  }
  Wh_Log(L"Tiling Helper mod initialized successfully");
  return TRUE;
}

void WhTool_ModUninit() {
  Wh_Log(L"Tiling Helper mod uninitializing...");
  StopHotkeyThread();
  Wh_Log(L"Tiling Helper mod uninitialized");
}

void WhTool_ModSettingsChanged() {
  Wh_Log(L"Settings changed, reloading...");
  StopHotkeyThread();
  // Settings reload changes defaults/policies but does not destroy per-desktop
  // workspace identity, layout mode, ordering, or learned ratios.
  LoadSettings();
  if (!StartHotkeyThread()) {
    Wh_Log(L"Failed to restart hotkey thread after settings change");
  }
}

//=============================================================================
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk-mods/pull/1916
//
// The mod will load and run in a dedicated windhawk.exe process.
//=============================================================================

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
  Wh_Log(L">");
  ExitThread(0);
}

BOOL Wh_ModInit() {
  DWORD sessionId;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
      sessionId == 0) {
    return FALSE;
  }

  bool isExcluded = false;
  bool isToolModProcess = false;
  bool isCurrentToolModProcess = false;
  int argc;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
  if (!argv) {
    Wh_Log(L"CommandLineToArgvW failed");
    return FALSE;
  }

  for (int i = 1; i < argc; i++) {
    if (wcscmp(argv[i], L"-service") == 0 ||
        wcscmp(argv[i], L"-service-start") == 0 ||
        wcscmp(argv[i], L"-service-stop") == 0) {
      isExcluded = true;
      break;
    }
  }

  for (int i = 1; i < argc - 1; i++) {
    if (wcscmp(argv[i], L"-tool-mod") == 0) {
      isToolModProcess = true;
      if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
        isCurrentToolModProcess = true;
      }
      break;
    }
  }

  LocalFree(argv);

  if (isExcluded) {
    return FALSE;
  }

  if (isCurrentToolModProcess) {
    g_toolModProcessMutex =
        CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
    if (!g_toolModProcessMutex) {
      Wh_Log(L"CreateMutex failed");
      ExitProcess(1);
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
      ExitProcess(1);
    }

    if (!WhTool_ModInit()) {
      ExitProcess(1);
    }

    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
    IMAGE_NT_HEADERS* ntHeaders =
        (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

    DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

    Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
    return TRUE;
  }

  if (isToolModProcess) {
    return FALSE;
  }

  g_isToolModProcessLauncher = true;
  return TRUE;
}

void Wh_ModAfterInit() {
  if (!g_isToolModProcessLauncher) {
    return;
  }

  WCHAR currentProcessPath[MAX_PATH];
  switch (GetModuleFileName(nullptr, currentProcessPath, ARRAYSIZE(currentProcessPath))) {
    case 0:
    case ARRAYSIZE(currentProcessPath):
      Wh_Log(L"GetModuleFileName failed");
      return;
  }

  WCHAR commandLine[MAX_PATH + 2 + (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
  swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath, WH_MOD_ID);

  HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
  if (!kernelModule) {
    kernelModule = GetModuleHandle(L"kernel32.dll");
    if (!kernelModule) {
      Wh_Log(L"No kernelbase.dll/kernel32.dll");
      return;
    }
  }

  using CreateProcessInternalW_t = BOOL(WINAPI*)(
      HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
      LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
      LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation,
      PHANDLE hRestrictedUserToken);
  CreateProcessInternalW_t pCreateProcessInternalW =
      (CreateProcessInternalW_t)GetProcAddress(kernelModule, "CreateProcessInternalW");
  if (!pCreateProcessInternalW) {
    Wh_Log(L"No CreateProcessInternalW");
    return;
  }

  STARTUPINFO si{
      .cb = sizeof(STARTUPINFO),
      .dwFlags = STARTF_FORCEOFFFEEDBACK,
  };
  PROCESS_INFORMATION pi;
  if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine, nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                               nullptr, nullptr, &si, &pi, nullptr)) {
    Wh_Log(L"CreateProcess failed");
    return;
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
  if (g_isToolModProcessLauncher) {
    return;
  }

  WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
  if (g_isToolModProcessLauncher) {
    return;
  }

  WhTool_ModUninit();
  ExitProcess(0);
}

