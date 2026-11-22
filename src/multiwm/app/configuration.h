// Settings parsing and loading.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Configuration {

// Records a setting this load couldn't use, as one short toast line
// (SettingsState::problems). The detail is logged where it was found.
static void NoteSettingProblem(PCWSTR format, ...) {
  wchar_t text[160]{};
  va_list args;
  va_start(args, format);
  const int written = vswprintf(text, ARRAYSIZE(text), format, args);
  va_end(args);
  if (written > 0) g_settings.problems.emplace_back(text);
}

UINT ParseModifiers(PCWSTR str) {
  UINT modifiers = 0;
  if (wcsstr(str, L"alt")) modifiers |= MOD_ALT;
  if (wcsstr(str, L"ctrl")) modifiers |= MOD_CONTROL;
  if (wcsstr(str, L"shift")) modifiers |= MOD_SHIFT;
  return modifiers;
}

static bool TryParseLayoutSetting(PCWSTR str, TileLayout* outLayout) {
  if (!str || !outLayout) return false;
  for (size_t index = 0; index < std::size(kLayoutNames); ++index) {
    if (wcscmp(str, kLayoutNames[index].setting) == 0) {
      *outLayout = static_cast<TileLayout>(index);
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

// `label` names the hotkey in the toast for an invalid key.
template <typename T, typename Parser>
T ReadHotkeySetting(PCWSTR name, PCWSTR label, Parser parser) {
  auto value = WindhawkUtils::StringSetting::make(name);
  if (!*value.get()) return T{};  // Blank hotkey = unused.

  T result = parser(value.get());
  if (!result) {
    Wh_Log(
        L"Invalid %ls hotkey setting '%ls'; hotkey disabled", name,
        value.get());
    NoteSettingProblem(L"%ls hotkey off: \"%ls\" isn't a key", label, value.get());
  }
  return result;
}

UINT ReadModifierSetting(PCWSTR name, UINT defaultVal) {
  auto value = WindhawkUtils::StringSetting::make(name);
  UINT result = ParseModifiers(value.get());
  return result ? result : defaultVal;
}

// Invalid values use the documented default; they are deliberately not clamped.
// `label` names the setting in the toast for an out-of-range value.
static int ReadBoundedIntSetting(PCWSTR name, PCWSTR label, int minimum, int maximum, int fallback) {
  const int value = Wh_GetIntSetting(name);
  if (value >= minimum && value <= maximum) return value;
  Wh_Log(L"%ls setting %d is outside %d-%d; using %d", name, value, minimum, maximum, fallback);
  NoteSettingProblem(L"%ls must be %d-%d; using %d", label, minimum, maximum, fallback);
  return fallback;
}

// `text` without leading or trailing whitespace (as iswspace classifies it).
// Every text setting and rule-list value is trimmed this way.
static std::wstring TrimSettingText(std::wstring_view text) {
  size_t first = 0;
  while (first < text.size() && std::iswspace(text[first])) ++first;
  size_t last = text.size();
  while (last > first && std::iswspace(text[last - 1])) --last;
  return std::wstring(text.substr(first, last - first));
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
  const wchar_t c = str[0];

  if (c >= L'A' && c <= L'Z') return c;
  if (c >= L'a' && c <= L'z') return c - L'a' + L'A';
  if (c >= L'0' && c <= L'9') return c;

  // Symbols, typed or shifted, on a US keyboard layout.
  static const std::pair<wchar_t, UINT> kSymbolKeys[] = {
      {L'!', '1'}, {L'@', '2'}, {L'#', '3'}, {L'$', '4'}, {L'%', '5'},
      {L'^', '6'}, {L'&', '7'}, {L'*', '8'}, {L'(', '9'}, {L')', '0'},
      {L'`', VK_OEM_3}, {L'~', VK_OEM_3}, {L'-', VK_OEM_MINUS}, {L'_', VK_OEM_MINUS},
      {L'=', VK_OEM_PLUS}, {L'+', VK_OEM_PLUS}, {L'[', VK_OEM_4}, {L'{', VK_OEM_4},
      {L']', VK_OEM_6}, {L'}', VK_OEM_6}, {L'\\', VK_OEM_5}, {L'|', VK_OEM_5},
      {L';', VK_OEM_1}, {L':', VK_OEM_1}, {L'\'', VK_OEM_7}, {L'"', VK_OEM_7},
      {L',', VK_OEM_COMMA}, {L'<', VK_OEM_COMMA}, {L'.', VK_OEM_PERIOD}, {L'>', VK_OEM_PERIOD},
      {L'/', VK_OEM_2}, {L'?', VK_OEM_2}, {L' ', VK_SPACE},
  };
  for (const auto& [symbol, key] : kSymbolKeys) {
    if (c == symbol) return key;
  }
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

    if (std::wstring trimmed = TrimSettingText(value); !trimmed.empty()) values.push_back(std::move(trimmed));

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
      NoteSettingProblem(L"Per-monitor limit %d ignored: negative limit", i + 1);
      continue;
    }
    SettingsState::MonitorWindowLimit rule;
    const auto selector = ParseRuleMonitor(monitor.get(), /*allowAny=*/false);
    if (!selector) {
      Wh_Log(L"Ignoring invalid monitor selector at limit rule %d", i + 1);
      NoteSettingProblem(L"Per-monitor limit %d ignored: invalid monitor", i + 1);
      continue;
    }
    rule.monitor = *selector;
    if (rule.monitor == RuleMonitor::Specific) {
      const MonitorIdParse id = ParseMonitorIdSetting(monitorId.get(), &rule.monitorId);
      if (id == MonitorIdParse::Empty) continue;  // Not filled in yet: skip quietly.
      if (id == MonitorIdParse::Invalid) {
        Wh_Log(L"Ignoring invalid monitor ID at limit rule %d", i + 1);
        NoteSettingProblem(L"Per-monitor limit %d ignored: invalid monitor ID", i + 1);
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
      NoteSettingProblem(L"Initialization rule %d ignored: invalid layout or desktop number", i + 1);
      continue;
    }
    const auto selector = ParseRuleMonitor(monitor.get(), /*allowAny=*/true);
    if (!selector) {
      Wh_Log(L"Ignoring invalid monitor selector at workspace rule %d", i + 1);
      NoteSettingProblem(L"Initialization rule %d ignored: invalid monitor", i + 1);
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
      NoteSettingProblem(L"Initialization rule %d ignored: invalid monitor ID", i + 1);
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
    NoteSettingProblem(L"Window rule %d ignored: invalid monitor", i + 1);
    return false;
  }
  rule->monitor = *selector;
  if (rule->monitor == RuleMonitor::Specific &&
      ParseMonitorIdSetting(monitorId, &rule->monitorId) != MonitorIdParse::Valid) {
    Wh_Log(L"Ignoring invalid monitor target at window rule %d", i + 1);
    NoteSettingProblem(L"Window rule %d ignored: invalid monitor ID", i + 1);
    return false;
  }
  rule->desktopNumber = desktopNumber;
  rule->desktopNameContains = TrimSettingText(desktopName);
  if (desktopNumber < 0) {
    Wh_Log(L"Ignoring invalid desktop number at window rule %d", i + 1);
    NoteSettingProblem(L"Window rule %d ignored: invalid desktop number", i + 1);
    return false;
  }
  rule->followWindow = Wh_GetIntSetting(L"windowRules.Rules[%d].FollowWindow", i) != 0;
  return !(rule->monitor == RuleMonitor::Any && !desktopNumber && rule->desktopNameContains.empty());
}

// Fills the treatment of window rule `i` from its Treatment value (`kind`) and
// the fields that treatment uses. Unknown or blank values mean Exclude. False
// (after logging, unless the row names no destination) when the row is invalid.
static bool ParseWindowRuleTreatment(int i, PCWSTR kind, PCWSTR size, PCWSTR monitor, PCWSTR monitorId,
                                     PCWSTR desktopName, int desktopNumber, WindowRule* rule) {
  if (_wcsicmp(kind, L"default_to_floating") == 0) {
    rule->treatment = WindowRuleTreatment::DefaultToFloating;
  } else if (_wcsicmp(kind, L"always_floating") == 0) {
    rule->treatment = WindowRuleTreatment::AlwaysFloating;
  } else if (_wcsicmp(kind, L"trace_to_owner") == 0) {
    rule->treatment = WindowRuleTreatment::TraceToOwner;
  } else if (_wcsicmp(kind, L"open_on_monitor") == 0) {
    rule->treatment = WindowRuleTreatment::InitialPlacement;
    return ParseInitialPlacementRule(i, monitor, monitorId, desktopName, desktopNumber, rule);
  } else if (_wcsicmp(kind, L"preserve_size_when_centering") == 0) {
    rule->treatment = WindowRuleTreatment::FloatingPlacementOverride;
    rule->preserveFloatingSize = true;
  } else if (_wcsicmp(kind, L"override_size_when_centering") == 0) {
    rule->treatment = WindowRuleTreatment::FloatingPlacementOverride;
    if (_wcsicmp(size, L"Workspace") == 0) {
      rule->useWorkspaceSize = true;
    } else if (!ParseFloatingDefaultSizeSetting(size, &rule->floatingSizeDip)) {
      Wh_Log(L"Ignoring invalid centered size override at rule %d: %s", i + 1, size);
      NoteSettingProblem(L"Window rule %d ignored: invalid size", i + 1);
      return false;
    }
    rule->preserveFloatingSize = false;
  } else {
    rule->treatment = WindowRuleTreatment::Exclude;
  }
  return true;
}

// Window rules in settings order. Like the initialization rules, the whole row
// is read before detecting the end: only a completely empty row ends the list,
// and a row whose match fields were cleared is skipped. WM thread only.
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
      if (!*treatment.get() && !*size.get() && !*monitor.get() && !*monitorId.get() &&
          !*desktopName.get() && desktopNumber == 0) {
        break;
      }
      Wh_Log(L"Window rule %d has no match fields; skipping it", i + 1);
      continue;
    }

    WindowRule rule;
    if (!ParseWindowRuleTreatment(i, treatment.get(), size.get(), monitor.get(), monitorId.get(),
                                  desktopName.get(), desktopNumber, &rule)) {
      continue;
    }
    rule.processNames = ParseWindowRuleList(process.get());
    rule.classNames = ParseWindowRuleList(className.get());
    rule.titleFragments = ParseWindowRuleList(titleContains.get());
    if (rule.processNames.empty() && rule.classNames.empty() && rule.titleFragments.empty()) {
      Wh_Log(L"Ignoring window rule %d with no usable match values", i + 1);
      NoteSettingProblem(L"Window rule %d ignored: no app, class or title", i + 1);
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
      NoteSettingProblem(L"Layout Cycle entry %d ignored: unknown layout", i + 1);
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
  g_settings.problems.clear();

  g_settings.tilingModifiers = ReadModifierSetting(L"hotkeys.TilingModifier", MOD_ALT);

  // Geometry (DIP values; converted to physical pixels per monitor).
  auto insets = StringSetting::make(L"workspace.WorkspaceInsets");
  if (!ParseWorkspaceInsetsSetting(insets.get(), &g_settings.insetsDip)) {
    g_settings.insetsDip = {};
    Wh_Log(L"Invalid WorkspaceInsets setting; using 6, 6, 6, 6");
    NoteSettingProblem(L"Insets need four values from 0 to 500; using 6, 6, 6, 6");
  }
  g_settings.gapDip = ReadBoundedIntSetting(L"workspace.TileGap", L"Window Gap", 0, 100, 6);
  auto floatingSize = StringSetting::make(L"windowBehavior.FloatingDefaultSize");
  if (!ParseFloatingDefaultSizeSetting(floatingSize.get(), &g_settings.floatingDefaultSizeDip)) {
    g_settings.floatingDefaultSizeDip = {};
    Wh_Log(L"Invalid FloatingDefaultSize setting; using 960, 640");
    NoteSettingProblem(L"Floating Default Size needs width, height (100-4000); using 960, 640");
  }

  // Layouts.
  g_settings.masterPercent = ReadBoundedIntSetting(L"workspace.MasterPercent", L"Master Size", 1, 99, 50);
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
  g_settings.bspSplitEdgePercent =
      ReadBoundedIntSetting(L"windowBehavior.BspSplitEdgePercent", L"BSP Split Edge Size", 0, 50, 25);
  g_settings.dragPreviewFillOpacity =
      ReadBoundedIntSetting(L"appearance.DragPreview.FillOpacity", L"Drag Preview Fill Opacity", 0, 100, 25);
  g_settings.dragPreviewOutlineOpacity =
      ReadBoundedIntSetting(L"appearance.DragPreview.OutlineOpacity", L"Drag Preview Outline Opacity", 0, 100, 100);
  g_settings.dragPreviewOutlineWidthDip =
      ReadBoundedIntSetting(L"appearance.DragPreview.OutlineWidth", L"Drag Preview Outline Width", 1, 20, 4);
  g_settings.dragPreviewOutlineRadiusDip =
      ReadBoundedIntSetting(L"appearance.DragPreview.OutlineCornerRadius", L"Drag Preview Outline Corner Radius", 0, 32, 8);
  g_settings.dragPreviewIconSizeDip =
      ReadBoundedIntSetting(L"appearance.DragPreview.AppIconSize", L"Drag Preview App Icon Size", 0, 64, 32);
  g_settings.dragPreviewFadeMs =
      ReadBoundedIntSetting(L"appearance.DragPreview.FadeDurationMs", L"Drag Preview Fade Duration", 0, 500, 96);
  g_settings.automaticNewWindowPosition =
      StringSettingIs(L"general.AutomaticNewWindowPosition", L"after_focused")
      ? AutomaticNewWindowPosition::AfterFocused : AutomaticNewWindowPosition::LastSlot;
  g_settings.defaultManagementMode = StringSettingIs(L"general.DefaultWindowManagementMode", L"manual")
      ? ManagementMode::Manual : ManagementMode::Automatic;

  // Timing.
  g_settings.conformanceLeaseMs = ReadBoundedIntSetting(L"advanced.ConformanceLeaseMs", L"Keep Windows in Place", 0, 10000, 3000);
  g_settings.conformanceRepairIntervalMs =
      ReadBoundedIntSetting(L"advanced.ConformanceRepairIntervalMs", L"Position Repair Interval", 20, 2000, 75);
  g_settings.reconcileDelayMs = ReadBoundedIntSetting(L"advanced.ReconcileDelayMs", L"Window Change Delay", 20, 2000, 50);
  g_settings.slowApplicationDelayMs = ReadBoundedIntSetting(L"advanced.SlowApplicationDelayMs", L"New Window Retry Delay", 0, 2000, 250);

  LoadWindowRules();

  // Hotkeys. Blank disables a hotkey; an unparseable key is logged and disabled.
  for (const HotkeyBinding& hotkey : kHotkeyBindings) {
    g_settings.*hotkey.key = ReadHotkeySetting<UINT>(hotkey.setting, hotkey.name, ParseHotkeyKey);
  }
  g_settings.resizeStepPercent = ReadBoundedIntSetting(L"hotkeys.WindowSize.StepPercent", L"Step Size", 1, 50, 5);

  auto diagnosticsPath = StringSetting::make(L"diagnostics.DiagnosticsOutputPath");
  g_settings.diagnosticsOutputPath =
      *diagnosticsPath.get() ? diagnosticsPath.get() : kDefaultDiagnosticsOutputPath;

  TrayUi::LoadSettings();
}
} // namespace Configuration
