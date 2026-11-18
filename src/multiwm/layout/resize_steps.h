// Window-size commands: grow, shrink or reset the preference that sizes one tiled
// window. Part of MultiWM's single source file; see the include order in
// src/multiwm/multiwm.cpp. Uses the visible-cell helpers in layout/resize_learning.h.

namespace Model {

static LONG SpanAlong(const RECT& rect, Layout::Axis axis) {
  return axis == Layout::Axis::Y ? Layout::RectHeight(rect) : Layout::RectWidth(rect);
}

// The smallest span a step may leave a window along `axis`: its cached native
// minimum, and never less than a resize drag may leave it.
static LONG StepMinimum(const WindowConstraints& constraints, Layout::Axis axis) {
  const LONG native = !constraints.valid          ? 1
                      : axis == Layout::Axis::Y   ? constraints.minHeight
                                                  : constraints.minWidth;
  return std::max(kMinLearnedResizeSpan, native);
}

// Moves `span` by `delta` within [minimum, maximum]. A span already outside them
// never moves further out.
static LONG StepSpan(LONG span, LONG delta, LONG minimum, LONG maximum) {
  const LONG target = span + delta;
  return delta > 0 ? std::max(span, std::min(target, maximum))
                   : std::min(span, std::max(target, minimum));
}

// How ComputeWeightedSizes reads a weight.
static double EffectiveWeight(double weight) {
  return std::isfinite(weight) && weight > 0.0 ? weight : 1.0;
}

static std::optional<double> MeanOfOtherWeights(const std::vector<double>& weights, size_t index) {
  if (weights.size() < 2 || index >= weights.size()) return std::nullopt;
  double sum = 0.0;
  for (size_t i = 0; i < weights.size(); ++i) {
    if (i != index) sum += EffectiveWeight(weights[i]);
  }
  return sum / static_cast<double>(weights.size() - 1);
}

static bool SetPreference(double& preference, double value) {
  if (!std::isfinite(value) || std::abs(value - preference) <= 1e-9) return false;
  preference = value;
  return true;
}

// Moves a preference from `current` toward `end` until the window's planned
// span reaches `target` (`grow`: at least it; otherwise at most it). `spanAt`
// plans with a candidate value and has no value where no layout fits.
//
// The span is not monotone in the value: minimums make only part of the way
// feasible, and fitting can hold a window back at some values but not others
// (pushing deeper Dwindle dividers works until nothing fits, where adaptive
// fitting snaps the window back). So the way is scanned in order and the first
// sample that reaches the target is refined by bisection. If none does, the
// result is the smallest move that gets as close as any sample, so a plateau
// past it is never saved as a preference. Interpolates geometrically for
// weights, linearly for ratios. No value when no sample fits.
static std::optional<double> SolvePreference(
    double current, double end, bool geometric, bool grow, LONG target,
    const std::function<std::optional<LONG>(double)>& spanAt) {
  constexpr int kSamples = 96;
  constexpr int kRefinements = 32;
  auto valueAt = [&](double t) {
    return geometric ? current * std::pow(end / current, t) : current + (end - current) * t;
  };
  auto progress = [&](LONG span) { return grow ? span : -span; };
  // The smallest t in (low, high] whose span reaches `goal`; `high` does.
  auto refine = [&](double low, double high, LONG goal) {
    for (int i = 0; i < kRefinements; ++i) {
      const double middle = (low + high) / 2.0;
      const auto span = spanAt(valueAt(middle));
      if (span && progress(*span) >= goal) {
        high = middle;
      } else {
        low = middle;
      }
    }
    return std::pair{low, high};
  };

  const LONG goal = progress(target);
  std::optional<LONG> best;
  double bestT = 0.0;
  for (int i = 1; i <= kSamples; ++i) {
    const double t = static_cast<double>(i) / kSamples;
    const auto span = spanAt(valueAt(t));
    if (!span) continue;
    const LONG reached = progress(*span);
    if (reached >= goal) {
      const auto [low, high] = refine(static_cast<double>(i - 1) / kSamples, t, goal);
      const auto lowSpan = spanAt(valueAt(low));
      const auto highSpan = spanAt(valueAt(high));
      if (lowSpan && highSpan && std::abs(*lowSpan - target) < std::abs(*highSpan - target)) {
        return valueAt(low);
      }
      return valueAt(high);
    }
    if (!best || reached > *best) {
      best = reached;
      bestT = t;
    }
  }
  if (!best) return std::nullopt;
  return valueAt(refine(bestT - 1.0 / kSamples, bestT, *best).second);
}

// The smallest region each Dwindle subtree needs if its own dividers may move:
// along a split's axis both sides' minimums plus the gap, across it the larger.
// Index k is the subtree of windows [k, count).
static std::vector<SIZE> FlexibleDwindleMinimums(
    const std::vector<WindowConstraints>& constraints, LONG gap) {
  const size_t count = constraints.size();
  std::vector<SIZE> minimums(count);
  for (size_t k = count; k-- > 0;) {
    const Layout::ConstraintExtent own = Layout::ToConstraintExtent(constraints[k]);
    if (k + 1 == count) {
      minimums[k] = {own.minWidth, own.minHeight};
      continue;
    }
    const SIZE tail = minimums[k + 1];
    minimums[k] = (k % 2) == 0
        ? SIZE{own.minWidth + gap + tail.cx, std::max(own.minHeight, tail.cy)}
        : SIZE{std::max(own.minWidth, tail.cx), own.minHeight + gap + tail.cy};
  }
  return minimums;
}

// The ratios at Dwindle split `k` that leave both sides their minimums in the
// plain layout of `ratios` (one per active window; the last is dormant), with
// the windows after it free to move their own dividers. No value when none do.
static std::optional<std::pair<double, double>> DwindleSplitRoom(
    const std::vector<double>& ratios, size_t k, const std::vector<WindowConstraints>& constraints,
    const std::vector<SIZE>& flexible, const RECT& workArea, LONG gap) {
  const size_t count = constraints.size();
  if (k + 1 >= count) return std::nullopt;
  std::vector<RECT> rects(count);
  std::vector<Layout::DwindleSplit> splits;
  Layout::LayoutDwindle(workArea, gap, 0, count, ratios, g_settings.reverseDwindle, rects, &splits);
  if (k >= splits.size() || splits[k].usableSpan < 2) return std::nullopt;
  const LONG usable = splits[k].usableSpan;
  const bool alongX = splits[k].axis == Layout::Axis::X;
  const Layout::ConstraintExtent own = Layout::ToConstraintExtent(constraints[k]);
  const LONG first = alongX ? own.minWidth : own.minHeight;
  const LONG tail = alongX ? flexible[k + 1].cx : flexible[k + 1].cy;
  const double lowest = std::max(0.1, (first + 0.5) / usable);
  const double highest = std::min(0.9, (usable - tail - 0.5) / usable);
  if (lowest > highest) return std::nullopt;
  return std::pair{lowest, highest};
}

// The first split whose divider stays where its preference puts it: adaptive
// splits before it are fitted by the planner, which never saves the result.
static size_t FirstFixedDwindleSplit() {
  return Layout::AutomaticDwindleAdaptationEnabled() ? g_settings.adaptiveDwindleSlots : 0;
}

// Moves the fixed Dwindle dividers after `split` only as far as their windows'
// minimums require, top-down, so a window that grows pushes deeper dividers
// instead of stopping at their saved positions.
static void FitDeeperDwindleRatios(
    std::vector<double>& ratios, size_t split, const std::vector<WindowConstraints>& constraints,
    const std::vector<SIZE>& flexible, const RECT& workArea, LONG gap) {
  for (size_t k = std::max(split + 1, FirstFixedDwindleSplit()); k + 1 < constraints.size(); ++k) {
    const auto room = DwindleSplitRoom(ratios, k, constraints, flexible, workArea, gap);
    if (!room) continue;  // Nothing fits; the planner decides.
    ratios[k] = std::clamp(Layout::ClampSplitRatio(ratios[k]), room->first, room->second);
  }
}

bool Workspace::IsDwindlePushCurrent(const DwindlePush& push) const {
  if (layout_ != TileLayout::Dwindle || push.split + 1 >= windows_.size() ||
      windows_[push.split] != push.owner) {
    return false;
  }
  const WindowRecord* owner = Find(push.owner);
  return owner && owner->tilingGeneration == push.ownerGeneration;
}

// Remembers the earlier ratio of every divider other than `ownSplit` that
// differs from `before`. A divider pushed again keeps its first ratio.
void Workspace::RecordDwindlePushes(const std::vector<double>& before, size_t ownSplit) {
  std::erase_if(dwindlePushes_, [&](const DwindlePush& push) { return !IsDwindlePushCurrent(push); });
  for (size_t k = 0; k + 1 < windows_.size() && k < before.size(); ++k) {
    if (k == ownSplit || std::abs(dwindleRatios_[k] - before[k]) <= 1e-9) continue;
    const bool known = std::any_of(dwindlePushes_.begin(), dwindlePushes_.end(),
                                   [&](const DwindlePush& push) { return push.split == k; });
    if (known) continue;
    const WindowRecord* owner = Find(windows_[k]);
    dwindlePushes_.push_back({k, windows_[k], owner ? owner->tilingGeneration : 0, before[k]});
  }
}

// The user placed this divider; it no longer springs back.
void Workspace::KeepDwindleDivider(size_t split) {
  std::erase_if(dwindlePushes_, [&](const DwindlePush& push) { return push.split == split; });
}

// Lets dividers after split `afterSplit` that a step pushed move back toward
// their earlier ratios, top-down, as far as the layout still plans and the
// window at `keepIndex` (the one the user is acting on) keeps its rectangle.
// A divider that gets all the way back is forgotten. True when a ratio changed.
bool Workspace::RelaxDwindlePushes(const RECT& workArea, LONG gap, size_t afterSplit, size_t keepIndex) {
  std::erase_if(dwindlePushes_, [&](const DwindlePush& push) { return !IsDwindlePushCurrent(push); });
  if (dwindlePushes_.empty() || keepIndex >= windows_.size()) return false;
  std::sort(dwindlePushes_.begin(), dwindlePushes_.end(),
            [](const DwindlePush& a, const DwindlePush& b) { return a.split < b.split; });
  std::vector<RECT> rects;
  if (Layout::BuildWorkspaceLayoutPlan(*this, workArea, gap, &rects) != Layout::LayoutPlanStatus::Success) {
    return false;
  }
  const RECT kept = rects[keepIndex];
  auto acceptable = [&] {
    std::vector<RECT> planned;
    return Layout::BuildWorkspaceLayoutPlan(*this, workArea, gap, &planned) == Layout::LayoutPlanStatus::Success &&
           EqualRect(&planned[keepIndex], &kept);
  };

  bool changed = false;
  std::vector<size_t> restored;
  for (const DwindlePush& push : dwindlePushes_) {
    if (push.split <= afterSplit) continue;
    double& ratio = dwindleRatios_[push.split];
    const double from = ratio;
    ratio = push.ratio;
    if (acceptable()) {
      restored.push_back(push.split);
    } else {
      double low = 0.0;  // Fractions of the way back; `from` is acceptable.
      double high = 1.0;
      for (int i = 0; i < 24; ++i) {
        const double middle = (low + high) / 2.0;
        ratio = from + (push.ratio - from) * middle;
        if (acceptable()) {
          low = middle;
        } else {
          high = middle;
        }
      }
      ratio = from + (push.ratio - from) * low;
    }
    changed |= std::abs(ratio - from) > 1e-9;
  }
  for (size_t split : restored) KeepDwindleDivider(split);
  return changed;
}

bool Workspace::StepTiledSize(
    const RECT& workArea, LONG gap, size_t index, bool grow, LONG stepPercent) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::StepTiledSize");
  EnsureWeights();
  const size_t count = windows_.size();
  if (count < 2 || index >= count || stepPercent <= 0) return false;

  // Visible cells of `workspace`, which has this layout and these windows.
  auto plan = [&](const Workspace& workspace, std::vector<RECT>* rects,
                  std::vector<Layout::DwindleSplit>* splits) {
    if (IsGridLayout(layout_)) return PlanGridCells(workspace, workArea, gap, rects);
    if (IsMasterStackLayout(layout_)) return PlanMasterStackCells(workspace, workArea, gap, rects);
    if (layout_ == TileLayout::Dwindle) return PlanDwindleCells(workspace, workArea, gap, rects, splits);
    return false;
  };
  std::vector<RECT> rects;
  std::vector<Layout::DwindleSplit> splits;
  if (!plan(*this, &rects, &splits)) return false;
  const std::vector<WindowConstraints> constraints = Layout::ActiveWindowConstraints(*this);

  // What sizes the window: which preference, along which axis, in which
  // direction, and how far its span may go.
  Layout::Axis axis = Layout::LayoutAxis(layout_);
  std::function<double&(Workspace&)> preference;
  bool increasing = true;
  bool geometric = false;
  double low = 0.1;  // Ratios share ClampSplitRatio's range.
  double high = 0.9;
  LONG minimum = 0;
  LONG maximum = 0;
  // A ratio starts from its visible position, which minimums can hold away
  // from the saved one, as a resize drag does.
  std::optional<double> visibleStart;
  // Dwindle: pushes deeper dividers as far as the growing window needs.
  std::function<void(Workspace&)> fitDeeper;
  std::optional<size_t> dwindleSplit;  // The split the step moves.
  // Bounds and a weight range for the window among peers [first, count).
  auto usePeers = [&](std::vector<double>& weights, size_t first) {
    LONG available = 0;
    LONG othersMinimum = 0;
    for (size_t i = first; i < count; ++i) {
      available += SpanAlong(rects[i], axis);
      if (i != index) othersMinimum += StepMinimum(constraints[i], axis);
    }
    minimum = StepMinimum(constraints[index], axis);
    maximum = available - othersMinimum;
    const double mean = MeanOfOtherWeights(weights, index - first).value_or(1.0);
    low = mean * 1e-3;
    high = mean * 1e3;
    geometric = true;
  };

  if (IsGridLayout(layout_)) {
    usePeers(gridWeights_, 0);
    preference = [index](Workspace& w) -> double& { return w.gridWeights_[index]; };
  } else if (IsMasterStackLayout(layout_) && index > 0 && count > 2) {
    axis = axis == Layout::Axis::X ? Layout::Axis::Y : Layout::Axis::X;
    usePeers(stackWeights_, 1);
    preference = [index](Workspace& w) -> double& { return w.stackWeights_[index - 1]; };
  } else if (IsMasterStackLayout(layout_)) {
    // The master divider sizes the master and a lone stack window alike.
    const LONG totalSpan = SpanAlong(workArea, axis);
    if (totalSpan < 2) return false;
    const LONG usableSpan = totalSpan - std::clamp<LONG>(gap, 0, totalSpan - 2);
    LONG stackMinimum = 0;
    for (size_t i = 1; i < count; ++i) {
      stackMinimum = std::max(stackMinimum, StepMinimum(constraints[i], axis));
    }
    const LONG masterMinimum = StepMinimum(constraints[0], axis);
    increasing = index == 0;
    minimum = increasing ? masterMinimum : stackMinimum;
    maximum = usableSpan - (increasing ? stackMinimum : masterMinimum);
    visibleStart = Layout::ClampSplitRatio(
        static_cast<double>(SpanAlong(rects[0], axis)) / static_cast<double>(usableSpan));
    preference = [](Workspace& w) -> double& { return w.masterRatio_; };
  } else if (layout_ == TileLayout::Dwindle) {
    // A window is the first side of its own split; the last one is the second
    // side of the split before it.
    increasing = index + 1 < count;
    const size_t splitIndex = increasing ? index : count - 2;
    dwindleSplit = splitIndex;
    const Layout::DwindleSplit& split = splits[splitIndex];
    if (split.usableSpan < 2) return false;
    axis = split.axis;
    minimum = StepMinimum(constraints[index], axis);
    // Adaptive splits can widen this split's region when its other side needs
    // room, so its current size is no limit; the solver finds the real one.
    maximum = SpanAlong(workArea, axis);
    visibleStart = Layout::ClampSplitRatio(
        static_cast<double>(split.firstSpan) / static_cast<double>(split.usableSpan));
    preference = [splitIndex](Workspace& w) -> double& { return w.dwindleRatios_[splitIndex]; };
    // Only growing a first side squeezes the windows after it.
    if (increasing && grow) {
      fitDeeper = [&, splitIndex, flexible = FlexibleDwindleMinimums(constraints, gap)](Workspace& w) {
        FitDeeperDwindleRatios(w.dwindleRatios_, splitIndex, constraints, flexible, workArea, gap);
      };
    }
  } else {
    return false;  // Monocle.
  }

  const LONG span = SpanAlong(rects[index], axis);
  const LONG step = std::max<LONG>(
      1, MulDiv(SpanAlong(workArea, axis), static_cast<int>(stepPercent), 100));
  const LONG target = StepSpan(span, grow ? step : -step, minimum, maximum);
  if (target == span) return false;

  Workspace probe = *this;
  const std::vector<double> savedDwindleRatios = dwindleRatios_;
  // Sets the preference (and pushes deeper dividers) on `workspace`.
  auto apply = [&](Workspace& workspace, double value) {
    if (fitDeeper) workspace.dwindleRatios_ = savedDwindleRatios;
    preference(workspace) = value;
    if (fitDeeper) fitDeeper(workspace);
  };
  auto spanAt = [&](double value) -> std::optional<LONG> {
    apply(probe, value);
    std::vector<RECT> probeRects;
    std::vector<Layout::DwindleSplit> probeSplits;
    if (!plan(probe, &probeRects, &probeSplits)) return std::nullopt;
    return SpanAlong(probeRects[index], axis);
  };
  // Grow moves the preference the way that enlarges the window.
  const double current = visibleStart ? *visibleStart
                         : geometric  ? EffectiveWeight(preference(probe))
                                      : preference(probe);
  const double end = grow == increasing ? high : low;
  const auto value = SolvePreference(current, end, geometric, grow, target, spanAt);
  if (!value || !std::isfinite(*value)) return false;
  const auto reached = spanAt(*value);
  if (!reached || (grow ? *reached <= span : *reached >= span)) return false;
  // Like a drag, the step saves the size it shows: a squeeze that held the
  // window elsewhere does not bring an older preference back when it ends.
  apply(*this, *value);
  if (dwindleSplit) {
    // The stepped divider is the user's choice now. Growing remembers what it
    // pushed; shrinking frees room for pushed dividers after it.
    KeepDwindleDivider(*dwindleSplit);
    if (fitDeeper) {
      RecordDwindlePushes(savedDwindleRatios, *dwindleSplit);
    } else {
      RelaxDwindlePushes(workArea, gap, *dwindleSplit, index);
    }
  }
  DebugValidateMutation(L"Workspace::StepTiledSize");
  return true;
}

bool Workspace::ResetTiledSize(const RECT& workArea, LONG gap, size_t index, double defaultMasterRatio) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ResetTiledSize");
  EnsureWeights();
  const size_t count = windows_.size();
  if (count < 2 || index >= count) return false;

  bool changed = false;
  if (IsGridLayout(layout_)) {
    const auto mean = MeanOfOtherWeights(gridWeights_, index);
    changed = mean && SetPreference(gridWeights_[index], *mean);
  } else if (IsMasterStackLayout(layout_)) {
    if (index == 0 || count == 2) {
      changed = SetPreference(masterRatio_, Layout::ClampSplitRatio(defaultMasterRatio));
    } else {
      const auto mean = MeanOfOtherWeights(stackWeights_, index - 1);
      changed = mean && SetPreference(stackWeights_[index - 1], *mean);
    }
  } else if (layout_ == TileLayout::Dwindle) {
    const size_t split = std::min(index, count - 2);
    changed = SetPreference(dwindleRatios_[split], 0.5);
    KeepDwindleDivider(split);
    changed |= RelaxDwindlePushes(workArea, gap, split, index);
  }

  if (changed) DebugValidateMutation(L"Workspace::ResetTiledSize");
  return changed;
}

}  // namespace Model
