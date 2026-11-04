// Model-aware layout planning and constraint candidate searches.
// Implementation/interface fragment assembled by src/multiwm.cpp in dependency order.

namespace Layout {

struct AdmissionRecoveryPlan {
  Workspace workspace;
  HWND window = nullptr;
  size_t fromIndex = 0;
  size_t toIndex = 0;
  size_t reassignedWindows = 0;
};

struct ConstraintCandidate {
  HWND hwnd;
  uint64_t generation;
};

struct ConstraintSlotCompatibility {
  std::vector<std::vector<size_t>> slotsByWindow;
};

enum class LayoutPlanStatus {
  Success,
  NotApplicable,
  Unsatisfiable,
};

// Range of ratios an adaptive Dwindle split may temporarily move to, e.g. 0.35..0.65.
struct AdaptiveDwindleRatioEnvelope {
  double minimum = 0.1;
  double maximum = 0.9;

  // Smallest and largest leading-side size, in pixels, inside this envelope.
  std::pair<LONG, LONG> SpanBounds(LONG usable) const {
    return {SplitLeadingSpan(usable, minimum), SplitLeadingSpan(usable, maximum)};
  }
};

static std::vector<WindowConstraints> ActiveWindowConstraints(
    const Workspace& workspace) {
  std::vector<WindowConstraints> constraints;
  constraints.reserve(workspace.ActiveCount());
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    constraints.push_back(record ? record->constraints : WindowConstraints{});
  }
  return constraints;
}

// True when `slot` is at least the window's reported minimum size. A window
// without a valid minimum-size report fits any slot.
//
// This is the single fit predicate for the planner: admission recovery,
// conflict reporting and plan validation all use it so they can never disagree
// about whether a slot is usable.
static bool WindowConstraintFitsSlot(
    const WindowConstraints& constraint, const RECT& slot) {
  return !constraint.valid ||
      (RectWidth(slot) >= std::max<LONG>(1, constraint.minWidth) &&
       RectHeight(slot) >= std::max<LONG>(1, constraint.minHeight));
}

// True when every rectangle fits the window assigned to the same index.
static bool RectsSatisfyWindowConstraints(
    const std::vector<RECT>& rects,
    const std::vector<WindowConstraints>& constraints) {
  if (rects.size() != constraints.size()) return false;
  for (size_t i = 0; i < rects.size(); ++i) {
    if (!WindowConstraintFitsSlot(constraints[i], rects[i])) return false;
  }
  return true;
}

static bool AutomaticDwindleAdaptationEnabled() {
  return g_settings.adaptiveDwindleSlots > 0 &&
      g_settings.adaptiveDwindlePercentage > 0;
}

static AdaptiveDwindleRatioEnvelope GetAdaptiveDwindleRatioEnvelope() {
  const double deviation =
      static_cast<double>(std::clamp<LONG>(
          g_settings.adaptiveDwindlePercentage, 0, 40)) /
      100.0;
  return {0.5 - deviation, 0.5 + deviation};
}

// Calculate temporary geometry from the saved divider preferences. Only the
// configured leading splits may adapt; deeper ratios and window order stay fixed.
// A feasible manual ratio outside the automatic envelope remains unchanged.
// Never write fitted ratios back to the workspace: removing a constraint should
// let the divider return to its preferred position.
static bool BuildAdaptiveDwindleLayout(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects,
    std::vector<DwindleSplit>* outSplits = nullptr) {
  if (workspace.Layout() != TileLayout::Dwindle || workspace.ActiveCount() < 2 ||
      !AutomaticDwindleAdaptationEnabled()) {
    return false;
  }

  const auto& windows = workspace.TiledWindows();
  const auto constraints = ActiveWindowConstraints(workspace);
  std::vector<double> ratios = workspace.DwindleRatios();
  const size_t adaptiveSplitCount =
      std::min(g_settings.adaptiveDwindleSlots, ratios.size());
  if (adaptiveSplitCount == 0) return false;
  const AdaptiveDwindleRatioEnvelope adaptiveEnvelope =
      GetAdaptiveDwindleRatioEnvelope();

  std::vector<SIZE> windowMinimums(windows.size(), SIZE{1, 1});
  for (size_t i = 0; i < constraints.size(); ++i) {
    const ConstraintExtent minimum = ToConstraintExtent(constraints[i]);
    windowMinimums[i] = {minimum.minWidth, minimum.minHeight};
  }

  const LONG maximumSpan = std::max(RectWidth(workArea), RectHeight(workArea));
  auto requiredSpan = [&](LONG first, LONG tail, size_t splitIndex,
                          bool adaptive) {
    const double fixedRatio = ClampSplitRatio(ratios[splitIndex]);
    auto fits = [&](LONG span) {
      if (span < 2) return false;
      const LONG usable = UsableSplitSpan(span, gap);
      if (usable < 2) return false;

      // A manually chosen ratio outside the adaptive envelope remains a legal
      // no-op if it already satisfies both sides. Only a ratio we actually need
      // to change must land inside the configured symmetric envelope.
      const LONG currentAllocated = SplitLeadingSpan(usable, fixedRatio);
      if (currentAllocated >= first && usable - currentAllocated >= tail) {
        return true;
      }
      if (!adaptive) return false;

      const auto [minimumAdaptive, maximumAdaptive] = adaptiveEnvelope.SpanBounds(usable);
      const LONG lower = std::max(first, minimumAdaptive);
      const LONG upper = std::min(usable - tail, maximumAdaptive);
      return lower <= upper;
    };

    if (!fits(maximumSpan)) return maximumSpan + 1;
    LONG low = 2;
    LONG high = maximumSpan;
    while (low < high) {
      const LONG middle = low + (high - low) / 2;
      if (fits(middle)) {
        high = middle;
      } else {
        low = middle + 1;
      }
    }
    return low;
  };

  // Minimum extent of each recursive tail. Fixed descendants preserve their
  // current ratios; adaptive descendants contribute only the space they need
  // somewhere inside the configured adaptive ratio envelope.
  std::vector<SIZE> subtreeMinimums = windowMinimums;
  for (size_t splitIndex = windows.size() - 1; splitIndex-- > 0;) {
    const SIZE first = windowMinimums[splitIndex];
    const SIZE tail = subtreeMinimums[splitIndex + 1];
    SIZE combined{
        std::max(first.cx, tail.cx),
        std::max(first.cy, tail.cy)};
    const bool adaptive = splitIndex < adaptiveSplitCount;
    if ((splitIndex % 2) == 0) {
      combined.cx = requiredSpan(first.cx, tail.cx, splitIndex, adaptive);
    } else {
      combined.cy = requiredSpan(first.cy, tail.cy, splitIndex, adaptive);
    }
    subtreeMinimums[splitIndex] = combined;
  }

  if (RectWidth(workArea) < subtreeMinimums[0].cx ||
      RectHeight(workArea) < subtreeMinimums[0].cy) {
    return false;
  }

  std::vector<RECT> rects(windows.size());
  std::vector<DwindleSplit> splits;
  RECT remaining = workArea;
  for (size_t splitIndex = 0; splitIndex < adaptiveSplitCount; ++splitIndex) {
    const bool vertical = (splitIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(remaining) : RectHeight(remaining);
    if (span < 2) return false;
    const LONG usable = UsableSplitSpan(span, gap);
    if (usable < 2) return false;

    const LONG firstMinimum = vertical
        ? windowMinimums[splitIndex].cx
        : windowMinimums[splitIndex].cy;
    const LONG tailMinimum = vertical
        ? subtreeMinimums[splitIndex + 1].cx
        : subtreeMinimums[splitIndex + 1].cy;
    const LONG desired = SplitLeadingSpan(usable, ClampSplitRatio(ratios[splitIndex]));
    LONG firstSpan = desired;
    if (desired < firstMinimum || usable - desired < tailMinimum) {
      const auto [minimumAdaptive, maximumAdaptive] = adaptiveEnvelope.SpanBounds(usable);
      const LONG lower = std::max(firstMinimum, minimumAdaptive);
      const LONG upper = std::min(usable - tailMinimum, maximumAdaptive);
      if (lower > upper) return false;
      firstSpan = std::clamp<LONG>(desired, lower, upper);
    }
    ratios[splitIndex] = static_cast<double>(firstSpan) / usable;
    LayoutDwindle(
        workArea, gap, 0, windows.size(), ratios,
        g_settings.reverseDwindle, rects, &splits);
    if (splitIndex >= splits.size()) return false;
    remaining = splits[splitIndex].remaining;
  }

  if (!RectsSatisfyWindowConstraints(rects, constraints)) return false;
  *outRects = std::move(rects);
  if (outSplits) *outSplits = std::move(splits);
  return true;
}

// Uses cached constraints to calculate physical screen rectangles without native
// window queries or workspace mutation. Output is usable only on Success; other
// outcomes may leave partial geometry, which callers must discard.
static LayoutPlanStatus BuildWorkspaceLayoutPlan(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects) {
  if (!outRects) return LayoutPlanStatus::Unsatisfiable;
  const TileLayout layout = workspace.Layout();
  if (layout == TileLayout::Floating || layout == TileLayout::COUNT) {
    outRects->clear();
    return LayoutPlanStatus::NotApplicable;
  }

  const size_t count = workspace.ActiveCount();
  const Axis axis = LayoutAxis(layout);
  const double masterRatio = ClampSplitRatio(workspace.MasterRatio());
  outRects->assign(count, RECT{});

  // Step 1: the ordinary layout from saved preferences. When every window's
  // minimum size already fits, this is the result.
  if (IsMasterStackLayout(layout)) {
    LayoutMasterStackWeighted(workArea, gap, count, *outRects, axis, masterRatio,
                              workspace.StackWeights());
  } else if (IsGridLayout(layout)) {
    LayoutGridWeighted(workArea, gap, count, *outRects, axis, workspace.GridWeights());
  } else if (layout == TileLayout::Dwindle) {
    LayoutDwindle(workArea, gap, 0, count, workspace.DwindleRatios(),
                  g_settings.reverseDwindle, *outRects);
  } else {  // Monocle
    outRects->assign(count, workArea);
  }

  const std::vector<WindowConstraints> constraints = ActiveWindowConstraints(workspace);
  if (RectsSatisfyWindowConstraints(*outRects, constraints)) {
    return LayoutPlanStatus::Success;
  }

  // Step 2: some window is too small. Re-solve with minimum sizes as hard limits.
  // Saved preferences are not changed, so dividers return once room is available.
  bool fitted = false;
  if (IsMasterStackLayout(layout)) {
    fitted = LayoutMasterStackConstrained(workArea, gap, axis, masterRatio,
                                          workspace.StackWeights(), constraints, outRects);
  } else if (IsGridLayout(layout)) {
    fitted = LayoutGridConstrained(workArea, gap, workspace.GridWeights(), constraints,
                                   axis, outRects);
  } else if (layout == TileLayout::Dwindle) {
    fitted = BuildAdaptiveDwindleLayout(workspace, workArea, gap, outRects);
  } else {  // Monocle
    fitted = LayoutMonocleConstrained(workArea, constraints, outRects);
  }
  return fitted ? LayoutPlanStatus::Success : LayoutPlanStatus::Unsatisfiable;
}

static HWND FindConstraintCulprit(
    const Workspace& workspace, const std::vector<RECT>& rects,
    HWND preferred = nullptr) {
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  auto violatesAssignedRect = [&](size_t index) {
    return index < constraints.size() && index < rects.size() &&
        !WindowConstraintFitsSlot(constraints[index], rects[index]);
  };

  const size_t preferredIndex = workspace.TiledIndex(preferred);
  if (violatesAssignedRect(preferredIndex)) return preferred;
  for (size_t i = 0; i < constraints.size(); ++i) {
    if (violatesAssignedRect(i)) return workspace.TiledWindows()[i];
  }
  return nullptr;
}

// For each window, lists the slots it fits (see WindowConstraintFitsSlot).
// With preferNearbySlots, each list is ordered closest-to-current-position first.
// Admission recovery and conflict reporting share this and the augmenting-path
// matcher below, so diagnostics cannot disagree with the solver.
static ConstraintSlotCompatibility BuildConstraintSlotCompatibility(
    const std::vector<WindowConstraints>& constraints,
    const std::vector<RECT>& slots, bool preferNearbySlots) {
  ConstraintSlotCompatibility compatibility;
  compatibility.slotsByWindow.resize(constraints.size());
  for (size_t windowIndex = 0; windowIndex < constraints.size(); ++windowIndex) {
    auto& compatibleSlots = compatibility.slotsByWindow[windowIndex];
    for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
      if (WindowConstraintFitsSlot(constraints[windowIndex], slots[slotIndex])) {
        compatibleSlots.push_back(slotIndex);
      }
    }
    if (preferNearbySlots) SortByDistanceFrom(compatibleSlots, windowIndex);
  }
  return compatibility;
}

static bool TryAssignConstraintWindow(
    size_t candidateWindow,
    const ConstraintSlotCompatibility& compatibility,
    std::vector<size_t>* slotOwner, const std::vector<bool>& fixedWindows,
    std::vector<bool>* visitedSlots, bool preferFreeSlots) {
  if (!slotOwner || !visitedSlots ||
      candidateWindow >= compatibility.slotsByWindow.size()) {
    return false;
  }
  constexpr size_t kUnassigned = static_cast<size_t>(-1);
  const auto& compatibleSlots = compatibility.slotsByWindow[candidateWindow];

  if (preferFreeSlots) {
    for (size_t slotIndex : compatibleSlots) {
      if (slotIndex >= slotOwner->size() || (*visitedSlots)[slotIndex] ||
          (*slotOwner)[slotIndex] != kUnassigned) {
        continue;
      }
      (*visitedSlots)[slotIndex] = true;
      (*slotOwner)[slotIndex] = candidateWindow;
      return true;
    }
  }

  for (size_t slotIndex : compatibleSlots) {
    if (slotIndex >= slotOwner->size() || (*visitedSlots)[slotIndex]) continue;
    (*visitedSlots)[slotIndex] = true;
    const size_t displacedWindow = (*slotOwner)[slotIndex];
    if (displacedWindow == kUnassigned ||
        (displacedWindow < fixedWindows.size() &&
         !fixedWindows[displacedWindow] &&
         TryAssignConstraintWindow(
             displacedWindow, compatibility, slotOwner, fixedWindows,
             visitedSlots, preferFreeSlots))) {
      (*slotOwner)[slotIndex] = candidateWindow;
      return true;
    }
  }
  return false;
}

// Applies an already-decided active-slot permutation without changing membership.
// Suspended logical slots remain anchored; swapping active leaves changes only which
// active window occupies each currently available logical position.
static bool ApplyTiledWindowOrder(
    Workspace& workspace, const std::vector<HWND>& desiredOrder) {
  if (desiredOrder.size() != workspace.ActiveCount()) return false;
  for (size_t slotIndex = 0; slotIndex < desiredOrder.size(); ++slotIndex) {
    const HWND current = workspace.TiledWindows()[slotIndex];
    if (current != desiredOrder[slotIndex] &&
        !workspace.SwapTiled(current, desiredOrder[slotIndex])) {
      return false;
    }
  }
  return true;
}

// Layout conversion has different semantics from an explicit order mutation.
// Preserve the existing order first (the caller already tried that), but if the
// new Dwindle geometry cannot host that order, find a compatible permutation.
//
// Adaptive prefix splits are searched jointly with assignment: for a candidate
// occupant we allocate the smallest legal span that satisfies that window, which
// maximizes the recursive tail. With minimum-only constraints, any larger tail is
// never worse, so failure with this maximal tail proves that occupant cannot lead
// to a feasible continuation. Once the adaptive prefix ends, ordinary bipartite
// matching solves the remaining fixed-geometry tail in polynomial time.
static bool ReassignDwindleLayoutConversionForConstraints(
    Workspace& workspace, const RECT& workArea, LONG gap,
    size_t* outMovedWindowCount) {
  if (workspace.Layout() != TileLayout::Dwindle || workspace.ActiveCount() < 2) {
    return false;
  }

  const std::vector<HWND> windows = workspace.TiledWindows();
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  const std::vector<double> ratios = workspace.DwindleRatios();
  const size_t count = windows.size();
  if (constraints.size() != count || ratios.size() + 1 < count) return false;

  const size_t adaptiveSplitCount = AutomaticDwindleAdaptationEnabled()
      ? std::min(
            g_settings.adaptiveDwindleSlots,
            count > 0 ? count - 1 : size_t{0})
      : 0;
  const AdaptiveDwindleRatioEnvelope adaptiveEnvelope =
      GetAdaptiveDwindleRatioEnvelope();
  std::vector<bool> used(count, false);
  std::vector<HWND> desiredOrder(count, nullptr);
  uint64_t usedMask = 0;
  size_t searchNodes = 0;
  bool searchLimitReached = false;
  constexpr size_t kMaxSearchNodes = 250000;

  struct SearchKey {
    uint64_t usedMask = 0;
    uint32_t slotIndex = 0;
    LONG width = 0;
    LONG height = 0;
    bool operator==(const SearchKey& other) const noexcept {
      return usedMask == other.usedMask && slotIndex == other.slotIndex &&
          width == other.width && height == other.height;
    }
  };
  struct SearchKeyHash {
    size_t operator()(const SearchKey& key) const noexcept {
      size_t value = std::hash<uint64_t>{}(key.usedMask);
      value ^= std::hash<uint32_t>{}(key.slotIndex) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      value ^= std::hash<LONG>{}(key.width) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      value ^= std::hash<LONG>{}(key.height) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      return value;
    }
  };
  std::unordered_set<SearchKey, SearchKeyHash> deadStates;
  const bool canMemoize = count <= 64;

  auto makeSplit = [&](const RECT& area, size_t slotIndex, LONG firstSpan,
                       RECT* first, RECT* remaining) {
    if (!first || !remaining) return false;
    const bool vertical = (slotIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(area) : RectHeight(area);
    if (span < 2) return false;
    const LONG usable = UsableSplitSpan(span, gap);
    const LONG effectiveGap = span - usable;
    if (firstSpan < 1 || firstSpan >= usable) return false;

    *remaining = area;
    if (vertical) {
      if (g_settings.reverseDwindle) {
        *first = {area.right - firstSpan, area.top, area.right, area.bottom};
        remaining->right = first->left - effectiveGap;
      } else {
        *first = {area.left, area.top, area.left + firstSpan, area.bottom};
        remaining->left = first->right + effectiveGap;
      }
    } else {
      if (g_settings.reverseDwindle) {
        *first = {area.left, area.bottom - firstSpan, area.right, area.bottom};
        remaining->bottom = first->top - effectiveGap;
      } else {
        *first = {area.left, area.top, area.right, area.top + firstSpan};
        remaining->top = first->bottom + effectiveGap;
      }
    }
    return RectWidth(*remaining) > 0 && RectHeight(*remaining) > 0;
  };

  auto matchFixedTail = [&](size_t slotIndex, const RECT& area) {
    const size_t tailCount = count - slotIndex;
    if (tailCount == 0) return true;

    std::vector<size_t> remainingWindows;
    remainingWindows.reserve(tailCount);
    for (size_t windowIndex = 0; windowIndex < count; ++windowIndex) {
      if (!used[windowIndex]) remainingWindows.push_back(windowIndex);
    }
    if (remainingWindows.size() != tailCount) return false;

    std::vector<RECT> allSlots(count);
    LayoutDwindle(
        area, gap, slotIndex, tailCount, ratios,
        g_settings.reverseDwindle, allSlots);
    std::vector<RECT> tailSlots(
        allSlots.begin() + slotIndex, allSlots.end());
    std::vector<WindowConstraints> tailConstraints;
    tailConstraints.reserve(tailCount);
    for (size_t windowIndex : remainingWindows) {
      tailConstraints.push_back(constraints[windowIndex]);
    }
    const ConstraintSlotCompatibility compatibility =
        BuildConstraintSlotCompatibility(tailConstraints, tailSlots, true);
    for (const auto& compatibleSlots : compatibility.slotsByWindow) {
      if (compatibleSlots.empty()) return false;
    }

    constexpr size_t kUnassigned = static_cast<size_t>(-1);
    std::vector<size_t> slotOwner(tailCount, kUnassigned);
    std::vector<bool> fixed(tailCount, false);

    // Seed surviving exact positions first so a layout conversion disturbs as
    // little order as possible before augmenting paths are allowed to move them.
    for (size_t localWindow = 0; localWindow < tailCount; ++localWindow) {
      const size_t originalIndex = remainingWindows[localWindow];
      if (originalIndex < slotIndex) continue;
      const size_t localSlot = originalIndex - slotIndex;
      if (localSlot >= tailCount || slotOwner[localSlot] != kUnassigned) continue;
      const auto& compatibleSlots = compatibility.slotsByWindow[localWindow];
      if (std::find(
              compatibleSlots.begin(), compatibleSlots.end(), localSlot) !=
          compatibleSlots.end()) {
        slotOwner[localSlot] = localWindow;
      }
    }

    std::vector<size_t> assignmentOrder(tailCount);
    for (size_t i = 0; i < tailCount; ++i) assignmentOrder[i] = i;
    std::stable_sort(
        assignmentOrder.begin(), assignmentOrder.end(),
        [&](size_t first, size_t second) {
          const size_t firstChoices = compatibility.slotsByWindow[first].size();
          const size_t secondChoices = compatibility.slotsByWindow[second].size();
          return firstChoices != secondChoices
              ? firstChoices < secondChoices
              : remainingWindows[first] < remainingWindows[second];
        });

    for (size_t localWindow : assignmentOrder) {
      if (std::find(slotOwner.begin(), slotOwner.end(), localWindow) !=
          slotOwner.end()) {
        continue;
      }
      std::vector<bool> visitedSlots(tailCount, false);
      if (!TryAssignConstraintWindow(
              localWindow, compatibility, &slotOwner, fixed,
              &visitedSlots, true)) {
        return false;
      }
    }

    for (size_t localSlot = 0; localSlot < tailCount; ++localSlot) {
      if (slotOwner[localSlot] == kUnassigned) return false;
      desiredOrder[slotIndex + localSlot] =
          windows[remainingWindows[slotOwner[localSlot]]];
    }
    return true;
  };

  auto search = [&](auto&& self, size_t slotIndex, const RECT& area) -> bool {
    if (++searchNodes > kMaxSearchNodes) {
      searchLimitReached = true;
      return false;
    }
    if (slotIndex >= count) return true;

    SearchKey key{};
    if (canMemoize) {
      key = {usedMask, static_cast<uint32_t>(slotIndex),
             RectWidth(area), RectHeight(area)};
      if (deadStates.find(key) != deadStates.end()) return false;
    }

    if (slotIndex >= adaptiveSplitCount || slotIndex + 1 >= count) {
      const bool matched = matchFixedTail(slotIndex, area);
      if (!matched && canMemoize) deadStates.insert(key);
      return matched;
    }

    const bool vertical = (slotIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(area) : RectHeight(area);
    if (span < 2) {
      if (canMemoize) deadStates.insert(key);
      return false;
    }
    const LONG usable = UsableSplitSpan(span, gap);
    if (usable < 2) {
      if (canMemoize) deadStates.insert(key);
      return false;
    }
    const auto [minimumAdaptive, maximumAdaptive] = adaptiveEnvelope.SpanBounds(usable);
    const LONG currentSpan = SplitLeadingSpan(usable, ClampSplitRatio(ratios[slotIndex]));

    std::vector<size_t> candidates;
    candidates.reserve(count - slotIndex);
    for (size_t windowIndex = 0; windowIndex < count; ++windowIndex) {
      if (!used[windowIndex]) candidates.push_back(windowIndex);
    }
    SortByDistanceFrom(candidates, slotIndex);

    for (size_t windowIndex : candidates) {
      const WindowConstraints& constraint = constraints[windowIndex];
      const LONG primaryMinimum = constraint.valid
          ? std::max<LONG>(
                1, vertical ? constraint.minWidth : constraint.minHeight)
          : 1;
      LONG firstSpan = usable;
      if (currentSpan >= primaryMinimum) {
        // Keeping the current manual ratio is always allowed, even when it lies
        // outside the automatic envelope.
        firstSpan = currentSpan;
      }
      const LONG adaptiveSpan = std::max(primaryMinimum, minimumAdaptive);
      if (adaptiveSpan <= maximumAdaptive) {
        // The smallest permitted automatic span maximizes the recursive tail;
        // with minimum-only constraints this is never worse for continuation.
        firstSpan = std::min(firstSpan, adaptiveSpan);
      }
      if (firstSpan >= usable) continue;

      RECT first{};
      RECT remaining{};
      if (!makeSplit(area, slotIndex, firstSpan, &first, &remaining) ||
          !WindowConstraintFitsSlot(constraint, first)) {
        continue;
      }

      used[windowIndex] = true;
      if (canMemoize) usedMask |= (uint64_t{1} << windowIndex);
      desiredOrder[slotIndex] = windows[windowIndex];
      if (self(self, slotIndex + 1, remaining)) return true;
      desiredOrder[slotIndex] = nullptr;
      if (canMemoize) usedMask &= ~(uint64_t{1} << windowIndex);
      used[windowIndex] = false;
      if (searchLimitReached) return false;
    }

    if (canMemoize) deadStates.insert(key);
    return false;
  };

  RECT normalizedArea{0, 0, RectWidth(workArea), RectHeight(workArea)};
  if (!search(search, 0, normalizedArea)) {
    if (searchLimitReached) {
      Diagnostics::RecordEvent(
          L"dwindle layout conversion search limit reached windows=%zu adaptive=%zu nodes=%zu",
          count, adaptiveSplitCount, searchNodes);
    }
    return false;
  }

  Workspace trial = workspace;
  if (!ApplyTiledWindowOrder(trial, desiredOrder)) return false;
  std::vector<RECT> rects;
  ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
  if (BuildWorkspaceLayoutPlan(
          trial, workArea, gap, &rects) != LayoutPlanStatus::Success) {
    return false;
  }

  size_t movedWindowCount = 0;
  for (size_t i = 0; i < count; ++i) {
    if (desiredOrder[i] != windows[i]) ++movedWindowCount;
  }
  if (outMovedWindowCount) *outMovedWindowCount = movedWindowCount;
  workspace = std::move(trial);
  return true;
}

// Dwindle geometry belongs to slots, not applications. During admission only,
// assign every active window to a compatible existing slot before concluding
// that one must float. The new window stays in the candidate slot, and an
// explicit user-mutation subject stays put.
static bool ReassignPendingDwindleAdmissionForConstraints(
    Workspace& workspace, const std::vector<RECT>& slots,
    HWND pendingAdmission, HWND fixedWindow, size_t* outMovedWindowCount) {
  if (workspace.Layout() != TileLayout::Dwindle ||
      slots.size() != workspace.ActiveCount()) {
    return false;
  }

  const WindowRecord* pendingRecord = workspace.Find(pendingAdmission);
  const size_t pendingWindowIndex = workspace.TiledIndex(pendingAdmission);
  if (!pendingRecord || !pendingRecord->pendingInsertionPlacement ||
      pendingWindowIndex >= workspace.ActiveCount()) {
    return false;
  }

  const std::vector<HWND> windows = workspace.TiledWindows();
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  const size_t count = windows.size();
  const ConstraintSlotCompatibility compatibility =
      BuildConstraintSlotCompatibility(constraints, slots, true);
  for (const auto& compatibleSlots : compatibility.slotsByWindow) {
    if (compatibleSlots.empty()) return false;
  }

  constexpr size_t kUnassigned = static_cast<size_t>(-1);
  std::vector<size_t> slotOwner(count, kUnassigned);
  std::vector<bool> fixed(count, false);
  auto pinWindow = [&](size_t windowIndex, size_t slotIndex) {
    if (windowIndex >= count || slotIndex >= count ||
        slotOwner[slotIndex] != kUnassigned) {
      return false;
    }
    const auto& compatibleSlots = compatibility.slotsByWindow[windowIndex];
    if (std::find(
            compatibleSlots.begin(), compatibleSlots.end(), slotIndex) ==
        compatibleSlots.end()) {
      return false;
    }
    slotOwner[slotIndex] = windowIndex;
    fixed[windowIndex] = true;
    return true;
  };

  if (!pinWindow(pendingWindowIndex, pendingWindowIndex)) return false;
  const size_t fixedWindowIndex = workspace.TiledIndex(fixedWindow);
  if (fixedWindow && fixedWindow != pendingAdmission &&
      !pinWindow(fixedWindowIndex, fixedWindowIndex)) {
    return false;
  }

  // Seed every compatible current placement. Starting with empty slots allowed
  // later flexible windows to evict earlier ones and shuffle the whole order.
  for (size_t i = 0; i < count; ++i) {
    const auto& compatibleSlots = compatibility.slotsByWindow[i];
    if (!fixed[i] && slotOwner[i] == kUnassigned &&
        std::find(compatibleSlots.begin(), compatibleSlots.end(), i) !=
            compatibleSlots.end()) {
      slotOwner[i] = i;
    }
  }
  std::vector<size_t> assignmentOrder;
  assignmentOrder.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (!fixed[i]) assignmentOrder.push_back(i);
  }
  std::stable_sort(
      assignmentOrder.begin(), assignmentOrder.end(),
      [&](size_t first, size_t second) {
        const size_t firstChoices = compatibility.slotsByWindow[first].size();
        const size_t secondChoices = compatibility.slotsByWindow[second].size();
        return firstChoices != secondChoices
            ? firstChoices < secondChoices
            : first < second;
      });

  for (size_t windowIndex : assignmentOrder) {
    if (std::find(slotOwner.begin(), slotOwner.end(), windowIndex) != slotOwner.end())
      continue;
    std::vector<bool> visitedSlots(count, false);
    if (!TryAssignConstraintWindow(
            windowIndex, compatibility, &slotOwner, fixed,
            &visitedSlots, true)) {
      return false;
    }
  }

  std::vector<HWND> desiredOrder(count);
  size_t movedWindowCount = 0;
  for (size_t slotIndex = 0; slotIndex < count; ++slotIndex) {
    if (slotOwner[slotIndex] == kUnassigned) return false;
    desiredOrder[slotIndex] = windows[slotOwner[slotIndex]];
    if (desiredOrder[slotIndex] != windows[slotIndex]) ++movedWindowCount;
  }
  if (!ApplyTiledWindowOrder(workspace, desiredOrder)) return false;
  if (outMovedWindowCount) *outMovedWindowCount = movedWindowCount;
  return movedWindowCount > 0;
}

// Precondition: the current order already failed its ordinary layout plan.
// Search caller-prioritized candidates without changing the input workspace or
// querying/placing windows. Failure also leaves outPlan untouched. Successful
// trials are committed by ArrangeWorkspace only after preserving rollback state.
static bool TryRecoverPendingAdmission(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    const std::vector<ConstraintCandidate>& candidates, HWND preferredTiledWindow,
    AdmissionRecoveryPlan* outPlan) {
  // Planning already tried temporary divider fitting at the configured
  // position. Try nearby slots before allowing broader sibling reassignment.
  const TileLayout layout = workspace.Layout();
  const bool searchAlternateInsertion =
      IsMasterStackLayout(layout) || IsGridLayout(layout) || layout == TileLayout::Dwindle;
  const size_t preferredTiledIndex =
      workspace.TiledIndex(preferredTiledWindow);
  AdmissionRecoveryPlan plan;

  // First try nearby slots without moving established siblings. Dwindle
  // alone gets a second pass that may reassign compatible siblings; that
  // pass tries nearby slots before retrying the admission's original slot.
  if (searchAlternateInsertion) {
    for (bool reassignSiblings : {false, true}) {
      if (reassignSiblings && layout != TileLayout::Dwindle) break;
      for (const ConstraintCandidate& candidate : candidates) {
        const WindowRecord* record = workspace.Find(candidate.hwnd);
        if (!record || !record->pendingInsertionPlacement ||
            candidate.hwnd == preferredTiledWindow) {
          continue;
        }

        const size_t currentIndex = workspace.TiledIndex(candidate.hwnd);
        if (currentIndex >= workspace.ActiveCount()) continue;

        auto tryInsertionIndex = [&](size_t targetIndex) {
          Workspace trial = workspace;
          if (targetIndex != currentIndex &&
              !trial.MovePendingAdmissionToIndex(candidate.hwnd, targetIndex)) {
            return false;
          }
          if (preferredTiledIndex < workspace.ActiveCount() &&
              trial.TiledIndex(preferredTiledWindow) != preferredTiledIndex) {
            return false;
          }

          std::vector<RECT> trialRects;
          ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
          LayoutPlanStatus trialStatus = BuildWorkspaceLayoutPlan(
              trial, workArea, gap, &trialRects);
          size_t movedWindowCount = 0;
          if (trialStatus != LayoutPlanStatus::Success) {
            if (!reassignSiblings ||
                !ReassignPendingDwindleAdmissionForConstraints(
                    trial, trialRects, candidate.hwnd,
                    preferredTiledWindow, &movedWindowCount)) {
              return false;
            }
            ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
            trialStatus = BuildWorkspaceLayoutPlan(
                trial, workArea, gap, &trialRects);
            if (trialStatus != LayoutPlanStatus::Success) return false;
          }
          plan.workspace = std::move(trial);
          plan.window = candidate.hwnd;
          plan.fromIndex = currentIndex;
          plan.toIndex = targetIndex;
          plan.reassignedWindows = movedWindowCount;
          return true;
        };

        for (size_t distance = 1;
             distance < workspace.ActiveCount() && !plan.window;
             ++distance) {
          if (currentIndex + distance < workspace.ActiveCount() &&
              tryInsertionIndex(currentIndex + distance)) break;
          if (currentIndex >= distance &&
              tryInsertionIndex(currentIndex - distance)) break;
        }
        if (!plan.window && reassignSiblings) {
          tryInsertionIndex(currentIndex);
        }
        if (plan.window) {
          *outPlan = std::move(plan);
          return true;
        }
      }
    }
  }

  return false;
}

} // namespace Layout

// Learning layout preferences from completed resize gestures.
// Implementation/interface fragment assembled by src/multiwm.cpp in dependency order.

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
