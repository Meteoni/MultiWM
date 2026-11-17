// ==WindhawkMod==
// @id              multiwm-companion
// @name            MultiWM Companion
// @description     Opens new windows directly in the slot MultiWM will give them, so they don't appear elsewhere first and jump.
// @version         0.2.0
// @author          meteoni
// @github          https://github.com/Meteoni
// @license         MIT
// @include         *
// @exclude         windhawk.exe
// @exclude         windhawk-mod.exe
// @exclude         windhawk-mod-uiaccess.exe
// @exclude         ShellExperienceHost.exe
// @exclude         StartMenuExperienceHost.exe
// @exclude         SearchHost.exe
// @exclude         LockApp.exe
// @exclude         SystemSettings.exe
// @exclude         TextInputHost.exe
// @exclude         Widgets.exe
// @exclude         ctfmon.exe
// @exclude         GameBarPresenceWriter.exe
// @exclude         backgroundTaskHost.exe
// @exclude         SecurityHealthSystray.exe
// @compilerOptions -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# MultiWM Companion

An add-on for [MultiWM](https://github.com/Meteoni/MultiWM). Requires MultiWM to be
installed and running.

The companion prepares new windows and supported restores for their MultiWM
destination, reducing the visible jump when MultiWM arranges them afterward.

## How it works
- Placement hints, including restoration, apply only in Automatic mode. In Manual
  mode this mod passes the application's placement and restore calls through.
- MultiWM publishes, read-only, where the next window on each monitor would go, along
  with its window rules.
- Just before a window is shown for the first time, this mod checks whether MultiWM
  would manage it, and if so moves it there. It never follows ongoing window
  movement and never talks back to MultiWM.
- Predicted tiles include the configured gap and workspace insets. Live minimum
  and maximum sizes are converted for the window's DPI; capped windows are centred
  within their tile. Minimum-size growth can still need a later layout correction.
- Asynchronous shows are hinted only on the window's owning thread. Deferred
  positioning keeps the application's batch timing. Windows that paint during
  visible creation may appear before the creation hook can place them.
- For each suspended member, MultiWM publishes the tile calculated by restoring
  its saved slot on a workspace copy, including saved weights and size limits.
  The companion uses measured resize borders to prepare Windows' restore rectangle
  just before explicit show/placement calls or default `SC_RESTORE` handling on
  the owning thread. Taskbar restores in the tested apps reached this path.
- Known windows hidden to the tray without minimizing also use their own restore
  tile when shown again, including saved and deferred placements. Missing restore
  hints leave them untouched rather than giving them a new-window slot.
- Definite overflow floats can open at the centred floating position when MultiWM
  will keep them on the current monitor. With one monitor and desktop creation off,
  Try other monitors first also uses this path. Routed destinations are not hinted.
- If the guess is wrong (for example, the app changes its title or minimum size right
  after opening), MultiWM moves the window as it always does.

Dialogs, tool windows, excluded windows and new maximized/fullscreen windows
are left alone. Restore-to-maximized and missing, stale-DPI or unavailable restore
information also pass through. The companion does not pre-position minimized
windows in the background or change MultiWM's saved slots and final layout rules.

## Compatibility
Both mods must use snapshot ABI major 4. Version 0.2.0 understands ABI 4.1;
4.0 peers remain compatible but do not provide the new overflow float hints.
Don't combine with mods that also move windows before they are shown, such as
Window Centralizer or Center New Windows: both would move the same window.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- logDecisions: false
  $name: Log decisions
  $description: Write each placement decision to the Windhawk log, for troubleshooting.
*/
// ==/WindhawkModSettings==

// Like src/multiwm/multiwm.cpp, the headers below are chapters of this one file, pasted
// in order by WHAM into output/companion/multiwm-companion.wh.cpp. This mod runs inside
// every application, so it stays small: one decision per window as it is shown.

#include <windows.h>
#include <dwmapi.h>
#include <windhawk_utils.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Shared with MultiWM (src/multiwm/multiwm.cpp): rule matching and the snapshot ABI.
#include "../shared/window_rules.h"
#include "../shared/companion_snapshot.h"
#include "../shared/geometry.h"

#include "snapshot_reader.h"
#include "placement.h"
#include "restoration.h"
#include "show_hooks.h"

BOOL Wh_ModInit() {
  Companion::LoadSettings();
  if (!Companion::InstallShowHooks()) Wh_Log(L"Some show hooks could not be installed");
  return TRUE;
}

void Wh_ModSettingsChanged() {
  Companion::LoadSettings();
}

void Wh_ModUninit() {
  Companion::CloseSnapshotReader();
}
