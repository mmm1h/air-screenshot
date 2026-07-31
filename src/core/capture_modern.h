#pragma once

#include "airshot/bitmap.h"

namespace airshot::capture_detail {

// Windows.Graphics.Capture is attempted only for whole display/window frames.
// Callers must keep a GDI fallback because capture can be disabled by policy,
// unavailable in remote sessions, or fail during a graphics-device reset.
[[nodiscard]] Bitmap capture_monitor_modern(HMONITOR monitor) noexcept;
[[nodiscard]] Bitmap capture_window_modern(HWND window) noexcept;

}  // namespace airshot::capture_detail
