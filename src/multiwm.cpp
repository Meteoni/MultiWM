// ==WindhawkMod==
// @id              multiwm
// @name            MultiWM
// @description     Lightweight, low-cortisol window manager with true per-virtual-desktop layouts. 
// @version         1.13.59
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
- Columns / Rows
- Monocle

## Getting Started
Move a tiled window to **float it** or **swap it with the window underneath**. 

By default, hotkeys are available for tiling (Alt+T), changing layouts (Alt+L), setting the master window (Alt+M),
moving windows through the logical order (Alt+, / .), floating a window (Alt+F), and switching between Automatic
and Manual management (Alt+R).

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
    $description: Automatic adds windows as they appear. Manual adds them when you choose Tile Workspace.
    $options:
      - manual: Manual
      - automatic: Automatic
  - AutomaticNewWindowPosition: last_slot
    $name: Automatic Window Insertion Position
    $description: Preferred position for new or moved windows in Automatic mode. Tries another position if needed.
    $options:
      - last_slot: Last slot
      - after_focused: After focused window
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
      - monocle: Monocle (Fullscreen)
      - floating: Floating (No tiling)
  - LayoutCycle: [master_stack, master_stack_h, dwindle, columns, rows, monocle, floating]
    $name: Layout Cycle
    $description: Layouts used by Cycle Layout, in this order.
    $options:
      - master_stack: Master + Stack (Vertical)
      - master_stack_h: Master + Stack (Horizontal)
      - columns: Columns
      - rows: Rows
      - dwindle: Dwindle
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
      $description: Applies when tiled windows exceed the limit or cannot fit. Created virtual desktops stay after their windows close or MultiWM is turned off.
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
    $description: Space at the left, top, right, and bottom edges. Enter four values from 0 to 500.
  - MasterPercent: 50
    $name: Master Size (%)
    #! $min: 1
    #! $max: 99
    #! $format: slider
    $description: Starting screen share for the main window in Master + Stack layouts (1-99%).
  - ReverseDwindle: false
    $name: Reverse Dwindle
    $description: Grow the Dwindle layout from the top-left instead of the bottom-right.
  - AdaptiveDwindleSlots: 2
    $name: Adaptive Dwindle Slots
    #! $min: 0
    #! $max: 64
    $description: How many splits can temporarily adjust to fit windows (0-64). Splits return when the extra space is no longer needed. Manually resizing a divider saves its new position. 0 keeps all splits fixed.
  - AdaptiveDwindlePercentage: 15
    $name: Adaptive Dwindle Percentage
    #! $min: 0
    #! $max: 40
    #! $format: slider
    #! $hideIf: {AdaptiveDwindleSlots: 0}
    $description: How far splits may move from the middle (0-40). 15 allows 35%-65%. 0 disables adjustment.
  $name: Workspace

- appearance:
  - ShowToasts: true
    $name: Show Toasts
    $description: Show layout, mode, and error toasts.
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
    $description: Move messages left or right. Positive values move right.
  - FlyoutOffsetY: 0
    $name: Toast Vertical Offset (DPI-scaled pixels)
    #! $showIf: {ShowToasts: true}
    $description: Move messages up or down. Positive values move down.
  - CustomIcons:
      - - Layout: master_stack
          $name: Layout
          $options:
            - master_stack: Master + Stack (Vertical)
            - master_stack_h: Master + Stack (Horizontal)
            - columns: Columns
            - rows: Rows
            - dwindle: Dwindle
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
    $name: Set Master Window
    $description: Make the focused window the main window. Blank disables this shortcut.
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
  - FloatingDefaultSize: "960, 640"
    $name: Floating Default Size (DPI-scaled pixels)
    $description: Starting size in Floating layouts. Enter width, height (100-4000). Automatic mode only.
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
// the compiler (and by WHAM when it generates output/multiwm.wh.cpp). A chapter
// may use anything from an earlier chapter. core/forward_declarations.h declares
// the few functions that are needed before their chapter.
//
// Read docs/ARCHITECTURE.md for what each folder owns and how an event flows.

// Shared vocabulary: platform declarations, settings, messages, model types, actor state.
#include "core/shared_types.h"
#include "model/workspace.h"
#include "core/runtime_state.h"
#include "core/forward_declarations.h"

// Pure geometry and the observation/model layer.
#include "layout/algorithms.h"
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
#include "placement/arrangement.h"
#include "placement/conformance.h"

// Turning observed OS state into model changes, and user commands.
#include "reconcile/reconcile.h"
#include "reconcile/window_routing.h"
#include "commands/commands.h"

// Windows integration: WinEvent hooks and the private virtual-desktop COM API.
#include "platform/window_events.h"
#include "platform/virtual_desktop.h"

// Process lifetime, presentation and the WM thread's message loop.
#include "app/runtime_lifecycle.h"
#include "app/tray_ui.h"
#include "app/diagnostics.h"
#include "app/wm_thread.h"

//=============================================================================
// Windhawk Tool Mod Entry Points
//=============================================================================

BOOL WhTool_ModInit() {
  Diagnostics::InitializeProcessRuntime();
  Wh_Log(L"MultiWM mod initializing...");
  Configuration::LoadSettings();
  if (!RuntimeLifecycle::StartHotkeyThread()) {
    Wh_Log(L"Failed to start hotkey thread");
    return FALSE;
  }
  Wh_Log(L"MultiWM mod initialized successfully");
  return TRUE;
}

void WhTool_ModUninit() {
  Wh_Log(L"MultiWM mod uninitializing...");
  if (!RuntimeLifecycle::StopHotkeyThread()) {
    // The standard tool-mod wrapper exits this dedicated process immediately
    // after WhTool_ModUninit returns, so never create a second owner here.
    Wh_Log(L"WM worker did not join before tool-process shutdown");
  }
  Wh_Log(L"MultiWM mod uninitialized");
}

void WhTool_ModSettingsChanged() {
  Wh_Log(L"Settings changed, reloading...");

  // Join the old diagnostics/model owner before touching lifetime telemetry.
  // If it cannot stop, keep the old worker/settings rather than allowing two WM
  // actors to overlap inside the dedicated tool-mod process.
  if (!RuntimeLifecycle::StopHotkeyThread()) {
    Wh_Log(L"Settings reload aborted because the existing WM worker is still alive");
    return;
  }

  ++Diagnostics::g_runtime.counters.settingsReloads;
  Diagnostics::RecordEvent(L"settings reload requested");

  // Settings reload changes defaults/policies but does not destroy per-desktop
  // workspace identity, layout mode, ordering, or learned ratios.
  Configuration::LoadSettings();
  if (!RuntimeLifecycle::StartHotkeyThread()) {
    Wh_Log(L"Failed to restart WM worker after settings change");
  }
}

#include "windhawk/tool_mod_launcher.h"
