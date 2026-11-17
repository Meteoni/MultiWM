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

inline bool CoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
  constexpr LONG tolerance = 2;
  return std::abs(windowRect.left - monitorRect.left) <= tolerance &&
         std::abs(windowRect.top - monitorRect.top) <= tolerance &&
         std::abs(windowRect.right - monitorRect.right) <= tolerance &&
         std::abs(windowRect.bottom - monitorRect.bottom) <= tolerance;
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
