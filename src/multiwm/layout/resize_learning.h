// Learns divider and weight preferences from completed user resize gestures.
// Turns a before/after frame pair into updated ratios or weights on the Workspace.

namespace Model {

constexpr LONG kResizeEdgeDeltaTolerance = 1;
constexpr LONG kMinLearnedResizeSpan = 80;

static bool IsMeaningfulResizeDelta(LONG delta) {
  return std::abs(delta) > kResizeEdgeDeltaTolerance;
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
  Layout::LayoutMasterStackWeighted(
      workArea, gap, windows_.size(), expectedRects, axis,
      Layout::ClampSplitRatio(masterRatio_), stackWeights_);
  if (!Layout::RectsSatisfyWindowConstraints(expectedRects, constraints)) {
    std::vector<RECT> constrainedRects;
    if (Layout::LayoutMasterStackConstrained(
            workArea, gap, axis,
            Layout::ClampSplitRatio(masterRatio_), stackWeights_,
            constraints, &constrainedRects, &constrainedWeightPerPixel)) {
      expectedRects = std::move(constrainedRects);
    }
  }
  if (expectedRects.size() != windows_.size()) return false;

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
  Layout::LayoutGridWeighted(
      workArea, gap, windows_.size(), expectedRects,
      axis, gridWeights_);
  if (!Layout::RectsSatisfyWindowConstraints(expectedRects, constraints)) {
    std::vector<RECT> constrainedRects;
    if (Layout::LayoutGridConstrained(
            workArea, gap, gridWeights_, constraints,
            axis, &constrainedRects, &constrainedWeightPerPixel)) {
      expectedRects = std::move(constrainedRects);
    }
  }
  if (expectedRects.size() != windows_.size()) return false;

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

bool Workspace::LearnDwindleResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::LearnDwindleResize");
  EnsureWeights();
  if (layout_ != TileLayout::Dwindle || windows_.size() < 2 ||
      resizedIndex >= windows_.size()) {
    return false;
  }

  std::vector<RECT> expectedRects(windows_.size());
  std::vector<Layout::DwindleSplit> splits;
  Layout::LayoutDwindle(
      workArea, gap, 0, windows_.size(), dwindleRatios_,
      g_settings.reverseDwindle, expectedRects, &splits);
  if (!Layout::RectsSatisfyWindowConstraints(
          expectedRects, Layout::ActiveWindowConstraints(*this)) &&
      !Layout::BuildAdaptiveDwindleLayout(
          *this, workArea, gap, &expectedRects, &splits)) {
    return false;
  }
  if (splits.size() + 1 != dwindleRatios_.size()) return false;

  const RECT& resizedRect = expectedRects[resizedIndex];
  size_t selectedVerticalSplit = static_cast<size_t>(-1);
  size_t selectedHorizontalSplit = static_cast<size_t>(-1);
  LONG selectedVerticalDelta = 0;
  LONG selectedHorizontalDelta = 0;
  for (size_t splitIndex = 0;
       splitIndex < splits.size() && splitIndex <= resizedIndex;
       ++splitIndex) {
    const Layout::DwindleSplit& split = splits[splitIndex];
    LONG firstSpanDelta = 0;
    if (split.axis == Layout::Axis::X) {
      if (resizedIndex == splitIndex) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.left - gesture.start.left)
            : gesture.end.right - gesture.start.right;
      } else if ((!g_settings.reverseDwindle &&
                  resizedRect.left == split.remaining.left) ||
                 (g_settings.reverseDwindle &&
                  resizedRect.right == split.remaining.right)) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.right - gesture.start.right)
            : gesture.end.left - gesture.start.left;
      }
    } else {
      if (resizedIndex == splitIndex) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.top - gesture.start.top)
            : gesture.end.bottom - gesture.start.bottom;
      } else if ((!g_settings.reverseDwindle &&
                  resizedRect.top == split.remaining.top) ||
                 (g_settings.reverseDwindle &&
                  resizedRect.bottom == split.remaining.bottom)) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.bottom - gesture.start.bottom)
            : gesture.end.top - gesture.start.top;
      }
    }
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
    if (std::abs(newRatio - dwindleRatios_[splitIndex]) <= 1e-9) return;

    dwindleRatios_[splitIndex] = newRatio;
    changed = true;
    Wh_Log(
        L"Dwindle divider updated slot=%zu ratio=%.6f",
        splitIndex, newRatio);
    Diagnostics::RecordEvent(
        L"dwindle divider updated hwnd=%p slot=%zu ratio=%.6f",
        windows_[resizedIndex], splitIndex, newRatio);
  };
  applySelectedDivider(selectedVerticalSplit, selectedVerticalDelta);
  applySelectedDivider(selectedHorizontalSplit, selectedHorizontalDelta);
  if (!changed) return false;

  DebugValidateMutation(L"Workspace::LearnDwindleResize");
  return true;
}

} // namespace Model
