// ==WindhawkMod==
// @id              tiling-helper-experimental
// @name            Tiling Helper Experimental
// @description     Tile windows on the current monitor with customizable layouts and hotkeys
// @version         1.6.0
// @author          u2x1, meteoni
// @github          https://github.com/u2x1
// @include         windhawk.exe
// @compilerOptions -lole32 -loleaut32 -luuid -ldwmapi
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
- Optional capture of current window sizes when tiling (useful if auto-retile is off)

## Default Hotkeys
- Tile windows: Alt + D
- Cycle layout: Alt + L
- Toggle retile-on-resize: Alt + R

## Notes
- Layout mode, ordering, and resize state are stored per virtual desktop + monitor.
- Floating is a first-class workspace mode: windows remain managed but are not repositioned.
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

- EnableResizeRetile: true
  $name: '[Tiling] Retile On Resize'
  $description: Automatically retile when a tiled window is resized (only applies when tiling is enabled)

- RetileToggleKey: "R"
  $name: '[Tiling] Retile Toggle Key'
  $description: 'Key to pause/resume retile-on-resize. Workspace memory is preserved.'

- SwapMasterKey: "M"
  $name: '[Tiling] Switch Master Window Key'
  $description: 'Key to set another master window. Only active when Retile On Resize is enabled.'

- CaptureLayoutOnTile: true
  $name: '[Tiling] Capture Layout On Tile'
  $description: Use current window sizes as layout weights when tiling (useful when auto-retile is disabled)

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
#include <windhawk_utils.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
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

//=============================================================================
// Global State
//=============================================================================
static IServiceProvider* g_pServiceProvider = nullptr;
static IVirtualDesktopManagerInternal* g_pDesktopManagerInternal = nullptr;
static IVirtualDesktopManager* g_pDesktopManager = nullptr;
static bool g_bInitialized = false;

static HANDLE g_hThread = nullptr;
static DWORD g_threadId = 0;
static HANDLE g_hReadyEvent = nullptr;
static volatile bool g_stopHotkeyThread = false;

// Hotkey IDs
enum HotkeyIds {
  HK_TILE = 1,
  HK_LAYOUT = 2,
  HK_RETILE_TOGGLE = 3,
  HK_SWAP_MASTER = 4, 
  HK_TILENEWWIN_TOGGLE = 5,
};

// Messages to hotkey thread
constexpr UINT WM_APP_RETILE_ON_RESIZE = WM_APP + 1;
constexpr UINT WM_APP_WINDOW_EVENT = WM_APP + 2;

static UINT g_tilingModifiers = MOD_ALT;
static UINT g_tileKey = 'D';
static UINT g_layoutKey = 'L';
static UINT g_retileToggleKey = 'R';
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
static bool g_enableResizeRetile = true;
static bool g_captureLayoutOnTile = true;
static bool g_enableLayoutCycle = true;
static bool g_retileSuspended = false;
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

struct TilingState {
  TileLayout layout = TileLayout::MasterStack;
  // Active, tile-participating windows in logical layout order.
  std::vector<HWND> windows;
  // Windows temporarily absent from placement (minimized/hidden during a VD
  // switch, etc.) keep their logical slot so restore doesn't reshuffle layout.
  std::unordered_map<HWND, SuspendedSlot> suspendedSlots;
  double masterRatio = 0.5;
  std::vector<double> stackWeights;
  std::vector<double> gridWeights;
};

static std::unordered_map<DesktopMonitorKey, TilingState, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>
    g_tilingStateMap;
static SRWLOCK g_tilingStateLock = SRWLOCK_INIT;
// Rect cache thread safety lock
static SRWLOCK g_moveSizeRectsLock = SRWLOCK_INIT;
static HWINEVENTHOOK g_hMoveSizeHook = nullptr;
static HWINEVENTHOOK g_hMinimizeHook = nullptr;
static HWINEVENTHOOK g_hHideDestroyHook = nullptr;
static HWINEVENTHOOK g_hCloakHook = nullptr;

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

bool ContainsWindow(const std::vector<HWND>& windows, HWND hwnd) {
  for (HWND w : windows) {
    if (w == hwnd) return true;
  }
  return false;
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
// Virtual Desktop API (minimal for per-desktop memory)
//=============================================================================

static const int VTABLE_GET_CURRENT_DESKTOP = 6;

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
  if (!hShell || !IsWindow(hShell)) {
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
  return InitializeVirtualDesktopAPI();
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
    // Fast membership test (linear, but st.windows is small)
    if (ContainsWindow(st.windows, hwnd) || st.suspendedSlots.find(hwnd) != st.suspendedSlots.end()) {
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

void PlaceWindow(HWND hwnd, const RECT& targetRect) {
  if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);

  RECT windowRect, extendedFrame;
  LONG offsetLeft = 0, offsetTop = 0, offsetRight = 0, offsetBottom = 0;

  if (GetWindowRect(hwnd, &windowRect) &&
      SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extendedFrame, sizeof(extendedFrame)))) {
    offsetLeft = extendedFrame.left - windowRect.left;
    offsetTop = extendedFrame.top - windowRect.top;
    offsetRight = windowRect.right - extendedFrame.right;
    offsetBottom = windowRect.bottom - extendedFrame.bottom;
  }

  SetWindowPos(hwnd, nullptr, targetRect.left - offsetLeft, targetRect.top - offsetTop,
               targetRect.right - targetRect.left + offsetLeft + offsetRight,
               targetRect.bottom - targetRect.top + offsetTop + offsetBottom,
               SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING); 
}

void SwapMaster() {
  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
  if (!mon) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{desktopId, mon};

  bool didSwap  = false;
  HWND oldMaster = nullptr;
  HWND resolved  = nullptr;
  RECT rMaster{}, rResolved{};
  bool lockHeld {false};

  // ---- critical section (state mutation) ----
  AcquireSRWLockExclusive(&g_tilingStateLock);
  lockHeld = true;

  {
    auto it = g_tilingStateMap.find(key);
    if (it == g_tilingStateMap.end() || it->second.windows.size() < 2) goto swap_cleanup;

    TilingState& st = it->second;
    if (st.layout != TileLayout::MasterStack && st.layout != TileLayout::MasterStackH) goto swap_cleanup;

    resolved = ResolveToTiledWindow(fg, st.windows);
    if (!resolved) goto swap_cleanup;

    size_t idx = (size_t)-1;
    for (size_t i = 0; i < st.windows.size(); ++i) {
      if (st.windows[i] == resolved) { idx = i; break; }
    }
    if (idx == (size_t)-1 || idx == 0) goto swap_cleanup;

    oldMaster = st.windows[0];

    // Capture current slot rects (frame bounds)
    if (!GetWindowFrameRect(oldMaster, &rMaster)) goto swap_cleanup;
    if (!GetWindowFrameRect(resolved,  &rResolved)) goto swap_cleanup;

    // Persist slot swap
    std::swap(st.windows[0], st.windows[idx]);
    didSwap = true;
  }

swap_cleanup:
  if (lockHeld) {
    ReleaseSRWLockExclusive(&g_tilingStateLock);
    lockHeld = false;
  }

  // Apply swap visually. WinEvents are serialized onto the WM thread, so no
  // global in-progress gate is needed.
  if (didSwap) {
    PlaceWindow(oldMaster, rResolved);
    PlaceWindow(resolved,  rMaster);

    // purge potential stale window rects after swapping  
    AcquireSRWLockExclusive(&g_moveSizeRectsLock);
    g_moveSizeStartRects.erase(oldMaster);
    g_moveSizeEndRects.erase(oldMaster);
    g_moveSizeStartRects.erase(resolved);
    g_moveSizeEndRects.erase(resolved);
    ReleaseSRWLockExclusive(&g_moveSizeRectsLock);

  }

  return;
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
    desktopId = GUID{};
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

static void NormalizeWeights(TilingState& state) {
  if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    size_t stackCount = state.windows.empty() ? 0 : state.windows.size() - 1;
    if (state.stackWeights.size() != stackCount) state.stackWeights = DefaultWeights(stackCount);
    state.gridWeights.clear();
  } else if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    if (state.gridWeights.size() != state.windows.size()) state.gridWeights = DefaultWeights(state.windows.size());
    state.stackWeights.clear();
  } else {
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
  NormalizeWeights(state);
}

static size_t ActiveIndexToLogicalIndex(const TilingState& state, size_t activeIndex) {
  std::vector<size_t> suspendedIndices;
  suspendedIndices.reserve(state.suspendedSlots.size());
  for (const auto& kv : state.suspendedSlots) suspendedIndices.push_back(kv.second.index);
  std::sort(suspendedIndices.begin(), suspendedIndices.end());

  size_t logicalIndex = activeIndex;
  for (size_t suspendedIndex : suspendedIndices) {
    if (suspendedIndex <= logicalIndex) ++logicalIndex;
    else break;
  }
  return logicalIndex;
}

static size_t LogicalIndexToActiveIndex(const TilingState& state, size_t logicalIndex, HWND restoringHwnd) {
  size_t suspendedBefore = 0;
  for (const auto& kv : state.suspendedSlots) {
    if (kv.first != restoringHwnd && kv.second.index < logicalIndex) ++suspendedBefore;
  }
  return logicalIndex >= suspendedBefore ? logicalIndex - suspendedBefore : 0;
}

static void CloseLogicalGapAfterRemoval(TilingState& state, size_t removedLogicalIndex, HWND exceptHwnd = nullptr) {
  for (auto& kv : state.suspendedSlots) {
    if (kv.first != exceptHwnd && kv.second.index > removedLogicalIndex) --kv.second.index;
  }
}

static bool SuspendWindowInState(TilingState& state, HWND hwnd) {
  auto it = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (it == state.windows.end()) return false;
  SuspendedSlot slot{};
  size_t activeIndex = static_cast<size_t>(it - state.windows.begin());
  size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
  RemoveActiveWindowAt(state, activeIndex, &slot);
  slot.index = logicalIndex;
  state.suspendedSlots[hwnd] = slot;
  return true;
}

static bool RestoreWindowInState(TilingState& state, HWND hwnd) {
  auto it = state.suspendedSlots.find(hwnd);
  if (it == state.suspendedSlots.end()) return false;

  SuspendedSlot slot = it->second;
  size_t index = std::min(LogicalIndexToActiveIndex(state, slot.index, hwnd), state.windows.size());
  state.suspendedSlots.erase(it);
  state.windows.insert(state.windows.begin() + index, hwnd);

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    if (state.gridWeights.size() + 1 == state.windows.size()) {
      state.gridWeights.insert(state.gridWeights.begin() + std::min(index, state.gridWeights.size()),
                               slot.hasWeight ? slot.weight : 1.0);
    }
  } else if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    if (slot.wasMaster) {
      state.stackWeights.clear();
    } else if (index > 0 && state.stackWeights.size() + 1 == state.windows.size() - 1) {
      size_t stackIndex = std::min(index - 1, state.stackWeights.size());
      state.stackWeights.insert(state.stackWeights.begin() + stackIndex, slot.hasWeight ? slot.weight : 1.0);
    }
  }

  NormalizeWeights(state);
  return true;
}

static bool ForgetWindowInState(TilingState& state, HWND hwnd) {
  auto activeIt = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (activeIt != state.windows.end()) {
    size_t activeIndex = static_cast<size_t>(activeIt - state.windows.begin());
    size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
    RemoveActiveWindowAt(state, activeIndex, nullptr);
    CloseLogicalGapAfterRemoval(state, logicalIndex);
    return true;
  }

  auto suspendedIt = state.suspendedSlots.find(hwnd);
  if (suspendedIt != state.suspendedSlots.end()) {
    size_t logicalIndex = suspendedIt->second.index;
    state.suspendedSlots.erase(suspendedIt);
    CloseLogicalGapAfterRemoval(state, logicalIndex);
    return true;
  }
  return false;
}

static void ArrangeWorkspace(const DesktopMonitorKey& key) {
  // Never drive windows on an inactive virtual desktop. Lifecycle events from
  // the desktop being hidden can arrive during a switch; its model should be
  // updated now and rendered when that desktop becomes current again.
  GUID zeroGuid{};
  if (!IsEqualGUID(key.desktopId, zeroGuid)) {
    GUID currentDesktop{};
    if (GetCurrentDesktopId(&currentDesktop) && !IsEqualGUID(currentDesktop, key.desktopId)) return;
  }

  TilingState state;
  if (!LoadTilingState(key, &state)) return;

  RECT monitorWork{};
  if (!GetMonitorWorkArea(key.monitor, &monitorWork)) return;
  RECT workArea = {monitorWork.left + g_tileMargin, monitorWork.top + g_tileMargin,
                   monitorWork.right - g_tileMargin, monitorWork.bottom - g_tileMargin};
  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) return;

  // The model is authoritative, but dead HWNDs can still exist if Windows drops a
  // destroy notification. Repair those opportunistically without rebuilding order.
  for (size_t i = state.windows.size(); i-- > 0;) {
    if (!IsWindow(state.windows[i])) RemoveActiveWindowAt(state, i, nullptr);
  }
  for (auto it = state.suspendedSlots.begin(); it != state.suspendedSlots.end();) {
    if (!IsWindow(it->first)) it = state.suspendedSlots.erase(it);
    else ++it;
  }

  NormalizeWeights(state);
  SaveTilingState(key, state);

  // Floating is intentionally still a managed workspace. Lifecycle/order stays
  // remembered, but the WM does not impose geometry.
  if (state.layout == TileLayout::Floating || state.windows.empty()) return;

  std::vector<RECT> windowRects(state.windows.size());
  switch (state.layout) {
    case TileLayout::MasterStack:
      LayoutMasterStackWeighted(workArea, state.windows.size(), windowRects, false,
                                ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      break;
    case TileLayout::MasterStackH:
      LayoutMasterStackWeighted(workArea, state.windows.size(), windowRects, true,
                                ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      break;
    case TileLayout::Columns:
      LayoutGridWeighted(workArea, state.windows.size(), windowRects, false, state.gridWeights);
      break;
    case TileLayout::Rows:
      LayoutGridWeighted(workArea, state.windows.size(), windowRects, true, state.gridWeights);
      break;
    case TileLayout::BSP:
      LayoutBSP(workArea, 0, state.windows.size(), 0, windowRects);
      break;
    case TileLayout::Monocle:
      windowRects.assign(state.windows.size(), workArea);
      break;
    case TileLayout::Floating:
    case TileLayout::COUNT:
      return;
  }

  for (size_t i = 0; i < state.windows.size(); ++i) {
    if (IsWindow(state.windows[i]) && !IsIconic(state.windows[i]) && IsWindowVisible(state.windows[i])) {
      PlaceWindow(state.windows[i], windowRects[i]);
    }
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

  // Restore known temporarily absent windows which are visible again.
  for (HWND w : snapshot) {
    if (state.suspendedSlots.find(w) != state.suspendedSlots.end()) RestoreWindowInState(state, w);
  }

  // Windows no longer in the visible snapshot are suspended rather than forgotten.
  // SuspendWindowInState uses stable logical indices, so several minimized
  // windows can restore in any order without changing the workspace order.
  std::vector<HWND> toSuspend;
  for (HWND w : state.windows) {
    if (!ContainsWindow(snapshot, w)) toSuspend.push_back(w);
  }
  for (HWND w : toSuspend) SuspendWindowInState(state, w);

  // Preserve the logical order of known windows; append newly discovered ones.
  for (HWND w : snapshot) {
    if (!ContainsWindow(state.windows, w) && state.suspendedSlots.find(w) == state.suspendedSlots.end()) {
      state.windows.push_back(w);
    }
  }

  // Geometry capture is now an explicit/manual operation only. Automatic
  // lifecycle handling never rebuilds the model from transient rectangles.
  if (adoptCurrentGeometry && g_captureLayoutOnTile && state.layout != TileLayout::Floating &&
      state.suspendedSlots.empty() && SnapshotLooksTiled(snapshot, workArea)) {
    HWND preferredMaster = state.windows.empty() ? nullptr : state.windows.front();
    TilingState captured = BuildStateFromWindows(state.layout, workArea, snapshot, monitor, preferredMaster);
    captured.suspendedSlots = state.suspendedSlots;
    state = std::move(captured);
  } else {
    NormalizeWeights(state);
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
  Wh_Log(L"Manual tile/reconcile completed");
}

static bool GetWorkspaceKeyForWindow(HWND hwnd, DesktopMonitorKey* outKey) {
  if (!hwnd || !outKey || !IsWindow(hwnd)) return false;
  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(hwnd, &desktopId)) {
    if (!GetCurrentDesktopId(&desktopId)) desktopId = GUID{};
  }
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  if (!monitor) return false;
  outKey->desktopId = desktopId;
  outKey->monitor = monitor;
  return true;
}

static bool FindStateContainingWindow(HWND hwnd, DesktopMonitorKey* outKey, TilingState* outState) {
  bool found = false;
  AcquireSRWLockShared(&g_tilingStateLock);
  for (const auto& kv : g_tilingStateMap) {
    if (ContainsWindow(kv.second.windows, hwnd) || kv.second.suspendedSlots.find(hwnd) != kv.second.suspendedSlots.end()) {
      if (outKey) *outKey = kv.first;
      if (outState) *outState = kv.second;
      found = true;
      break;
    }
  }
  ReleaseSRWLockShared(&g_tilingStateLock);
  return found;
}

static void SuspendManagedWindow(HWND hwnd) {
  DesktopMonitorKey key{};
  TilingState state;
  if (!FindStateContainingWindow(hwnd, &key, &state)) return;
  if (SuspendWindowInState(state, hwnd)) {
    SaveTilingState(key, state);
    ArrangeWorkspace(key);
  }
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

static void RestoreOrAttachManagedWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;

  DesktopMonitorKey existingKey{};
  TilingState state;
  if (FindStateContainingWindow(hwnd, &existingKey, &state)) {
    if (RestoreWindowInState(state, hwnd)) {
      SaveTilingState(existingKey, state);
    }
    // SHOW/UNCLOAK is also our inexpensive desktop-return reconciliation path.
    ArrangeWorkspace(existingKey);
    return;
  }

  if (!g_enableTileNewWin || g_tileNewWinSuspended || !CouldBeTileEligible(hwnd)) return;

  DesktopMonitorKey key{};
  if (!GetWorkspaceKeyForWindow(hwnd, &key)) return;
  if (!IsTileEligible(hwnd, key.monitor)) return;

  if (!LoadTilingState(key, &state)) {
    // First event on a previously unseen desktop/monitor: seed the authoritative
    // model from the current eligible set, but do NOT infer layout from geometry.
    EnsureWorkspaceFromSnapshot(key.monitor, false);
    return;
  }

  if (!ContainsWindow(state.windows, hwnd)) {
    state.windows.push_back(hwnd);
    NormalizeWeights(state);
    SaveTilingState(key, state);
    ArrangeWorkspace(key);
  }
}

static void ProcessWindowLifecycleEvent(DWORD event, HWND hwnd) {
  switch (event) {
    case EVENT_OBJECT_DESTROY:
      ForgetManagedWindow(hwnd);
      break;
    case EVENT_OBJECT_HIDE:
    case EVENT_SYSTEM_MINIMIZESTART:
      SuspendManagedWindow(hwnd);
      break;
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_UNCLOAKED:
    case EVENT_SYSTEM_MINIMIZEEND:
      RestoreOrAttachManagedWindow(hwnd);
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
  for (auto& kv : state.suspendedSlots) {
    kv.second.weight = 1.0;
    kv.second.hasWeight = false;
    kv.second.wasMaster = false;
  }
  NormalizeWeights(state);
  SaveTilingState(key, state);
  ArrangeWorkspace(key);
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(state.layout));
}

// Handle <=1 active windows without destroying workspace memory. This preserves
// per-desktop layout mode and suspended slots even when the desktop is empty.
static bool HandleTrivialState(const DesktopMonitorKey& key, TilingState& state, const RECT& workArea) {
  NormalizeWeights(state);
  SaveTilingState(key, state);
  if (state.windows.empty()) return true;
  if (state.windows.size() == 1) {
    if (state.layout != TileLayout::Floating) PlaceWindow(state.windows[0], workArea);
    return true;
  }
  return false;
}

void RetileFromResize(HWND hwnd) {
  if (!g_enableTiling || !IsWindow(hwnd)) {
    return;
  }


  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  RECT monitorWork = {};
  if (!monitor || !GetMonitorWorkArea(monitor, &monitorWork)) {
    return;
  }

  GUID desktopId = {};
  bool hasDesktopId = GetWindowDesktopIdSafe(hwnd, &desktopId);
  if (!hasDesktopId) {
    return;
  }

  DesktopMonitorKey key{desktopId, monitor};
  TilingState state;
  bool hasState = false;

  AcquireSRWLockShared(&g_tilingStateLock);
  auto it = g_tilingStateMap.find(key);
  if (it != g_tilingStateMap.end()) {
    state = it->second;
    hasState = true;
  }
  ReleaseSRWLockShared(&g_tilingStateLock);

  RECT workArea = {monitorWork.left + g_tileMargin, monitorWork.top + g_tileMargin,
                   monitorWork.right - g_tileMargin, monitorWork.bottom - g_tileMargin};

  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) {
    return;
  }

  HWND resizedHwnd = hwnd;

  if (hasState) {
    if (state.layout == TileLayout::Floating) return;
    if (!ContainsWindow(state.windows, resizedHwnd)) {
      HWND resolved = ResolveToTiledWindow(resizedHwnd, state.windows);
      if (resolved) {
        resizedHwnd = resolved; 

      } else {
        Wh_Log(L"Unresolved window");
        return;
      }
    }
  } else {
    // No saved tiling state for this desktop+monitor:
    // do not auto-rebuild state on resize/move events.
    Wh_Log(L"Tiling not set up for current desktop");
    return;

    // Below code is disabled for now to emulate windows default multitasking behavior. 
    // Can be reused if a tileOnDefault setting is added. 
  
    std::vector<HWND> windows = CollectTileWindows(monitor);
    if (windows.empty()) {
      return;
    }

    if (!ContainsWindow(windows, resizedHwnd)) {
      HWND resolved = ResolveToTiledWindow(resizedHwnd, windows);
      if (resolved) {
        resizedHwnd = resolved;
      } else {
        return;
      }  
      return; 
    }

    TileLayout layout = g_defaultLayout;
    state = BuildStateFromWindows(layout, workArea, windows, monitor);
    state.layout = layout;
    hasState = true;

  }

  // Handle empty/single-window states
  if (HandleTrivialState(key, state, workArea)) return;

  // Window-movement-based un-tiling
  // (compares cached window states)
  AcquireSRWLockExclusive(&g_moveSizeRectsLock);
  auto itStart = g_moveSizeStartRects.find(hwnd);
  if (itStart != g_moveSizeStartRects.end()) {
      
      const RECT& before = itStart->second;

      auto itEnd = g_moveSizeEndRects.find(hwnd);
      if (itEnd != g_moveSizeEndRects.end()) {
          const RECT& after = itEnd->second;

          RectChange change = ClassifyRectChange(before, after, 1);

          // Clean up cache entries first 

          g_moveSizeStartRects.erase(hwnd);
          g_moveSizeEndRects.erase(hwnd);
          ReleaseSRWLockExclusive(&g_moveSizeRectsLock);

          if (change == RectChange::MoveOnly) {
              state.windows.erase(
              std::remove(state.windows.begin(), state.windows.end(), resizedHwnd),
              state.windows.end()
              );

              // handles trivial state
              if (HandleTrivialState(key, state, workArea)) return;

              AcquireSRWLockExclusive(&g_tilingStateLock);
              g_tilingStateMap[key] = state;
              ReleaseSRWLockExclusive(&g_tilingStateLock);
              
          }

      } else {
          Wh_Log(L"End rect missing; cleaned up start rect");
          g_moveSizeStartRects.erase(hwnd);
          ReleaseSRWLockExclusive(&g_moveSizeRectsLock);
      }
  } else {
  g_moveSizeEndRects.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSizeRectsLock);
  Wh_Log(L"Start rect missing; cleaned end rect cache");
  }

    
  //Cleanup pass to remove stale windows before retile
  state.windows.erase(
    std::remove_if(state.windows.begin(), state.windows.end(),
        [&](HWND w) {
            RECT r{};
            return !IsWindow(w) || !GetWindowFrameRect(w, &r);
        }),
    state.windows.end());

  if (HandleTrivialState(key, state, workArea)) return;


  // Loop over state to place all windows
  for (auto itWin = state.windows.begin(); itWin != state.windows.end(); ) {
      HWND w = *itWin;

      RECT rect{};
      if (!GetWindowFrameRect(w, &rect)) {
          Wh_Log(L"Failed to retrieve window rectangle (post-cleanup), removing stale window");
          itWin = state.windows.erase(itWin);   // returning next iterator
          continue;
      }

      if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != monitor) {
          if (IsIconic(w)) {
              Wh_Log(L"Window left monitor (minimized); removing from state");
              itWin = state.windows.erase(itWin); 

              if (HandleTrivialState(key, state, workArea)) return;

              // If the resized target was removed, retarget
              if (w == resizedHwnd || !ContainsWindow(state.windows, resizedHwnd)) {
                  resizedHwnd = state.windows.front();
                  state.stackWeights.clear();
                  state.gridWeights.clear();
              }

              continue;
          } else {
              Wh_Log(L"Window left managed monitor; arranging authoritative workspace as fallback");
              ArrangeWorkspace(key);
              return;
          }
      }

      ++itWin; // only increment when not erasing
  }
  
  std::vector<RECT> windowRects(state.windows.size());

  constexpr LONG kMinRetileSpan = 80; 
  if (state.layout == TileLayout::MasterStack || state.layout == TileLayout::MasterStackH) {
    bool horizontal = (state.layout == TileLayout::MasterStackH);
    LONG totalSize = horizontal ? (workArea.bottom - workArea.top) : (workArea.right - workArea.left);
    if (totalSize <= g_tileGap + 1) {
      return;
    }

    size_t resizedIndex = 0;
    for (size_t i = 0; i < state.windows.size(); ++i) {
      if (state.windows[i] == resizedHwnd) {
        resizedIndex = i;
        break;
      }
    }

    RECT resizedRect = {};
    if (!GetWindowFrameRect(resizedHwnd, &resizedRect)) {
      return;
    }

    LONG resizedSize = horizontal ? (resizedRect.bottom - resizedRect.top) : (resizedRect.right - resizedRect.left);
    if (resizedSize < kMinRetileSpan) resizedSize = kMinRetileSpan;

    LONG masterSize = resizedSize;
    if (resizedIndex != 0) {
      LONG candidateSize = horizontal ? (resizedRect.top - workArea.top - g_tileGap)
                                      : (resizedRect.left - workArea.left - g_tileGap);
      LONG maxMaster = totalSize - g_tileGap - 1;
      if (candidateSize >= 1 && candidateSize <= maxMaster) {
        masterSize = candidateSize;
      } else {
        RECT masterRect = {};
        if (!GetWindowFrameRect(state.windows[0], &masterRect)) {
          return;
        }
        masterSize = horizontal ? (masterRect.bottom - masterRect.top) : (masterRect.right - masterRect.left);
        if (masterSize < kMinRetileSpan) masterSize = kMinRetileSpan;
      }
    }

    state.masterRatio = ClampDouble((double)masterSize / (double)(totalSize - g_tileGap), 0.1, 0.9);

    RECT masterRect = workArea;
    if (horizontal) {
      masterRect.bottom = masterRect.top + masterSize;
      if (masterRect.bottom > workArea.bottom - g_tileGap) {
        masterRect.bottom = workArea.bottom - g_tileGap;
      }
    } else {
      masterRect.right = masterRect.left + masterSize;
      if (masterRect.right > workArea.right - g_tileGap) {
        masterRect.right = workArea.right - g_tileGap;
      }
    }

    RECT stackArea = workArea;
    if (horizontal) {
      stackArea.top = masterRect.bottom + g_tileGap;
    } else {
      stackArea.left = masterRect.right + g_tileGap;
    }

    size_t stackCount = state.windows.size() - 1;
    state.stackWeights.clear();
    state.stackWeights.reserve(stackCount);
    for (size_t i = 1; i < state.windows.size(); ++i) {
      RECT rect = {};
      if (!GetWindowFrameRect(state.windows[i], &rect)) {
        state.stackWeights.push_back(1.0);
        continue;
      }
      LONG size = horizontal ? (rect.right - rect.left) : (rect.bottom - rect.top);
      if (size < 1) size = 1;
      state.stackWeights.push_back(static_cast<double>(size));
    }

    std::vector<LONG> stackSizes;
    if (stackCount > 0) {
      size_t fixedIndex = (resizedIndex == 0) ? (size_t)-1 : (resizedIndex - 1);
      if (fixedIndex != (size_t)-1) {
        LONG fixedSize = horizontal ? (resizedRect.right - resizedRect.left) : (resizedRect.bottom - resizedRect.top);
        if (fixedSize < kMinRetileSpan) fixedSize = kMinRetileSpan;
        LONG stackTotal = horizontal ? (stackArea.right - stackArea.left) : (stackArea.bottom - stackArea.top);
        stackSizes = ComputeWeightedSizesWithFixed(stackTotal, g_tileGap, state.stackWeights, fixedIndex, fixedSize);
      } else {
        LONG stackTotal = horizontal ? (stackArea.right - stackArea.left) : (stackArea.bottom - stackArea.top);
        stackSizes = ComputeWeightedSizes(stackTotal, g_tileGap, state.stackWeights);
      }
    }

    windowRects[0] = masterRect;
    if (stackCount > 0) {
      if (horizontal) {
        LONG x = stackArea.left;
        for (size_t i = 0; i < stackCount; ++i) {
          LONG end = (i == stackCount - 1) ? stackArea.right : x + stackSizes[i];
          windowRects[i + 1] = {x, stackArea.top, end, stackArea.bottom};
          x = end + g_tileGap;
        }
      } else {
        LONG y = stackArea.top;
        for (size_t i = 0; i < stackCount; ++i) {
          LONG end = (i == stackCount - 1) ? stackArea.bottom : y + stackSizes[i];
          windowRects[i + 1] = {stackArea.left, y, stackArea.right, end};
          y = end + g_tileGap;
        }
      }
    }
  } else if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    bool horizontal = (state.layout == TileLayout::Rows);

    struct OrderedWindow {
      HWND hwnd;
      RECT rect;
    };

    std::vector<OrderedWindow> ordered;
    ordered.reserve(state.windows.size());
    for (HWND w : state.windows) {
      RECT rect = {};
      if (!GetWindowFrameRect(w, &rect)) {
        rect = workArea;
      }
      ordered.push_back({w, rect});
    }

    std::stable_sort(ordered.begin(), ordered.end(), [horizontal](const OrderedWindow& a, const OrderedWindow& b) {
      return horizontal ? (a.rect.top < b.rect.top) : (a.rect.left < b.rect.left);
    });

    size_t resizedIndex = 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
      if (ordered[i].hwnd == hwnd) {
        resizedIndex = i;
        break;
      }
    }

    state.gridWeights.clear();
    for (const auto& item : ordered) {
      LONG size = horizontal ? (item.rect.bottom - item.rect.top) : (item.rect.right - item.rect.left);
      if (size < 1) size = 1;
      state.gridWeights.push_back(static_cast<double>(size));
    }

    LONG totalSize = horizontal ? (workArea.bottom - workArea.top) : (workArea.right - workArea.left);
    LONG fixedSize = horizontal ? (ordered[resizedIndex].rect.bottom - ordered[resizedIndex].rect.top)
                                : (ordered[resizedIndex].rect.right - ordered[resizedIndex].rect.left);
    if (fixedSize < kMinRetileSpan) fixedSize = kMinRetileSpan;

    std::vector<LONG> sizes =
        ComputeWeightedSizesWithFixed(totalSize, g_tileGap, state.gridWeights, resizedIndex, fixedSize);

    LONG position = horizontal ? workArea.top : workArea.left;
    for (size_t i = 0; i < ordered.size(); ++i) {
      LONG end = (i == ordered.size() - 1) ? (horizontal ? workArea.bottom : workArea.right) : position + sizes[i];
      RECT rect = horizontal ? RECT{workArea.left, position, workArea.right, end}
                             : RECT{position, workArea.top, end, workArea.bottom};
      windowRects[i] = rect;
      position = end + g_tileGap;
    }

    std::vector<HWND> newOrder;
    newOrder.reserve(ordered.size());
    for (const auto& item : ordered) newOrder.push_back(item.hwnd);
    state.windows = std::move(newOrder);
  } else if (state.layout == TileLayout::BSP) {
    LayoutBSP(workArea, 0, state.windows.size(), 0, windowRects);
  } else if (state.layout == TileLayout::Monocle) {
    windowRects.assign(state.windows.size(), workArea);
  }

  for (size_t i = 0; i < state.windows.size(); ++i) {
    PlaceWindow(state.windows[i], windowRects[i]);
  }

  AcquireSRWLockExclusive(&g_tilingStateLock);
  g_tilingStateMap[key] = state;
  ReleaseSRWLockExclusive(&g_tilingStateLock);
}

void OnWindowResizeEnd(HWND hwnd) {
  if (!g_enableTiling || !g_enableResizeRetile || g_retileSuspended || !IsWindow(hwnd)) return;
  if (!g_threadId) return;
  if (!PostThreadMessage(g_threadId, WM_APP_RETILE_ON_RESIZE, reinterpret_cast<WPARAM>(hwnd), 0)) {
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
    case EVENT_SYSTEM_MOVESIZESTART:
    case EVENT_SYSTEM_MOVESIZEEND:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_UNCLOAKED:
      break;
    default:
      return;
  }

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

    if (!(g_enableTileNewWin && !g_tileNewWinSuspended)) {
      AcquireSRWLockExclusive(&g_moveSizeRectsLock);
      RECT r{};
      if (GetWindowFrameRect(hwnd, &r)) {
        if (event == EVENT_SYSTEM_MOVESIZESTART) g_moveSizeStartRects[hwnd] = r;
        else g_moveSizeEndRects[hwnd] = r;
      }
      ReleaseSRWLockExclusive(&g_moveSizeRectsLock);
    }

    if (event == EVENT_SYSTEM_MOVESIZEEND) OnWindowResizeEnd(hwnd);
    return;
  }

  // Lifecycle events are never dropped because another WM operation is active.
  // PostThreadMessage gives us a serialized event queue owned by HotkeyThreadProc.
  QueueWindowEvent(event, hwnd);
}

void InstallWinEventHooks() {
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
      EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hCloakHook) Wh_Log(L"Failed to install uncloak hook");
  }
}

void RemoveWinEventHooks() {
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
  g_enableResizeRetile = Wh_GetIntSetting(L"EnableResizeRetile") != 0;
  g_captureLayoutOnTile = Wh_GetIntSetting(L"CaptureLayoutOnTile") != 0;
  g_enableLayoutCycle = Wh_GetIntSetting(L"EnableLayoutCycle") != 0;
  // New! 
  g_enableTileNewWin = Wh_GetIntSetting(L"EnableTileNewWin") != 0;
  g_retileSuspended = false;
  g_tileNewWinSuspended = false;


  g_tileKey = ReadStringSetting(L"TileKey", ParseSingleCharKey, (UINT)'D');
  g_layoutKey = ReadStringSetting(L"LayoutKey", ParseSingleCharKey, (UINT)'L');
  g_retileToggleKey = ReadStringSetting(L"RetileToggleKey", ParseSingleCharKey, (UINT)'R');
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
    if (g_enableResizeRetile) {
      RegisterHotKey(nullptr, HK_RETILE_TOGGLE, g_tilingModifiers, g_retileToggleKey);
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

  while (!g_stopHotkeyThread) {
    DWORD waitResult = MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);

    if (waitResult == WAIT_OBJECT_0) {
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {

        switch (msg.message) {  
          case WM_QUIT: 
            goto cleanup;
          
          case WM_APP_RETILE_ON_RESIZE: {
            HWND resizedHwnd = reinterpret_cast<HWND>(msg.wParam);
            RetileFromResize(resizedHwnd);
            continue;
          }

          case WM_APP_WINDOW_EVENT: {
            DWORD event = static_cast<DWORD>(msg.wParam);
            HWND hwnd = reinterpret_cast<HWND>(msg.lParam);
            ProcessWindowLifecycleEvent(event, hwnd);
            continue;
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

              case HK_RETILE_TOGGLE:
                if (g_enableResizeRetile) {
                  g_retileSuspended = !g_retileSuspended;
                  if (g_retileSuspended) {
                    Wh_Log(L"Retile-on-resize suspended; workspace memory preserved");
                  } else {
                    Wh_Log(L"Retile-on-resize resumed");
                  } 
                }
                continue;
            }
            continue;
          }
        }
      }
    }
  }

cleanup:
  if (g_enableTiling) {
    UnregisterHotKey(nullptr, HK_TILE);
    UnregisterHotKey(nullptr, HK_SWAP_MASTER);
    UnregisterHotKey(nullptr, HK_TILENEWWIN_TOGGLE);
    if (g_enableLayoutCycle) {
      UnregisterHotKey(nullptr, HK_LAYOUT);
    }
    if (g_enableResizeRetile) {
      UnregisterHotKey(nullptr, HK_RETILE_TOGGLE);
    }
  }

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

