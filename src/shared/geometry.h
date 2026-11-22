// Geometry and tracking-limit conversion used by both mods. All frame sizes
// and rectangles are physical pixels unless a DPI is explicitly supplied.
// No window queries, placement, model state or layout solver lives here.
#ifndef MULTIWM_SHARED_GEOMETRY_H
#define MULTIWM_SHARED_GEOMETRY_H

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <optional>

namespace SharedGeometry {

// Visible-frame limits. A maximum of zero means the app kept the unrestricted
// system default; a reported maximum is never allowed below the minimum.
struct FrameLimits {
  LONG minWidth = 1;
  LONG minHeight = 1;
  LONG maxWidth = 0;
  LONG maxHeight = 0;
};

inline LONG Width(const RECT& rect) { return rect.right - rect.left; }
inline LONG Height(const RECT& rect) { return rect.bottom - rect.top; }

inline POINT RectCenter(const RECT& rect) {
  return {rect.left + (rect.right - rect.left) / 2,
          rect.top + (rect.bottom - rect.top) / 2};
}

inline RECT CenteredRect(POINT center, LONG width, LONG height) {
  width = std::max<LONG>(1, width);
  height = std::max<LONG>(1, height);
  const LONG left = center.x - width / 2;
  const LONG top = center.y - height / 2;
  return {left, top, left + width, top + height};
}

inline RECT CenteredIn(const RECT& area, LONG width, LONG height) {
  return CenteredRect(RectCenter(area), width, height);
}

inline RECT ClampToArea(RECT rect, const RECT& area) {
  const LONG width = std::min(std::max<LONG>(1, rect.right - rect.left),
                              std::max<LONG>(1, area.right - area.left));
  const LONG height = std::min(std::max<LONG>(1, rect.bottom - rect.top),
                               std::max<LONG>(1, area.bottom - area.top));
  LONG left = std::max(rect.left, area.left);
  LONG top = std::max(rect.top, area.top);
  if (left + width > area.right) left = area.right - width;
  if (top + height > area.bottom) top = area.bottom - height;
  return {left, top, left + width, top + height};
}

inline LONG ScaleDip(LONG value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

// A `width` x `height` frame centred in `area` and kept inside it: where a new
// or overflowed floating window goes.
inline RECT CenteredClampedIn(const RECT& area, LONG width, LONG height) {
  return ClampToArea(CenteredIn(area, width, height), area);
}

// The size an override-size window rule gives a centred floating window: the
// whole work area for "Workspace", else the rule's DIP size at `dpi`. `Rule` is
// MultiWM's WindowRule or the snapshot's SnapshotRule; both have
// `useWorkspaceSize` and `floatingSizeDip.width/height`.
template <class Rule>
SIZE RuleFloatingSize(const Rule& rule, const RECT& workArea, UINT dpi) {
  if (rule.useWorkspaceSize) return {Width(workArea), Height(workArea)};
  return {ScaleDip(rule.floatingSizeDip.width, dpi), ScaleDip(rule.floatingSizeDip.height, dpi)};
}

// A window's invisible resize borders: how far each edge of its visible (DWM)
// frame lies inside its window rectangle. Physical pixels.
inline RECT FrameMargins(const RECT& windowRect, const RECT& frame) {
  return {frame.left - windowRect.left, frame.top - windowRect.top,
          windowRect.right - frame.right, windowRect.bottom - frame.bottom};
}

// The window rectangle around visible frame `frame` with resize borders `margins`.
inline RECT AddFrameMargins(const RECT& frame, const RECT& margins) {
  return {frame.left - margins.left, frame.top - margins.top,
          frame.right + margins.right, frame.bottom + margins.bottom};
}

// The visible frame inside window rectangle `windowRect` with resize borders `margins`.
inline RECT RemoveFrameMargins(const RECT& windowRect, const RECT& margins) {
  return {windowRect.left + margins.left, windowRect.top + margins.top,
          windowRect.right - margins.right, windowRect.bottom - margins.bottom};
}

// How close, in physical pixels, every edge of a window must be to its
// monitor's for the window to count as covering it (CoversMonitor), and its
// size to the monitor's to count as monitor-sized (IsMonitorSized).
inline constexpr LONG kCoversMonitorTolerance = 2;

// True when `windowRect` is `monitorRect`'s size, within the tolerance, wherever
// it is: an app asking for fullscreen without maximizing. Both mods read
// fullscreen this way, so a window mid-way through moving and sizing itself to
// its monitor already counts. Exact, not "at least": with an auto-hidden
// taskbar, a window filling the work area is larger than its monitor by its
// invisible resize borders, and isn't fullscreen. Compare with the monitor the
// window is on, never another, or a window the size of a smaller monitor would
// count on a larger one.
inline bool IsMonitorSized(const RECT& windowRect, const RECT& monitorRect) {
  return std::abs((windowRect.right - windowRect.left) - (monitorRect.right - monitorRect.left)) <=
             kCoversMonitorTolerance &&
         std::abs((windowRect.bottom - windowRect.top) - (monitorRect.bottom - monitorRect.top)) <=
             kCoversMonitorTolerance;
}

// True when `windowRect` covers `monitorRect`, every edge within the tolerance:
// how an app in fullscreen without maximizing looks.
inline bool CoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
  return std::abs(windowRect.left - monitorRect.left) <= kCoversMonitorTolerance &&
         std::abs(windowRect.top - monitorRect.top) <= kCoversMonitorTolerance &&
         std::abs(windowRect.right - monitorRect.right) <= kCoversMonitorTolerance &&
         std::abs(windowRect.bottom - monitorRect.bottom) <= kCoversMonitorTolerance;
}

// The area to tile in, given `workArea` (the monitor's work area minus the
// insets) on `monitorRect`. A tile filling a work area that covers the whole
// monitor would look fullscreen (CoversMonitor), so then every edge closer than
// the tolerance is moved in to one pixel past it, evenly. That only happens
// with a hidden taskbar and insets of about zero; any other area is unchanged.
inline RECT KeepTilesOffWholeMonitor(RECT workArea, const RECT& monitorRect) {
  if (!CoversMonitor(workArea, monitorRect)) return workArea;
  constexpr LONG margin = kCoversMonitorTolerance + 1;
  workArea.left = std::max(workArea.left, monitorRect.left + margin);
  workArea.top = std::max(workArea.top, monitorRect.top + margin);
  workArea.right = std::min(workArea.right, monitorRect.right - margin);
  workArea.bottom = std::min(workArea.bottom, monitorRect.bottom - margin);
  return workArea;
}

// The track sizes Windows offers a window before it answers WM_GETMINMAXINFO,
// in the window's own DPI units: the system minimum and maximum, the maximum
// never below the minimum. Both mods' size-limit probes start from these.
struct TrackingDefaults {
  POINT minimum;
  POINT maximum;
};

inline TrackingDefaults DefaultTrackingLimits(UINT windowDpi) {
  const UINT dpi = windowDpi ? windowDpi : 96;
  const LONG minWidth = std::max<LONG>(1, GetSystemMetricsForDpi(SM_CXMINTRACK, dpi));
  const LONG minHeight = std::max<LONG>(1, GetSystemMetricsForDpi(SM_CYMINTRACK, dpi));
  return {{minWidth, minHeight},
          {std::max<LONG>(minWidth, GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi)),
           std::max<LONG>(minHeight, GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi))}};
}

// Convert an application's WM_GETMINMAXINFO reply and the supplied defaults
// from window-DPI units to physical frame sizes. Decoration spans are already
// physical. Callers obtain the metrics and send the message using their own
// timeout/reentrancy policy; both DPIs must be nonzero.
inline FrameLimits TrackingLimitsToFrame(
    const MINMAXINFO& reply, POINT systemMinimum, POINT systemMaximum,
    UINT windowDpi, UINT physicalDpi, LONG decorationWidth, LONG decorationHeight) {
  auto toPhysical = [&](LONG windowUnits) {
    return MulDiv(windowUnits, static_cast<int>(physicalDpi), static_cast<int>(windowDpi));
  };
  FrameLimits frame;
  frame.minWidth = std::max<LONG>(
      1, toPhysical(std::max(systemMinimum.x, reply.ptMinTrackSize.x)) - decorationWidth);
  frame.minHeight = std::max<LONG>(
      1, toPhysical(std::max(systemMinimum.y, reply.ptMinTrackSize.y)) - decorationHeight);
  // Identify an explicit maximum in the window's units, before scaling.
  if (reply.ptMaxTrackSize.x < systemMaximum.x) {
    frame.maxWidth = std::max<LONG>(frame.minWidth, toPhysical(reply.ptMaxTrackSize.x) - decorationWidth);
  }
  if (reply.ptMaxTrackSize.y < systemMaximum.y) {
    frame.maxHeight = std::max<LONG>(frame.minHeight, toPhysical(reply.ptMaxTrackSize.y) - decorationHeight);
  }
  return frame;
}

// The size within `limits` nearest to `size`.
inline SIZE ClampSizeToLimits(SIZE size, const FrameLimits& limits) {
  LONG width = std::max(size.cx, limits.minWidth);
  LONG height = std::max(size.cy, limits.minHeight);
  if (limits.maxWidth > 0) width = std::min(width, limits.maxWidth);
  if (limits.maxHeight > 0) height = std::min(height, limits.maxHeight);
  return {width, height};
}

// `frame` moved, not resized, to lie inside `area`. Along an axis where it is
// larger than `area`, it starts at the area's left or top edge, so a window's
// caption stays on screen.
inline RECT MoveInsideArea(const RECT& frame, const RECT& area) {
  auto start = [](LONG from, LONG span, LONG low, LONG high) {
    return span >= high - low ? low : std::clamp(from, low, high - span);
  };
  const LONG width = Width(frame);
  const LONG height = Height(frame);
  const LONG left = start(frame.left, width, area.left, area.right);
  const LONG top = start(frame.top, height, area.top, area.bottom);
  return {left, top, left + width, top + height};
}

// --- Floating placement -------------------------------------------------------
//
// Where a floating window goes. One rule for both mods: MultiWM places windows
// with it (RepairFloatingGeometry) and the companion predicts where MultiWM will
// put a new window with it (PredictWindowPlacement). Each mod reads the size
// limits its own way (docs/ARCHITECTURE.md).

// What sizes a floating placement.
enum class FloatingSizeSource {
  Remembered,  // The window's remembered floating size; without one, as Fallback.
  Fallback,    // An override-size rule's size, else the default floating size.
  Own,         // The window's current size, unless an override-size rule names
               // another and the window can resize. At its own size it is only
               // moved, and may extend past the work area.
};

// An override-size rule as it applies to one placement.
struct FloatingSizeRule {
  bool preserveSize = false;  // Keep the window's current size.
  SIZE size{};                // Otherwise this size (physical pixels).
};

// `rule` (MultiWM's WindowRule or the snapshot's SnapshotRule) at `dpi`.
template <class Rule>
FloatingSizeRule ResolveFloatingSizeRule(const Rule& rule, const RECT& workArea, UINT dpi) {
  if (rule.preserveFloatingSize) return {true, {}};
  return {false, RuleFloatingSize(rule, workArea, dpi)};
}

// Everything a floating placement depends on. Sizes and points are physical.
struct FloatingRequest {
  FloatingSizeSource source = FloatingSizeSource::Fallback;
  RECT workArea{};
  std::optional<SIZE> defaultSize;       // Floating Default Size at this DPI.
  std::optional<SIZE> remembered;        // The remembered size, at this DPI.
  std::optional<SIZE> current;           // The window's current size.
  bool canResize = true;
  std::optional<FloatingSizeRule> rule;  // A matching override-size rule.
  bool honorPreserveSize = true;         // False: a preserve-size rule is ignored.
  std::optional<POINT> center;           // Default: the work area's center.
};

struct FloatingPlacement {
  RECT frame{};
  bool moveOnly = false;       // Keeps its own size, unclamped; only moved.
  bool appliesLimits = false;  // Its size still has to fit the window's size
                               // limits (FitFloatingPlacementToLimits).
  POINT center{};              // Where it is centred, and the work area it is
  RECT workArea{};             // kept inside, for fitting it to those limits.
};

// The frame for `request`, before the window's size limits
// (FitFloatingPlacementToLimits). Centered on `center`; a frame that is resized
// is kept inside the work area. No value when the size it needs is unknown.
// Whether the limits apply is decided here: never to a window that is only
// moved or can't resize.
inline std::optional<FloatingPlacement> PlanFloatingPlacement(const FloatingRequest& request) {
  std::optional<FloatingSizeRule> rule = request.rule;
  if (rule && rule->preserveSize && !request.honorPreserveSize) rule.reset();
  const bool ruleNamesSize = rule && !rule->preserveSize;

  FloatingPlacement placement;
  std::optional<SIZE> size;
  if (request.source == FloatingSizeSource::Own) {
    if (!request.current) return std::nullopt;
    placement.moveOnly = !ruleNamesSize || !request.canResize;
    size = placement.moveOnly ? *request.current : rule->size;
  } else if (request.source == FloatingSizeSource::Remembered && request.remembered) {
    size = request.remembered;
  } else if (rule && rule->preserveSize && request.current) {
    size = request.current;
  } else {
    size = ruleNamesSize ? std::optional<SIZE>{rule->size} : request.defaultSize;
  }
  if (!request.canResize && request.current) size = request.current;
  if (!size) return std::nullopt;

  placement.appliesLimits = !placement.moveOnly && request.canResize;
  placement.center = request.center ? *request.center : RectCenter(request.workArea);
  placement.workArea = request.workArea;
  placement.frame = CenteredRect(placement.center, size->cx, size->cy);
  if (!placement.moveOnly) placement.frame = ClampToArea(placement.frame, request.workArea);
  return placement;
}

// `placement`'s frame with its size fitted to the window's size `limits`, still
// centred where it was planned and kept inside the work area; a minimum larger
// than the work area overhangs its right or bottom edge (MoveInsideArea).
// Unchanged when the limits don't apply or the planned size already fits them.
inline RECT FitFloatingPlacementToLimits(const FloatingPlacement& placement, const FrameLimits& limits) {
  if (!placement.appliesLimits) return placement.frame;
  const SIZE planned{Width(placement.frame), Height(placement.frame)};
  const SIZE size = ClampSizeToLimits(planned, limits);
  if (size.cx == planned.cx && size.cy == planned.cy) return placement.frame;
  return MoveInsideArea(CenteredRect(placement.center, size.cx, size.cy), placement.workArea);
}

// Centre one axis inside its cell when a maximum caps it. Returns whether the
// axis changed; integer rounding deliberately matches the layout planner.
inline bool CenterWithinMaximumSize(LONG& start, LONG& end, LONG maximum) {
  if (maximum <= 0 || end - start <= maximum) return false;
  start += (end - start - maximum) / 2;
  end = start + maximum;
  return true;
}

}  // namespace SharedGeometry

#endif  // MULTIWM_SHARED_GEOMETRY_H
