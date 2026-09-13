// ==WindhawkMod==
// @id              multiwm
// @name            MultiWM
// @description     Lightweight, low-cortisol window manager with true per-virtual-desktop layouts. 
// @version         1.13.42
// @author          meteoni
// @github          https://github.com/Meteony
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

![GIF](https://raw.githubusercontent.com/Meteony/meteoni-assets/main/MultiWM/MultiWM.gif)

## Notes
- *MultiWM launches a dedicated `explorer.exe -tool-mod` worker for compatibility with Windhawk 1.7.x. Restarting every Explorer process also restarts MultiWM and loses its in-memory workspace state; Windhawk 2.0 (or higher) users can change the target process (`explorer.exe` -> `windhawk.exe`) for better preservation across Explorer restarts.*
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
- Dwindle — recursive splits alternate side-by-side and top/bottom, starting
  side-by-side. Resizing changes split ratios without rotating dividers.
  Constraint recovery can automatically reshape a configurable number of leading
  slots; set Adaptive Dwindle Slots to 0 to keep automatic recovery on fixed ratios.
- Columns / Rows
- Monocle

## Getting Started
Move a tiled window to **float it** or **swap it with the window underneath**. 

By default, hotkeys are available for tiling (Alt+T), changing layouts (Alt+L), setting the master window (Alt+M),
moving windows through the logical order (Alt+, / .), floating a window (Alt+F), and switching between Automatic
and Manual management (Alt+R).

A small tray indicator shows the active workspace layout and provides quick access to layouts and management mode.


## Also check out
- [Virtual Desktop Helper by u2x1](https://windhawk.net/mods/virtual-desktop-helper)
- [Taskbar Desktop Indicator by Simon Benedict](https://windhawk.net/mods/taskbar-desktop-indicator)

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- general:
  - DefaultWindowManagementMode: automatic
    $name: Default Window Management Mode
    $description: 'Choose membership policy after startup. Manual keeps Alt+T groups contained: new windows are not admitted, minimizing / moving to another desktop releases membership. Maximized windows still preserve their slot. Automatic continuously discovers and migrates managed windows.'
    $options:
      - manual: Manual
      - automatic: Automatic
  - AutomaticNewWindowPosition: last_slot
    $name: Automatic Window Insertion Position
    $description: 'Choose the preferred position for newly discovered windows and migrated windows from another workspace. If constraints prevent it, MultiWM tries the nearest viable slot. Maximized / minimized windows are restored in-place.'
    $options:
      - last_slot: Last slot
      - after_focused: After focused window
  $name: General

- workspace:
  - DefaultLayout: master_stack
    $name: Default Layout
    $description: Initial layout used when a new desktop + monitor workspace is created.
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
    $description: 'Layouts visited by the Cycle Layout hotkey, in order. Remove layouts you never want to cycle through, or re-order entries as you wish.'
    $options:
      - master_stack: Master + Stack (Vertical)
      - master_stack_h: Master + Stack (Horizontal)
      - columns: Columns
      - rows: Rows
      - dwindle: Dwindle
      - monocle: Monocle (Fullscreen)
      - floating: Floating (No tiling)
  - TileGap: 6
    $name: Window Gap (DPI-scaled pixels)
    $description: Gap between adjacent tiled windows, scaled per monitor.
  - WorkspaceInsets: "6, 6, 6, 6"
    $name: Insets (DPI-scaled pixels)
    $description: 'Left, Top, Right, Bottom (0-500), scaled per monitor. Positive values inset the workspace from that edge. Example: 4, 4, 4, 4'
  - MasterPercent: 50
    $name: Master Size (%)
    $description: Default master share in Master + Stack layouts (1-99).
  - ReverseDwindle: false
    $name: Reverse Dwindle
    $description: Reverse the recursive growth direction from bottom-right to top-left.
  - AdaptiveDwindleSlots: 2
    $name: Adaptive Dwindle Slots
    $description: 'Number of leading Dwindle slots whose split ratios may be adjusted automatically to satisfy proven window minimum sizes during admission and eligible exchanges. Set to 0 for fixed-ratio Dwindle behavior. Manual divider resizing is unaffected. (0-64)'
  $name: Workspace

- appearance:
  - FlyoutPosition: top
    $name: Status Flyout Position
    $description: Show layout and mode flyout at the top or bottom center of the active monitor.
    $options:
      - top: Top
      - bottom: Bottom
  - FlyoutOffsetX: 0
    $name: Status Flyout X Offset
    $description: Horizontal offset in DPI-scaled pixels (positive moves right).
  - FlyoutOffsetY: 0
    $name: Status Flyout Y Offset
    $description: Vertical offset in DPI-scaled pixels (positive moves down).
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
          $name: Icon path (.ico)
    $name: Custom Tray Layout Icons
    $description: Optional per-layout .ico overrides. Failed or empty paths use the generated text icon.
  $name: Appearance

- hotkeys:
  - TilingModifier: alt
    $name: Modifier
    $description: "Modifier used by all window-manager hotkeys. \nNote: Alt+letter shortcuts can override application menu mnemonics. Choose Alt+Shift or Ctrl+Alt if this is disruptive. "
    $options:
      - alt: Alt
      - ctrl: Ctrl
      - alt+shift: Alt + Shift
      - ctrl+alt: Ctrl + Alt
      - ctrl+shift: Ctrl + Shift
  - TileKey: "T"
    $name: Tile Workspace
    $description: 'Reconcile and tile windows on the current monitor. Leave blank to disable. Examples: T, D, Space, `, -'
  - LayoutKey: "L"
    $name: Cycle Layout
    $description: 'Cycle the current workspace layout. Leave blank to disable. Examples: L, Tab, =, ], /'
  - SwapMasterKey: "M"
    $name: Set Master Window
    $description: 'Set another tiled window as the master. Leave blank to disable.'
  - PromoteWindowKey: ","
    $name: Promote Focused Window
    $description: 'Move the focused tiled window one logical slot earlier. Does nothing in the first slot. Leave blank to disable.'
  - DemoteWindowKey: "."
    $name: Demote Focused Window
    $description: 'Move the focused tiled window one logical slot later. Does nothing in the last slot. Leave blank to disable.'
  - ManagementModeToggleKey: "R"
    $name: Toggle Window Management Mode
    $description: 'Switch between Automatic and Manual modes at runtime. Leave blank to disable.'
  - FloatFocusedKey: "F"
    $name: Float Focused Window
    $description: 'Float the focused tiled window. Leave blank to disable.'
  - DiagnosticDumpKey: ""
    $name: Write Diagnostic Report
    $description: 'Write a formatted state, health, churn, and invariant report to the configured output directory. Uses the configured modifier. Leave blank to disable.'
  $name: Hotkeys

- diagnostics:
  - DiagnosticsOutputPath: "%USERPROFILE%\\Documents\\MultiWMDiagnostics"
    $name: Output Directory
    $description: 'Directory for timestamped UTF-8 .txt diagnostic reports. Environment variables such as %USERPROFILE% are supported and missing directories are created. Reports include window titles and executable paths, so review them before sharing.'
  $name: Diagnostics

- windowBehavior:
  - MouseMoveBehavior: float
    $name: Tiled Window Drag Action
    $description: Action when a tiled window is dragged without resizing.
    $options:
      - float: Float moved window
      - swap: Swap with tiled window under pointer
  - FloatingDefaultSize: "960, 640"
    $name: Floating Default Size (DPI-scaled pixels)
    $description: 'Width, Height used for newly discovered windows in Floating workspaces (Automatic mode only). Values must be 100-4000. Scaled per monitor. Example: 960, 640'
  $name: Window Behavior

- windowRules:
  - Rules:
      - - Process: ""
          $name: Process name
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
            - trace_to_owner: Trace to owner
            - preserve_size_when_centering: Preserve size when centering
            - override_size_when_centering: Override size when centering
        - Size: ""
          $name: Centering size override
          $description: 'Only effective when "Override size" is selected. Use Width, Height in DPI-scaled pixels, or "Workspace" to fill the current monitor''s usable workspace. Examples: 720, 480'
    $name: Rules
    $description: "There's no one-size-fits-all shoe. \n
      Exclude prevents management. Default to floating admits a matching window as managed Floating during Automatic discovery. Trace to owner retargets move/size to its root owner. Every non-blank match field in a rule must match; comma-separated values within one field are alternatives."
  $name: Window Rules

- advanced:
  - ConformanceLeaseMs: 3000
    $name: Tiled Window Conformance Lease (ms)
    $description: 'Briefly reinforces the assigned rectangle after placement in case the application moves itself again. A still-tiled window that remains outside its tile is floated and the remaining layout reflows. Set to 0 to disable (0-10000 ms). [Default 3000]'
  - ConformanceRepairIntervalMs: 75
    $name: Conformance Repair Interval (ms)
    $description: 'Minimum delay between attempts to return a tiled window to its assigned rectangle during the conformance lease (20-2000 ms). [Default 75]'
  - ReconcileDelayMs: 50
    $name: Lifecycle Settle Delay (ms)
    $description: Delay before visibility/lifecycle bursts are reconciled. Increase only if shell transitions are unusually slow (20-2000 ms).
  - SlowApplicationDelayMs: 250
    $name: Slow Application Discovery Delay (ms)
    $description: 'One-time delay before rechecking an Automatic-mode window that was not ready for admission after lifecycle settling. Set to 0 to disable (0-2000 ms). [Default 250]'
  $name: Advanced
*/
// ==/WindhawkModSettings==

#include "includes/multiwm_core.h"
#include "includes/multiwm_gestures.h"
#include "includes/multiwm_configuration_helpers.h"
#include "includes/multiwm_layout.h"
#include "includes/multiwm_window_observation.h"
#include "includes/multiwm_workspace_repository.h"
#include "includes/multiwm_workspace_operations.h"
#include "includes/multiwm_reconcile.h"
#include "includes/multiwm_commands.h"
#include "includes/multiwm_window_events.h"
#include "includes/multiwm_virtual_desktop.h"
#include "includes/multiwm_runtime_lifecycle.h"
#include "includes/multiwm_tray_ui.h"
#include "includes/multiwm_diagnostics.h"
#include "includes/multiwm_bootstrap.h"

//=============================================================================
// Windhawk Tool Mod Entry Points
//=============================================================================

BOOL WhTool_ModInit() {
  Diagnostics::InitializeProcessRuntime();
  Wh_Log(L"MultiWM mod initializing...");
  LoadSettings();
  if (!StartHotkeyThread()) {
    Wh_Log(L"Failed to start hotkey thread");
    return FALSE;
  }
  Wh_Log(L"MultiWM mod initialized successfully");
  return TRUE;
}

void WhTool_ModUninit() {
  Wh_Log(L"MultiWM mod uninitializing...");
  if (!StopHotkeyThread()) {
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
  if (!StopHotkeyThread()) {
    Wh_Log(L"Settings reload aborted because the existing WM worker is still alive");
    return;
  }

  ++Diagnostics::g_runtime.counters.settingsReloads;
  Diagnostics::RecordEvent(L"settings reload requested");

  // Settings reload changes defaults/policies but does not destroy per-desktop
  // workspace identity, layout mode, ordering, or learned ratios.
  LoadSettings();
  if (!StartHotkeyThread()) {
    Wh_Log(L"Failed to restart WM worker after settings change");
  }
}

//=============================================================================
// Windhawk tool mod implementation for mods which don't need to inject into other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
//
// This mod launches a dedicated copy of its configured host executable.
//=============================================================================

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

// Ends only the dedicated worker's bootstrap thread at its normal entry point;
// the already-started hotkey thread keeps the tool process alive.
void WINAPI EntryPoint_Hook() {
  Wh_Log(L">");
  ExitThread(0);
}

// Routes each host executable instance into its tool-mod role. Normal instances act
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

// Launcher-side phase: starts a copy of the current host executable tagged as this mod's
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

