// ==WindhawkMod==
// @id              tiling-helper-experimental
// @name            Tiling Helper Experimental
// @description     Tile windows on the current monitor with customizable layouts and hotkeys
// @version         1.9.0
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
- Independent workspace insets, window gap, and master size
- Remembers layout mode, ordering, and resize adjustments per virtual desktop + monitor
- Uses native virtual-desktop notifications when available, with coalesced visibility/cloak reconciliation as a safety fallback
- Checked placement with per-window failure tracking and automatic floating for repeatedly non-compliant windows
- Manual or automatic new-window discovery, switchable at runtime without disabling management of existing windows
- Configurable mouse drag behavior: float the moved window or swap it with the tiled window under the pointer
- Dedicated optional hotkey to float the focused tiled window
- Theme-aware notification-area text icon shows the current workspace layout (MV/MH/CL/RW/BS/MN/FL)
- Optional per-layout .ico overrides with generated text fallback
- Compact Win32 status flyout appears on layout/desktop changes and Automatic/Manual mode switches
- Manual Tile globally reconciles known windows, then adopts current tiled geometry when the full tiled set is visible
- Scoped reconciliation keeps lifecycle, virtual-desktop, and monitor ownership repair separate
- Maximized windows preserve their saved tile and use a targeted location-change fallback to detect restore transitions reliably
- Cross-workspace repair preserves unaffected resize weights; only the migrated window gets a default destination weight
- Known HWND ownership is globally unique; current-desktop discovery migrates existing records instead of duplicating them across workspaces
- Optional process/class/title exclusion rules keep user-selected windows out of management
- Diagnostic bookkeeping dump prints a deterministic, formatted snapshot of every saved workspace/window record with live window/process identity, without mutating state
- New-workspace initialization discovers visible minimized/maximized windows as suspended logical members instead of losing them
- Hidden, minimized, and maximized tiled windows preserve their exact logical slot/weight while temporarily leaving active layout
- Native VD switches settle shell state, then softly reinforce every known workspace on the incoming desktop

## Default Hotkeys
- Tile windows: Alt + D
- Cycle layout: Alt + L
- Switch master window: Alt + M
- Float focused window: unbound by default
- Toggle Automatic/Manual mode: Alt + U
- Dump saved bookkeeping: Alt + I

## Notes
- Layout mode, ordering, and resize state are stored per virtual desktop + monitor.
- Floating exists both as a workspace mode and as a per-window managed state.
- Native virtual-desktop notifications are selected from the actual Explorer build/revision; unsupported builds fall back to settled WinEvent reconciliation.
- If Virtual Desktop APIs are unavailable, tiling still works but per-desktop memory is disabled.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- EnableTiling: true
  $name: '[General] Enable Window Manager'
  $description: Enable workspace management, tiling, and related hotkeys.

- DefaultWindowManagementMode: manual
  $name: '[General] Default Window Management Mode'
  $description: Choose whether eligible new windows are discovered automatically after startup. Manual starts automatic discovery paused; already-managed windows are still maintained.
  $options:
    - manual: Manual
    - automatic: Automatic

- DefaultLayout: master_stack
  $name: '[General] Default Workspace Layout'
  $description: Initial layout used when a new desktop + monitor workspace is created.
  $options:
    - master_stack: Master + Stack (Vertical)
    - master_stack_h: Master + Stack (Horizontal)
    - columns: Columns
    - rows: Rows
    - bsp: BSP (Binary Space Partition)
    - monocle: Monocle (Fullscreen)
    - floating: Floating (managed, no automatic placement)

- TileGap: 4
  $name: '[Workspace] Window Gap (pixels)'
  $description: Gap between adjacent tiled windows (0-100).

- WorkspaceInsetLeft: 4
  $name: '[Workspace] Left Inset (pixels)'
  $description: Reserved space inside the monitor work area on the left edge (0-500).

- WorkspaceInsetTop: 4
  $name: '[Workspace] Top Inset (pixels)'
  $description: Reserved space inside the monitor work area on the top edge (0-500).

- WorkspaceInsetRight: 4
  $name: '[Workspace] Right Inset (pixels)'
  $description: Reserved space inside the monitor work area on the right edge (0-500).

- WorkspaceInsetBottom: 4
  $name: '[Workspace] Bottom Inset (pixels)'
  $description: Reserved space inside the monitor work area on the bottom edge (0-500).

- MasterPercent: 50
  $name: '[Workspace] Master Size (%)'
  $description: Default master share in Master + Stack layouts (1-99).

- MouseMoveBehavior: float
  $name: '[Window Behavior] Tiled Window Drag Action'
  $description: Action when a tiled window is moved without resizing.
  $options:
    - float: Float moved window
    - swap: Swap with tiled window under pointer

- Exclusions:
    - - Match: process
        $name: Match by
        $options:
          - process: Process name
          - class: Window class
          - title: Window title contains
      - Value: ""
        $name: Value
  $name: '[Window Rules] Window Exclusions'
  $description: Optional exclusions. Process/class are case-insensitive exact matches; title is a case-insensitive substring.

- TilingModifier: alt
  $name: '[Hotkeys] Modifier'
  $description: Modifier used by all window-manager hotkeys.
  $options:
    - alt: Alt
    - ctrl: Ctrl
    - alt+shift: Alt + Shift
    - ctrl+alt: Ctrl + Alt
    - ctrl+shift: Ctrl + Shift

- TileKey: "D"
  $name: '[Hotkeys] Tile Workspace'
  $description: 'Reconcile and tile windows on the current monitor. Leave blank to disable. Examples: D, T, Space, `, -'

- LayoutKey: "L"
  $name: '[Hotkeys] Cycle Layout'
  $description: 'Cycle the current workspace layout. Leave blank to disable. Examples: L, Tab, =, ], /'

- SwapMasterKey: "M"
  $name: '[Hotkeys] Set Master Window'
  $description: 'Set another tiled window as the master. Leave blank to disable.'

- ManagementModeToggleKey: "U"
  $name: '[Hotkeys] Toggle Window Management Mode'
  $description: 'Switch between Automatic and Manual modes at runtime. Leave blank to disable.'

- FloatFocusedKey: ""
  $name: '[Hotkeys] Float Focused Window'
  $description: 'Float the focused tiled window. Leave blank to disable.'

- FlyoutPosition: bottom
  $name: '[Appearance] Status Flyout Position'
  $description: Show layout and mode feedback at the top or bottom center of the active monitor.
  $options:
    - top: Top
    - bottom: Bottom

- FlyoutOffsetX: 0
  $name: '[Appearance] Status Flyout X Offset'
  $description: Horizontal offset in DPI-scaled pixels (positive moves right).

- FlyoutOffsetY: 0
  $name: '[Appearance] Status Flyout Y Offset'
  $description: Vertical offset in DPI-scaled pixels (positive moves down).

- CustomIcons:
    - - Layout: master_stack
        $name: Layout
        $options:
          - master_stack: Master + Stack (Vertical)
          - master_stack_h: Master + Stack (Horizontal)
          - columns: Columns
          - rows: Rows
          - bsp: BSP
          - monocle: Monocle
          - floating: Floating
      - Path: ""
        $name: Icon path (.ico)
  $name: '[Appearance] Custom Tray Layout Icons'
  $description: Optional per-layout .ico overrides. Failed or empty paths use the generated text icon.

- WindowsVersion: win11_26100
  $name: '[Advanced] Windows Version'
  $description: Select your Windows version for private virtual-desktop interface IDs. Run "winver" to check your build number.
  $options:
    - win10_old: Windows 10 (Build < 20348)
    - win10_20348: Windows 10 (Build 20348 - 21999)
    - win11_22000: Windows 11 (Build 22000 - 22482)
    - win11_22621: Windows 11 (Build 22621/22631/23H2)
    - win11_26100: Windows 11 (Build 26100+ / 24H2)

- ReconcileDelayMs: 80
  $name: '[Advanced] Lifecycle Settle Delay (ms)'
  $description: Delay before visibility/lifecycle bursts are reconciled. Increase only if shell transitions are unusually slow (20-2000 ms).

- DiagnosticDumpKey: "I"
  $name: '[Diagnostics] Dump Workspace State'
  $description: 'Dump the complete saved workspace/window state and invariant audit to the Windhawk log. Uses the configured modifier. Leave blank to disable.'
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
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <new>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

//=============================================================================
// Maintainer architecture map
//=============================================================================
//
// Runtime ownership
// -----------------
// OS callbacks (WinEvent + private virtual-desktop COM) never mutate workspace
// state directly. They enqueue messages onto the STA WM thread. That thread owns
// command handling, lifecycle reconciliation, workspace mutation, placement, and
// UI refresh sequencing.
//
// Event flow
// ----------
// WinEvent / VD callback
//        -> WM thread message
//        -> reconcile observed OS state into the model
//        -> arrange only affected workspaces
//        -> refresh user feedback
//
// Model invariants
// ----------------
// 1. WorkspaceState::records is the authoritative set of logically known HWNDs.
// 2. WorkspaceState::windows contains only active Tiled records, exactly once each,
//    in logical layout order.
// 3. Floating records exist in records but never consume a slot in windows.
// 4. Suspended records exist in records, do not appear in windows, and preserve
//    a saved logical slot/weight until restored or explicitly floated/forgotten.
// 5. Columns/Rows keep one grid weight per active tiled window.
//    MasterStack/MasterStackH keep one stack weight per non-master active window.
// 6. A managed HWND has exactly one desktop+monitor workspace owner.
// 7. Visibility/cloak events are dirty signals; ownership comes from explicit
//    desktop/monitor queries, not from cloak state.
// 8. Automatic vs Manual mode changes only admission of previously unknown HWNDs;
//    all already-managed reconciliation and retile behavior remains active.
//
// If a change cannot be explained in terms of those invariants, it probably
// belongs in the observation/reconciliation layer rather than layout code.

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
// Runtime/configuration state groups
//=============================================================================

// Hotkey IDs are stable message identifiers, not user configuration.
enum HotkeyIds {
  HK_TILE = 1,
  HK_LAYOUT = 2,
  HK_SWAP_MASTER = 3,
  HK_MANAGEMENT_MODE_TOGGLE = 4,
  HK_FLOAT_FOCUSED = 5,
  HK_DIAGNOSTIC_DUMP = 6,
};

// Messages consumed by the serialized WM thread.
constexpr UINT WM_APP_MOVE_SIZE_END = WM_APP + 1;
constexpr UINT WM_APP_WINDOW_EVENT = WM_APP + 2;
constexpr UINT WM_APP_VIRTUAL_DESKTOP_CHANGED = WM_APP + 3;
constexpr UINT WM_APP_TRAY_REFRESH = WM_APP + 4;
constexpr UINT WM_APP_LAYOUT_CYCLE = WM_APP + 5;

enum class TileLayout { MasterStack, Columns, Rows, MasterStackH, BSP, Monocle, Floating, COUNT };
enum class MouseMoveBehavior { Float, Swap };
enum class ExclusionMatch { Process, Class, Title };

struct ExclusionRule {
  ExclusionMatch match = ExclusionMatch::Process;
  std::wstring value;
};

struct WorkspaceInsets {
  LONG left = 4;
  LONG top = 4;
  LONG right = 4;
  LONG bottom = 4;
};

// Windhawk settings and immutable-at-runtime policy values. Settings reloads
// replace these fields; transient WM state lives in WmRuntime instead.
struct SettingsState {
  int windowsVersionIndex = 4;
  UINT reconcileDelayMs = 80;

  UINT tilingModifiers = MOD_ALT;
  UINT tileKey = 'D';
  UINT layoutKey = 'L';
  UINT swapMasterKey = 'M';
  UINT managementModeToggleKey = 'U';
  UINT floatFocusedKey = 0;
  UINT diagnosticDumpKey = 'I';

  WorkspaceInsets insets{};
  LONG gap = 4;
  LONG masterPercent = 50;

  MouseMoveBehavior mouseMoveBehavior = MouseMoveBehavior::Float;
  TileLayout defaultLayout = TileLayout::MasterStack;
  bool enabled = true;
  std::vector<ExclusionRule> exclusionRules;
};

// Private virtual-desktop COM state. This entire object belongs to the STA WM
// thread; callback entry points only post work back to that same thread.
struct VirtualDesktopRuntime {
  IServiceProvider* serviceProvider = nullptr;
  IVirtualDesktopManagerInternal* managerInternal = nullptr;
  IVirtualDesktopManager* desktopManager = nullptr;
  IVirtualDesktopNotificationService* notificationService = nullptr;
  VirtualDesktopNotificationObject* notificationObject = nullptr;
  DWORD notificationCookie = 0;
  bool notificationsRegistered = false;
  DWORD explorerBuild = 0;
  DWORD explorerRevision = 0;
  LONG changeQueued = 0;
  bool initialized = false;
};

// Serialized actor/thread state. Manual mode is represented only by
// autoDiscoverySuspended; the rest of the WM pipeline remains live.
struct WmRuntime {
  HANDLE thread = nullptr;
  DWORD threadId = 0;
  HANDLE readyEvent = nullptr;
  volatile bool stopRequested = false;

  UINT_PTR lifecycleTimer = 0;
  std::vector<HWND> lifecycleDirtyWindows;
  std::vector<PendingWorkspaceArrange> pendingDesktopArranges;
  bool autoDiscoverySuspended = true;
};

static SettingsState g_settings;
static VirtualDesktopRuntime g_vd;
static WmRuntime g_wm;

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

// Why a logically managed tiled window is temporarily absent from the active
// layout. This mirrors FancyWM's Restored vs Minimized/Maximized state boundary,
// while retaining Hidden for close-to-tray behavior.
enum class SuspensionReason {
  None,
  Hidden,
  Minimized,
  Maximized,
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
  // position/weight across non-participating physical states.
  SuspensionReason suspensionReason = SuspensionReason::None;
  SuspendedSlot savedSlot{};
  bool hasSavedSlot = false;
};

struct WorkspaceState {
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

using WorkspaceMap = std::unordered_map<
    DesktopMonitorKey, WorkspaceState, DesktopMonitorKeyHash, DesktopMonitorKeyEqual>;

// Mutations are serialized on the WM thread. The lock remains because WinEvent
// callbacks perform a few read-only "is this HWND already managed?" checks before
// posting work; the store is therefore single-writer / occasionally multi-reader.
struct WorkspaceStore {
  WorkspaceMap states;
  SRWLOCK lock = SRWLOCK_INIT;
};

struct MoveSizeTracker {
  SRWLOCK lock = SRWLOCK_INIT;
  std::unordered_map<HWND, RECT> startRects;
  std::unordered_map<HWND, RECT> endRects;
  std::unordered_map<HWND, POINT> endPoints;
};

struct WinEventHooks {
  HWINEVENTHOOK moveSize = nullptr;
  HWINEVENTHOOK minimize = nullptr;
  HWINEVENTHOOK hideDestroy = nullptr;
  HWINEVENTHOOK cloak = nullptr;
  HWINEVENTHOOK state = nullptr;
  HWINEVENTHOOK foreground = nullptr;
  HWINEVENTHOOK locationChange = nullptr;
};

static WorkspaceStore g_workspaces;
static MoveSizeTracker g_moveSize;
static WinEventHooks g_hooks;

//=============================================================================
// Move/size gesture tracking + classification
//=============================================================================



enum class MoveSizeIntent {
    None,
    Move,
    Resize,
};

static inline bool Differs(LONG a, LONG b, LONG tol = 1) {
    return (a > b) ? (a - b > tol) : (b - a > tol);
}

// Classify the user's *gesture*, not whether position and size both happened to
// change. WinUI/DWM windows can perturb one frame edge slightly during a pure
// move, while a legitimate top/left-edge resize necessarily changes position
// and size together. Opposite edges moving together is therefore the stronger
// signal for translation.
MoveSizeIntent ClassifyMoveSizeIntent(const RECT& before, const RECT& after, LONG tol = 1) {
    const LONG dl = after.left - before.left;
    const LONG dr = after.right - before.right;
    const LONG dt = after.top - before.top;
    const LONG db = after.bottom - before.bottom;

    const LONG maxEdgeDelta = std::max(
        std::max(std::abs(dl), std::abs(dr)),
        std::max(std::abs(dt), std::abs(db)));
    if (maxEdgeDelta <= tol) {
        Wh_Log(L"Move/size intent: none");
        return MoveSizeIntent::None;
    }

    // Twice the center displacement versus actual width/height deformation.
    // Use 64-bit intermediates so adding two LONG deltas cannot overflow.
    const long long moveScore = std::max(
        std::llabs(static_cast<long long>(dl) + dr),
        std::llabs(static_cast<long long>(dt) + db));
    const long long resizeScore = std::max(
        std::llabs(static_cast<long long>(dr) - dl),
        std::llabs(static_cast<long long>(db) - dt));

    // A translation must dominate deformation by a healthy margin. A normal
    // one-edge resize produces roughly equal scores, so it remains Resize.
    if (moveScore > resizeScore * 2) {
        Wh_Log(L"Move/size intent: move (move=%lld resize=%lld)", moveScore, resizeScore);
        return MoveSizeIntent::Move;
    }

    Wh_Log(L"Move/size intent: resize (move=%lld resize=%lld)", moveScore, resizeScore);
    return MoveSizeIntent::Resize;
}






//=============================================================================
// Window/layout utility functions
//=============================================================================

template <typename T>
T GetVTableFunction(void* pInterface, int index) {
  return reinterpret_cast<T>((*reinterpret_cast<void***>(pInterface))[index]);
}

bool UsesHMonitorParameter() { return g_versionIIDs[g_settings.windowsVersionIndex].usesHMonitor; }

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
T ReadHotkeySetting(PCWSTR name, Parser parser, T defaultVal) {
  PCWSTR str = Wh_GetStringSetting(name);
  if (!str || !*str) {
    Wh_FreeStringSetting(str);
    return T{};  // Blank hotkey = unused.
  }
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

bool IsCurrentlyManagedWindow(HWND hwnd, HMONITOR targetMonitor);
static bool WindowCanBeManaged(HWND hwnd);
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
    WorkspaceState& state, HWND hwnd, ManageState initialState = ManageState::Tiled) {
  auto [it, inserted] = state.records.emplace(hwnd, MakeWindowRecord(hwnd, initialState));
  if (!inserted) {
    it->second.hwnd = hwnd;
  }
  return it->second;
}

static WindowRecord* FindWindowRecord(WorkspaceState& state, HWND hwnd) {
  auto it = state.records.find(hwnd);
  return it == state.records.end() ? nullptr : &it->second;
}

// Canonical admission/re-adoption transition for an active tiled participant.
// Layout-specific weight normalization remains the caller's responsibility so
// migration paths can preserve existing weights deliberately.
static bool ActivateWindowAsTiled(
    WorkspaceState& state, HWND hwnd, bool resetPlacementFailures = false) {
  if (!hwnd) return false;

  bool changed = false;
  auto recordIt = state.records.find(hwnd);
  if (recordIt == state.records.end()) {
    recordIt = state.records.emplace(
        hwnd, MakeWindowRecord(hwnd, ManageState::Tiled)).first;
    changed = true;
  }

  WindowRecord& record = recordIt->second;
  if (record.hwnd != hwnd) {
    record.hwnd = hwnd;
    changed = true;
  }
  if (record.state != ManageState::Tiled || record.hasSavedSlot ||
      record.suspensionReason != SuspensionReason::None) {
    record.state = ManageState::Tiled;
    record.suspensionReason = SuspensionReason::None;
    record.savedSlot = {};
    record.hasSavedSlot = false;
    changed = true;
  }
  if (resetPlacementFailures && record.placementFailures != 0) {
    record.placementFailures = 0;
    changed = true;
  }
  if (!ContainsWindow(state.windows, hwnd)) {
    state.windows.push_back(hwnd);
    changed = true;
  }
  return changed;
}

// Best-effort bridge for apps that replace one top-level HWND with another.
// Candidates must share the PID and class; an exact title match wins, otherwise
// the first PID/class match is returned from the caller's scoped candidate set.
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

// Maps an event or foreground HWND to its active tile, preferring exact and
// ownership relationships before falling back to the looser equivalence check.
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
        if (IsCurrentlyManagedWindow(hwnd, ctx->targetMonitor)) {
          ctx->windowList->push_back(hwnd);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

// Adopts the supplied windows' current geometry as authoritative workspace state.
// Master/grid layouts infer ordering and resize weights from frame bounds; every
// supplied HWND is rebuilt as an active tiled record.
WorkspaceState BuildStateFromWindows(TileLayout layout, const RECT& workArea, const std::vector<HWND>& windows,
                                  HMONITOR monitor, HWND preferredFirstWin = nullptr) {
  WorkspaceState state;
  state.layout = layout;
  state.windows = windows;
  if (windows.empty()) return state;
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
    if (totalSize > g_settings.gap + 1) {
      LONG masterSize = horizontal ? (master.rect.bottom - master.rect.top) : (master.rect.right - master.rect.left);
      if (masterSize < 1) masterSize = 1;
      state.masterRatio = ClampDouble((double)masterSize / (double)(totalSize - g_settings.gap), 0.1, 0.9);
    } else {
      state.masterRatio = ClampDouble(g_settings.masterPercent / 100.0, 0.1, 0.9);
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
  std::vector<LONG> sizes = ComputeWeightedSizes(totalSize, g_settings.gap, weights);
  LONG position = horizontal ? area.top : area.left;

  for (size_t i = 0; i < windowCount; ++i) {
    LONG size = sizes[i];
    LONG end = (i == windowCount - 1) ? (horizontal ? area.bottom : area.right) : position + size;
    outRects[i] = horizontal ? RECT{area.left, position, area.right, end} : RECT{position, area.top, end, area.bottom};
    position = end + g_settings.gap;
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
  LONG masterSize = static_cast<LONG>(std::llround((totalSize - g_settings.gap) * masterRatio));
  if (masterSize < 1) masterSize = 1;
  if (masterSize > totalSize - g_settings.gap - 1) masterSize = totalSize - g_settings.gap - 1;

  size_t stackCount = windowCount - 1;
  std::vector<double> weights = stackWeights;
  if (weights.size() != stackCount) weights = DefaultWeights(stackCount);

  if (horizontal) {
    outRects[0] = {area.left, area.top, area.right, area.top + masterSize};
    LONG stackTop = area.top + masterSize + g_settings.gap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.right - area.left, g_settings.gap, weights);
    LONG x = area.left;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.right : x + sizes[i];
      outRects[i + 1] = {x, stackTop, end, area.bottom};
      x = end + g_settings.gap;
    }
  } else {
    outRects[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    LONG stackLeft = area.left + masterSize + g_settings.gap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.bottom - area.top, g_settings.gap, weights);
    LONG y = area.top;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.bottom : y + sizes[i];
      outRects[i + 1] = {stackLeft, y, area.right, end};
      y = end + g_settings.gap;
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
  LONG mid = splitVertical ? area.left + (area.right - area.left - g_settings.gap) / 2
                           : area.top + (area.bottom - area.top - g_settings.gap) / 2;

  outRects[startIndex] =
      splitVertical ? RECT{area.left, area.top, mid, area.bottom} : RECT{area.left, area.top, area.right, mid};

  RECT remaining = splitVertical ? RECT{mid + g_settings.gap, area.top, area.right, area.bottom}
                                 : RECT{area.left, mid + g_settings.gap, area.right, area.bottom};

  LayoutBSP(remaining, startIndex + 1, count - 1, depth + 1, outRects);
}

//=============================================================================
// Virtual Desktop API + native change notifications
//=============================================================================

static const int VTABLE_GET_CURRENT_DESKTOP = 6;

// Selects the private notification IID and vtable shape for this Explorer build.
// An empty result means the ABI is unknown and native notifications are unsafe.
static NotificationInterfaceConfig GetNotificationInterfaceConfig() {
  if (g_vd.explorerBuild < 22000) return {};

  // Mirrors Windhawk's Taskbar Desktop Indicator ABI table. The private
  // notification interface changed both IID and callback layout within the
  // Windows 11 22H2/23H2 servicing lifetime, so build number alone is not enough.
  if (g_vd.explorerBuild < 22483 ||
      (g_vd.explorerBuild == 22621 && g_vd.explorerRevision < 2215)) {
    return {IID_IVirtualDesktopNotification_Old, 13, 11, true};
  }

  if (g_vd.explorerBuild < 22631 ||
      (g_vd.explorerBuild == 22631 && g_vd.explorerRevision < 3085)) {
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

  g_vd.explorerBuild = HIWORD(fixedInfo->dwFileVersionLS);
  g_vd.explorerRevision = LOWORD(fixedInfo->dwFileVersionLS);
  Wh_Log(L"Explorer version for VD notifications: build=%lu revision=%lu",
         g_vd.explorerBuild, g_vd.explorerRevision);
  return true;
}

static bool IsCurrentNotificationInterface(REFIID riid) {
  NotificationInterfaceConfig config = GetNotificationInterfaceConfig();
  return config.methodCount > 0 && InlineIsEqualGUID(riid, config.iid);
}

static void QueueVirtualDesktopChangedNotification() {
  if (!g_wm.threadId || g_wm.stopRequested) return;

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

static bool RegisterVirtualDesktopNotifications() {
  if (g_vd.notificationsRegistered) return true;
  if (!g_vd.serviceProvider) return false;
  if (!ReadExplorerVersion()) return false;

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
  HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
                                (void**)&g_vd.serviceProvider);
  if (FAILED(hr) || !g_vd.serviceProvider) {
    Wh_Log(L"Failed to create ImmersiveShell: 0x%08X", hr);
    return false;
  }

  hr = g_vd.serviceProvider->QueryService(CLSID_VirtualDesktopManagerInternal,
                                        g_versionIIDs[g_settings.windowsVersionIndex].managerInternal,
                                        (void**)&g_vd.managerInternal);
  if (FAILED(hr) || !g_vd.managerInternal) {
    Wh_Log(L"Failed to get VirtualDesktopManagerInternal: 0x%08X", hr);
    SAFE_RELEASE(g_vd.serviceProvider);
    return false;
  }

  hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager,
                        (void**)&g_vd.desktopManager);
  if (FAILED(hr) || !g_vd.desktopManager) {
    Wh_Log(L"Failed to create VirtualDesktopManager: 0x%08X", hr);
    SAFE_RELEASE(g_vd.managerInternal);
    SAFE_RELEASE(g_vd.serviceProvider);
    return false;
  }

  return true;
}

// Releases virtual-desktop COM state while Explorer is responsive. During shell
// teardown, pointers are intentionally dropped without Release to avoid hangs.
void CleanupVirtualDesktopAPI() {
  HWND hShell = GetShellWindow();
  const bool shellAvailable = hShell && IsWindow(hShell);

  ClearVirtualDesktopNotificationState(shellAvailable);

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
  CleanupVirtualDesktopAPI();
  if (!InitializeVirtualDesktopAPI()) return false;
  if (g_settings.enabled) RegisterVirtualDesktopNotifications();
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
  if (!g_vd.managerInternal) return false;

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
  if (!InitializeVirtualDesktopAPI() || !g_vd.desktopManager) return false;
  return SUCCEEDED(g_vd.desktopManager->GetWindowDesktopId(hwnd, outGuid));
}

//=============================================================================
// Window observation + eligibility
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

  // WindowRecord identity is exact. Child/owned HWND lifecycle events must not
  // inherit the managed state of their top-level/root-owner window.
  bool tracked = false;

  AcquireSRWLockShared(&g_workspaces.lock);
  for (const auto& kv : g_workspaces.states) {
    const WorkspaceState& st = kv.second;
    if (st.records.find(hwnd) != st.records.end()) {
      tracked = true;
      break;
    }
  }
  ReleaseSRWLockShared(&g_workspaces.lock);

  return tracked;
}

static bool EqualsInsensitive(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty()) return false;
  return std::search(
             text.begin(), text.end(), needle.begin(), needle.end(),
             [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end();
}

static std::wstring GetWindowProcessName(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return {};

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};

  wchar_t path[32768]{};
  DWORD chars = ARRAYSIZE(path);
  std::wstring result;
  if (QueryFullProcessImageNameW(process, 0, path, &chars) && chars) {
    const wchar_t* base = path;
    for (DWORD i = 0; i < chars; ++i) {
      if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    }
    result.assign(base, static_cast<size_t>((path + chars) - base));
  }
  CloseHandle(process);
  return result;
}

static bool IsWindowExcludedByRules(HWND hwnd) {
  if (!hwnd || g_settings.exclusionRules.empty()) return false;

  std::wstring processName;
  std::wstring className;
  std::wstring title;
  bool haveProcess = false;
  bool haveClass = false;
  bool haveTitle = false;

  for (const auto& rule : g_settings.exclusionRules) {
    switch (rule.match) {
      case ExclusionMatch::Process:
        if (!haveProcess) {
          processName = GetWindowProcessName(hwnd);
          haveProcess = true;
        }
        if (!processName.empty() && EqualsInsensitive(processName, rule.value)) return true;
        break;

      case ExclusionMatch::Class:
        if (!haveClass) {
          wchar_t buffer[256]{};
          if (GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer))) className = buffer;
          haveClass = true;
        }
        if (!className.empty() && EqualsInsensitive(className, rule.value)) return true;
        break;

      case ExclusionMatch::Title:
        if (!haveTitle) {
          wchar_t buffer[512]{};
          if (GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer))) title = buffer;
          haveTitle = true;
        }
        if (!title.empty() && ContainsInsensitive(title, rule.value)) return true;
        break;
    }
  }
  return false;
}

// Applies stable structural and exclusion policy only; visibility, show state,
// desktop, and monitor participation are checked separately. Tracked windows
// tolerate a temporarily missing resize style so transient app UI cannot evict them.
static bool WindowCanBeManaged(HWND hwnd) {
  if (!hwnd) return false;
  if (IsWindowExcludedByRules(hwnd)) return false;

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

// Determines whether an HWND can actively participate on targetMonitor now.
// Participation requires restored, visible, uncloaked current-desktop geometry,
// while structural eligibility remains tolerant of already-tracked windows.
bool IsCurrentlyManagedWindow(HWND hwnd, HMONITOR targetMonitor) {
  // window might become temporarily disabled
  // (example: VSCode unstaged commit dialog)
  if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return false;

  // FancyWM only tiles windows in the Restored state. Maximized windows remain
  // managed logically but do not participate until Windows reports Restored.
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return false;
  if (!WindowCanBeManaged(hwnd)) return false;
  if (IsWindowCloaked(hwnd)) return false;

  // Virtual desktop filter: other desktops' windows may not yet be cloaked
  // at the moment we enumerate (race between uncloak/cloak during switch).
  if (g_vd.desktopManager) {
    BOOL onCurrent = FALSE;
    if (SUCCEEDED(g_vd.desktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) && !onCurrent)
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

// Positions the visible DWM frame without changing activation, Z-order, or show
// state, then records whether Windows accepted or adjusted the requested geometry.
static PlacementResult PlaceWindowChecked(WindowRecord& record, const RECT& targetRect) {
  HWND hwnd = record.hwnd;
  record.lastRequestedRect = targetRect;

  auto finish = [&](PlacementResult result) {
    record.lastPlacementResult = result;
    return result;
  };

  if (!hwnd || !IsWindow(hwnd)) return finish(PlacementResult::Dead);
  if (!record.canMove) return finish(PlacementResult::Refused);

  // Soft reinforcement should be a no-op when Windows already matches the
  // authoritative layout, just like FancyWM skips unchanged reposition targets.
  RECT currentFrame{};
  if (GetWindowFrameRect(hwnd, &currentFrame)) {
    record.lastObservedRect = currentFrame;
    if (RectsNear(currentFrame, targetRect)) {
      return finish(PlacementResult::Success);
    }
  }

  // Window state transitions are policy, not placement. A maximized window must
  // never be silently restored merely because a layout pass ran.
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
static void ClearMoveSizeSamples(HWND hwnd);

// Promotes the foreground window, or its tiled owner, to the master slot, then
// clears stale gesture samples for both windows and reflows the workspace.
void SwapMaster() {
  HWND fg = GetForegroundWindow();
  if (!fg) return;

  HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(fg, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  WorkspaceState state;
  if (!LoadWorkspaceState(key, &state) || state.windows.size() < 2 ||
      state.layout == TileLayout::Floating) {
    return;
  }

  HWND resolved = ResolveToTiledWindow(fg, state.windows);
  auto resolvedIt = std::find(state.windows.begin(), state.windows.end(), resolved);
  if (!resolved || resolvedIt == state.windows.end() || resolvedIt == state.windows.begin()) {
    return;
  }

  HWND oldMaster = state.windows.front();
  std::iter_swap(state.windows.begin(), resolvedIt);
  SaveWorkspaceState(key, state);

  // Purge gesture samples for both windows, then render through the same checked
  // placement path used by every other workspace mutation.
  ClearMoveSizeSamples(oldMaster);
  ClearMoveSizeSamples(resolved);
  ArrangeWorkspace(key);
}



//=============================================================================
// Workspace repository + authoritative model
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

static bool GetWorkspaceWorkArea(HMONITOR monitor, RECT* outWorkArea) {
  if (!monitor || !outWorkArea) return false;
  RECT monitorWork{};
  if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;

  *outWorkArea = {
      monitorWork.left + g_settings.insets.left,
      monitorWork.top + g_settings.insets.top,
      monitorWork.right - g_settings.insets.right,
      monitorWork.bottom - g_settings.insets.bottom};
  return outWorkArea->right > outWorkArea->left &&
         outWorkArea->bottom > outWorkArea->top;
}

// Resolves a monitor on the current virtual desktop to its workspace key and,
// when requested, returns the configured workspace-inset work area.
static bool GetCurrentWorkspaceKey(HMONITOR monitor, DesktopMonitorKey* outKey, RECT* outWorkArea = nullptr) {
  if (!monitor || !outKey) return false;
  GUID desktopId{};
  // Never collapse separate desktops into a zero-GUID workspace when the
  // private API is temporarily unavailable; let the caller retry after recovery.
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&desktopId)) {
    return false;
  }
  outKey->desktopId = desktopId;
  outKey->monitor = monitor;

  if (outWorkArea && !GetWorkspaceWorkArea(monitor, outWorkArea)) return false;
  return true;
}

static bool ValidateWorkspaceState(
    const DesktopMonitorKey& key, const WorkspaceState& state,
    const wchar_t* context = L"save") {
  bool valid = true;
  auto fail = [&](const wchar_t* message, HWND hwnd = nullptr) {
    Wh_Log(
        L"Workspace invariant violation (%s, desktop=%08X monitor=%p hwnd=%p): %s",
        context, key.desktopId.Data1, key.monitor, hwnd, message);
    valid = false;
  };

  std::unordered_set<HWND> active;
  for (HWND hwnd : state.windows) {
    if (!hwnd) fail(L"null HWND in active tiled order");
    if (!active.insert(hwnd).second) fail(L"duplicate HWND in active tiled order", hwnd);

    auto recordIt = state.records.find(hwnd);
    if (recordIt == state.records.end()) {
      fail(L"active tiled HWND has no WindowRecord", hwnd);
    } else if (recordIt->second.state != ManageState::Tiled) {
      fail(L"active tiled HWND does not have Tiled state", hwnd);
    }
  }

  size_t suspendedCount = 0;
  std::unordered_set<size_t> suspendedLogicalSlots;
  for (const auto& kv : state.records) {
    HWND hwnd = kv.first;
    const WindowRecord& record = kv.second;
    const bool isActive = active.find(hwnd) != active.end();

    if (record.hwnd && record.hwnd != hwnd) {
      fail(L"WindowRecord hwnd field disagrees with record-map key", hwnd);
    }

    switch (record.state) {
      case ManageState::Tiled:
        if (!isActive) fail(L"Tiled record is absent from active tiled order", hwnd);
        if (record.hasSavedSlot) fail(L"Tiled record retains a suspended slot", hwnd);
        break;

      case ManageState::Suspended:
        ++suspendedCount;
        if (isActive) fail(L"Suspended record appears in active tiled order", hwnd);
        if (!record.hasSavedSlot) {
          fail(L"Suspended record has no saved logical slot", hwnd);
        } else if (!suspendedLogicalSlots.insert(record.savedSlot.index).second) {
          fail(L"two suspended records share the same logical slot", hwnd);
        }
        break;

      case ManageState::Floating:
      case ManageState::Ignored:
        if (isActive) fail(L"non-tiled record appears in active tiled order", hwnd);
        if (record.hasSavedSlot) fail(L"non-suspended record retains a saved slot", hwnd);
        break;
    }
  }

  const size_t logicalCount = state.windows.size() + suspendedCount;
  for (const auto& kv : state.records) {
    const WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot &&
        record.savedSlot.index >= logicalCount) {
      fail(L"suspended logical slot is outside workspace logical order", kv.first);
    }
  }

  if (!std::isfinite(state.masterRatio) || state.masterRatio < 0.1 || state.masterRatio > 0.9) {
    fail(L"master ratio is outside the supported range");
  }

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    if (state.gridWeights.size() != state.windows.size()) {
      fail(L"grid weight count does not match active tiled count");
    }
    if (!state.stackWeights.empty()) fail(L"grid layout retains stack weights");
  } else if (state.layout == TileLayout::MasterStack ||
             state.layout == TileLayout::MasterStackH) {
    const size_t expected = state.windows.empty() ? 0 : state.windows.size() - 1;
    if (state.stackWeights.size() != expected) {
      fail(L"stack weight count does not match non-master tiled count");
    }
    if (!state.gridWeights.empty()) fail(L"master/stack layout retains grid weights");
  } else {
    if (!state.stackWeights.empty() || !state.gridWeights.empty()) {
      fail(L"weightless layout retains layout weights");
    }
  }

  return valid;
}

// Persistence boundary for in-session workspace state. All writers validate the
// complete model before publishing a new snapshot to cross-thread readers.
static void SaveWorkspaceState(const DesktopMonitorKey& key, const WorkspaceState& state) {
  ValidateWorkspaceState(key, state);
  AcquireSRWLockExclusive(&g_workspaces.lock);
  g_workspaces.states[key] = state;
  ReleaseSRWLockExclusive(&g_workspaces.lock);
}

static bool LoadWorkspaceState(const DesktopMonitorKey& key, WorkspaceState* state) {
  if (!state) return false;
  bool found = false;
  AcquireSRWLockShared(&g_workspaces.lock);
  auto it = g_workspaces.states.find(key);
  if (it != g_workspaces.states.end()) {
    *state = it->second;
    found = true;
  }
  ReleaseSRWLockShared(&g_workspaces.lock);
  return found;
}


// UI is intentionally implemented later so the complete workspace model and
// reconciliation pipeline can be read contiguously. Only this narrow surface is
// visible to the WM core.
namespace TrayUi {
static void LoadSettings();
static bool Initialize();
static void Shutdown();
static void UpdateIcon(TileLayout layout);
static void ShowLayoutFlyout(TileLayout layout);
static void ShowManagementModeFlyout(bool automatic);
static void RefreshForCurrentWorkspace();
}  // namespace TrayUi



//=============================================================================
// Workspace mutation API
//=============================================================================
//
// These functions are the only supported ways to change logical participation:
//   SuspendWindowInState  Tiled -> Suspended (preserve logical slot/weight)
//   RestoreWindowInState  Suspended -> Tiled
//   FloatWindowInState    Tiled/Suspended -> Floating (close logical gap)
//   ForgetWindowInState   any managed state -> unmanaged
//   ActivateWindowAsTiled / AddInitialWorkspaceWindow -> admission/re-adoption
//   AppendTiledWindowWithDefaultWeight -> weight-preserving migration admission
//
// Callers may adjust layout choice/weights/order for explicit user commands, but
// should not hand-edit WindowRecord participation fields or suspended slots.

// Keep the active layout's weight vector aligned with its kind.
// A size mismatch resets that layout's weights to equal shares.
static void EnsureSaneWeightVector(WorkspaceState& state) {
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

// Removes an active tile and its layout-specific weight, optionally capturing
// the slot metadata needed to restore its order and proportions later.
static void RemoveActiveWindowAt(WorkspaceState& state, size_t index, SuspendedSlot* saved = nullptr) {
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
      // windows[1] becomes the temporary master, so only its former stack
      // weight stops participating. Keep the remaining stack weights attached
      // to the same windows instead of resetting the whole stack.
      if (!state.stackWeights.empty()) {
        slot.weight = state.stackWeights.front();
        slot.hasWeight = true;
        state.stackWeights.erase(state.stackWeights.begin());
      }
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

// Maps an active-vector index into persistent logical order by accounting for
// the saved positions of all currently suspended windows.
static size_t ActiveIndexToLogicalIndex(const WorkspaceState& state, size_t activeIndex) {
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

// Maps a saved logical slot back into the active vector while excluding the
// suspended record currently being restored.
static size_t LogicalIndexToActiveIndex(
    const WorkspaceState& state, size_t logicalIndex, HWND restoringHwnd) {
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
    WorkspaceState& state, size_t removedLogicalIndex, HWND exceptHwnd = nullptr) {
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

// Removes a tiled window from active placement while preserving its logical slot
// and weight. Repeated suspension changes only the physical-state reason.
static bool SuspendWindowInState(
    WorkspaceState& state, HWND hwnd, SuspensionReason reason) {
  WindowRecord* existing = FindWindowRecord(state, hwnd);
  if (existing && existing->state == ManageState::Suspended) {
    if (existing->suspensionReason == reason) return false;
    existing->suspensionReason = reason;
    return true;
  }

  auto it = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (it == state.windows.end()) return false;

  WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Tiled);
  SuspendedSlot slot{};
  size_t activeIndex = static_cast<size_t>(it - state.windows.begin());
  size_t logicalIndex = ActiveIndexToLogicalIndex(state, activeIndex);
  RemoveActiveWindowAt(state, activeIndex, &slot);
  slot.index = logicalIndex;

  record.state = ManageState::Suspended;
  record.suspensionReason = reason;
  record.savedSlot = slot;
  record.hasSavedSlot = true;
  return true;
}

static SuspensionReason GetPhysicalSuspensionReason(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return SuspensionReason::None;
  if (IsIconic(hwnd)) return SuspensionReason::Minimized;
  if (!IsWindowVisible(hwnd)) return SuspensionReason::Hidden;
  if (IsZoomed(hwnd)) return SuspensionReason::Maximized;
  return SuspensionReason::None;
}


// Workspace initialization needs a broader physical snapshot than normal layout
// participation. A visible top-level window can already belong to this workspace
// while minimized or maximized, even though CollectTileWindows correctly excludes
// it from placement. Enumerate that broader set only when creating a workspace.
static std::vector<HWND> CollectWorkspaceWindowsForInitialization(
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

        // Hidden/close-to-tray HWNDs are not safe to admit from a one-shot scan.
        // Minimized and maximized windows normally retain WS_VISIBLE and are the
        // non-participating states this initialization pass is intended to find.
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (!WindowCanBeManaged(hwnd)) return TRUE;
        if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        // Do not reject DWMWA_CLOAKED here. A brand-new incoming VD can still
        // be shell-cloaked while its exact desktop GUID is already authoritative.
        // Normal arrangement still refuses to place cloaked HWNDs after settle.

        // MonitorFromWindow deliberately uses the pre-minimize rectangle for a
        // minimized HWND, so it is safer here than inspecting minimized geometry.
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != ctx->targetMonitor) {
          return TRUE;
        }

        ctx->windowList->push_back(hwnd);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

static size_t InitialLogicalWindowCount(const WorkspaceState& state) {
  size_t count = state.windows.size();
  for (const auto& kv : state.records) {
    if (kv.second.state == ManageState::Suspended && kv.second.hasSavedSlot) ++count;
  }
  return count;
}

// Add one previously unknown HWND while creating a workspace. Restored windows
// participate immediately; minimized/maximized windows receive a neutral saved
// logical slot and stay out of state.windows until their normal restore event.
static bool AddInitialWorkspaceWindow(WorkspaceState& state, HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd) || state.records.find(hwnd) != state.records.end() ||
      IsWindowTrackedInAnyState(hwnd)) {
    return false;
  }

  SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
  if (reason == SuspensionReason::Hidden) return false;

  WindowRecord record = MakeWindowRecord(
      hwnd, reason == SuspensionReason::None ? ManageState::Tiled
                                             : ManageState::Suspended);

  if (reason == SuspensionReason::None) {
    state.windows.push_back(hwnd);
  } else {
    const size_t logicalIndex = InitialLogicalWindowCount(state);
    record.suspensionReason = reason;
    record.savedSlot.index = logicalIndex;

    if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
      record.savedSlot.weight = 1.0;
      record.savedSlot.hasWeight = true;
    } else if (state.layout == TileLayout::MasterStack ||
               state.layout == TileLayout::MasterStackH) {
      if (logicalIndex == 0) {
        record.savedSlot.wasMaster = true;
      } else {
        record.savedSlot.weight = 1.0;
        record.savedSlot.hasWeight = true;
      }
    }

    record.hasSavedSlot = true;
  }

  state.records.emplace(hwnd, record);
  return true;
}

// EVENT_OBJECT_LOCATIONCHANGE is extremely noisy for ordinary tiled windows,
// including changes caused by our own SetWindowPos calls. We only need it as a
// missing-state-event fallback while a managed window is logically detached for
// maximization; at that point ArrangeWorkspace does not move the HWND itself.
static bool IsWindowSuspendedMaximized(HWND hwnd) {
  if (!hwnd) return false;

  bool suspendedMaximized = false;
  AcquireSRWLockShared(&g_workspaces.lock);
  for (const auto& kv : g_workspaces.states) {
    auto it = kv.second.records.find(hwnd);
    if (it != kv.second.records.end() &&
        it->second.state == ManageState::Suspended &&
        it->second.suspensionReason == SuspensionReason::Maximized) {
      suspendedMaximized = true;
      break;
    }
  }
  ReleaseSRWLockShared(&g_workspaces.lock);
  return suspendedMaximized;
}

// Re-inserts a suspended window at its saved logical slot and restores the
// layout-specific weight and master bookkeeping captured on suspension.
static bool RestoreWindowInState(WorkspaceState& state, HWND hwnd) {
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
      // The window which temporarily became master returns to stack index 0.
      // Restore exactly the weight it had before the original master left.
      if (state.stackWeights.size() + 1 == state.windows.size() - 1) {
        state.stackWeights.insert(
            state.stackWeights.begin(), slot.hasWeight ? slot.weight : 1.0);
      }
    } else if (index > 0 &&
               state.stackWeights.size() + 1 == state.windows.size() - 1) {
      size_t stackIndex = std::min(index - 1, state.stackWeights.size());
      state.stackWeights.insert(
          state.stackWeights.begin() + stackIndex,
          slot.hasWeight ? slot.weight : 1.0);
    }
  }

  record->state = ManageState::Tiled;
  record->suspensionReason = SuspensionReason::None;
  record->savedSlot = {};
  record->hasSavedSlot = false;
  EnsureSaneWeightVector(state);
  return true;
}

// Removes a tiled or suspended window's logical slot while retaining its record
// as individually floating and renumbering later suspended slots.
static bool FloatWindowInState(WorkspaceState& state, HWND hwnd) {
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
  record.suspensionReason = SuspensionReason::None;
  record.savedSlot = {};
  record.hasSavedSlot = false;
  EnsureSaneWeightVector(state);
  return true;
}

static void FloatFocusedWindow() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return;

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  GUID desktopId{};
  if (!monitor || !GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  WorkspaceState state;
  if (!LoadWorkspaceState(key, &state)) return;

  HWND tiled = ResolveToTiledWindow(hwnd, state.windows);
  if (!tiled || !FloatWindowInState(state, tiled)) return;

  if (WindowRecord* record = FindWindowRecord(state, tiled)) {
    record->placementFailures = 0;
  }
  SaveWorkspaceState(key, state);
  ArrangeWorkspace(key);
  Wh_Log(L"Focused window floated %p", tiled);
}

// Remove a window from management entirely. Floating/ignored windows have no
// logical tiled slot, while tiled/suspended windows close their layout gap.
static bool ForgetWindowInState(WorkspaceState& state, HWND hwnd) {
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

static bool IsWorkspaceOnActiveDesktop(const DesktopMonitorKey& key) {
  GUID zeroGuid{};
  if (IsEqualGUID(key.desktopId, zeroGuid)) return true;

  GUID currentDesktop{};
  return !GetCurrentDesktopId(&currentDesktop) ||
         IsEqualGUID(currentDesktop, key.desktopId);
}

// Repairs only model-local arrangement preconditions. OS lifecycle/ownership
// reconciliation belongs elsewhere; this function handles missed destruction and
// legacy states whose active-order record marker drifted.
static void RepairWorkspaceForArrangement(WorkspaceState& state) {
  std::vector<HWND> invalidWindows;
  for (const auto& kv : state.records) {
    if (!IsWindow(kv.first)) invalidWindows.push_back(kv.first);
  }
  for (HWND hwnd : invalidWindows) {
    ForgetWindowInState(state, hwnd);
  }

  // Iterate over a copy because the canonical transition is also responsible for
  // ensuring active-order membership when repairing legacy records.
  const std::vector<HWND> activeOrder = state.windows;
  for (HWND hwnd : activeOrder) {
    ActivateWindowAsTiled(state, hwnd);
  }
  EnsureSaneWeightVector(state);
}

static bool BuildWorkspaceLayoutPlan(
    const WorkspaceState& state, const RECT& workArea,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  outRects->assign(state.windows.size(), RECT{});

  switch (state.layout) {
    case TileLayout::MasterStack:
      LayoutMasterStackWeighted(
          workArea, state.windows.size(), *outRects, false,
          ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      return true;

    case TileLayout::MasterStackH:
      LayoutMasterStackWeighted(
          workArea, state.windows.size(), *outRects, true,
          ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
      return true;

    case TileLayout::Columns:
      LayoutGridWeighted(
          workArea, state.windows.size(), *outRects, false, state.gridWeights);
      return true;

    case TileLayout::Rows:
      LayoutGridWeighted(
          workArea, state.windows.size(), *outRects, true, state.gridWeights);
      return true;

    case TileLayout::BSP:
      LayoutBSP(workArea, 0, state.windows.size(), 0, *outRects);
      return true;

    case TileLayout::Monocle:
      outRects->assign(state.windows.size(), workArea);
      return true;

    case TileLayout::Floating:
    case TileLayout::COUNT:
      outRects->clear();
      return false;
  }
  return false;
}

struct PlacementBatchResult {
  std::vector<HWND> toFloat;
  std::vector<HWND> toForget;
};

static PlacementBatchResult ApplyWorkspacePlacement(
    WorkspaceState& state, const std::vector<RECT>& rects) {
  PlacementBatchResult batch;
  const size_t count = std::min(state.windows.size(), rects.size());

  for (size_t i = 0; i < count; ++i) {
    HWND hwnd = state.windows[i];
    if (!IsWindow(hwnd)) {
      batch.toForget.push_back(hwnd);
      continue;
    }

    // Renderability is separate from logical membership. A temporarily
    // non-renderable tiled record continues to consume its saved logical slot.
    if (IsIconic(hwnd) || !IsWindowVisible(hwnd) || IsWindowCloaked(hwnd) ||
        IsZoomed(hwnd)) {
      continue;
    }

    WindowRecord& record = EnsureWindowRecord(state, hwnd, ManageState::Tiled);
    switch (PlaceWindowChecked(record, rects[i])) {
      case PlacementResult::Success:
        record.placementFailures = 0;
        break;

      case PlacementResult::AdjustedByWindow:
      case PlacementResult::AccessDenied:
      case PlacementResult::Refused:
        ++record.placementFailures;
        if (record.placementFailures >= kPlacementFailureLimit) {
          batch.toFloat.push_back(hwnd);
        }
        break;

      case PlacementResult::Dead:
        batch.toForget.push_back(hwnd);
        break;
    }
  }
  return batch;
}

static bool ApplyPlacementConsequences(
    WorkspaceState& state, const PlacementBatchResult& batch) {
  bool participationChanged = false;

  for (HWND hwnd : batch.toForget) {
    participationChanged |= ForgetWindowInState(state, hwnd);
  }

  for (HWND hwnd : batch.toFloat) {
    WindowRecord* record = FindWindowRecord(state, hwnd);
    if (!record || record->state != ManageState::Tiled) continue;

    Wh_Log(
        L"Window %p failed checked placement %d times (last result=%d); switching it to Floating",
        hwnd, record->placementFailures,
        static_cast<int>(record->lastPlacementResult));
    participationChanged |= FloatWindowInState(state, hwnd);
  }

  EnsureSaneWeightVector(state);
  return participationChanged;
}

// Renders one workspace from authoritative model state. Arrangement itself is a
// small pipeline: repair local invariants -> compute targets -> apply placement ->
// fold placement outcomes back into the model. Reflow is iterative rather than
// recursive; each continuing pass must have removed at least one active tile, so
// the original recursive behavior has a finite workspace-sized upper bound.
static void ArrangeWorkspace(const DesktopMonitorKey& key) {
  if (!IsWorkspaceOnActiveDesktop(key)) return;

  WorkspaceState state;
  if (!LoadWorkspaceState(key, &state)) return;

  RECT workArea{};
  if (!GetWorkspaceWorkArea(key.monitor, &workArea)) return;

  RepairWorkspaceForArrangement(state);

  const size_t maxPasses = state.windows.size() + 1;
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    if (state.layout == TileLayout::Floating || state.windows.empty()) {
      SaveWorkspaceState(key, state);
      return;
    }

    std::vector<RECT> rects;
    if (!BuildWorkspaceLayoutPlan(state, workArea, &rects)) {
      SaveWorkspaceState(key, state);
      return;
    }

    PlacementBatchResult batch = ApplyWorkspacePlacement(state, rects);
    const bool participationChanged = ApplyPlacementConsequences(state, batch);
    SaveWorkspaceState(key, state);

    if (!participationChanged || state.windows.empty()) return;
  }
}

// Uses aggregate work-area coverage as a coarse guard against learning arbitrary
// floating geometry as an authoritative tiled layout.
static bool SnapshotLooksTiled(const std::vector<HWND>& windows, const RECT& workArea) {
  if (windows.empty()) return false;
  const long long workAreaArea = RectAreaLL(workArea);
  long long sumArea = 0;
  for (HWND w : windows) sumArea += WindowAreaOnWorkArea(w, workArea);
  const long long lo = (workAreaArea * 85) / 100;
  const long long hi = (windows.size() <= 2) ? (workAreaArea * 115) / 100 : (workAreaArea * 105) / 100;
  return sumArea >= lo && sumArea <= hi;
}

static bool SameWorkspaceKey(const DesktopMonitorKey& a, const DesktopMonitorKey& b);

//=============================================================================
// Workspace reconciliation
//=============================================================================

enum class ReconcileScope : uint8_t {
  None = 0,
  Participation = 1 << 0,
  Desktop = 1 << 1,
  Monitor = 1 << 2,
  All = (1 << 0) | (1 << 1) | (1 << 2),
};

constexpr ReconcileScope operator|(ReconcileScope a, ReconcileScope b) {
  return static_cast<ReconcileScope>(
      static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool HasReconcileScope(ReconcileScope value, ReconcileScope flag) {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

static void ReconcileKnownWindows(
    ReconcileScope scope, std::vector<DesktopMonitorKey>* changedKeys,
    HWND onlyHwnd = nullptr);
static bool EnsureUniqueWindowOwnership(
    HWND hwnd, const DesktopMonitorKey& targetKey,
    std::vector<DesktopMonitorKey>* changedKeys = nullptr);

// Reconciles a monitor snapshot into the current workspace while preserving known
// order and inactive records. Explicit adoption can re-tile floated windows and
// learn complete tiled geometry before persisting and arranging the result.
static bool EnsureWorkspaceFromSnapshot(
    HMONITOR monitor, bool adoptCurrentGeometry,
    std::vector<DesktopMonitorKey>* ownershipChangedKeys = nullptr) {
  DesktopMonitorKey key{};
  RECT workArea{};
  if (!GetCurrentWorkspaceKey(monitor, &key, &workArea)) return false;

  std::vector<HWND> snapshot = CollectTileWindows(monitor);

  // Decide whether this is genuine workspace initialization before ownership
  // repair can migrate an existing record here and implicitly create the state.
  WorkspaceState state;
  const bool workspaceExisted = LoadWorkspaceState(key, &state);
  std::vector<HWND> initialSnapshot;
  if (!workspaceExisted) {
    initialSnapshot = CollectWorkspaceWindowsForInitialization(monitor, key.desktopId);
  }

  // For an existing workspace, the active snapshot remains the authoritative
  // ownership signal. During first creation use the broader initialization set
  // so tracked minimized/maximized HWNDs can migrate without being duplicated.
  const std::vector<HWND>& ownershipSnapshot =
      workspaceExisted ? snapshot : initialSnapshot;
  for (HWND w : ownershipSnapshot) {
    if (IsWindowTrackedInAnyState(w)) {
      EnsureUniqueWindowOwnership(w, key, ownershipChangedKeys);
    }
  }

  // Ownership migration above may have created/changed this workspace, so reload
  // before making local edits. workspaceExisted still records whether this call
  // is the initialization boundary.
  bool hadState = LoadWorkspaceState(key, &state);
  if (!hadState) {
    state.layout = g_settings.defaultLayout;
    state.masterRatio = ClampDouble(g_settings.masterPercent / 100.0, 0.1, 0.9);
  }

  if (!workspaceExisted) {
    for (HWND w : initialSnapshot) {
      AddInitialWorkspaceWindow(state, w);
    }
  }

  // Manual Tile is an explicit repair/adoption operation. A visible suspended
  // record can be restored here if its state event was missed, but absence from
  // this discovery snapshot never ejects a known tiled window.
  for (HWND w : snapshot) {
    WindowRecord* record = FindWindowRecord(state, w);
    if (record && record->state == ManageState::Suspended) {
      RestoreWindowInState(state, w);
    }
  }

  // Manual Tile explicitly re-adopts individually floated windows. Automatic
  // reconciliation never does this, so a user drag exclusion remains stable.
  if (adoptCurrentGeometry) {
    for (HWND w : snapshot) {
      WindowRecord* record = FindWindowRecord(state, w);
      if (record && record->state == ManageState::Floating) {
        ActivateWindowAsTiled(state, w, true);
      }
    }
  }

  // Preserve known membership/order and append truly new windows.
  for (HWND w : snapshot) {
    if (state.records.find(w) == state.records.end() &&
        !IsWindowTrackedInAnyState(w)) {
      ActivateWindowAsTiled(state, w);
    }
  }

  bool hasSuspended = false;
  for (const auto& kv : state.records) {
    if (kv.second.state == ManageState::Suspended) {
      hasSuspended = true;
      break;
    }
  }

  bool allTiledVisible = std::all_of(
      state.windows.begin(), state.windows.end(),
      [&](HWND w) { return ContainsWindow(snapshot, w); });

  // Geometry capture remains explicit/manual. Only learn from a complete visible
  // tiled set; a temporarily hidden/cloaked known window keeps its slot instead.
  if (adoptCurrentGeometry && state.layout != TileLayout::Floating &&
      !hasSuspended && allTiledVisible &&
      SnapshotLooksTiled(state.windows, workArea)) {
    HWND preferredMaster = state.windows.empty() ? nullptr : state.windows.front();
    WorkspaceState captured =
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

  SaveWorkspaceState(key, state);
  ArrangeWorkspace(key);
  return true;
}

// Manual tile: reconcile membership and explicitly adopt the user's current
// geometry. Automatic events never call this with adoptCurrentGeometry=true.
void TileWindows() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;

  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(ReconcileScope::All, &changedKeys);

  DesktopMonitorKey targetKey{};
  GetCurrentWorkspaceKey(monitor, &targetKey);
  if (!EnsureWorkspaceFromSnapshot(monitor, true, &changedKeys)) return;

  // Ensure other affected current workspaces also lose stale slots. Keep the
  // target workspace until after geometry adoption so Alt+D does not overwrite
  // the user's geometry before capturing it.
  for (const auto& key : changedKeys) {
    if (!SameWorkspaceKey(key, targetKey)) ArrangeWorkspace(key);
  }

  TrayUi::RefreshForCurrentWorkspace();
  Wh_Log(L"Manual tile/global reconcile completed");
}

// A managed HWND is allowed to have exactly one logical workspace owner. Keep
// the scan explicit instead of adding a second ownership cache that could drift
// out of sync with g_workspaces.states.
static std::vector<DesktopMonitorKey> FindWorkspaceKeysContainingWindow(HWND hwnd) {
  std::vector<DesktopMonitorKey> keys;
  if (!hwnd) return keys;

  AcquireSRWLockShared(&g_workspaces.lock);
  for (const auto& kv : g_workspaces.states) {
    if (kv.second.records.find(hwnd) != kv.second.records.end()) {
      keys.push_back(kv.first);
    }
  }
  ReleaseSRWLockShared(&g_workspaces.lock);
  return keys;
}

static void ClearMoveSizeSamples(HWND hwnd) {
  AcquireSRWLockExclusive(&g_moveSize.lock);
  g_moveSize.startRects.erase(hwnd);
  g_moveSize.endRects.erase(hwnd);
  g_moveSize.endPoints.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSize.lock);
}

// Removes an HWND from every recorded owner, reflows each affected workspace,
// and clears any captured move/size samples.
static void ForgetManagedWindow(HWND hwnd) {
  std::vector<DesktopMonitorKey> keys = FindWorkspaceKeysContainingWindow(hwnd);
  for (const auto& key : keys) {
    WorkspaceState state;
    if (!LoadWorkspaceState(key, &state)) continue;
    if (ForgetWindowInState(state, hwnd)) {
      SaveWorkspaceState(key, state); // Keep empty workspace state and its layout mode.
      ArrangeWorkspace(key);
    }
  }

  ClearMoveSizeSamples(hwnd);
}

static bool SameWorkspaceKey(const DesktopMonitorKey& a, const DesktopMonitorKey& b) {
  return a.monitor == b.monitor && IsEqualGUID(a.desktopId, b.desktopId);
}

static void AddUniqueWorkspaceKey(
    std::vector<DesktopMonitorKey>& keys, const DesktopMonitorKey& key) {
  if (!key.monitor) return;
  if (std::none_of(keys.begin(), keys.end(),
                   [&](const DesktopMonitorKey& existing) {
                     return SameWorkspaceKey(existing, key);
                   })) {
    keys.push_back(key);
  }
}

static size_t LogicalWindowCount(const WorkspaceState& state) {
  size_t count = state.windows.size();
  for (const auto& kv : state.records) {
    if (kv.second.state == ManageState::Suspended && kv.second.hasSavedSlot) ++count;
  }
  return count;
}

// Add a migrated tiled window at the logical end without disturbing the
// destination workspace's existing resize weights. The new window alone gets
// the neutral default weight where the layout has per-window stack/grid weights.
static void AppendTiledWindowWithDefaultWeight(WorkspaceState& state, HWND hwnd) {
  EnsureSaneWeightVector(state);
  const size_t oldCount = state.windows.size();
  state.windows.push_back(hwnd);

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    state.gridWeights.push_back(1.0);
  } else if ((state.layout == TileLayout::MasterStack ||
              state.layout == TileLayout::MasterStackH) &&
             oldCount > 0) {
    state.stackWeights.push_back(1.0);
  }
}

static SuspendedSlot MakeAppendedSuspendedSlot(const WorkspaceState& state) {
  SuspendedSlot slot{};
  slot.index = LogicalWindowCount(state);

  if (state.layout == TileLayout::Columns || state.layout == TileLayout::Rows) {
    slot.weight = 1.0;
    slot.hasWeight = true;
  } else if (state.layout == TileLayout::MasterStack ||
             state.layout == TileLayout::MasterStackH) {
    if (slot.index == 0) {
      slot.wasMaster = true;
    } else {
      slot.weight = 1.0;
      slot.hasWeight = true;
    }
  }

  return slot;
}

// Transfers a managed record between workspaces, rebasing tiled or suspended
// membership at the destination end. Both states are saved; callers own reflow.
static bool MigrateManagedWindow(
    HWND hwnd, const DesktopMonitorKey& sourceKey, const DesktopMonitorKey& targetKey) {
  if (!hwnd || SameWorkspaceKey(sourceKey, targetKey)) return false;

  WorkspaceState source;
  if (!LoadWorkspaceState(sourceKey, &source)) return false;
  auto sourceRecord = source.records.find(hwnd);
  if (sourceRecord == source.records.end()) return false;

  WindowRecord record = sourceRecord->second;
  if (!ForgetWindowInState(source, hwnd)) return false;
  SaveWorkspaceState(sourceKey, source);

  WorkspaceState target;
  if (!LoadWorkspaceState(targetKey, &target)) {
    target.layout = g_settings.defaultLayout;
    target.masterRatio = ClampDouble(g_settings.masterPercent / 100.0, 0.1, 0.9);
  }

  // Defensive duplicate cleanup if an older reconciliation path left the HWND
  // registered in both workspaces.
  ForgetWindowInState(target, hwnd);

  if (record.state == ManageState::Tiled) {
    record.suspensionReason = SuspensionReason::None;
    record.savedSlot = {};
    record.hasSavedSlot = false;
    AppendTiledWindowWithDefaultWeight(target, hwnd);
  } else if (record.state == ManageState::Suspended) {
    record.savedSlot = MakeAppendedSuspendedSlot(target);
    record.hasSavedSlot = true;
  }
  target.records[hwnd] = record;
  EnsureSaneWeightVector(target);
  SaveWorkspaceState(targetKey, target);
  return true;
}

// Reconcile one tracked HWND to a single logical workspace. This mirrors the
// mature WM model: physical ownership is queried/observed globally, while the
// layout tree keeps one logical owner. If an older path already duplicated the
// HWND, prefer the record that is already in the observed workspace and remove
// every stale copy.
static bool EnsureUniqueWindowOwnership(
    HWND hwnd, const DesktopMonitorKey& targetKey,
    std::vector<DesktopMonitorKey>* changedKeys) {
  std::vector<DesktopMonitorKey> owners = FindWorkspaceKeysContainingWindow(hwnd);
  if (owners.empty()) return false;

  const bool targetAlreadyOwns = std::any_of(
      owners.begin(), owners.end(), [&](const DesktopMonitorKey& key) {
        return SameWorkspaceKey(key, targetKey);
      });

  bool changed = false;
  if (!targetAlreadyOwns) {
    const DesktopMonitorKey sourceKey = owners.front();
    if (!MigrateManagedWindow(hwnd, sourceKey, targetKey)) {
      Wh_Log(L"Failed to reconcile HWND %p to observed workspace", hwnd);
      return false;
    }

    changed = true;
    if (changedKeys) {
      AddUniqueWorkspaceKey(*changedKeys, sourceKey);
      AddUniqueWorkspaceKey(*changedKeys, targetKey);
    }
  }

  // MigrateManagedWindow removes its selected source and any pre-existing
  // destination record, but an old bug may have left more than two copies.
  // Remove every remaining non-target owner explicitly.
  for (const auto& ownerKey : owners) {
    if (SameWorkspaceKey(ownerKey, targetKey)) continue;

    WorkspaceState ownerState;
    if (!LoadWorkspaceState(ownerKey, &ownerState)) continue;
    if (ForgetWindowInState(ownerState, hwnd)) {
      SaveWorkspaceState(ownerKey, ownerState);
      changed = true;
      if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
    }
  }

  if (owners.size() > 1) {
    Wh_Log(L"Repaired duplicate HWND ownership for %p (%zu logical owners)",
           hwnd, owners.size());
  }

  return changed;
}

// Refreshes only the requested parts of a fallback key from the HWND's physical
// desktop and monitor, succeeding when at least one requested observation is valid.
static bool GetObservedWorkspaceKey(
    HWND hwnd, const DesktopMonitorKey& fallback, bool reconcileDesktop,
    bool reconcileMonitor, DesktopMonitorKey* outKey) {
  if (!outKey) return false;

  DesktopMonitorKey observed = fallback;
  bool observedAnything = false;

  if (reconcileDesktop) {
    GUID desktopId{};
    if (GetWindowDesktopIdSafe(hwnd, &desktopId)) {
      observed.desktopId = desktopId;
      observedAnything = true;
    }
  }

  if (reconcileMonitor) {
    HMONITOR monitor = nullptr;
    RECT frame{};
    if (GetWindowFrameRect(hwnd, &frame) &&
        frame.right > frame.left && frame.bottom > frame.top) {
      monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONULL);
    }
    if (!monitor) monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (monitor) {
      observed.monitor = monitor;
      observedAnything = true;
    }
  }

  if (!observedAnything || !observed.monitor) return false;
  *outKey = observed;
  return true;
}

// Dirty-check all known HWNDs. Generic lifecycle signals repair only liveness,
// exclusions and settled participation. Desktop and monitor ownership are
// reconciled only when the caller has an authoritative reason to query them.
static void ReconcileKnownWindows(
    ReconcileScope scope, std::vector<DesktopMonitorKey>* changedKeys,
    HWND onlyHwnd) {
  const bool settleParticipation =
      HasReconcileScope(scope, ReconcileScope::Participation);
  const bool reconcileDesktop = HasReconcileScope(scope, ReconcileScope::Desktop);
  const bool reconcileMonitor = HasReconcileScope(scope, ReconcileScope::Monitor);
  std::vector<HWND> known;

  if (onlyHwnd) {
    known.push_back(onlyHwnd);
  } else {
    AcquireSRWLockShared(&g_workspaces.lock);
    for (const auto& kv : g_workspaces.states) {
      for (const auto& record : kv.second.records) {
        if (std::find(known.begin(), known.end(), record.first) == known.end()) {
          known.push_back(record.first);
        }
      }
    }
    ReleaseSRWLockShared(&g_workspaces.lock);
  }

  for (HWND hwnd : known) {
    std::vector<DesktopMonitorKey> ownerKeys = FindWorkspaceKeysContainingWindow(hwnd);
    if (ownerKeys.empty()) continue;

    if (!IsWindow(hwnd) || IsWindowExcludedByRules(hwnd)) {
      for (const auto& ownerKey : ownerKeys) {
        WorkspaceState ownerState;
        if (!LoadWorkspaceState(ownerKey, &ownerState)) continue;
        if (ForgetWindowInState(ownerState, hwnd)) {
          SaveWorkspaceState(ownerKey, ownerState);
          if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
        }
      }
      ClearMoveSizeSamples(hwnd);
      continue;
    }

    // Events are dirty signals; ownership is decided by querying physical
    // reality. When the query succeeds, collapse any stale duplicate records to
    // the observed desktop/monitor instead of letting discovery add another copy.
    if (reconcileDesktop || reconcileMonitor) {
      DesktopMonitorKey observedKey{};
      if (GetObservedWorkspaceKey(
              hwnd, ownerKeys.front(), reconcileDesktop, reconcileMonitor,
              &observedKey)) {
        EnsureUniqueWindowOwnership(hwnd, observedKey, changedKeys);
        ownerKeys = FindWorkspaceKeysContainingWindow(hwnd);
        if (ownerKeys.empty()) continue;
      }
    }

    if (!settleParticipation) continue;

    // Participation is a queried physical state, not an interpretation of the
    // raw WinEvent. DWM cloak remains deliberately irrelevant: VD transitions
    // can cloak a perfectly valid managed window without changing its logical
    // membership. Hidden, minimized, and maximized windows preserve their slot.
    const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
    for (const auto& ownerKey : ownerKeys) {
      WorkspaceState ownerState;
      if (!LoadWorkspaceState(ownerKey, &ownerState)) continue;

      WindowRecord* record = FindWindowRecord(ownerState, hwnd);
      if (!record) continue;

      bool changed = false;
      if (record->state == ManageState::Tiled &&
          reason != SuspensionReason::None) {
        changed = SuspendWindowInState(ownerState, hwnd, reason);
      } else if (record->state == ManageState::Suspended) {
        if (reason == SuspensionReason::None) {
          changed = RestoreWindowInState(ownerState, hwnd);
        } else {
          // State transitions can arrive repeatedly or in quick succession
          // (e.g. Hidden -> Maximized). Keep the saved slot, update only why it
          // is currently non-participating.
          changed = SuspendWindowInState(ownerState, hwnd, reason);
        }
      }

      if (changed) {
        SaveWorkspaceState(ownerKey, ownerState);
        if (changedKeys) AddUniqueWorkspaceKey(*changedKeys, ownerKey);
      }
    }
  }
}

static void AddUniqueMonitor(std::vector<HMONITOR>& monitors, HMONITOR monitor) {
  if (monitor && std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
    monitors.push_back(monitor);
  }
}

static void AddPendingWorkspaceArrange(const GUID& desktopId, HMONITOR monitor) {
  if (!monitor) return;
  // Only if not scheduled already
  auto it = std::find_if(
      g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
      [&](const PendingWorkspaceArrange& pending) {
        return pending.monitor == monitor && IsEqualGUID(pending.desktopId, desktopId);
      });
  if (it == g_wm.pendingDesktopArranges.end()) {
    g_wm.pendingDesktopArranges.push_back({desktopId, monitor});
  }
}

static void ScheduleLifecycleReconcile(HWND hwnd);

// Current-desktop discovery has two separate jobs: reconcile already-managed
// HWND ownership to the observed workspace, and optionally admit truly new
// windows while Automatic mode is active.
static bool DiscoverWorkspaceWindows(
    HMONITOR monitor, const GUID& desktopId, const std::vector<HWND>& snapshot,
    DesktopMonitorKey* outKey,
    std::vector<DesktopMonitorKey>* ownershipChangedKeys = nullptr) {
  if (!monitor) return false;

  DesktopMonitorKey key{desktopId, monitor};
  if (outKey) *outKey = key;

  WorkspaceState state;
  const bool workspaceExisted = LoadWorkspaceState(key, &state);
  const bool admitUntracked = !g_wm.autoDiscoverySuspended;
  const bool initializing = !workspaceExisted && admitUntracked;
  std::vector<HWND> initialSnapshot;
  if (initializing) {
    initialSnapshot = CollectWorkspaceWindowsForInitialization(monitor, desktopId);
  }

  // Reconcile all tracked HWNDs first, before making any unsaved local edits.
  // A brand-new workspace uses the broader initialization set so a minimized or
  // maximized tracked HWND can migrate even though it is not an active tile.
  bool changed = false;
  const std::vector<HWND>& ownershipSnapshot =
      initializing ? initialSnapshot : snapshot;
  for (HWND w : ownershipSnapshot) {
    if (IsWindowTrackedInAnyState(w) &&
        EnsureUniqueWindowOwnership(w, key, ownershipChangedKeys)) {
      changed = true;
    }
  }

  // Migration may have created the destination workspace; reload after ownership
  // reconciliation, then seed only truly untracked initialization candidates.
  bool hasState = LoadWorkspaceState(key, &state);
  if (!hasState) {
    state.layout = g_settings.defaultLayout;
    state.masterRatio = ClampDouble(g_settings.masterPercent / 100.0, 0.1, 0.9);
  }

  if (initializing) {
    for (HWND w : initialSnapshot) {
      if (AddInitialWorkspaceWindow(state, w)) {
        hasState = true;
        changed = true;
      }
    }
  }

  // Reconcile already-managed windows independently of automatic discovery.
  // Being present in this current-desktop snapshot is authoritative evidence
  // for the window's logical workspace, just like FancyWM's desktop.HasWindow
  // reconciliation and komorebi's Show/Uncloak reconciliation.
  for (HWND w : snapshot) {
    WindowRecord* record = FindWindowRecord(state, w);
    if (record) {
      // The snapshot contains only visible, restored windows. If this HWND was
      // suspended while it was on another desktop, reactivate its saved slot in
      // the now-authoritative workspace.
      if (record->state == ManageState::Suspended &&
          RestoreWindowInState(state, w)) {
        changed = true;
      }
      continue;
    }

    // A tracked HWND that is still absent from the target state means ownership
    // reconciliation could not complete. Never create a second logical copy.
    if (IsWindowTrackedInAnyState(w)) continue;

    if (admitUntracked && ActivateWindowAsTiled(state, w)) {
      hasState = true;
      changed = true;
    }
  }

  if (!hasState || !changed) return false;
  EnsureSaneWeightVector(state);
  SaveWorkspaceState(key, state);
  return true;
}

// Runs the normal discovery path for one monitor. The pipeline is always active;
// DiscoverWorkspaceWindows() alone decides whether untracked HWND admission is
// currently suspended by Manual mode.
static bool DiscoverCurrentWorkspaceOnMonitor(HMONITOR monitor) {
  if (!monitor) return false;

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    return false;
  }

  std::vector<HWND> snapshot = CollectTileWindows(monitor);
  DesktopMonitorKey key{};
  if (!DiscoverWorkspaceWindows(monitor, currentDesktop, snapshot, &key)) {
    return false;
  }

  ArrangeWorkspace(key);
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

// During a virtual-desktop switch, DWM cloaks and uncloaks windows one at a time.
// Snapshot otherwise eligible windows by desktop GUID while deliberately ignoring
// that transient cloak bit; settled reconciliation handles genuine cloaks later.
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

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return TRUE;
        if (!WindowCanBeManaged(hwnd)) return TRUE;
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
// signal. The cloak-insensitive snapshot is used only to discover new incoming
// windows; known WindowRecord membership is never rebuilt from shell visibility.
static void HandleVirtualDesktopChanged() {
  InterlockedExchange(&g_vd.changeQueued, 0);

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    Wh_Log(L"Native VD change received but current desktop query failed");
    ScheduleLifecycleReconcile(nullptr);
    return;
  }

  // A native VD transition is an authoritative reason to repair desktop ownership.
  // Do not infer monitor migration here; defer visibility/window-state participation
  // until the shell's cloak/show burst has settled.
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(ReconcileScope::Desktop, &changedKeys);
  for (const auto& key : changedKeys) {
    if (IsEqualGUID(key.desktopId, currentDesktop)) {
      AddPendingWorkspaceArrange(key.desktopId, key.monitor);
    }
  }

  std::vector<HMONITOR> monitors;
  EnumerateCurrentMonitors(monitors);

  // FancyWM refreshes ownership and then unconditionally invalidates the incoming
  // desktop layout. Do the equivalent here, but defer actual placement until the
  // shell's cloak/uncloak burst settles. Existing workspaces are reinforced even
  // when their bookkeeping did not change.
  for (HMONITOR monitor : monitors) {
    DesktopMonitorKey knownKey{currentDesktop, monitor};
    WorkspaceState knownState;
    if (LoadWorkspaceState(knownKey, &knownState)) {
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  for (HMONITOR monitor : monitors) {
    std::vector<HWND> snapshot = GetTileWindowsAfterDesktopSwitch(monitor, currentDesktop);
    DesktopMonitorKey key{};
    if (DiscoverWorkspaceWindows(
            monitor, currentDesktop, snapshot, &key)) {
      // New/migrated membership is also reinforced by the same settled pass.
      AddPendingWorkspaceArrange(currentDesktop, monitor);
    }
  }

  // Coalesce the remaining shell burst for discovery/fallback and the unconditional
  // incoming-desktop reinforcement. The settled pass may also repair physical state.
  ScheduleLifecycleReconcile(nullptr);
  TrayUi::RefreshForCurrentWorkspace();
}

// Coalesce shell visibility bursts for discovery and deferred placement. Generic
// lifecycle events are dirty signals: after the settle delay we may re-query
// desktop ownership for the affected tracked HWND, but never infer ownership
// directly from HIDE/SHOW/CLOAK itself.
static void ScheduleLifecycleReconcile(HWND hwnd) {
  // HotkeyThreadProc owns this vector/timer; no additional lock is required.
  if (hwnd && std::find(g_wm.lifecycleDirtyWindows.begin(), g_wm.lifecycleDirtyWindows.end(), hwnd) ==
                  g_wm.lifecycleDirtyWindows.end()) {
    g_wm.lifecycleDirtyWindows.push_back(hwnd);
  }

  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }

  g_wm.lifecycleTimer = SetTimer(nullptr, 0, g_settings.reconcileDelayMs, nullptr);
  if (!g_wm.lifecycleTimer) {
    Wh_Log(L"Failed to schedule lifecycle reconciliation");
  }
}

// After a lifecycle burst settles, reconcile tracked-window physical and desktop
// state, discover eligible windows only on the current desktop, then arrange each
// changed or pending workspace once and refresh the tray.
static void ReconcileDeferredLifecycle() {
  if (g_wm.lifecycleTimer) {
    KillTimer(nullptr, g_wm.lifecycleTimer);
    g_wm.lifecycleTimer = 0;
  }

  std::vector<HWND> dirtyWindows;
  dirtyWindows.swap(g_wm.lifecycleDirtyWindows);

  // First repair liveness/exclusions and settled hidden/minimized/maximized participation
  // globally. This also suspends every stale logical copy defensively if an old
  // version already duplicated an HWND.
  std::vector<DesktopMonitorKey> globallyChanged;
  ReconcileKnownWindows(ReconcileScope::Participation, &globallyChanged);

  // WinMan/FancyWM treat visibility events as prompts to dirty-check physical
  // state rather than as membership decisions. Do the same, scoped only to the
  // tracked HWNDs implicated by this burst. A per-window VD move does not emit
  // CurrentVirtualDesktopChanged, so this is the recovery path for that case.
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindowTrackedInAnyState(hwnd)) continue;
    ReconcileKnownWindows(ReconcileScope::Desktop, &globallyChanged, hwnd);
  }

  GUID currentDesktop{};
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&currentDesktop)) {
    Wh_Log(L"Lifecycle discovery skipped: current virtual desktop unavailable");
    return;
  }

  std::vector<HMONITOR> monitors;

  // Discover only on the current desktop after global known-window repair.
  AcquireSRWLockShared(&g_workspaces.lock);
  for (const auto& kv : g_workspaces.states) {
    if (IsEqualGUID(kv.first.desktopId, currentDesktop)) {
      AddUniqueMonitor(monitors, kv.first.monitor);
    }
  }
  ReleaseSRWLockShared(&g_workspaces.lock);

  // Also include monitors implicated by the shell burst so automatic discovery
  // can seed a new desktop+monitor workspace.
  for (HWND hwnd : dirtyWindows) {
    if (!hwnd || !IsWindow(hwnd)) continue;

    // GetWindowDesktopId can be briefly unavailable while a window is being
    // reassigned. Prefer the public current-desktop predicate here; if even that
    // is inconclusive, including the monitor is harmless because CollectTileWindows
    // performs the authoritative eligibility/current-desktop filter afterward.
    if (g_vd.desktopManager) {
      BOOL onCurrent = FALSE;
      if (SUCCEEDED(g_vd.desktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) &&
          !onCurrent) {
        continue;
      }
    }
    AddUniqueMonitor(monitors, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL));
  }

  std::vector<DesktopMonitorKey> arrangedKeys;
  for (HMONITOR monitor : monitors) {
    DesktopMonitorKey key{};
    std::vector<HWND> snapshot = CollectTileWindows(monitor);
    bool changed = DiscoverWorkspaceWindows(
        monitor, currentDesktop, snapshot, &key, &globallyChanged);
    bool pendingArrange = std::any_of(
        g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == monitor &&
                 IsEqualGUID(pending.desktopId, currentDesktop);
        });
    if (changed || pendingArrange) {
      ArrangeWorkspace(key);
      AddUniqueWorkspaceKey(arrangedKeys, key);
    }
  }

  for (const auto& key : globallyChanged) {
    if (std::none_of(arrangedKeys.begin(), arrangedKeys.end(),
                     [&](const DesktopMonitorKey& arranged) {
                       return SameWorkspaceKey(arranged, key);
                     })) {
      ArrangeWorkspace(key);
    }
  }

  g_wm.pendingDesktopArranges.erase(
      std::remove_if(
          g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
          [&](const PendingWorkspaceArrange& pending) {
            return IsEqualGUID(pending.desktopId, currentDesktop);
          }),
      g_wm.pendingDesktopArranges.end());

  TrayUi::RefreshForCurrentWorkspace();
}

static void ReconcileManagedWindowStateNow(HWND hwnd) {
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(ReconcileScope::Participation, &changedKeys, hwnd);
  for (const auto& key : changedKeys) ArrangeWorkspace(key);
}

// Converts queued lifecycle events into model actions: destruction is authoritative,
// state changes get immediate and settled reconciliation, while visibility and
// cloak events remain dirty signals until shell state stabilizes.
static void ProcessWindowLifecycleEvent(DWORD event, HWND hwnd) {
  switch (event) {
    case EVENT_OBJECT_DESTROY:
      ForgetManagedWindow(hwnd);
      break;

    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_STATECHANGE:
    case EVENT_OBJECT_LOCATIONCHANGE:
      // FancyWM/WinMan treat these as prompts to query the actual window state. Do the
      // same: the event says "dirty", while IsIconic/IsZoomed/visibility decide
      // whether the saved logical slot participates. Run once immediately for a
      // responsive reflow and once after settle to cover transition races.
      ReconcileManagedWindowStateNow(hwnd);
      ScheduleLifecycleReconcile(hwnd);
      break;

    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
      // These are dirty signals, not ownership decisions. The settled lifecycle
      // pass re-queries physical state and may repair this tracked HWND's VD.
      ScheduleLifecycleReconcile(hwnd);
      break;
  }
}

// Advances the current workspace layout and invalidates layout-specific hints in
// suspended slots before persisting, arranging, and updating UI feedback.
static void CycleCurrentWorkspaceLayout() {
  HMONITOR monitor = GetTargetMonitor();
  if (!monitor) return;
  DesktopMonitorKey key{};
  if (!GetCurrentWorkspaceKey(monitor, &key)) return;

  WorkspaceState state;
  if (!LoadWorkspaceState(key, &state)) {
    if (!EnsureWorkspaceFromSnapshot(monitor, false) || !LoadWorkspaceState(key, &state)) return;
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
  SaveWorkspaceState(key, state);
  ArrangeWorkspace(key);
  TrayUi::UpdateIcon(state.layout);
  TrayUi::ShowLayoutFlyout(state.layout);
  Wh_Log(L"Workspace layout changed to %d", static_cast<int>(state.layout));
}

struct MoveSizeGesture {
  MoveSizeIntent intent = MoveSizeIntent::None;
  RECT start{};
  RECT end{};
  POINT dropPoint{};
  bool hasRects = false;
  bool hasDropPoint = false;
};

// Atomically consumes and clears the WinEvent samples for one completed gesture.
// The WM core never reads these caches piecemeal after this boundary.
static MoveSizeGesture ConsumeMoveSizeGesture(HWND hwnd) {
  MoveSizeGesture gesture;

  AcquireSRWLockExclusive(&g_moveSize.lock);
  auto startIt = g_moveSize.startRects.find(hwnd);
  auto endIt = g_moveSize.endRects.find(hwnd);
  auto pointIt = g_moveSize.endPoints.find(hwnd);

  if (startIt != g_moveSize.startRects.end() && endIt != g_moveSize.endRects.end()) {
    gesture.start = startIt->second;
    gesture.end = endIt->second;
    gesture.hasRects = true;
    gesture.intent = ClassifyMoveSizeIntent(gesture.start, gesture.end, 1);
  }
  if (pointIt != g_moveSize.endPoints.end()) {
    gesture.dropPoint = pointIt->second;
    gesture.hasDropPoint = true;
  }

  g_moveSize.startRects.erase(hwnd);
  g_moveSize.endRects.erase(hwnd);
  g_moveSize.endPoints.erase(hwnd);
  ReleaseSRWLockExclusive(&g_moveSize.lock);
  return gesture;
}

constexpr LONG kResizeEdgeDeltaTolerance = 1;
constexpr LONG kMinLearnedResizeSpan = 80;

static bool IsMeaningfulResizeDelta(LONG delta) {
  return std::abs(delta) > kResizeEdgeDeltaTolerance;
}

// Transfers one divider delta between adjacent logical weights while preserving
// their combined weight. Expected layout rectangles provide the pre-drag spans;
// live sibling geometry is intentionally never learned into durable state.
static bool TransferPairResizeDelta(
    std::vector<double>& weights, size_t firstWeight,
    const RECT& firstRect, const RECT& secondRect,
    bool verticalAxis, LONG delta) {
  if (!IsMeaningfulResizeDelta(delta) || firstWeight + 1 >= weights.size()) {
    return false;
  }

  const LONG firstSpan = verticalAxis ? firstRect.bottom - firstRect.top
                                      : firstRect.right - firstRect.left;
  const LONG secondSpan = verticalAxis ? secondRect.bottom - secondRect.top
                                       : secondRect.right - secondRect.left;
  const LONG pairSpan = firstSpan + secondSpan;
  if (firstSpan < 1 || secondSpan < 1 || pairSpan <= 1) return false;

  const LONG minSpan = std::min<LONG>(
      kMinLearnedResizeSpan, std::max<LONG>(1, (pairSpan - 1) / 2));
  const LONG maxFirstSpan = pairSpan - minSpan;
  const LONG newFirstSpan =
      std::clamp<LONG>(firstSpan + delta, minSpan, maxFirstSpan);
  if (newFirstSpan == firstSpan) return false;

  double pairWeight = weights[firstWeight] + weights[firstWeight + 1];
  if (!(pairWeight > 0.0) || !std::isfinite(pairWeight)) pairWeight = 2.0;

  const double firstFraction =
      static_cast<double>(newFirstSpan) / static_cast<double>(pairSpan);
  weights[firstWeight] = pairWeight * firstFraction;
  weights[firstWeight + 1] = pairWeight - weights[firstWeight];
  return true;
}

static bool LearnMasterStackResize(
    WorkspaceState& state, const RECT& workArea, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  const bool horizontal = state.layout == TileLayout::MasterStackH;
  std::vector<RECT> expectedRects;
  LayoutMasterStackWeighted(
      workArea, state.windows.size(), expectedRects, horizontal,
      ClampDouble(state.masterRatio, 0.1, 0.9), state.stackWeights);
  if (expectedRects.size() != state.windows.size()) return false;

  bool changed = false;

  // Main master/stack divider.
  const LONG mainDelta = horizontal
      ? (resizedIndex == 0 ? gesture.end.bottom - gesture.start.bottom
                           : gesture.end.top - gesture.start.top)
      : (resizedIndex == 0 ? gesture.end.right - gesture.start.right
                           : gesture.end.left - gesture.start.left);

  if (state.windows.size() > 1 && IsMeaningfulResizeDelta(mainDelta)) {
    const LONG totalSize = horizontal ? workArea.bottom - workArea.top
                                      : workArea.right - workArea.left;
    const LONG usableSize = totalSize - g_settings.gap;
    if (usableSize > 1) {
      const LONG oldMasterSpan = horizontal
          ? expectedRects[0].bottom - expectedRects[0].top
          : expectedRects[0].right - expectedRects[0].left;
      const LONG newMasterSpan = std::clamp<LONG>(
          oldMasterSpan + mainDelta, 1, usableSize - 1);
      const double newRatio = ClampDouble(
          static_cast<double>(newMasterSpan) / static_cast<double>(usableSize),
          0.1, 0.9);
      if (std::abs(newRatio - state.masterRatio) > 1e-9) {
        state.masterRatio = newRatio;
        changed = true;
      }
    }
  }

  // Stack-axis dividers.
  if (resizedIndex > 0 && state.stackWeights.size() + 1 == state.windows.size()) {
    const size_t stackIndex = resizedIndex - 1;
    const LONG leadingDelta = horizontal
        ? gesture.end.left - gesture.start.left
        : gesture.end.top - gesture.start.top;
    const LONG trailingDelta = horizontal
        ? gesture.end.right - gesture.start.right
        : gesture.end.bottom - gesture.start.bottom;

    if (stackIndex > 0) {
      changed |= TransferPairResizeDelta(
          state.stackWeights, stackIndex - 1,
          expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
          !horizontal, leadingDelta);
    }
    if (stackIndex + 1 < state.stackWeights.size()) {
      changed |= TransferPairResizeDelta(
          state.stackWeights, stackIndex,
          expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
          !horizontal, trailingDelta);
    }
  }

  return changed;
}

static bool LearnGridResize(
    WorkspaceState& state, const RECT& workArea, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  const bool horizontal = state.layout == TileLayout::Rows;
  std::vector<RECT> expectedRects;
  LayoutGridWeighted(
      workArea, state.windows.size(), expectedRects, horizontal, state.gridWeights);
  if (expectedRects.size() != state.windows.size()) return false;

  bool changed = false;
  const LONG leadingDelta = horizontal
      ? gesture.end.top - gesture.start.top
      : gesture.end.left - gesture.start.left;
  const LONG trailingDelta = horizontal
      ? gesture.end.bottom - gesture.start.bottom
      : gesture.end.right - gesture.start.right;

  if (resizedIndex > 0) {
    changed |= TransferPairResizeDelta(
        state.gridWeights, resizedIndex - 1,
        expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
        horizontal, leadingDelta);
  }
  if (resizedIndex + 1 < state.windows.size()) {
    changed |= TransferPairResizeDelta(
        state.gridWeights, resizedIndex,
        expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
        horizontal, trailingDelta);
  }
  return changed;
}

static void HandleUserMove(
    const DesktopMonitorKey& key, WorkspaceState& state, HWND hwnd,
    const MoveSizeGesture& gesture) {
  if (g_settings.mouseMoveBehavior == MouseMoveBehavior::Float) {
    if (FloatWindowInState(state, hwnd)) {
      if (WindowRecord* record = FindWindowRecord(state, hwnd)) {
        record->placementFailures = 0;
      }
      SaveWorkspaceState(key, state);
      Wh_Log(L"User move floated managed window %p", hwnd);
    }
    ArrangeWorkspace(key);
    return;
  }

  auto sourceIt = std::find(state.windows.begin(), state.windows.end(), hwnd);
  auto targetIt = state.windows.end();
  if (sourceIt != state.windows.end() && gesture.hasDropPoint) {
    for (auto it = state.windows.begin(); it != state.windows.end(); ++it) {
      if (it == sourceIt) continue;
      RECT rect{};
      if (GetWindowFrameRect(*it, &rect) && PtInRect(&rect, gesture.dropPoint)) {
        targetIt = it;
        break;
      }
    }
  }

  if (sourceIt != state.windows.end() && targetIt != state.windows.end()) {
    HWND target = *targetIt;
    std::iter_swap(sourceIt, targetIt);
    SaveWorkspaceState(key, state);
    Wh_Log(L"User move swapped managed windows %p and %p", hwnd, target);
  }

  // No target means the source remains tiled and simply snaps back to its slot.
  ArrangeWorkspace(key);
}

static void HandleUserResize(
    const DesktopMonitorKey& key, WorkspaceState& state, HWND hwnd,
    const RECT& workArea, const MoveSizeGesture& gesture) {
  if (!gesture.hasRects) {
    ArrangeWorkspace(key);
    return;
  }

  auto resizedIt = std::find(state.windows.begin(), state.windows.end(), hwnd);
  if (resizedIt == state.windows.end()) {
    ArrangeWorkspace(key);
    return;
  }
  const size_t resizedIndex = static_cast<size_t>(resizedIt - state.windows.begin());

  EnsureSaneWeightVector(state);
  bool changed = false;
  switch (state.layout) {
    case TileLayout::MasterStack:
    case TileLayout::MasterStackH:
      changed = LearnMasterStackResize(state, workArea, resizedIndex, gesture);
      break;

    case TileLayout::Columns:
    case TileLayout::Rows:
      changed = LearnGridResize(state, workArea, resizedIndex, gesture);
      break;

    case TileLayout::COUNT:
      return;

    case TileLayout::BSP:
    case TileLayout::Monocle:
    case TileLayout::Floating:
      break;
  }

  if (changed) {
    EnsureSaneWeightVector(state);
    SaveWorkspaceState(key, state);
    Wh_Log(L"User resize updated local divider state for %p", hwnd);
  }

  // Unsupported outer-edge drags and clamped moves simply snap back. The normal
  // arrangement path renders any newly mutated logical state.
  ArrangeWorkspace(key);
}

// Consumes one completed user gesture and routes it to the move or resize policy.
// The top-level handler deliberately contains no layout-specific math.
void ApplyUserMoveSize(HWND hwnd) {
  if (!g_settings.enabled || !IsWindow(hwnd)) return;

  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    ClearMoveSizeSamples(hwnd);
    return;
  }

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
  if (!monitor) return;

  GUID desktopId{};
  if (!GetWindowDesktopIdSafe(hwnd, &desktopId)) return;

  DesktopMonitorKey key{desktopId, monitor};
  WorkspaceState state;
  if (!LoadWorkspaceState(key, &state) || state.layout == TileLayout::Floating) return;

  RECT workArea{};
  if (!GetWorkspaceWorkArea(monitor, &workArea)) return;

  HWND tiledHwnd = ContainsWindow(state.windows, hwnd)
      ? hwnd
      : ResolveToTiledWindow(hwnd, state.windows);
  if (!tiledHwnd) {
    Wh_Log(L"Unresolved window");
    return;
  }

  MoveSizeGesture gesture = ConsumeMoveSizeGesture(hwnd);
  switch (gesture.intent) {
    case MoveSizeIntent::None:
      ArrangeWorkspace(key);
      break;

    case MoveSizeIntent::Move:
      HandleUserMove(key, state, tiledHwnd, gesture);
      break;

    case MoveSizeIntent::Resize:
      HandleUserResize(key, state, tiledHwnd, workArea, gesture);
      break;
  }
}

void OnWindowMoveSizeEnd(HWND hwnd) {
  if (!g_settings.enabled || !IsWindow(hwnd)) return;
  if (!g_wm.threadId) return;
  if (!PostThreadMessage(g_wm.threadId, WM_APP_MOVE_SIZE_END, reinterpret_cast<WPARAM>(hwnd), 0)) {
    Wh_Log(L"Failed to queue resize event");
  }
}

static void QueueWindowEvent(DWORD event, HWND hwnd) {
  if (!g_wm.threadId || !hwnd) return;
  if (!PostThreadMessage(g_wm.threadId, WM_APP_WINDOW_EVENT,
                         static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd))) {
    Wh_Log(L"Failed to queue lifecycle event %lu", event);
  }
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
    if (g_wm.threadId) PostThreadMessage(g_wm.threadId, WM_APP_TRAY_REFRESH, 0, 0);
    return;
  }

  // For window-level events only.
  if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

  const bool tracked = IsWindowTrackedInAnyState(hwnd);

  // WinMan uses location changes as one of its dirty-check signals. Do not mirror
  // that broadly here: normal layout placement itself produces LOCATIONCHANGE.
  // The event is only needed to catch a maximized managed HWND becoming restored
  // on Windows/apps that don't deliver a useful OBJECT_STATECHANGE at that edge.
  if (event == EVENT_OBJECT_LOCATIONCHANGE) {
    if (!tracked || !IsWindowSuspendedMaximized(hwnd)) return;
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

void InstallWinEventHooks() {
  if (!g_hooks.foreground) {
    g_hooks.foreground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.foreground) Wh_Log(L"Failed to install foreground hook");
  }

  if (!g_hooks.locationChange) {
    g_hooks.locationChange = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.locationChange) Wh_Log(L"Failed to install location-change hook");
  }

  if (!g_hooks.moveSize) {
    g_hooks.moveSize = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.moveSize) Wh_Log(L"Failed to install move/size hook");
  }

  if (!g_hooks.minimize) {
    g_hooks.minimize = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.minimize) Wh_Log(L"Failed to install minimization hook");
  }

  if (!g_hooks.hideDestroy) {
    g_hooks.hideDestroy = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hooks.hideDestroy) Wh_Log(L"Failed to install hide/destroy hook");
  }

  if (!g_hooks.cloak) {
    g_hooks.cloak = SetWinEventHook(
      EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
      nullptr, WinEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_hooks.cloak) Wh_Log(L"Failed to install uncloak hook");
  }

  if (!g_hooks.state) {
    g_hooks.state = SetWinEventHook(
        EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE,
        nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hooks.state) Wh_Log(L"Failed to install state-change hook");
  }
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

void ResetWorkspaceStateMemory() {
  AcquireSRWLockExclusive(&g_workspaces.lock);
  g_workspaces.states.clear();
  ReleaseSRWLockExclusive(&g_workspaces.lock);
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
// Settings + naming
//-----------------------------------------------------------------------------

constexpr wchar_t kTrayWindowClassName[] = L"WindhawkTilingHelperTrayWindow";
constexpr wchar_t kFlyoutWindowClassName[] = L"WindhawkTilingHelperStatusFlyout";
constexpr UINT kTrayCallbackMessage = WM_APP + 20;
constexpr UINT kIconId = 1;
constexpr int kIconCanvasSize = 32;
constexpr int kTrayFontHeight = 26;
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
static bool g_flyoutTop = false;
static int g_flyoutOffsetX = 0;
static int g_flyoutOffsetY = 0;
static TileLayout g_displayedLayout = TileLayout::COUNT;
static GUID g_displayedDesktopId{};
static bool g_haveDisplayedDesktopId = false;
static std::wstring g_flyoutText;
static DWORD g_flyoutShownTick = 0;
static NOTIFYICONDATAW g_notifyData{};
static std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> g_icons{};
static std::array<std::wstring, static_cast<size_t>(TileLayout::COUNT)>
    g_customIconPaths{};

static const wchar_t* LayoutDisplayName(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"Master + Stack (Vertical)";
    case TileLayout::MasterStackH: return L"Master + Stack (Horizontal)";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    default: return L"Unknown";
  }
}

static const wchar_t* LayoutTrayText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"MV";
    case TileLayout::MasterStackH: return L"MH";
    case TileLayout::Columns: return L"CL";
    case TileLayout::Rows: return L"RW";
    case TileLayout::BSP: return L"BS";
    case TileLayout::Monocle: return L"MN";
    case TileLayout::Floating: return L"FL";
    default: return L"??";
  }
}

static const wchar_t* LayoutFlyoutText(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"Master + Stack V";
    case TileLayout::MasterStackH: return L"Master + Stack H";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    default: return L"Unknown";
  }
}

static void LoadSettings() {
  using WindhawkUtils::StringSetting;
  auto position = StringSetting::make(L"FlyoutPosition");
  g_flyoutTop = _wcsicmp(position.get(), L"top") == 0;
  g_flyoutOffsetX = Wh_GetIntSetting(L"FlyoutOffsetX");
  g_flyoutOffsetY = Wh_GetIntSetting(L"FlyoutOffsetY");

  for (auto& path : g_customIconPaths) path.clear();
  for (int i = 0; i < 32; ++i) {
    auto layoutName = StringSetting::make(L"CustomIcons[%d].Layout", i);
    if (!*layoutName.get()) break;
    auto path = StringSetting::make(L"CustomIcons[%d].Path", i);
    size_t index = static_cast<size_t>(ParseLayoutSetting(layoutName.get()));
    if (*path.get() && index < g_customIconPaths.size() &&
        g_customIconPaths[index].empty()) {
      g_customIconPaths[index] = path.get();
    }
  }
}

//-----------------------------------------------------------------------------
// Theme-aware tray icons
//-----------------------------------------------------------------------------

static bool ReadSystemLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
          &size) == ERROR_SUCCESS) {
    return value != 0;
  }
  COLORREF fallback = GetSysColor(COLOR_WINDOW);
  return GetRValue(fallback) + GetGValue(fallback) + GetBValue(fallback) >= 384;
}

static COLORREF ForegroundColor() {
  return g_systemLightTheme ? RGB(24, 24, 24) : RGB(255, 255, 255);
}

static COLORREF FlyoutBackgroundColor() {
  return g_systemLightTheme ? RGB(243, 243, 243) : RGB(36, 36, 36);
}

static HICON CreateTextIcon(TileLayout layout) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = kIconCanvasSize;
  bmi.bmiHeader.biHeight = -kIconCanvasSize;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
  HDC dc = color ? CreateCompatibleDC(nullptr) : nullptr;
  if (!color || !dc || !bits) {
    if (dc) DeleteDC(dc);
    if (color) DeleteObject(color);
    return nullptr;
  }

  HGDIOBJ oldBitmap = SelectObject(dc, color);
  RECT rect{0, 0, kIconCanvasSize, kIconCanvasSize};
  FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  HFONT font = CreateFontW(-kTrayFontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  DrawTextW(dc, LayoutTrayText(layout), -1, &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);

  auto* pixels = static_cast<uint32_t*>(bits);
  COLORREF fg = ForegroundColor();
  uint32_t r = GetRValue(fg), g = GetGValue(fg), b = GetBValue(fg);
  for (int i = 0; i < kIconCanvasSize * kIconCanvasSize; ++i) {
    uint32_t p = pixels[i];
    uint32_t a = std::max({p & 0xFFu, (p >> 8) & 0xFFu,
                           (p >> 16) & 0xFFu});
    pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }

  SelectObject(dc, oldBitmap);
  DeleteDC(dc);

  constexpr int maskStride = ((kIconCanvasSize + 31) / 32) * 4;
  std::array<BYTE, maskStride * kIconCanvasSize> maskBits{};
  HBITMAP mask = CreateBitmap(kIconCanvasSize, kIconCanvasSize, 1, 1,
                              maskBits.data());
  ICONINFO info{TRUE, 0, 0, mask, color};
  HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
  if (mask) DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

// Returns an owned icon, preferring the configured file and falling back to
// generated text. The icon must eventually be released with DestroyIcon.
static HICON CreateLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_customIconPaths.size() && !g_customIconPaths[index].empty()) {
    HICON icon = static_cast<HICON>(LoadImageW(
        nullptr, g_customIconPaths[index].c_str(), IMAGE_ICON,
        kIconCanvasSize, kIconCanvasSize, LR_LOADFROMFILE));
    if (icon) return icon;
    Wh_Log(L"Failed to load custom tray icon, using text fallback: %s",
           g_customIconPaths[index].c_str());
  }
  return CreateTextIcon(layout);
}

static void DestroyIconSet(
    std::array<HICON, static_cast<size_t>(TileLayout::COUNT)>& icons) {
  for (HICON& icon : icons) {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
  }
}

static HICON GetLayoutIcon(TileLayout layout) {
  size_t index = static_cast<size_t>(layout);
  if (index < g_icons.size() && g_icons[index]) return g_icons[index];
  return LoadIconW(nullptr, IDI_APPLICATION);
}

static void SetTooltip(TileLayout layout) {
  std::wstring tooltip = L"Tiling Helper - ";
  tooltip += LayoutDisplayName(layout);
  wcsncpy(g_notifyData.szTip, tooltip.c_str(), ARRAYSIZE(g_notifyData.szTip) - 1);
  g_notifyData.szTip[ARRAYSIZE(g_notifyData.szTip) - 1] = L'\0';
}

static bool AddIconToShell() {
  if (!g_trayWindow) return false;
  if (g_displayedLayout == TileLayout::COUNT) g_displayedLayout = g_settings.defaultLayout;

  g_notifyData = {};
  g_notifyData.cbSize = sizeof(g_notifyData);
  g_notifyData.hWnd = g_trayWindow;
  g_notifyData.uID = kIconId;
  g_notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_notifyData.uCallbackMessage = kTrayCallbackMessage;
  g_notifyData.hIcon = GetLayoutIcon(g_displayedLayout);
  SetTooltip(g_displayedLayout);
  if (!Shell_NotifyIconW(NIM_ADD, &g_notifyData)) {
    g_iconAdded = false;
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

// Repoints the live tray entry to a complete new icon set before destroying the
// old handles, so the shell never observes an HICON after it has been released.
static void RebuildIcons() {
  std::array<HICON, static_cast<size_t>(TileLayout::COUNT)> icons{};
  for (size_t i = 0; i < icons.size(); ++i) {
    icons[i] = CreateLayoutIcon(static_cast<TileLayout>(i));
  }
  auto old = g_icons;
  g_icons = icons;
  if (g_displayedLayout != TileLayout::COUNT) UpdateIcon(g_displayedLayout);
  DestroyIconSet(old);
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

      HFONT font = CreateFlyoutFont(GetDpiForWindow(hwnd));
      HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, ForegroundColor());
      DrawTextW(dc, g_flyoutText.c_str(), -1, &client,
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
          alpha = static_cast<BYTE>(255 - std::min<DWORD>(
              255, ((elapsed - kFlyoutHoldMs) * 255) / kFlyoutFadeMs));
        }
        SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
        return 0;
      }
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void ShowStatusFlyout(const wchar_t* text) {
  if (!g_flyoutWindow || !text || !*text) return;
  HMONITOR monitor = GetTargetMonitor();
  RECT workArea{};
  if (!monitor || !GetMonitorWorkArea(monitor, &workArea)) return;

  g_flyoutText = text;

  UINT dpi = GetDpiForWindow(g_flyoutWindow);
  if (!dpi) dpi = 96;
  HFONT font = CreateFlyoutFont(dpi);
  HDC dc = GetDC(g_flyoutWindow);
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
  SIZE textSize{};
  GetTextExtentPoint32W(dc, g_flyoutText.c_str(),
                        static_cast<int>(g_flyoutText.size()), &textSize);
  if (oldFont) SelectObject(dc, oldFont);
  if (font) DeleteObject(font);
  ReleaseDC(g_flyoutWindow, dc);

  int width = std::max(ScaleForDpi(128, dpi),
                       static_cast<int>(textSize.cx) + ScaleForDpi(48, dpi));
  int height = ScaleForDpi(48, dpi);
  int x = workArea.left + ((workArea.right - workArea.left) - width) / 2 +
          ScaleForDpi(g_flyoutOffsetX, dpi);
  int edgeGap = ScaleForDpi(16, dpi);
  int y = g_flyoutTop ? workArea.top + edgeGap
                      : workArea.bottom - height - edgeGap;
  y += ScaleForDpi(g_flyoutOffsetY, dpi);

  int radius = ScaleForDpi(8, dpi);
  HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                   radius * 2, radius * 2);
  if (region && !SetWindowRgn(g_flyoutWindow, region, TRUE)) DeleteObject(region);

  g_flyoutShownTick = GetTickCount();
  SetLayeredWindowAttributes(g_flyoutWindow, 0, 255, LWA_ALPHA);
  SetWindowPos(g_flyoutWindow, HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(g_flyoutWindow, nullptr, TRUE);
  UpdateWindow(g_flyoutWindow);
  SetTimer(g_flyoutWindow, kFlyoutTimerId, 16, nullptr);
}

static void ShowLayoutFlyout(TileLayout layout) {
  if (layout == TileLayout::COUNT) return;
  ShowStatusFlyout(LayoutFlyoutText(layout));
}

static void ShowManagementModeFlyout(bool automatic) {
  ShowStatusFlyout(automatic ? L"Automatic Mode" : L"Manual Mode");
}

//-----------------------------------------------------------------------------
// Lifecycle / shell integration
//-----------------------------------------------------------------------------

// Synchronizes the tray with the target workspace and shows the flyout only when
// the refresh observes a transition between known virtual desktops.
static void RefreshForCurrentWorkspace() {
  TileLayout layout = g_settings.defaultLayout;
  HMONITOR monitor = GetTargetMonitor();
  DesktopMonitorKey key{};
  WorkspaceState state;
  bool haveKey = monitor && GetCurrentWorkspaceKey(monitor, &key);
  if (haveKey && LoadWorkspaceState(key, &state)) layout = state.layout;

  bool desktopChanged = haveKey && g_haveDisplayedDesktopId &&
                        !IsEqualGUID(key.desktopId, g_displayedDesktopId);
  if (haveKey) {
    g_displayedDesktopId = key.desktopId;
    g_haveDisplayedDesktopId = true;
  }
  UpdateIcon(layout);
  if (desktopChanged) ShowLayoutFlyout(layout);
}

static void HandleThemeChanged() {
  bool light = ReadSystemLightTheme();
  if (light == g_systemLightTheme) return;
  g_systemLightTheme = light;
  RebuildIcons();
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

  if (message == kTrayCallbackMessage) {
    const UINT event = LOWORD(lParam);
    const UINT iconId = HIWORD(lParam);
    if (iconId == kIconId &&
        (event == NIN_SELECT || event == NIN_KEYSELECT)) {
      // Emit the exact same action as the layout hotkey; layout mutation stays
      // serialized on the WM thread and is implemented in one place only.
      if (g_wm.threadId) PostThreadMessageW(g_wm.threadId, WM_APP_LAYOUT_CYCLE, 0, 0);
    }
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
  RebuildIcons();

  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW trayClass{};
  trayClass.lpfnWndProc = TrayWindowProc;
  trayClass.hInstance = instance;
  trayClass.lpszClassName = kTrayWindowClassName;
  g_trayWindowClass = RegisterClassW(&trayClass);
  if (!g_trayWindowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  WNDCLASSW flyoutClass{};
  flyoutClass.lpfnWndProc = FlyoutWindowProc;
  flyoutClass.hInstance = instance;
  flyoutClass.lpszClassName = kFlyoutWindowClassName;
  g_flyoutWindowClass = RegisterClassW(&flyoutClass);
  bool haveFlyoutClass = g_flyoutWindowClass != 0 ||
                         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

  g_trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClassName, L"",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 instance, nullptr);
  if (!g_trayWindow) return false;

  if (haveFlyoutClass) {
    g_flyoutWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kFlyoutWindowClassName, L"", WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, instance, nullptr);
  }

  g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
  g_displayedLayout = g_settings.defaultLayout;
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
  if (g_flyoutWindowClass) UnregisterClassW(kFlyoutWindowClassName, instance);
  if (g_trayWindowClass) UnregisterClassW(kTrayWindowClassName, instance);
  g_flyoutWindowClass = g_trayWindowClass = 0;
  g_taskbarCreatedMessage = 0;
  g_displayedLayout = TileLayout::COUNT;
  g_displayedDesktopId = {};
  g_haveDisplayedDesktopId = false;
  g_flyoutLayout = TileLayout::COUNT;
  g_notifyData = {};
}

}  // namespace TrayUi


//=============================================================================
// Diagnostic bookkeeping dump
//=============================================================================

static const wchar_t* DiagnosticLayoutName(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"MasterStack";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::MasterStackH: return L"MasterStackH";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    case TileLayout::COUNT: return L"COUNT";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticManageStateName(ManageState state) {
  switch (state) {
    case ManageState::Tiled: return L"Tiled";
    case ManageState::Floating: return L"Floating";
    case ManageState::Suspended: return L"Suspended";
    case ManageState::Ignored: return L"Ignored";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticSuspensionReasonName(SuspensionReason reason) {
  switch (reason) {
    case SuspensionReason::None: return L"None";
    case SuspensionReason::Hidden: return L"Hidden";
    case SuspensionReason::Minimized: return L"Minimized";
    case SuspensionReason::Maximized: return L"Maximized";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticPlacementResultName(PlacementResult result) {
  switch (result) {
    case PlacementResult::Success: return L"Success";
    case PlacementResult::AdjustedByWindow: return L"AdjustedByWindow";
    case PlacementResult::AccessDenied: return L"AccessDenied";
    case PlacementResult::Refused: return L"Refused";
    case PlacementResult::Dead: return L"Dead";
    default: return L"Unknown";
  }
}

static std::wstring DiagnosticGuidString(const GUID& guid) {
  wchar_t buffer[64]{};
  if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) > 0) {
    return buffer;
  }
  return L"{GUID_FORMAT_FAILED}";
}

static std::wstring DiagnosticRectString(const RECT& rect) {
  wchar_t buffer[128]{};
  swprintf(
      buffer, ARRAYSIZE(buffer), L"[%ld,%ld,%ld,%ld] (%ldx%ld)",
      rect.left, rect.top, rect.right, rect.bottom,
      rect.right - rect.left, rect.bottom - rect.top);
  return buffer;
}

static std::wstring DiagnosticWeightsString(const std::vector<double>& weights) {
  std::wstring result = L"[";
  for (size_t i = 0; i < weights.size(); ++i) {
    wchar_t buffer[64]{};
    swprintf(buffer, ARRAYSIZE(buffer), L"%.9g", weights[i]);
    if (i) result += L", ";
    result += buffer;
  }
  result += L"]";
  return result;
}

static std::wstring DiagnosticSanitizeText(const wchar_t* text) {
  if (!text || !*text) return L"";

  std::wstring result = text;
  for (wchar_t& ch : result) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  return result;
}

struct DiagnosticWindowIdentity {
  bool hwndAlive = false;
  DWORD livePid = 0;
  bool pidMatchesRecord = false;
  std::wstring processName;
  std::wstring imagePath;
  std::wstring className;
  std::wstring title;
};

static DiagnosticWindowIdentity DiagnosticReadWindowIdentity(
    HWND hwnd, DWORD recordPid) {
  DiagnosticWindowIdentity identity;
  identity.hwndAlive = hwnd && IsWindow(hwnd);

  if (identity.hwndAlive) {
    GetWindowThreadProcessId(hwnd, &identity.livePid);
    identity.pidMatchesRecord = recordPid != 0 && identity.livePid == recordPid;

    wchar_t className[256]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) > 0) {
      identity.className = DiagnosticSanitizeText(className);
    }

    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) > 0) {
      identity.title = DiagnosticSanitizeText(title);
    }
  }

  // Prefer the PID currently attached to the exact HWND. If the HWND has already
  // disappeared, the saved PID is still useful for best-effort identification,
  // but the dump keeps that distinction visible through hwndAlive/pidMatchesRecord.
  DWORD queryPid = identity.livePid ? identity.livePid : recordPid;
  if (queryPid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, queryPid);
    if (process) {
      std::vector<wchar_t> path(32768);
      DWORD chars = static_cast<DWORD>(path.size());
      if (QueryFullProcessImageNameW(process, 0, path.data(), &chars) && chars > 0) {
        identity.imagePath.assign(path.data(), chars);
        const wchar_t* base = wcsrchr(identity.imagePath.c_str(), L'\\');
        identity.processName = base ? base + 1 : identity.imagePath;
      }
      CloseHandle(process);
    }
  }

  return identity;
}

static const wchar_t* DiagnosticDisplayText(const std::wstring& text) {
  return text.empty() ? L"<unavailable>" : text.c_str();
}

static size_t DiagnosticActiveIndex(const WorkspaceState& state, HWND hwnd) {
  auto it = std::find(state.windows.begin(), state.windows.end(), hwnd);
  return it == state.windows.end()
             ? static_cast<size_t>(-1)
             : static_cast<size_t>(it - state.windows.begin());
}

static size_t DiagnosticLogicalIndex(const WorkspaceState& state, HWND hwnd, const WindowRecord& record) {
  if (record.state == ManageState::Suspended && record.hasSavedSlot) {
    return record.savedSlot.index;
  }

  if (record.state == ManageState::Tiled) {
    size_t activeIndex = DiagnosticActiveIndex(state, hwnd);
    if (activeIndex != static_cast<size_t>(-1)) {
      return ActiveIndexToLogicalIndex(state, activeIndex);
    }
  }

  return static_cast<size_t>(-1);
}

static void DiagnosticDumpWindowRecord(
    const WorkspaceState& state, HWND mapHwnd, const WindowRecord& record,
    const wchar_t* prefix, size_t ordinal) {
  const size_t activeIndex = DiagnosticActiveIndex(state, mapHwnd);
  const size_t logicalIndex = DiagnosticLogicalIndex(state, mapHwnd, record);
  const bool hasActiveIndex = activeIndex != static_cast<size_t>(-1);
  const bool hasLogicalIndex = logicalIndex != static_cast<size_t>(-1);

  if (hasLogicalIndex && hasActiveIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=%zu activeIndex=%zu hwnd=%p",
        prefix, ordinal, logicalIndex, activeIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else if (hasLogicalIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=%zu activeIndex=- hwnd=%p",
        prefix, ordinal, logicalIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else if (hasActiveIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=- activeIndex=%zu hwnd=%p",
        prefix, ordinal, activeIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else {
    Wh_Log(
        L"  %s[%zu] logicalIndex=- activeIndex=- hwnd=%p",
        prefix, ordinal, reinterpret_cast<void*>(mapHwnd));
  }

  Wh_Log(
      L"      recordHwnd=%p pid=%lu state=%s suspension=%s",
      reinterpret_cast<void*>(record.hwnd), record.pid,
      DiagnosticManageStateName(record.state),
      DiagnosticSuspensionReasonName(record.suspensionReason));

  DiagnosticWindowIdentity identity = DiagnosticReadWindowIdentity(mapHwnd, record.pid);
  Wh_Log(
      L"      identity: hwndAlive=%d livePid=%lu pidMatchesRecord=%d process=%s",
      identity.hwndAlive ? 1 : 0, identity.livePid,
      identity.pidMatchesRecord ? 1 : 0,
      DiagnosticDisplayText(identity.processName));
  Wh_Log(L"      image: %s", DiagnosticDisplayText(identity.imagePath));
  Wh_Log(L"      class: %s", DiagnosticDisplayText(identity.className));
  Wh_Log(L"      title: \"%s\"", DiagnosticDisplayText(identity.title));

  Wh_Log(
      L"      capabilities: move=%d resize=%d topmost=%d",
      record.canMove ? 1 : 0,
      record.canResize ? 1 : 0,
      record.topmost ? 1 : 0);

  Wh_Log(
      L"      placement: result=%s failures=%d",
      DiagnosticPlacementResultName(record.lastPlacementResult),
      record.placementFailures);

  std::wstring requested = DiagnosticRectString(record.lastRequestedRect);
  std::wstring observed = DiagnosticRectString(record.lastObservedRect);
  Wh_Log(L"      lastRequested=%s", requested.c_str());
  Wh_Log(L"      lastObserved =%s", observed.c_str());

  if (record.hasSavedSlot) {
    Wh_Log(
        L"      savedSlot: index=%zu weight=%.9g hasWeight=%d wasMaster=%d",
        record.savedSlot.index,
        record.savedSlot.weight,
        record.savedSlot.hasWeight ? 1 : 0,
        record.savedSlot.wasMaster ? 1 : 0);
  } else {
    Wh_Log(L"      savedSlot: none");
  }
}

static void DumpSavedBookkeeping() {
  // Copy the persistent model once. Everything structural below comes from that
  // immutable snapshot. Per-record title/class/process identity is queried later
  // only for human readability; those lookups are observational and never trigger
  // reconciliation, placement, ownership repair, or any model mutation.
  std::vector<std::pair<DesktopMonitorKey, WorkspaceState>> workspaces;
  AcquireSRWLockShared(&g_workspaces.lock);
  workspaces.reserve(g_workspaces.states.size());
  for (const auto& kv : g_workspaces.states) {
    workspaces.push_back(kv);
  }
  ReleaseSRWLockShared(&g_workspaces.lock);

  // g_workspaces.states is unordered. Give repeated dumps a stable order so diffs
  // are useful: desktop GUID first, monitor handle second.
  std::sort(
      workspaces.begin(), workspaces.end(),
      [](const auto& a, const auto& b) {
        std::wstring aDesktop = DiagnosticGuidString(a.first.desktopId);
        std::wstring bDesktop = DiagnosticGuidString(b.first.desktopId);
        if (aDesktop != bDesktop) return aDesktop < bDesktop;
        return reinterpret_cast<uintptr_t>(a.first.monitor) <
               reinterpret_cast<uintptr_t>(b.first.monitor);
      });

  Wh_Log(L"");
  Wh_Log(L"==================== TILING BOOKKEEPING ====================");
  Wh_Log(L"workspaceCount: %zu", workspaces.size());
  Wh_Log(L"pendingReinforcementCount: %zu", g_wm.pendingDesktopArranges.size());
  Wh_Log(L"lifecycleDirtyWindowCount: %zu", g_wm.lifecycleDirtyWindows.size());

  for (size_t workspaceIndex = 0; workspaceIndex < workspaces.size(); ++workspaceIndex) {
    const DesktopMonitorKey& key = workspaces[workspaceIndex].first;
    const WorkspaceState& state = workspaces[workspaceIndex].second;
    std::wstring desktopId = DiagnosticGuidString(key.desktopId);

    size_t tiledCount = 0;
    size_t floatingCount = 0;
    size_t suspendedCount = 0;
    size_t ignoredCount = 0;
    for (const auto& recordKv : state.records) {
      switch (recordKv.second.state) {
        case ManageState::Tiled: ++tiledCount; break;
        case ManageState::Floating: ++floatingCount; break;
        case ManageState::Suspended: ++suspendedCount; break;
        case ManageState::Ignored: ++ignoredCount; break;
      }
    }

    const bool pendingReinforcement = std::any_of(
        g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == key.monitor &&
                 IsEqualGUID(pending.desktopId, key.desktopId);
        });

    Wh_Log(L"");
    Wh_Log(
        L"---------------- Workspace %zu / %zu ----------------",
        workspaceIndex + 1, workspaces.size());
    Wh_Log(L"desktopId: %s", desktopId.c_str());
    Wh_Log(L"monitor:   %p", reinterpret_cast<void*>(key.monitor));
    Wh_Log(
        L"layout:    %s (%d)",
        DiagnosticLayoutName(state.layout), static_cast<int>(state.layout));
    Wh_Log(L"masterRatio: %.9g", state.masterRatio);
    Wh_Log(
        L"counts: active=%zu records=%zu tiled=%zu suspended=%zu floating=%zu ignored=%zu",
        state.windows.size(), state.records.size(), tiledCount, suspendedCount,
        floatingCount, ignoredCount);
    Wh_Log(L"pendingReinforcement: %d", pendingReinforcement ? 1 : 0);

    std::wstring stackWeights = DiagnosticWeightsString(state.stackWeights);
    std::wstring gridWeights = DiagnosticWeightsString(state.gridWeights);
    Wh_Log(L"stackWeights: %s", stackWeights.c_str());
    Wh_Log(L"gridWeights:  %s", gridWeights.c_str());

    // Print tiled + suspended records in their reconstructed logical order. This
    // makes the saved slot relationship visible directly instead of forcing the
    // reader to correlate an unordered records{} dump with windows[].
    struct LogicalRecordRef {
      size_t logicalIndex;
      HWND hwnd;
      const WindowRecord* record;
    };
    std::vector<LogicalRecordRef> logicalRecords;
    std::vector<std::pair<HWND, const WindowRecord*>> otherRecords;

    logicalRecords.reserve(state.records.size());
    otherRecords.reserve(state.records.size());
    for (const auto& recordKv : state.records) {
      const WindowRecord& record = recordKv.second;
      size_t logicalIndex = DiagnosticLogicalIndex(state, recordKv.first, record);
      if ((record.state == ManageState::Tiled || record.state == ManageState::Suspended) &&
          logicalIndex != static_cast<size_t>(-1)) {
        logicalRecords.push_back({logicalIndex, recordKv.first, &record});
      } else {
        otherRecords.push_back({recordKv.first, &record});
      }
    }

    std::sort(
        logicalRecords.begin(), logicalRecords.end(),
        [](const LogicalRecordRef& a, const LogicalRecordRef& b) {
          if (a.logicalIndex != b.logicalIndex) return a.logicalIndex < b.logicalIndex;
          return reinterpret_cast<uintptr_t>(a.hwnd) < reinterpret_cast<uintptr_t>(b.hwnd);
        });
    std::sort(
        otherRecords.begin(), otherRecords.end(),
        [](const auto& a, const auto& b) {
          if (a.second->state != b.second->state) {
            return static_cast<int>(a.second->state) < static_cast<int>(b.second->state);
          }
          return reinterpret_cast<uintptr_t>(a.first) < reinterpret_cast<uintptr_t>(b.first);
        });

    Wh_Log(L"  logicalTiledOrder: %zu", logicalRecords.size());
    if (logicalRecords.empty()) {
      Wh_Log(L"    (none)");
    } else {
      for (size_t i = 0; i < logicalRecords.size(); ++i) {
        DiagnosticDumpWindowRecord(
            state, logicalRecords[i].hwnd, *logicalRecords[i].record,
            L"window", i);
      }
    }

    Wh_Log(L"  nonLayoutRecords: %zu", otherRecords.size());
    if (otherRecords.empty()) {
      Wh_Log(L"    (none)");
    } else {
      for (size_t i = 0; i < otherRecords.size(); ++i) {
        DiagnosticDumpWindowRecord(
            state, otherRecords[i].first, *otherRecords[i].second,
            L"record", i);
      }
    }
  }

  if (!g_wm.lifecycleDirtyWindows.empty()) {
    Wh_Log(L"");
    Wh_Log(L"pendingLifecycleWindows:");
    for (size_t i = 0; i < g_wm.lifecycleDirtyWindows.size(); ++i) {
      Wh_Log(
          L"  [%zu] hwnd=%p",
          i, reinterpret_cast<void*>(g_wm.lifecycleDirtyWindows[i]));
    }
  }

  if (!g_wm.pendingDesktopArranges.empty()) {
    Wh_Log(L"");
    Wh_Log(L"pendingWorkspaceReinforcements:");
    for (size_t i = 0; i < g_wm.pendingDesktopArranges.size(); ++i) {
      std::wstring desktopId = DiagnosticGuidString(g_wm.pendingDesktopArranges[i].desktopId);
      Wh_Log(
          L"  [%zu] desktopId=%s monitor=%p",
          i, desktopId.c_str(),
          reinterpret_cast<void*>(g_wm.pendingDesktopArranges[i].monitor));
    }
  }

  Wh_Log(L"============================================================");
  Wh_Log(L"");
}

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
  g_wm.autoDiscoverySuspended =
      !managementMode || _wcsicmp(managementMode, L"automatic") != 0;
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
    WorkspaceState existingState;
    if (GetCurrentWorkspaceKey(monitor, &key) &&
        LoadWorkspaceState(key, &existingState)) {
      ArrangeWorkspace(key);
    } else {
      DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
  }

  Wh_Log(
      L"Window management mode at startup: %s",
      g_wm.autoDiscoverySuspended ? L"Manual" : L"Automatic");
}

static void ToggleWindowManagementMode() {
  g_wm.autoDiscoverySuspended = !g_wm.autoDiscoverySuspended;
  const bool automatic = !g_wm.autoDiscoverySuspended;
  Wh_Log(L"Window management mode: %s", automatic ? L"Automatic" : L"Manual");
  TrayUi::ShowManagementModeFlyout(automatic);

  // Reopening admission runs the normal discovery path for every monitor. Known
  // Floating records remain managed-but-excluded and are never re-adopted here.
  if (automatic) {
    std::vector<HMONITOR> monitors;
    EnumerateCurrentMonitors(monitors);
    for (HMONITOR monitor : monitors) {
      DiscoverCurrentWorkspaceOnMonitor(monitor);
    }
    ScheduleLifecycleReconcile(nullptr);
  }
}

static void HandleMoveSizeEndMessage(HWND hwnd) {
  // MOVESIZEEND is an authoritative monitor-transition boundary. Repair monitor
  // ownership before interpreting final geometry, without querying desktop
  // ownership for unrelated lifecycle noise.
  std::vector<DesktopMonitorKey> changedKeys;
  ReconcileKnownWindows(ReconcileScope::Monitor, &changedKeys, hwnd);
  ApplyUserMoveSize(hwnd);
  for (const auto& key : changedKeys) ArrangeWorkspace(key);
  ScheduleLifecycleReconcile(hwnd);
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
      ProcessWindowLifecycleEvent(
          static_cast<DWORD>(msg.wParam), reinterpret_cast<HWND>(msg.lParam));
      return WmMessageDisposition::Handled;

    case WM_APP_VIRTUAL_DESKTOP_CHANGED:
      HandleVirtualDesktopChanged();
      return WmMessageDisposition::Handled;

    case WM_APP_TRAY_REFRESH:
      TrayUi::RefreshForCurrentWorkspace();
      return WmMessageDisposition::Handled;

    case WM_APP_LAYOUT_CYCLE:
      CycleCurrentWorkspaceLayout();
      return WmMessageDisposition::Handled;

    case WM_TIMER:
      if (g_wm.lifecycleTimer &&
          static_cast<UINT_PTR>(msg.wParam) == g_wm.lifecycleTimer) {
        ReconcileDeferredLifecycle();
        return WmMessageDisposition::Handled;
      }
      return WmMessageDisposition::DispatchToWindows;

    case WM_HOTKEY:
      switch (static_cast<UINT>(msg.wParam)) {
        case HK_TILE:
          TileWindows();
          break;
        case HK_LAYOUT:
          CycleCurrentWorkspaceLayout();
          break;
        case HK_SWAP_MASTER:
          SwapMaster();
          break;
        case HK_FLOAT_FOCUSED:
          FloatFocusedWindow();
          break;
        case HK_DIAGNOSTIC_DUMP:
          DumpSavedBookkeeping();
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
  RemoveWinEventHooks();
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
    InstallWinEventHooks();
    ScheduleLifecycleReconcile(nullptr);
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

// Ends only the dedicated worker's bootstrap thread at its normal entry point;
// the already-started hotkey thread keeps the tool process alive.
void WINAPI EntryPoint_Hook() {
  Wh_Log(L">");
  ExitThread(0);
}

// Routes each windhawk.exe instance into its tool-mod role. Normal instances act
// as launchers; only the worker tagged with this mod ID enforces singleton
// ownership and initializes the tiling service.
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

// Launcher-side phase: starts a copy of windhawk.exe tagged as this mod's
// dedicated tool process, then releases the returned process and thread handles.
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

