namespace Layout {

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
    std::vector<RECT>& outRects, bool horizontal,
    const std::vector<double>& weights) {
  outRects.resize(windowCount);
  if (windowCount == 0) return;

  const std::vector<double>* effectiveWeights = &weights;
  std::vector<double> defaultWeights;
  if (weights.size() != windowCount) {
    defaultWeights = DefaultWeights(windowCount);
    effectiveWeights = &defaultWeights;
  }

  LONG totalSize = horizontal ? (area.bottom - area.top) : (area.right - area.left);
  LONG effectiveGap = 0;
  std::vector<LONG> sizes =
      ComputeWeightedSizes(totalSize, gap, *effectiveWeights, &effectiveGap);
  LONG position = horizontal ? area.top : area.left;

  for (size_t i = 0; i < windowCount; ++i) {
    LONG size = sizes[i];
    LONG end = (i == windowCount - 1) ? (horizontal ? area.bottom : area.right) : position + size;
    outRects[i] = horizontal ? RECT{area.left, position, area.right, end} : RECT{position, area.top, end, area.bottom};
    position = end + effectiveGap;
  }
}

void LayoutMasterStackWeighted(
    const RECT& area, LONG gap, size_t windowCount,
    std::vector<RECT>& outRects, bool horizontal,
    double masterRatio, const std::vector<double>& stackWeights) {
  outRects.resize(windowCount);
  if (windowCount == 0) return;
  if (windowCount == 1) {
    outRects[0] = area;
    return;
  }

  LONG totalSize = horizontal ? (area.bottom - area.top) : (area.right - area.left);
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

  if (horizontal) {
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

void LayoutBSP(
    const RECT& area, LONG gap, size_t startIndex, size_t count, int depth,
    std::vector<RECT>& outRects) {
  if (count == 0) return;
  if (count == 1) {
    outRects[startIndex] = area;
    return;
  }

  const LONG width = area.right - area.left;
  const LONG height = area.bottom - area.top;
  bool splitVertical = depth % 2 == 0;
  if ((splitVertical ? width : height) < 2) splitVertical = !splitVertical;

  const LONG span = splitVertical ? width : height;
  if (span < 2) {
    std::fill(
        outRects.begin() + startIndex,
        outRects.begin() + startIndex + count, area);
    return;
  }

  const LONG effectiveGap = std::clamp<LONG>(gap, 0, span - 2);
  const LONG firstSpan = (span - effectiveGap) / 2;
  RECT remaining = area;
  if (splitVertical) {
    outRects[startIndex] = {
        area.left, area.top, area.left + firstSpan, area.bottom};
    remaining.left = outRects[startIndex].right + effectiveGap;
  } else {
    outRects[startIndex] = {
        area.left, area.top, area.right, area.top + firstSpan};
    remaining.top = outRects[startIndex].bottom + effectiveGap;
  }


  LayoutBSP(remaining, gap, startIndex + 1, count - 1, depth + 1, outRects);
}

struct ConstraintExtent {
  LONG minWidth = 1;
  LONG minHeight = 1;
};

static bool ConstraintDimensionFits(LONG value, LONG minimum) {
  return value >= minimum;
}

// Weighted allocation with the same gap-degradation semantics as the ordinary
// layout path. Relative weights remain preferences; members pin to their proven
// minimum only when required to make the full span legal.
static bool ComputeConstrainedWeightedSizes(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    const std::vector<LONG>& minimums,
    std::vector<LONG>* outSizes, LONG* outEffectiveGap = nullptr) {
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
      c.valid && c.enforceMinWidth ? std::max<LONG>(1, c.minWidth) : 1;
  const LONG minHeight =
      c.valid && c.enforceMinHeight ? std::max<LONG>(1, c.minHeight) : 1;
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
    bool horizontal, std::vector<RECT>* outRects) {
  if (!outRects) return false;
  const size_t count = constraints.size();
  outRects->assign(count, RECT{});
  if (count == 0) return true;

  const LONG crossSpan =
      horizontal ? area.right - area.left : area.bottom - area.top;
  std::vector<LONG> mins(count);
  for (size_t i = 0; i < count; ++i) {
    const ConstraintExtent c = ToConstraintExtent(constraints[i]);
    if (horizontal) {
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
      horizontal ? area.bottom - area.top : area.right - area.left;
  LONG effectiveGap = 0;
  std::vector<LONG> sizes;
  if (!ComputeConstrainedWeightedSizes(
          total, gap, weights, mins, &sizes, &effectiveGap)) {
    return false;
  }

  LONG position = horizontal ? area.top : area.left;
  for (size_t i = 0; i < count; ++i) {
    const LONG end =
        i + 1 == count
            ? (horizontal ? area.bottom : area.right)
            : position + sizes[i];
    (*outRects)[i] = horizontal
        ? RECT{area.left, position, area.right, end}
        : RECT{position, area.top, end, area.bottom};
    position = end + effectiveGap;
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
    if (!ConstraintAreaFits(area, ToConstraintExtent(constraints[0]))) {
      return false;
    }
    (*outRects)[0] = area;
    return true;
  }

  const ConstraintExtent master = ToConstraintExtent(constraints[0]);
  const LONG areaWidth = area.right - area.left;
  const LONG areaHeight = area.bottom - area.top;
  const LONG splitSpan = horizontal ? areaHeight : areaWidth;
  const LONG innerSpan = horizontal ? areaWidth : areaHeight;

  if (horizontal) {
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
    if (horizontal) {
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
          horizontal ? master.minHeight : master.minWidth,
          stackSplitMin,
          &masterSize, &masterGap)) {
    return false;
  }

  LONG stackGap = 0;
  std::vector<LONG> innerSizes;
  if (!ComputeConstrainedWeightedSizes(
          innerSpan, gap, stackWeights, innerMins,
          &innerSizes, &stackGap)) {
    return false;
  }

  if (horizontal) {
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
