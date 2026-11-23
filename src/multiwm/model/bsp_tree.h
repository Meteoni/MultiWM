// The BSP layout's split tree: which windows share which part of the work area.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// Like bspwm, qtile's Bsp and Hyprland's dwindle, the tree is kept and edited in
// place rather than derived from the window order. One rule for every arrival,
// one for every departure:
//  - An arriving window (new, restored, re-tiled or moved here) splits the
//    focused window's leaf, as bspwm does, or the largest leaf, and takes the
//    second half.
//  - A leaving window (closed, floated, minimized, hidden or moved away) gives
//    its space to its sibling, so only the sibling's windows change size
//    (moving up a level turns splits that still follow the depth rule).
//  - A maximized window is still on screen, so it keeps its leaf; the layout
//    skips it and its sibling covers its space until it is restored.
// Geometry is computed by Layout::LayoutBsp (layout/bsp_layout.h).

namespace Layout { enum class Axis; }

namespace Model {

// One node in preorder. A leaf holds a window; a split (window == nullptr) is
// followed by its first subtree, then its second. A split follows its depth
// unless it needed an alternate direction to fit when it was created.
struct BspNode {
  HWND window = nullptr;
  double ratio = 0.5;  // Splits only: the first side's saved share.
  std::optional<Layout::Axis> direction = std::nullopt;
  HWND pendingDirectionWindow = nullptr;  // Arrival that created this split, until it settles.
  bool operator==(const BspNode&) const = default;
};

class BspTree {
 public:
  static constexpr size_t kNotFound = static_cast<size_t>(-1);

  const std::vector<BspNode>& Nodes() const { return nodes_; }
  bool Empty() const { return nodes_.empty(); }
  bool operator==(const BspTree&) const = default;

  static bool IsSplit(const BspNode& node) { return node.window == nullptr; }

  // One past the last node of the subtree that starts at `node`.
  size_t SubtreeEnd(size_t node) const {
    size_t open = 1;
    size_t index = node;
    while (open > 0 && index < nodes_.size()) {
      open += IsSplit(nodes_[index]) ? 1 : -1;
      ++index;
    }
    return index;
  }
  // The second child of split `node` (its first child is node + 1).
  size_t SecondChild(size_t node) const { return SubtreeEnd(node + 1); }

  // The windows in leaf order: left to right, top to bottom.
  std::vector<HWND> Leaves() const {
    std::vector<HWND> leaves;
    for (const BspNode& node : nodes_) {
      if (!IsSplit(node)) leaves.push_back(node.window);
    }
    return leaves;
  }

  // The node holding `window`, or kNotFound.
  size_t FindLeaf(HWND window) const {
    if (!window) return kNotFound;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (nodes_[i].window == window) return i;
    }
    return kNotFound;
  }
  bool Contains(HWND window) const { return FindLeaf(window) != kNotFound; }

  // True when `shown` accepts a leaf of the subtree at `node`.
  bool AnyLeaf(size_t node, const std::function<bool(HWND)>& shown) const {
    for (size_t i = node, end = SubtreeEnd(node); i < end; ++i) {
      if (!IsSplit(nodes_[i]) && shown(nodes_[i].window)) return true;
    }
    return false;
  }

  // The split whose child is `node`, or kNotFound for the root.
  size_t Parent(size_t node) const {
    for (size_t i = node; i-- > 0;) {
      if (IsSplit(nodes_[i]) && (i + 1 == node || SecondChild(i) == node)) return i;
    }
    return kNotFound;
  }

  // Makes `window` the only leaf. False unless the tree is empty.
  bool AddRoot(HWND window) {
    if (!window || !nodes_.empty()) return false;
    nodes_.push_back({window});
    return true;
  }

  // Splits the anchor at 50/50 and gives the second half to the new window.
  bool SplitLeaf(HWND anchor, HWND window, bool allowAlternateDirection = false) {
    const size_t leaf = FindLeaf(anchor);
    if (leaf == kNotFound || !window || Contains(window)) return false;
    const BspNode existing = nodes_[leaf];
    nodes_[leaf] = BspNode{};
    if (allowAlternateDirection) nodes_[leaf].pendingDirectionWindow = window;
    nodes_.insert(nodes_.begin() + leaf + 1, {existing, BspNode{window}});
    return true;
  }

  void ClearPendingDirection(HWND window = nullptr) {
    for (BspNode& node : nodes_) {
      if (!window || node.pendingDirectionWindow == window) node.pendingDirectionWindow = nullptr;
    }
  }

  // The window whose leaf gets the largest share of the area when only the
  // leaves `shown` accepts are laid out, the first of equals; null when none
  // is shown. Shares come from the ratios alone, so no work area is needed:
  // gaps and rounding move them by a few pixels at most.
  HWND LargestLeaf(const std::function<bool(HWND)>& shown) const {
    if (nodes_.empty()) return nullptr;
    HWND largest = nullptr;
    double largestShare = 0.0;
    auto visit = [&](auto&& self, size_t node, double share) -> void {
      if (!IsSplit(nodes_[node])) {
        if (shown(nodes_[node].window) && share > largestShare * (1.0 + 1e-9)) {
          largest = nodes_[node].window;
          largestShare = share;
        }
        return;
      }
      // A side with nothing shown gives the whole share to the other, as the layout does.
      const double ratio = SharedSplits::ClampSplitRatio(nodes_[node].ratio);
      const size_t second = SecondChild(node);
      self(self, node + 1, AnyLeaf(second, shown) ? share * ratio : share);
      self(self, second, AnyLeaf(node + 1, shown) ? share * (1.0 - ratio) : share);
    };
    visit(visit, 0, 1.0);
    return largest;
  }

  // Removes `window`'s leaf; its sibling, with its own splits, takes the
  // parent's place. False when `window` has no leaf.
  bool Remove(HWND window) {
    const size_t leaf = FindLeaf(window);
    if (leaf == kNotFound) return false;
    const size_t parent = Parent(leaf);
    if (parent == kNotFound) {
      nodes_.clear();
      return true;
    }
    const size_t second = SecondChild(parent);
    const size_t siblingBegin = leaf == parent + 1 ? second : parent + 1;
    const size_t siblingEnd = SubtreeEnd(siblingBegin);
    std::vector<BspNode> nodes(nodes_.begin(), nodes_.begin() + parent);
    nodes.insert(nodes.end(), nodes_.begin() + siblingBegin, nodes_.begin() + siblingEnd);
    nodes.insert(nodes.end(), nodes_.begin() + SubtreeEnd(parent), nodes_.end());
    nodes_ = std::move(nodes);
    ClearPendingDirection(window);
    return true;
  }

  // Exchanges two windows' leaves. False unless both have one.
  bool SwapWindows(HWND first, HWND second) {
    const size_t a = FindLeaf(first);
    const size_t b = FindLeaf(second);
    if (a == kNotFound || b == kNotFound) return false;
    std::swap(nodes_[a].window, nodes_[b].window);
    return true;
  }

  void Clear() { nodes_.clear(); }

  // A split's saved first-side share. Only ratio and direction can be changed
  // in place; which window a leaf holds changes through the operations above,
  // so the shape of the tree stays valid.
  double& SplitRatio(size_t node) {
    assert(node < nodes_.size() && IsSplit(nodes_[node]));
    return nodes_[node].ratio;
  }
  void SetSplitDirection(size_t node, std::optional<Layout::Axis> direction) {
    assert(node < nodes_.size() && IsSplit(nodes_[node]));
    nodes_[node].direction = direction;
  }

 private:
  std::vector<BspNode> nodes_;
};

}  // namespace Model

using Model::BspNode;
using Model::BspTree;
