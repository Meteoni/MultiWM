// ==WindhawkMod==
// @id              tiling-helper-experimental
// @name            Tiling Helper Experimental
// @description     Tile windows on the current monitor with customizable layouts and hotkeys
// @version         1.13.8
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
- Layout cycle hotkey with a user-configurable per-layout cycle order
- 7 per-workspace modes: Master+Stack (Vertical/Horizontal), Columns, Rows, BSP, Monocle, Floating
- DPI-aware workspace insets/window gaps and independent master size
- Remembers layout mode, ordering, and resize adjustments per virtual desktop + monitor
- Uses native virtual-desktop notifications with centralized transient-failure retry; settled cloak/visibility reconciliation independently detects missed desktop switches as a safety fallback
- Checked placement with per-window failure tracking, a configurable short conformance lease for late application self-moves, and automatic floating for repeatedly non-compliant windows
- Manual or Automatic management modes: Manual keeps Alt+D groups inert, while Automatic continuously discovers and migrates windows
- Configurable mouse drag behavior: float the moved window or swap it with the tiled window under the pointer
- Dedicated optional hotkey to float the focused tiled window
- Theme-aware notification-area text icon shows the active/last-targeted monitor workspace layout (MV/MH/CL/RW/BS/MN/FL), with a multi-monitor tooltip summary when more than one display is connected
- Standard Win32 tray context menu tiles the represented workspace, offers one management-mode switch action, and selects layouts directly in the user-configured cycle order
- Optional per-layout .ico overrides with generated text fallback
- Compact Win32 status flyouts appear on the affected monitor for layout/mode changes and on every monitor after a settled virtual-desktop switch
- Workspace-level hotkeys target the monitor under the mouse cursor; focused-window commands continue to target the focused window's physical monitor
- Manual Tile globally reconciles known windows, then adopts current tiled geometry when the full tiled set is visible
- Scoped reconciliation keeps lifecycle, virtual-desktop, and monitor ownership repair separate; every connected monitor is visited at startup and authoritative topology passes
- Maximized windows preserve their saved tile and use a targeted location-change fallback to detect restore transitions reliably
- Cross-workspace repair preserves unaffected resize weights; only the migrated window gets a default destination weight
- Known HWND ownership is globally unique; current-desktop discovery migrates existing records instead of duplicating them across workspaces
- Optional process/class/title exclusion rules keep user-selected windows out of management
- Hotkey diagnostics write a timestamped UTF-8 text report with runtime/churn counters, diagnostic self-cost accounting, significant/lifecycle histories, platform health, telemetry/invariant audits, and deterministic per-workspace/window state
- New-workspace initialization discovers visible minimized/maximized windows as suspended logical members instead of losing them
- Hidden, minimized, and maximized tiled windows preserve their exact logical slot/weight while temporarily leaving active layout
- Native VD switches settle shell state, then softly reinforce every known workspace on the incoming desktop

## Default Hotkeys
- Tile windows: Alt + D
- Cycle layout: Alt + L
- Switch master window: Alt + M
- Float focused window: unbound by default
- Toggle Automatic/Manual mode: Alt + U
- Write diagnostic report: Alt + I

## Notes
- Layout mode, ordering, and resize state are stored per virtual desktop + monitor.
- Floating exists both as a workspace mode and as a per-window managed state; remembered floating geometry is restored on migration, explicit floating, and workspace layout transitions into Floating.
- Private virtual-desktop manager and notification ABIs are selected automatically from the actual Explorer build/revision; unknown manager generations are rejected instead of guessed.
- If Virtual Desktop APIs are unavailable, tiling still works but per-desktop memory is disabled.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- DefaultWindowManagementMode: manual
  $name: '[General] Default Window Management Mode'
  $description: 'Choose membership policy after startup. Manual keeps Alt+D groups inert: new windows are not admitted, minimizing releases membership, and moving a managed window to another virtual desktop does not carry it into that desktop. Maximized windows still preserve their slot. Automatic continuously discovers and migrates managed windows.'
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

- LayoutCycle: [master_stack, columns, rows, master_stack_h, bsp, monocle, floating]
  $name: '[Workspace] Layout Cycle'
  $description: 'Layouts visited by the Cycle Layout hotkey, in order. Remove layouts you never want to cycle through or drag entries to reorder them. If the current layout is not in the list, the next cycle starts at the first entry.'
  $options:
    - master_stack: Master + Stack (Vertical)
    - master_stack_h: Master + Stack (Horizontal)
    - columns: Columns
    - rows: Rows
    - bsp: BSP (Binary Space Partition)
    - monocle: Monocle (Fullscreen)
    - floating: Floating (managed, no automatic placement)

- TileGap: 4
  $name: '[Workspace] Window Gap (DPI-scaled pixels)'
  $description: Gap between adjacent tiled windows at 96 DPI (0-100); scaled per monitor.

- WorkspaceInsets: "4, 4, 4, 4"
  $name: '[Workspace] Insets (DPI-scaled pixels)'
  $description: 'Left, Top, Right, Bottom at 96 DPI. Positive values inset the workspace from that edge; each value must be 0-500 and is scaled per monitor. Example: 4, 4, 4, 4'

- MasterPercent: 50
  $name: '[Workspace] Master Size (%)'
  $description: Default master share in Master + Stack layouts (1-99).

- MouseMoveBehavior: float
  $name: '[Window Behavior] Tiled Window Drag Action'
  $description: Action when a tiled window is moved without resizing.
  $options:
    - float: Float moved window
    - swap: Swap with tiled window under pointer

- FloatingDefaultSize: "960, 640"
  $name: '[Floating] Default Size (DPI-scaled pixels)'
  $description: 'Fallback Width, Height at 96 DPI when no remembered floating size exists. Each value must be 100-4000 and is scaled per monitor. Example: 960, 640'

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

- ConformanceLeaseMs: 1500
  $name: '[Advanced] Tiled Window Conformance Lease (ms)'
  $description: 'After an authoritative tiled placement, briefly reinforce the assigned rectangle if the application moves itself again. User move/resize gestures cancel the lease immediately. Set to 0 to disable (0-10000 ms). [Default 1500]'

- ReconcileDelayMs: 80
  $name: '[Advanced] Lifecycle Settle Delay (ms)'
  $description: Delay before visibility/lifecycle bursts are reconciled. Increase only if shell transitions are unusually slow (20-2000 ms).

- DiagnosticsOutputPath: "%USERPROFILE%\\Documents\\TilingHelperDiagnostics"
  $name: '[Diagnostics] Output Directory'
  $description: 'Directory for timestamped UTF-8 .txt diagnostic reports. Environment variables such as %USERPROFILE% are supported and missing directories are created. Reports include window titles and executable paths, so review them before sharing.'

- DiagnosticDumpKey: "I"
  $name: '[Diagnostics] Write Diagnostic Report'
  $description: 'Write a formatted state, health, churn, and invariant report to the configured output directory. Uses the configured modifier. Leave blank to disable.'
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
  if (!StopHotkeyThread()) {
    // The standard tool-mod wrapper exits this dedicated process immediately
    // after WhTool_ModUninit returns, so never create a second owner here.
    Wh_Log(L"WM worker did not join before tool-process shutdown");
  }
  Wh_Log(L"Tiling Helper mod uninitialized");
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

