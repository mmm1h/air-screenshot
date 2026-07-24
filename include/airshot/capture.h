#pragma once

#include "airshot/bitmap.h"

namespace airshot {

struct MonitorSnapshot {
    HMONITOR handle{};
    RectI bounds;
    bool primary{};
    std::wstring device_name;
    Bitmap bitmap;
};

struct WindowCandidate {
    HWND handle{};
    RectI bounds;
};

[[nodiscard]] std::vector<MonitorSnapshot> capture_monitors();
[[nodiscard]] std::vector<WindowCandidate> enumerate_window_candidates();
[[nodiscard]] Bitmap capture_rect(const RectI& rect);
[[nodiscard]] Bitmap capture_virtual_desktop();
[[nodiscard]] std::optional<std::pair<Bitmap, RectI>> capture_active_window();
[[nodiscard]] std::optional<std::pair<Bitmap, RectI>> capture_monitor(std::wstring_view selector);
[[nodiscard]] Bitmap compose_selection(const std::vector<MonitorSnapshot>& monitors, const RectI& selection);

}  // namespace airshot
