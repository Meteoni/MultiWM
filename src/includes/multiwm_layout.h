namespace Layout {

struct ConstraintExtent {
  LONG minWidth = 1;
  LONG minHeight = 1;
  LONG maxWidth = LONG_MAX;
  LONG maxHeight = LONG_MAX;
};

static LONG EffectiveMax(LONG value, LONG available) {
  return value == LONG_MAX ? available : std::min(value, available);
}

static bool DimensionFits(LONG value, LONG minimum, LONG maximum) {
  return value >= minimum && (maximum == LONG_MAX || value <= maximum);
}

// Finds the closest weighted allocation that satisfies hard min/max bounds and
// exactly consumes the usable span. This is a water-filling solve: unconstrained
// items preserve their relative weights while constrained items pin to a bound.
static bool ComputeConstrainedWeightedSizes(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    const std::vector<LONG>& minimums, const std::vector<LONG>& maximums,
    std::vector<LONG>* outSizes) {
  if (!outSizes || minimums.size() != maximums.size()) return false;
  const size_t count = minimums.size();
  outSizes->assign(count, 0);
  if (count == 0) return true;

  const long long gapTotal = static_cast<long long>(gap) * (count - 1);
  const long long available64 = static_cast<long long>(totalSize) - gapTotal;
  if (available64 < static_cast<long long>(count) || available64 > LONG_MAX) {
    return false;
  }
  const LONG available = static_cast<LONG>(available64);

  std::vector<LONG> mins(count), maxs(count);
  long long minSum = 0, maxSum = 0;
  std::vector<double> normalizedWeights(count, 1.0);
  for (size_t i = 0; i < count; ++i) {
    mins[i] = std::max<LONG>(1, minimums[i]);
    maxs[i] = std::max<LONG>(mins[i], EffectiveMax(maximums[i], available));
    minSum += mins[i];
    maxSum += maxs[i];
    if (i < weights.size() && weights[i] > 0.0 && std::isfinite(weights[i])) {
      normalizedWeights[i] = weights[i];
    }
  }
  if (minSum > available || maxSum < available) return false;

  auto sumAt = [&](double lambda) {
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
      sum += std::clamp(lambda * normalizedWeights[i],
                        static_cast<double>(mins[i]),
                        static_cast<double>(maxs[i]));
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
    if (sumAt(mid) < static_cast<double>(available)) low = mid;
    else high = mid;
  }

  std::vector<double> raw(count);
  long long sum = 0;
  for (size_t i = 0; i < count; ++i) {
    raw[i] = std::clamp(high * normalizedWeights[i],
                        static_cast<double>(mins[i]),
                        static_cast<double>(maxs[i]));
    LONG rounded = static_cast<LONG>(std::floor(raw[i]));
    rounded = std::clamp(rounded, mins[i], maxs[i]);
    (*outSizes)[i] = rounded;
    sum += rounded;
  }

  while (sum < available) {
    size_t best = count;
    double bestFraction = -1.0;
    for (size_t i = 0; i < count; ++i) {
      if ((*outSizes)[i] >= maxs[i]) continue;
      const double fraction = raw[i] - std::floor(raw[i]);
      if (best == count || fraction > bestFraction) {
        best = i;
        bestFraction = fraction;
      }
    }
    if (best == count) return false;
    ++(*outSizes)[best];
    ++sum;
    raw[best] = static_cast<double>((*outSizes)[best]);
  }
  while (sum > available) {
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
    --sum;
    raw[best] = static_cast<double>((*outSizes)[best]);
  }
  return true;
}

static ConstraintExtent ToExtent(const Model::WindowConstraints& c) {
  return {std::max<LONG>(1, c.minWidth), std::max<LONG>(1, c.minHeight),
          c.maxWidthBounded ? std::max<LONG>(c.minWidth, c.maxWidth) : LONG_MAX,
          c.maxHeightBounded ? std::max<LONG>(c.minHeight, c.maxHeight) : LONG_MAX};
}

static bool AreaFits(const RECT& area, const ConstraintExtent& c) {
  const LONG width = area.right - area.left;
  const LONG height = area.bottom - area.top;
  return width > 0 && height > 0 &&
         DimensionFits(width, c.minWidth, c.maxWidth) &&
         DimensionFits(height, c.minHeight, c.maxHeight);
}

static bool ChooseConstrainedSplit(
    LONG span, LONG gap, double preferredFirstRatio,
    LONG firstMin, LONG firstMax, LONG secondMin, LONG secondMax,
    LONG* firstSize) {
  if (!firstSize || span <= gap + 1) return false;
  const LONG available = span - gap;
  firstMax = EffectiveMax(firstMax, available);
  secondMax = EffectiveMax(secondMax, available);
  const LONG low = std::max<LONG>(firstMin, available - secondMax);
  const LONG high = std::min<LONG>(firstMax, available - secondMin);
  if (low > high) return false;
  const LONG preferred = static_cast<LONG>(
      std::llround(static_cast<double>(available) * preferredFirstRatio));
  *firstSize = std::clamp(preferred, low, high);
  return true;
}

bool LayoutGridConstrained(
    const RECT& area, LONG gap, const std::vector<double>& weights,
    const std::vector<Model::WindowConstraints>& constraints, bool horizontal,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  const size_t count = constraints.size();
  outRects->assign(count, RECT{});
  if (count == 0) return true;

  const LONG crossSpan = horizontal ? area.right - area.left
                                    : area.bottom - area.top;
  std::vector<LONG> mins(count), maxs(count);
  for (size_t i = 0; i < count; ++i) {
    const ConstraintExtent c = ToExtent(constraints[i]);
    if (horizontal) {
      if (!DimensionFits(crossSpan, c.minWidth, c.maxWidth)) return false;
      mins[i] = c.minHeight;
      maxs[i] = c.maxHeight;
    } else {
      if (!DimensionFits(crossSpan, c.minHeight, c.maxHeight)) return false;
      mins[i] = c.minWidth;
      maxs[i] = c.maxWidth;
    }
  }

  const LONG total = horizontal ? area.bottom - area.top
                                : area.right - area.left;
  std::vector<LONG> sizes;
  if (!ComputeConstrainedWeightedSizes(total, gap, weights, mins, maxs, &sizes)) {
    return false;
  }

  LONG position = horizontal ? area.top : area.left;
  for (size_t i = 0; i < count; ++i) {
    const LONG end = (i + 1 == count)
        ? (horizontal ? area.bottom : area.right)
        : position + sizes[i];
    (*outRects)[i] = horizontal
        ? RECT{area.left, position, area.right, end}
        : RECT{position, area.top, end, area.bottom};
    position = end + gap;
  }
  return true;
}

bool LayoutMasterStackConstrained(
    const RECT& area, LONG gap, bool horizontal, double masterRatio,
    const std::vector<double>& stackWeights,
    const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  const size_t count = constraints.size();
  outRects->assign(count, RECT{});
  if (count == 0) return true;
  if (count == 1) {
    const ConstraintExtent only = ToExtent(constraints[0]);
    if (!AreaFits(area, only)) return false;
    (*outRects)[0] = area;
    return true;
  }

  const ConstraintExtent master = ToExtent(constraints[0]);
  const LONG areaWidth = area.right - area.left;
  const LONG areaHeight = area.bottom - area.top;
  const LONG splitSpan = horizontal ? areaHeight : areaWidth;
  const LONG innerSpan = horizontal ? areaWidth : areaHeight;

  if (horizontal) {
    if (!DimensionFits(areaWidth, master.minWidth, master.maxWidth)) return false;
  } else {
    if (!DimensionFits(areaHeight, master.minHeight, master.maxHeight)) return false;
  }

  LONG stackSplitMin = 1;
  LONG stackSplitMax = LONG_MAX;
  std::vector<LONG> innerMins, innerMaxs;
  innerMins.reserve(count - 1);
  innerMaxs.reserve(count - 1);
  for (size_t i = 1; i < count; ++i) {
    const ConstraintExtent c = ToExtent(constraints[i]);
    if (horizontal) {
      stackSplitMin = std::max(stackSplitMin, c.minHeight);
      stackSplitMax = std::min(stackSplitMax, c.maxHeight);
      innerMins.push_back(c.minWidth);
      innerMaxs.push_back(c.maxWidth);
    } else {
      stackSplitMin = std::max(stackSplitMin, c.minWidth);
      stackSplitMax = std::min(stackSplitMax, c.maxWidth);
      innerMins.push_back(c.minHeight);
      innerMaxs.push_back(c.maxHeight);
    }
  }
  if (stackSplitMin > stackSplitMax) return false;

  const LONG masterSplitMin = horizontal ? master.minHeight : master.minWidth;
  const LONG masterSplitMax = horizontal ? master.maxHeight : master.maxWidth;
  LONG masterSize = 0;
  if (!ChooseConstrainedSplit(
          splitSpan, gap, masterRatio, masterSplitMin, masterSplitMax,
          stackSplitMin, stackSplitMax, &masterSize)) {
    return false;
  }

  std::vector<LONG> innerSizes;
  if (!ComputeConstrainedWeightedSizes(
          innerSpan, gap, stackWeights, innerMins, innerMaxs, &innerSizes)) {
    return false;
  }

  if (horizontal) {
    (*outRects)[0] = {area.left, area.top, area.right, area.top + masterSize};
    const LONG stackTop = area.top + masterSize + gap;
    LONG x = area.left;
    for (size_t i = 0; i < innerSizes.size(); ++i) {
      const LONG end = (i + 1 == innerSizes.size()) ? area.right : x + innerSizes[i];
      (*outRects)[i + 1] = {x, stackTop, end, area.bottom};
      x = end + gap;
    }
  } else {
    (*outRects)[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    const LONG stackLeft = area.left + masterSize + gap;
    LONG y = area.top;
    for (size_t i = 0; i < innerSizes.size(); ++i) {
      const LONG end = (i + 1 == innerSizes.size()) ? area.bottom : y + innerSizes[i];
      (*outRects)[i + 1] = {stackLeft, y, area.right, end};
      y = end + gap;
    }
  }
  return true;
}

static LONG SaturatingConstraintSum(LONG a, LONG b, LONG gap) {
  if (a == LONG_MAX || b == LONG_MAX) return LONG_MAX;
  const long long sum = static_cast<long long>(a) + b + gap;
  return sum >= LONG_MAX ? LONG_MAX : static_cast<LONG>(sum);
}

static bool BspAggregate(
    const std::vector<Model::WindowConstraints>& constraints,
    LONG gap, size_t start, size_t count, int depth, ConstraintExtent* out) {
  if (!out || count == 0 || start >= constraints.size()) return false;
  if (count == 1) {
    *out = ToExtent(constraints[start]);
    return out->minWidth <= out->maxWidth && out->minHeight <= out->maxHeight;
  }

  const ConstraintExtent first = ToExtent(constraints[start]);
  ConstraintExtent rest{};
  if (!BspAggregate(constraints, gap, start + 1, count - 1, depth + 1, &rest)) {
    return false;
  }

  const bool splitVertical = (depth % 2 == 0);
  if (splitVertical) {
    out->minWidth = SaturatingConstraintSum(first.minWidth, rest.minWidth, gap);
    out->maxWidth = SaturatingConstraintSum(first.maxWidth, rest.maxWidth, gap);
    out->minHeight = std::max(first.minHeight, rest.minHeight);
    out->maxHeight = std::min(first.maxHeight, rest.maxHeight);
  } else {
    out->minHeight = SaturatingConstraintSum(first.minHeight, rest.minHeight, gap);
    out->maxHeight = SaturatingConstraintSum(first.maxHeight, rest.maxHeight, gap);
    out->minWidth = std::max(first.minWidth, rest.minWidth);
    out->maxWidth = std::min(first.maxWidth, rest.maxWidth);
  }
  return out->minWidth <= out->maxWidth && out->minHeight <= out->maxHeight;
}

static bool LayoutBSPConstrainedRecursive(
    const RECT& area, LONG gap, size_t start, size_t count, int depth,
    const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects) {
  if (count == 0) return true;
  if (count == 1) {
    const ConstraintExtent leaf = ToExtent(constraints[start]);
    if (!AreaFits(area, leaf)) return false;
    (*outRects)[start] = area;
    return true;
  }

  const ConstraintExtent first = ToExtent(constraints[start]);
  ConstraintExtent rest{};
  if (!BspAggregate(constraints, gap, start + 1, count - 1, depth + 1, &rest)) {
    return false;
  }

  const bool splitVertical = (depth % 2 == 0);
  const LONG cross = splitVertical ? area.bottom - area.top : area.right - area.left;
  if (splitVertical) {
    if (!DimensionFits(cross, first.minHeight, first.maxHeight) ||
        !DimensionFits(cross, rest.minHeight, rest.maxHeight)) return false;
  } else {
    if (!DimensionFits(cross, first.minWidth, first.maxWidth) ||
        !DimensionFits(cross, rest.minWidth, rest.maxWidth)) return false;
  }

  const LONG span = splitVertical ? area.right - area.left : area.bottom - area.top;
  LONG firstSize = 0;
  if (!ChooseConstrainedSplit(
          span, gap, 0.5,
          splitVertical ? first.minWidth : first.minHeight,
          splitVertical ? first.maxWidth : first.maxHeight,
          splitVertical ? rest.minWidth : rest.minHeight,
          splitVertical ? rest.maxWidth : rest.maxHeight,
          &firstSize)) {
    return false;
  }

  RECT firstRect{}, remaining{};
  if (splitVertical) {
    firstRect = {area.left, area.top, area.left + firstSize, area.bottom};
    remaining = {firstRect.right + gap, area.top, area.right, area.bottom};
  } else {
    firstRect = {area.left, area.top, area.right, area.top + firstSize};
    remaining = {area.left, firstRect.bottom + gap, area.right, area.bottom};
  }
  if (!AreaFits(firstRect, first)) return false;
  (*outRects)[start] = firstRect;
  return LayoutBSPConstrainedRecursive(
      remaining, gap, start + 1, count - 1, depth + 1, constraints, outRects);
}

bool LayoutBSPConstrained(
    const RECT& area, LONG gap,
    const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  outRects->assign(constraints.size(), RECT{});
  if (constraints.empty()) return true;
  ConstraintExtent aggregate{};
  if (!BspAggregate(constraints, gap, 0, constraints.size(), 0, &aggregate) ||
      !AreaFits(area, aggregate)) {
    return false;
  }
  return LayoutBSPConstrainedRecursive(
      area, gap, 0, constraints.size(), 0, constraints, outRects);
}

bool LayoutMonocleConstrained(
    const RECT& area, const std::vector<Model::WindowConstraints>& constraints,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  outRects->assign(constraints.size(), area);
  for (const auto& c : constraints) {
    if (!AreaFits(area, ToExtent(c))) return false;
  }
  return true;
}


}  // namespace Layout
