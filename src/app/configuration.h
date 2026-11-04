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

// Parses exactly `count` (at most 4) comma-separated integers, each within
// [minimum, maximum], e.g. "6, 6, 6, 6". Whitespace around numbers is allowed.
// On failure `out` is left unchanged.
static bool ParseBoundedIntegerList(PCWSTR text, size_t count, long minimum, long maximum, LONG* out) {
  if (!text || !out || count > 4) return false;
  LONG values[4]{};
  const wchar_t* cursor = text;
  for (size_t i = 0; i < count; ++i) {
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    wchar_t* end = nullptr;
    const long value = std::wcstol(cursor, &end, 10);
    if (end == cursor || value < minimum || value > maximum) return false;
    values[i] = static_cast<LONG>(value);
    cursor = end;
    while (*cursor && std::iswspace(*cursor)) ++cursor;
    if (i + 1 < count) {
      if (*cursor != L',') return false;
      ++cursor;
    }
  }
  while (*cursor && std::iswspace(*cursor)) ++cursor;
  if (*cursor != L'\0') return false;
  std::copy(values, values + count, out);
  return true;
}

// "left, top, right, bottom", each 0..500 DIP.
static bool ParseWorkspaceInsetsSetting(PCWSTR text, WorkspaceInsets* outInsets) {
  LONG values[4]{};
  if (!outInsets || !ParseBoundedIntegerList(text, 4, 0, 500, values)) return false;
  *outInsets = {values[0], values[1], values[2], values[3]};
  return true;
}

// "width, height", each 100..4000 DIP.
static bool ParseFloatingDefaultSizeSetting(PCWSTR text, FloatingDefaultSize* outSize) {
  LONG values[2]{};
  if (!outSize || !ParseBoundedIntegerList(text, 2, 100, 4000, values)) return false;
  *outSize = {values[0], values[1]};
  return true;
}

// Monitor selector shared by limit, workspace and window rules. "primary",
// "non_primary" and "specific" always parse. With `allowAny`, blank and "any"
// mean Any. Anything else is invalid (no value).
static std::optional<RuleMonitor> ParseRuleMonitor(PCWSTR text, bool allowAny) {
  if (_wcsicmp(text, L"primary") == 0) return RuleMonitor::Primary;
  if (_wcsicmp(text, L"non_primary") == 0) return RuleMonitor::NonPrimary;
  if (_wcsicmp(text, L"specific") == 0) return RuleMonitor::Specific;
  if (allowAny && (!*text || _wcsicmp(text, L"any") == 0)) return RuleMonitor::Any;
  return std::nullopt;
}

enum class MonitorIdParse { Valid, Empty, Invalid };

// A monitor ID pasted from the diagnostic report: trimmed, upper-cased, and
// required to start with "IFACE:" or "GDI:". `out` receives the normalized text.
static MonitorIdParse ParseMonitorIdSetting(PCWSTR text, std::wstring* out) {
  *out = TrimSettingText(text);
  if (out->empty()) return MonitorIdParse::Empty;
  Model::NormalizeMonitorIdentityToken(out);
  return out->rfind(L"IFACE:", 0) == 0 || out->rfind(L"GDI:", 0) == 0 ? MonitorIdParse::Valid
                                                                       : MonitorIdParse::Invalid;
}

// Splits a rule match field ("a.exe, b.exe") into trimmed, non-empty values.
// "\," is a literal comma inside a value.
static std::vector<std::wstring> ParseWindowRuleList(const wchar_t* text) {
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
    if (first < last) values.emplace_back(value.substr(first, last - first));

    value.clear();
    if (*cursor == L'\0') break;
  }
  return values;
}

// Per-monitor overflow limits. A fully blank row ends the list; a "disabled"
// row is skipped without reading its hidden fields. WM thread only.
static void LoadMonitorWindowLimits() {
  using WindhawkUtils::StringSetting;
  g_settings.monitorWindowLimits.clear();
  for (int i = 0;; ++i) {
    auto monitor = StringSetting::make(L"workspace.Overflow.MonitorLimits[%d].Monitor", i);
    auto monitorId = StringSetting::make(L"workspace.Overflow.MonitorLimits[%d].MonitorId", i);
    const int limit = Wh_GetIntSetting(L"workspace.Overflow.MonitorLimits[%d].Limit", i);
    if (!*monitor.get() && !*monitorId.get() && limit == 0) break;
    if (!*monitor.get() || _wcsicmp(monitor.get(), L"disabled") == 0) continue;
    if (limit < 0) {
      Wh_Log(L"Ignoring negative per-monitor limit at rule %d", i + 1);
      continue;
    }
    SettingsState::MonitorWindowLimit rule;
    const auto selector = ParseRuleMonitor(monitor.get(), /*allowAny=*/false);
    if (!selector) {
      Wh_Log(L"Ignoring invalid monitor selector at limit rule %d", i + 1);
      continue;
    }
    rule.monitor = *selector;
    if (rule.monitor == RuleMonitor::Specific) {
      const MonitorIdParse id = ParseMonitorIdSetting(monitorId.get(), &rule.monitorId);
      if (id == MonitorIdParse::Empty) continue;  // Not filled in yet: skip quietly.
      if (id == MonitorIdParse::Invalid) {
        Wh_Log(L"Ignoring invalid monitor ID at limit rule %d", i + 1);
        continue;
      }
    }
    rule.limit = static_cast<size_t>(limit);
    g_settings.monitorWindowLimits.push_back(std::move(rule));
  }
}

// Initial-layout rules for new workspaces. Added rows can omit unchanged fields,
// so the whole row is read before detecting the end; a blank Layout only
// disables that row. WM thread only.
static void LoadWorkspaceInitializationRules() {
  using WindhawkUtils::StringSetting;
  g_settings.workspaceInitializationRules.clear();
  for (int i = 0;; ++i) {
    auto ruleLayout = StringSetting::make(L"workspace.InitializationRules[%d].Layout", i);
    auto desktopName = StringSetting::make(L"workspace.InitializationRules[%d].DesktopNameContains", i);
    auto monitor = StringSetting::make(L"workspace.InitializationRules[%d].Monitor", i);
    auto monitorId = StringSetting::make(L"workspace.InitializationRules[%d].MonitorId", i);
    const int desktopNumber = Wh_GetIntSetting(L"workspace.InitializationRules[%d].DesktopNumber", i);
    if (!*ruleLayout.get() && !*desktopName.get() && !*monitor.get() &&
        !*monitorId.get() && desktopNumber == 0) {
      break;
    }
    if (!*ruleLayout.get()) continue;

    WorkspaceInitializationRule rule;
    rule.settingsIndex = i + 1;
    rule.desktopNumber = desktopNumber;
    if (rule.desktopNumber < 0 || !TryParseLayoutSetting(ruleLayout.get(), &rule.layout)) {
      Wh_Log(L"Ignoring invalid workspace initialization rule %d", i + 1);
      continue;
    }
    const auto selector = ParseRuleMonitor(monitor.get(), /*allowAny=*/true);
    if (!selector) {
      Wh_Log(L"Ignoring invalid monitor selector at workspace rule %d", i + 1);
      continue;
    }
    rule.monitor = *selector;
    rule.desktopNameContains = TrimSettingText(desktopName.get());
    std::transform(
        rule.desktopNameContains.begin(), rule.desktopNameContains.end(),
        rule.desktopNameContains.begin(), [](wchar_t ch) { return std::towlower(ch); });
    if (rule.monitor == RuleMonitor::Specific &&
        ParseMonitorIdSetting(monitorId.get(), &rule.monitorId) != MonitorIdParse::Valid) {
      Wh_Log(L"Ignoring invalid monitor ID at workspace rule %d", i + 1);
      continue;
    }
    // A rule with no condition at all would match everything; ignore it.
    if (rule.desktopNameContains.empty() && rule.desktopNumber == 0 &&
        rule.monitor == RuleMonitor::Any && rule.monitorId.empty()) {
      continue;
    }
    g_settings.workspaceInitializationRules.push_back(std::move(rule));
  }
}

static void LoadWorkspaceRules() {
  LoadMonitorWindowLimits();
  LoadWorkspaceInitializationRules();
}

// Fills the "Open on monitor / desktop" part of a window rule. False (after
// logging) when the row is invalid; also false, silently, when it names no
// destination at all.
static bool ParseInitialPlacementRule(int i, PCWSTR monitor, PCWSTR monitorId,
                                      PCWSTR desktopName, int desktopNumber, WindowRule* rule) {
  const auto selector = ParseRuleMonitor(monitor, /*allowAny=*/true);
  if (!selector) {
    Wh_Log(L"Ignoring invalid monitor selector at window rule %d", i + 1);
    return false;
  }
  rule->monitor = *selector;
  if (rule->monitor == RuleMonitor::Specific &&
      ParseMonitorIdSetting(monitorId, &rule->monitorId) != MonitorIdParse::Valid) {
    Wh_Log(L"Ignoring invalid monitor target at window rule %d", i + 1);
    return false;
  }
  rule->desktopNumber = desktopNumber;
  rule->desktopNameContains = TrimSettingText(desktopName);
  if (desktopNumber < 0) {
    Wh_Log(L"Ignoring invalid desktop number at window rule %d", i + 1);
    return false;
  }
  rule->followWindow = Wh_GetIntSetting(L"windowRules.Rules[%d].FollowWindow", i) != 0;
  return !(rule->monitor == RuleMonitor::Any && !desktopNumber && rule->desktopNameContains.empty());
}

// Window rules in settings order. A row with no match fields ends the list.
// WM thread only.
static void LoadWindowRules() {
  using WindhawkUtils::StringSetting;
  g_settings.windowRules.clear();
  for (int i = 0;; ++i) {
    auto process = StringSetting::make(L"windowRules.Rules[%d].Process", i);
    auto className = StringSetting::make(L"windowRules.Rules[%d].Class", i);
    auto titleContains = StringSetting::make(L"windowRules.Rules[%d].TitleContains", i);
    auto treatment = StringSetting::make(L"windowRules.Rules[%d].Treatment", i);
    auto size = StringSetting::make(L"windowRules.Rules[%d].Size", i);
    auto monitor = StringSetting::make(L"windowRules.Rules[%d].Monitor", i);
    auto monitorId = StringSetting::make(L"windowRules.Rules[%d].MonitorId", i);
    auto desktopName = StringSetting::make(L"windowRules.Rules[%d].DesktopNameContains", i);
    const int desktopNumber = Wh_GetIntSetting(L"windowRules.Rules[%d].DesktopNumber", i);
    if (!*process.get() && !*className.get() && !*titleContains.get()) {
      Wh_Log(L"Window rule %d is blank; later rules will not be loaded", i + 1);
      break;
    }

    WindowRule rule;
    const PCWSTR kind = treatment.get();
    if (_wcsicmp(kind, L"default_to_floating") == 0) {
      rule.treatment = WindowRuleTreatment::DefaultToFloating;
    } else if (_wcsicmp(kind, L"always_floating") == 0) {
      rule.treatment = WindowRuleTreatment::AlwaysFloating;
    } else if (_wcsicmp(kind, L"trace_to_owner") == 0) {
      rule.treatment = WindowRuleTreatment::TraceToOwner;
    } else if (_wcsicmp(kind, L"open_on_monitor") == 0) {
      rule.treatment = WindowRuleTreatment::InitialPlacement;
      if (!ParseInitialPlacementRule(i, monitor.get(), monitorId.get(), desktopName.get(),
                                     desktopNumber, &rule)) {
        continue;
      }
    } else if (_wcsicmp(kind, L"preserve_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      rule.preserveFloatingSize = true;
    } else if (_wcsicmp(kind, L"override_size_when_centering") == 0) {
      rule.treatment = WindowRuleTreatment::FloatingPlacementOverride;
      if (_wcsicmp(size.get(), L"Workspace") == 0) {
        rule.useWorkspaceSize = true;
      } else if (!ParseFloatingDefaultSizeSetting(size.get(), &rule.floatingSizeDip)) {
        Wh_Log(L"Ignoring invalid centered size override at rule %d: %s", i + 1, size.get());
        continue;
      }
      rule.preserveFloatingSize = false;
    } else {
      rule.treatment = WindowRuleTreatment::Exclude;
    }
    rule.processNames = ParseWindowRuleList(process.get());
    rule.classNames = ParseWindowRuleList(className.get());
    rule.titleFragments = ParseWindowRuleList(titleContains.get());
    if (rule.processNames.empty() && rule.classNames.empty() && rule.titleFragments.empty()) {
      Wh_Log(L"Ignoring window rule %d with no usable match values", i + 1);
      continue;
    }
    g_settings.windowRules.push_back(std::move(rule));
  }
}

// True when the string setting `name` equals `value`, ignoring case.
static bool StringSettingIs(PCWSTR name, PCWSTR value) {
  auto setting = WindhawkUtils::StringSetting::make(name);
  return _wcsicmp(setting.get(), value) == 0;
}

// Layouts used by Cycle Layout, in order, without duplicates. Invalid entries
// are skipped; an empty result falls back to the built-in sequence.
static std::vector<TileLayout> ReadLayoutCycleSetting() {
  std::vector<TileLayout> cycle;
  for (int i = 0; i < 32; ++i) {
    auto entry = WindhawkUtils::StringSetting::make(L"workspace.LayoutCycle[%d]", i);
    if (!*entry.get()) break;
    TileLayout layout{};
    if (!TryParseLayoutSetting(entry.get(), &layout)) {
      Wh_Log(L"Ignoring invalid LayoutCycle[%d] entry: %s", i, entry.get());
      continue;
    }
    if (std::find(cycle.begin(), cycle.end(), layout) == cycle.end()) cycle.push_back(layout);
  }
  if (cycle.empty()) {
    Wh_Log(L"LayoutCycle is empty or invalid; using the built-in layout sequence");
    cycle = MakeBuiltInLayoutCycle();
  }
  return cycle;
}

// Replaces configured policy on the WM thread. Reads stay in settings order.
// Invalid numbers use the documented default; they are not clamped unless noted.
void LoadSettings() {
  using WindhawkUtils::StringSetting;

  g_settings.tilingModifiers = ReadModifierSetting(L"hotkeys.TilingModifier", MOD_ALT);

  // Geometry (DIP values; converted to physical pixels per monitor).
  auto insets = StringSetting::make(L"workspace.WorkspaceInsets");
  if (!ParseWorkspaceInsetsSetting(insets.get(), &g_settings.insetsDip)) {
    g_settings.insetsDip = {};
    Wh_Log(L"Invalid WorkspaceInsets setting; using 6, 6, 6, 6");
  }
  g_settings.gapDip = ReadBoundedIntSetting(L"workspace.TileGap", 0, 100, 6);
  auto floatingSize = StringSetting::make(L"windowBehavior.FloatingDefaultSize");
  if (!ParseFloatingDefaultSizeSetting(floatingSize.get(), &g_settings.floatingDefaultSizeDip)) {
    g_settings.floatingDefaultSizeDip = {};
    Wh_Log(L"Invalid FloatingDefaultSize setting; using 960, 640");
  }

  // Layouts.
  g_settings.masterPercent = ReadBoundedIntSetting(L"workspace.MasterPercent", 1, 99, 50);
  g_settings.reverseDwindle = Wh_GetIntSetting(L"workspace.ReverseDwindle") != 0;
  // These two are clamped rather than reset to a default.
  g_settings.adaptiveDwindleSlots =
      static_cast<size_t>(std::clamp(Wh_GetIntSetting(L"workspace.AdaptiveDwindleSlots"), 0, 64));
  g_settings.adaptiveDwindlePercentage =
      std::clamp(Wh_GetIntSetting(L"workspace.AdaptiveDwindlePercentage"), 0, 40);
  auto layout = StringSetting::make(L"workspace.DefaultLayout");
  g_settings.defaultLayout = ParseLayoutSetting(layout.get());
  g_settings.layoutCycle = ReadLayoutCycleSetting();

  // Overflow and workspace rules.
  g_settings.automaticWindowLimit =
      static_cast<size_t>(std::max(0, Wh_GetIntSetting(L"workspace.Overflow.WindowLimit")));
  g_settings.overflowCreateDesktop = StringSettingIs(L"workspace.Overflow.Action", L"new_desktop");
  g_settings.overflowTryOtherMonitors = Wh_GetIntSetting(L"workspace.Overflow.TryOtherMonitors") != 0;
  LoadWorkspaceRules();

  // Behavior choices.
  g_settings.mouseMoveBehavior = StringSettingIs(L"windowBehavior.MouseMoveBehavior", L"swap")
      ? MouseMoveBehavior::Swap : MouseMoveBehavior::Float;
  g_settings.automaticNewWindowPosition =
      StringSettingIs(L"general.AutomaticNewWindowPosition", L"after_focused")
      ? AutomaticNewWindowPosition::AfterFocused : AutomaticNewWindowPosition::LastSlot;
  g_wm.managementMode = StringSettingIs(L"general.DefaultWindowManagementMode", L"manual")
      ? ManagementMode::Manual : ManagementMode::Automatic;

  // Timing.
  g_settings.conformanceLeaseMs = ReadBoundedIntSetting(L"advanced.ConformanceLeaseMs", 0, 10000, 3000);
  g_settings.conformanceRepairIntervalMs =
      ReadBoundedIntSetting(L"advanced.ConformanceRepairIntervalMs", 20, 2000, 75);
  g_settings.reconcileDelayMs = ReadBoundedIntSetting(L"advanced.ReconcileDelayMs", 20, 2000, 50);
  g_settings.slowApplicationDelayMs = ReadBoundedIntSetting(L"advanced.SlowApplicationDelayMs", 0, 2000, 250);

  LoadWindowRules();

  // Hotkeys. Blank disables a hotkey; an unparseable key is logged and disabled.
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

  auto diagnosticsPath = StringSetting::make(L"diagnostics.DiagnosticsOutputPath");
  g_settings.diagnosticsOutputPath = *diagnosticsPath.get()
      ? std::wstring(diagnosticsPath.get())
      : std::wstring(L"%USERPROFILE%\\Documents\\MultiWMDiagnostics");

  TrayUi::LoadSettings();
}
} // namespace Configuration
