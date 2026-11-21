// Geometry and tracking-limit conversion used by both mods. All frame sizes
// and rectangles are physical pixels unless a DPI is explicitly supplied.
// No window queries, placement, model state or layout solver lives here.
#ifndef MULTIWM_SHARED_GEOMETRY_H
#define MULTIWM_SHARED_GEOMETRY_H

#include <windows.h>
#include <algorithm>
#include <cstdlib>

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
