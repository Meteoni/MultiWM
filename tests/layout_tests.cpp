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

// Grow, shrink and reset change only the preference that sizes the chosen
// window, start from its visible span, and stop at minimums.
static void RunWindowSizeStepTests() {
  g_settings = SettingsState{};
  const RECT area{0, 0, 1000, 1000};
  constexpr LONG kStep = 5;  // 50 px of a 1000 px span.
  int checks = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) {
      std::fprintf(stderr, "FAILED: %s\n", message);
      std::exit(1);
    }
    ++checks;
  };
  auto make = [&](TileLayout layout, size_t count, LONG firstMinWidth = 80) {
    Workspace workspace;
    workspace.SetLayout(layout);
    for (size_t index = 0; index < count; ++index) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(index + 1);
      record.pid = 1;
      record.constraints = {index == 0 ? firstMinWidth : 80, 80, 96, true};
      check(workspace.AdmitTiled(record), "admit test window");
    }
    return workspace;
  };
  auto span = [&](const Workspace& workspace, size_t index, bool alongY) {
    std::vector<RECT> rects;
    check(Layout::BuildWorkspaceLayoutPlan(workspace, area, 0, &rects) ==
              Layout::LayoutPlanStatus::Success, "plan stepped layout");
    return alongY ? Layout::RectHeight(rects[index]) : Layout::RectWidth(rects[index]);
  };
  auto aboutEqual = [](LONG actual, LONG expected) { return std::abs(actual - expected) <= 1; };

  for (TileLayout layout : {TileLayout::Columns, TileLayout::Rows}) {
    const bool alongY = layout == TileLayout::Rows;
    Workspace grid = make(layout, 3);
    const LONG initial = span(grid, 0, alongY);
    check(grid.StepTiledSize(area, 0, 0, true, kStep) && aboutEqual(span(grid, 0, alongY), initial + 50),
          "grow adds one step to the window");
    check(grid.GridWeights()[1] == 1.0 && grid.GridWeights()[2] == 1.0, "grow changes only the window's weight");
    check(aboutEqual(span(grid, 1, alongY), span(grid, 2, alongY)), "peers keep their relative sizes");
    check(grid.StepTiledSize(area, 0, 0, false, kStep) && aboutEqual(span(grid, 0, alongY), initial),
          "shrink undoes grow");
    check(grid.StepTiledSize(area, 0, 0, true, kStep) && grid.ResetTiledSize(area, 0, 0, 0.5) &&
              grid.GridWeights() == std::vector<double>({1, 1, 1}),
          "reset returns the weight to its peers' mean");
    check(!grid.ResetTiledSize(area, 0, 0, 0.5), "resetting a default size changes nothing");
    int steps = 0;
    while (steps < 100 && grid.StepTiledSize(area, 0, 0, true, kStep)) ++steps;
    check(steps < 100 && span(grid, 1, alongY) >= 80 && span(grid, 2, alongY) >= 80,
          "growth stops at the peers' minimums");
  }

  // A window pinned at its minimum: its own shrink stops there, and a peer's
  // step leaves its preference alone.
  Workspace pinned = make(TileLayout::Columns, 3, 650);
  check(span(pinned, 0, false) == 650, "minimum displaces the divider");
  check(!pinned.StepTiledSize(area, 0, 0, false, kStep), "shrink stops at the native minimum");
  const LONG peer = span(pinned, 1, false);
  check(pinned.StepTiledSize(area, 0, 1, true, kStep) && aboutEqual(span(pinned, 1, false), peer + 50) &&
            span(pinned, 0, false) == 650 && pinned.GridWeights()[0] == 1.0,
        "a peer grows from its visible size without saving the pinned window's fitting");

  for (TileLayout layout : {TileLayout::MasterStack, TileLayout::MasterStackH}) {
    const bool mainY = layout == TileLayout::MasterStackH;
    Workspace stack = make(layout, 3);
    check(stack.StepTiledSize(area, 0, 0, true, kStep) && std::abs(stack.MasterRatio() - 0.55) < 0.002 &&
              stack.StackWeights() == std::vector<double>({1, 1}),
          "growing the master moves only the master divider");
    const LONG stackSpan = span(stack, 1, !mainY);
    check(stack.StepTiledSize(area, 0, 1, true, kStep) && aboutEqual(span(stack, 1, !mainY), stackSpan + 50) &&
              stack.StackWeights()[1] == 1.0 && std::abs(stack.MasterRatio() - 0.55) < 0.002,
          "growing a stack window moves only its stack weight");
    check(stack.ResetTiledSize(area, 0, 0, 0.5) && stack.MasterRatio() == 0.5, "reset returns the master ratio");
    check(stack.ResetTiledSize(area, 0, 1, 0.5) && stack.StackWeights() == std::vector<double>({1, 1}),
          "reset returns a stack weight to its peers' mean");

    Workspace pair = make(layout, 2);
    check(pair.StepTiledSize(area, 0, 1, true, kStep) && std::abs(pair.MasterRatio() - 0.45) < 0.002,
          "a lone stack window grows by moving the master divider");
    check(pair.ResetTiledSize(area, 0, 1, 0.5) && pair.MasterRatio() == 0.5, "a lone stack window resets the master ratio");
  }

  Workspace dwindle = make(TileLayout::Dwindle, 3);
  check(dwindle.StepTiledSize(area, 0, 0, true, kStep) && std::abs(dwindle.DwindleRatios()[0] - 0.55) < 0.002 &&
            dwindle.DwindleRatios()[1] == 0.5,
        "a Dwindle window grows by its own split");
  check(dwindle.StepTiledSize(area, 0, 2, true, kStep) && std::abs(dwindle.DwindleRatios()[1] - 0.45) < 0.002 &&
            aboutEqual(span(dwindle, 2, true), 550),
        "the last Dwindle window grows by the split before it");
  check(dwindle.ResetTiledSize(area, 0, 2, 0.5) && dwindle.DwindleRatios()[1] == 0.5 &&
            std::abs(dwindle.DwindleRatios()[0] - 0.55) < 0.002,
        "reset evens only the window's own split");
  check(dwindle.ResetTiledSize(area, 0, 0, 0.5) && dwindle.DwindleRatios() == std::vector<double>({0.5, 0.5}),
        "reset evens the first split");

  // Deep Dwindle splits under adaptive fitting: a minimum taller than half the
  // work area (LocalSend) makes the plain layout unsatisfiable, and the ends of
  // the ratio range don't fit at all. Steps must still move the deep dividers.
  {
    const RECT wide{12, 12, 3060, 1812};
    const LONG wideGap = 12;
    const LONG minimums[4][2] = {{824, 553}, {931, 193}, {650, 397}, {786, 993}};
    Workspace deep;
    deep.SetLayout(TileLayout::Dwindle);
    for (size_t index = 0; index < 4; ++index) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(index + 1);
      record.pid = 1;
      record.constraints = {minimums[index][0], minimums[index][1], 192, true};
      check(deep.AdmitTiled(record), "admit deep Dwindle window");
    }
    auto width = [&](const Workspace& workspace, size_t index) {
      std::vector<RECT> rects;
      check(Layout::BuildWorkspaceLayoutPlan(workspace, wide, wideGap, &rects) ==
                Layout::LayoutPlanStatus::Success, "plan deep Dwindle layout");
      return Layout::RectWidth(rects[index]);
    };
    // On the accepted layout only the stepped divider moves. LocalSend (3) can
    // take room from Notepad (2); Notepad can't take any from LocalSend, which
    // is at its minimum, and no longer borrows it by moving another divider.
    Workspace grown = deep;
    check(grown.StepTiledSize(wide, wideGap, 3, true, kStep) && width(grown, 3) > width(deep, 3) &&
              width(grown, 0) == width(deep, 0) && width(grown, 1) == width(deep, 1),
          "a deep Dwindle window grows its split; no other divider moves");
    Workspace blocked = deep;
    check(!blocked.StepTiledSize(wide, wideGap, 2, true, kStep) && blocked.DwindleRatios() == deep.DwindleRatios(),
          "a window whose neighbour is at its minimum does not grow, and saves nothing");

    // Drags follow the same rule: the dragged divider stops where the accepted
    // layout runs out of room, so no other divider moves to make it fit.
    std::vector<RECT> deepRects;
    check(Layout::BuildWorkspaceLayoutPlan(deep, wide, wideGap, &deepRects) == Layout::LayoutPlanStatus::Success,
          "plan before dragging");
    MoveSizeGesture drag;
    drag.intent = MoveSizeIntent::Resize;
    drag.hasRects = true;
    drag.start = drag.end = deepRects[2];
    drag.end.right += 200;  // Notepad into LocalSend, which is at its minimum.
    Workspace intoMinimum = deep;
    check(!intoMinimum.LearnDwindleResize(wide, wideGap, 2, drag) && intoMinimum.DwindleRatios() == deep.DwindleRatios(),
          "a drag into a neighbour at its minimum changes nothing");
    drag.start = drag.end = deepRects[3];
    drag.end.left -= 300;  // LocalSend into Notepad, which has 136 px to spare.
    Workspace partial = deep;
    check(partial.LearnDwindleResize(wide, wideGap, 3, drag) && width(partial, 0) == width(deep, 0) &&
              width(partial, 1) == width(deep, 1) && std::abs(width(partial, 3) - (width(deep, 3) + 136)) <= 1,
          "a drag stops where the neighbour reaches its minimum; no other divider moves");
    // A squeezed window: adaptive fitting holds the first window below its
    // saved half. Like a drag, a step saves the size it shows, so that size is
    // what remains once the squeeze ends.
    Workspace squeezed = deep;
    check(width(squeezed, 0) < (Layout::RectWidth(wide) - wideGap) / 2, "the first window is squeezed");
    check(squeezed.StepTiledSize(wide, wideGap, 0, true, kStep), "grow the squeezed window");
    const LONG shown = width(squeezed, 0);
    for (HWND hwnd : squeezed.TiledWindows()) squeezed.UpdateConstraints(hwnd, {80, 80, 192, true});
    check(std::abs(width(squeezed, 0) - shown) <= 1, "the size a step shows is the size kept after the squeeze");
    Workspace shrunk = deep;
    check(shrunk.StepTiledSize(wide, wideGap, 2, false, kStep) && width(shrunk, 2) < width(deep, 2),
          "a deep Dwindle window shrinks its split under adaptive fitting");
    Workspace pinnedDeep = deep;
    check(!pinnedDeep.StepTiledSize(wide, wideGap, 3, false, kStep) &&
              pinnedDeep.DwindleRatios() == deep.DwindleRatios(),
          "a window at its minimum width does not shrink or change its ratio");
    Workspace shrinkTop = deep;
    check(shrinkTop.StepTiledSize(wide, wideGap, 1, false, kStep) &&
              shrinkTop.DwindleRatios()[2] == deep.DwindleRatios()[2],
          "shrinking a window leaves deeper dividers alone");
  }

  // Growing an early Dwindle window pushes a fixed deeper divider as far as its
  // windows' minimums allow, instead of stopping at the divider's saved position.
  {
    g_settings.adaptiveDwindleSlots = 2;
    g_settings.adaptiveDwindlePercentage = 15;
    const RECT wide{14, 14, 3058, 1804};
    const LONG wideGap = 14;
    const LONG minimums[4][2] = {{802, 542}, {300, 475}, {300, 475}, {644, 394}};
    Workspace pushed;
    pushed.SetLayout(TileLayout::Dwindle);
    for (size_t index = 0; index < 4; ++index) {
      WindowRecord record;
      record.hwnd = reinterpret_cast<HWND>(index + 1);
      record.pid = 1;
      record.constraints = {minimums[index][0], minimums[index][1], 192, true};
      check(pushed.AdmitTiled(record), "admit pushed Dwindle window");
    }
    auto widths = [&](const Workspace& workspace) {
      std::vector<RECT> rects;
      check(Layout::BuildWorkspaceLayoutPlan(workspace, wide, wideGap, &rects) ==
                Layout::LayoutPlanStatus::Success, "plan pushed Dwindle layout");
      std::vector<LONG> result;
      for (const RECT& rect : rects) result.push_back(Layout::RectWidth(rect));
      return result;
    };
    // Grow the deep Explorer until Notepad, beside it, is at its minimum.
    check(pushed.StepTiledSize(wide, wideGap, 2, true, kStep), "grow the deep window");
    check(!pushed.StepTiledSize(wide, wideGap, 2, true, kStep), "the deep window stops at Notepad's minimum");
    const double deepRatio = pushed.DwindleRatios()[2];
    LONG previous = widths(pushed)[0];
    int grown = 0;
    while (grown < 20 && pushed.StepTiledSize(wide, wideGap, 0, true, kStep)) {
      const auto now = widths(pushed);
      check(now[0] > previous && now[2] >= 300 && now[3] >= 644, "each grow widens the first window within minimums");
      previous = now[0];
      ++grown;
    }
    check(grown >= 3 && pushed.DwindleRatios()[2] < deepRatio,
          "the first window grows past the deep divider's saved position by pushing it");
    check(widths(pushed)[2] < 300 + 76, "growth stops once the pushed window reaches its minimum");
    const Workspace fullyPushed = pushed;
    const auto pushedRatios = pushed.DwindleRatios();

    // Shrinking the window that pushed lets the divider spring back, one step
    // at a time, and never costs the shrinking window more than its step.
    check(pushed.StepTiledSize(wide, wideGap, 0, false, kStep) && widths(pushed)[0] < previous &&
              widths(pushed)[0] >= previous - 152 - 2 && pushed.DwindleRatios()[0] < pushedRatios[0],
          "one shrink moves the first divider back by one step");
    check(pushed.DwindleRatios()[2] > pushedRatios[2] && pushed.DwindleRatios()[2] <= deepRatio + 1e-9,
          "the shrink lets the pushed divider spring back toward its earlier position");
    for (int i = 0; i < 20 && std::abs(pushed.DwindleRatios()[2] - deepRatio) > 1e-9; ++i) {
      check(pushed.StepTiledSize(wide, wideGap, 0, false, kStep), "keep shrinking the first window");
    }
    check(std::abs(pushed.DwindleRatios()[2] - deepRatio) <= 1e-9,
          "shrinking far enough returns the pushed divider exactly");

    // Reset returns pushed dividers as far as the reset window's size allows.
    Workspace reset = fullyPushed;
    check(reset.ResetTiledSize(wide, wideGap, 0, 0.5) && reset.DwindleRatios()[0] == 0.5 &&
              reset.DwindleRatios()[2] > pushedRatios[2] && reset.DwindleRatios()[2] <= deepRatio + 1e-9,
          "resetting the window that pushed lets the divider spring back");
    std::vector<RECT> resetRects;
    std::vector<RECT> plainRects(4);
    Layout::LayoutDwindle(wide, wideGap, 0, 4, reset.DwindleRatios(), false, plainRects);
    check(Layout::BuildWorkspaceLayoutPlan(reset, wide, wideGap, &resetRects) == Layout::LayoutPlanStatus::Success &&
              Layout::RectWidth(resetRects[0]) == Layout::RectWidth(plainRects[0]),
          "the reset window keeps its default size while the divider springs back");

    // A divider the user steps or drags itself, or whose window leaves the slot,
    // stays where it is. Fully pushed, both windows beside the deep divider are
    // at their minimums, so one shrink of the first window frees room to step it.
    Workspace kept = fullyPushed;
    check(kept.StepTiledSize(wide, wideGap, 0, false, kStep), "free room beside the pushed divider");
    check(kept.StepTiledSize(wide, wideGap, 3, true, kStep), "step the pushed divider's last window");
    const double keptRatio = kept.DwindleRatios()[2];
    check(kept.StepTiledSize(wide, wideGap, 0, false, kStep) && kept.DwindleRatios()[2] == keptRatio,
          "a divider the user stepped no longer springs back");
    Workspace swapped = fullyPushed;
    // Both Explorers have the same minimums, so the swapped layout still fits.
    check(swapped.SwapTiled(swapped.TiledWindows()[1], swapped.TiledWindows()[2]), "swap the pushed slot's window");
    check(swapped.StepTiledSize(wide, wideGap, 0, false, kStep) && swapped.DwindleRatios()[2] == pushedRatios[2],
          "a push lapses once its window leaves the slot");
    g_settings = SettingsState{};
  }

  Workspace monocle = make(TileLayout::Monocle, 2);
  check(!monocle.StepTiledSize(area, 0, 0, true, kStep) && !monocle.ResetTiledSize(area, 0, 0, 0.5),
        "Monocle has no window sizes");
  std::printf("Passed %d window-size step checks.\n", checks);
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
    crowded.ClearInsertionPlacement(record.hwnd);
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

int main() {
  RunMaximumSizeTests();
  RunSettingsParserDigest();
  RunRuleFieldParserTests();
  RunAdaptiveDwindleTests();
  RunWeightedResizeTests();
  RunWindowSizeStepTests();
  RunDwindlePlannerDigest();
  RunAdmissionPredictionTests();
  RunCompanionRuleEncodingTests();
  RunRestorePublicationTests();
  RunTrackedWindowPublicationTests();
  RunCompanionSnapshotRecoveryTests();
}
