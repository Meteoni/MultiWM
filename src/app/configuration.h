// Settings parsing and loading.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Configuration {

UINT ParseModifiers(PCWSTR str) {
  UINT modifiers = 0;
  if (wcsstr(str, L"alt")) modifiers |= MOD_ALT;
  if (wcsstr(str, L"ctrl")) modifiers |= MOD_CONTROL;
  if (wcsstr(str, L"shift")) modifiers |= MOD_SHIFT;
  return modifiers;
}

static bool TryParseLayoutSetting(PCWSTR str, TileLayout* outLayout) {
  if (!str || !outLayout) return false;
  static const std::pair<PCWSTR, TileLayout> kLayoutMap[] = {
      {L"master_stack", TileLayout::MasterStack},
      {L"master_stack_h", TileLayout::MasterStackH},
      {L"columns", TileLayout::Columns},
      {L"rows", TileLayout::Rows},
      {L"dwindle", TileLayout::Dwindle},
      // Compatibility alias for configurations saved before 1.13.29.
      {L"bsp", TileLayout::Dwindle},
      {L"monocle", TileLayout::Monocle},
      {L"floating", TileLayout::Floating},
  };
  for (const auto& entry : kLayoutMap) {
    if (wcscmp(str, entry.first) == 0) {
      *outLayout = entry.second;
      return true;
    }
  }
  return false;
}

TileLayout ParseLayoutSetting(PCWSTR str) {
  TileLayout layout = TileLayout::MasterStack;
  TryParseLayoutSetting(str, &layout);
  return layout;
}

template <typename T, typename Parser>
T ReadHotkeySetting(PCWSTR name, Parser parser) {
  auto value = WindhawkUtils::StringSetting::make(name);
  if (!*value.get()) return T{};  // Blank hotkey = unused.

  T result = parser(value.get());
  if (!result) {
    Wh_Log(
        L"Invalid %ls hotkey setting '%ls'; hotkey disabled", name,
        value.get());
  }
  return result;
}

UINT ReadModifierSetting(PCWSTR name, UINT defaultVal) {
  auto value = WindhawkUtils::StringSetting::make(name);
  UINT result = ParseModifiers(value.get());
  return result ? result : defaultVal;
}

// Invalid values use the documented default; they are deliberately not clamped.
static int ReadBoundedIntSetting(PCWSTR name, int minimum, int maximum, int fallback) {
  const int value = Wh_GetIntSetting(name);
  return value < minimum || value > maximum ? fallback : value;
}

// Settings strings are non-null. Match the rule format's ASCII whitespace only.
static std::wstring TrimSettingText(PCWSTR text) {
  const std::wstring value = text;
  const size_t first = value.find_first_not_of(L" \t\r\n");
  return first == std::wstring::npos ? std::wstring{}
      : value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

UINT ParseHotkeyKey(PCWSTR str) {
  if (!str || !str[0]) return 0;

  static const std::pair<PCWSTR, UINT> kNamedKeys[] = {
      {L"Space", VK_SPACE},
      {L"Tab", VK_TAB},
      {L"Enter", VK_RETURN},
      {L"Return", VK_RETURN},
      {L"Esc", VK_ESCAPE},
      {L"Escape", VK_ESCAPE},
      {L"Backspace", VK_BACK},
      {L"Insert", VK_INSERT},
      {L"Ins", VK_INSERT},
      {L"Delete", VK_DELETE},
      {L"Del", VK_DELETE},
      {L"Home", VK_HOME},
      {L"End", VK_END},
      {L"PageUp", VK_PRIOR},
      {L"PgUp", VK_PRIOR},
      {L"PageDown", VK_NEXT},
      {L"PgDn", VK_NEXT},
      {L"Left", VK_LEFT},
      {L"Right", VK_RIGHT},
      {L"Up", VK_UP},
      {L"Down", VK_DOWN},
  };
  for (const auto& namedKey : kNamedKeys) {
    if (_wcsicmp(str, namedKey.first) == 0) return namedKey.second;
  }

  if ((str[0] == L'F' || str[0] == L'f') && str[1]) {
    wchar_t* end = nullptr;
    long functionKey = std::wcstol(str + 1, &end, 10);
    if (*end == L'\0' && functionKey >= 1 && functionKey <= 12) {
      return VK_F1 + static_cast<UINT>(functionKey - 1);
    }
  }

  if (str[1] != L'\0') return 0;
  wchar_t c = str[0];

  if (c >= L'A' && c <= L'Z') return c;
  if (c >= L'a' && c <= L'z') return c - L'a' + L'A';
  if (c >= L'0' && c <= L'9') return c;

  if (c == L'!') return '1';
  if (c == L'@') return '2';
  if (c == L'#') return '3';
  if (c == L'$') return '4';
  if (c == L'%') return '5';
  if (c == L'^') return '6';
  if (c == L'&') return '7';
  if (c == L'*') return '8';
  if (c == L'(') return '9';
  if (c == L')') return '0';

  if (c == L'`' || c == L'~') return VK_OEM_3;
  if (c == L'-' || c == L'_') return VK_OEM_MINUS;
  if (c == L'=' || c == L'+') return VK_OEM_PLUS;
  if (c == L'[' || c == L'{') return VK_OEM_4;
  if (c == L']' || c == L'}') return VK_OEM_6;
  if (c == L'\\' || c == L'|') return VK_OEM_5;
  if (c == L';' || c == L':') return VK_OEM_1;
  if (c == L'\'' || c == L'"') return VK_OEM_7;
  if (c == L',' || c == L'<') return VK_OEM_COMMA;
  if (c == L'.' || c == L'>') return VK_OEM_PERIOD;
  if (c == L'/' || c == L'?') return VK_OEM_2;
  if (c == L' ') return VK_SPACE;

  return 0;
}

static bool ParseWorkspaceInsetsSetting(PCWSTR text, WorkspaceInsets* outInsets) {
  if (!text || !outInsets) return false;

  LONG values[4]{};
  const wchar_t* cursor = text;

  for (size_t i = 0; i < 4; ++i) {
    while (*cursor && std::iswspace(*cursor)) ++cursor;

    wchar_t* end = nullptr;
    long value = std::wcstol(cursor, &end, 10);
    if (end == cursor || value < 0 || value > 500) return false;
    values[i] = static_cast<LONG>(value);
    cursor = end;

    while (*cursor && std::iswspace(*cursor)) ++cursor;
    if (i < 3) {
      if (*cursor != L',') return false;
      ++cursor;
    }
  }

  while (*cursor && std::iswspace(*cursor)) ++cursor;
  if (*cursor != L'\0') return false;

  *outInsets = {values[0], values[1], values[2], values[3]};
  return true;
}

static bool ParseFloatingDefaultSizeSetting(
    PCWSTR text, FloatingDefaultSize* outSize) {
  if (!text || !outSize) return false;

  LONG values[2]{};
  const wchar_t* cursor = text;
  for (size_t i = 0; i < 2; ++i) {
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    wchar_t* end = nullptr;
    long value = std::wcstol(cursor, &end, 10);
    if (end == cursor || value < 100 || value > 4000) return false;
    values[i] = static_cast<LONG>(value);
    cursor = end;
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    if (i == 0) {
      if (*cursor != L',') return false;
      ++cursor;
    }
  }
  while (*cursor && std::iswspace(*cursor)) ++cursor;
  if (*cursor != L'\0') return false;
  *outSize = {values[0], values[1]};
  return true;
}

// Replaces workspace rules in settings order. Disabled rows and malformed rows
// retain their distinct skip/termination policies. WM thread only.
static void LoadWorkspaceRules() {
  using WindhawkUtils::StringSetting;
  g_settings.monitorWindowLimits.clear();
  for (int i = 0;; ++i) {
    auto monitor = StringSetting::make(L"workspace.Overflow.MonitorLimits[%d].Monitor", i);
    auto monitorId = StringSetting::make(L"workspace.Overflow.MonitorLimits[%d].MonitorId", i);
    const int limit = Wh_GetIntSetting(L"workspace.Overflow.MonitorLimits[%d].Limit", i);
    if (!*monitor.get() && !*monitorId.get() && limit == 0) break;
    // Disabled rows do not terminate the list or inspect their hidden fields.
    if (!*monitor.get() || _wcsicmp(monitor.get(), L"disabled") == 0) continue;
    if (limit < 0) {
      Wh_Log(L"Ignoring negative per-monitor limit at rule %d", i + 1);
      continue;
    }
    SettingsState::MonitorWindowLimit rule;
    if (_wcsicmp(monitor.get(), L"primary") == 0) rule.monitor = RuleMonitor::Primary;
    else if (_wcsicmp(monitor.get(), L"non_primary") == 0) rule.monitor = RuleMonitor::NonPrimary;
    else if (_wcsicmp(monitor.get(), L"specific") != 0) {
      Wh_Log(L"Ignoring invalid monitor selector at limit rule %d", i + 1);
      continue;
    }
    if (rule.monitor == RuleMonitor::Specific) {
      rule.monitorId = TrimSettingText(monitorId.get());
      if (rule.monitorId.empty()) continue;
      Model::NormalizeMonitorIdentityToken(&rule.monitorId);
      if (rule.monitorId.rfind(L"IFACE:", 0) != 0 && rule.monitorId.rfind(L"GDI:", 0) != 0) {
        Wh_Log(L"Ignoring invalid monitor ID at limit rule %d", i + 1);
        continue;
      }
    }
    rule.limit = static_cast<size_t>(limit);
    g_settings.monitorWindowLimits.push_back(std::move(rule));
  }

  g_settings.workspaceInitializationRules.clear();
  for (int i = 0;; ++i) {
    auto ruleLayout =
        StringSetting::make(L"workspace.InitializationRules[%d].Layout", i);
    auto desktopName = StringSetting::make(
        L"workspace.InitializationRules[%d].DesktopNameContains", i);
    auto monitor =
        StringSetting::make(L"workspace.InitializationRules[%d].Monitor", i);
    auto monitorId =
        StringSetting::make(L"workspace.InitializationRules[%d].MonitorId", i);
    const int desktopNumber = Wh_GetIntSetting(
        L"workspace.InitializationRules[%d].DesktopNumber", i);
    // Added rows can omit unchanged fields, including Monitor. Read the whole
    // row before detecting the end, as blank Layout only disables that row.
    if (!*ruleLayout.get() && !*desktopName.get() && !*monitor.get() &&
        !*monitorId.get() && desktopNumber == 0) {
      break;
    }
    if (!*ruleLayout.get()) continue;

    WorkspaceInitializationRule rule;
    rule.settingsIndex = i + 1;
    rule.desktopNumber = desktopNumber;
    if (rule.desktopNumber < 0 ||
        !TryParseLayoutSetting(ruleLayout.get(), &rule.layout)) {
      Wh_Log(L"Ignoring invalid workspace initialization rule %d", i + 1);
      continue;
    }
    if (_wcsicmp(monitor.get(), L"primary") == 0) {
      rule.monitor = RuleMonitor::Primary;
    } else if (_wcsicmp(monitor.get(), L"non_primary") == 0) {
      rule.monitor = RuleMonitor::NonPrimary;
    } else if (_wcsicmp(monitor.get(), L"specific") == 0) {
      rule.monitor = RuleMonitor::Specific;
    } else if (*monitor.get() && _wcsicmp(monitor.get(), L"any") != 0) {
      Wh_Log(L"Ignoring invalid monitor selector at workspace rule %d", i + 1);
      continue;
    }
    rule.desktopNameContains = TrimSettingText(desktopName.get());
    std::transform(
        rule.desktopNameContains.begin(), rule.desktopNameContains.end(),
        rule.desktopNameContains.begin(),
        [](wchar_t ch) { return std::towlower(ch); });
    if (rule.monitor == RuleMonitor::Specific) {
      rule.monitorId = TrimSettingText(monitorId.get());
      Model::NormalizeMonitorIdentityToken(&rule.monitorId);
      if (rule.monitorId.empty() || (rule.monitorId.rfind(L"IFACE:", 0) != 0 &&
                                   rule.monitorId.rfind(L"GDI:", 0) != 0)) {
        Wh_Log(L"Ignoring invalid monitor ID at workspace rule %d", i + 1);
        continue;
      }
    }
    if (rule.desktopNameContains.empty() && rule.desktopNumber == 0 &&
        rule.monitor == RuleMonitor::Any && rule.monitorId.empty()) {
      continue;
    }
    g_settings.workspaceInitializationRules.push_back(std::move(rule));
  }
}

// Replaces window rules in settings order. A blank match row ends the list;
// escaped commas remain part of a match value. WM thread only.
static void LoadWindowRules() {
  using WindhawkUtils::StringSetting;
  auto parseWindowRuleList = [](const wchar_t* text) {
    std::vector<std::wstring> values;
    std::wstring value;
    for (const wchar_t* cursor = text; ; ++cursor) {
      if (*cursor == L'\\' && cursor[1] == L',') {
        value.push_back(L',');
        ++cursor;
        continue;
      }
      if (*cursor != L',' && *cursor != L'\0') {
        value.push_back(*cursor);
        continue;
      }

      size_t first = 0;
      while (first < value.size() && std::iswspace(value[first])) ++first;
      size_t last = value.size();
      while (last > first && std::iswspace(value[last - 1])) --last;
      if (first < last) {
        values.emplace_back(value.substr(first, last - first));
      }

      value.clear();
      if (*cursor == L'\0') break;
    }
    return values;
  };

  g_settings.windowRules.clear();
  for (int i = 0;; ++i) {
    auto process =
        StringSetting::make(L"windowRules.Rules[%d].Process", i);
    auto className =
        StringSetting::make(L"windowRules.Rules[%d].Class", i);
    auto titleContains =
        StringSetting::make(L"windowRules.Rules[%d].TitleContains", i);
    auto treatment =
        StringSetting::make(L"windowRules.Rules[%d].Treatment", i);
    auto size = StringSetting::make(L"windowRules.Rules[%d].Size", i);
    auto monitor = StringSetting::make(L"windowRules.Rules[%d].Monitor", i);
    auto monitorId = StringSetting::make(L"windowRules.Rules[%d].MonitorId", i);
    auto desktopName = StringSetting::make(L"windowRules.Rules[%d].DesktopNameContains", i);
    const int desktopNumber = Wh_GetIntSetting(L"windowRules.Rules[%d].DesktopNumber", i);
    if (!*process.get() && !*className.get() && !*titleContains.get()) {
      Wh_Log(
          L"Window rule %d is blank; later rules will not be loaded",
          i + 1);
      break;
    }

    WindowRule rule;
    if (_wcsicmp(treatment.get(), L"default_to_floating") == 0) {
      rule.treatment = WindowRuleTreatment::DefaultToFloating;
    } else if (_wcsicmp(treatment.get(), L"always_floating") == 0) {
      rule.treatment = WindowRuleTreatment::AlwaysFloating;
    } else if (_wcsicmp(treatment.get(), L"trace_to_owner") == 0) {
      rule.treatment = WindowRuleTreatment::TraceToOwner;
    } else if (_wcsicmp(treatment.get(), L"open_on_monitor") == 0) {
      rule.treatment = WindowRuleTreatment::InitialPlacement;
      if (_wcsicmp(monitor.get(), L"primary") == 0) rule.monitor = RuleMonitor::Primary;
      else if (_wcsicmp(monitor.get(), L"non_primary") == 0) rule.monitor = RuleMonitor::NonPrimary;
      else if (_wcsicmp(monitor.get(), L"specific") == 0) rule.monitor = RuleMonitor::Specific;
      else if (*monitor.get() && _wcsicmp(monitor.get(), L"any") != 0) {
        Wh_Log(L"Ignoring invalid monitor selector at window rule %d", i + 1);
        continue;
      }
      if (rule.monitor == RuleMonitor::Specific) {
        rule.monitorId = TrimSettingText(monitorId.get());
        Model::NormalizeMonitorIdentityToken(&rule.monitorId);
        if (rule.monitorId.empty() || (rule.monitorId.rfind(L"IFACE:", 0) != 0 &&
                                     rule.monitorId.rfind(L"GDI:", 0) != 0)) {
          Wh_Log(L"Ignoring invalid monitor target at window rule %d", i + 1);
          continue;
        }
      }
      rule.desktopNumber = desktopNumber;
      rule.desktopNameContains = TrimSettingText(desktopName.get());
      if (desktopNumber < 0) {
        Wh_Log(L"Ignoring invalid desktop number at window rule %d", i + 1);
        continue;
      }
      rule.followWindow = Wh_GetIntSetting(L"windowRules.Rules[%d].FollowWindow", i) != 0;
      if (rule.monitor == RuleMonitor::Any && !desktopNumber && rule.desktopNameContains.empty()) continue;
    } else if (_wcsicmp(
                   treatment.get(), L"preserve_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      rule.preserveFloatingSize = true;
    } else if (_wcsicmp(
                   treatment.get(), L"override_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      if (_wcsicmp(size.get(), L"Workspace") == 0) {
        rule.useWorkspaceSize = true;
      } else if (!ParseFloatingDefaultSizeSetting(
                     size.get(), &rule.floatingSizeDip)) {
        Wh_Log(
            L"Ignoring invalid centered size override at rule %d: %s",
            i + 1, size.get());
        continue;
      }
      rule.preserveFloatingSize = false;
    } else {
      rule.treatment = WindowRuleTreatment::Exclude;
    }
    rule.processNames = parseWindowRuleList(process.get());
    rule.classNames = parseWindowRuleList(className.get());
    rule.titleFragments = parseWindowRuleList(titleContains.get());
    if (rule.processNames.empty() && rule.classNames.empty() &&
        rule.titleFragments.empty()) {
      Wh_Log(L"Ignoring window rule %d with no usable match values", i + 1);
      continue;
    }
    g_settings.windowRules.push_back(std::move(rule));
  }
}

// Replaces configured policy on the WM thread. Reads stay in settings order;
// bounds/defaults are explicit here, while rule loaders own list semantics.
void LoadSettings() {
  using WindhawkUtils::StringSetting;

  g_settings.tilingModifiers =
      ReadModifierSetting(L"hotkeys.TilingModifier", MOD_ALT);

  auto insets = StringSetting::make(L"workspace.WorkspaceInsets");
  WorkspaceInsets parsedInsets{};
  if (ParseWorkspaceInsetsSetting(insets.get(), &parsedInsets)) {
    g_settings.insetsDip = parsedInsets;
  } else {
    g_settings.insetsDip = {};
    Wh_Log(L"Invalid WorkspaceInsets setting; using 6, 6, 6, 6");
  }
  g_settings.gapDip = ReadBoundedIntSetting(L"workspace.TileGap", 0, 100, 6);

  auto floatingSize =
      StringSetting::make(L"windowBehavior.FloatingDefaultSize");
  FloatingDefaultSize parsedFloatingSize{};
  if (ParseFloatingDefaultSizeSetting(floatingSize.get(), &parsedFloatingSize)) {
    g_settings.floatingDefaultSizeDip = parsedFloatingSize;
  } else {
    g_settings.floatingDefaultSizeDip = {};
    Wh_Log(L"Invalid FloatingDefaultSize setting; using 960, 640");
  }
  g_settings.masterPercent = ReadBoundedIntSetting(L"workspace.MasterPercent", 1, 99, 50);
  g_settings.reverseDwindle =
      Wh_GetIntSetting(L"workspace.ReverseDwindle") != 0;
  const int adaptiveDwindleSlots =
      Wh_GetIntSetting(L"workspace.AdaptiveDwindleSlots");
  g_settings.adaptiveDwindleSlots = static_cast<size_t>(
      std::clamp(adaptiveDwindleSlots, 0, 64));
  const int adaptiveDwindlePercentage =
      Wh_GetIntSetting(L"workspace.AdaptiveDwindlePercentage");
  g_settings.adaptiveDwindlePercentage =
      std::clamp(adaptiveDwindlePercentage, 0, 40);

  auto layout = StringSetting::make(L"workspace.DefaultLayout");
  g_settings.defaultLayout = ParseLayoutSetting(layout.get());

  g_settings.layoutCycle.clear();
  for (int i = 0; i < 32; ++i) {
    auto cycleEntry =
        StringSetting::make(L"workspace.LayoutCycle[%d]", i);
    if (!*cycleEntry.get()) break;

    TileLayout cycleLayout{};
    if (!TryParseLayoutSetting(cycleEntry.get(), &cycleLayout)) {
      Wh_Log(L"Ignoring invalid LayoutCycle[%d] entry: %s", i, cycleEntry.get());
      continue;
    }
    if (std::find(g_settings.layoutCycle.begin(), g_settings.layoutCycle.end(),
                  cycleLayout) == g_settings.layoutCycle.end()) {
      g_settings.layoutCycle.push_back(cycleLayout);
    }
  }
  if (g_settings.layoutCycle.empty()) {
    g_settings.layoutCycle = MakeBuiltInLayoutCycle();
    Wh_Log(L"LayoutCycle is empty or invalid; using the built-in layout sequence");
  }

  g_settings.automaticWindowLimit = static_cast<size_t>(
      std::max(0, Wh_GetIntSetting(L"workspace.Overflow.WindowLimit")));
  auto overflowAction = StringSetting::make(L"workspace.Overflow.Action");
  g_settings.overflowCreateDesktop =
      _wcsicmp(overflowAction.get(), L"new_desktop") == 0;
  g_settings.overflowTryOtherMonitors =
      Wh_GetIntSetting(L"workspace.Overflow.TryOtherMonitors") != 0;
  LoadWorkspaceRules();

  auto mouseBehavior =
      StringSetting::make(L"windowBehavior.MouseMoveBehavior");
  g_settings.mouseMoveBehavior =
      _wcsicmp(mouseBehavior.get(), L"swap") == 0
          ? MouseMoveBehavior::Swap
          : MouseMoveBehavior::Float;

  auto automaticNewWindowPosition =
      StringSetting::make(L"general.AutomaticNewWindowPosition");
  g_settings.automaticNewWindowPosition =
      _wcsicmp(automaticNewWindowPosition.get(), L"after_focused") == 0
          ? AutomaticNewWindowPosition::AfterFocused
          : AutomaticNewWindowPosition::LastSlot;

  auto managementMode =
      StringSetting::make(L"general.DefaultWindowManagementMode");
  g_wm.managementMode =
      _wcsicmp(managementMode.get(), L"manual") == 0
          ? ManagementMode::Manual
          : ManagementMode::Automatic;

  g_settings.conformanceLeaseMs = ReadBoundedIntSetting(
      L"advanced.ConformanceLeaseMs", 0, 10000, 3000);
  g_settings.conformanceRepairIntervalMs = ReadBoundedIntSetting(
      L"advanced.ConformanceRepairIntervalMs", 20, 2000, 75);
  g_settings.reconcileDelayMs = ReadBoundedIntSetting(
      L"advanced.ReconcileDelayMs", 20, 2000, 50);
  g_settings.slowApplicationDelayMs = ReadBoundedIntSetting(
      L"advanced.SlowApplicationDelayMs", 0, 2000, 250);

  LoadWindowRules();

  const std::pair<PCWSTR, UINT*> hotkeys[] = {
      {L"hotkeys.TileKey", &g_settings.tileKey},
      {L"hotkeys.LayoutKey", &g_settings.layoutKey},
      {L"hotkeys.SwapMasterKey", &g_settings.swapMasterKey},
      {L"hotkeys.PromoteWindowKey", &g_settings.promoteWindowKey},
      {L"hotkeys.DemoteWindowKey", &g_settings.demoteWindowKey},
      {L"hotkeys.ManagementModeToggleKey", &g_settings.managementModeToggleKey},
      {L"hotkeys.FloatFocusedKey", &g_settings.floatFocusedKey},
      {L"hotkeys.DiagnosticDumpKey", &g_settings.diagnosticDumpKey},
  };
  for (const auto& [settingName, destination] : hotkeys) {
    *destination = ReadHotkeySetting<UINT>(settingName, ParseHotkeyKey);
  }

  auto diagnosticsPath =
      StringSetting::make(L"diagnostics.DiagnosticsOutputPath");
  if (*diagnosticsPath.get()) {
    g_settings.diagnosticsOutputPath = diagnosticsPath.get();
  } else {
    g_settings.diagnosticsOutputPath =
        L"%USERPROFILE%\\Documents\\MultiWMDiagnostics";
  }

  TrayUi::LoadSettings();
}

} // namespace Configuration
