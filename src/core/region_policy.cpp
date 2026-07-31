#include "airshot/region_policy.h"

#include "overlay_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <tuple>

namespace airshot {
namespace {

struct CheckedRegion {
    std::int64_t width{};
    std::int64_t height{};
};

[[nodiscard]] std::optional<CheckedRegion> checked_region(
    const RectI& bounds,
    int minimum_extent = 1) noexcept {
    const std::int64_t width =
        static_cast<std::int64_t>(bounds.right) - bounds.left;
    const std::int64_t height =
        static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    if (width < minimum_extent || height < minimum_extent ||
        width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return CheckedRegion{width, height};
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1'099'511'628'211ULL;
}

void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        hash_byte(
            hash,
            static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void hash_wstring(
    std::uint64_t& hash,
    std::wstring_view value) noexcept {
    hash_u32(hash, static_cast<std::uint32_t>(value.size()));
    for (const wchar_t character : value) {
        const auto code_unit = static_cast<std::uint16_t>(character);
        hash_byte(hash, static_cast<std::uint8_t>(code_unit & 0xFFU));
        hash_byte(
            hash,
            static_cast<std::uint8_t>((code_unit >> 8) & 0xFFU));
    }
}

[[nodiscard]] std::optional<int> parse_positive_integer(
    std::wstring_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const int digit = character - L'0';
        if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
    }
    return parsed;
}

[[nodiscard]] std::optional<int> parse_signed_integer(
    std::wstring_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    std::size_t offset = 0;
    if (value.front() == L'-') {
        negative = true;
        offset = 1;
    }
    if (offset == value.size()) {
        return std::nullopt;
    }

    const std::int64_t limit = negative
                                   ? static_cast<std::int64_t>(
                                         std::numeric_limits<int>::max()) +
                                         1
                                   : std::numeric_limits<int>::max();
    std::int64_t parsed = 0;
    for (; offset < value.size(); ++offset) {
        const wchar_t character = value[offset];
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const int digit = character - L'0';
        if (parsed > (limit - digit) / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
    }
    if (negative) {
        parsed = -parsed;
    }
    return static_cast<int>(parsed);
}

[[nodiscard]] std::int64_t floor_divide_by_two(
    std::int64_t value) noexcept {
    if (value >= 0 || value % 2 == 0) {
        return value / 2;
    }
    return value / 2 - 1;
}

struct SelectionGeometryResolution {
    SelectionGeometryParseError error{SelectionGeometryParseError::none};
    RectI bounds;
};

[[nodiscard]] SelectionGeometryResolution resolve_selection_geometry_impl(
    RectI current,
    int x,
    int y,
    int width,
    int height,
    RectI desktop_bounds,
    SelectionSizeAnchor anchor) noexcept {
    current = current.normalized();
    desktop_bounds = desktop_bounds.normalized();
    const auto current_size = checked_region(current, 2);
    const auto desktop_size = checked_region(desktop_bounds, 2);
    if (!current_size || !desktop_size) {
        return {SelectionGeometryParseError::invalid_limits};
    }
    if (width < 2 || width > desktop_size->width) {
        return {SelectionGeometryParseError::width_out_of_range};
    }
    if (height < 2 || height > desktop_size->height) {
        return {SelectionGeometryParseError::height_out_of_range};
    }

    std::int64_t left = x;
    std::int64_t top = y;
    if (anchor == SelectionSizeAnchor::center) {
        if (x == current.left) {
            left = floor_divide_by_two(
                static_cast<std::int64_t>(current.left) +
                current.right - width);
        }
        if (y == current.top) {
            top = floor_divide_by_two(
                static_cast<std::int64_t>(current.top) +
                current.bottom - height);
        }
    }
    const std::int64_t right = left + width;
    const std::int64_t bottom = top + height;
    if (left < desktop_bounds.left || right > desktop_bounds.right) {
        return {SelectionGeometryParseError::horizontal_out_of_range};
    }
    if (top < desktop_bounds.top || bottom > desktop_bounds.bottom) {
        return {SelectionGeometryParseError::vertical_out_of_range};
    }
    return {
        SelectionGeometryParseError::none,
        RectI{
            static_cast<int>(left),
            static_cast<int>(top),
            static_cast<int>(right),
            static_cast<int>(bottom)}};
}

[[nodiscard]] bool regions_intersect(
    const RectI& first,
    const RectI& second) noexcept {
    return first.left < second.right && first.right > second.left &&
           first.top < second.bottom && first.bottom > second.top;
}

}  // namespace

std::optional<RectI> display_topology_bounds(
    std::span<const DisplayMonitorGeometry> monitors) noexcept {
    if (monitors.empty()) {
        return std::nullopt;
    }
    RectI result = monitors.front().bounds;
    if (!checked_region(result)) {
        return std::nullopt;
    }
    for (const auto& monitor : monitors.subspan(1)) {
        if (!checked_region(monitor.bounds)) {
            return std::nullopt;
        }
        result.left = std::min(result.left, monitor.bounds.left);
        result.top = std::min(result.top, monitor.bounds.top);
        result.right = std::max(result.right, monitor.bounds.right);
        result.bottom = std::max(result.bottom, monitor.bounds.bottom);
    }
    return checked_region(result) ? std::optional<RectI>(result)
                                  : std::nullopt;
}

std::wstring display_topology_signature(
    std::span<const DisplayMonitorGeometry> monitors) {
    if (!display_topology_bounds(monitors)) {
        return {};
    }
    std::vector<DisplayMonitorGeometry> canonical(
        monitors.begin(),
        monitors.end());
    std::ranges::sort(
        canonical,
        [](const auto& first, const auto& second) {
            return std::tie(
                       first.bounds.left,
                       first.bounds.top,
                       first.bounds.right,
                       first.bounds.bottom,
                       first.primary,
                       first.device_name) <
                   std::tie(
                       second.bounds.left,
                       second.bounds.top,
                       second.bounds.right,
                       second.bounds.bottom,
                       second.primary,
                       second.device_name);
        });

    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    hash_byte(hash, 1U);
    hash_u32(hash, static_cast<std::uint32_t>(canonical.size()));
    for (const auto& monitor : canonical) {
        hash_u32(hash, static_cast<std::uint32_t>(monitor.bounds.left));
        hash_u32(hash, static_cast<std::uint32_t>(monitor.bounds.top));
        hash_u32(hash, static_cast<std::uint32_t>(monitor.bounds.right));
        hash_u32(hash, static_cast<std::uint32_t>(monitor.bounds.bottom));
        hash_byte(hash, monitor.primary ? 1U : 0U);
        hash_wstring(hash, monitor.device_name);
    }
    return std::format(L"v1-{:016x}", hash);
}

bool valid_display_topology_signature(
    std::wstring_view value) noexcept {
    if (value.size() != 19 || !value.starts_with(L"v1-")) {
        return false;
    }
    return std::ranges::all_of(
        value.substr(3),
        [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'f');
        });
}

RepeatRegionResolution resolve_repeat_region(
    const std::optional<LastRegionCapture>& history,
    std::span<const DisplayMonitorGeometry> current_monitors) {
    if (!history) {
        return {RepeatRegionStatus::no_history};
    }
    const auto saved_size = checked_region(history->bounds, 2);
    if (!saved_size ||
        !valid_display_topology_signature(history->topology_signature)) {
        return {RepeatRegionStatus::invalid_history};
    }
    const auto desktop = display_topology_bounds(current_monitors);
    if (!desktop) {
        return {RepeatRegionStatus::no_displays};
    }
    const std::wstring current_signature =
        display_topology_signature(current_monitors);
    if (current_signature.empty()) {
        return {RepeatRegionStatus::no_displays};
    }

    const bool changed =
        current_signature != history->topology_signature;
    const RectI saved = history->bounds;
    const bool intersects_display = std::ranges::any_of(
        current_monitors,
        [&saved](const DisplayMonitorGeometry& monitor) {
            return regions_intersect(saved, monitor.bounds);
        });
    if (!intersects_display) {
        return {
            RepeatRegionStatus::no_intersection,
            {},
            changed,
            false};
    }

    if (!changed) {
        if (saved.left < desktop->left || saved.top < desktop->top ||
            saved.right > desktop->right ||
            saved.bottom > desktop->bottom) {
            return {RepeatRegionStatus::invalid_history};
        }
        return {
            RepeatRegionStatus::success,
            saved,
            false,
            false};
    }

    const auto desktop_size = checked_region(*desktop, 2);
    if (!desktop_size) {
        return {RepeatRegionStatus::no_displays};
    }
    bool cropped = false;
    std::int64_t left = saved.left;
    std::int64_t top = saved.top;
    std::int64_t width = saved_size->width;
    std::int64_t height = saved_size->height;
    if (width > desktop_size->width) {
        left = desktop->left;
        width = desktop_size->width;
        cropped = true;
    } else {
        left = std::clamp<std::int64_t>(
            left,
            desktop->left,
            static_cast<std::int64_t>(desktop->right) - width);
    }
    if (height > desktop_size->height) {
        top = desktop->top;
        height = desktop_size->height;
        cropped = true;
    } else {
        top = std::clamp<std::int64_t>(
            top,
            desktop->top,
            static_cast<std::int64_t>(desktop->bottom) - height);
    }
    const RectI resolved{
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(left + width),
        static_cast<int>(top + height),
    };
    if (!checked_region(resolved, 2)) {
        return {RepeatRegionStatus::invalid_history};
    }
    return {
        RepeatRegionStatus::success,
        resolved,
        true,
        cropped};
}

SelectionSizeParseResult parse_selection_size(
    std::wstring_view width,
    std::wstring_view height,
    int maximum_width,
    int maximum_height) noexcept {
    if (maximum_width < 2 || maximum_height < 2) {
        return {SelectionSizeParseError::invalid_limits};
    }
    const auto parsed_width = parse_positive_integer(width);
    if (!parsed_width) {
        return {SelectionSizeParseError::invalid_width};
    }
    const auto parsed_height = parse_positive_integer(height);
    if (!parsed_height) {
        return {SelectionSizeParseError::invalid_height};
    }
    if (*parsed_width < 2 || *parsed_width > maximum_width) {
        return {SelectionSizeParseError::width_out_of_range};
    }
    if (*parsed_height < 2 || *parsed_height > maximum_height) {
        return {SelectionSizeParseError::height_out_of_range};
    }
    return {
        SelectionSizeParseError::none,
        *parsed_width,
        *parsed_height};
}

SelectionGeometryParseResult parse_selection_geometry(
    std::wstring_view x,
    std::wstring_view y,
    std::wstring_view width,
    std::wstring_view height,
    RectI current,
    RectI desktop_bounds,
    SelectionSizeAnchor anchor) noexcept {
    if (!checked_region(current.normalized(), 2) ||
        !checked_region(desktop_bounds.normalized(), 2)) {
        return {SelectionGeometryParseError::invalid_limits};
    }
    const auto parsed_x = parse_signed_integer(x);
    if (!parsed_x) {
        return {SelectionGeometryParseError::invalid_x};
    }
    const auto parsed_y = parse_signed_integer(y);
    if (!parsed_y) {
        return {SelectionGeometryParseError::invalid_y};
    }
    const auto parsed_width = parse_positive_integer(width);
    if (!parsed_width) {
        return {SelectionGeometryParseError::invalid_width};
    }
    const auto parsed_height = parse_positive_integer(height);
    if (!parsed_height) {
        return {SelectionGeometryParseError::invalid_height};
    }

    const SelectionGeometryResolution resolved =
        resolve_selection_geometry_impl(
            current,
            *parsed_x,
            *parsed_y,
            *parsed_width,
            *parsed_height,
            desktop_bounds,
            anchor);
    return {
        resolved.error,
        *parsed_x,
        *parsed_y,
        *parsed_width,
        *parsed_height,
        resolved.bounds};
}

std::optional<RectI> resolve_selection_geometry(
    RectI current,
    int x,
    int y,
    int width,
    int height,
    RectI desktop_bounds,
    SelectionSizeAnchor anchor) noexcept {
    const SelectionGeometryResolution resolved =
        resolve_selection_geometry_impl(
            current,
            x,
            y,
            width,
            height,
            desktop_bounds,
            anchor);
    return resolved.error == SelectionGeometryParseError::none
               ? std::optional<RectI>(resolved.bounds)
               : std::nullopt;
}

std::optional<RectI> resize_selection_to_size(
    RectI current,
    int width,
    int height,
    RectI desktop_bounds,
    SelectionSizeAnchor anchor) noexcept {
    const auto current_size = checked_region(current, 2);
    const auto desktop_size = checked_region(desktop_bounds, 2);
    if (!current_size || !desktop_size || width < 2 || height < 2 ||
        width > desktop_size->width || height > desktop_size->height) {
        return std::nullopt;
    }

    std::int64_t desired_left = current.left;
    std::int64_t desired_top = current.top;
    if (anchor == SelectionSizeAnchor::center) {
        desired_left = floor_divide_by_two(
            static_cast<std::int64_t>(current.left) + current.right - width);
        desired_top = floor_divide_by_two(
            static_cast<std::int64_t>(current.top) + current.bottom - height);
    }
    const std::int64_t left = std::clamp<std::int64_t>(
        desired_left,
        desktop_bounds.left,
        static_cast<std::int64_t>(desktop_bounds.right) - width);
    const std::int64_t top = std::clamp<std::int64_t>(
        desired_top,
        desktop_bounds.top,
        static_cast<std::int64_t>(desktop_bounds.bottom) - height);
    return RectI{
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(left + width),
        static_cast<int>(top + height),
    };
}

RectI selection_size_badge_bounds(
    RectI selection,
    RectI desktop_bounds,
    unsigned int dpi) noexcept {
    const overlay_detail::OverlayUiMetrics ui{dpi};
    const int badge_width = ui.px(300);
    const int badge_height = ui.px(24);
    const int gap = ui.px(4);
    if (!checked_region(selection, 2) ||
        !checked_region(desktop_bounds, 2)) {
        return {};
    }
    const int width = std::min(badge_width, desktop_bounds.width());
    const int height = std::min(badge_height, desktop_bounds.height());
    const int left = std::clamp(
        selection.left,
        desktop_bounds.left,
        desktop_bounds.right - width);
    std::int64_t top =
        static_cast<std::int64_t>(selection.top) - height - gap;
    if (top < desktop_bounds.top) {
        top = static_cast<std::int64_t>(selection.top) + gap;
    }
    top = std::clamp<std::int64_t>(
        top,
        desktop_bounds.top,
        static_cast<std::int64_t>(desktop_bounds.bottom) - height);
    const int resolved_top = static_cast<int>(top);
    return {
        left,
        resolved_top,
        left + width,
        resolved_top + height};
}

}  // namespace airshot
