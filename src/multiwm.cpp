// ==WindhawkMod==
// @id              tiling-helper-experimental
// @name            Tiling Helper Experimental
// @description     Tile windows on the current monitor with customizable layouts and hotkeys
// @version         1.11.0
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
#include "includes/multiwm_tray_ui.h"
#include "includes/multiwm_diagnostics.h"
#include "includes/multiwm_bootstrap.h"

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

