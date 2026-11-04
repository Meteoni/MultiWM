// Declarations of functions that earlier chapters call before the chapter that
// defines them (see the include order in src/multiwm.cpp). For example, layout
// placement/arrangement.h starts conformance leases, but BeginConformanceLease is
// defined later, in placement/conformance.h.
//
// When you get "use of undeclared identifier" for a function defined in a later
// file, add its declaration here, next to its owner's namespace. Keep the
// signature (including default arguments) identical to the definition.
namespace Placement {
// Where Placement::RepairFloatingGeometry (placement/floating.h) should put a
// window that becomes floating.
enum class FloatingPlacementIntent {
  PassiveRestore,          // Reuse the remembered floating frame when it is on this monitor.
  PreserveAnchor,          // Keep the window centered on `anchor` (where the user dropped it).
  CenterWithFloatingSize,  // Center on the workspace at the remembered/default floating size.
  NewWindowCenter,         // Center at the size a new floating window gets (rules apply).
  CenterPreservingSize,    // Center without resizing; does nothing if the frame is unknown.
  FailureCenter,           // Fallback after a failed tile: like NewWindowCenter, but rules
                           // that preserve the app's size are ignored.
};
struct FloatingPlacementHint {
  FloatingPlacementIntent intent = FloatingPlacementIntent::PassiveRestore;
  POINT anchor{};  // Physical screen pixels; used only by PreserveAnchor.
};
// Defined in placement/arrangement.h.
static void ArrangeWorkspace(const DesktopMonitorKey& key, HWND preferredTiledWindow = nullptr);
// Defined in placement/conformance.h.
static void BeginConformanceLease(
    HWND hwnd, const RECT& expectedRect, bool scheduleRepair = false,
    HWND preferredTiledWindow = nullptr);
static bool CancelConformanceLease(HWND hwnd, uint64_t generation = 0);
static bool ClearSettledInsertionPlacements(const DesktopMonitorKey& key, Workspace& workspace);
static bool HasOutstandingConformanceLease(const Workspace& workspace);
}  // namespace Placement

namespace Platform::Windows {
// Defined in platform/native_windows.h.
static HMONITOR GetCurrentManagedWindowMonitor(HWND hwnd, const GUID* expectedDesktop = nullptr);
static HMONITOR GetWindowPhysicalMonitor(HWND hwnd);
static bool IsWindowTrackedInAnyState(HWND hwnd);
static bool WindowCanBeManaged(HWND hwnd, bool allowFixedSize = false);
}  // namespace Platform::Windows

namespace Platform::WindowEvents {
// Defined in platform/window_events.h.
static bool HasTrackedMonitorOwnershipMismatch(HWND hwnd);
}  // namespace Platform::WindowEvents

namespace Reconcile {
// Defined in reconcile/reconcile.h.
static void ScheduleLifecycleReconcile(HWND hwnd);
// Defined in reconcile/window_routing.h.
static void RouteNewWindows(std::vector<HMONITOR>& monitors);
static void FollowInitiallyPlacedWindow();
static void ProcessWindowOverflow();
// Defined in reconcile/reconcile.h.
static void EnumerateCurrentMonitors(std::vector<HMONITOR>& monitors);
}  // namespace Reconcile

namespace RuntimeLifecycle {
// Defined in app/wm_thread.h.
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg);
}  // namespace RuntimeLifecycle

// Defined in app/tray_ui.h.
namespace TrayUi {
static void LoadSettings();
static bool Initialize();
static void Shutdown();
static void UpdateIcon(TileLayout layout, HMONITOR monitor = nullptr);
static void ShowLayoutFlyout(TileLayout layout, HMONITOR monitor = nullptr);
static void ShowCommandFailureFlyout(const wchar_t* text, HMONITOR monitor = nullptr);
static void ShowManagementModeFlyout(bool automatic);
static void ShowDesktopSwitchFlyouts(const GUID& desktopId);
static void RefreshForMonitor(HMONITOR monitor);
static void RefreshDisplayedMonitor();
static void RefreshForCurrentWorkspace();
}  // namespace TrayUi
