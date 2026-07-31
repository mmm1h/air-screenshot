#pragma once

#include "airshot/bitmap.h"
#include "airshot/region_policy.h"

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
    // Candidates are emitted in top-level Z order and grouped by root. Child
    // HWNDs keep the root handle so hit testing never allows a control from an
    // occluded window to outrank the foreground window.
    HWND root{};
    // Closest ancestor that is also present in the candidate list. This keeps
    // wheel navigation on the selected control's real hierarchy instead of
    // jumping to an unrelated overlapping sibling at the same depth.
    HWND parent{};
    int depth{};
};

[[nodiscard]] std::vector<MonitorSnapshot> capture_monitors(
    bool include_cursor = false);
[[nodiscard]] std::vector<DisplayMonitorGeometry> current_display_topology();
[[nodiscard]] std::vector<WindowCandidate> enumerate_window_candidates();
[[nodiscard]] std::optional<WindowCandidate> window_candidate_at_point(
    std::span<const WindowCandidate> candidates,
    POINT point,
    std::size_t ancestor_offset = 0) noexcept;
[[nodiscard]] Bitmap capture_rect(const RectI& rect);
[[nodiscard]] Bitmap capture_rect(
    const RectI& rect,
    bool include_cursor);
[[nodiscard]] Bitmap capture_virtual_desktop(bool include_cursor = false);
[[nodiscard]] std::optional<std::pair<Bitmap, RectI>> capture_active_window(
    bool include_cursor = false);
[[nodiscard]] std::optional<std::pair<Bitmap, RectI>> capture_monitor(
    std::wstring_view selector,
    bool include_cursor = false);
[[nodiscard]] Bitmap compose_selection(const std::vector<MonitorSnapshot>& monitors, const RectI& selection);

}  // namespace airshot
