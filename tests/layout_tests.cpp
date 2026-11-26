#include "../src/multiwm/multiwm.cpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>

// Exercise real model/planner behavior without creating or moving native windows.

static void RunAdaptiveSplitTests() {
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
    // A drag accepts the shown layout: the untouched divider is saved where
    // adaptive fitting showed it (350 of 1000), not at its earlier 0.5.
    check(std::abs(dragged.DwindleRatios()[0] - 0.348) < 1e-9 &&
              std::abs(dragged.DwindleRatios()[1] - 0.35) < 1e-9,
          "manual drag starts from visible split and accepts the shown layout");
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
    check(std::abs(nestedDrag.DwindleRatios()[0] - 0.35) < 1e-9 &&
              std::abs(nestedDrag.DwindleRatios()[1] - 0.348) < 1e-9,
          "nested drag accepts the shown layout");
    check(nestedDrag.UpdateConstraints(large, {80, 80, 96, true}), "relax after nested drag");
    check(Layout::BuildWorkspaceLayoutPlan(nestedDrag, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 350 &&
              Layout::RectHeight(rects[1]) == 348, "the accepted layout persists after the constraint relaxes");

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
  g_settings.adaptiveDwindlePercentage = 15;
  for (TileLayout layout : {TileLayout::Dwindle, TileLayout::Bsp}) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (HWND hwnd : {first, second}) {
      WindowRecord record;
      record.hwnd = hwnd;
      record.pid = 1;
      check(workspace.AdmitTiled(record), "admit a window for the saved split test");
    }
    check(workspace.StepTiledSize(area, 0, workspace.TiledIndex(first), true, 30), "save an 80/20 split");
    std::vector<RECT> rects;
    auto plan = [&] { return Layout::BuildWorkspaceLayoutPlan(workspace, area, 0, &rects); };
    check(plan() == Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 800,
          "the saved position gives the first window 80 percent");
    workspace.UpdateConstraints(second, {210, 1, 96, true});
    check(plan() == Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 790,
          "a one-point adjustment starts from the saved divider");
    workspace.UpdateConstraints(second, {360, 1, 96, true});
    check(plan() == Layout::LayoutPlanStatus::Unsatisfiable, "adjustment stays within 15 points of the saved divider");
    workspace.UpdateConstraints(second, {80, 1, 96, true});
    workspace.UpdateConstraints(first, {900, 1, 96, true});
    check(plan() == Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 900,
          "the saved range allows up to the 90 percent ratio limit");
    workspace.UpdateConstraints(first, {910, 1, 96, true});
    check(plan() == Layout::LayoutPlanStatus::Unsatisfiable, "adjustment respects the 90 percent ratio limit");
    workspace.UpdateConstraints(first, {80, 1, 96, true});
    check(plan() == Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 800,
          "relaxing constraints restores the saved divider");

    if (layout == TileLayout::Dwindle) {
      workspace.UpdateConstraints(first, {300, 1, 96, true});
      workspace.UpdateConstraints(second, {700, 1, 96, true});
      check(plan() == Layout::LayoutPlanStatus::Unsatisfiable, "these minimums require a Dwindle reorder");
      size_t moved = 0;
      check(Layout::ReassignDwindleLayoutConversionForConstraints(workspace, area, 0, &moved) && moved == 2,
            "Dwindle reorder searches around the saved divider");
      check(plan() == Layout::LayoutPlanStatus::Success && Layout::RectWidth(rects[0]) == 700,
            "the reordered Dwindle split fits at 70/30");
    }
  }
  std::printf("Passed %d adaptive split checks.\n", checks);
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

// The floating placement rule both mods share (shared/geometry.h): MultiWM
// places floating windows with it and the companion predicts them with it.
// A window's size limits are fitted around the planned centre, inside the work
// area.
static void RunFloatingPlacementTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  auto sizeIs = [](SIZE size, LONG width, LONG height) { return size.cx == width && size.cy == height; };
  const SIZE size{1100, 735};
  check(sizeIs(SharedGeometry::ClampSizeToLimits(size, {1, 1, 0, 0}), 1100, 735),
        "limits the size already satisfies change nothing");
  check(sizeIs(SharedGeometry::ClampSizeToLimits(size, {1, 1, 500, 400}), 500, 400), "a maximum shrinks the size");
  check(sizeIs(SharedGeometry::ClampSizeToLimits(size, {1300, 900, 0, 0}), 1300, 900), "a minimum grows the size");
  check(sizeIs(SharedGeometry::ClampSizeToLimits(size, {1, 900, 500, 0}), 500, 900), "each axis takes its own limit");

  const RECT box{0, 0, 1000, 800};
  RECT expected{};
  RECT moved = SharedGeometry::MoveInsideArea({100, 100, 400, 300}, box);
  expected = {100, 100, 400, 300};
  check(EqualRect(&moved, &expected), "a frame inside the area stays put");
  moved = SharedGeometry::MoveInsideArea({-50, 700, 250, 900}, box);
  expected = {0, 600, 300, 800};
  check(EqualRect(&moved, &expected), "a frame across an edge moves inside, keeping its size");
  moved = SharedGeometry::MoveInsideArea({-100, -100, 1100, 300}, box);
  expected = {0, 0, 1200, 400};
  check(EqualRect(&moved, &expected), "an axis larger than the area starts at its left or top edge");

  // A work area that covers the whole monitor (hidden taskbar, insets about 0)
  // keeps 3 px on every edge; any other work area is left as it is.
  const RECT monitorRect{0, 0, 1920, 1080};
  RECT tileArea = SharedGeometry::KeepTilesOffWholeMonitor(monitorRect, monitorRect);
  expected = {3, 3, 1917, 1077};
  check(EqualRect(&tileArea, &expected), "zero insets on a whole-monitor work area keep 3 px on every edge");
  tileArea = SharedGeometry::KeepTilesOffWholeMonitor({2, 1, 1919, 1078}, monitorRect);
  check(EqualRect(&tileArea, &expected), "edges within the tolerance move to 3 px too, evenly");
  check(!SharedGeometry::CoversMonitor(tileArea, monitorRect), "the result never covers the monitor");
  check(!SharedGeometry::IsMonitorSized(tileArea, monitorRect), "nor is it the monitor's size");

  // Fullscreen is the monitor's size, wherever the window is.
  check(SharedGeometry::IsMonitorSized({0, 0, 1920, 1080}, monitorRect), "the monitor's own rect is fullscreen");
  check(SharedGeometry::IsMonitorSized({300, 200, 2220, 1280}, monitorRect),
        "a monitor-sized window part-way to its corner is fullscreen");
  check(SharedGeometry::IsMonitorSized({1, -1, 1922, 1081}, monitorRect), "within the tolerance is fullscreen");
  check(!SharedGeometry::IsMonitorSized({-7, -7, 1927, 1087}, monitorRect),
        "a work-area window with invisible borders under a hidden taskbar isn't fullscreen");
  check(!SharedGeometry::IsMonitorSized({0, 0, 1920, 1040}, monitorRect), "one axis alone isn't fullscreen");
  tileArea = SharedGeometry::KeepTilesOffWholeMonitor({0, 0, 1920, 1022}, monitorRect);
  expected = {0, 0, 1920, 1022};
  check(EqualRect(&tileArea, &expected), "a visible taskbar or any larger inset leaves the work area alone");

  // The floating placement rule both mods share. A 1200x1000 work area.
  using SharedGeometry::FloatingSizeSource;
  const RECT area{0, 0, 1200, 1000};
  auto request = [&](FloatingSizeSource source) {
    SharedGeometry::FloatingRequest r;
    r.source = source;
    r.workArea = area;
    r.defaultSize = SIZE{600, 400};
    r.current = SIZE{300, 200};
    return r;
  };
  auto plan = [](const SharedGeometry::FloatingRequest& r) { return SharedGeometry::PlanFloatingPlacement(r); };
  auto is = [](const std::optional<SharedGeometry::FloatingPlacement>& p, RECT rect, bool moveOnly) {
    return p && EqualRect(&p->frame, &rect) && p->moveOnly == moveOnly;
  };
  auto appliesLimits = [](const std::optional<SharedGeometry::FloatingPlacement>& p) {
    return p && p->appliesLimits;
  };
  const SharedGeometry::FloatingSizeRule sizeRule{false, {800, 500}};
  const SharedGeometry::FloatingSizeRule preserveRule{true, {}};

  auto r = request(FloatingSizeSource::Fallback);
  check(is(plan(r), {300, 300, 900, 700}, false), "fallback: the default size, centred");
  r.rule = sizeRule;
  check(is(plan(r), {200, 250, 1000, 750}, false), "fallback: a size rule names the size");
  r.rule = preserveRule;
  check(is(plan(r), {450, 400, 750, 600}, false), "fallback: a preserve-size rule keeps the current size");
  r.honorPreserveSize = false;
  check(is(plan(r), {300, 300, 900, 700}, false), "fallback: an ignored preserve-size rule gives the default");
  r = request(FloatingSizeSource::Fallback);
  r.defaultSize.reset();
  check(!plan(r), "fallback: no default and no rule gives no placement");

  r = request(FloatingSizeSource::Remembered);
  r.remembered = SIZE{500, 300};
  r.rule = sizeRule;
  check(is(plan(r), {350, 350, 850, 650}, false), "remembered: the remembered size wins over rules");
  r.remembered.reset();
  check(is(plan(r), {200, 250, 1000, 750}, false), "remembered: without one, as fallback");

  r = request(FloatingSizeSource::Own);
  r.current = SIZE{1400, 300};
  check(is(plan(r), {-100, 350, 1300, 650}, true) && !appliesLimits(plan(r)),
        "own: the window's size, only moved, past the work area");
  r.rule = sizeRule;
  check(is(plan(r), {200, 250, 1000, 750}, false) && appliesLimits(plan(r)),
        "own: a size rule resizes a resizable window");
  r.canResize = false;
  check(is(plan(r), {-100, 350, 1300, 650}, true) && !appliesLimits(plan(r)),
        "own: a window that can't resize is only moved");
  r.current.reset();
  check(!plan(r), "own: an unknown frame gives no placement");

  r = request(FloatingSizeSource::Fallback);
  r.canResize = false;
  r.center = POINT{100, 100};
  check(is(plan(r), {0, 0, 300, 200}, false) && !appliesLimits(plan(r)),
        "a window that can't resize keeps its size, inside the work area, without limits");

  // Size limits: fitted around the planned centre, inside the work area.
  auto fit = [&](const SharedGeometry::FloatingRequest& req, SharedGeometry::FrameLimits limits) {
    return SharedGeometry::FitFloatingPlacementToLimits(*plan(req), limits);
  };
  r = request(FloatingSizeSource::Fallback);  // 600x400 centred: {300, 300, 900, 700}
  RECT fitted = fit(r, {1, 1, 0, 0});
  expected = {300, 300, 900, 700};
  check(EqualRect(&fitted, &expected), "limits the planned size satisfies change nothing");
  fitted = fit(r, {1, 1, 400, 200});
  expected = {400, 400, 800, 600};
  check(EqualRect(&fitted, &expected), "a maximum shrinks the frame around its centre");
  fitted = fit(r, {800, 600, 0, 0});
  expected = {200, 200, 1000, 800};
  check(EqualRect(&fitted, &expected), "a minimum grows the frame around its centre");
  r.center = POINT{1100, 900};
  fitted = fit(r, {800, 600, 0, 0});
  expected = {400, 400, 1200, 1000};
  check(EqualRect(&fitted, &expected), "a grown frame stays inside the work area");
  r.center.reset();
  fitted = fit(r, {1400, 600, 0, 0});
  expected = {0, 200, 1400, 800};
  check(EqualRect(&fitted, &expected), "a minimum wider than the work area starts at its left edge");
  r = request(FloatingSizeSource::Own);
  fitted = fit(r, {800, 600, 0, 0});
  expected = {450, 400, 750, 600};
  check(EqualRect(&fitted, &expected), "a window that is only moved keeps its size despite its limits");
  r = request(FloatingSizeSource::Fallback);
  r.canResize = false;
  fitted = fit(r, {800, 600, 0, 0});
  expected = {450, 400, 750, 600};
  check(EqualRect(&fitted, &expected), "a window that can't resize keeps its size despite its limits");
  std::printf("Passed %d floating placement checks.\n", checks);
}

// A tiled workspace of `layout` with one window per minimum size (physical
// pixels at `dpi`) and fake HWNDs 1..n. For model tests only.
static Workspace MakeTiledWorkspace(TileLayout layout, const std::vector<SIZE>& minimums, UINT dpi = 96) {
  Workspace workspace;
  workspace.SetLayout(layout);
  for (size_t index = 0; index < minimums.size(); ++index) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(index + 1);
    record.pid = 1;
    record.constraints = {minimums[index].cx, minimums[index].cy, dpi, true};
    workspace.AdmitTiled(record);
  }
  return workspace;
}

// The planned rectangles; empty when the layout can't be planned.
static std::vector<RECT> PlannedRects(const Workspace& workspace, const RECT& area, LONG gap) {
  std::vector<RECT> rects;
  if (Layout::BuildWorkspaceLayoutPlan(workspace, area, gap, &rects) != Layout::LayoutPlanStatus::Success) {
    rects.clear();
  }
  return rects;
}

static LONG Span(const RECT& rect, bool alongY) {
  return alongY ? Layout::RectHeight(rect) : Layout::RectWidth(rect);
}

// What each window-size command changes in each layout: only the preference
// that sizes the chosen window, by one step of what is shown. Properties every
// layout keeps are checked over random workspaces in RunWindowSizePropertyTests.
static void RunWindowSizeStepTests() {
  g_settings = SettingsState{};
  const RECT area{0, 0, 1000, 1000};
  constexpr LONG kStep = 5;  // 50 px of the 1000 px span.
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  auto span = [&](const Workspace& workspace, size_t index, bool alongY) {
    const std::vector<RECT> rects = PlannedRects(workspace, area, 0);
    check(!rects.empty(), "plan the layout");
    return Span(rects[index], alongY);
  };
  auto within = [](double actual, double expected, double tolerance) { return std::abs(actual - expected) <= tolerance; };
  const std::vector<SIZE> small(3, SIZE{80, 80});

  for (TileLayout layout : {TileLayout::Columns, TileLayout::Rows}) {
    const bool alongY = layout == TileLayout::Rows;
    Workspace grid = MakeTiledWorkspace(layout, small);
    const LONG initial = span(grid, 0, alongY);
    check(grid.StepTiledSize(area, 0, 0, true, kStep) && within(span(grid, 0, alongY), initial + 50, 1) &&
              grid.GridWeights()[1] == 1.0 && grid.GridWeights()[2] == 1.0,
          "grow adds one step by changing only the window's weight");
    check(grid.StepTiledSize(area, 0, 0, false, kStep) && within(span(grid, 0, alongY), initial, 1),
          "shrink undoes grow");
    check(grid.StepTiledSize(area, 0, 0, true, kStep) && grid.ResetTiledSize(area, 0, 0, 0.5) &&
              grid.GridWeights() == std::vector<double>({1, 1, 1}),
          "reset returns the weight to its peers' mean");
    check(!grid.ResetTiledSize(area, 0, 0, 0.5), "resetting a default size changes nothing");
  }

  // A window held at its minimum stops there, and a peer's step neither moves
  // it nor saves its temporary fitting as a preference.
  Workspace pinned = MakeTiledWorkspace(TileLayout::Columns, {{650, 80}, {80, 80}, {80, 80}});
  check(span(pinned, 0, false) == 650 && !pinned.StepTiledSize(area, 0, 0, false, kStep),
        "shrink stops at the native minimum");
  const LONG peer = span(pinned, 1, false);
  check(pinned.StepTiledSize(area, 0, 1, true, kStep) && within(span(pinned, 1, false), peer + 50, 1) &&
            span(pinned, 0, false) == 650 && pinned.GridWeights()[0] == 1.0,
        "a peer grows from its visible size without saving the pinned window's fitting");

  for (TileLayout layout : {TileLayout::MasterStack, TileLayout::MasterStackH}) {
    const bool stackAlongY = layout == TileLayout::MasterStack;
    Workspace stack = MakeTiledWorkspace(layout, small);
    check(stack.StepTiledSize(area, 0, 0, true, kStep) && within(stack.MasterRatio(), 0.55, 0.002) &&
              stack.StackWeights() == std::vector<double>({1, 1}),
          "growing the master moves only the master divider");
    const LONG stackSpan = span(stack, 1, stackAlongY);
    check(stack.StepTiledSize(area, 0, 1, true, kStep) && within(span(stack, 1, stackAlongY), stackSpan + 50, 1) &&
              stack.StackWeights()[1] == 1.0 && within(stack.MasterRatio(), 0.55, 0.002),
          "growing a stack window moves only its stack weight");
    check(stack.ResetTiledSize(area, 0, 0, 0.5) && stack.MasterRatio() == 0.5 &&
              stack.ResetTiledSize(area, 0, 1, 0.5) && stack.StackWeights() == std::vector<double>({1, 1}),
          "reset returns the master ratio, and a stack weight to its peers' mean");
    Workspace pair = MakeTiledWorkspace(layout, {{80, 80}, {80, 80}});
    check(pair.StepTiledSize(area, 0, 1, true, kStep) && within(pair.MasterRatio(), 0.45, 0.002) &&
              pair.ResetTiledSize(area, 0, 1, 0.5) && pair.MasterRatio() == 0.5,
          "a lone stack window grows and resets through the master divider");
  }

  Workspace dwindle = MakeTiledWorkspace(TileLayout::Dwindle, small);
  check(dwindle.StepTiledSize(area, 0, 0, true, kStep) && within(dwindle.DwindleRatios()[0], 0.55, 0.002) &&
            dwindle.DwindleRatios()[1] == 0.5,
        "a Dwindle window grows by its own split");
  check(dwindle.StepTiledSize(area, 0, 2, true, kStep) && within(dwindle.DwindleRatios()[1], 0.45, 0.002) &&
            within(span(dwindle, 2, true), 550, 1),
        "the last Dwindle window grows by the split before it");
  check(dwindle.ResetTiledSize(area, 0, 2, 0.5) && dwindle.DwindleRatios()[1] == 0.5 &&
            within(dwindle.DwindleRatios()[0], 0.55, 0.002),
        "reset evens only the window's own split");

  Workspace monocle = MakeTiledWorkspace(TileLayout::Monocle, {{80, 80}, {80, 80}});
  check(!monocle.StepTiledSize(area, 0, 0, true, kStep) && !monocle.ResetTiledSize(area, 0, 0, 0.5),
        "Monocle has no window sizes");
  std::printf("Passed %d window-size step checks.\n", checks);
}

// Properties every window-size step and Dwindle drag keeps, over seeded random
// workspaces (minimums that often force adaptive fitting) and random sequences
// of operations on them. An operation either changes nothing at all, or moves
// the window the way it was asked, still plans, and changes no other window
// across its axis or by more than the window itself changed. These properties
// found the bugs the scenario tests missed: steps that silently did nothing,
// and other windows jumping by hundreds of pixels.
static void RunWindowSizePropertyTests() {
  const RECT area{14, 14, 3058, 1804};
  const LONG gap = 14;
  constexpr int kWorkspacesPerLayout = 40;
  constexpr int kOperationsPerWorkspace = 10;
  std::mt19937 random(20261119);  // Fixed: a failure reproduces.
  auto between = [&](LONG low, LONG high) { return low + static_cast<LONG>(random() % (high - low + 1)); };
  int operations = 0;
  auto fail = [&](const char* message, TileLayout layout, int workspace, int operation) {
    std::fprintf(stderr, "FAILED: %s (layout=%d workspace=%d operation=%d)\n", message, static_cast<int>(layout),
                 workspace, operation);
    std::exit(1);
  };

  for (TileLayout layout : {TileLayout::Columns, TileLayout::Rows, TileLayout::MasterStack,
                            TileLayout::MasterStackH, TileLayout::Dwindle}) {
    for (int trial = 0; trial < kWorkspacesPerLayout; ++trial) {
      g_settings = SettingsState{};
      g_settings.adaptiveDwindleSlots = static_cast<size_t>(between(1, 2));
      g_settings.adaptiveDwindlePercentage = between(10, 40);
      g_settings.reverseDwindle = between(0, 1) == 1;
      std::vector<SIZE> minimums(static_cast<size_t>(between(3, 5)));
      for (SIZE& minimum : minimums) {
        const bool demanding = between(0, 1) == 1;
        minimum = demanding ? SIZE{between(400, 1200), between(300, 900)} : SIZE{between(100, 350), between(100, 350)};
      }
      Workspace workspace = MakeTiledWorkspace(layout, minimums, 192);
      if (PlannedRects(workspace, area, gap).empty()) continue;

      for (int operation = 0; operation < kOperationsPerWorkspace; ++operation, ++operations) {
        const std::vector<RECT> before = PlannedRects(workspace, area, gap);
        const size_t index = static_cast<size_t>(between(0, static_cast<LONG>(minimums.size()) - 1));
        Workspace next = workspace;
        bool changed = false;
        bool grow = false;
        std::optional<bool> dragAlongY;
        LONG dragDistance = 0;
        if (layout == TileLayout::Dwindle && between(0, 1) == 1) {
          const int edge = between(0, 3);  // left, top, right, bottom
          MoveSizeGesture drag;
          drag.intent = MoveSizeIntent::Resize;
          drag.hasRects = true;
          drag.start = drag.end = before[index];
          LONG* sides[] = {&drag.end.left, &drag.end.top, &drag.end.right, &drag.end.bottom};
          dragDistance = between(50, 400);
          *sides[edge] += between(0, 1) == 1 ? dragDistance : -dragDistance;
          dragAlongY = edge == 1 || edge == 3;
          changed = next.LearnDwindleResize(area, gap, index, drag);
        } else {
          grow = between(0, 1) == 1;
          changed = next.StepTiledSize(area, gap, index, grow, 5);
        }
        if (!changed) {
          if (!next.HasSameLayoutStateAs(workspace)) fail("an operation that changes nothing saved something", layout, trial, operation);
          continue;
        }

        const std::vector<RECT> after = PlannedRects(next, area, gap);
        if (after.empty()) fail("a changed layout no longer plans", layout, trial, operation);
        if (std::equal(before.begin(), before.end(), after.begin(), after.end(),
                       [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; })) {
          fail("an operation reported a change but nothing on screen moved", layout, trial, operation);
        }
        const LONG dw = Layout::RectWidth(after[index]) - Layout::RectWidth(before[index]);
        const LONG dh = Layout::RectHeight(after[index]) - Layout::RectHeight(before[index]);
        const bool alongY = dragAlongY.value_or(std::abs(dh) > std::abs(dw));
        const LONG own = alongY ? dh : dw;
        if (!dragAlongY && (grow ? own <= 0 : own >= 0)) fail("a step moved the window the wrong way", layout, trial, operation);
        // No other window changes more than a step changed the window itself,
        // or more than a drag moved its edge. (A dragged edge can be an earlier
        // split's divider, of which the dragged window gets only a share.)
        const LONG limit = dragAlongY ? dragDistance : std::abs(own);
        for (size_t other = 0; other < after.size(); ++other) {
          if (other == index) continue;
          const LONG across = Span(after[other], !alongY) - Span(before[other], !alongY);
          const LONG along = Span(after[other], alongY) - Span(before[other], alongY);
          if (std::abs(across) > 1 || std::abs(along) > limit + 2) {
            std::fprintf(stderr, "  %s window %zu (alongY=%d): it changed %ld; window %zu changed %ld along, %ld across\n",
                         dragAlongY ? "drag on" : (grow ? "grow" : "shrink"), index, alongY ? 1 : 0, own, other,
                         along, across);
            fail(std::abs(across) > 1 ? "another window changed across the resize axis"
                                      : "another window changed more than the resize",
                 layout, trial, operation);
          }
        }
        workspace = std::move(next);
      }
    }
  }
  g_settings = SettingsState{};
  std::printf("Passed window-size properties over %d random operations.\n", operations);
}

// Dwindle layouts from real reports, at 200% DPI. Resizing accepts the layout
// as shown, so only the divider acted on moves; a growing window can push later
// dividers on its axis, which spring back when it shrinks.
static void RunDwindleResizeScenarioTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  constexpr LONG kStep = 5;

  // VS Code | PowerShell over (Notepad | LocalSend). LocalSend's minimum height
  // (993) doesn't fit half the work area, so adaptive fitting squeezes the
  // layout. LocalSend is at its minimum width (786); Notepad (650) has room.
  {
    g_settings = SettingsState{};
    const RECT area{12, 12, 3060, 1812};
    const LONG gap = 12;
    const Workspace shown = MakeTiledWorkspace(TileLayout::Dwindle, {{824, 553}, {931, 193}, {650, 397}, {786, 993}}, 192);
    auto widths = [&](const Workspace& workspace) {
      std::vector<LONG> result;
      for (const RECT& rect : PlannedRects(workspace, area, gap)) result.push_back(Layout::RectWidth(rect));
      check(result.size() == 4, "plan the layout");
      return result;
    };
    const std::vector<LONG> before = widths(shown);
    const LONG notepadRoom = before[2] - 650;
    check(before[3] == 786 && notepadRoom > 0, "LocalSend is at its minimum width; Notepad has room");

    Workspace localSend = shown;
    check(localSend.StepTiledSize(area, gap, 3, true, kStep) && widths(localSend)[3] > before[3] &&
              widths(localSend)[0] == before[0],
          "growing LocalSend moves only the divider on its left");
    Workspace notepad = shown;
    check(!notepad.StepTiledSize(area, gap, 2, true, kStep) && notepad.HasSameLayoutStateAs(shown),
          "Notepad can't grow into LocalSend, and saves nothing");
    check(!notepad.StepTiledSize(area, gap, 3, false, kStep), "LocalSend can't shrink below its minimum");

    const std::vector<RECT> rects = PlannedRects(shown, area, gap);
    MoveSizeGesture drag;
    drag.intent = MoveSizeIntent::Resize;
    drag.hasRects = true;
    drag.start = drag.end = rects[3];
    drag.end.left -= 300;
    Workspace dragged = shown;
    check(dragged.LearnDwindleResize(area, gap, 3, drag) &&
              std::abs(widths(dragged)[3] - (before[3] + notepadRoom)) <= 1 && widths(dragged)[0] == before[0],
          "a drag stops where Notepad reaches its minimum; no other divider moves");

    // VS Code is squeezed below its saved half. A step keeps what it shows, so
    // that size remains once the squeeze ends.
    Workspace squeezed = shown;
    check(before[0] < (Layout::RectWidth(area) - gap) / 2 && squeezed.StepTiledSize(area, gap, 0, true, kStep),
          "grow the squeezed VS Code");
    const LONG grown = widths(squeezed)[0];
    for (HWND hwnd : squeezed.TiledWindows()) squeezed.UpdateConstraints(hwnd, {80, 80, 192, true});
    check(std::abs(widths(squeezed)[0] - grown) <= 1, "the size a step shows is kept after the squeeze ends");
  }

  // VS Code | Explorer over (Explorer | Notepad). Growing VS Code pushes the
  // Explorer|Notepad divider, which springs back as VS Code shrinks.
  {
    g_settings = SettingsState{};
    g_settings.adaptiveDwindleSlots = 2;
    g_settings.adaptiveDwindlePercentage = 15;
    const RECT area{14, 14, 3058, 1804};
    const LONG gap = 14;
    const LONG stepPx = MulDiv(Layout::RectWidth(area), kStep, 100);
    Workspace pushed = MakeTiledWorkspace(TileLayout::Dwindle, {{802, 542}, {300, 475}, {300, 475}, {644, 394}}, 192);
    auto widths = [&](const Workspace& workspace) {
      std::vector<LONG> result;
      for (const RECT& rect : PlannedRects(workspace, area, gap)) result.push_back(Layout::RectWidth(rect));
      check(result.size() == 4, "plan the layout");
      return result;
    };
    check(pushed.StepTiledSize(area, gap, 2, true, kStep) && !pushed.StepTiledSize(area, gap, 2, true, kStep),
          "grow the deep Explorer until Notepad is at its minimum");
    const double deepRatio = pushed.DwindleRatios()[2];
    int grows = 0;
    while (grows < 20 && pushed.StepTiledSize(area, gap, 0, true, kStep)) ++grows;
    const std::vector<LONG> full = widths(pushed);
    check(grows >= 3 && pushed.DwindleRatios()[2] < deepRatio && full[2] < 300 + stepPx && full[3] >= 644,
          "VS Code grows by pushing the deep divider until Explorer reaches its minimum");
    const Workspace fullyPushed = pushed;

    check(pushed.StepTiledSize(area, gap, 0, false, kStep) && full[0] - widths(pushed)[0] <= stepPx + 2 &&
              pushed.DwindleRatios()[2] > fullyPushed.DwindleRatios()[2],
          "one shrink costs VS Code one step and lets the pushed divider spring back");
    for (int i = 0; i < 20 && std::abs(pushed.DwindleRatios()[2] - deepRatio) > 1e-9; ++i) {
      pushed.StepTiledSize(area, gap, 0, false, kStep);
    }
    check(std::abs(pushed.DwindleRatios()[2] - deepRatio) <= 1e-9, "shrinking far enough returns the divider exactly");

    Workspace reset = fullyPushed;
    check(reset.ResetTiledSize(area, gap, 0, 0.5) && reset.DwindleRatios()[0] == 0.5 &&
              reset.DwindleRatios()[2] > fullyPushed.DwindleRatios()[2],
          "reset evens VS Code and lets the divider spring back as far as that leaves room");

    // Fully pushed, both windows beside the deep divider are at their minimums;
    // one shrink of VS Code frees room to step that divider directly.
    Workspace kept = fullyPushed;
    check(kept.StepTiledSize(area, gap, 0, false, kStep) && kept.StepTiledSize(area, gap, 3, true, kStep),
          "step the pushed divider directly");
    const double keptRatio = kept.DwindleRatios()[2];
    check(kept.StepTiledSize(area, gap, 0, false, kStep) && kept.DwindleRatios()[2] == keptRatio,
          "a divider the user stepped no longer springs back");
    Workspace swapped = fullyPushed;  // The Explorers have equal minimums, so this fits.
    check(swapped.SwapTiled(swapped.TiledWindows()[1], swapped.TiledWindows()[2]) &&
              swapped.StepTiledSize(area, gap, 0, false, kStep) &&
              swapped.DwindleRatios()[2] == fullyPushed.DwindleRatios()[2],
          "a push lapses once its window leaves the slot");
  }
  g_settings = SettingsState{};
  std::printf("Passed %d Dwindle resize scenario checks.\n", checks);
}

// Characterization test for the Dwindle planner. Generates many deterministic
// workspaces (window counts, areas, gaps, saved ratios, minimum sizes, reverse
// and adaptive settings) and folds every planner output into one digest.
// The digest is the behavior: a pure refactor must print the same value, and an
// intended behavior change must update kExpectedDigest deliberately.
static void RunDwindlePlannerDigest() {
  // Dwindle starts stacked on portrait work areas.
  constexpr uint64_t kExpectedDigest = 0x64e969f883e0f56full;
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
    Layout::LayoutDwindle(area, gap, 0, count, ratios, g_settings.reverseDwindle, Layout::FirstSplitAxis(area), frames);

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

// Characterizes the whole planner over random workspaces: every layout but
// Floating, random minimum and maximum sizes, gaps, work areas and adaptive
// Dwindle settings. The digest pins each plan's status and every rectangle,
// including what a failed plan leaves behind (conflict reporting and admission
// recovery read those), plus the grow, shrink and reset size steps.
static void RunPlanningTraceDigest() {
  // Dwindle starts stacked on portrait work areas.
  constexpr uint64_t kExpectedDigest = 0x866af9eb719e197bull;
  const SettingsState savedSettings = g_settings;
  uint64_t digest = 1469598103934665603ull;
  auto mix = [&](uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      digest ^= (value >> (i * 8)) & 0xff;
      digest *= 1099511628211ull;
    }
  };
  auto mixDouble = [&](double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    mix(bits);
  };
  std::mt19937 rng(424242);
  auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
  const TileLayout layouts[] = {TileLayout::MasterStack, TileLayout::Columns, TileLayout::Rows,
                                TileLayout::MasterStackH, TileLayout::Dwindle, TileLayout::Monocle};
  size_t plans = 0;
  size_t unsatisfiable = 0;
  size_t changedSteps = 0;
  for (int run = 0; run < 10000; ++run) {
    g_settings = SettingsState{};
    g_settings.reverseDwindle = pick(0, 1);
    g_settings.adaptiveDwindleSlots = static_cast<size_t>(pick(0, 4));
    g_settings.adaptiveDwindlePercentage = pick(0, 40);
    Workspace workspace;
    workspace.SetLayout(layouts[pick(0, 5)]);
    const int count = pick(1, 7);
    for (int i = 1; i <= count; ++i) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(i));
      record.pid = 1;
      record.constraints.valid = pick(0, 2) != 0;
      record.constraints.minWidth = pick(1, 1400);
      record.constraints.minHeight = pick(1, 900);
      record.constraints.maxWidth = pick(0, 3) == 0 ? pick(record.constraints.minWidth, 2000) : 0;
      record.constraints.maxHeight = pick(0, 3) == 0 ? pick(record.constraints.minHeight, 1400) : 0;
      workspace.AdmitTiled(record);
    }
    workspace.SetMasterRatio(pick(10, 90) / 100.0);
    RECT area{pick(-20, 20), pick(-20, 20), 0, 0};
    area.right = area.left + pick(300, 3000);
    area.bottom = area.top + pick(300, 1800);
    const LONG gap = pick(0, 40);
    for (int step = 0; step < 6; ++step) {
      std::vector<RECT> rects{RECT{1, 2, 3, 4}};
      const auto status = Layout::BuildWorkspaceLayoutPlan(workspace, area, gap, &rects);
      mix(static_cast<uint64_t>(status));
      mix(rects.size());
      for (const RECT& rect : rects) {
        mix(rect.left);
        mix(rect.top);
        mix(rect.right);
        mix(rect.bottom);
      }
      ++plans;
      if (status == Layout::LayoutPlanStatus::Unsatisfiable) ++unsatisfiable;
      const size_t index = static_cast<size_t>(pick(0, count - 1));
      const int operation = pick(0, 2);
      Workspace candidate = workspace;
      const bool changed = operation == 2 ? candidate.ResetTiledSize(area, gap, index, 0.5)
                                          : candidate.StepTiledSize(area, gap, index, operation == 0, pick(1, 30));
      mix(changed);
      if (changed) ++changedSteps;
      mix(candidate.StackWeights().size());
      for (double weight : candidate.StackWeights()) mixDouble(weight);
      for (double weight : candidate.GridWeights()) mixDouble(weight);
      for (double ratio : candidate.DwindleRatios()) mixDouble(ratio);
      mixDouble(candidate.MasterRatio());
      if (pick(0, 1)) workspace = candidate;
    }
  }
  g_settings = savedSettings;

  if (unsatisfiable == 0 || unsatisfiable == plans || changedSteps == 0) {
    std::fprintf(stderr, "FAILED: planning trace inputs do not exercise all planner outcomes\n");
    std::exit(1);
  }
  if (kExpectedDigest && digest != kExpectedDigest) {
    std::fprintf(stderr, "FAILED: planning trace digest %016llx != expected %016llx\n",
                 static_cast<unsigned long long>(digest), static_cast<unsigned long long>(kExpectedDigest));
    std::exit(1);
  }
  std::printf("Planning trace digest %016llx over %zu plans (%zu unsatisfiable, %zu size steps).\n",
              static_cast<unsigned long long>(digest), plans, unsatisfiable, changedSteps);
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
  // Recorded from the implementation as of 2026-11-22, when "bsp" stopped
  // naming Dwindle and became the BSP layout.
  if (digest != 0x96a3f72e962247d1ull) {
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

  // What didn't take effect is reported in one toast.
  const SettingsState savedSettings = g_settings;
  g_settings.problems.clear();
  Configuration::NoteSettingProblem(L"Window rule %d ignored: invalid size", 3);
  Configuration::NoteSettingProblem(L"%ls must be %d-%d; using %d", L"Window Gap", 0, 100, 6);
  check(g_settings.problems.size() == 2 && g_settings.problems[0] == L"Window rule 3 ignored: invalid size" &&
            g_settings.problems[1] == L"Window Gap must be 0-100; using 6",
        "each unusable setting is noted as one line");
  check(RuntimeLifecycle::StartupProblemsText({}, {}).empty(), "no toast when everything took effect");
  check(RuntimeLifecycle::StartupProblemsText({}, {g_settings.problems[0]}) == L"Window rule 3 ignored: invalid size",
        "one problem is shown as noted");
  check(RuntimeLifecycle::StartupProblemsText({L"Tile Workspace"}, g_settings.problems) ==
            L"Hotkey in use: Tile Workspace; Window rule 3 ignored: invalid size (+1 more in the log)",
        "hotkeys and settings share one toast, and further problems are counted");
  g_settings = savedSettings;

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
                workspace, metrics, false, position == AutomaticNewWindowPosition::AfterFocused);
            // Default size 960x640 at 96 DPI, centred: where overflow would centre it.
            const RECT expectedFloating{480, 200, 1440, 840};
            check(prediction.floatingFrame && EqualRect(&*prediction.floatingFrame, &expectedFloating),
                  "tiled layouts publish the centred default floating frame");
            check(prediction.tiledFrame.has_value() != prediction.newWindowFloats ||
                      (!prediction.tiledFrame && !prediction.newWindowFloats),
                  "a tiled slot and an overflow float never coexist");

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
            for (SuspensionReason reason : {SuspensionReason::None, SuspensionReason::Hidden,
                                             SuspensionReason::Minimized, SuspensionReason::Maximized}) {
              Workspace migrated = workspace;
              WindowRecord returning;
              returning.hwnd = incoming;
              returning.state = reason == SuspensionReason::None ? ManageState::Tiled : ManageState::Suspended;
              returning.suspensionReason = reason;
              returning.hasSavedSlot = reason != SuspensionReason::None;
              returning.savedSlot = {99, 0.8, true, true};  // Source slot must not leak into the destination.
              const HWND anchor = position == AutomaticNewWindowPosition::AfterFocused
                                      ? migrated.LastFocusedTiledWindow() : nullptr;
              migrated.AdmitMigrated(returning, anchor);
              if (reason != SuspensionReason::None) check(migrated.Restore(incoming), "restore a migrated suspended member");
              std::vector<RECT> migratedRects;
              const bool migrationPlanned = Layout::BuildWorkspaceLayoutPlan(
                  migrated, metrics.workArea, metrics.gap, &migratedRects) == Layout::LayoutPlanStatus::Success;
              check(migrationPlanned == planned && migrated.TiledIndex(incoming) == admitted.TiledIndex(incoming),
                    "every tiled arrival follows destination admission order");
              if (planned) {
                check(EqualRect(&migratedRects[migrated.TiledIndex(incoming)], &*prediction.tiledFrame),
                      "published admission tile also matches the real migration and restore");
              }
            }
            check(workspace.ActiveCount() == existing && !workspace.HasRecord(Placement::kPredictedWindow),
                  "prediction leaves the workspace unchanged");
          }
        }
      }
    }
  }

  g_settings = SettingsState{};
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;
  const HWND first = reinterpret_cast<HWND>(0x1500);
  const HWND previous = reinterpret_cast<HWND>(0x1504);
  const HWND latest = reinterpret_cast<HWND>(0x1508);
  const HWND last = reinterpret_cast<HWND>(0x150C);
  const HWND floatingWindow = reinterpret_cast<HWND>(0x1510);
  for (TileLayout layout : {TileLayout::Dwindle, TileLayout::Bsp}) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (HWND hwnd : {first, previous, latest, last}) {
      check(workspace.AdmitTiled({.hwnd = hwnd}), "admit a focus test window");
    }
    check(workspace.RememberFocusedWindow(previous) && workspace.RememberFocusedWindow(latest) &&
              !workspace.RememberFocusedWindow(incoming) && workspace.LastFocusedTiledWindow() == latest,
          "unmanaged foreground leaves the tiled anchor alone");
    check(workspace.Suspend(latest, SuspensionReason::Minimized) &&
              workspace.LastFocusedTiledWindow() == previous,
          "minimizing the last-focused tile falls back to the previous one");
    check(workspace.Restore(latest) && workspace.LastFocusedTiledWindow() == latest,
          "the restored window is eligible as a tiled anchor again");
    check(workspace.Forget(latest) && !workspace.LastFocusedWindow() &&
              workspace.LastFocusedTiledWindow() == previous,
          "closing the last-focused tile falls back to the previous one");
    check(workspace.AdmitFloating({.hwnd = floatingWindow}) &&
              workspace.RememberFocusedWindow(floatingWindow) && workspace.LastFocusedTiledWindow() == previous,
          "managed floating focus leaves the tiled anchor available");

    const auto prediction = Placement::PredictAutomaticAdmission(workspace, metrics, false, true);
    check(Reconcile::AdmitUntrackedSnapshot(workspace, {incoming}, true, true) &&
              workspace.TiledIndex(incoming) == workspace.TiledIndex(previous) + 1,
          "admission uses the surviving focused tile");
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(workspace, metrics.workArea, metrics.gap, &rects) ==
              Layout::LayoutPlanStatus::Success &&
              prediction.tiledFrame && EqualRect(&*prediction.tiledFrame, &rects[workspace.TiledIndex(incoming)]),
          "the companion's prediction uses the same fallback tile");
  }

  // The automatic window limit, counted exactly as EnforceAutomaticWindowLimit
  // counts: tiled members and maximized members keeping a slot count, minimized
  // ones don't. A full workspace gets no tiled hint.
  g_settings = SettingsState{};
  POINT origin{};
  const HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  DesktopMonitorKey limitKey{};
  check(DesktopMonitorKey::FromHMonitor(GUID{0x11A17001}, primary, &limitKey), "resolve limit test monitor");
  Workspace full;
  full.SetLayout(TileLayout::Columns);
  for (size_t i = 0; i < 4; ++i) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(0x2000 + i * 4);
    check(full.AdmitTiled(record), "admit window for limit test");
  }
  check(full.Suspend(reinterpret_cast<HWND>(0x2008), SuspensionReason::Maximized), "suspend a maximized member");
  check(full.Suspend(reinterpret_cast<HWND>(0x200C), SuspensionReason::Minimized), "suspend a minimized member");
  // Two tiled plus one maximized occupy slots; the minimized one doesn't.
  check(Placement::CollectAutomaticLimitOccupants(limitKey, full, primary).size() == 3,
        "occupants are the visible members that keep a slot");
  g_settings.automaticWindowLimit = 3;
  const auto limited = Placement::PredictAutomaticAdmission(full, metrics, true, false);
  check(Placement::WouldExceedAutomaticWindowLimit(limitKey, full, primary) && !limited.tiledFrame &&
            limited.newWindowFloats,
        "limit reached: no tiled slot, overflow floats the window");
  g_settings.automaticWindowLimit = 4;
  const auto belowLimit = Placement::PredictAutomaticAdmission(full, metrics, false, false);
  check(!Placement::WouldExceedAutomaticWindowLimit(limitKey, full, primary) && belowLimit.tiledFrame.has_value() &&
            !belowLimit.newWindowFloats,
        "below the limit: tiled slot");
  g_settings.automaticWindowLimit = 0;
  check(!Placement::WouldExceedAutomaticWindowLimit(limitKey, full, primary), "no limit configured: never reached");

  // The Floating layout centers the DPI-scaled default size, clamped to the work area.
  Workspace floating;
  floating.SetLayout(TileLayout::Floating);
  const auto centered = Placement::PredictAutomaticAdmission(floating, metrics, false, false);
  const RECT expectedCentered{480, 200, 1440, 840};
  check(!centered.tiledFrame && centered.floatingFrame && EqualRect(&*centered.floatingFrame, &expectedCentered),
        "floating default size is centered");
  const Platform::Monitors::WorkspaceMetrics highDpi{{0, 0, 1920, 1040}, 192, 12};
  const auto clamped = Placement::PredictAutomaticAdmission(floating, highDpi, false, false);
  check(clamped.floatingFrame && EqualRect(&*clamped.floatingFrame, &highDpi.workArea),
        "scaled default size is clamped to the work area");
  check(!centered.newWindowFloats, "the Floating layout never reports an overflow float");

  // No slot fits even a minimal new window: two settled columns whose minimums
  // fill the width. Recovery can't help (every slot is too narrow), so MultiWM
  // floats the newcomer, and the prediction says so.
  Workspace crowded;
  crowded.SetLayout(TileLayout::Columns);
  for (size_t i = 0; i < 2; ++i) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(0x3000 + i * 4);
    // 2 x 955 + one 6 px gap = 1916 fits; adding a 1 px newcomer and a second
    // gap makes 1923 > 1920.
    record.constraints = {955, 300, 96, true};
    check(crowded.AdmitTiled(record), "admit crowded column");
    crowded.MarkSettled(record.hwnd);
  }
  const auto overfull = Placement::PredictAutomaticAdmission(crowded, metrics, false, false);
  check(!overfull.tiledFrame && overfull.newWindowFloats, "no slot fits: overflow floats the window");
  // The same columns with room to spare tile it.
  Workspace roomy = crowded;
  roomy.UpdateConstraints(reinterpret_cast<HWND>(0x3000), {300, 300, 96, true});
  const auto fits = Placement::PredictAutomaticAdmission(roomy, metrics, false, false);
  check(fits.tiledFrame && !fits.newWindowFloats, "room left: tiled slot");
  // A conflict among the windows already here is not the newcomer's fault: they
  // are reordered or one of them is floated, so no float is predicted.
  Workspace conflicted = crowded;
  conflicted.UpdateConstraints(reinterpret_cast<HWND>(0x3000), {1500, 300, 96, true});
  const auto blamedElsewhere = Placement::PredictAutomaticAdmission(conflicted, metrics, false, false);
  check(!blamedElsewhere.tiledFrame && !blamedElsewhere.newWindowFloats,
        "an existing conflict predicts neither a slot nor a float");

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

static void RunRestorePublicationTests() {
  auto body = std::make_unique<CompanionAbi::SnapshotBody>();
  const GUID desktop{0xAABBC001};
  POINT origin{};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  DesktopMonitorKey key{};
  if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &key)) std::exit(1);
  g_settings = SettingsState{};
  g_settings.automaticWindowLimit = 0;
  Workspace workspace;
  workspace.SetLayout(TileLayout::Columns);
  const HWND windows[] = {reinterpret_cast<HWND>(0x4000), reinterpret_cast<HWND>(0x4004),
                           reinterpret_cast<HWND>(0x4008), reinterpret_cast<HWND>(0x400C)};
  for (HWND hwnd : windows) {
    WindowRecord record;
    record.hwnd = hwnd;
    record.pid = 7;
    workspace.AdmitTiled(record);
  }
  workspace.Suspend(windows[0], SuspensionReason::Minimized);
  workspace.Suspend(windows[3], SuspensionReason::Maximized);
  g_workspaces.Save(key, workspace);
  // Borders remembered from when the windows were restored. The second belongs to
  // another process, as if its HWND value had been reused, so it must not be used.
  const RECT measuredMargins{11, 0, 11, 11};
  CompanionSnapshot::g_publisher.frameMargins[windows[0]] = {7, measuredMargins, 192};
  CompanionSnapshot::g_publisher.frameMargins[windows[3]] = {8, measuredMargins, 192};
  const auto probes = Diagnostics::g_runtime.counters.constraintProbeAttempts;
  CompanionSnapshot::AppendWorkspaceHints(desktop, body.get());
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  Platform::Monitors::WorkspaceMetrics metrics;
  check(Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics), "read the test monitor's metrics");
  check(body->workspaceCount == 1 && body->restoreWindowCount == 2, "publish only the suspended members");
  check(body->restoreWindows[0].hwnd == 0x4000 && body->restoreWindows[1].hwnd == 0x400C,
        "restore hints are published in a stable order");
  check(body->restoreWindows[0].processId == 7 && body->restoreWindows[0].workspaceIndex == 0,
        "restore hints retain identity and workspace");
  // Restoring either window gives three columns. The first window returns to the
  // first column (at the work area's left edge), the last to the last column.
  const auto& first = body->restoreWindows[0];
  const auto& last = body->restoreWindows[1];
  check(first.hasFrame && first.frame.left == metrics.workArea.left && first.frame.top == metrics.workArea.top &&
            first.frame.bottom == metrics.workArea.bottom,
        "a restored first window gets the first column");
  check(last.hasFrame && last.frame.right == metrics.workArea.right && last.frame.left > first.frame.right,
        "a restored last window gets the last column");
  // The frames are exactly what MultiWM's own restore produces.
  for (const auto& hint : {first, last}) {
    Workspace restored = workspace;
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hint.hwnd));
    std::vector<RECT> rects;
    check(restored.Restore(hwnd) &&
              Layout::BuildWorkspaceLayoutPlan(restored, metrics.workArea, metrics.gap, &rects) ==
                  Layout::LayoutPlanStatus::Success &&
              EqualRect(&rects[restored.TiledIndex(hwnd)], &hint.frame),
          "the restore hint equals the tile MultiWM's restore produces");
  }
  check(probes == Diagnostics::g_runtime.counters.constraintProbeAttempts,
        "restore publication adds no native constraint probes");
  const auto firstTracked = CompanionSnapshot::MakeTrackedWindowHint(*workspace.Find(windows[0]), desktop, metrics.workArea);
  const auto lastTracked = CompanionSnapshot::MakeTrackedWindowHint(*workspace.Find(windows[3]), desktop, metrics.workArea);
  check(firstTracked.marginsDpi == 192 && EqualRect(&firstTracked.frameMargins, &measuredMargins),
        "restore hints carry the borders measured while the window was restored");
  check(lastTracked.marginsDpi == 0,
        "borders measured for another process are not published");

  // Borders measured at another window DPI (or with other frame styles) no
  // longer describe the window. A suspended window can't be measured again, so
  // nothing is published for it. These fake HWNDs read back DPI 0 and styles 0.
  auto stale = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::MeasuredFrameMargins otherDpi{7, measuredMargins, 192};
  otherDpi.windowDpi = 96;
  CompanionSnapshot::g_publisher.frameMargins[windows[0]] = otherDpi;
  CompanionSnapshot::AppendWorkspaceHints(desktop, stale.get());
  check(stale->restoreWindowCount == 2 &&
            CompanionSnapshot::MakeTrackedWindowHint(*workspace.Find(windows[0]), desktop, metrics.workArea).marginsDpi == 0,
        "borders from another window DPI are not published");
  CompanionSnapshot::g_publisher.frameMargins.clear();

  // A minimized master returns as the master, which the old next-window plan
  // (a newcomer appended to the stack) could never express.
  Workspace masterStack;
  masterStack.SetLayout(TileLayout::MasterStack);
  for (HWND hwnd : windows) {
    WindowRecord record;
    record.hwnd = hwnd;
    record.pid = 7;
    masterStack.AdmitTiled(record);
  }
  masterStack.Suspend(windows[0], SuspensionReason::Minimized);
  g_workspaces.Save(key, masterStack);
  auto master = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::AppendWorkspaceHints(desktop, master.get());
  check(master->restoreWindowCount == 1 && master->restoreWindows[0].hasFrame &&
            master->restoreWindows[0].frame.left == metrics.workArea.left &&
            master->restoreWindows[0].frame.top == metrics.workArea.top &&
            master->restoreWindows[0].frame.bottom == metrics.workArea.bottom &&
            master->restoreWindows[0].frame.right < metrics.workArea.right,
        "a restored master gets the full-height master column");

  // A window whose minimum can't fit once restored has no exact tile: MultiWM
  // would move or float it, so no frame is published.
  WindowConstraints huge{100000, 100000, 96, true};
  masterStack.UpdateConstraints(windows[0], huge);
  g_workspaces.Save(key, masterStack);
  auto unsatisfiable = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::AppendWorkspaceHints(desktop, unsatisfiable.get());
  check(unsatisfiable->restoreWindowCount == 1 && !unsatisfiable->restoreWindows[0].hasFrame,
        "an unsatisfiable restore publishes no frame");

  Workspace bsp;
  bsp.SetLayout(TileLayout::Bsp);
  for (size_t i = 0; i < 3; ++i) {
    WindowRecord record;
    record.hwnd = windows[i];
    record.pid = 7;
    bsp.AdmitTiled(record);
  }
  check(bsp.Suspend(windows[2], SuspensionReason::Minimized) && bsp.RememberFocusedWindow(windows[1]),
        "prepare a minimized BSP member and a focused tile");
  g_workspaces.Save(key, bsp);
  for (const bool splitFocused : {false, true}) {
    g_settings.automaticNewWindowPosition = splitFocused ? AutomaticNewWindowPosition::AfterFocused
                                                         : AutomaticNewWindowPosition::LastSlot;
    auto bspHints = std::make_unique<CompanionAbi::SnapshotBody>();
    CompanionSnapshot::AppendWorkspaceHints(desktop, bspHints.get());
    const auto& arrival = bspHints->workspaces[0];
    const auto& restore = bspHints->restoreWindows[0];
    check(bspHints->workspaceCount == 1 && bspHints->bspWorkspaces == 1 && !arrival.newWindowFloats &&
              arrival.hasTiledFrame && bspHints->restoreWindowCount == 1 && restore.hasFrame,
          "publish BSP arrival and restore frames");
    Workspace expected = bsp;
    WindowRecord newcomer;
    newcomer.hwnd = windows[3];
    newcomer.pid = 7;
    std::vector<RECT> frames;
    check(expected.AdmitTiledAfter(newcomer, windows[splitFocused ? 1 : 0]) &&
              Layout::BuildWorkspaceLayoutPlan(expected, metrics.workArea, metrics.gap, &frames) ==
                  Layout::LayoutPlanStatus::Success &&
              EqualRect(&arrival.tiledFrame, &frames[expected.TiledIndex(newcomer.hwnd)]),
          "the BSP arrival hint follows the selected split");
    check(EqualRect(&restore.frame, &arrival.tiledFrame), "the minimized BSP member uses the same split");
  }

  g_settings.adaptiveDwindleSlots = 0;
  bsp.Forget(windows[1]);
  bsp.UpdateConstraints(windows[0], {Layout::RectWidth(metrics.workArea), Layout::RectHeight(metrics.workArea),
                                    metrics.dpi, true});
  g_workspaces.Save(key, bsp);
  auto bspFloat = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::AppendWorkspaceHints(desktop, bspFloat.get());
  check(bspFloat->bspWorkspaces == 1 && !bspFloat->workspaces[0].hasTiledFrame &&
            bspFloat->workspaces[0].newWindowFloats,
        "a BSP split that cannot fit even a minimal window publishes an overflow");
  g_settings = SettingsState{};
  std::printf("Passed %d restore publication checks.\n", checks);
}

static void RunTrackedWindowPublicationTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const GUID desktop{0xAABBC002};
  DesktopMonitorKey source{};
  check(DesktopMonitorKey::FromHMonitor(desktop, MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &source),
        "resolve tracked-window publication monitor");
  DesktopMonitorKey target = source;
  ++target.desktopId.Data1;
  Workspace workspace;
  WindowRecord tiled;
  tiled.hwnd = reinterpret_cast<HWND>(0x70001000);
  tiled.pid = 321;
  workspace.AdmitTiled(tiled);
  workspace.Suspend(tiled.hwnd, SuspensionReason::Hidden);
  WindowRecord floating;
  floating.hwnd = reinterpret_cast<HWND>(0x70001004);
  floating.pid = 654;
  floating.floatingRect = {100, 100, 700, 500};
  floating.hasFloatingRect = true;
  workspace.AdmitFloating(floating);
  g_workspaces.Save(source, workspace);
  const auto plans = Diagnostics::g_runtime.counters.layoutPlansBuilt;
  auto body = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionSnapshot::AppendTrackedWindows(body.get());
  auto find = [&](HWND hwnd) -> const CompanionAbi::TrackedWindowHint* {
    const uint32_t value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    for (uint32_t i = 0; i < body->trackedCount; ++i) if (body->trackedWindows[i].hwnd == value) return &body->trackedWindows[i];
    return nullptr;
  };
  const auto* hidden = find(tiled.hwnd);
  check(hidden && hidden->processId == tiled.pid && IsEqualGUID(hidden->desktopId, desktop) &&
            hidden->monitorRect.right > hidden->monitorRect.left && hidden->treatment == CompanionAbi::WindowTreatment::Tiled,
        "inactive hidden owner publishes its identity, workspace and tiled treatment");
  const auto* floated = find(floating.hwnd);
  check(floated && floated->treatment == CompanionAbi::WindowTreatment::Floating,
        "explicit floating treatment is distinguishable from tiled arrivals");
  check(Diagnostics::g_runtime.counters.layoutPlansBuilt == plans && (body->flags & CompanionAbi::kFlagTrackedComplete),
        "ownership publication is complete without additional layout plans");
  Workspace migrated;
  migrated.AdmitMigrated(*workspace.Find(floating.hwnd));
  check(migrated.Find(floating.hwnd)->state == ManageState::Floating && migrated.ActiveCount() == 0 &&
            migrated.Find(floating.hwnd)->hasFloatingRect &&
            EqualRect(&migrated.Find(floating.hwnd)->floatingRect, &floating.floatingRect),
        "migration retains explicit floating treatment and remembered geometry");
  Workspace duplicate;
  duplicate.AdmitTiled(tiled);
  g_workspaces.Save(target, duplicate);
  *body = {};
  CompanionSnapshot::AppendTrackedWindows(body.get());
  hidden = find(tiled.hwnd);
  check(hidden && hidden->treatment == CompanionAbi::WindowTreatment::Unknown && hidden->processId == 0,
        "duplicate ownership stays tracked without granting either placement policy");
  g_workspaces.Save(source, Workspace{});
  g_workspaces.Save(target, Workspace{});

  Workspace crowded;
  for (uint32_t i = 0; i <= CompanionAbi::kMaxTrackedWindows; ++i) {
    WindowRecord record;
    record.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x71000000 + i * 4));
    crowded.AdmitTiled(record);
  }
  g_workspaces.Save(source, crowded);
  *body = {};
  CompanionSnapshot::AppendTrackedWindows(body.get());
  check(body->trackedCount == CompanionAbi::kMaxTrackedWindows && !(body->flags & CompanionAbi::kFlagTrackedComplete),
        "truncated ownership metadata disables placement hints");
  g_workspaces.Save(source, Workspace{});
  std::printf("Passed %d tracked-window publication checks.\n", checks);
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

// A window capped below its cell is shrunk to its maximum and centred in that
// cell; every other window keeps the cell it would have had anyway. Covers each
// layout family, the cross axis, the constrained (minimum) path and the case
// where the maximum doesn't bind.
static void RunMaximumSizeTests() {
  g_settings = SettingsState{};
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const RECT area{0, 0, 1200, 1000};
  auto plan = [&](TileLayout layout, const std::vector<WindowConstraints>& constraints) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (size_t i = 0; i < constraints.size(); ++i) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(0x5000 + i * 4);
      record.pid = 1;
      record.constraints = constraints[i];
      check(workspace.AdmitTiled(record), "admit window for maximum test");
    }
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(workspace, area, 0, &rects) == Layout::LayoutPlanStatus::Success,
          "a capped window never makes the plan unsatisfiable");
    return rects;
  };
  auto capped = [](LONG maxWidth, LONG maxHeight) {
    WindowConstraints constraints{1, 1, 96, true};
    constraints.maxWidth = maxWidth;
    constraints.maxHeight = maxHeight;
    return constraints;
  };
  auto same = [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; };
  const WindowConstraints free{1, 1, 96, true};

  auto columns = plan(TileLayout::Columns, {free, capped(200, 0), free});
  check(same(columns[0], {0, 0, 400, 1000}) && same(columns[2], {800, 0, 1200, 1000}),
        "neighbours keep their cells");
  check(same(columns[1], {500, 0, 700, 1000}), "capped width is centred in its column");

  auto crossAxis = plan(TileLayout::Columns, {capped(0, 400), free});
  check(same(crossAxis[0], {0, 300, 600, 700}) && same(crossAxis[1], {600, 0, 1200, 1000}),
        "capped height is centred on the cross axis");

  auto masterStack = plan(TileLayout::MasterStack, {capped(300, 0), free, free});
  check(same(masterStack[0], {150, 0, 450, 1000}) && same(masterStack[1], {600, 0, 1200, 500}),
        "a capped master is centred while the stack keeps its column");

  auto monocle = plan(TileLayout::Monocle, {capped(600, 400)});
  check(same(monocle[0], {300, 300, 900, 700}), "Monocle centres a capped window in the work area");

  auto dwindle = plan(TileLayout::Dwindle, {free, capped(200, 300)});
  check(same(dwindle[0], {0, 0, 600, 1000}) && same(dwindle[1], {800, 350, 1000, 650}),
        "Dwindle centres a capped window in its cell");

  // The constrained step runs first, then centring: a large minimum widens one
  // column and the capped window is centred in what remains for it.
  WindowConstraints wide{900, 1, 96, true};
  auto constrained = plan(TileLayout::Columns, {wide, capped(100, 0)});
  check(same(constrained[0], {0, 0, 900, 1000}) && same(constrained[1], {1000, 0, 1100, 1000}),
        "centring follows the constrained allocation");

  auto loose = plan(TileLayout::Columns, {capped(800, 1200), free});
  check(same(loose[0], {0, 0, 600, 1000}), "a maximum at least as large as the cell changes nothing");

  std::printf("Passed %d maximum size checks.\n", checks);
}

// BSP keeps its split tree as windows arrive, leave, or are resized. A
// maximized window keeps its leaf. When a window gives up its leaf, its sibling
// takes its space and any splits in that subtree turn as their depth changes.
static void RunBspLayoutTests() {
  g_settings = SettingsState{};
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const RECT area{0, 0, 1600, 900};
  auto window = [](int id) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(0x7000 + id * 4)); };
  const HWND a = window(1), b = window(2), c = window(3), d = window(4), e = window(5), f = window(6);
  auto admit = [&](Workspace& workspace, HWND hwnd, WindowConstraints constraints = {}) {
    WindowRecord record;
    record.hwnd = hwnd;
    record.pid = 1;
    record.constraints = constraints;
    check(workspace.AdmitTiled(record), "admit BSP window");
  };
  auto valid = [](const Workspace& workspace) {
    std::vector<std::wstring> errors;
    return workspace.Validate(DesktopMonitorKey{}, L"bsp test", &errors, false);
  };
  // The planned frame of each listed window, in the order given.
  auto framesIn = [&](const Workspace& workspace, const RECT& workArea, std::initializer_list<HWND> windows) {
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(workspace, workArea, 0, &rects) == Layout::LayoutPlanStatus::Success,
          "BSP plan succeeds");
    std::vector<RECT> result;
    for (HWND hwnd : windows) result.push_back(rects[workspace.TiledIndex(hwnd)]);
    return result;
  };
  auto frames = [&](const Workspace& workspace, std::initializer_list<HWND> windows) {
    return framesIn(workspace, area, windows);
  };
  auto same = [](const std::vector<RECT>& actual, const std::vector<RECT>& expected) {
    return std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(),
                      [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; });
  };

  Workspace balanced;
  balanced.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b, c, d}) admit(balanced, hwnd);
  check(valid(balanced), "BSP workspace is valid");
  check(same(frames(balanced, {a, b, c, d}),
             {{0, 0, 800, 450}, {800, 0, 1600, 450}, {0, 450, 800, 900}, {800, 450, 1600, 900}}),
        "each window splits the largest, and splits alternate side by side, stacked");
  check(balanced.TiledWindows() == std::vector<HWND>({a, c, b, d}), "the tiled order follows the leaves");
  admit(balanced, e);
  check(same(frames(balanced, {a, e}), {{0, 0, 400, 450}, {400, 0, 800, 450}}),
        "the fifth window splits the first of the equal quarters, side by side again");

  Workspace tall;
  tall.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b, c}) admit(tall, hwnd);
  const RECT portrait{0, 0, 900, 1600};
  check(same(framesIn(tall, portrait, {a, c, b}), {{0, 0, 450, 800}, {450, 0, 900, 800}, {0, 800, 900, 1600}}),
        "a portrait work area starts stacked, then alternates");
  Workspace tallDwindle;
  tallDwindle.SetLayout(TileLayout::Dwindle);
  for (HWND hwnd : {a, b}) admit(tallDwindle, hwnd);
  check(same(framesIn(tallDwindle, portrait, {a, b}), {{0, 0, 900, 800}, {0, 800, 900, 1600}}),
        "Dwindle also starts stacked on a portrait work area");

  Workspace closed = balanced;
  check(closed.Forget(a), "close a window");
  check(valid(closed), "closing keeps the tree valid");
  check(same(frames(closed, {e, b, c, d}),
             {{0, 0, 800, 450}, {800, 0, 1600, 450}, {0, 450, 800, 900}, {800, 450, 1600, 900}}),
        "its sibling takes its space and nobody else moves");

  Workspace minimized = balanced;
  check(minimized.Suspend(c, SuspensionReason::Minimized), "minimize a window");
  check(valid(minimized) && !minimized.Bsp().Contains(c), "a minimized window gives up its leaf");
  check(same(frames(minimized, {a, e, b, d}),
             {{0, 0, 800, 450}, {0, 450, 800, 900}, {800, 0, 1600, 450}, {800, 450, 1600, 900}}),
        "its sibling takes its space, its split turning as it moves up a level");
  check(minimized.Restore(c), "restore the minimized window");
  check(valid(minimized) && same(frames(minimized, {a, c}), {{0, 0, 400, 450}, {400, 0, 800, 450}}),
        "a restored window arrives like a new one, splitting the largest");
  Workspace followed = balanced;
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;
  check(followed.RememberFocusedWindow(d) && followed.Suspend(c, SuspensionReason::Minimized), "minimize after focusing");
  check(followed.RememberFocusedWindow(c), "the restoring window takes focus");
  check(followed.Restore(c) && same(frames(followed, {d, c}), {{800, 450, 1200, 900}, {1200, 450, 1600, 900}}),
        "with Focused tile, it splits the last tiled window that had focus");
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;

  Workspace maximized = balanced;
  check(maximized.Suspend(c, SuspensionReason::Maximized), "maximize a window");
  check(valid(maximized) && maximized.Bsp().Contains(c), "a maximized window keeps its leaf");
  check(maximized.Restore(c) && same(frames(maximized, {a, e, b, c, d}), frames(balanced, {a, e, b, c, d})),
        "un-maximizing puts it back where it was");
  check(maximized.Suspend(c, SuspensionReason::Maximized) && maximized.Suspend(c, SuspensionReason::Minimized) &&
            valid(maximized) && !maximized.Bsp().Contains(c),
        "minimizing a maximized window gives up its leaf");

  Workspace focused = balanced;
  WindowRecord added;
  added.hwnd = f;
  added.pid = 1;
  check(focused.AdmitTiledAfter(added, b), "admit after the focused window");
  check(same(frames(focused, {b, f, d}), {{800, 0, 1200, 450}, {1200, 0, 1600, 450}, {800, 450, 1600, 900}}),
        "a window inserted after the focused one splits only that window");

  Workspace swapped = balanced;
  check(swapped.SwapTiled(a, d), "swap two windows");
  check(same(frames(swapped, {d, a}), {{0, 0, 400, 450}, {800, 450, 1600, 900}}), "swapped windows trade leaves");
  check(valid(swapped), "swapping keeps the tree valid");

  Workspace floated = balanced;
  check(floated.Float(b), "float a window");
  check(same(frames(floated, {d}), {{800, 0, 1600, 900}}), "a floated window's sibling takes its space");
  check(floated.ActivateTiled(b), "tile it again");
  check(valid(floated), "re-tiling keeps the tree valid");

  // Dragging a divider changes its position and preserves split directions.
  Workspace dragged;
  dragged.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b, c}) admit(dragged, hwnd);
  MoveSizeGesture gesture;
  gesture.hasRects = true;
  gesture.intent = MoveSizeIntent::Resize;
  gesture.start = gesture.end = frames(dragged, {a})[0];
  gesture.end.right += 500;
  check(dragged.LearnBspResize(area, 0, dragged.TiledIndex(a), gesture), "learn a divider drag");
  check(same(frames(dragged, {a, c, b}), {{0, 0, 1300, 450}, {0, 450, 1300, 900}, {1300, 0, 1600, 900}}),
        "the dragged divider moves and the region it widened stays stacked");
  gesture.start = gesture.end = frames(dragged, {c})[0];
  check(!dragged.LearnBspResize(area, 0, dragged.TiledIndex(c), gesture), "an unchanged gesture saves nothing");
  gesture.end.top -= 150;
  check(dragged.LearnBspResize(area, 0, dragged.TiledIndex(c), gesture), "drag a nested divider by its far side");
  check(same(frames(dragged, {a, c}), {{0, 0, 1300, 300}, {0, 300, 1300, 900}}), "the nested divider follows");
  Workspace cycled = dragged;
  cycled.SetLayout(TileLayout::Monocle);
  cycled.SetLayout(TileLayout::Bsp);
  check(same(frames(cycled, {a, b, c}), frames(dragged, {a, b, c})), "switching away and back keeps the tree");

  // Adaptive splits (Adaptive Split Depth) move only as far as minimums need,
  // within the adaptive range; the saved ratio stays.
  Workspace constrained;
  constrained.SetLayout(TileLayout::Bsp);
  admit(constrained, a, {1000, 1, 96, true});
  admit(constrained, b);
  check(same(frames(constrained, {a, b}), {{0, 0, 1000, 900}, {1000, 0, 1600, 900}}),
        "a minimum moves an adaptive divider just far enough");
  check(constrained.Bsp().Nodes()[0].ratio == 0.5, "fitting keeps the saved ratio");
  admit(constrained, c, {1, 600, 96, true});
  check(same(frames(constrained, {a, c}), {{0, 0, 1000, 300}, {0, 300, 1000, 900}}),
        "a split one level down adapts too");
  std::vector<RECT> rects;
  g_settings.adaptiveDwindleSlots = 1;
  check(Layout::BuildWorkspaceLayoutPlan(constrained, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable,
        "a split below the adaptive depth stays where it is");
  g_settings.adaptiveDwindleSlots = 2;
  g_settings.adaptiveDwindlePercentage = 10;
  check(Layout::BuildWorkspaceLayoutPlan(constrained, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable,
        "an adaptive split stays inside the adaptive range");
  g_settings.adaptiveDwindlePercentage = 40;
  Workspace impossible = constrained;
  admit(impossible, d, {1700, 1, 96, true});
  check(Layout::BuildWorkspaceLayoutPlan(impossible, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable,
        "a window wider than the work area makes the plan unsatisfiable");

  g_settings.adaptiveDwindleSlots = 3;
  g_settings.adaptiveDwindlePercentage = 15;
  for (const bool splitFocused : {false, true}) {
    Workspace arriving;
    arriving.SetLayout(TileLayout::Bsp);
    for (HWND hwnd : {a, b, c}) {
      admit(arriving, hwnd);
      arriving.MarkSettled(hwnd);
    }
    if (splitFocused) {
      admit(arriving, d);
      arriving.MarkSettled(d);
      check(arriving.RememberFocusedWindow(a), "select the smaller focused tile");
    }
    g_settings.automaticNewWindowPosition = splitFocused ? AutomaticNewWindowPosition::AfterFocused
                                                         : AutomaticNewWindowPosition::LastSlot;
    const WindowConstraints minimum = splitFocused ? WindowConstraints{700, 250, 96, true}
                                                   : WindowConstraints{460, 600, 96, true};
    Workspace returning = arriving;
    WindowRecord minimizedRecord;
    minimizedRecord.hwnd = f;
    minimizedRecord.pid = 1;
    minimizedRecord.constraints = minimum;
    check(returning.AdmitInitial(minimizedRecord, SuspensionReason::Minimized), "reserve a minimized BSP arrival");
    const Platform::Monitors::WorkspaceMetrics fallbackMetrics{area, 96, 0};
    const auto restoreHint = Placement::PredictRestoredFrame(returning, f, fallbackMetrics);
    admit(arriving, f, minimum);
    const auto& tree = arriving.Bsp();
    const size_t split = tree.Parent(tree.FindLeaf(f));
    const HWND target = splitFocused ? a : b;
    check(split == tree.Parent(tree.FindLeaf(target)) && tree.Nodes()[split].ratio == 0.5,
          "an arrival starts at 50/50 in the selected tile");
    check(Layout::BuildWorkspaceLayoutPlan(arriving, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable,
          "the usual direction cannot fit within the adaptive range");
    check(Layout::PlanOrTurnNewBspSplit(arriving, area, 0, &rects) == Layout::LayoutPlanStatus::Success,
          "the alternate direction fits with adaptive movement from 50/50");
    const RECT fitted = splitFocused ? RECT{0, 200, 800, 450} : RECT{1140, 0, 1600, 900};
    check(EqualRect(&rects[arriving.TiledIndex(f)], &fitted) &&
              tree.Nodes()[split].direction == (splitFocused ? Layout::Axis::Y : Layout::Axis::X) &&
              tree.Nodes()[split].ratio == 0.5,
          "the new direction is saved while the fitted position stays temporary");
    check(restoreHint && EqualRect(&*restoreHint, &fitted), "the companion predicts the same alternate split on restore");
    if (!splitFocused) {
      Workspace provisional = arriving;
      provisional.UpdateConstraints(f, {720, 350, 96, true});
      check(Layout::BuildWorkspaceLayoutPlan(provisional, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable &&
                Layout::PlanOrTurnNewBspSplit(provisional, area, 0, &rects) == Layout::LayoutPlanStatus::Success &&
                !provisional.Bsp().Nodes()[split].direction.has_value(),
            "an unsettled admission can return to the original direction when its minimum changes");
    }
    arriving.MarkSettled(f);
    arriving.UpdateConstraints(f, {1, 1, 96, true});
    const RECT relaxed = splitFocused ? RECT{0, 225, 800, 450} : RECT{1200, 0, 1600, 900};
    check(Layout::BuildWorkspaceLayoutPlan(arriving, area, 0, &rects) == Layout::LayoutPlanStatus::Success &&
              EqualRect(&rects[arriving.TiledIndex(f)], &relaxed),
          "relaxing the constraints returns the divider to 50/50 in its saved direction");
  }
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::LastSlot;
  Workspace settled;
  settled.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b}) {
    admit(settled, hwnd);
    settled.MarkSettled(hwnd);
    settled.UpdateConstraints(hwnd, {900, 1, 96, true});
  }
  check(Layout::PlanOrTurnNewBspSplit(settled, area, 0, &rects) == Layout::LayoutPlanStatus::Unsatisfiable &&
            !settled.Bsp().Nodes()[0].direction,
        "raising minimums cannot turn a settled divider");
  g_settings.adaptiveDwindleSlots = 2;
  g_settings.adaptiveDwindlePercentage = 40;

  // Size steps move the nearest split that divides the window from a neighbour.
  Workspace stepped;
  stepped.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b}) admit(stepped, hwnd);
  check(stepped.StepTiledSize(area, 0, stepped.TiledIndex(b), true, 5), "grow the second window");
  check(same(frames(stepped, {a, b}), {{0, 0, 720, 900}, {720, 0, 1600, 900}}), "a step grows by 5% of the width");
  check(stepped.ResetTiledSize(area, 0, stepped.TiledIndex(b), 0.5), "reset the window size");
  check(same(frames(stepped, {a, b}), {{0, 0, 800, 900}, {800, 0, 1600, 900}}), "reset evens the split");
  check(!stepped.ResetTiledSize(area, 0, stepped.TiledIndex(b), 0.5), "an even split has nothing to reset");

  // The companion's prediction runs the real admission, so it lands in the
  // leaf the arrival rule picks.
  const Platform::Monitors::WorkspaceMetrics metrics{area, 96, 0};
  Workspace predicted;
  predicted.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b}) admit(predicted, hwnd);
  const auto prediction = Placement::PredictAutomaticAdmission(predicted, metrics, false, false);
  const RECT expectedPrediction{0, 450, 800, 900};
  check(prediction.tiledFrame && EqualRect(&*prediction.tiledFrame, &expectedPrediction) != FALSE,
        "prediction splits the largest leaf");

  std::printf("Passed %d BSP layout checks.\n", checks);
}

// Random BSP membership changes keep the model valid; closing or minimizing a
// window moves only its sibling's windows; maximizing and restoring moves nothing.
static void RunBspPropertyTests() {
  g_settings = SettingsState{};
  auto fail = [](const char* message, int operation) {
    std::fprintf(stderr, "FAILED: %s (operation %d)\n", message, operation);
    std::exit(1);
  };
  std::mt19937 rng(6016);
  auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
  const RECT area{0, 0, 1920, 1080};
  auto plan = [&](const Workspace& workspace) {
    std::map<HWND, RECT> frames;
    std::vector<RECT> rects;
    if (Layout::BuildWorkspaceLayoutPlan(workspace, area, 6, &rects) != Layout::LayoutPlanStatus::Success) return frames;
    for (size_t i = 0; i < rects.size(); ++i) frames[workspace.TiledWindows()[i]] = rects[i];
    return frames;
  };
  auto sameFrame = [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; };
  // The tiled windows whose frames may change when `leaving` gives up its leaf:
  // its sibling's, or, when that shows none, the nearest ancestor's other side.
  auto siblingWindows = [](const Workspace& workspace, HWND leaving) {
    const BspTree& tree = workspace.Bsp();
    std::set<HWND> sibling;
    for (size_t node = tree.FindLeaf(leaving); sibling.empty();) {
      const size_t parent = tree.Parent(node);
      if (parent == BspTree::kNotFound) break;
      const size_t begin = node == parent + 1 ? tree.SecondChild(parent) : parent + 1;
      for (size_t i = begin; i < tree.SubtreeEnd(begin); ++i) {
        if (workspace.IsTiled(tree.Nodes()[i].window)) sibling.insert(tree.Nodes()[i].window);
      }
      node = parent;
    }
    return sibling;
  };
  int operations = 0;
  for (int run = 0; run < 200; ++run) {
    Workspace workspace;
    workspace.SetLayout(TileLayout::Bsp);
    g_settings.automaticNewWindowPosition =
        pick(0, 1) ? AutomaticNewWindowPosition::AfterFocused : AutomaticNewWindowPosition::LastSlot;
    int nextId = 1;
    for (int step = 0; step < 40; ++step, ++operations) {
      const std::vector<HWND> tiled = workspace.TiledWindows();
      std::vector<HWND> known;
      for (const auto& [hwnd, record] : workspace.Records()) known.push_back(hwnd);
      std::sort(known.begin(), known.end());
      const HWND anyTiled = tiled.empty() ? nullptr : tiled[pick(0, static_cast<int>(tiled.size()) - 1)];
      const HWND anyKnown = known.empty() ? nullptr : known[pick(0, static_cast<int>(known.size()) - 1)];
      const auto before = plan(workspace);
      switch (pick(0, 9)) {
        case 0:
        case 1: {
          WindowRecord record;
          record.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9000 + 4 * nextId++));
          record.pid = 1;
          if (anyTiled && pick(0, 1)) workspace.AdmitTiledAfter(record, anyTiled);
          else workspace.AdmitTiled(record);
          break;
        }
        case 2:
        case 3: {
          if (!anyTiled) break;
          const std::set<HWND> sibling = siblingWindows(workspace, anyTiled);
          if (pick(0, 1)) workspace.Forget(anyTiled);
          else workspace.Suspend(anyTiled, SuspensionReason::Minimized);
          for (const auto& [hwnd, frame] : plan(workspace)) {
            if (!sibling.count(hwnd) && !sameFrame(frame, before.at(hwnd))) fail("leaving moved a non-sibling", operations);
          }
          break;
        }
        case 4: {
          if (!anyTiled) break;
          Workspace maximized = workspace;
          maximized.Suspend(anyTiled, SuspensionReason::Maximized);
          if (pick(0, 1)) {
            maximized.Restore(anyTiled);
            const auto after = plan(maximized);
            for (const auto& [hwnd, frame] : before) {
              if (!sameFrame(frame, after.at(hwnd))) fail("maximize and restore moved a window", operations);
            }
          }
          workspace = maximized;
          break;
        }
        case 5:
          for (const auto& [hwnd, record] : workspace.Records()) {
            if (record.state == ManageState::Suspended) {
              workspace.Restore(hwnd);
              break;
            }
          }
          break;
        case 6:
          if (anyKnown) workspace.Float(anyKnown);
          break;
        case 7:
          if (anyKnown) workspace.ActivateTiled(anyKnown);
          break;
        case 8:
          if (anyTiled) workspace.RememberFocusedWindow(anyTiled);
          if (tiled.size() >= 2) workspace.SwapTiled(tiled.front(), tiled.back());
          break;
        case 9:
          workspace.SetLayout(pick(0, 1) ? TileLayout::Monocle : TileLayout::Columns);
          workspace.SetLayout(TileLayout::Bsp);
          break;
      }
      std::vector<std::wstring> errors;
      if (!workspace.Validate(DesktopMonitorKey{}, L"bsp property", &errors, false)) {
        std::fwprintf(stderr, L"%ls\n", errors.front().c_str());
        fail("BSP workspace invariant broke", operations);
      }
      if (workspace.ActiveCount() > 0 && plan(workspace).size() != workspace.ActiveCount()) {
        fail("an unconstrained BSP workspace failed to plan", operations);
      }
    }
  }
  g_settings = SettingsState{};
  std::printf("Passed BSP properties over %d random operations.\n", operations);
}

// What arrangement does with a BSP arrival of minimum `minWidth` x `minHeight`
// admitted into `workspace` (after `anchor` when given): its tile, or no
// value when it floats. The same steps as ArrangeWorkspace: the plan with the
// new split turned if needed, then settling recovery, then a float.
static std::optional<RECT> ArrangeBspArrival(Workspace workspace, const Platform::Monitors::WorkspaceMetrics& metrics,
                                             HWND arrival, HWND anchor, LONG minWidth, LONG minHeight) {
  WindowRecord record{.hwnd = arrival};
  record.constraints = {minWidth, minHeight, metrics.dpi, true};
  if (!(anchor ? workspace.AdmitTiledAfter(record, anchor) : workspace.AdmitTiled(record))) return std::nullopt;
  std::vector<RECT> rects;
  if (Layout::PlanOrTurnNewBspSplit(workspace, metrics.workArea, metrics.gap, &rects) !=
      Layout::LayoutPlanStatus::Success) {
    Layout::SettlingRecoveryPlan recovery;
    if (!Layout::TryRecoverSettlingWindow(workspace, metrics.workArea, metrics.gap, {{arrival, 0}}, nullptr,
                                          &recovery)) {
      return std::nullopt;
    }
    workspace = std::move(recovery.workspace);
    rects = std::move(recovery.rects);
  }
  return rects[workspace.TiledIndex(arrival)];
}

// The companion's decision for the same arrival, from the published tiles
// alone: true when it agrees exactly with `real` (the same tile, or both float).
// No tiles at all means not even a minimal window fits, which the snapshot's
// newWindowFloats tells the companion; the arrival must then float.
static bool CompanionAgrees(const CompanionAbi::BspArrivalTiles& tiles, const RECT& workArea,
                            const std::optional<RECT>& real, LONG minWidth, LONG minHeight) {
  if (tiles.count == 0) return !real;
  const CompanionAbi::BspArrivalChoice choice = CompanionAbi::ChooseBspArrivalTile(tiles, workArea, minWidth, minHeight);
  if (!real) return choice.outcome == CompanionAbi::BspArrivalChoice::Outcome::Floats;
  return choice.outcome == CompanionAbi::BspArrivalChoice::Outcome::Tile && EqualRect(&choice.tile, &*real) != FALSE;
}

// The BSP arrival tiles published to the companion let it place a new window
// exactly where arrangement will: for windows of many minimum sizes, in fixed
// and random workspaces, it picks the same tile at the same rectangle, adaptive
// dividers included, and floats the same windows.
static void RunBspArrivalTileTests() {
  g_settings = SettingsState{};
  g_settings.automaticNewWindowPosition = AutomaticNewWindowPosition::AfterFocused;
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  const Platform::Monitors::WorkspaceMetrics metrics{{0, 0, 1600, 900}, 96, 0};
  const HWND a = reinterpret_cast<HWND>(0x7800), b = reinterpret_cast<HWND>(0x7804);
  const HWND c = reinterpret_cast<HWND>(0x7808), arrival = reinterpret_cast<HWND>(0x780C);
  Workspace established;
  established.SetLayout(TileLayout::Bsp);
  for (HWND hwnd : {a, b, c}) check(established.AdmitTiled({.hwnd = hwnd}), "admit a BSP window");
  // Settled, as arrangement leaves them once they hold their tiles: only the
  // arrival's own split may then turn.
  for (HWND hwnd : {a, b, c}) established.MarkSettled(hwnd);
  check(established.RememberFocusedWindow(a), "focus the top-left quarter");

  const CompanionAbi::BspArrivalTiles tiles = Placement::PredictBspArrivalTiles(established, metrics, true);
  auto same = [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; };
  check(tiles.count == 6 && !tiles.truncated && same(tiles.tiles[0].tile, {400, 0, 800, 450}) &&
            same(tiles.tiles[1].tile, {0, 225, 800, 450}) && same(tiles.tiles[2].tile, {400, 450, 800, 900}) &&
            same(tiles.tiles[3].tile, {0, 675, 800, 900}) && same(tiles.tiles[4].tile, {800, 450, 1600, 900}) &&
            same(tiles.tiles[5].tile, {1200, 0, 1600, 900}),
        "the focused tile, turned, then its neighbour, turned, then the larger tile beyond, turned");

  // More tiles than the snapshot holds: the list says so, and a window that
  // fits none of them gets no hint rather than a predicted float.
  {
    Workspace crowded;
    crowded.SetLayout(TileLayout::Bsp);
    for (uintptr_t i = 0; i < 10; ++i) {
      const HWND hwnd = reinterpret_cast<HWND>(0x7A00 + 4 * i);
      check(crowded.AdmitTiled({.hwnd = hwnd}), "admit a window to a crowded BSP workspace");
      crowded.MarkSettled(hwnd);
    }
    const CompanionAbi::BspArrivalTiles many = Placement::PredictBspArrivalTiles(crowded, metrics, false);
    check(many.count <= CompanionAbi::kMaxBspArrivalTiles && many.count >= CompanionAbi::kMaxBspArrivalTiles - 1 &&
              many.truncated,
          "a crowded workspace fills the tile list and marks it truncated");
    check(CompanionAbi::ChooseBspArrivalTile(many, metrics.workArea, 2000, 2000).outcome ==
              CompanionAbi::BspArrivalChoice::Outcome::Unknown,
          "a window that fits no published tile of a truncated list gets no hint");
  }

  for (LONG minWidth = 50; minWidth <= 1650; minWidth += 100) {
    for (LONG minHeight = 50; minHeight <= 950; minHeight += 75) {
      const auto real = ArrangeBspArrival(established, metrics, arrival, a, minWidth, minHeight);
      check(CompanionAgrees(tiles, metrics.workArea, real, minWidth, minHeight),
            "the companion places the arrival exactly where arrangement does");
    }
  }

  // Random workspaces: trees, saved ratios, minimum sizes, gaps, orientations
  // and adaptive settings.
  std::mt19937 rng(6017);
  auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
  int compared = 0;
  for (int run = 0; run < 400; ++run) {
    g_settings = SettingsState{};
    g_settings.automaticNewWindowPosition =
        pick(0, 1) ? AutomaticNewWindowPosition::AfterFocused : AutomaticNewWindowPosition::LastSlot;
    g_settings.adaptiveDwindleSlots = static_cast<size_t>(pick(0, 3));
    g_settings.adaptiveDwindlePercentage = pick(0, 40);
    const bool portrait = pick(0, 3) == 0;
    const LONG width = portrait ? pick(700, 1200) : pick(1000, 2600);
    const LONG height = portrait ? pick(1200, 2000) : pick(600, 1500);
    const Platform::Monitors::WorkspaceMetrics randomMetrics{{0, 0, width, height}, 96, pick(0, 16)};
    Workspace workspace;
    workspace.SetLayout(TileLayout::Bsp);
    const int count = pick(1, 7);
    std::vector<HWND> members;
    for (int i = 0; i < count; ++i) {
      WindowRecord record{.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x7900 + 4 * i))};
      if (pick(0, 1)) record.constraints = {pick(1, width / 3), pick(1, height / 3), 96, true};
      workspace.AdmitTiled(record);
      members.push_back(record.hwnd);
    }
    for (int i = 0; i < 3; ++i) {
      const size_t index = static_cast<size_t>(pick(0, count - 1));
      workspace.StepTiledSize(randomMetrics.workArea, randomMetrics.gap, index, pick(0, 1) != 0, pick(1, 20));
    }
    for (HWND hwnd : members) workspace.MarkSettled(hwnd);
    workspace.RememberFocusedWindow(members[static_cast<size_t>(pick(0, count - 1))]);
    std::vector<RECT> rects;
    if (Layout::BuildWorkspaceLayoutPlan(workspace, randomMetrics.workArea, randomMetrics.gap, &rects) !=
        Layout::LayoutPlanStatus::Success) {
      continue;  // Arrangement would float someone before any arrival.
    }
    const bool afterFocused = g_settings.automaticNewWindowPosition == AutomaticNewWindowPosition::AfterFocused;
    const CompanionAbi::BspArrivalTiles published = Placement::PredictBspArrivalTiles(workspace, randomMetrics, afterFocused);
    const HWND anchor = afterFocused ? workspace.LastFocusedTiledWindow() : nullptr;
    for (int sample = 0; sample < 12; ++sample) {
      const LONG minWidth = pick(1, width + 50);
      const LONG minHeight = pick(1, height + 50);
      const auto real = ArrangeBspArrival(workspace, randomMetrics, arrival, anchor, minWidth, minHeight);
      check(CompanionAgrees(published, randomMetrics.workArea, real, minWidth, minHeight),
            "the companion agrees with arrangement in a random BSP workspace");
      ++compared;
    }
  }
  check(compared > 2000, "the random cross-check compares enough arrivals");
  g_settings = SettingsState{};
  std::printf("Passed %d BSP arrival tile checks.\n", checks);
}

// What the overlays draw, with literal numbers: the toast's frame, the preview's
// fill and outline opacities, and the badges' cards and icons (app/overlay.h).
static void RunOverlayGeometryTests() {
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
    ++checks;
  };
  auto same = [](const D2D1_RECT_F& a, float left, float top, float right, float bottom) {
    return a.left == left && a.top == top && a.right == right && a.bottom == bottom;
  };
  auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
  auto sameRect = [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; };

  // Toasts on a 1920x1040 work area.
  const SIZE work{1920, 1040};
  Overlay::ToastRequest toast;
  auto frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  check(frame && same(*frame, 835, 976, 1084, 1024),
        "a bottom toast is centred, as wide as its text plus 48 DIP, 16 DIP above the bottom");
  toast.atTop = true;
  frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  check(frame && same(*frame, 835, 16, 1084, 64), "a top toast sits 16 DIP below the top");
  toast.atTop = false;
  frame = Overlay::ToastFrame(toast, work, 96, 10);
  check(frame && frame->right - frame->left == 128, "short text keeps the 128 DIP minimum width");
  frame = Overlay::ToastFrame(toast, work, 96, 5000);
  check(frame && same(*frame, 0, 976, 1920, 1024), "text wider than the work area is clipped to its width");
  toast.xOffsetDip = 100;
  toast.yOffsetDip = -20;
  frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  check(frame && same(*frame, 935, 956, 1184, 1004), "offsets move the toast");
  toast.xOffsetDip = 100000;
  toast.yOffsetDip = 500;
  frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  check(frame && same(*frame, 1671, 992, 1920, 1040), "offsets past the work area stop at its edges");
  toast.xOffsetDip = -100000;
  frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  check(frame && frame->left == 0, "a large negative offset stops at the left edge");
  toast = {};
  frame = Overlay::ToastFrame(toast, work, 144, 200);
  check(frame && frame->bottom - frame->top == 72 && frame->right - frame->left == 272 && frame->top == 1040 - 72 - 24,
        "toast sizes and gaps scale with DPI");
  check(!Overlay::ToastFrame(toast, SIZE{800, 40}, 96, 50), "a work area shorter than a toast shows none");

  // A toast's icons and text, centred as one row.
  auto whole = [](const D2D1_RECT_F& r) {
    return r.left == std::floor(r.left) && r.top == std::floor(r.top) && r.right == std::floor(r.right) &&
           r.bottom == std::floor(r.bottom);
  };
  check(Overlay::ToastIconsWidth(0, 16, 96) == 0 && Overlay::ToastIconsWidth(1, 16, 96) == 24 &&
            Overlay::ToastIconsWidth(3, 24, 144) == 3 * 24 + 2 * 6 + 12,
        "icons are 4 DIP apart and 8 DIP before the text");
  frame = Overlay::ToastFrame(toast, work, 96, Overlay::ToastIconsWidth(1, 16, 96) + 200.3f);
  auto content = Overlay::LayOutToastContent(*frame, 1, 16, 96, 200.3f, 24.6f);
  check(frame && frame->right - frame->left == 273 && content.icons.size() == 1 &&
            content.icons[0].left - frame->left == 24 && frame->right - (content.textLeft + content.textWidth) == 24 &&
            content.textLeft - content.icons[0].right == 8 && content.textWidth == 201,
        "an icon and its text keep 24 DIP from either side of the toast");
  check(whole(content.icons[0]) && content.icons[0].right - content.icons[0].left == 16 &&
            content.icons[0].top == std::round(frame->top + 24.6f - 8),
        "the icon sits on whole pixels, centred on the text's capitals");
  frame = Overlay::ToastFrame(toast, work, 144, Overlay::ToastIconsWidth(3, 24, 144) + 300);
  content = Overlay::LayOutToastContent(*frame, 3, 24, 144, 300, 36);
  check(content.icons.size() == 3 && content.icons[1].left - content.icons[0].right == 6 &&
            content.icons[2].left - content.icons[1].right == 6 && content.textLeft - content.icons[2].right == 12 &&
            std::ranges::all_of(content.icons, whole),
        "several icons scale their spacing with DPI");
  frame = Overlay::ToastFrame(toast, work, 96, 200.3f);
  content = Overlay::LayOutToastContent(*frame, 0, 16, 96, 200.3f, 24);
  check(content.icons.empty() && content.textLeft - frame->left == 24 && content.textWidth == 201,
        "without icons the text is centred as before");
  frame = Overlay::ToastFrame(toast, work, 96, Overlay::ToastIconsWidth(1, 16, 96) + 5000);
  content = Overlay::LayOutToastContent(*frame, 1, 16, 96, 5000, 24);
  check(content.icons[0].left == frame->left && content.textLeft + content.textWidth == frame->right,
        "text wider than the work area is cut at the toast's edge, after its icon");

  // The tray icon: drawn at the requested size, with smooth edges whatever the
  // system's font smoothing.
  for (const int size : {16, 20, 24, 32}) {
    const HICON icon = TrayUi::CreateTextIcon(TileLayout::Bsp, size);
    ICONINFO info{};
    BITMAP bitmap{};
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    const HDC dc = GetDC(nullptr);
    const bool read = icon && GetIconInfo(icon, &info) && info.hbmColor &&
                      GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) && bitmap.bmWidth == size &&
                      bitmap.bmHeight == size &&
                      GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &bmi, DIB_RGB_COLORS) == size;
    ReleaseDC(nullptr, dc);
    const auto alpha = [](uint32_t pixel) { return pixel >> 24; };
    check(read && std::ranges::any_of(pixels, [&](uint32_t p) { return alpha(p) == 255; }) &&
              std::ranges::any_of(pixels, [&](uint32_t p) { return alpha(p) > 0 && alpha(p) < 255; }),
          "the tray icon is drawn at its size with opaque strokes and partly covered edges");
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (icon) DestroyIcon(icon);
  }

  // Preview opacities: fill and outline each at their own share of the fade.
  Overlay::PreviewStyle style;
  style.fillOpacity = 25;
  style.outlineOpacity = 100;
  auto opacity = Overlay::PreviewOpacitiesAt(style, 255);
  check(approx(opacity.fill, 0.25f) && approx(opacity.outline, 1.0f) && approx(opacity.badge, 1.0f),
        "a fully faded-in preview uses the configured opacities");
  opacity = Overlay::PreviewOpacitiesAt(style, 128);
  check(approx(opacity.fill, 0.25f * 128 / 255) && approx(opacity.outline, 128 / 255.0f) &&
            approx(opacity.badge, 128 / 255.0f),
        "fill, outline and badges fade together");
  style.fillOpacity = 10;
  style.outlineOpacity = 60;
  opacity = Overlay::PreviewOpacitiesAt(style, 255);
  check(approx(opacity.fill, 0.10f) && approx(opacity.outline, 0.60f), "fill and outline opacities are independent");
  style.fillOpacity = 0;
  opacity = Overlay::PreviewOpacitiesAt(style, 255);
  check(opacity.fill == 0 && approx(opacity.outline, 0.60f), "zero fill opacity draws only the outline");
  check(Overlay::PreviewOpacitiesAt(style, 0).outline == 0, "a faded-out preview draws nothing");

  // Badges: a card the icon plus 4 DIP each side, centred on the tile.
  const RECT tile{0, 0, 400, 300};
  auto badge = Overlay::BadgeGeometryFor(tile, 32, 96);
  check(badge && badge->iconSize == 32 && badge->badgeSize == 40 && sameRect(badge->frame, RECT{180, 130, 220, 170}),
        "a badge is centred on its tile");
  badge = Overlay::BadgeGeometryFor(tile, 32, 144);
  check(badge && badge->iconSize == 48 && badge->badgeSize == 60 && sameRect(badge->frame, RECT{170, 120, 230, 180}),
        "badge sizes scale with DPI");
  check(!Overlay::BadgeGeometryFor(tile, 0, 96), "an icon size of 0 shows no badge");
  check(!Overlay::BadgeGeometryFor(RECT{0, 0, 39, 300}, 32, 96) && Overlay::BadgeGeometryFor(RECT{0, 0, 40, 40}, 32, 96),
        "a badge shows only on a tile it fits");
  check(same(Overlay::BadgeIconRect(D2D1::RectF(180, 130, 220, 170), 40, 32), 184, 134, 216, 166),
        "the icon is centred in its card");
  check(same(Overlay::BadgeIconRect(D2D1::RectF(0, 0, 61, 61), 61, 48), 6.5f, 6.5f, 54.5f, 54.5f),
        "an odd margin centres the icon on a half pixel");
  check(same(Overlay::ToHostRect(RECT{1930, 10, 2030, 110}, RECT{1920, 0, 3840, 1040}), 10, 10, 110, 110),
        "screen rectangles map to the host's own coordinates");
  std::printf("Passed %d overlay geometry checks.\n", checks);
}

int main() {
  RunOverlayGeometryTests();
  RunBspLayoutTests();
  RunBspPropertyTests();
  RunBspArrivalTileTests();
  RunMaximumSizeTests();
  RunSettingsParserDigest();
  RunRuleFieldParserTests();
  RunAdaptiveSplitTests();
  RunWeightedResizeTests();
  RunFloatingPlacementTests();
  RunWindowSizeStepTests();
  RunWindowSizePropertyTests();
  RunDwindleResizeScenarioTests();
  RunDwindlePlannerDigest();
  RunPlanningTraceDigest();
  RunAdmissionPredictionTests();
  RunCompanionRuleEncodingTests();
  RunRestorePublicationTests();
  RunTrackedWindowPublicationTests();
  RunCompanionSnapshotRecoveryTests();
}
