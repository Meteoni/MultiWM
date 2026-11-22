// ==WindhawkMod==
// @id              multiwm-companion
// @name            MultiWM Companion
// @description     Opens new windows directly in the slot MultiWM will give them, so they don't appear elsewhere first and jump.
// @version         0.3.2
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

An optional add-on for [MultiWM](https://github.com/Meteoni/MultiWM). Without it,
a new window first appears where the app puts it, then jumps into its tile. With
it, the window opens in its tile directly.

## What it does
- New windows open in the tile MultiWM will give them, with your gap, insets and
  window rules applied.
- Windows restored from the taskbar or the tray reappear in their own tile.
- Windows that will float (by a rule, in the Floating layout, or because they
  don't fit) open where they'll float.
- If a guess turns out wrong (say the app resizes itself right after opening),
  MultiWM moves the window as usual.

Dialogs, tool windows, excluded windows, and windows that open maximized or
fullscreen are left alone.

## Requirements
- MultiWM installed and running, in Automatic mode. In Manual mode this mod does
  nothing.
- Update MultiWM and the companion together; mismatched versions may give no hints.

## Compatibility
Don't combine with other mods that move windows before they appear, such as
Window Centralizer or Center New Windows: both would move the same window.

## Troubleshooting
Enable logging for this mod in Windhawk to see each placement decision.
*/
// ==/WindhawkModReadme==

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
#include "../shared/split_geometry.h"
#include "../shared/companion_snapshot.h"
#include "../shared/geometry.h"

#include "snapshot_reader.h"
#include "placement.h"
#include "restoration.h"
#include "show_hooks.h"

BOOL Wh_ModInit() {
  if (!Companion::InstallShowHooks()) Wh_Log(L"Some show hooks could not be installed");
  return TRUE;
}

void Wh_ModUninit() {
  Companion::CloseSnapshotReader();
}
