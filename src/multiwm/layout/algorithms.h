// Layout calculations use physical screen pixels and never query native windows.
// Ratios and weights are preferences; callers own all model/placement changes.
namespace Layout {

static inline bool Differs(LONG a, LONG b, LONG tol = 1) {
    return (a > b) ? (a - b > tol) : (b - a > tol);
}

inline double ClampDouble(double value, double minVal, double maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}

// Divider ratios (master size, Dwindle splits) are kept within 10%..90% so neither
// side of a divider can collapse to nothing.
inline double ClampSplitRatio(double ratio) {
  return ClampDouble(ratio, 0.1, 0.9);
}

// Pixels left for the two sides of a split after removing the gap between them.
// The gap shrinks if needed so each side keeps at least one pixel.
// Requires span >= 2.
inline LONG UsableSplitSpan(LONG span, LONG gap) {
  return span - std::clamp<LONG>(gap, 0, span - 2);
}

// Size of the first side of a split: `ratio` of `usable`, rounded to whole pixels,
// always leaving at least one pixel for each side. Requires usable >= 2.
inline LONG SplitLeadingSpan(LONG usable, double ratio) {
  return std::clamp<LONG>(static_cast<LONG>(std::llround(usable * ratio)), 1, usable - 1);
}

// Orders `indices` by distance from `origin` (closest first); ties keep the
// lower index first. Used to prefer moving a window as little as possible.
inline void SortByDistanceFrom(std::vector<size_t>& indices, size_t origin) {
  auto distance = [origin](size_t index) {
    return index > origin ? index - origin : origin - index;
  };
  std::stable_sort(indices.begin(), indices.end(), [&](size_t first, size_t second) {
    return distance(first) != distance(second) ? distance(first) < distance(second)
                                               : first < second;
  });
}

std::vector<double> DefaultWeights(size_t count) {
  return std::vector<double>(count, 1.0);
}

static inline long long RectAreaLL(const RECT& r) {
  long long w = (long long)r.right - (long long)r.left;
  long long h = (long long)r.bottom - (long long)r.top;
  if (w <= 0 || h <= 0) return 0;
  return w * h;
}

static LONG RectWidth(const RECT& rect) { return SharedGeometry::Width(rect); }
static LONG RectHeight(const RECT& rect) { return SharedGeometry::Height(rect); }

static bool RectsNear(const RECT& a, const RECT& b, LONG tolerance = Model::kPlacementTolerancePx) {
  return !Differs(a.left, b.left, tolerance) &&
         !Differs(a.top, b.top, tolerance) &&
         !Differs(a.right, b.right, tolerance) &&
         !Differs(a.bottom, b.bottom, tolerance);
}

// Axis along which windows divide the available span: X is width, Y is height.
// For MasterStack this is the master divider; stack members use the other axis.
enum class Axis { X, Y };

// Divider axis of a weighted layout: Master + Stack (Vertical) and Columns split
// the width; the Horizontal and Rows variants split the height.
constexpr Axis LayoutAxis(TileLayout layout) {
  return layout == TileLayout::MasterStackH || layout == TileLayout::Rows ? Axis::Y : Axis::X;
}

// The gap between `count` members sharing `totalSize` pixels: `gap`, reduced so
// every member keeps at least one pixel. 0 for fewer than two members.
static LONG WeightedLayoutGap(LONG totalSize, LONG gap, size_t count) {
  if (count < 2 || totalSize <= static_cast<LONG>(count)) return 0;
  const LONG maximumGap =
      (totalSize - static_cast<LONG>(count)) / static_cast<LONG>(count - 1);
  return std::clamp<LONG>(gap, 0, maximumGap);
}

// Cuts `band` along `axis` into consecutive rectangles of `sizes` pixels with
// `gap` between them, written to out[0..sizes.size()). The last rectangle always
// ends at the band's far edge, absorbing rounding.
static void SplitBandAlongAxis(
    const RECT& band, Axis axis, const std::vector<LONG>& sizes, LONG gap, RECT* out) {
  const bool alongY = axis == Axis::Y;
  LONG position = alongY ? band.top : band.left;
  for (size_t i = 0; i < sizes.size(); ++i) {
    const LONG end = i + 1 == sizes.size() ? (alongY ? band.bottom : band.right) : position + sizes[i];
    out[i] = alongY ? RECT{band.left, position, band.right, end} : RECT{position, band.top, end, band.bottom};
    position = end + gap;
  }
}

// Writes the master rectangle (`masterSize` pixels along `axis`, from the
// top-left) to out[0] and the stack, split across the other axis by
// `stackSizes`, to out[1..]. `masterGap` separates master and stack.
static void PlaceMasterStackRects(
    const RECT& area, Axis axis, LONG masterSize, LONG masterGap,
    const std::vector<LONG>& stackSizes, LONG stackGap, RECT* out) {
  if (axis == Axis::Y) {
    out[0] = {area.left, area.top, area.right, area.top + masterSize};
    const RECT stack{area.left, area.top + masterSize + masterGap, area.right, area.bottom};
    SplitBandAlongAxis(stack, Axis::X, stackSizes, stackGap, out + 1);
  } else {
    out[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    const RECT stack{area.left + masterSize + masterGap, area.top, area.right, area.bottom};
    SplitBandAlongAxis(stack, Axis::Y, stackSizes, stackGap, out + 1);
  }
}

std::vector<LONG> ComputeWeightedSizes(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    LONG* outEffectiveGap = nullptr) {
  size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (outEffectiveGap) *outEffectiveGap = 0;
  if (count == 0 || totalSize <= 0) return sizes;

  const LONG effectiveGap = WeightedLayoutGap(totalSize, gap, count);
  if (outEffectiveGap) *outEffectiveGap = effectiveGap;

  LONG available =
      totalSize - effectiveGap * static_cast<LONG>(count - 1);
  if (available <= 0) return sizes;

  double sum = 0.0;
  for (double weight : weights) {
    sum += std::isfinite(weight) && weight > 0.0 ? weight : 1.0;
  }

  LONG used = 0;
  double remainingSum = sum;
  for (size_t i = 0; i < count; ++i) {
    double w = weights[i];
    if (!std::isfinite(w) || w <= 0.0) w = 1.0;
    LONG remainingSlots = (LONG)(count - i - 1);
    LONG size = 0;
    if (i == count - 1) {
      size = available - used;

    } else {
      double ratio = w / remainingSum;
      size = static_cast<LONG>(std::llround(static_cast<double>(available - used) * ratio));
      if (size < 1) size = 1;
      LONG maxSize = available - used - remainingSlots;
      if (size > maxSize) size = maxSize;
    }
    sizes[i] = size;
    used += size;
    remainingSum -= w;
    if (remainingSum <= 0.0) remainingSum = 1.0;
  }
  return sizes;
}

void LayoutGridWeighted(
    const RECT& area, LONG gap, size_t windowCount,
    std::vector<RECT>& outRects, Axis axis,
    const std::vector<double>& weights) {
  const bool alongY = axis == Axis::Y;
  outRects.resize(windowCount);
  if (windowCount == 0) return;

  const std::vector<double>* effectiveWeights = &weights;
  std::vector<double> defaultWeights;
  if (weights.size() != windowCount) {
    defaultWeights = DefaultWeights(windowCount);
    effectiveWeights = &defaultWeights;
  }

  LONG totalSize = alongY ? (area.bottom - area.top) : (area.right - area.left);
  LONG effectiveGap = 0;
  std::vector<LONG> sizes =
      ComputeWeightedSizes(totalSize, gap, *effectiveWeights, &effectiveGap);
  SplitBandAlongAxis(area, axis, sizes, effectiveGap, outRects.data());
}

void LayoutMasterStackWeighted(
    const RECT& area, LONG gap, size_t windowCount,
    std::vector<RECT>& outRects, Axis axis,
    double masterRatio, const std::vector<double>& stackWeights) {
  const bool alongY = axis == Axis::Y;
  outRects.resize(windowCount);
  if (windowCount == 0) return;
  if (windowCount == 1) {
    outRects[0] = area;
    return;
  }

  LONG totalSize = alongY ? (area.bottom - area.top) : (area.right - area.left);
  if (totalSize < 2) {
    std::fill(outRects.begin(), outRects.end(), area);
    return;
  }

  const LONG masterGap = std::clamp<LONG>(gap, 0, totalSize - 2);
  LONG masterSize = static_cast<LONG>(
      std::llround((totalSize - masterGap) * masterRatio));
  if (masterSize < 1) masterSize = 1;
  if (masterSize > totalSize - masterGap - 1) {
    masterSize = totalSize - masterGap - 1;
  }

  size_t stackCount = windowCount - 1;
  std::vector<double> weights = stackWeights;
  if (weights.size() != stackCount) weights = DefaultWeights(stackCount);

  LONG stackGap = 0;
  const std::vector<LONG> sizes = ComputeWeightedSizes(
      alongY ? area.right - area.left : area.bottom - area.top, gap, weights, &stackGap);
  PlaceMasterStackRects(area, axis, masterSize, masterGap, sizes, stackGap, outRects.data());
}

struct DwindleSplit {
  Axis axis = Axis::Y; // Unvisited nodes retain the legacy Y-axis default.
  LONG usableSpan = 0;
  LONG firstSpan = 0;
  RECT first{};
  RECT remaining{};
};

// Cuts `area` into a leading rectangle `firstSpan` pixels wide (splitAlongX) or
// tall, and the remainder, with `effectiveGap` pixels between them. With
// `reverse`, the leading rectangle sits at the right/bottom edge instead.
static void SplitDwindleArea(
    const RECT& area, bool splitAlongX, LONG firstSpan, LONG effectiveGap, bool reverse,
    RECT* first, RECT* remaining) {
  *remaining = area;
  if (splitAlongX) {
    if (reverse) {
      *first = {area.right - firstSpan, area.top, area.right, area.bottom};
      remaining->right = first->left - effectiveGap;
    } else {
      *first = {area.left, area.top, area.left + firstSpan, area.bottom};
      remaining->left = first->right + effectiveGap;
    }
  } else {
    if (reverse) {
      *first = {area.left, area.bottom - firstSpan, area.right, area.bottom};
      remaining->bottom = first->top - effectiveGap;
    } else {
      *first = {area.left, area.top, area.right, area.top + firstSpan};
      remaining->top = first->bottom + effectiveGap;
    }
  }
}

// Ordered-chain Dwindle: each node gives its first child to startIndex and
// recursively gives its second child to all later windows. Ratios belong to
// those split nodes, so swapping windows exchanges leaves without reshaping the
// tree. Fibonacci-style axes alternate by depth, starting side-by-side;
// resizing an ancestor never rotates a descendant's divider.
void LayoutDwindle(
    const RECT& area, LONG gap, size_t startIndex, size_t count,
    const std::vector<double>& splitRatios, bool reverse,
    std::vector<RECT>& outRects,
    std::vector<DwindleSplit>* outSplits = nullptr) {
  if (count == 0) return;
  if (count == 1) {
    outRects[startIndex] = area;
    return;
  }

  const LONG width = area.right - area.left;
  const LONG height = area.bottom - area.top;
  const bool splitAlongX = (startIndex % 2) == 0;

  const LONG span = splitAlongX ? width : height;
  if (span < 2) {
    std::fill(
        outRects.begin() + startIndex,
        outRects.begin() + startIndex + count, area);
    return;
  }

  const LONG usableSpan = UsableSplitSpan(span, gap);
  const LONG effectiveGap = span - usableSpan;
  const double ratio = startIndex < splitRatios.size()
      ? ClampSplitRatio(splitRatios[startIndex])
      : 0.5;
  const LONG firstSpan = SplitLeadingSpan(usableSpan, ratio);
  RECT remaining{};
  SplitDwindleArea(area, splitAlongX, firstSpan, effectiveGap, reverse,
                   &outRects[startIndex], &remaining);

  if (outSplits) {
    if (outSplits->size() < startIndex + count - 1) {
      outSplits->resize(startIndex + count - 1);
    }
    (*outSplits)[startIndex] = {
        splitAlongX ? Axis::X : Axis::Y, usableSpan, firstSpan,
        outRects[startIndex], remaining};
  }

  LayoutDwindle(
      remaining, gap, startIndex + 1, count - 1,
      splitRatios, reverse, outRects, outSplits);
}

struct ConstraintExtent {
  LONG minWidth = 1;
  LONG minHeight = 1;
};

static bool ConstraintDimensionFits(LONG value, LONG minimum) {
  return value >= minimum;
}

// Weighted allocation with the same gap-degradation semantics as the ordinary
// layout path. Relative weights remain preferences; members pin to their native
// minimum only when required to make the full span legal.
static bool ComputeConstrainedWeightedSizes(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    const std::vector<LONG>& minimums,
    std::vector<LONG>* outSizes, LONG* outEffectiveGap = nullptr,
    double* outWeightPerPixel = nullptr) {
  if (outWeightPerPixel) *outWeightPerPixel = 0.0;
  if (!outSizes) return false;
  const size_t count = minimums.size();
  outSizes->assign(count, 0);
  if (outEffectiveGap) *outEffectiveGap = 0;
  if (count == 0) return true;
  if (totalSize <= 0) return false;

  const LONG effectiveGap = WeightedLayoutGap(totalSize, gap, count);
  if (outEffectiveGap) *outEffectiveGap = effectiveGap;

  const long long available64 =
      static_cast<long long>(totalSize) -
      static_cast<long long>(effectiveGap) * (count - 1);
  if (available64 < static_cast<long long>(count) ||
      available64 > LONG_MAX) {
    return false;
  }
  const LONG available = static_cast<LONG>(available64);

  std::vector<LONG> mins(count);
  std::vector<double> normalizedWeights(count, 1.0);
  long long minSum = 0;
  for (size_t i = 0; i < count; ++i) {
    mins[i] = std::max<LONG>(1, minimums[i]);
    minSum += mins[i];
    if (i < weights.size() &&
        std::isfinite(weights[i]) && weights[i] > 0.0) {
      normalizedWeights[i] = weights[i];
    }
  }
  if (minSum > available) return false;

  auto sumAt = [&](double lambda) {
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
      sum += std::max(
          lambda * normalizedWeights[i], static_cast<double>(mins[i]));
    }
    return sum;
  };

  double low = 0.0;
  double high = 1.0;
  while (sumAt(high) < static_cast<double>(available) && high < 1.0e12) {
    high *= 2.0;
  }
  for (int iteration = 0; iteration < 80; ++iteration) {
    const double mid = (low + high) * 0.5;
    if (sumAt(mid) < static_cast<double>(available)) {
      low = mid;
    } else {
      high = mid;
    }
  }

  if (outWeightPerPixel) *outWeightPerPixel = 1.0 / high;

  std::vector<double> raw(count);
  long long roundedSum = 0;
  for (size_t i = 0; i < count; ++i) {
    raw[i] = std::max(
        high * normalizedWeights[i], static_cast<double>(mins[i]));
    LONG rounded = static_cast<LONG>(std::floor(raw[i]));
    rounded = std::max(rounded, mins[i]);
    (*outSizes)[i] = rounded;
    roundedSum += rounded;
  }

  while (roundedSum < available) {
    size_t best = count;
    double bestFraction = -1.0;
    for (size_t i = 0; i < count; ++i) {
      const double fraction = raw[i] - std::floor(raw[i]);
      if (best == count || fraction > bestFraction) {
        best = i;
        bestFraction = fraction;
      }
    }
    if (best == count) return false;
    ++(*outSizes)[best];
    ++roundedSum;
    raw[best] = static_cast<double>((*outSizes)[best]);
  }

  while (roundedSum > available) {
    size_t best = count;
    double bestFraction = 2.0;
    for (size_t i = 0; i < count; ++i) {
      if ((*outSizes)[i] <= mins[i]) continue;
      const double fraction = raw[i] - std::floor(raw[i]);
      if (best == count || fraction < bestFraction) {
        best = i;
        bestFraction = fraction;
      }
    }
    if (best == count) return false;
    --(*outSizes)[best];
    --roundedSum;
    raw[best] = static_cast<double>((*outSizes)[best]);
  }
  return true;
}

static ConstraintExtent ToConstraintExtent(
    const Model::WindowConstraints& c) {
  const LONG minWidth =
      c.valid ? std::max<LONG>(1, c.minWidth) : 1;
  const LONG minHeight =
      c.valid ? std::max<LONG>(1, c.minHeight) : 1;
  return {minWidth, minHeight};
}

static bool ConstraintAreaFits(
    const RECT& area, const ConstraintExtent& c) {
  const LONG width = area.right - area.left;
  const LONG height = area.bottom - area.top;
  return width > 0 && height > 0 &&
         ConstraintDimensionFits(width, c.minWidth) &&
         ConstraintDimensionFits(height, c.minHeight);
}

static bool ChooseConstrainedSplit(
    LONG span, LONG gap, double preferredFirstRatio,
    LONG firstMin, LONG secondMin,
    LONG* outFirstSize, LONG* outEffectiveGap = nullptr) {
  if (!outFirstSize || span < 2) return false;
  const LONG effectiveGap = std::clamp<LONG>(gap, 0, span - 2);
  if (outEffectiveGap) *outEffectiveGap = effectiveGap;
  const LONG available = span - effectiveGap;

  const LONG low = std::max<LONG>(1, firstMin);
  const LONG high = available - std::max<LONG>(1, secondMin);
  if (low > high) return false;

  const LONG preferred = static_cast<LONG>(
      std::llround(static_cast<double>(available) * preferredFirstRatio));
  *outFirstSize = std::clamp(preferred, low, high);
  return true;
}

bool LayoutGridConstrained(
    const RECT& area, LONG gap, const std::vector<double>& weights,
    const std::vector<Model::WindowConstraints>& constraints,
    Axis axis, std::vector<RECT>* outRects,
    double* outWeightPerPixel = nullptr) {
  const bool alongY = axis == Axis::Y;
  if (outWeightPerPixel) *outWeightPerPixel = 0.0;
  if (!outRects) return false;
  const size_t count = constraints.size();
  outRects->assign(count, RECT{});
  if (count == 0) return true;

  const LONG crossSpan =
      alongY ? area.right - area.left : area.bottom - area.top;
  std::vector<LONG> mins(count);
  for (size_t i = 0; i < count; ++i) {
    const ConstraintExtent c = ToConstraintExtent(constraints[i]);
    if (alongY) {
      if (!ConstraintDimensionFits(crossSpan, c.minWidth)) {
        return false;
      }
      mins[i] = c.minHeight;
    } else {
      if (!ConstraintDimensionFits(crossSpan, c.minHeight)) {
        return false;
      }
      mins[i] = c.minWidth;
    }
  }

  const LONG total =
      alongY ? area.bottom - area.top : area.right - area.left;
  LONG effectiveGap = 0;
  std::vector<LONG> sizes;
  if (!ComputeConstrainedWeightedSizes(
          total, gap, weights, mins, &sizes, &effectiveGap,
          outWeightPerPixel)) {
    return false;
  }
  SplitBandAlongAxis(area, axis, sizes, effectiveGap, outRects->data());
  return true;
}

bool LayoutMasterStackConstrained(
    const RECT& area, LONG gap, Axis axis, double masterRatio,
    const std::vector<double>& stackWeights,
    const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects,
    double* outStackWeightPerPixel = nullptr) {
  const bool alongY = axis == Axis::Y;
  if (outStackWeightPerPixel) *outStackWeightPerPixel = 0.0;
  if (!outRects) return false;
  const size_t count = constraints.size();
  outRects->assign(count, RECT{});
  if (count == 0) return true;
  if (count == 1) {
    if (!ConstraintAreaFits(area, ToConstraintExtent(constraints[0]))) {
      return false;
    }
    (*outRects)[0] = area;
    return true;
  }

  const ConstraintExtent master = ToConstraintExtent(constraints[0]);
  const LONG areaWidth = area.right - area.left;
  const LONG areaHeight = area.bottom - area.top;
  const LONG splitSpan = alongY ? areaHeight : areaWidth;
  const LONG innerSpan = alongY ? areaWidth : areaHeight;

  if (alongY) {
    if (!ConstraintDimensionFits(areaWidth, master.minWidth)) {
      return false;
    }
  } else if (!ConstraintDimensionFits(areaHeight, master.minHeight)) {
    return false;
  }

  LONG stackSplitMin = 1;
  std::vector<LONG> innerMins;
  innerMins.reserve(count - 1);
  for (size_t i = 1; i < count; ++i) {
    const ConstraintExtent c = ToConstraintExtent(constraints[i]);
    if (alongY) {
      stackSplitMin = std::max(stackSplitMin, c.minHeight);
      innerMins.push_back(c.minWidth);
    } else {
      stackSplitMin = std::max(stackSplitMin, c.minWidth);
      innerMins.push_back(c.minHeight);
    }
  }

  LONG masterSize = 0;
  LONG masterGap = 0;
  if (!ChooseConstrainedSplit(
          splitSpan, gap, masterRatio,
          alongY ? master.minHeight : master.minWidth,
          stackSplitMin,
          &masterSize, &masterGap)) {
    return false;
  }

  LONG stackGap = 0;
  std::vector<LONG> innerSizes;
  if (!ComputeConstrainedWeightedSizes(
          innerSpan, gap, stackWeights, innerMins,
          &innerSizes, &stackGap, outStackWeightPerPixel)) {
    return false;
  }
  PlaceMasterStackRects(area, axis, masterSize, masterGap, innerSizes, stackGap, outRects->data());
  return true;
}

bool LayoutMonocleConstrained(
    const RECT& area,
    const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  outRects->assign(constraints.size(), area);
  for (const auto& constraint : constraints) {
    if (!ConstraintAreaFits(area, ToConstraintExtent(constraint))) {
      return false;
    }
  }
  return true;
}

}  // namespace Layout

