// Learns divider and weight preferences from completed user resize gestures.
// Turns a before/after frame pair into updated ratios or weights on the Workspace.

namespace Model {

constexpr LONG kResizeEdgeDeltaTolerance = 1;
constexpr LONG kMinLearnedResizeSpan = 80;

static bool IsMeaningfulResizeDelta(LONG delta) {
  return std::abs(delta) > kResizeEdgeDeltaTolerance;
}

// --- Visible cells -------------------------------------------------------------
//
// The cells a layout renders before capped windows are centred inside them: the
// preferred allocation, or the constrained one when native minimums displace
// it. Resizes start from these, so a step or drag begins where the user sees the
// divider. `outWeightPerPixel` (optional) receives the constrained allocation's
// weight scale, or 0 when the preferred allocation fits. Weights must already be
// sized for the active windows.

// Grid and Master + Stack. Keeps the preferred cells when no constrained
// allocation fits, so a resize can still start from what is shown.
static bool PlanWeightedCells(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects, double* outWeightPerPixel = nullptr) {
  const std::vector<WindowConstraints> constraints = Layout::ActiveWindowConstraints(workspace);
  if (outWeightPerPixel) *outWeightPerPixel = 0.0;
  Layout::PlanPreferredCells(workspace, workArea, gap, outRects);
  if (!Layout::RectsSatisfyWindowConstraints(*outRects, constraints)) {
    std::vector<RECT> constrainedRects;
    double weightPerPixel = 0.0;
    if (Layout::PlanConstrainedCells(workspace, workArea, gap, constraints, &constrainedRects, &weightPerPixel)) {
      *outRects = std::move(constrainedRects);
      if (outWeightPerPixel) *outWeightPerPixel = weightPerPixel;
    }
  }
  return outRects->size() == workspace.ActiveCount();
}

static bool PlanDwindleCells(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects, std::vector<Layout::DwindleSplit>* outSplits) {
  const size_t count = workspace.ActiveCount();
  Layout::PlanPreferredCells(workspace, workArea, gap, outRects, outSplits);
  const std::vector<WindowConstraints> constraints = Layout::ActiveWindowConstraints(workspace);
  if (!Layout::RectsSatisfyWindowConstraints(*outRects, constraints) &&
      !Layout::PlanConstrainedCells(workspace, workArea, gap, constraints, outRects, nullptr, outSplits)) {
    return false;
  }
  return count >= 2 && outSplits->size() + 1 == count;
}

// The Dwindle or BSP layout exactly as the saved ratios give it, without
// adaptive fitting. Once a resize has accepted the shown layout, this is the
// layout the resize works on. False when a window would be below its minimum.
static bool PlanSavedCells(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects, std::vector<Layout::DwindleSplit>* outSplits = nullptr) {
  if (workspace.ActiveCount() < 2) {
    outRects->assign(workspace.ActiveCount(), RECT{});
    if (outSplits) outSplits->clear();
    return false;
  }
  Layout::PlanPreferredCells(workspace, workArea, gap, outRects, outSplits);
  return Layout::RectsSatisfyWindowConstraints(*outRects, Layout::ActiveWindowConstraints(workspace));
}

// Moves `ratio` toward `target` as far as `allowed()` still holds, which it
// must where the ratio starts. A move can be smaller than a pixel; callers
// that need to know whether anything moved compare the planned layout.
static void MoveRatioToward(double& ratio, double target, const std::function<bool()>& allowed) {
  const double from = ratio;
  ratio = target;
  if (!allowed()) {
    double low = 0.0;  // Fractions of the way to `target`; 0 is allowed.
    double high = 1.0;
    for (int i = 0; i < 24; ++i) {
      const double middle = (low + high) / 2.0;
      ratio = from + (target - from) * middle;
      if (allowed()) {
        low = middle;
      } else {
        high = middle;
      }
    }
    ratio = from + (target - from) * low;
  }
}

// Transfer a drag between the adjacent spans from the rendered layout. When
// minimums displaced the divider, use the constrained allocation's weight scale
// to remember the new visible spans without changing unrelated preferences.
static bool TransferPairResizeDelta(
    std::vector<double>& weights, size_t firstWeight,
    const RECT& firstRect, const RECT& secondRect,
    const WindowConstraints& firstConstraints,
    const WindowConstraints& secondConstraints,
    Layout::Axis axis, LONG delta, double constrainedWeightPerPixel) {
  if (!IsMeaningfulResizeDelta(delta) || firstWeight + 1 >= weights.size()) {
    return false;
  }

  const bool alongY = axis == Layout::Axis::Y;
  const LONG firstSpan = alongY ? firstRect.bottom - firstRect.top
                                      : firstRect.right - firstRect.left;
  const LONG secondSpan = alongY ? secondRect.bottom - secondRect.top
                                       : secondRect.right - secondRect.left;
  const LONG pairSpan = firstSpan + secondSpan;
  if (firstSpan < 1 || secondSpan < 1 || pairSpan <= 1) return false;

  const LONG minSpan = std::min<LONG>(
      kMinLearnedResizeSpan, std::max<LONG>(1, (pairSpan - 1) / 2));
  const LONG firstMinimum = firstConstraints.valid
      ? std::max<LONG>(minSpan, alongY ? firstConstraints.minHeight
                                           : firstConstraints.minWidth)
      : minSpan;
  const LONG secondMinimum = secondConstraints.valid
      ? std::max<LONG>(minSpan, alongY ? secondConstraints.minHeight
                                           : secondConstraints.minWidth)
      : minSpan;
  const LONG maxFirstSpan = pairSpan - secondMinimum;
  if (firstMinimum > maxFirstSpan) return false;
  const LONG newFirstSpan =
      std::clamp<LONG>(firstSpan + delta, firstMinimum, maxFirstSpan);
  if (newFirstSpan == firstSpan) return false;

  if (constrainedWeightPerPixel > 0.0 &&
      std::isfinite(constrainedWeightPerPixel)) {
    weights[firstWeight] = newFirstSpan * constrainedWeightPerPixel;
    weights[firstWeight + 1] =
        (pairSpan - newFirstSpan) * constrainedWeightPerPixel;
    return true;
  }

  double pairWeight = weights[firstWeight] + weights[firstWeight + 1];
  if (!(pairWeight > 0.0) || !std::isfinite(pairWeight)) pairWeight = 2.0;

  const double firstFraction =
      static_cast<double>(newFirstSpan) / static_cast<double>(pairSpan);
  weights[firstWeight] = pairWeight * firstFraction;
  weights[firstWeight + 1] = pairWeight - weights[firstWeight];
  return true;
}

bool Workspace::LearnMasterStackResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::LearnMasterStackResize");
  EnsureWeights();
  // Callers dispatch here only for Master + Stack layouts.
  const auto axis = layout_ == TileLayout::MasterStackH ? Layout::Axis::Y : Layout::Axis::X;
  const bool alongY = axis == Layout::Axis::Y;
  const std::vector<WindowConstraints> constraints =
      Layout::ActiveWindowConstraints(*this);

  double constrainedWeightPerPixel = 0.0;
  std::vector<RECT> expectedRects;
  if (!PlanWeightedCells(*this, workArea, gap, &expectedRects, &constrainedWeightPerPixel)) {
    return false;
  }

  bool changed = false;

  // Main master/stack divider.
  const LONG mainDelta = alongY
      ? (resizedIndex == 0 ? gesture.end.bottom - gesture.start.bottom
                           : gesture.end.top - gesture.start.top)
      : (resizedIndex == 0 ? gesture.end.right - gesture.start.right
                           : gesture.end.left - gesture.start.left);

  if (windows_.size() > 1 && IsMeaningfulResizeDelta(mainDelta)) {
    const LONG totalSize = alongY ? workArea.bottom - workArea.top
                                      : workArea.right - workArea.left;
    const LONG usableSize = totalSize - gap;
    if (usableSize > 1) {
      const LONG oldMasterSpan = alongY
          ? expectedRects[0].bottom - expectedRects[0].top
          : expectedRects[0].right - expectedRects[0].left;
      const LONG newMasterSpan = std::clamp<LONG>(
          oldMasterSpan + mainDelta, 1, usableSize - 1);
      const double newRatio = Layout::ClampSplitRatio(static_cast<double>(newMasterSpan) / static_cast<double>(usableSize));
      if (std::abs(newRatio - masterRatio_) > 1e-9) {
        masterRatio_ = newRatio;
        changed = true;
      }
    }
  }

  // Stack-axis dividers.
  if (resizedIndex > 0 && stackWeights_.size() + 1 == windows_.size()) {
    const auto stackAxis = alongY ? Layout::Axis::X : Layout::Axis::Y;
    const size_t stackIndex = resizedIndex - 1;
    const LONG leadingDelta = alongY
        ? gesture.end.left - gesture.start.left
        : gesture.end.top - gesture.start.top;
    const LONG trailingDelta = alongY
        ? gesture.end.right - gesture.start.right
        : gesture.end.bottom - gesture.start.bottom;

    if (stackIndex > 0) {
      changed |= TransferPairResizeDelta(
          stackWeights_, stackIndex - 1,
          expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
          constraints[resizedIndex - 1], constraints[resizedIndex],
          stackAxis, leadingDelta, constrainedWeightPerPixel);
    }
    if (stackIndex + 1 < stackWeights_.size()) {
      changed |= TransferPairResizeDelta(
          stackWeights_, stackIndex,
          expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
          constraints[resizedIndex], constraints[resizedIndex + 1],
          stackAxis, trailingDelta, constrainedWeightPerPixel);
    }
  }

  if (changed) DebugValidateMutation(L"Workspace::LearnMasterStackResize");
  return changed;
}

bool Workspace::LearnGridResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::LearnGridResize");
  EnsureWeights();
  const auto axis = layout_ == TileLayout::Rows ? Layout::Axis::Y : Layout::Axis::X;
  const bool alongY = axis == Layout::Axis::Y;
  const std::vector<WindowConstraints> constraints =
      Layout::ActiveWindowConstraints(*this);

  double constrainedWeightPerPixel = 0.0;
  std::vector<RECT> expectedRects;
  if (!PlanWeightedCells(*this, workArea, gap, &expectedRects, &constrainedWeightPerPixel)) {
    return false;
  }

  bool changed = false;
  const LONG leadingDelta = alongY
      ? gesture.end.top - gesture.start.top
      : gesture.end.left - gesture.start.left;
  const LONG trailingDelta = alongY
      ? gesture.end.bottom - gesture.start.bottom
      : gesture.end.right - gesture.start.right;

  if (resizedIndex > 0) {
    changed |= TransferPairResizeDelta(
        gridWeights_, resizedIndex - 1,
        expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
        constraints[resizedIndex - 1], constraints[resizedIndex],
        axis, leadingDelta, constrainedWeightPerPixel);
  }
  if (resizedIndex + 1 < windows_.size()) {
    changed |= TransferPairResizeDelta(
        gridWeights_, resizedIndex,
        expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
        constraints[resizedIndex], constraints[resizedIndex + 1],
        axis, trailingDelta, constrainedWeightPerPixel);
  }
  if (changed) DebugValidateMutation(L"Workspace::LearnGridResize");
  return changed;
}

// How far a drag of the window at `resizedIndex` (visible cell `resizedRect`)
// moved the leading side of Dwindle split `splitIndex`, along the split's axis:
// positive when the leading side grows, 0 when the window's dragged edge isn't
// this split's divider. The window at `splitIndex` is the split's leading side
// and drags its far edge; a later window touches the divider with its near edge
// when it starts where the split's remainder does. `reverse` mirrors both.
static LONG DwindleSplitDragDelta(
    const Layout::DwindleSplit& split, size_t splitIndex, size_t resizedIndex,
    const RECT& resizedRect, const MoveSizeGesture& gesture, bool reverse) {
  const bool alongX = split.axis == Layout::Axis::X;
  auto nearEdge = [alongX](const RECT& rect) { return alongX ? rect.left : rect.top; };
  auto farEdge = [alongX](const RECT& rect) { return alongX ? rect.right : rect.bottom; };
  if (resizedIndex == splitIndex) {
    return reverse ? -(nearEdge(gesture.end) - nearEdge(gesture.start))
                   : farEdge(gesture.end) - farEdge(gesture.start);
  }
  const bool touchesDivider = reverse ? farEdge(resizedRect) == farEdge(split.remaining)
                                      : nearEdge(resizedRect) == nearEdge(split.remaining);
  if (!touchesDivider) return 0;
  return reverse ? -(farEdge(gesture.end) - farEdge(gesture.start))
                 : nearEdge(gesture.end) - nearEdge(gesture.start);
}

bool Workspace::LearnDwindleResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::LearnDwindleResize");
  EnsureWeights();
  if (layout_ != TileLayout::Dwindle || windows_.size() < 2 ||
      resizedIndex >= windows_.size()) {
    return false;
  }

  // A drag that changes a divider accepts the shown layout, as size commands
  // do (layout/resize_steps.h). A gesture that changes nothing saves nothing.
  DwindleState before = SaveDwindleState();
  AcceptShownDwindleLayout(workArea, gap);

  std::vector<RECT> expectedRects;
  std::vector<Layout::DwindleSplit> splits;
  if (!PlanDwindleCells(*this, workArea, gap, &expectedRects, &splits)) {
    RestoreDwindleState(std::move(before));
    return false;
  }

  const RECT& resizedRect = expectedRects[resizedIndex];
  size_t selectedVerticalSplit = static_cast<size_t>(-1);
  size_t selectedHorizontalSplit = static_cast<size_t>(-1);
  LONG selectedVerticalDelta = 0;
  LONG selectedHorizontalDelta = 0;
  for (size_t splitIndex = 0;
       splitIndex < splits.size() && splitIndex <= resizedIndex;
       ++splitIndex) {
    const Layout::DwindleSplit& split = splits[splitIndex];
    const LONG firstSpanDelta = DwindleSplitDragDelta(
        split, splitIndex, resizedIndex, resizedRect, gesture, g_settings.reverseDwindle);
    if (!IsMeaningfulResizeDelta(firstSpanDelta) || split.usableSpan < 2) {
      continue;
    }

    // A descendant can share an edge with more than one same-axis ancestor.
    // Keep the nearest match on each axis. A corner drag may intentionally
    // update one horizontal and one vertical divider.
    if (split.axis == Layout::Axis::X) {
      selectedVerticalSplit = splitIndex;
      selectedVerticalDelta = firstSpanDelta;
    } else {
      selectedHorizontalSplit = splitIndex;
      selectedHorizontalDelta = firstSpanDelta;
    }
  }

  // The dragged divider stops where the accepted layout runs out of room, so
  // adaptive fitting never has to move another divider to make the drag fit.
  auto fits = [&] {
    std::vector<RECT> rects;
    return PlanSavedCells(*this, workArea, gap, &rects);
  };
  bool changed = false;
  auto applySelectedDivider = [&](size_t splitIndex, LONG firstSpanDelta) {
    if (splitIndex >= splits.size()) return;
    const Layout::DwindleSplit& split = splits[splitIndex];
    const LONG minimumSpan = std::min<LONG>(
        kMinLearnedResizeSpan,
        std::max<LONG>(1, (split.usableSpan - 1) / 2));
    const LONG newFirstSpan = std::clamp<LONG>(
        split.firstSpan + firstSpanDelta,
        minimumSpan, split.usableSpan - minimumSpan);
    const double newRatio = Layout::ClampSplitRatio(static_cast<double>(newFirstSpan) / static_cast<double>(split.usableSpan));
    double& ratio = dwindleRatios_[splitIndex];
    const double shownRatio = ratio;
    MoveRatioToward(ratio, newRatio, fits);
    if (Layout::SplitLeadingSpan(split.usableSpan, Layout::ClampSplitRatio(ratio)) == split.firstSpan) {
      ratio = shownRatio;  // No room: not even a pixel moved.
      return;
    }

    KeepDwindleDivider(splitIndex);  // A dragged divider stays where it was put.
    changed = true;
    Wh_Log(
        L"Dwindle divider updated slot=%zu ratio=%.6f",
        splitIndex, ratio);
    Diagnostics::RecordEvent(
        L"dwindle divider updated hwnd=%p slot=%zu ratio=%.6f",
        windows_[resizedIndex], splitIndex, ratio);
  };
  applySelectedDivider(selectedVerticalSplit, selectedVerticalDelta);
  applySelectedDivider(selectedHorizontalSplit, selectedHorizontalDelta);
  if (!changed) {
    RestoreDwindleState(std::move(before));
    return false;
  }

  DebugValidateMutation(L"Workspace::LearnDwindleResize");
  return true;
}

// The BSP layout as shown: the preferred cells, or the adaptively fitted ones
// when minimums move dividers (still the preferred ones when nothing fits).
static Layout::BspLayoutResult PlanBspCells(const Workspace& workspace, const RECT& workArea, LONG gap) {
  Layout::BspLayoutResult shown;
  Layout::LayoutBsp(workspace.Bsp(), workspace.TiledWindows(), workArea, gap, &shown);
  const std::vector<WindowConstraints> constraints = Layout::ActiveWindowConstraints(workspace);
  if (!Layout::RectsSatisfyWindowConstraints(shown.rects, constraints)) {
    Layout::BspLayoutResult fitted;
    if (Layout::FitBspLayout(workspace.Bsp(), workspace.TiledWindows(), workArea, gap, constraints,
                             Layout::GetBspAdaptation(), &fitted)) {
      shown = std::move(fitted);
    }
  }
  return shown;
}

// Accepts the BSP layout as shown, as AcceptShownDwindleLayout does: every
// divider adaptive fitting holds away from its ratio is saved where it shows.
bool Workspace::AcceptShownBspLayout(const RECT& workArea, LONG gap) {
  if (layout_ != TileLayout::Bsp) return false;
  bool changed = false;
  for (const Layout::BspSplit& split : PlanBspCells(*this, workArea, gap).splits) {
    double& ratio = bspTree_.SplitNode(split.node).ratio;
    if (split.usableSpan < 2 || Layout::SplitLeadingSpan(split.usableSpan, Layout::ClampSplitRatio(ratio)) == split.firstSpan) {
      continue;
    }
    ratio = Layout::ClampSplitRatio(static_cast<double>(split.firstSpan) / split.usableSpan);
    changed = true;
  }
  return changed;
}

// A shown split that divides a leaf from a neighbour, and the side it is on.
struct BspSplitAround {
  Layout::BspSplit split;
  bool leafFirst = false;
};

// The shown splits with `leaf` (a node index) on one side, nearest first. The
// nearest one on an axis and side is the divider along that edge of the leaf:
// any split between them has the leaf on the other side, so that edge is
// theirs too.
static std::vector<BspSplitAround> BspSplitsAround(
    const BspTree& tree, size_t leaf, const std::vector<Layout::BspSplit>& splits) {
  std::vector<BspSplitAround> around;
  if (leaf == BspTree::kNotFound) return around;
  // Splits are listed ancestors first, so the nearest is the last.
  for (auto it = splits.rbegin(); it != splits.rend(); ++it) {
    if (leaf <= it->node || leaf >= tree.SubtreeEnd(it->node)) continue;
    around.push_back({*it, leaf < tree.SecondChild(it->node)});
  }
  return around;
}

bool Workspace::LearnBspResize(
    const RECT& workArea, LONG gap, size_t resizedIndex, const MoveSizeGesture& gesture) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::LearnBspResize");
  EnsureWeights();
  if (layout_ != TileLayout::Bsp || windows_.size() < 2 || resizedIndex >= windows_.size()) return false;

  // A drag that changes a divider accepts the shown layout, as in Dwindle.
  const BspTree before = bspTree_;
  AcceptShownBspLayout(workArea, gap);
  const Layout::BspLayoutResult shown = PlanBspCells(*this, workArea, gap);
  const std::vector<BspSplitAround> around =
      BspSplitsAround(bspTree_, bspTree_.FindLeaf(windows_[resizedIndex]), shown.splits);

  struct Edge {
    Layout::Axis axis;
    bool farSide;  // Right or bottom.
    LONG delta;
  };
  const Edge edges[] = {
      {Layout::Axis::X, false, gesture.end.left - gesture.start.left},
      {Layout::Axis::X, true, gesture.end.right - gesture.start.right},
      {Layout::Axis::Y, false, gesture.end.top - gesture.start.top},
      {Layout::Axis::Y, true, gesture.end.bottom - gesture.start.bottom},
  };
  // The dragged divider stops where the accepted layout runs out of room, so
  // adaptive fitting never has to move another divider to make the drag fit.
  auto fits = [&] {
    std::vector<RECT> rects;
    return PlanSavedCells(*this, workArea, gap, &rects);
  };
  bool changed = false;
  for (const Edge& edge : edges) {
    if (!IsMeaningfulResizeDelta(edge.delta)) continue;
    // The far edge borders a split from its first side, the near edge from its second.
    const auto match = std::find_if(around.begin(), around.end(), [&](const BspSplitAround& candidate) {
      return candidate.split.axis == edge.axis && candidate.leafFirst == edge.farSide;
    });
    if (match == around.end()) continue;  // An edge of the work area.
    const Layout::BspSplit& split = match->split;
    if (split.usableSpan < 2) continue;
    const LONG minimumSpan = std::min<LONG>(kMinLearnedResizeSpan, std::max<LONG>(1, (split.usableSpan - 1) / 2));
    // Either edge moving by `delta` grows the first side by it.
    const LONG newFirstSpan = std::clamp<LONG>(split.firstSpan + edge.delta, minimumSpan, split.usableSpan - minimumSpan);
    double& ratio = bspTree_.SplitNode(split.node).ratio;
    const double saved = ratio;
    MoveRatioToward(ratio, Layout::ClampSplitRatio(static_cast<double>(newFirstSpan) / split.usableSpan), fits);
    if (Layout::SplitLeadingSpan(split.usableSpan, Layout::ClampSplitRatio(ratio)) == split.firstSpan) {
      ratio = saved;  // No room: not even a pixel moved.
      continue;
    }
    changed = true;
    Diagnostics::RecordEvent(L"bsp divider updated hwnd=%p node=%zu ratio=%.6f",
                             windows_[resizedIndex], split.node, ratio);
  }
  if (!changed) {
    bspTree_ = before;
    return false;
  }
  DebugValidateMutation(L"Workspace::LearnBspResize");
  return true;
}

} // namespace Model
