namespace Layout {

std::vector<LONG> ComputeWeightedSizes(LONG totalSize, LONG gap, const std::vector<double>& weights) {
  size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (count == 0) return sizes;
  LONG available = totalSize - gap * (LONG)(count - 1);
  if (available <= 0) return sizes;

  double sum = 0.0;
  for (double w : weights) sum += (w > 0.0 ? w : 0.0);
  if (sum <= 0.0) sum = static_cast<double>(count);

  LONG used = 0;
  double remainingSum = sum;
  for (size_t i = 0; i < count; ++i) {
    double w = weights[i];
    if (w <= 0.0) w = 1.0;
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

std::vector<LONG> ComputeWeightedSizesWithFixed(
    LONG totalSize, LONG gap, const std::vector<double>& weights,
    size_t fixedIndex, LONG fixedSize) {

  const size_t count = weights.size();
  std::vector<LONG> sizes(count, 0);
  if (count == 0) return sizes;
  if (fixedIndex >= count) return ComputeWeightedSizes(totalSize, gap, weights);

  const LONG available = totalSize - gap * (LONG)(count - 1);
  if (available <= 0) return sizes;

  // Clamp fixed size so remaining windows can still be >=1
  const LONG minFixed = 1;
  const LONG maxFixed = std::max<LONG>(1, available - (LONG)(count - 1));
  fixedSize = std::clamp(fixedSize, minFixed, maxFixed);

  sizes[fixedIndex] = fixedSize;

  LONG remaining = available - fixedSize;
  if (count == 1) return sizes;

  // Sum weights for non-fixed slots
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    if (i == fixedIndex) continue;
    sum += (weights[i] > 0.0 ? weights[i] : 1.0);
  }
  if (sum <= 0.0) sum = (double)(count - 1);

  LONG used = 0;
  double remainingSum = sum;

  // Distribute remaining across non-fixed indices (no gap math here)
  for (size_t i = 0; i < count; ++i) {
    if (i == fixedIndex) continue;

    // how many non-fixed slots remain after i?
    LONG slotsLeft = 0;
    for (size_t j = i + 1; j < count; ++j) if (j != fixedIndex) ++slotsLeft;

    double w = (weights[i] > 0.0 ? weights[i] : 1.0);
    LONG size = 0;

    if (slotsLeft == 0) {
      size = remaining - used;
    } else {
      const double ratio = w / remainingSum;
      size = (LONG)std::llround((double)(remaining - used) * ratio);
      if (size < 1) size = 1;

      const LONG maxSize = (remaining - used) - slotsLeft; // leave >=1 for each remaining slot
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

  LONG totalSize = horizontal ? (area.bottom - area.top) : (area.right - area.left);
  std::vector<LONG> sizes = ComputeWeightedSizes(totalSize, gap, weights);
  LONG position = horizontal ? area.top : area.left;

  for (size_t i = 0; i < windowCount; ++i) {
    LONG size = sizes[i];
    LONG end = (i == windowCount - 1) ? (horizontal ? area.bottom : area.right) : position + size;
    outRects[i] = horizontal ? RECT{area.left, position, area.right, end} : RECT{position, area.top, end, area.bottom};
    position = end + gap;
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
  LONG masterSize = static_cast<LONG>(std::llround((totalSize - gap) * masterRatio));
  if (masterSize < 1) masterSize = 1;
  if (masterSize > totalSize - gap - 1) masterSize = totalSize - gap - 1;

  size_t stackCount = windowCount - 1;
  std::vector<double> weights = stackWeights;
  if (weights.size() != stackCount) weights = DefaultWeights(stackCount);

  if (horizontal) {
    outRects[0] = {area.left, area.top, area.right, area.top + masterSize};
    LONG stackTop = area.top + masterSize + gap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.right - area.left, gap, weights);
    LONG x = area.left;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.right : x + sizes[i];
      outRects[i + 1] = {x, stackTop, end, area.bottom};
      x = end + gap;
    }
  } else {
    outRects[0] = {area.left, area.top, area.left + masterSize, area.bottom};
    LONG stackLeft = area.left + masterSize + gap;
    std::vector<LONG> sizes = ComputeWeightedSizes(area.bottom - area.top, gap, weights);
    LONG y = area.top;
    for (size_t i = 0; i < stackCount; ++i) {
      LONG end = (i == stackCount - 1) ? area.bottom : y + sizes[i];
      outRects[i + 1] = {stackLeft, y, area.right, end};
      y = end + gap;
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

  bool splitVertical = (depth % 2 == 0);
  LONG mid = splitVertical ? area.left + (area.right - area.left - gap) / 2
                           : area.top + (area.bottom - area.top - gap) / 2;

  outRects[startIndex] =
      splitVertical ? RECT{area.left, area.top, mid, area.bottom} : RECT{area.left, area.top, area.right, mid};

  RECT remaining = splitVertical ? RECT{mid + gap, area.top, area.right, area.bottom}
                                 : RECT{area.left, mid + gap, area.right, area.bottom};

  LayoutBSP(remaining, gap, startIndex + 1, count - 1, depth + 1, outRects);
}


}  // namespace Layout
