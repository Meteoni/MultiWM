// ==WindhawkMod==
// @id              multiwm-companion
// @name            MultiWM Companion
// @description     Opens new windows directly in the slot MultiWM will give them, so they don't appear elsewhere first and jump.
// @version         0.1.0
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

Without it, a new window opens wherever the application puts it, and MultiWM moves it
into its tile a moment later. With it, the window opens directly in that tile: the next
slot of the current layout, or MultiWM's default floating position.

## How it works
- MultiWM publishes, read-only, where the next window on each monitor would go, along
  with its window rules.
- Just before a window is shown for the first time, this mod checks whether MultiWM
  would manage it, and if so moves it there. That is all it does: it never moves a
  window again, and it never talks back to MultiWM.
- The hinted size is clamped to the window's reported size limits, keeping the
  predicted position. MultiWM may refine the layout afterward.
- Asynchronous shows are hinted only on the window's owning thread. Deferred
  positioning keeps the application's batch timing. Windows that paint during
  visible creation may appear before the creation hook can place them.
- If the guess is wrong (for example, the app changes its title or minimum size right
  after opening), MultiWM moves the window as it always does.

Windows MultiWM doesn't manage are left alone: dialogs, tool windows, windows
excluded by a rule, windows in Manual mode, and windows opening maximized or
fullscreen.

## Compatibility
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
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Shared with MultiWM (src/multiwm/multiwm.cpp): rule matching and the snapshot ABI.
#include "../shared/window_rules.h"
#include "../shared/companion_snapshot.h"

#include "snapshot_reader.h"
#include "placement.h"
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
