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

inline bool CoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
  constexpr LONG tolerance = 2;
  return std::abs(windowRect.left - monitorRect.left) <= tolerance &&
         std::abs(windowRect.top - monitorRect.top) <= tolerance &&
         std::abs(windowRect.right - monitorRect.right) <= tolerance &&
         std::abs(windowRect.bottom - monitorRect.bottom) <= tolerance;
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

// A floating frame with its size clamped to `limits` and its top-left corner
// kept, which is how Windows applies a window's size limits to a positioning
// call. MultiWM places with SWP_NOSENDCHANGING, which skips that clamp, so it
// applies this itself; the companion predicts its floating placements with it.
inline RECT ClampSizeKeepingOrigin(RECT frame, const FrameLimits& limits) {
  const LONG width = std::max(Width(frame), limits.minWidth);
  const LONG height = std::max(Height(frame), limits.minHeight);
  frame.right = frame.left + (limits.maxWidth > 0 ? std::min(width, limits.maxWidth) : width);
  frame.bottom = frame.top + (limits.maxHeight > 0 ? std::min(height, limits.maxHeight) : height);
  return frame;
}

// --- Floating placement -------------------------------------------------------
//
// Where a floating window goes. One rule for both mods: MultiWM places windows
// with it (RepairFloatingGeometry) and the companion predicts where MultiWM will
// put a new window with it (PredictFrame), so the two can't drift apart.

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
  bool moveOnly = false;  // Keeps its own size, unclamped; only moved.
};

// The frame for `request`, before the window's size limits
// (FitFloatingPlacementToLimits). Centered on `center`; a frame that is resized
// is kept inside the work area. No value when the size it needs is unknown.
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

  const POINT center = request.center ? *request.center : RectCenter(request.workArea);
  placement.frame = CenteredRect(center, size->cx, size->cy);
  if (!placement.moveOnly) placement.frame = ClampToArea(placement.frame, request.workArea);
  return placement;
}

// `placement`'s frame with the window's size limits applied; a window that is
// only moved keeps its own size.
inline RECT FitFloatingPlacementToLimits(const FloatingPlacement& placement, const FrameLimits& limits) {
  return placement.moveOnly ? placement.frame : ClampSizeKeepingOrigin(placement.frame, limits);
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
