// ==WindhawkMod==
// @id              multiwm
// @name            MultiWM
// @description     Lightweight, low-cortisol window manager with true per-virtual-desktop layouts. 
// @version         1.16.1
// @author          meteoni
// @github          https://github.com/Meteoni
// @license         MIT
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -luuid -ldwmapi -lversion -lgdi32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# MultiWM

MultiWM is a continuation of the original [Tiling Helper](https://windhawk.net/mods/tiling-helper) by [U2X1](https://github.com/u2x1).

A lightweight, low-cortisol window manager for Windows 11 with true per-virtual-desktop layouts - 
including floating - and simple, predictable controls. 

![GIF](https://raw.githubusercontent.com/Meteoni/meteoni-assets/main/MultiWM/MultiWM.gif)

## Notes
- *Windhawk 2.0 (or higher) users can change the target process (`explorer.exe` -> `windhawk.exe, windhawk-mod.exe, windhawk-mod-uiaccess.exe`) for a better experience.*
- *MultiWM's virtual-desktop notification integration is based on [Taskbar Desktop Indicator by Simon Benedict](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-desktop-indicator.wh.cpp) (MIT).*

## Features
- **First-class floating** - use Floating as a whole-workspace layout 
or float individual windows while retaining useful geometry.
- **Automatic or Manual management** - let MultiWM continuously discover 
and migrate windows, or keep explicitly tiled groups under your control.
- **True virtual-desktop layouts** - uses Windows' existing virtual desktops 
instead of implementing a separate desktop system.
- **Event-driven** - changes are signaled through native events rather than 
continuous polling.

## Layouts
- Floating
- Master + Stack (vertical or horizontal)
- Dwindle
- BSP
- Columns / Rows
- Monocle

## Getting Started
Move a tiled window to **float it** or **swap it with the window underneath**. In BSP, drop near an edge to split the target tile or in the center to swap. Windows sharing a divider swap on center and parallel edge drops, and rotate on perpendicular edge drops. The preview fills the destination of the dragged window and outlines the destination of the other window.

By default, hotkeys are available for tiling (Alt+T), changing layouts (Alt+L), the layout action (Alt+M, rotates the focused split in BSP or promotes the focused window),
moving windows through the logical order (Alt+, / .), resizing the focused window (Alt+= / - / 0),
floating a window (Alt+F), and switching between Automatic and Manual management (Alt+R).
If another program already uses one of these shortcuts, a toast names it. A hotkey that can't act,
such as promoting the first window, says why in a toast.

A small tray indicator shows the active workspace layout and provides quick access to layouts and management mode.

## Contributing
MultiWM has a dedicated [GitHub repository](https://github.com/Meteoni/MultiWM)! Pull requests are welcome!


## Also check out
- [Virtual Desktop Helper by u2x1](https://windhawk.net/mods/virtual-desktop-helper)
- [Taskbar Desktop Indicator by Simon Benedict](https://windhawk.net/mods/taskbar-desktop-indicator)
- [GNOME-like dynamic virtual desktops by Giggig](https://windhawk.net/mods/gnome-dynamic-desktops)

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- general:
  - DefaultWindowManagementMode: automatic
    $name: Default Window Management Mode
    $description: Automatic adds windows as they appear. Manual adds them when you choose Tile Workspace. Applies when MultiWM starts; a mode chosen with the hotkey or tray menu stays until MultiWM restarts or this setting changes.
    $options:
      - manual: Manual
      - automatic: Automatic
  - AutomaticNewWindowPosition: last_slot
    $name: Automatic Window Insertion Position
    $description: Where new or moved windows are tiled in Automatic mode. BSP tries both split directions in the selected tile, then in the biggest tile, before floating the window. If there is no focused tile, BSP starts with the biggest tile.
    $options:
      - last_slot: Last slot / Biggest tile (BSP)
      - after_focused: After focused / Focused tile (BSP)
  - TrayClickAction: cycle_layout
    $name: Tray Icon Click
    $description: Action when the tray icon is left-clicked.
    $options:
      - cycle_layout: Switch layout
      - context_menu: Open context menu
  $name: General

- workspace:
  - DefaultLayout: master_stack
    $name: Fallback Layout
    $description: Starting layout when no rule matches.
    $options:
      - master_stack: Master + Stack (Vertical)
      - master_stack_h: Master + Stack (Horizontal)
      - columns: Columns
      - rows: Rows
      - dwindle: Dwindle
      - bsp: BSP
      - monocle: Monocle (Fullscreen)
      - floating: Floating (No tiling)
  - LayoutCycle: [master_stack, master_stack_h, dwindle, columns, rows, monocle, floating, bsp]
    $name: Layout Cycle
    $description: Layouts used by Cycle Layout, in this order.
    $options:
      - master_stack: Master + Stack (Vertical)
      - master_stack_h: Master + Stack (Horizontal)
      - columns: Columns
      - rows: Rows
      - dwindle: Dwindle
      - bsp: BSP
      - monocle: Monocle (Fullscreen)
      - floating: Floating (No tiling)
  - InitializationRules:
      - - Layout: ""
          $name: Initial Layout
          $description: Choose a layout and at least one desktop or monitor condition. Disabled ignores this row.
          $options:
            - "": Disabled
            - master_stack: Master + Stack (Vertical)
            - master_stack_h: Master + Stack (Horizontal)
            - columns: Columns
            - rows: Rows
            - dwindle: Dwindle
            - bsp: BSP
            - monocle: Monocle (Fullscreen)
            - floating: Floating (No tiling)
        - DesktopNameContains: ""
          $name: Desktop Name Contains
          #! $hideIf: {Layout: ""}
          $description: Match part of a custom desktop name, ignoring case. Blank matches any desktop.
        - DesktopNumber: 0
          $name: Desktop Number
          #! $hideIf: {Layout: ""}
          #! $min: 0
          $description: Desktop position in Task View, starting at 1. 0 matches any desktop.
        - Monitor: any
          $name: Monitor
          #! $hideIf: {Layout: ""}
          $options:
            - any: Any
            - primary: Primary
            - non_primary: Non-primary
            - specific: Specific monitor
        - MonitorId: ""
          $name: Monitor ID
          #! $hideIf: {Layout: ""}
          #! $showIf: {Monitor: specific}
          $description: Paste a monitor ID from diagnostics. Blank disables this rule.
    $name: Initialization Rules
    $description: Special handling rules for specific virtual desktops / monitors.
  - Overflow:
    - WindowLimit: 0
      $name: Auto-tiling Limit
      #! $min: 0
      $description: Maximum tiled windows per monitor on each desktop. 0 means unlimited.
    - Action: float
      $name: Overflow Action
      $description: Applies when tiled windows exceed the limit or cannot fit. BSP uses Float window and can try other monitors on the current desktop when enabled. A new or returning BSP window floats in place if neither its tile nor the biggest one can fit it. Created virtual desktops stay after their windows close or MultiWM is turned off.
      $options:
        - float: Float window
        - new_desktop: New desktop after current
    - TryOtherMonitors: false
      $name: Try Other Monitors First
      $description: Try another monitor before floating or changing desktop.
    - MonitorLimits:
      - - Monitor: disabled
          $name: Monitor
          $options:
            - disabled: Rule disabled
            - primary: Primary
            - non_primary: Non-primary
            - specific: Specific monitor
        - MonitorId: ""
          $name: Monitor ID
          #! $showIf: {Monitor: specific}
          $description: Paste a monitor ID from diagnostics. Blank disables this rule.
        - Limit: 0
          $name: Auto-tiling Limit
          #! $min: 0
          #! $hideIf: {Monitor: disabled}
          $description: Limit for each matching monitor per desktop. 0 means unlimited.
      $name: Per-Monitor Limits
      $description: First matching rule wins. Other monitors use the main limit.
    $name: Overflow
    $description: Automatic mode only. Minimized and hidden windows do not count. Maximized windows count.
  - TileGap: 6
    $name: Window Gap (DPI-scaled pixels)
    #! $min: 0
    #! $max: 100
    $description: Space between tiled windows (0-100).
  - WorkspaceInsets: "6, 6, 6, 6"
    $name: Insets (DPI-scaled pixels)
    $description: Space at the left, top, right, and bottom edges. Enter four values from 0 to 500. With a hidden taskbar and all four insets near 0, 3 physical pixels are kept on every edge, so a lone window isn't taken for fullscreen.
  - MasterPercent: 50
    $name: Master Size (%)
    #! $min: 1
    #! $max: 99
    #! $format: slider
    $description: Starting screen share for the main window in Master + Stack layouts (1-99%).
  - ReverseDwindle: false
    $name: Reverse Dwindle
    $description: Grow the Dwindle layout from the top-left instead of the bottom-right. BSP ignores this setting.
  - AdaptiveDwindleSlots: 2
    $name: Adaptive Split Depth
    #! $min: 0
    #! $max: 64
    $description: Number of split levels in Dwindle and BSP that can temporarily adjust to fit windows (0-64), starting from the first split. Splits return when the extra space is no longer needed, unless you resize a window first. Resizing keeps the layout as shown. 0 keeps all splits fixed.
  - AdaptiveDwindlePercentage: 15
    $name: Adaptive Split Percentage
    #! $min: 0
    #! $max: 40
    #! $format: slider
    #! $hideIf: {AdaptiveDwindleSlots: 0}
    $description: How far an adaptive split may move from its saved position, in percentage points (0-40). New dividers start at 50%. For an 80% divider, 15 allows 65%-90% after applying the 10%-90% ratio limits. 0 disables adjustment.
  $name: Workspace

- appearance:
  - ShowToasts: true
    $name: Show Toasts
    $description: Show layout, mode and error toasts, and say why a hotkey did nothing.
  - FlyoutPosition: top
    $name: Toast Position
    #! $showIf: {ShowToasts: true}
    $description: Show messages at the top or bottom of the active monitor.
    $options:
      - top: Top
      - bottom: Bottom
  - FlyoutOffsetX: 0
    $name: Toast Horizontal Offset (DPI-scaled pixels)
    #! $showIf: {ShowToasts: true}
    $description: Move messages left or right. Positive values move right. Messages stay inside the monitor's work area.
  - FlyoutOffsetY: 0
    $name: Toast Vertical Offset (DPI-scaled pixels)
    #! $showIf: {ShowToasts: true}
    $description: Move messages up or down. Positive values move down. Messages stay inside the monitor's work area, clear of the taskbar.
  - DragPreview:
    - FillOpacity: 25
      $name: Fill Opacity (%)
      #! $min: 0
      #! $max: 100
      #! $format: slider
      $description: Opacity of the filled destination for the dragged window. Set to 0 to hide the fill.
    - OutlineOpacity: 100
      $name: Outline Opacity (%)
      #! $min: 0
      #! $max: 100
      #! $format: slider
      $description: Opacity of the outline for the other window. Set to 0 to hide the outline.
    - OutlineWidth: 4
      $name: Outline Width (DPI-scaled pixels)
      #! $min: 1
      #! $max: 20
      $description: Thickness of the outline (1-20).
    - OutlineCornerRadius: 8
      $name: Outline Corner Radius (DPI-scaled pixels)
      #! $min: 0
      #! $max: 32
      $description: Outer corner radius (0-32). The inner radius is reduced by the outline width. Set to 0 for square corners.
    - AppIconSize: 32
      $name: App Icon Size (DPI-scaled pixels)
      #! $min: 0
      #! $max: 64
      $description: Size of the app icons centered in the preview tiles (0-64). Set to 0 to hide the icons.
    - FadeDurationMs: 96
      $name: Fade Duration (ms)
      #! $min: 0
      #! $max: 500
      $description: Time for the preview to appear, disappear, or fade in at a new destination (0-500). Set to 0 for instant changes.
    $name: Drag Preview
    $description: Preview a tiled window drag when Tiled Window Drag Action is set to swap.
  - CustomIcons:
      - - Layout: master_stack
          $name: Layout
          $options:
            - master_stack: Master + Stack (Vertical)
            - master_stack_h: Master + Stack (Horizontal)
            - columns: Columns
            - rows: Rows
            - dwindle: Dwindle
            - bsp: BSP
            - monocle: Monocle
            - floating: Floating
        - Path: ""
          $name: Icon File (.ico)
          #! $format: filePath
    $name: Custom Tray Layout Icons
    $description: Choose a .ico file for each layout. Blank or invalid files use the built-in icon.
  $name: Appearance

- hotkeys:
  - TilingModifier: alt
    $name: Modifier
    $description: Held with every shortcut below. Alt may conflict with app menus; choose another modifier if needed.
    $options:
      - alt: Alt
      - ctrl: Ctrl
      - alt+shift: Alt + Shift
      - ctrl+alt: Ctrl + Alt
      - ctrl+shift: Ctrl + Shift
  - TileKey: "T"
    $name: Tile Workspace
    $description: Arrange windows on the current monitor. Blank disables this shortcut. Examples include T, Space, or F5.
  - LayoutKey: "L"
    $name: Cycle Layout
    $description: Switch to the next layout. Blank disables this shortcut.
  - SwapMasterKey: "M"
    $name: Layout Action
    $description: Rotate the focused window's parent split in BSP. In other tiled layouts, promote the focused window to the first slot. Blank disables this shortcut.
  - PromoteWindowKey: ","
    $name: Promote Focused Window
    $description: Move the focused window one place earlier. Blank disables this shortcut.
  - DemoteWindowKey: "."
    $name: Demote Focused Window
    $description: Move the focused window one place later. Blank disables this shortcut.
  - ManagementModeToggleKey: "R"
    $name: Toggle Window Management Mode
    $description: Switch between Automatic and Manual mode. Blank disables this shortcut.
  - FloatFocusedKey: "F"
    $name: Float Focused Window
    $description: Remove the focused window from tiling. Blank disables this shortcut.
  - WindowSize:
    - GrowKey: "="
      $name: Grow Focused Window
      $description: Make the focused window larger by one step. Hold to repeat. Blank disables this shortcut.
    - ShrinkKey: "-"
      $name: Shrink Focused Window
      $description: Make the focused window smaller by one step. Hold to repeat. Blank disables this shortcut.
    - ResetKey: "0"
      $name: Reset Focused Window Size
      $description: Return the focused window to its default size. Floating windows are also centered. Blank disables this shortcut.
    - StepPercent: 5
      $name: Step Size (%)
      #! $min: 1
      #! $max: 50
      $description: Size change per step, as a percentage of the work area.
    $name: Window Size
  - DiagnosticDumpKey: ""
    $name: Write Diagnostic Report
    $description: Save a diagnostic report to the Output Directory. Blank disables this shortcut.
  $name: Hotkeys

- diagnostics:
  - DiagnosticsOutputPath: ""
    $name: Output Directory
    #! $format: folderPath
    $description: Folder for diagnostic reports. Leave blank to use %USERPROFILE%\Documents\MultiWMDiagnostics. Reports include window titles and app paths; review them before sharing.
  $name: Diagnostics

- windowBehavior:
  - MouseMoveBehavior: float
    $name: Tiled Window Drag Action
    $description: What happens when you drag a tiled window.
    $options:
      - float: Float moved window
      - swap: Swap with tiled window under pointer
  - BspSplitEdgePercent: 25
    $name: BSP Split Edge Size
    #! $showIf: {MouseMoveBehavior: swap}
    #! $min: 0
    #! $max: 50
    #! $format: slider
    $description: Percentage of each BSP tile from an edge that triggers a split. A drop on an edge splits in that direction if both windows fit and the dragged pane overlaps that edge of the original tile. The center swaps windows. At 0 all drops swap window positions and keep the weights with the tiles. For windows sharing a divider, perpendicular edges rotate the split. The rest of the tile swaps the windows. Each window keeps its saved share.
  - FloatingDefaultSize: "960, 640"
    $name: Floating Default Size (DPI-scaled pixels)
    $description: Size of a window the first time it floats (Float Focused, a drag, the Floating layout) and of a new window that can't be tiled. Override-size rules replace it in Automatic mode. Enter width, height (100-4000).
  $name: Window Behavior

- windowRules:
  - Rules:
      - - Process: ""
          $name: App File Name
          $description: "Case-insensitive exact match. Leave blank to match any process. \n
            Separate multiple names with commas; escape a literal comma as \\,."
        - Class: ""
          $name: Window class
          $description: "Case-insensitive exact match. Leave blank to match any class. \n
            Separate multiple names with commas; escape a literal comma as \\,."
        - TitleContains: ""
          $name: Window title contains
          $description: "Case-insensitive substring match. Leave blank to match any title. \n
            Separate multiple fragments with commas; escape a literal comma as \\,."
        - Treatment: exclude
          $name: Treatment
          $options:
            - exclude: Exclude
            - default_to_floating: Default to floating
            - always_floating: Always floating
            - trace_to_owner: Trace to owner
            - open_on_monitor: Open on monitor / desktop
            - preserve_size_when_centering: Preserve size when centering
            - override_size_when_centering: Override size when centering
        - Monitor: any
          $name: Monitor
          #! $showIf: {Treatment: open_on_monitor}
          $description: Any keeps the current monitor.
          $options:
            - any: Any
            - primary: Primary
            - non_primary: Non-primary
            - specific: Specific monitor
        - MonitorId: ""
          $name: Monitor ID
          #! $showIf: {Treatment: open_on_monitor, Monitor: specific}
          $description: Paste a monitor ID from diagnostics. Blank disables this rule.
        - DesktopNumber: 0
          $name: Desktop Number
          #! $showIf: {Treatment: open_on_monitor}
          #! $min: 0
          $description: Position in Task View, starting at 1. Both desktop fields must match. 0 matches any number.
        - DesktopNameContains: ""
          $name: Desktop Name Contains
          #! $showIf: {Treatment: open_on_monitor}
          $description: Match part of a name, ignoring case. First match wins. Leave both desktop fields empty to stay.
        - FollowWindow: false
          $name: Follow Window
          #! $showIf: {Treatment: open_on_monitor}
          $description: Switch to the window's desktop after moving it.
        - Size: ""
          $name: Centering size override
          #! $showIf: {Treatment: override_size_when_centering}
          $description: 'Only effective when "Override size" is selected. Default-to-floating and Always-floating discovery always preserve the application''s size. Use Width, Height in DPI-scaled pixels, or "Workspace" to fill the current monitor''s usable workspace. Examples: 720, 480'
    $name: Rules
    $description: 
        "Both floating treatments center without changing the application's size. \n
        Trace to owner retargets move/size to its root owner. \n
        Every non-blank match field in a rule must match; comma-separated values within one field are alternatives, e.g. Thunderbird.exe, Librewolf.exe."
  $name: Window Rules
  $description: One shoe does not fit everyone, apparently...

- advanced:
  - ConformanceLeaseMs: 3000
    $name: Keep Windows in Place (ms)
    #! $min: 0
    #! $max: 10000
    $description: Keep windows in their tiles for this long after placement (0-10000 ms). Windows that resist will float. 0 disables this.
  - ConformanceRepairIntervalMs: 75
    $name: Position Repair Interval (ms)
    #! $min: 20
    #! $max: 2000
    #! $hideIf: {ConformanceLeaseMs: 0}
    $description: Time between attempts to keep windows in their tiles (20-2000 ms).
  - ReconcileDelayMs: 50
    $name: Window Change Delay (ms)
    #! $min: 20
    #! $max: 2000
    $description: Wait this long before handling window changes (20-2000 ms).
  - SlowApplicationDelayMs: 250
    $name: New Window Retry Delay (ms)
    #! $min: 0
    #! $max: 2000
    $description: Wait this long before checking a new window again (0-2000 ms). 0 disables retries.
  $name: Advanced
*/
// ==/WindhawkModSettings==

// Windhawk compiles a mod as ONE source file, so the headers below are not
// independent headers: they are chapters of this file, pasted in this order by
// the compiler (and by WHAM when it generates output/multiwm/multiwm.wh.cpp). A chapter
// may use anything from an earlier chapter. core/forward_declarations.h declares
// the few functions that are needed before their chapter.
//
// Read docs/ARCHITECTURE.md for what each folder owns and how an event flows.

// Shared with the MultiWM Companion mod (src/companion/multiwm-companion.cpp): rule
// matching, the snapshot ABI and geometry. Self-contained; no MultiWM chapter
// needed.
#include "../shared/window_rules.h"
#include "../shared/split_geometry.h"
#include "../shared/companion_snapshot.h"
#include "../shared/geometry.h"

// Shared vocabulary: platform declarations, settings, messages, model types, actor state.
#include "core/shared_types.h"
#include "model/bsp_tree.h"
#include "model/workspace.h"
#include "core/runtime_state.h"
#include "core/forward_declarations.h"

// Pure geometry and the observation/model layer.
#include "layout/algorithms.h"
#include "layout/bsp_layout.h"
#include "commands/gestures.h"
#include "app/configuration.h"
#include "platform/monitors.h"
#include "model/workspace_impl.h"
#include "platform/native_windows.h"
#include "placement/floating.h"
#include "model/repository.h"

// Planning and placing tiled windows.
#include "layout/planning.h"
#include "layout/resize_learning.h"
#include "layout/resize_steps.h"
#include "placement/arrangement.h"
#include "placement/conformance.h"
#include "placement/admission_prediction.h"

// Turning observed OS state into model changes, and user commands.
#include "reconcile/reconcile.h"
#include "reconcile/window_routing.h"
#include "commands/commands.h"

// Windows integration: WinEvent hooks and the private virtual-desktop COM API.
#include "platform/window_events.h"
#include "platform/virtual_desktop.h"

// Process lifetime, presentation and the WM thread's message loop.
#include "app/popup_surface.h"
#include "app/overlay.h"
#include "app/runtime_lifecycle.h"
#include "app/tray_ui.h"
#include "app/diagnostics.h"
#include "app/companion_snapshot.h"
#include "app/drag_preview.h"
#include "app/wm_thread.h"

//=============================================================================
// Windhawk Tool Mod Entry Points
//=============================================================================

BOOL WhTool_ModInit() {
  const std::scoped_lock lock(RuntimeLifecycle::g_lifecycle.mutex);
  Diagnostics::InitializeProcessRuntime();
  Wh_Log(L"MultiWM mod initializing...");
  Configuration::LoadSettings();
  g_wm.managementMode = g_settings.defaultManagementMode;
  // Optional: without it the companion mod gives no hints; MultiWM is unaffected.
  CompanionSnapshot::Open();
  if (!RuntimeLifecycle::StartHotkeyThread()) {
    Wh_Log(L"Failed to start hotkey thread");
    return FALSE;
  }
  Wh_Log(L"MultiWM mod initialized successfully");
  return TRUE;
}

void WhTool_ModUninit() {
  Wh_Log(L"MultiWM mod uninitializing...");
  // A settings reload still waiting for the old worker must not start a new one.
  RuntimeLifecycle::CancelDeferredRestart();
  const std::scoped_lock lock(RuntimeLifecycle::g_lifecycle.mutex);
  if (!RuntimeLifecycle::StopHotkeyThread()) {
    // The standard tool-mod wrapper exits this dedicated process immediately
    // after WhTool_ModUninit returns, so never create a second owner here.
    // The live worker may still write the snapshot, so leave it mapped; process
    // exit releases it and companions see MultiWM's liveness event disappear.
    Wh_Log(L"WM worker did not join before tool-process shutdown");
  } else {
    CompanionSnapshot::Close();
  }
  Wh_Log(L"MultiWM mod uninitialized");
}

// The rest of a settings reload, once the old worker has exited: loads the
// current settings and starts a new worker. The caller holds
// RuntimeLifecycle::g_lifecycle.mutex. A deferred reload runs this on a
// thread-pool thread: Windhawk's setting getters read storage afresh on each
// call with no thread affinity, and Windhawk itself doesn't promise one thread
// for this callback either.
static void FinishSettingsReload() {
  ++Diagnostics::g_runtime.counters.settingsReloads;
  Diagnostics::RecordEvent(L"settings reload requested");

  // Settings reload changes defaults/policies but does not destroy per-desktop
  // workspace identity, layout mode, ordering, or learned ratios. The current
  // management mode is kept too, unless its default setting is what changed.
  const ManagementMode previousDefaultMode = g_settings.defaultManagementMode;
  Configuration::LoadSettings();
  if (g_settings.defaultManagementMode != previousDefaultMode) {
    g_wm.managementMode = g_settings.defaultManagementMode;
  }
  if (!RuntimeLifecycle::StartHotkeyThread()) {
    Wh_Log(L"Failed to restart WM worker after settings change");
  }
}

void WhTool_ModSettingsChanged() {
  Wh_Log(L"Settings changed, reloading...");
  const std::scoped_lock lock(RuntimeLifecycle::g_lifecycle.mutex);
  if (RuntimeLifecycle::g_lifecycle.deferredWait) {
    Wh_Log(L"A reload is already waiting for the previous WM worker; it will read these settings");
    return;
  }

  // Join the old diagnostics/model owner before touching lifetime telemetry. Two
  // WM actors must never overlap inside the dedicated tool-mod process, so if it
  // is still busy, finish the reload once it exits: it already has WM_QUIT.
  if (!RuntimeLifecycle::StopHotkeyThread()) {
    if (RuntimeLifecycle::DeferUntilWorkerExits(FinishSettingsReload)) {
      Wh_Log(L"Settings reload deferred until the previous WM worker exits");
    } else {
      Wh_Log(L"Settings reload abandoned; MultiWM restarts at the next settings change");
    }
    return;
  }
  FinishSettingsReload();
}

#include "windhawk/tool_mod_launcher.h"
