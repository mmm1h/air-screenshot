#pragma once

#include "airshot/common.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace airshot {

struct DisplayMonitorGeometry {
    RectI bounds;
    bool primary{};
    std::wstring device_name;
};

struct LastRegionCapture {
    RectI bounds;
    std::wstring topology_signature;
};

[[nodiscard]] std::optional<RectI> display_topology_bounds(
    std::span<const DisplayMonitorGeometry> monitors) noexcept;
[[nodiscard]] std::wstring display_topology_signature(
    std::span<const DisplayMonitorGeometry> monitors);
[[nodiscard]] bool valid_display_topology_signature(
    std::wstring_view value) noexcept;

enum class RepeatRegionStatus {
    success,
    no_history,
    invalid_history,
    no_displays,
    no_intersection,
};

struct RepeatRegionResolution {
    RepeatRegionStatus status{RepeatRegionStatus::invalid_history};
    RectI bounds;
    bool topology_changed{};
    bool cropped{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == RepeatRegionStatus::success;
    }
};

[[nodiscard]] RepeatRegionResolution resolve_repeat_region(
    const std::optional<LastRegionCapture>& history,
    std::span<const DisplayMonitorGeometry> current_monitors);

enum class SelectionSizeAnchor {
    center,
    top_left,
};

enum class SelectionSizeParseError {
    none,
    invalid_limits,
    invalid_width,
    invalid_height,
    width_out_of_range,
    height_out_of_range,
};

struct SelectionSizeParseResult {
    SelectionSizeParseError error{SelectionSizeParseError::none};
    int width{};
    int height{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SelectionSizeParseError::none;
    }
};

[[nodiscard]] SelectionSizeParseResult parse_selection_size(
    std::wstring_view width,
    std::wstring_view height,
    int maximum_width,
    int maximum_height) noexcept;

[[nodiscard]] std::optional<RectI> resize_selection_to_size(
    RectI current,
    int width,
    int height,
    RectI desktop_bounds,
    SelectionSizeAnchor anchor) noexcept;

[[nodiscard]] RectI selection_size_badge_bounds(
    RectI selection,
    RectI desktop_bounds,
    unsigned int dpi = 96U) noexcept;

}  // namespace airshot
