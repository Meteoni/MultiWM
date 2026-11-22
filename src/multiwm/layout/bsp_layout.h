// Rectangles for the BSP layout's split tree (model/bsp_tree.h).
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Layout {

// A split with windows shown on both sides.
struct BspSplit {
  size_t node = 0;  // Index in BspTree::Nodes().
  Axis axis = Axis::X;
  LONG usableSpan = 0;  // The region's span along `axis`, less the gap.
  LONG firstSpan = 0;   // The first side's part of usableSpan.
  RECT first{};
  RECT second{};
};

struct BspLayoutResult {
  std::vector<RECT> rects;       // One per tiled window, in tiled order.
  std::vector<BspSplit> splits;  // In tree order: an ancestor before its descendants.
};

// Splits above this depth may move from their saved ratio by up to `deviation`.
struct BspAdaptation {
  size_t depth = 0;
  double deviation = 0.0;
};

// One layout run over a BSP tree: the preferred layout, every divider at its
// ratio, or a fitted one, where adaptive splits move as far as minimums need.
// Splits alternate by depth unless they saved an alternate direction.
// Fitting adjusts divider positions while keeping those directions.
// `tiled` lists the tiled windows in order. Leaves of other windows (maximized
// ones) are skipped, and a split with nothing shown on one side gives its whole
// region to the other, whose splits keep their depth.
class BspPlacement {
 public:
  BspPlacement(const BspTree& tree, const std::vector<HWND>& tiled, const RECT& area, LONG gap)
      : tree_(tree), nodes_(tree.Nodes()), gap_(gap), firstAxis_(FirstSplitAxis(area)),
        maximumSpan_(std::max(RectWidth(area), RectHeight(area))), topology_(nodes_.size()) {
    if (nodes_.empty()) return;

    // Workspace supplies tiled windows in leaf order, skipping suspended
    // leaves. Match those directly and keep arbitrary caller order supported.
    size_t next = 0;
    Measure(0, [&](HWND window) {
      return next < tiled.size() && tiled[next] == window ? next++ : BspTree::kNotFound;
    });
    if (next == tiled.size()) return;

    std::unordered_map<HWND, size_t> tiledIndices;
    tiledIndices.reserve(tiled.size());
    for (size_t i = 0; i < tiled.size(); ++i) tiledIndices.emplace(tiled[i], i);
    Measure(0, [&](HWND window) {
      const auto found = tiledIndices.find(window);
      return found == tiledIndices.end() ? BspTree::kNotFound : found->second;
    });
  }

  // Makes Place fit `constraints` (one per tiled window) with `adaptation`:
  // it then returns false, with its output partly written, when they can't all be met.
  void Fit(const std::vector<WindowConstraints>* constraints, BspAdaptation adaptation) {
    constraints_ = constraints;
    adaptation_ = adaptation;
    if (constraints_ && !nodes_.empty()) {
      minimum_.assign(nodes_.size(), SIZE{1, 1});
      MeasureMinimum(0, 0);
    }
  }

  bool Place(size_t node, size_t depth, const RECT& region, BspLayoutResult* out) const {
    const BspNode& current = nodes_[node];
    if (!BspTree::IsSplit(current)) return PlaceLeaf(topology_[node].tiledIndex, region, out);
    const size_t a = node + 1;
    const size_t b = topology_[node].secondChild;
    const bool aShown = Shown(a);
    const bool bShown = Shown(b);
    if (!aShown || !bShown) return !(aShown || bShown) || Place(aShown ? a : b, depth + 1, region, out);

    BspSplit split{node, current.direction.value_or(AlternatingAxis(firstAxis_, depth)), 0, 0, region, region};
    const bool alongX = split.axis == Axis::X;
    const LONG span = alongX ? RectWidth(region) : RectHeight(region);
    if (span >= 2) {
      split.usableSpan = UsableSplitSpan(span, gap_);
      const std::optional<LONG> firstSpan = FirstSpan(current, a, b, depth, split.usableSpan, alongX);
      if (!firstSpan) return false;
      split.firstSpan = *firstSpan;
      if (alongX) {
        split.first.right = region.left + split.firstSpan;
        split.second.left = split.first.right + (span - split.usableSpan);
      } else {
        split.first.bottom = region.top + split.firstSpan;
        split.second.top = split.first.bottom + (span - split.usableSpan);
      }
      out->splits.push_back(split);
    } else if (constraints_) {
      return false;
    }
    // Too thin to split: both sides get all of it, as Dwindle does.
    return Place(a, depth + 1, split.first, out) && Place(b, depth + 1, split.second, out);
  }

  // The splits above `window`'s leaf that have windows on both sides, root
  // first, as SharedSplits::SolveBspPath takes them: each with the other
  // side's minimum. Requires Fit. Empty when `window` is the only one shown.
  std::vector<SharedSplits::BspPathSplit> PathTo(HWND window) const {
    std::vector<SharedSplits::BspPathSplit> path;
    const size_t leaf = tree_.FindLeaf(window);
    size_t node = 0;
    for (size_t depth = 0; leaf != BspTree::kNotFound && node != leaf; ++depth) {
      const size_t a = node + 1;
      const size_t b = topology_[node].secondChild;
      const bool windowFirst = leaf < b;
      const size_t other = windowFirst ? b : a;
      if (Shown(other)) {
        const BspNode& split = nodes_[node];
        const bool alongX = split.direction.value_or(AlternatingAxis(firstAxis_, depth)) == Axis::X;
        path.push_back({split.ratio, minimum_[other].cx, minimum_[other].cy, static_cast<uint8_t>(alongX),
                        static_cast<uint8_t>(windowFirst), static_cast<uint8_t>(Adaptive(depth)), 0, 0});
      }
      node = windowFirst ? a : b;
    }
    return path;
  }

 private:
  struct NodeTopology {
    size_t secondChild = 0;
    size_t shown = 0;
    size_t tiledIndex = BspTree::kNotFound;
  };

  // Return the next node after the subtree. For the first child that is the
  // second child's index, so we can collect both children in one traversal.
  template <class FindTiledIndex>
  size_t Measure(size_t node, const FindTiledIndex& findTiledIndex) {
    auto& info = topology_[node];
    if (!BspTree::IsSplit(nodes_[node])) {
      info.tiledIndex = findTiledIndex(nodes_[node].window);
      info.shown = info.tiledIndex != BspTree::kNotFound;
      return node + 1;
    }
    info.secondChild = Measure(node + 1, findTiledIndex);
    const size_t end = Measure(info.secondChild, findTiledIndex);
    info.shown = topology_[node + 1].shown + topology_[info.secondChild].shown;
    return end;
  }

  bool Shown(size_t node) const { return topology_[node].shown != 0; }
  bool Adaptive(size_t depth) const { return depth < adaptation_.depth; }

  bool PlaceLeaf(size_t index, const RECT& region, BspLayoutResult* out) const {
    if (index == BspTree::kNotFound) return true;
    out->rects[index] = region;
    return !constraints_ || ConstraintAreaFits(region, ToConstraintExtent((*constraints_)[index]));
  }

  SharedSplits::SplitFit FitOf(const BspNode& split, size_t depth) const {
    return {split.ratio, Adaptive(depth), adaptation_.deviation};
  }

  // The first side's span: the ratio's, or in a fitted run as the shared
  // arithmetic fits both sides' minimums (SharedSplits::FitSplitFirstSpan).
  std::optional<LONG> FirstSpan(const BspNode& split, size_t a, size_t b, size_t depth, LONG usable,
                                bool alongX) const {
    if (!constraints_) return SplitLeadingSpan(usable, ClampSplitRatio(split.ratio));
    return SharedSplits::FitSplitFirstSpan(FitOf(split, depth), usable, alongX ? minimum_[a].cx : minimum_[a].cy,
                                           alongX ? minimum_[b].cx : minimum_[b].cy);
  }

  // The smallest size the subtree at `node` fits in: across a split the larger
  // side, along it the smallest span whose divider (moved within the range
  // when adaptive) leaves both sides their minimums.
  SIZE MeasureMinimum(size_t node, size_t depth) {
    if (!BspTree::IsSplit(nodes_[node])) {
      const size_t index = topology_[node].tiledIndex;
      const ConstraintExtent extent =
          index != BspTree::kNotFound ? ToConstraintExtent((*constraints_)[index]) : ConstraintExtent{};
      return minimum_[node] = SIZE{extent.minWidth, extent.minHeight};
    }
    const size_t a = node + 1;
    const size_t b = topology_[node].secondChild;
    const SIZE first = MeasureMinimum(a, depth + 1);
    const SIZE second = MeasureMinimum(b, depth + 1);
    if (!Shown(a)) return minimum_[node] = second;
    if (!Shown(b)) return minimum_[node] = first;
    const bool alongX = nodes_[node].direction.value_or(AlternatingAxis(firstAxis_, depth)) == Axis::X;
    SIZE combined{std::max(first.cx, second.cx), std::max(first.cy, second.cy)};
    LONG& along = alongX ? combined.cx : combined.cy;
    along = SharedSplits::RequiredSplitSpan(FitOf(nodes_[node], depth), gap_, alongX ? first.cx : first.cy,
                                            alongX ? second.cx : second.cy, maximumSpan_);
    return minimum_[node] = combined;
  }

  const BspTree& tree_;
  const std::vector<BspNode>& nodes_;
  const LONG gap_;
  const Axis firstAxis_;
  const LONG maximumSpan_;
  std::vector<NodeTopology> topology_;
  const std::vector<WindowConstraints>* constraints_ = nullptr;
  BspAdaptation adaptation_{};
  std::vector<SIZE> minimum_;  // Fitted runs only: see MeasureMinimum.
};

// The preferred BSP layout of `tree` in `area` (see BspPlacement). Pure.
inline void LayoutBsp(const BspTree& tree, const std::vector<HWND>& tiled, const RECT& area, LONG gap,
                      BspLayoutResult* out) {
  out->rects.assign(tiled.size(), RECT{});
  out->splits.clear();
  if (tiled.empty() || tree.Empty()) return;
  BspPlacement(tree, tiled, area, gap).Place(0, 0, area, out);
}

// The BSP layout fitted to `constraints` (one per tiled window): adaptive
// splits move from their ratios only as far as minimums require, within
// `adaptation`. Returns false with `out` partly written if a window still
// can't fit. The saved ratios are unchanged.
inline bool FitBspLayout(const BspTree& tree, const std::vector<HWND>& tiled, const RECT& area, LONG gap,
                         const std::vector<WindowConstraints>& constraints, BspAdaptation adaptation,
                         BspLayoutResult* out) {
  out->rects.assign(tiled.size(), RECT{});
  out->splits.clear();
  if (tiled.empty() || tree.Empty()) return true;
  BspPlacement placement(tree, tiled, area, gap);
  placement.Fit(&constraints, adaptation);
  return placement.Place(0, 0, area, out);
}

// The splits above tiled `window` in `tree`, for SharedSplits::SolveBspPath:
// fitting that path with any minimum for `window` gives the tile FitBspLayout
// would, the other windows keeping `constraints` (one per tiled window). Pure.
inline std::vector<SharedSplits::BspPathSplit> BspPathTo(
    const BspTree& tree, const std::vector<HWND>& tiled, const RECT& area, LONG gap,
    const std::vector<WindowConstraints>& constraints, BspAdaptation adaptation, HWND window) {
  if (tiled.empty() || tree.Empty()) return {};
  BspPlacement placement(tree, tiled, area, gap);
  placement.Fit(&constraints, adaptation);
  return placement.PathTo(window);
}

}  // namespace Layout
