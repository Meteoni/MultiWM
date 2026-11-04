// Shared platform declarations, configuration values and message types.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

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
#include <atomic>
#include <cmath>
#include <climits>
#include <cassert>
#include <cstdarg>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <new>
#include <optional>
#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
// Logical ownership
// -----------------
// Model                    workspace values, membership and durable preferences
// Layout                   rectangle allocation, planning and resize learning
// Placement                floating/tiled placement, transactions and conformance
// Reconcile                observed ownership, discovery and window routing
// Platform::Windows        native window observations and checked native placement
// Platform::Monitors       monitor identity, DPI and physical work areas
// Platform::WindowEvents   WinEvent adapter and queueing
// Platform::VirtualDesktop private shell COM adapter and notification sink
// Configuration            settings parsing and loading
// RuntimeLifecycle         STA dispatch, hotkeys, startup and recovery
// TrayUi / Diagnostics     presentation and observability
//
// Each namespace lives in the matching src/ folder (Model -> model/, Platform ->
// platform/, and so on). Namespaces show who owns a function; because everything
// is one source file, they do not hide anything from other code.
// core/forward_declarations.h lists the calls needed before their definition.
//
// Model invariants
// ----------------
// 1. Workspace::records is the authoritative set of logically known HWNDs.
// 2. Workspace::windows contains only active Tiled records, exactly once each,
//    in logical layout order.
// 3. Floating records exist in records but never consume a slot in windows.
// 4. Suspended records exist in records, do not appear in windows, and preserve
//    a saved logical slot/weight until restored or explicitly floated/forgotten.
// 5. Columns/Rows keep one grid weight per active tiled window.
//    MasterStack/MasterStackH keep one stack weight per non-master active window.
//    Dwindle keeps one outgoing ratio per active slot. The final slot's ratio
//    is dormant until another leaf follows it, and survives suspension.
// 6. A managed HWND has exactly one desktop+monitor workspace owner.
// 7. Visibility/cloak events are dirty signals; ownership comes from explicit
//    desktop/monitor queries, not from cloak state.
// 8. Management mode controls group membership policy, not the core WM pipeline:
//    Automatic admits unknown HWNDs and migrates managed HWNDs between workspaces;
//    Manual admits only through explicit commands, forgets minimized members, and
//    releases membership when a managed HWND moves to another virtual desktop.
//    Maximized/fullscreen tiled members are suspended and restore to their saved
//    logical slot when they return to a normal window state.
// 9. Floating geometry is remembered independently of tiled geometry. Tiling and
//    geometry adoption never overwrite the last meaningful floating rectangle.
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

// Private virtual-desktop interface types are deliberately incomplete here.
// Their ABI definitions live in the Platform::VirtualDesktop section after the
// model/layout code, so maintainers encounter the WM model before shell internals.
namespace Platform::VirtualDesktop {
struct IVirtualDesktop;
struct IVirtualDesktopManagerInternal;
struct IVirtualDesktopNotificationService;
struct VirtualDesktopNotificationObject;

struct NotificationInterfaceConfig {
  IID iid{};
  int methodCount = 0;
  int currentChangedIndex = -1;
  bool currentChangedHasMonitors = false;
};

struct VirtualDesktopAbiProfile {
  IID managerInternal{};
  IID virtualDesktop{};
  bool usesHMonitor = false;
  int getDesktopsIndex = 7;
  int switchDesktopIndex = 9;
  int createDesktopIndex = 10;
  int moveDesktopIndex = -1;
  int removeDesktopIndex = 11;
  int animatedSwitchIndex = -1;
  int animationWaitIndex = -1;
  NotificationInterfaceConfig notification{};
};

bool InitializeVirtualDesktopAPI();
void CleanupVirtualDesktopAPI();
void AbandonVirtualDesktopAPIForShellRestart();
bool GetCurrentDesktopId(GUID* outGuid);
bool GetWindowDesktopIdSafe(HWND hwnd, GUID* outGuid);
bool IsWindowOnCurrentDesktopSafe(HWND hwnd, BOOL* onCurrent);
bool RegisterVirtualDesktopNotifications();
bool GetDesktopIdsInOrder(std::vector<GUID>* desktopIds);
bool CreateDesktopAfter(const GUID& sourceDesktop, GUID* desktopId, int* desktopNumber);
bool IsDesktopEmptyExcept(const GUID& desktopId, HWND allowedWindow = nullptr);
bool RemoveEmptyOverflowDesktop(const GUID& desktopId, const GUID& fallbackDesktop);
enum class WindowMovePurpose { Overflow, InitialPlacement };
bool MoveWindowToDesktop(HWND hwnd, DWORD processId, const GUID& sourceDesktop, const GUID& desktopId,
                         WindowMovePurpose purpose = WindowMovePurpose::Overflow);
bool SwitchToDesktop(HWND hwnd, DWORD processId, const GUID& desktopId, const GUID& sourceDesktop, DWORD inputTickMs,
                     WindowMovePurpose purpose = WindowMovePurpose::Overflow);

struct DesktopMetadata {
  std::wstring name;
  int number = 0;
};

DesktopMetadata ReadDesktopMetadata(const GUID& desktopId);

// True only when the shell answers and reports `desktopId` as the active desktop.
// A failed query counts as "no". Makes one COM call, which may deliver messages.
inline bool IsCurrentDesktop(const GUID& desktopId) {
  GUID current{};
  return GetCurrentDesktopId(&current) && IsEqualGUID(current, desktopId);
}

// True only when the shell answers and reports that `hwnd` lives on `desktopId`.
// A failed query counts as "no". Makes one COM call, which may deliver messages.
inline bool IsWindowOnDesktop(HWND hwnd, const GUID& desktopId) {
  GUID owner{};
  return GetWindowDesktopIdSafe(hwnd, &owner) && IsEqualGUID(owner, desktopId);
}
}  // namespace Platform::VirtualDesktop

namespace RuntimeLifecycle {
void RequestMaintenance(bool resetAttempts = false);
void RunMaintenanceNow();
void NotifyShellRestarted();
void NotifyDisplayTopologyChanged();
}  // namespace RuntimeLifecycle

// Narrow platform surface used by the WM core. The private COM implementation
// lives with the other Windows adapters near the end of the source.

// Hotkey IDs are stable message identifiers, not user configuration.
enum HotkeyIds {
  HK_TILE = 1,
  HK_LAYOUT = 2,
  HK_SWAP_MASTER = 3,
  HK_MANAGEMENT_MODE_TOGGLE = 4,
  HK_FLOAT_FOCUSED = 5,
  HK_DIAGNOSTIC_DUMP = 6,
  HK_PROMOTE_WINDOW = 7,
  HK_DEMOTE_WINDOW = 8,
};

// Messages consumed by the serialized WM thread.
constexpr UINT WM_APP_MOVE_SIZE_END = WM_APP + 1;
constexpr UINT WM_APP_WINDOW_EVENT = WM_APP + 2;
constexpr UINT WM_APP_VIRTUAL_DESKTOP_CHANGED = WM_APP + 3;
constexpr UINT WM_APP_LAYOUT_CYCLE = WM_APP + 5;
constexpr UINT WM_APP_RECONCILE_NOW = WM_APP + 6;
constexpr UINT WM_APP_LAYOUT_SET = WM_APP + 7;
constexpr UINT WM_APP_MANAGEMENT_MODE_SET = WM_APP + 8;
constexpr UINT WM_APP_TILE_WORKSPACE = WM_APP + 9;
constexpr UINT WM_APP_FOREGROUND_CHANGED = WM_APP + 10;

enum class WmMessageDisposition {
  Handled,
  DispatchToWindows,
};

enum class TileLayout { MasterStack, Columns, Rows, MasterStackH, Dwindle, Monocle, Floating, COUNT };

// Master + Stack in either orientation: one master window plus a weighted stack.
constexpr bool IsMasterStackLayout(TileLayout layout) {
  return layout == TileLayout::MasterStack || layout == TileLayout::MasterStackH;
}

// Columns or Rows: every window is a peer with one weight each.
constexpr bool IsGridLayout(TileLayout layout) {
  return layout == TileLayout::Columns || layout == TileLayout::Rows;
}
enum class MouseMoveBehavior { Float, Swap };
enum class WindowRuleTreatment {
  Exclude,
  DefaultToFloating,
  AlwaysFloating,
  TraceToOwner,
  FloatingPlacementOverride,
  InitialPlacement,
};
enum class ManagementMode { Manual, Automatic };
enum class AutomaticNewWindowPosition { LastSlot, AfterFocused };

static std::vector<TileLayout> MakeBuiltInLayoutCycle() {
  return {
      TileLayout::MasterStack,
      TileLayout::MasterStackH,
      TileLayout::Dwindle,
      TileLayout::Columns,
      TileLayout::Rows,
      TileLayout::Monocle,
      TileLayout::Floating};
}

struct WorkspaceInsets {
  LONG left = 6;
  LONG top = 6;
  LONG right = 6;
  LONG bottom = 6;
};

struct FloatingDefaultSize {
  LONG width = 960;
  LONG height = 640;
};

enum class RuleMonitor { Any, Primary, NonPrimary, Specific };

struct WindowRule {
  WindowRuleTreatment treatment = WindowRuleTreatment::Exclude;
  std::vector<std::wstring> processNames;
  std::vector<std::wstring> classNames;
  std::vector<std::wstring> titleFragments;
  bool preserveFloatingSize = true;
  bool useWorkspaceSize = false;
  FloatingDefaultSize floatingSizeDip{};
  RuleMonitor monitor = RuleMonitor::Any;
  std::wstring monitorId;
  int desktopNumber = 0;
  std::wstring desktopNameContains;
  bool followWindow = false;
};

struct WorkspaceInitializationRule {
  std::wstring desktopNameContains;
  int desktopNumber = 0;
  RuleMonitor monitor = RuleMonitor::Any;
  std::wstring monitorId;
  TileLayout layout = TileLayout::MasterStack;
  int settingsIndex = 0;
};

// Windhawk settings and immutable-at-runtime policy values. Settings reloads
// replace these fields; transient WM state lives in WmRuntime instead.
struct SettingsState {
  UINT reconcileDelayMs = 50;
  UINT slowApplicationDelayMs = 250;
  UINT conformanceLeaseMs = 3000;
  UINT conformanceRepairIntervalMs = 75;

  UINT tilingModifiers = MOD_ALT;
  UINT tileKey = 'T';
  UINT layoutKey = 'L';
  UINT swapMasterKey = 'M';
  UINT promoteWindowKey = VK_OEM_COMMA;
  UINT demoteWindowKey = VK_OEM_PERIOD;
  UINT managementModeToggleKey = 'R';
  UINT floatFocusedKey = 'F';
  UINT diagnosticDumpKey = 0;

  // User-facing pixel values are logical pixels at 96 DPI; scale only at a
  // concrete monitor/window boundary so stored screen geometry remains physical.
  WorkspaceInsets insetsDip{};
  LONG gapDip = 6;
  FloatingDefaultSize floatingDefaultSizeDip{};
  LONG masterPercent = 50;
  bool reverseDwindle = false;
  size_t adaptiveDwindleSlots = 2;
  LONG adaptiveDwindlePercentage = 40;

  MouseMoveBehavior mouseMoveBehavior = MouseMoveBehavior::Float;
  AutomaticNewWindowPosition automaticNewWindowPosition =
      AutomaticNewWindowPosition::LastSlot;
  TileLayout defaultLayout = TileLayout::MasterStack;
  std::vector<TileLayout> layoutCycle = MakeBuiltInLayoutCycle();
  std::vector<WorkspaceInitializationRule> workspaceInitializationRules;
  std::vector<WindowRule> windowRules;
  size_t automaticWindowLimit = 0;
  bool overflowCreateDesktop = false;
  bool overflowTryOtherMonitors = false;
  struct MonitorWindowLimit {
    RuleMonitor monitor = RuleMonitor::Specific;
    std::wstring monitorId;
    size_t limit = 0;
  };
  std::vector<MonitorWindowLimit> monitorWindowLimits;
  std::wstring diagnosticsOutputPath =
      L"%USERPROFILE%\\Documents\\MultiWMDiagnostics";
};

struct MoveSizeGesture;
