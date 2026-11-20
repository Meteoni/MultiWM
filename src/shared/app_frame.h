// The frame an app asked for when it opened a window that the companion then
// moved onto its tile before showing it.
//
// MultiWM remembers a new window's natural frame when it first records the
// window, and floats it at that frame later (Float Focused, a drag that floats
// it, switching to the Floating layout). Without the companion the window's
// current frame at that moment is the app's own. With the companion it is
// already the tile, so the companion leaves the app's frame on the window as
// two window properties for MultiWM to read instead.
//
// The properties are optional in both directions: an older MultiWM ignores
// them, and without them MultiWM uses the window's current frame as before.
// Both mods compile this file: only Windows and the standard library here.

#ifndef MULTIWM_SHARED_APP_FRAME_H
#define MULTIWM_SHARED_APP_FRAME_H

#include <windows.h>

#include <cstdint>
#include <optional>

namespace AppFrame {

// A property holds one HANDLE, which is 32 bits in a 32-bit app, so the frame
// (physical screen pixels, the visible frame without resize borders) is split
// into two 32-bit values: origin as two signed 16-bit coordinates, and size as
// two unsigned 16-bit spans. Size is never zero, so it also marks presence.
inline constexpr wchar_t kOriginProperty[] = L"MultiWM.Companion.AppFrameOrigin.v1";
inline constexpr wchar_t kSizeProperty[] = L"MultiWM.Companion.AppFrameSize.v1";

struct Packed {
  uint32_t origin = 0;
  uint32_t size = 0;
};

// No value when the frame is empty or a coordinate doesn't fit 16 bits.
inline std::optional<Packed> Pack(const RECT& frame) {
  const LONG width = frame.right - frame.left;
  const LONG height = frame.bottom - frame.top;
  if (width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF || frame.left < INT16_MIN ||
      frame.left > INT16_MAX || frame.top < INT16_MIN || frame.top > INT16_MAX) {
    return std::nullopt;
  }
  return Packed{static_cast<uint16_t>(static_cast<int16_t>(frame.left)) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(frame.top))) << 16),
                static_cast<uint32_t>(width) | (static_cast<uint32_t>(height) << 16)};
}

// No value when `packed.size` is zero (no frame recorded).
inline std::optional<RECT> Unpack(Packed packed) {
  const LONG width = static_cast<LONG>(packed.size & 0xFFFF);
  const LONG height = static_cast<LONG>(packed.size >> 16);
  if (width == 0 || height == 0) return std::nullopt;
  const LONG left = static_cast<int16_t>(packed.origin & 0xFFFF);
  const LONG top = static_cast<int16_t>(packed.origin >> 16);
  return RECT{left, top, left + width, top + height};
}

// The value a property holds, as stored by a 32- or 64-bit process.
inline uint32_t PropertyValue(HWND hwnd, const wchar_t* name) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetPropW(hwnd, name)));
}

// Companion, in the app's process: records `frame` unless the window already
// has one, so a later show of an already-tiled window can't replace it.
inline void Remember(HWND hwnd, const RECT& frame) {
  if (PropertyValue(hwnd, kSizeProperty)) return;
  const auto packed = Pack(frame);
  if (!packed) return;
  SetPropW(hwnd, kOriginProperty, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packed->origin)));
  SetPropW(hwnd, kSizeProperty, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packed->size)));
}

// Companion, when the window is destroyed: a process must remove the
// properties it added.
inline void Forget(HWND hwnd) {
  RemovePropW(hwnd, kSizeProperty);
  RemovePropW(hwnd, kOriginProperty);
}

// MultiWM: the recorded frame, if the companion left one.
inline std::optional<RECT> Read(HWND hwnd) {
  return Unpack({PropertyValue(hwnd, kOriginProperty), PropertyValue(hwnd, kSizeProperty)});
}

}  // namespace AppFrame

#endif  // MULTIWM_SHARED_APP_FRAME_H
