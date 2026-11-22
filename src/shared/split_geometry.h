// Divider arithmetic shared by MultiWM's layout planner and MultiWM Companion,
// so both mods size a split, and fit minimum sizes to it, with the same code.
// Physical pixels throughout. Windows and the standard library only.
//
// The companion uses it to place a new BSP window exactly where MultiWM will:
// MultiWM publishes the splits above the window's tile (BspPathSplit), and
// SolveBspPath fits the window's own minimum size to them.
#ifndef MULTIWM_SHARED_SPLIT_GEOMETRY_H
#define MULTIWM_SHARED_SPLIT_GEOMETRY_H

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace SharedSplits {

// Divider ratios (master size, Dwindle and BSP splits) are kept within 10%..90%
// so neither side of a divider can collapse to nothing.
inline double ClampSplitRatio(double ratio) {
  return std::clamp(ratio, 0.1, 0.9);
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

// Pixel bounds for automatic movement from the saved split ratio, within
// the same ratio limits used for manual resizing.
inline std::pair<LONG, LONG> AdaptiveSplitSpanBounds(LONG usable, double savedRatio, double deviation) {
  const double ratio = ClampSplitRatio(savedRatio);
  return {SplitLeadingSpan(usable, ClampSplitRatio(ratio - deviation)),
          SplitLeadingSpan(usable, ClampSplitRatio(ratio + deviation))};
}

// How one split may fit minimum sizes: its saved ratio, and whether (and how
// far) it may move from it.
struct SplitFit {
  double ratio = 0.5;
  bool adaptive = false;
  double deviation = 0.0;  // Of the usable span, when adaptive.
};

// The first side's span in `usable` pixels: the saved ratio's, unless that
// leaves a side below its minimum; then, for an adaptive split, the nearest
// span inside the adaptive range that fits both. No value when nothing does.
inline std::optional<LONG> FitSplitFirstSpan(const SplitFit& fit, LONG usable, LONG firstMinimum, LONG secondMinimum) {
  const LONG preferred = SplitLeadingSpan(usable, ClampSplitRatio(fit.ratio));
  if (preferred >= firstMinimum && usable - preferred >= secondMinimum) return preferred;
  if (!fit.adaptive) return std::nullopt;
  const auto [minimumAdaptive, maximumAdaptive] = AdaptiveSplitSpanBounds(usable, fit.ratio, fit.deviation);
  const LONG lower = std::max(firstMinimum, minimumAdaptive);
  const LONG upper = std::min(usable - secondMinimum, maximumAdaptive);
  if (lower > upper) return std::nullopt;
  return std::clamp(preferred, lower, upper);
}

// The smallest span along which a split can leave its sides `first` and
// `second` pixels (FitSplitFirstSpan succeeds); `maximumSpan` + 1 when no span
// up to `maximumSpan` can.
inline LONG RequiredSplitSpan(const SplitFit& fit, LONG gap, LONG first, LONG second, LONG maximumSpan) {
  auto fits = [&](LONG span) {
    if (span < 2) return false;
    const LONG usable = UsableSplitSpan(span, gap);
    return usable >= 2 && FitSplitFirstSpan(fit, usable, first, second).has_value();
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
}

// One split above a new BSP window's tile, as MultiWM publishes it to the
// companion: only splits with windows on both sides, from the root down. Fixed
// layout (CompanionAbi).
struct BspPathSplit {
  double ratio;               // The saved first-side share.
  int32_t otherMinimumWidth;  // The other side's smallest size, its own
  int32_t otherMinimumHeight; // adaptive splits free to move.
  uint8_t alongX;             // 1: side by side.
  uint8_t windowFirst;        // 1: the new window is on the first side.
  uint8_t adaptive;           // 1: the split may move to fit minimums.
  uint8_t reserved0;
  uint32_t reserved1;
};
static_assert(sizeof(BspPathSplit) == 24, "BspPathSplit layout");

// The tile a new BSP window with minimum `minWidth` x `minHeight` gets in
// `area`, below `path` (count splits, root first): every split along the path
// fits as MultiWM's planner fits it (FitSplitFirstSpan), with the window's own
// minimum and the other sides' published ones. The width depends on the side-
// by-side splits only and the height on the stacked ones, so the result is the
// planner's exactly. No value when the window can't fit there. Pure.
namespace Detail {

inline constexpr uint32_t kMaximumPathDepth = 64;

// The smallest size of the new window's side below each split of `path`, as
// MultiWM's planner measures a subtree: along a split the smallest span that
// leaves both sides their minimums, across it the larger side. below[count] is
// the window's own minimum.
inline void MeasureBelowPath(LONG gap, double deviation, const BspPathSplit* path, uint32_t count, SIZE window,
                             LONG maximumSpan, SIZE* below) {
  below[count] = window;
  for (uint32_t k = count; k-- > 0;) {
    const BspPathSplit& split = path[k];
    const SIZE other{std::max<LONG>(1, split.otherMinimumWidth), std::max<LONG>(1, split.otherMinimumHeight)};
    const SIZE& first = split.windowFirst ? below[k + 1] : other;
    const SIZE& second = split.windowFirst ? other : below[k + 1];
    SIZE combined{std::max(first.cx, second.cx), std::max(first.cy, second.cy)};
    LONG& along = split.alongX ? combined.cx : combined.cy;
    along = RequiredSplitSpan({split.ratio, split.adaptive != 0, deviation}, gap, split.alongX ? first.cx : first.cy,
                              split.alongX ? second.cx : second.cy, maximumSpan);
    below[k] = combined;
  }
}

// Cuts `region` at `split` as the planner does and keeps the new window's side,
// whose minimum is `window`. False when the split can't fit both sides.
inline bool CutTowardWindow(RECT& region, const BspPathSplit& split, LONG gap, double deviation, SIZE window) {
  LONG& start = split.alongX ? region.left : region.top;
  LONG& end = split.alongX ? region.right : region.bottom;
  const LONG span = end - start;
  if (span < 2) return false;
  const LONG usable = UsableSplitSpan(span, gap);
  const LONG windowMinimum = split.alongX ? window.cx : window.cy;
  const LONG otherMinimum = std::max<LONG>(1, split.alongX ? split.otherMinimumWidth : split.otherMinimumHeight);
  const std::optional<LONG> firstSpan =
      FitSplitFirstSpan({split.ratio, split.adaptive != 0, deviation}, usable,
                        split.windowFirst ? windowMinimum : otherMinimum, split.windowFirst ? otherMinimum : windowMinimum);
  if (!firstSpan) return false;
  const LONG divider = start + *firstSpan;
  if (split.windowFirst) {
    end = divider;
  } else {
    start = divider + (span - usable);
  }
  return true;
}

}  // namespace Detail

inline std::optional<RECT> SolveBspPath(const RECT& area, LONG gap, double deviation, const BspPathSplit* path,
                                        uint32_t count, LONG minWidth, LONG minHeight) {
  if (count > Detail::kMaximumPathDepth) return std::nullopt;
  const SIZE window{std::max<LONG>(1, minWidth), std::max<LONG>(1, minHeight)};
  SIZE below[Detail::kMaximumPathDepth + 1];
  Detail::MeasureBelowPath(gap, deviation, path, count, window,
                           std::max(area.right - area.left, area.bottom - area.top), below);
  RECT region = area;
  for (uint32_t k = 0; k < count; ++k) {
    if (!Detail::CutTowardWindow(region, path[k], gap, deviation, below[k + 1])) return std::nullopt;
  }
  if (region.right - region.left < window.cx || region.bottom - region.top < window.cy) return std::nullopt;
  return region;
}

}  // namespace SharedSplits

#endif  // MULTIWM_SHARED_SPLIT_GEOMETRY_H
