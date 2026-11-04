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

static LONG RectWidth(const RECT& rect) {
  return rect.right - rect.left;
}

static LONG RectHeight(const RECT& rect) {
  return rect.bottom - rect.top;
}

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

std::vector<LONG> ComputeWeightedSizes(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    LONG* outEffectiveGap = nullptr) {
  size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (outEffectiveGap) *outEffectiveGap = 0;
  if (count == 0 || totalSize <= 0) return sizes;

  LONG effectiveGap = 0;
  if (count > 1 && totalSize > static_cast<LONG>(count)) {
    const LONG maximumGap =
        (totalSize - static_cast<LONG>(count)) /
        static_cast<LONG>(count - 1);
    effectiveGap = std::clamp<LONG>(gap, 0, maximumGap);
  }
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
  LONG position = alongY ? area.top : area.left;

  for (size_t i = 0; i < windowCount; ++i) {
    LONG size = sizes[i];
    LONG end = (i == windowCount - 1) ? (alongY ? area.bottom : area.right) : position + size;
    outRects[i] = alongY ? RECT{area.left, position, area.right, end} : RECT{position, area.top, end, area.bottom};
    position = end + effectiveGap;
  }
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

  if (alongY) {
    outRects[0] = {area.left, area.top, area.right, area.top + masterSize};
    LONG stackTop = area.top + masterSize + masterGap;
    LONG stackGap = 0;
    std::vector<LONG> sizes = ComputeWeightedSizes(
        area.right - area.left, gap, weights, &stackGap);
    LONG x = area.left;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.right : x + sizes[i];
      outRects[i + 1] = {x, stackTop, end, area.bottom};
      x = end + stackGap;
    }
  } else {
    outRects[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    LONG stackLeft = area.left + masterSize + masterGap;
    LONG stackGap = 0;
    std::vector<LONG> sizes = ComputeWeightedSizes(
        area.bottom - area.top, gap, weights, &stackGap);
    LONG y = area.top;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.bottom : y + sizes[i];
      outRects[i + 1] = {stackLeft, y, area.right, end};
      y = end + stackGap;
    }
  }
}

struct DwindleSplit {
  Axis axis = Axis::Y; // Unvisited nodes retain the legacy Y-axis default.
  LONG usableSpan = 0;
  LONG firstSpan = 0;
  RECT first{};
  RECT remaining{};
};

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
  RECT remaining = area;
  if (splitAlongX) {
    if (reverse) {
      outRects[startIndex] = {
          area.right - firstSpan, area.top, area.right, area.bottom};
      remaining.right = outRects[startIndex].left - effectiveGap;
    } else {
      outRects[startIndex] = {
          area.left, area.top, area.left + firstSpan, area.bottom};
      remaining.left = outRects[startIndex].right + effectiveGap;
    }
  } else {
    if (reverse) {
      outRects[startIndex] = {
          area.left, area.bottom - firstSpan, area.right, area.bottom};
      remaining.bottom = outRects[startIndex].top - effectiveGap;
    } else {
      outRects[startIndex] = {
          area.left, area.top, area.right, area.top + firstSpan};
      remaining.top = outRects[startIndex].bottom + effectiveGap;
    }
  }

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

  LONG effectiveGap = 0;
  if (count > 1 && totalSize > static_cast<LONG>(count)) {
    const LONG maximumGap =
        (totalSize - static_cast<LONG>(count)) /
        static_cast<LONG>(count - 1);
    effectiveGap = std::clamp<LONG>(gap, 0, maximumGap);
  }
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

  LONG position = alongY ? area.top : area.left;
  for (size_t i = 0; i < count; ++i) {
    const LONG end =
        i + 1 == count
            ? (alongY ? area.bottom : area.right)
            : position + sizes[i];
    (*outRects)[i] = alongY
        ? RECT{area.left, position, area.right, end}
        : RECT{position, area.top, end, area.bottom};
    position = end + effectiveGap;
  }
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

  if (alongY) {
    (*outRects)[0] = {
        area.left, area.top, area.right, area.top + masterSize};
    const LONG stackTop = area.top + masterSize + masterGap;
    LONG x = area.left;
    for (size_t i = 0; i < innerSizes.size(); ++i) {
      const LONG end =
          i + 1 == innerSizes.size() ? area.right : x + innerSizes[i];
      (*outRects)[i + 1] = {x, stackTop, end, area.bottom};
      x = end + stackGap;
    }
  } else {
    (*outRects)[0] = {
        area.left, area.top, area.left + masterSize, area.bottom};
    const LONG stackLeft = area.left + masterSize + masterGap;
    LONG y = area.top;
    for (size_t i = 0; i < innerSizes.size(); ++i) {
      const LONG end =
          i + 1 == innerSizes.size() ? area.bottom : y + innerSizes[i];
      (*outRects)[i + 1] = {stackLeft, y, area.right, end};
      y = end + stackGap;
    }
  }
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

