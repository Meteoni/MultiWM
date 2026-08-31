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


}  // namespace Layout
