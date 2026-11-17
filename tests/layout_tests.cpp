#include "../src/multiwm/multiwm.cpp"
#include <cstdio>

// Exercise real model/planner behavior without creating or moving native windows.

static void RunAdaptiveDwindleTests() {
  g_settings = SettingsState{};
  g_settings.adaptiveDwindleSlots = 2;
  g_settings.adaptiveDwindlePercentage = 40;
  const RECT area{0, 0, 1000, 1000};
  const HWND first = reinterpret_cast<HWND>(1);
  const HWND second = reinterpret_cast<HWND>(2);
  const HWND large = reinterpret_cast<HWND>(3);
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) {
      std::fprintf(stderr, "FAILED: %s\n", message);
      std::exit(1);
    }
    ++checks;
  };

  // Use the actual model, planner and resize learner without creating windows.
  for (bool reverse : {false, true}) {
    g_settings.reverseDwindle = reverse;
    Workspace original;
    original.SetLayout(TileLayout::Dwindle);
    for (HWND hwnd : {first, second, large}) {
      WindowRecord record;
      record.hwnd = hwnd;
      record.pid = 1;
      record.constraints = {hwnd == large ? 650 : 80,
                            hwnd == large ? 650 : 80, 96, true};
      check(original.AdmitTiled(record), "admit test window");
    }
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(original, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "fit both adaptive dividers");
    check(Layout::RectWidth(rects[0]) == 350 && Layout::RectHeight(rects[1]) == 350,
          "minimums temporarily push both dividers");
    check(original.DwindleRatios() == std::vector<double>({0.5, 0.5}),
          "planning preserves preferred ratios");

    Workspace closed = original;
    check(closed.Forget(large), "close demanding window");
    check(Layout::BuildWorkspaceLayoutPlan(closed, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 500,
          "close restores preferred split");

    Workspace suspended = original;
    check(suspended.Suspend(large, SuspensionReason::Minimized), "minimize demanding window");
    check(Layout::BuildWorkspaceLayoutPlan(suspended, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 500,
          "minimize restores preferred split");
    check(suspended.Restore(large), "restore demanding window");
    check(Layout::BuildWorkspaceLayoutPlan(suspended, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 350,
          "restore reapplies temporary split");

    Workspace relaxed = original;
    check(relaxed.UpdateConstraints(large, {80, 80, 96, true}), "relax minimums");
    check(Layout::BuildWorkspaceLayoutPlan(relaxed, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 500 &&
              Layout::RectHeight(rects[1]) == 500, "relaxed minimums restore both preferences");

    Workspace dragged = original;
    check(Layout::BuildWorkspaceLayoutPlan(dragged, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "plan before manual resize");
    MoveSizeGesture gesture;
    gesture.hasRects = true;
    gesture.intent = MoveSizeIntent::Resize;
    gesture.start = gesture.end = rects[0];
    if (reverse) gesture.end.left += 2;
    else gesture.end.right -= 2;
    check(dragged.LearnDwindleResize(area, 0, 0, gesture), "learn a two-pixel manual drag");
    check(std::abs(dragged.DwindleRatios()[0] - 0.348) < 1e-9 &&
              dragged.DwindleRatios()[1] == 0.5,
          "manual drag starts from visible split and preserves untouched preference");
    check(Layout::BuildWorkspaceLayoutPlan(dragged, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 348,
          "manual divider stays at dragged position while minimum remains");
    check(dragged.Forget(large), "close after manual drag");
    check(Layout::BuildWorkspaceLayoutPlan(dragged, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 348,
          "manual preference survives closing demanding window");

    Workspace nestedDrag = original;
    check(Layout::BuildWorkspaceLayoutPlan(nestedDrag, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "plan before nested resize");
    gesture.start = gesture.end = rects[1];
    if (reverse) gesture.end.top += 2;
    else gesture.end.bottom -= 2;
    check(nestedDrag.LearnDwindleResize(area, 0, 1, gesture), "learn nested divider drag");
    check(nestedDrag.DwindleRatios()[0] == 0.5 &&
              std::abs(nestedDrag.DwindleRatios()[1] - 0.348) < 1e-9,
          "nested drag updates only touched preference");
    check(nestedDrag.UpdateConstraints(large, {80, 80, 96, true}), "relax after nested drag");
    check(Layout::BuildWorkspaceLayoutPlan(nestedDrag, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 500 &&
              Layout::RectHeight(rects[1]) == 348, "nested preference persists after constraint relaxes");

    Workspace jitter = original;
    check(Layout::BuildWorkspaceLayoutPlan(jitter, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "plan before unchanged gesture");
    gesture.start = gesture.end = rects[0];
    check(!jitter.LearnDwindleResize(area, 0, 0, gesture) &&
              jitter.DwindleRatios() == original.DwindleRatios(),
          "untouched temporary divider does not become a preference");

    Workspace cycled = original;
    cycled.SetLayout(TileLayout::Monocle);
    cycled.SetLayout(TileLayout::Dwindle);
    check(cycled.DwindleRatios() == original.DwindleRatios(),
          "layout cycling remembers preferences rather than fitted geometry");
    check(Layout::BuildWorkspaceLayoutPlan(cycled, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 350,
          "layout cycling restores temporary fitting");

    const RECT widerArea{0, 0, 1600, 1600};
    check(Layout::BuildWorkspaceLayoutPlan(original, widerArea, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 800 &&
              Layout::RectHeight(rects[1]) == 800, "larger work area releases temporary fitting");
    check(Layout::BuildWorkspaceLayoutPlan(original, area, 10, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 340 &&
              Layout::RectHeight(rects[1]) == 340, "temporary fitting includes gaps");

    g_settings.adaptiveDwindleSlots = 0;
    check(Layout::BuildWorkspaceLayoutPlan(original, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Unsatisfiable, "zero adaptive slots keeps geometry fixed");
    g_settings.adaptiveDwindleSlots = 2;
    g_settings.adaptiveDwindlePercentage = 0;
    check(Layout::BuildWorkspaceLayoutPlan(original, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Unsatisfiable, "zero adaptive percentage keeps geometry fixed");
    g_settings.adaptiveDwindlePercentage = 10;
    check(Layout::BuildWorkspaceLayoutPlan(original, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Unsatisfiable, "fitting respects automatic ratio envelope");
    g_settings.adaptiveDwindlePercentage = 40;
  }
  std::printf("Passed %d adaptive Dwindle checks.\n", checks);
}

static void RunWeightedResizeTests() {
  g_settings = SettingsState{};
  const RECT area{0, 0, 1000, 1000};
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) {
      std::fprintf(stderr, "FAILED: %s\n", message);
      std::exit(1);
    }
    ++checks;
  };
  for (TileLayout layout : {TileLayout::Columns, TileLayout::Rows,
                            TileLayout::MasterStack, TileLayout::MasterStackH}) {
    const bool stack = layout == TileLayout::MasterStack || layout == TileLayout::MasterStackH;
    const bool vertical = layout == TileLayout::Rows || layout == TileLayout::MasterStack;
    const size_t firstIndex = stack ? 1 : 0;
    auto span = [&](const RECT& rect) { return vertical ? Layout::RectHeight(rect) : Layout::RectWidth(rect); };
    auto weights = [&](const Workspace& workspace) {
      return stack ? workspace.StackWeights() : workspace.GridWeights();
    };
    for (LONG gap : {0, 10}) {
      Workspace original;
      original.SetLayout(layout);
      for (size_t index = 0; index < firstIndex + 3; ++index) {
        WindowRecord record;
        record.hwnd = reinterpret_cast<HWND>(index + 1);
        record.pid = 1;
        record.constraints = {80, 80, 96, true};
        if (index == firstIndex) {
          if (vertical) record.constraints.minHeight = 650;
          else record.constraints.minWidth = 650;
        }
        check(original.AdmitTiled(record), "admit test window");
      }
      const HWND demanding = original.TiledWindows()[firstIndex];
      std::vector<RECT> initialRects;
      check(Layout::BuildWorkspaceLayoutPlan(original, area, gap, &initialRects) ==
                Layout::LayoutPlanStatus::Success, "constrained layout fits");
      check(span(initialRects[firstIndex]) == 650, "minimum displaces divider");
      check(weights(original) == std::vector<double>({1, 1, 1}), "automatic fitting preserves weights");
      Workspace automatic = original;
      automatic.UpdateConstraints(demanding, {80, 80, 96, true});
      std::vector<RECT> rects;
      check(Layout::BuildWorkspaceLayoutPlan(automatic, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) < 400,
            "unmodified temporary divider returns after relaxation");

      // Resizing either side of the same divider must learn the same position.
      for (bool leadingEdge : {false, true}) {
        Workspace dragged = original;
        const size_t resizedIndex = firstIndex + (leadingEdge ? 1 : 0);
        MoveSizeGesture gesture;
        gesture.intent = MoveSizeIntent::Resize;
        gesture.hasRects = true;
        gesture.start = gesture.end = initialRects[resizedIndex];
        if (vertical && leadingEdge) gesture.end.top += 2;
        else if (vertical) gesture.end.bottom += 2;
        else if (leadingEdge) gesture.end.left += 2;
        else gesture.end.right += 2;
        check(stack ? dragged.LearnMasterStackResize(area, gap, resizedIndex, gesture)
                    : dragged.LearnGridResize(area, gap, resizedIndex, gesture),
              "learn a two-pixel drag from either side");
        check(weights(dragged)[2] == weights(original)[2], "unrelated preference stays unchanged");
        check(Layout::BuildWorkspaceLayoutPlan(dragged, area, gap, &rects) ==
                  Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) == 652 &&
                  span(rects[firstIndex + 1]) == span(initialRects[firstIndex + 1]) - 2 &&
                  span(rects[firstIndex + 2]) == span(initialRects[firstIndex + 2]),
              "drag moves only chosen divider");
        const auto savedWeights = weights(dragged);
        WindowConstraints larger = dragged.Find(demanding)->constraints;
        if (vertical) larger.minHeight = 700;
        else larger.minWidth = 700;
        dragged.UpdateConstraints(demanding, larger);
        check(Layout::BuildWorkspaceLayoutPlan(dragged, area, gap, &rects) ==
                  Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) == 700 &&
                  weights(dragged) == savedWeights, "later constraints remain temporary");
        dragged.UpdateConstraints(demanding, {80, 80, 96, true});
        check(Layout::BuildWorkspaceLayoutPlan(dragged, area, gap, &rects) ==
                  Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) == 652,
              "manual position survives relaxing minimum");
      }

      // Moving another divider must not save an untouched window's temporary size.
      Workspace otherDivider = original;
      MoveSizeGesture gesture;
      gesture.intent = MoveSizeIntent::Resize;
      gesture.hasRects = true;
      gesture.start = gesture.end = initialRects[firstIndex + 1];
      if (vertical) gesture.end.bottom += 2;
      else gesture.end.right += 2;
      check(stack ? otherDivider.LearnMasterStackResize(area, gap, firstIndex + 1, gesture)
                  : otherDivider.LearnGridResize(area, gap, firstIndex + 1, gesture),
            "resize flexible neighbors beside pinned window");
      check(weights(otherDivider)[0] == 1.0, "untouched constrained preference remains original");
      check(Layout::BuildWorkspaceLayoutPlan(otherDivider, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) == 650 &&
                span(rects[firstIndex + 1]) == span(initialRects[firstIndex + 1]) + 2,
            "flexible divider follows drag beside pinned window");
      otherDivider.UpdateConstraints(demanding, {80, 80, 96, true});
      check(Layout::BuildWorkspaceLayoutPlan(otherDivider, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) < 400,
            "untouched constrained window returns to its preference");

      Workspace pinned = original;
      const LONG remainingMinimum = (1000 - 2 * gap - 650) / 2;
      for (size_t index : {firstIndex + 1, firstIndex + 2}) {
        WindowConstraints minimums{80, 80, 96, true};
        if (vertical) minimums.minHeight = remainingMinimum;
        else minimums.minWidth = remainingMinimum;
        pinned.UpdateConstraints(pinned.TiledWindows()[index], minimums);
      }
      check(Layout::BuildWorkspaceLayoutPlan(pinned, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success, "fully pinned allocation fits");
      gesture.start = gesture.end = rects[firstIndex];
      if (vertical) gesture.end.bottom += 2;
      else gesture.end.right += 2;
      check(!(stack ? pinned.LearnMasterStackResize(area, gap, firstIndex, gesture)
                    : pinned.LearnGridResize(area, gap, firstIndex, gesture)) &&
                weights(pinned) == weights(original),
            "unavailable drag does not change preferences");

      Workspace ordinary = automatic;
      check(Layout::BuildWorkspaceLayoutPlan(ordinary, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success, "ordinary geometry fits");
      const LONG oldFirstSpan = span(rects[firstIndex]);
      const LONG oldLastSpan = span(rects[firstIndex + 2]);
      gesture.start = gesture.end = rects[firstIndex];
      if (vertical) gesture.end.bottom += 2;
      else gesture.end.right += 2;
      check(stack ? ordinary.LearnMasterStackResize(area, gap, firstIndex, gesture)
                  : ordinary.LearnGridResize(area, gap, firstIndex, gesture), "learn ordinary divider drag");
      check(Layout::BuildWorkspaceLayoutPlan(ordinary, area, gap, &rects) ==
                Layout::LayoutPlanStatus::Success && span(rects[firstIndex]) == oldFirstSpan + 2 &&
                span(rects[firstIndex + 2]) == oldLastSpan,
            "ordinary resize behavior stays local");
    }
  }
  std::printf("Passed %d weighted resize checks.\n", checks);
}

// Characterization test for the Dwindle planner. Generates many deterministic
// workspaces (window counts, areas, gaps, saved ratios, minimum sizes, reverse
// and adaptive settings) and folds every planner output into one digest.
// The digest is the behavior: a pure refactor must print the same value, and an
// intended behavior change must update kExpectedDigest deliberately.
static void RunDwindlePlannerDigest() {
  // Recorded from the implementation as of 2026-11-04 (commit 59f0168).
  constexpr uint64_t kExpectedDigest = 0x2c5aa1a1e5f7011eull;
  uint64_t digest = 1469598103934665603ull;
  auto mix = [&](uint64_t value) {
    digest ^= value;
    digest *= 1099511628211ull;
  };
  auto mixRect = [&](const RECT& rect) {
    mix(static_cast<uint32_t>(rect.left)); mix(static_cast<uint32_t>(rect.top));
    mix(static_cast<uint32_t>(rect.right)); mix(static_cast<uint32_t>(rect.bottom));
  };
  uint32_t seed = 20261104;
  auto random = [&](uint32_t bound) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % bound;
  };

  const SettingsState savedSettings = g_settings;
  size_t cases = 0, planned = 0, reassigned = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    g_settings.reverseDwindle = random(2) != 0;
    g_settings.adaptiveDwindleSlots = random(4);
    g_settings.adaptiveDwindlePercentage = static_cast<LONG>(random(41));
    const LONG gap = static_cast<LONG>(random(20));
    const size_t count = 2 + random(6);
    const RECT area{0, 0, 800 + static_cast<LONG>(random(1800)), 600 + static_cast<LONG>(random(900))};

    std::vector<double> ratios(count);
    for (double& ratio : ratios) ratio = 0.2 + random(61) / 100.0;
    std::vector<RECT> frames(count);
    Layout::LayoutDwindle(area, gap, 0, count, ratios, g_settings.reverseDwindle, frames);

    std::vector<WorkspaceGeometryItem> items;
    for (size_t i = 0; i < count; ++i) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(0x1000 + i * 4);
      record.pid = 1;
      record.canMove = false;
      if (random(3) != 0) {
        record.constraints.valid = true;
        record.constraints.minWidth = 1 + static_cast<LONG>(random(1200));
        record.constraints.minHeight = 1 + static_cast<LONG>(random(800));
      }
      items.push_back({record, frames[i]});
    }
    const Workspace workspace = Workspace::AdoptGeometry(TileLayout::Dwindle, area, gap, 0.5, items);

    std::vector<RECT> plan;
    const auto status = Layout::BuildWorkspaceLayoutPlan(workspace, area, gap, &plan);
    mix(static_cast<uint64_t>(status));
    if (status == Layout::LayoutPlanStatus::Success) {
      ++planned;
      for (const RECT& rect : plan) mixRect(rect);
    }

    Workspace converted = workspace;
    size_t moved = 0;
    const bool ok = Layout::ReassignDwindleLayoutConversionForConstraints(converted, area, gap, &moved);
    mix(ok ? 1 : 0);
    if (ok) {
      ++reassigned;
      mix(moved);
      for (HWND hwnd : converted.TiledWindows()) mix(reinterpret_cast<uintptr_t>(hwnd));
    }
    ++cases;
  }
  g_settings = savedSettings;

  if (planned == 0 || reassigned == 0 || planned == cases) {
    std::fprintf(stderr, "FAILED: digest inputs do not exercise all planner outcomes\n");
    std::exit(1);
  }
  if (kExpectedDigest && digest != kExpectedDigest) {
    std::fprintf(stderr, "FAILED: Dwindle planner digest %016llx != expected %016llx\n",
                 static_cast<unsigned long long>(digest), static_cast<unsigned long long>(kExpectedDigest));
    std::exit(1);
  }
  std::printf("Dwindle planner digest %016llx over %zu cases (%zu planned, %zu reassigned).\n",
              static_cast<unsigned long long>(digest), cases, planned, reassigned);
}

// Characterizes the pure settings parsers over awkward inputs (spacing, bounds,
// separators, case, junk). The digest pins accepted values and rejections.
static void RunSettingsParserDigest() {
  uint64_t digest = 1469598103934665603ull;
  auto mix = [&](uint64_t value) { digest ^= value; digest *= 1099511628211ull; };
  const PCWSTR corpus[] = {
      L"", L" ", L"6, 6, 6, 6", L"0,0,0,0", L"500,500,500,500", L"501,0,0,0", L"-1,0,0,0",
      L"  1 ,2,  3,4  ", L"1,2,3", L"1,2,3,4,5", L"1;2;3;4", L"a,b,c,d", L"1,2,3,4x", L"\t7,\t8,\t9,\t10\t",
      L"960, 640", L"100,100", L"4000,4000", L"99,100", L"100,4001", L"960 640", L"960,", L",640",
      L" 1200 , 800 ", L"960,640,1", L"0x10,200",
      L"T", L"t", L"F5", L"f12", L"F13", L"F0", L"Space", L"space", L"PgDn", L"Del", L"Return",
      L",", L".", L"<", L"?", L"~", L"!", L"(", L"9", L"AB", L"Esc", L"Left",
      L"alt", L"ctrl", L"alt+shift", L"ctrl+alt", L"ctrl+shift", L"win", L"ALT",
      L"master_stack", L"master_stack_h", L"columns", L"rows", L"dwindle", L"bsp", L"monocle",
      L"floating", L"Floating", L"unknown",
  };
  for (PCWSTR text : corpus) {
    WorkspaceInsets insets{};
    const bool insetsOk = Configuration::ParseWorkspaceInsetsSetting(text, &insets);
    mix(insetsOk);
    if (insetsOk) { mix(insets.left); mix(insets.top); mix(insets.right); mix(insets.bottom); }
    FloatingDefaultSize size{};
    const bool sizeOk = Configuration::ParseFloatingDefaultSizeSetting(text, &size);
    mix(sizeOk);
    if (sizeOk) { mix(size.width); mix(size.height); }
    mix(Configuration::ParseHotkeyKey(text));
    mix(Configuration::ParseModifiers(text));
    TileLayout layout{};
    mix(Configuration::TryParseLayoutSetting(text, &layout) ? 1 + static_cast<uint64_t>(layout) : 0);
  }
  // Recorded from the implementation as of 2026-11-04.
  if (digest != 0x88d4c12ceb48ea3eull) {
    std::fprintf(stderr, "FAILED: settings parser digest %016llx changed\n", static_cast<unsigned long long>(digest));
    std::exit(1);
  }
  std::printf("Settings parser digest %016llx over %zu inputs.\n",
              static_cast<unsigned long long>(digest), std::size(corpus));
}

static void RunRuleFieldParserTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  using Configuration::MonitorIdParse;
  check(Configuration::ParseRuleMonitor(L"Primary", false) == RuleMonitor::Primary, "selector ignores case");
  check(Configuration::ParseRuleMonitor(L"non_primary", false) == RuleMonitor::NonPrimary, "non-primary selector");
  check(Configuration::ParseRuleMonitor(L"specific", false) == RuleMonitor::Specific, "specific selector");
  check(Configuration::ParseRuleMonitor(L"", true) == RuleMonitor::Any, "blank means Any where allowed");
  check(Configuration::ParseRuleMonitor(L"any", true) == RuleMonitor::Any, "\"any\" where allowed");
  check(!Configuration::ParseRuleMonitor(L"any", false), "limit rules reject \"any\"");
  check(!Configuration::ParseRuleMonitor(L"left", true), "unknown selector is invalid");

  std::wstring id;
  check(Configuration::ParseMonitorIdSetting(L"  ", &id) == MonitorIdParse::Empty, "blank ID is Empty");
  check(Configuration::ParseMonitorIdSetting(L" iface:abc ", &id) == MonitorIdParse::Valid && id == L"IFACE:ABC",
        "ID is trimmed and upper-cased");
  check(Configuration::ParseMonitorIdSetting(L"GDI:\\\\.\\DISPLAY1", &id) == MonitorIdParse::Valid, "GDI fallback ID");
  check(Configuration::ParseMonitorIdSetting(L"DISPLAY1", &id) == MonitorIdParse::Invalid, "ID needs a known prefix");

  const auto values = Configuration::ParseWindowRuleList(L" a.exe , b\\,c.exe,, ,d ");
  check(values == std::vector<std::wstring>{L"a.exe", L"b,c.exe", L"d"},
        "rule list trims, drops blanks and keeps escaped commas");
  check(Configuration::ParseWindowRuleList(L"").empty(), "empty rule list");
  std::printf("Passed %d rule field parser checks.\n", checks);
}

// The companion's next-slot hint is only useful if it is exactly where MultiWM
// puts the window. Compare the prediction with what discovery's own admission
// (Reconcile::AdmitUntrackedSnapshot) plus the planner produce for the same
// workspace, across layouts, insertion positions and constrained neighbours.
static void RunAdmissionPredictionTests() {
  g_settings = SettingsState{};
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const Platform::Monitors::WorkspaceMetrics metrics{{0, 0, 1920, 1040}, 96, 6};
  // Never a test-owned or real window here: admission only reads its (absent) properties.
  const HWND incoming = reinterpret_cast<HWND>(0x7FFF0000);
  const TileLayout layouts[] = {TileLayout::MasterStack, TileLayout::MasterStackH, TileLayout::Dwindle,
                                TileLayout::Columns, TileLayout::Rows, TileLayout::Monocle};
  const AutomaticNewWindowPosition positions[] = {AutomaticNewWindowPosition::LastSlot,
                                                  AutomaticNewWindowPosition::AfterFocused};

  for (bool reverse : {false, true}) {
    for (AutomaticNewWindowPosition position : positions) {
      for (TileLayout layout : layouts) {
        for (size_t existing = 0; existing <= 5; ++existing) {
          for (bool constrained : {false, true}) {
            g_settings.reverseDwindle = reverse;
            g_settings.automaticNewWindowPosition = position;
            Workspace workspace;
            workspace.SetLayout(layout);
            for (size_t i = 0; i < existing; ++i) {
              WindowRecord record;
              record.hwnd = reinterpret_cast<HWND>(0x1000 + i * 4);
              record.pid = 1;
              // A wide first window forces the constrained planner path.
              if (constrained && i == 0) record.constraints = {1100, 300, 96, true};
              check(workspace.AdmitTiled(record), "admit existing window");
            }
            if (existing > 1) workspace.RememberFocusedWindow(workspace.TiledWindows()[1]);

            const Placement::AdmissionPrediction prediction = Placement::PredictAutomaticAdmission(
                workspace, metrics, 0, position == AutomaticNewWindowPosition::AfterFocused);
            check(!prediction.floatingFrame, "tiled layouts predict no floating frame");

            Workspace admitted = workspace;
            check(Reconcile::AdmitUntrackedSnapshot(admitted, {incoming}, true, true),
                  "discovery admits the incoming window");
            std::vector<RECT> rects;
            const bool planned = Layout::BuildWorkspaceLayoutPlan(admitted, metrics.workArea, metrics.gap,
                                                                  &rects) == Layout::LayoutPlanStatus::Success;
            check(planned == prediction.tiledFrame.has_value(), "prediction exists exactly when the plan succeeds");
            if (planned) {
              const RECT& actual = rects[admitted.TiledIndex(incoming)];
              check(EqualRect(&actual, &*prediction.tiledFrame), "predicted slot matches real admission");
            }
            check(workspace.ActiveCount() == existing && !workspace.HasRecord(Placement::kPredictedWindow),
                  "prediction leaves the workspace unchanged");
          }
        }
      }
    }
  }

  // The automatic window limit: a full workspace gets no tiled hint.
  g_settings = SettingsState{};
  Workspace full;
  full.SetLayout(TileLayout::Columns);
  for (size_t i = 0; i < 3; ++i) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(0x2000 + i * 4);
    check(full.AdmitTiled(record), "admit window for limit test");
  }
  check(!Placement::PredictAutomaticAdmission(full, metrics, 3, false).tiledFrame, "limit reached: no hint");
  check(Placement::PredictAutomaticAdmission(full, metrics, 4, false).tiledFrame.has_value(),
        "below the limit: hint");

  // The Floating layout centers the DPI-scaled default size, clamped to the work area.
  Workspace floating;
  floating.SetLayout(TileLayout::Floating);
  const auto centered = Placement::PredictAutomaticAdmission(floating, metrics, 0, false);
  const RECT expectedCentered{480, 200, 1440, 840};
  check(!centered.tiledFrame && centered.floatingFrame && EqualRect(&*centered.floatingFrame, &expectedCentered),
        "floating default size is centered");
  const Platform::Monitors::WorkspaceMetrics highDpi{{0, 0, 1920, 1040}, 192, 12};
  const auto clamped = Placement::PredictAutomaticAdmission(floating, highDpi, 0, false);
  check(clamped.floatingFrame && EqualRect(&*clamped.floatingFrame, &highDpi.workArea),
        "scaled default size is clamped to the work area");

  std::printf("Passed %d admission prediction checks.\n", checks);
}

// Rules travel to the companion in the snapshot; what it decodes must match
// what MultiWM matches against.
static void RunCompanionRuleEncodingTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  std::vector<WindowRule> rules(4);
  rules[0].treatment = WindowRuleTreatment::Exclude;
  rules[0].processNames = {L"Steam.exe"};
  rules[1].treatment = WindowRuleTreatment::TraceToOwner;
  rules[1].classNames = {L"OwnerHelper"};
  rules[2].treatment = WindowRuleTreatment::FloatingPlacementOverride;
  rules[2].preserveFloatingSize = false;
  rules[2].floatingSizeDip = {720, 480};
  rules[2].classNames = {L"Chrome_WidgetWin_1"};
  rules[2].titleFragments = {L"Préférences, avancées", L"设置"};
  rules[3].treatment = WindowRuleTreatment::DefaultToFloating;
  rules[3].processNames = {L"a.exe", L"b.exe"};

  std::vector<uint8_t> buffer(CompanionAbi::kRulesCapacity);
  bool complete = false;
  const uint32_t size =
      CompanionAbi::SerializeRules(rules, buffer.data(), CompanionAbi::kRulesCapacity, &complete);
  std::vector<CompanionAbi::SnapshotRule> decoded;
  check(complete && CompanionAbi::DeserializeRules(buffer.data(), size, &decoded), "rules round-trip");
  check(decoded.size() == 3, "trace-to-owner rules are left out");
  auto same = [](const WindowRule& original, const CompanionAbi::SnapshotRule& copy) {
    return original.treatment == copy.treatment && original.processNames == copy.processNames &&
           original.classNames == copy.classNames && original.titleFragments == copy.titleFragments &&
           original.preserveFloatingSize == copy.preserveFloatingSize &&
           original.useWorkspaceSize == copy.useWorkspaceSize &&
           original.floatingSizeDip.width == copy.floatingSizeDip.width &&
           original.floatingSizeDip.height == copy.floatingSizeDip.height;
  };
  check(same(rules[0], decoded[0]) && same(rules[2], decoded[1]) && same(rules[3], decoded[2]),
        "decoded rules keep every field");

  // A full buffer keeps whole rules only and reports itself incomplete.
  const uint32_t firstRuleSize =
      CompanionAbi::SerializeRules(std::vector<WindowRule>{rules[0]}, buffer.data(), CompanionAbi::kRulesCapacity,
                                   &complete);
  const uint32_t truncated =
      CompanionAbi::SerializeRules(rules, buffer.data(), firstRuleSize + 3, &complete);
  check(!complete && truncated == firstRuleSize, "overflow stops at a rule boundary");
  check(CompanionAbi::DeserializeRules(buffer.data(), truncated, &decoded) && decoded.size() == 1,
        "the kept prefix decodes");
  check(!CompanionAbi::DeserializeRules(buffer.data(), truncated - 1, &decoded) && decoded.empty(),
        "cut data is rejected");
  std::printf("Passed %d companion rule encoding checks.\n", checks);
}

static void RunCompanionSnapshotRecoveryTests() {
  auto section = std::make_unique<CompanionAbi::SnapshotSection>();
  auto copy = std::make_unique<CompanionAbi::SnapshotBody>();
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  // A new writer can inherit either a completed snapshot or an interrupted
  // write. Readers must reject the body throughout takeover, then recover.
  for (LONG previous : {0L, 2L, 1L, 3L, LONG_MAX}) {
    section->sequence = previous;
    CompanionAbi::BeginWrite(section.get());
    check((section->sequence & 1) && section->sequence != previous, "takeover advances to a new odd sequence");
    check(!CompanionAbi::ReadConsistentBody(section.get(), copy.get()), "takeover hides unfinished data");
    section->body.flags = CompanionAbi::kFlagRunning;
    CompanionAbi::EndWrite(section.get());
    check(CompanionAbi::ReadConsistentBody(section.get(), copy.get()) &&
              copy->flags == CompanionAbi::kFlagRunning, "completed takeover is readable");
    CompanionAbi::BeginWrite(section.get());
    section->body.flags = 0;
    CompanionAbi::EndWrite(section.get());
    check(CompanionAbi::ReadConsistentBody(section.get(), copy.get()) && copy->flags == 0,
          "publishing continues after takeover");
  }
  std::printf("Passed %d companion snapshot recovery checks.\n", checks);
}

int main() {
  RunSettingsParserDigest();
  RunRuleFieldParserTests();
  RunAdaptiveDwindleTests();
  RunWeightedResizeTests();
  RunDwindlePlannerDigest();
  RunAdmissionPredictionTests();
  RunCompanionRuleEncodingTests();
  RunCompanionSnapshotRecoveryTests();
}
