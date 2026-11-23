// Model-aware layout planning and constraint candidate searches.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Layout {

struct AdmissionRecoveryPlan {
  Workspace workspace;
  HWND window = nullptr;
  size_t fromIndex = 0;
  size_t toIndex = 0;
  std::vector<RECT> rects;
};

struct ConstraintCandidate {
  HWND hwnd;
  uint64_t demandGeneration;  // Model::LayoutDemandRecency of the window.
};

struct ConstraintSlotCompatibility {
  std::vector<std::vector<size_t>> slotsByWindow;
};

enum class LayoutPlanStatus {
  Success,
  NotApplicable,
  Unsatisfiable,
};


static std::vector<WindowConstraints> ActiveWindowConstraints(
    const Workspace& workspace) {
  std::vector<WindowConstraints> constraints;
  constraints.reserve(workspace.ActiveCount());
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    WindowConstraints limits = record ? record->constraints : WindowConstraints{};
    if (record && record->ghostSize.cx > 0 && record->ghostSize.cy > 0) {
      limits.minWidth = limits.maxWidth = record->ghostSize.cx;
      limits.minHeight = limits.maxHeight = record->ghostSize.cy;
      limits.valid = true;
    }
    constraints.push_back(limits);
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

// Shrinks each rectangle that is larger than its window's maximum tracking size
// to that maximum, centred in the original cell. Windows can't grow past that
// size: asked for the whole cell, a capped window kept its top-left corner and
// stayed short, so placement reported AdjustedByWindow, the conformance lease
// kept retrying, and the window was floated when the lease expired. The other
// windows keep their cells; the spare space stays as an even gap rather than
// being redistributed. A valid maximum is never below the minimum, so this
// never breaks WindowConstraintFitsSlot. Pure; changes only `rects`.
static void CenterWithinMaximumSizes(
    std::vector<RECT>& rects, const std::vector<WindowConstraints>& constraints) {
  const size_t count = std::min(rects.size(), constraints.size());
  for (size_t i = 0; i < count; ++i) {
    if (!constraints[i].valid) continue;
    SharedGeometry::CenterWithinMaximumSize(rects[i].left, rects[i].right, constraints[i].maxWidth);
    SharedGeometry::CenterWithinMaximumSize(rects[i].top, rects[i].bottom, constraints[i].maxHeight);
  }
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

static bool AdaptiveSplitsEnabled() {
  return g_settings.adaptiveDwindleSlots > 0 &&
      g_settings.adaptiveDwindlePercentage > 0;
}

static double GetAdaptiveSplitDeviation() {
  return std::clamp<LONG>(g_settings.adaptiveDwindlePercentage, 0, 40) / 100.0;
}

static BspAdaptation GetBspAdaptation() {
  if (!AdaptiveSplitsEnabled()) return {};
  return {g_settings.adaptiveDwindleSlots, GetAdaptiveSplitDeviation()};
}

// Calculate temporary geometry from the saved divider preferences. Only the
// configured leading splits may adapt; deeper ratios and window order stay fixed.
// Never write fitted ratios back to the workspace: removing a constraint should
// let the divider return to its preferred position. Only a user resize (a drag
// or a window-size command) saves fitted ratios, by accepting the layout as
// shown (Workspace::AcceptShownDwindleLayout, layout/resize_steps.h).
static bool BuildAdaptiveDwindleLayout(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects,
    std::vector<DwindleSplit>* outSplits = nullptr) {
  if (workspace.Layout() != TileLayout::Dwindle || workspace.ActiveCount() < 2 ||
      !AdaptiveSplitsEnabled()) {
    return false;
  }

  const auto& windows = workspace.TiledWindows();
  const auto constraints = ActiveWindowConstraints(workspace);
  std::vector<double> ratios = workspace.DwindleRatios();
  const size_t adaptiveSplitCount =
      std::min(g_settings.adaptiveDwindleSlots, ratios.size());
  if (adaptiveSplitCount == 0) return false;
  const double adaptiveDeviation = GetAdaptiveSplitDeviation();

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

      const LONG currentAllocated = SplitLeadingSpan(usable, fixedRatio);
      if (currentAllocated >= first && usable - currentAllocated >= tail) {
        return true;
      }
      if (!adaptive) return false;

      const auto [minimumAdaptive, maximumAdaptive] = AdaptiveSplitSpanBounds(usable, fixedRatio, adaptiveDeviation);
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
  // within the permitted distance from their saved ratios.
  std::vector<SIZE> subtreeMinimums = windowMinimums;
  for (size_t splitIndex = windows.size() - 1; splitIndex-- > 0;) {
    const SIZE first = windowMinimums[splitIndex];
    const SIZE tail = subtreeMinimums[splitIndex + 1];
    SIZE combined{
        std::max(first.cx, tail.cx),
        std::max(first.cy, tail.cy)};
    const bool adaptive = splitIndex < adaptiveSplitCount;
    if (AlternatingAxis(FirstSplitAxis(workArea), splitIndex) == Axis::X) {
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
    const bool vertical = AlternatingAxis(FirstSplitAxis(workArea), splitIndex) == Axis::X;
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
      const auto [minimumAdaptive, maximumAdaptive] =
          AdaptiveSplitSpanBounds(usable, ratios[splitIndex], adaptiveDeviation);
      const LONG lower = std::max(firstMinimum, minimumAdaptive);
      const LONG upper = std::min(usable - tailMinimum, maximumAdaptive);
      if (lower > upper) return false;
      firstSpan = std::clamp<LONG>(desired, lower, upper);
    }
    ratios[splitIndex] = static_cast<double>(firstSpan) / usable;
    LayoutDwindle(
        workArea, gap, 0, windows.size(), ratios,
        g_settings.reverseDwindle, FirstSplitAxis(workArea), rects, &splits);
    if (splitIndex >= splits.size()) return false;
    remaining = splits[splitIndex].remaining;
  }

  if (!RectsSatisfyWindowConstraints(rects, constraints)) return false;
  *outRects = std::move(rects);
  if (outSplits) *outSplits = std::move(splits);
  return true;
}

// Planning step 1: the cells the saved preferences give, ignoring native
// minimums. Monocle gives every window the whole work area. `outSplits`
// (optional, Dwindle only) receives the split geometry. Not for Floating.
static void PlanPreferredCells(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects, std::vector<DwindleSplit>* outSplits = nullptr) {
  const TileLayout layout = workspace.Layout();
  const size_t count = workspace.ActiveCount();
  outRects->assign(count, RECT{});
  if (outSplits) outSplits->clear();
  if (IsMasterStackLayout(layout)) {
    LayoutMasterStackWeighted(workArea, gap, count, *outRects, LayoutAxis(layout),
                              ClampSplitRatio(workspace.MasterRatio()), workspace.StackWeights());
  } else if (IsGridLayout(layout)) {
    LayoutGridWeighted(workArea, gap, count, *outRects, LayoutAxis(layout), workspace.GridWeights());
  } else if (layout == TileLayout::Dwindle) {
    LayoutDwindle(workArea, gap, 0, count, workspace.DwindleRatios(),
                  g_settings.reverseDwindle, FirstSplitAxis(workArea), *outRects, outSplits);
  } else if (layout == TileLayout::Bsp) {
    BspLayoutResult bsp;
    LayoutBsp(workspace.Bsp(), workspace.TiledWindows(), workArea, gap, &bsp);
    *outRects = std::move(bsp.rects);
  } else {  // Monocle
    outRects->assign(count, workArea);
  }
}

// Planning step 2: re-solves with the windows' native minimums (`constraints`,
// in tiled order) as hard limits. Saved preferences are not changed, so
// dividers return once room is available. False when nothing fits: Grid and
// Master + Stack may then leave partial output in `outRects`; Dwindle and BSP
// leave it unchanged. `outWeightPerPixel` (optional) receives the Grid or Master + Stack
// allocation's weight scale; `outSplits` is as for PlanPreferredCells.
static bool PlanConstrainedCells(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    const std::vector<WindowConstraints>& constraints, std::vector<RECT>* outRects,
    double* outWeightPerPixel = nullptr, std::vector<DwindleSplit>* outSplits = nullptr) {
  const TileLayout layout = workspace.Layout();
  if (IsMasterStackLayout(layout)) {
    return LayoutMasterStackConstrained(workArea, gap, LayoutAxis(layout),
                                        ClampSplitRatio(workspace.MasterRatio()), workspace.StackWeights(),
                                        constraints, outRects, outWeightPerPixel);
  }
  if (IsGridLayout(layout)) {
    return LayoutGridConstrained(workArea, gap, workspace.GridWeights(), constraints,
                                 LayoutAxis(layout), outRects, outWeightPerPixel);
  }
  if (layout == TileLayout::Dwindle) {
    return BuildAdaptiveDwindleLayout(workspace, workArea, gap, outRects, outSplits);
  }
  if (layout == TileLayout::Bsp) {
    BspLayoutResult bsp;
    if (!FitBspLayout(workspace.Bsp(), workspace.TiledWindows(), workArea, gap, constraints,
                      GetBspAdaptation(), &bsp)) {
      return false;
    }
    *outRects = std::move(bsp.rects);
    return true;
  }
  return LayoutMonocleConstrained(workArea, constraints, outRects);  // Monocle
}

// Uses cached constraints to calculate physical screen rectangles without native
// window queries or workspace mutation. Output is usable only on Success; other
// outcomes may leave partial geometry, which callers must discard. Every call
// counts toward the layoutPlansBuilt diagnostic. Skip maximum-size centering
// when a caller needs the full tile bounds.
static LayoutPlanStatus BuildWorkspaceLayoutPlan(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects, bool centerCappedWindows = true) {
  ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
  if (!outRects) return LayoutPlanStatus::Unsatisfiable;
  const TileLayout layout = workspace.Layout();
  if (layout == TileLayout::Floating || layout == TileLayout::COUNT) {
    outRects->clear();
    return LayoutPlanStatus::NotApplicable;
  }

  // Maximum sizes affect where a capped window sits inside its cell.
  PlanPreferredCells(workspace, workArea, gap, outRects);
  const std::vector<WindowConstraints> constraints = ActiveWindowConstraints(workspace);
  if (!RectsSatisfyWindowConstraints(*outRects, constraints) &&
      !PlanConstrainedCells(workspace, workArea, gap, constraints, outRects)) {
    return LayoutPlanStatus::Unsatisfiable;
  }
  if (centerCappedWindows) CenterWithinMaximumSizes(*outRects, constraints);
  return LayoutPlanStatus::Success;
}

struct BspMergeDestination {
  HWND window;
  RECT originalEdge;
};

static Axis DefaultBspSplitAxis(const BspTree& tree, size_t node, const RECT& workArea) {
  size_t depth = 0;
  for (size_t parent = tree.Parent(node); parent != BspTree::kNotFound; parent = tree.Parent(parent)) ++depth;
  return AlternatingAxis(FirstSplitAxis(workArea), depth);
}

// A direction matching the depth default lets the depth rule apply again.
static void TurnBspSplit(Workspace& workspace, size_t node, const RECT& workArea) {
  const BspTree& tree = workspace.Bsp();
  const Axis defaultDirection = DefaultBspSplitAxis(tree, node, workArea);
  const Axis alternate = tree.Nodes()[node].direction.value_or(defaultDirection) == Axis::X ? Axis::Y : Axis::X;
  workspace.SetBspSplitDirection(node, alternate == defaultDirection ? std::nullopt : std::optional{alternate});
}

// Plans `workspace` (BuildWorkspaceLayoutPlan). In BSP, when that is
// unsatisfiable, turns the split of a still-settling arrival to its
// perpendicular direction, trying the newest first, and keeps the first turn
// that fits: `workspace` then holds the turned split. Unchanged otherwise. An
// explicit edge drop (`merge`) never turns: it keeps its direction and must
// overlap the requested edge of the original tile. `outRects` as for
// BuildWorkspaceLayoutPlan.
static LayoutPlanStatus PlanOrTurnNewBspSplit(
    Workspace& workspace, const RECT& workArea, LONG gap, std::vector<RECT>* outRects,
    std::optional<BspMergeDestination> merge = std::nullopt) {
  const size_t draggedIndex = merge ? workspace.TiledIndex(merge->window) : Workspace::kNotFound;
  auto plan = [&](const Workspace& candidate, std::vector<RECT>* rects) {
    const auto status = BuildWorkspaceLayoutPlan(candidate, workArea, gap, rects, !merge);
    if (status != LayoutPlanStatus::Success || !merge) return status;
    if (draggedIndex >= rects->size()) return LayoutPlanStatus::Unsatisfiable;
    const RECT& draggedTile = (*rects)[draggedIndex];
    RECT intersection{};
    if (!IntersectRect(&intersection, &draggedTile, &merge->originalEdge)) {
      return LayoutPlanStatus::Unsatisfiable;
    }
    CenterWithinMaximumSizes(*rects, ActiveWindowConstraints(candidate));
    return status;
  };
  const auto status = plan(workspace, outRects);
  if (merge || !outRects || workspace.Layout() != TileLayout::Bsp || status == LayoutPlanStatus::Success) return status;

  const BspTree& tree = workspace.Bsp();
  for (size_t i = tree.Nodes().size(); i-- > 0;) {
    if (!workspace.IsTiled(tree.Nodes()[i].pendingDirectionWindow)) continue;

    Workspace trial = workspace;
    TurnBspSplit(trial, i, workArea);
    std::vector<RECT> fitted;
    if (plan(trial, &fitted) != LayoutPlanStatus::Success) continue;
    workspace = std::move(trial);
    *outRects = std::move(fitted);
    return LayoutPlanStatus::Success;
  }
  return LayoutPlanStatus::Unsatisfiable;
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
// Dwindle layout conversion and conflict reporting share this and the
// augmenting-path matcher below.
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

// slotOwner value for a slot no window has been matched to yet.
constexpr size_t kUnassignedSlot = static_cast<size_t>(-1);

// True when `window` fits `slot` according to `compatibility`.
static bool IsCompatibleSlot(
    const ConstraintSlotCompatibility& compatibility, size_t window, size_t slot) {
  const auto& compatibleSlots = compatibility.slotsByWindow[window];
  return std::find(compatibleSlots.begin(), compatibleSlots.end(), slot) != compatibleSlots.end();
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
  const auto& compatibleSlots = compatibility.slotsByWindow[candidateWindow];

  if (preferFreeSlots) {
    for (size_t slotIndex : compatibleSlots) {
      if (slotIndex >= slotOwner->size() || (*visitedSlots)[slotIndex] ||
          (*slotOwner)[slotIndex] != kUnassignedSlot) {
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
    if (displacedWindow == kUnassignedSlot ||
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

// Completes a window-to-slot matching. `slotOwner` arrives holding pinned and
// seeded assignments; `fixedWindows` are never displaced. Every window that has
// no slot yet is then assigned, the one with the fewest compatible slots first
// (ties by index), along augmenting paths that prefer free slots. True when
// every slot ends up with a window; false leaves a partial matching.
static bool CompleteSlotMatching(
    const ConstraintSlotCompatibility& compatibility, std::vector<size_t>& slotOwner,
    const std::vector<bool>& fixedWindows) {
  const size_t windowCount = compatibility.slotsByWindow.size();
  std::vector<size_t> assignmentOrder;
  assignmentOrder.reserve(windowCount);
  for (size_t window = 0; window < windowCount; ++window) {
    if (!fixedWindows[window]) assignmentOrder.push_back(window);
  }
  std::stable_sort(
      assignmentOrder.begin(), assignmentOrder.end(),
      [&](size_t first, size_t second) {
        const size_t firstChoices = compatibility.slotsByWindow[first].size();
        const size_t secondChoices = compatibility.slotsByWindow[second].size();
        return firstChoices != secondChoices ? firstChoices < secondChoices : first < second;
      });

  for (size_t window : assignmentOrder) {
    if (std::find(slotOwner.begin(), slotOwner.end(), window) != slotOwner.end()) continue;
    std::vector<bool> visitedSlots(slotOwner.size(), false);
    if (!TryAssignConstraintWindow(window, compatibility, &slotOwner, fixedWindows, &visitedSlots, true)) {
      return false;
    }
  }
  return std::find(slotOwner.begin(), slotOwner.end(), kUnassignedSlot) == slotOwner.end();
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

// Memo key for the Dwindle conversion search: which windows are placed, which
// slot is next, and the size of the area still to fill. A key that failed once
// fails again, so it is recorded and skipped.
struct DwindleSearchKey {
  uint64_t usedMask = 0;
  uint32_t slotIndex = 0;
  LONG width = 0;
  LONG height = 0;
  bool operator==(const DwindleSearchKey& other) const noexcept {
    return usedMask == other.usedMask && slotIndex == other.slotIndex &&
        width == other.width && height == other.height;
  }
};

struct DwindleSearchKeyHash {
  size_t operator()(const DwindleSearchKey& key) const noexcept {
    size_t value = std::hash<uint64_t>{}(key.usedMask);
    value ^= std::hash<uint32_t>{}(key.slotIndex) + 0x9e3779b9u + (value << 6) + (value >> 2);
    value ^= std::hash<LONG>{}(key.width) + 0x9e3779b9u + (value << 6) + (value >> 2);
    value ^= std::hash<LONG>{}(key.height) + 0x9e3779b9u + (value << 6) + (value >> 2);
    return value;
  }
};

// Splits `area` for Dwindle slot `slotIndex` with an explicit leading size.
// False when the split is impossible or leaves no room for the remaining slots.
static bool SplitDwindleSlot(
    const RECT& area, size_t slotIndex, Axis firstAxis, LONG firstSpan, LONG gap, RECT* first, RECT* remaining) {
  if (!first || !remaining) return false;
  const bool vertical = AlternatingAxis(firstAxis, slotIndex) == Axis::X;
  const LONG span = vertical ? RectWidth(area) : RectHeight(area);
  if (span < 2) return false;
  const LONG usable = UsableSplitSpan(span, gap);
  if (firstSpan < 1 || firstSpan >= usable) return false;
  SplitDwindleArea(area, vertical, firstSpan, span - usable, g_settings.reverseDwindle, first, remaining);
  return RectWidth(*remaining) > 0 && RectHeight(*remaining) > 0;
}

// Places every window not yet `used` into the fixed-ratio Dwindle slots from
// `slotIndex` onward inside `area`. Windows keep their current position where it
// fits; the rest are assigned by bipartite matching. On success writes those
// slots of `desiredOrder` and returns true; on failure `desiredOrder` may hold
// partial assignments that the caller discards.
static bool MatchDwindleTail(
    size_t slotIndex, const RECT& area, LONG gap, Axis firstAxis,
    const std::vector<HWND>& windows, const std::vector<WindowConstraints>& constraints,
    const std::vector<double>& ratios, const std::vector<bool>& used,
    std::vector<HWND>* desiredOrder) {
  const size_t count = windows.size();
  const size_t tailCount = count - slotIndex;
  if (tailCount == 0) return true;

  std::vector<size_t> remainingWindows;
  remainingWindows.reserve(tailCount);
  for (size_t windowIndex = 0; windowIndex < count; ++windowIndex) {
    if (!used[windowIndex]) remainingWindows.push_back(windowIndex);
  }
  if (remainingWindows.size() != tailCount) return false;

  std::vector<RECT> allSlots(count);
  LayoutDwindle(area, gap, slotIndex, tailCount, ratios, g_settings.reverseDwindle, firstAxis, allSlots);
  std::vector<RECT> tailSlots(allSlots.begin() + slotIndex, allSlots.end());
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

  std::vector<size_t> slotOwner(tailCount, kUnassignedSlot);

  // Seed surviving exact positions first so a layout conversion disturbs as
  // little order as possible before augmenting paths are allowed to move them.
  for (size_t localWindow = 0; localWindow < tailCount; ++localWindow) {
    const size_t originalIndex = remainingWindows[localWindow];
    if (originalIndex < slotIndex) continue;
    const size_t localSlot = originalIndex - slotIndex;
    if (localSlot >= tailCount || slotOwner[localSlot] != kUnassignedSlot) continue;
    if (IsCompatibleSlot(compatibility, localWindow, localSlot)) slotOwner[localSlot] = localWindow;
  }

  // remainingWindows is ascending, so ties by local index keep the original order.
  if (!CompleteSlotMatching(compatibility, slotOwner, std::vector<bool>(tailCount, false))) return false;
  for (size_t localSlot = 0; localSlot < tailCount; ++localSlot) {
    (*desiredOrder)[slotIndex + localSlot] = windows[remainingWindows[slotOwner[localSlot]]];
  }
  return true;
}
// Layout conversion has different semantics from an explicit order mutation.
// Preserve the existing order first (the caller already tried that), but if the
// new Dwindle geometry cannot host that order, find a compatible permutation.
// TryReorderForConstraints reuses this search after a sibling leaves.
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
  const Axis firstAxis = FirstSplitAxis(workArea);

  const size_t adaptiveSplitCount = AdaptiveSplitsEnabled()
      ? std::min(
            g_settings.adaptiveDwindleSlots,
            count > 0 ? count - 1 : size_t{0})
      : 0;
  const double adaptiveDeviation = GetAdaptiveSplitDeviation();
  std::vector<bool> used(count, false);
  std::vector<HWND> desiredOrder(count, nullptr);
  uint64_t usedMask = 0;
  size_t searchNodes = 0;
  bool searchLimitReached = false;
  constexpr size_t kMaxSearchNodes = 250000;

  std::unordered_set<DwindleSearchKey, DwindleSearchKeyHash> deadStates;
  const bool canMemoize = count <= 64;

  auto search = [&](auto&& self, size_t slotIndex, const RECT& area) -> bool {
    if (++searchNodes > kMaxSearchNodes) {
      searchLimitReached = true;
      return false;
    }
    if (slotIndex >= count) return true;

    DwindleSearchKey key{};
    if (canMemoize) {
      key = {usedMask, static_cast<uint32_t>(slotIndex),
             RectWidth(area), RectHeight(area)};
      if (deadStates.find(key) != deadStates.end()) return false;
    }

    if (slotIndex >= adaptiveSplitCount || slotIndex + 1 >= count) {
      const bool matched = MatchDwindleTail(
          slotIndex, area, gap, firstAxis, windows, constraints, ratios, used, &desiredOrder);
      if (!matched && canMemoize) deadStates.insert(key);
      return matched;
    }

    const bool vertical = AlternatingAxis(firstAxis, slotIndex) == Axis::X;
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
    const auto [minimumAdaptive, maximumAdaptive] = AdaptiveSplitSpanBounds(usable, ratios[slotIndex], adaptiveDeviation);

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
      const LONG firstSpan = std::max(primaryMinimum, minimumAdaptive);
      if (firstSpan > maximumAdaptive) continue;

      RECT first{};
      RECT remaining{};
      if (!SplitDwindleSlot(area, slotIndex, firstAxis, firstSpan, gap, &first, &remaining) ||
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

// Membership-preserving recovery for a layout that stopped fitting without a
// new admission to blame: a sibling left (shifting every later Dwindle slot's
// split axis, or promoting a new master) or a settled window raised its minimum.
// Finds a window order that fits so no settled window has to float.
//
// Only Dwindle and Master + Stack feasibility depends on order. Master + Stack
// depends only on which window is master, so each stack member is tried there,
// nearest first, by swapping it with the current master. Dwindle uses the same
// joint ratio/assignment search as a layout conversion. `pinnedWindow`, the
// window the user just acted on, keeps its slot. On success writes the trial to
// *outWorkspace; on failure leaves it untouched. Pure planning.
static bool TryReorderForConstraints(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    HWND pinnedWindow, Workspace* outWorkspace, size_t* outMovedWindowCount) {
  if (!outWorkspace || workspace.ActiveCount() < 2) return false;
  const TileLayout layout = workspace.Layout();
  const std::vector<HWND> windows = workspace.TiledWindows();
  const size_t pinnedIndex = workspace.TiledIndex(pinnedWindow);

  if (IsMasterStackLayout(layout)) {
    if (pinnedIndex == 0) return false;
    for (size_t index = 1; index < windows.size(); ++index) {
      if (index == pinnedIndex) continue;
      Workspace trial = workspace;
      if (!trial.MakeMaster(windows[index])) continue;
      std::vector<RECT> rects;
      if (BuildWorkspaceLayoutPlan(trial, workArea, gap, &rects) ==
          LayoutPlanStatus::Success) {
        if (outMovedWindowCount) *outMovedWindowCount = 2;
        *outWorkspace = std::move(trial);
        return true;
      }
    }
    return false;
  }

  if (layout == TileLayout::Dwindle) {
    Workspace trial = workspace;
    size_t moved = 0;
    if (!ReassignDwindleLayoutConversionForConstraints(trial, workArea, gap, &moved) ||
        moved == 0 ||
        (pinnedIndex < windows.size() && trial.TiledIndex(pinnedWindow) != pinnedIndex)) {
      return false;
    }
    if (outMovedWindowCount) *outMovedWindowCount = moved;
    *outWorkspace = std::move(trial);
    return true;
  }
  return false;  // Grid, Monocle and BSP (which never reorders) don't depend on order.
}

// Calls `tryIndex` with each index in [0, count) other than `origin`, nearest
// first and the later one first at equal distance, until it returns true. True
// when one did. Used to move a window as little as possible.
template <typename TryIndex>
static bool TryNearbyIndices(size_t origin, size_t count, TryIndex&& tryIndex) {
  for (size_t distance = 1; distance < count; ++distance) {
    if (origin + distance < count && tryIndex(origin + distance)) return true;
    if (origin >= distance && tryIndex(origin - distance)) return true;
  }
  return false;
}

// Moves BSP arrival `arrival` from the tile it split to a split of the largest
// other tile, measured without the arrival. False, changing nothing, when that
// is the tile it already split (MoveBspLeaf refuses its sibling) or there is
// none. The new split may still turn (PlanOrTurnNewBspSplit).
static bool MoveBspArrivalToLargestTile(Workspace& workspace, HWND arrival) {
  BspTree without = workspace.Bsp();
  without.Remove(arrival);
  const HWND largest = without.LargestLeaf([&](HWND window) { return window != arrival && workspace.IsTiled(window); });
  return largest && workspace.MoveBspLeaf(arrival, largest, true);
}

// BSP's fallback for an arrival that fits neither direction of the tile it
// split (the focused or the largest): the largest other tile, once, in either
// direction. When that tile is the one it already split, or doesn't fit
// either, the window floats. Same contract as TryRecoverPendingAdmission.
static bool TryRecoverPendingBspAdmission(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    const std::vector<ConstraintCandidate>& candidates, HWND preferredTiledWindow,
    AdmissionRecoveryPlan* outPlan) {
  for (const ConstraintCandidate& candidate : candidates) {
    const WindowRecord* record = workspace.Find(candidate.hwnd);
    if (!record || !record->pendingInsertionPlacement || candidate.hwnd == preferredTiledWindow ||
        !workspace.IsTiled(candidate.hwnd)) {
      continue;
    }
    Workspace trial = workspace;
    std::vector<RECT> rects;
    if (!MoveBspArrivalToLargestTile(trial, candidate.hwnd) ||
        PlanOrTurnNewBspSplit(trial, workArea, gap, &rects) != LayoutPlanStatus::Success) {
      continue;
    }
    const size_t fromIndex = workspace.TiledIndex(candidate.hwnd);
    const size_t toIndex = trial.TiledIndex(candidate.hwnd);
    *outPlan = {std::move(trial), candidate.hwnd, fromIndex, toIndex, std::move(rects)};
    return true;
  }
  return false;
}

// Precondition: the current order already failed its ordinary layout plan.
// Search nearby slots for each pending admission, preserving the other windows' order.
// BSP tries the largest other tile instead (TryRecoverPendingBspAdmission).
// The input workspace and native windows are untouched.
// A failed search leaves outPlan untouched. ArrangeWorkspace commits a
// successful trial after preserving rollback state.
static bool TryRecoverPendingAdmission(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    const std::vector<ConstraintCandidate>& candidates, HWND preferredTiledWindow,
    AdmissionRecoveryPlan* outPlan) {
  const TileLayout layout = workspace.Layout();
  if (layout == TileLayout::Bsp) {
    return TryRecoverPendingBspAdmission(workspace, workArea, gap, candidates, preferredTiledWindow, outPlan);
  }
  if (!IsMasterStackLayout(layout) && !IsGridLayout(layout) && layout != TileLayout::Dwindle) return false;

  const size_t preferredTiledIndex = workspace.TiledIndex(preferredTiledWindow);
  for (const ConstraintCandidate& candidate : candidates) {
    const WindowRecord* record = workspace.Find(candidate.hwnd);
    if (!record || !record->pendingInsertionPlacement || candidate.hwnd == preferredTiledWindow) {
      continue;
    }

    const size_t currentIndex = workspace.TiledIndex(candidate.hwnd);
    if (currentIndex >= workspace.ActiveCount()) continue;

    auto trySlot = [&](size_t targetIndex) {
      Workspace trial = workspace;
      const bool moved = trial.MovePendingAdmissionToIndex(candidate.hwnd, targetIndex);
      if (!moved || (preferredTiledIndex < workspace.ActiveCount() &&
                     trial.TiledIndex(preferredTiledWindow) != preferredTiledIndex)) {
        return false;
      }

      std::vector<RECT> trialRects;
      if (PlanOrTurnNewBspSplit(trial, workArea, gap, &trialRects) != LayoutPlanStatus::Success) {
        return false;
      }
      const size_t newIndex = trial.TiledIndex(candidate.hwnd);
      *outPlan = {std::move(trial), candidate.hwnd, currentIndex, newIndex, std::move(trialRects)};
      return true;
    };

    if (TryNearbyIndices(currentIndex, workspace.ActiveCount(), trySlot)) {
      return true;
    }
  }

  return false;
}

} // namespace Layout
