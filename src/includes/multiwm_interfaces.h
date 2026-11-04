// Shared placement input types and forward declarations for cross-file calls in the Windhawk translation unit.
// Types and runtime state precede this file. Helpers remain with their owner;
// these declarations resolve ordering, rather than enforcing module privacy.
namespace Placement {
enum class FloatingPlacementIntent {
  PassiveRestore,
  PreserveAnchor,
  CenterWithFloatingSize,
  NewWindowCenter,
  CenterPreservingSize,
  FailureCenter,
};
struct FloatingPlacementHint {
  FloatingPlacementIntent intent = FloatingPlacementIntent::PassiveRestore;
  POINT anchor{};
};
static void ArrangeWorkspace(const DesktopMonitorKey& key, HWND preferredTiledWindow = nullptr);
static void BeginConformanceLease(
    HWND hwnd, const RECT& expectedRect, bool scheduleRepair = false,
    HWND preferredTiledWindow = nullptr);
static bool CancelConformanceLease(HWND hwnd, uint64_t generation = 0);
static bool ClearSettledInsertionPlacements(const DesktopMonitorKey& key, Workspace& workspace);
static bool HasOutstandingConformanceLease(const Workspace& workspace);
}  // namespace Placement

namespace Platform::Windows {
static HMONITOR GetCurrentManagedWindowMonitor(HWND hwnd, const GUID* expectedDesktop = nullptr);
static HMONITOR GetWindowPhysicalMonitor(HWND hwnd);
static bool IsWindowTrackedInAnyState(HWND hwnd);
static bool WindowCanBeManaged(HWND hwnd, bool allowFixedSize = false);
}  // namespace Platform::Windows

namespace Platform::WindowEvents {
static bool HasTrackedMonitorOwnershipMismatch(HWND hwnd);
}  // namespace Platform::WindowEvents

namespace Reconcile {
static void ScheduleLifecycleReconcile(HWND hwnd);
static void RouteNewWindows(std::vector<HMONITOR>& monitors);
static void FollowInitiallyPlacedWindow();
static void ProcessWindowOverflow();
static void EnumerateCurrentMonitors(std::vector<HMONITOR>& monitors);
}  // namespace Reconcile

namespace RuntimeLifecycle {
static WmMessageDisposition HandleWmThreadMessage(const MSG& msg);
}  // namespace RuntimeLifecycle

// UI implementation follows the WM pipeline; only this surface is needed early.
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
