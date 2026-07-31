#pragma once

#include "airshot/bitmap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace airshot {

struct PinVisualEffects {
    bool grayscale{};
    bool inverted{};

    [[nodiscard]] friend constexpr bool operator==(
        const PinVisualEffects&,
        const PinVisualEffects&) noexcept = default;
};

enum class PinVisualEffectAction {
    toggle_grayscale,
    toggle_inverted,
};

[[nodiscard]] constexpr PinVisualEffects transition_pin_visual_effects(
    PinVisualEffects effects,
    PinVisualEffectAction action) noexcept {
    if (action == PinVisualEffectAction::toggle_grayscale) {
        effects.grayscale = !effects.grayscale;
    } else {
        effects.inverted = !effects.inverted;
    }
    return effects;
}

enum class PinSourceChange {
    replace,
    rotate,
    flip,
};

// Geometry/source changes replace the unfiltered source while preserving the
// user's two display toggles. The next frame is always derived from that source.
[[nodiscard]] constexpr PinVisualEffects pin_effects_after_source_change(
    PinVisualEffects effects,
    PinSourceChange) noexcept {
    return effects;
}

[[nodiscard]] constexpr bool pin_copy_uses_visible_effects() noexcept {
    return true;
}

[[nodiscard]] constexpr bool pin_window_needs_layered_style(
    int alpha,
    bool click_through) noexcept {
    return click_through || alpha < 255;
}

[[nodiscard]] constexpr bool pin_bitmap_storage_is_valid(
    const Bitmap& bitmap) noexcept;

struct PinBitmapAlphaSummary {
    bool alpha_is_defined{};
    bool has_transparency{};

    [[nodiscard]] friend constexpr bool operator==(
        const PinBitmapAlphaSummary&,
        const PinBitmapAlphaSummary&) noexcept = default;
};

// PinWindow receives a normalized Bitmap: capture/DIB producers must resolve
// reserved alpha at their source boundary, while every alpha byte here is real.
[[nodiscard]] inline PinBitmapAlphaSummary summarize_pin_bitmap_alpha(
    const Bitmap& bitmap) noexcept {
    if (!pin_bitmap_storage_is_valid(bitmap)) {
        return {};
    }
    bool any_not_opaque = false;
    for (std::size_t offset = 3;
         offset < bitmap.pixels.size();
         offset += Bitmap::bytes_per_pixel) {
        const std::uint8_t alpha = bitmap.pixels[offset];
        any_not_opaque = any_not_opaque || alpha != 255U;
    }
    return {
        true,
        any_not_opaque,
    };
}

struct PinWindowStylePlan {
    bool layered{};
    bool per_pixel_alpha{};

    [[nodiscard]] friend constexpr bool operator==(
        const PinWindowStylePlan&,
        const PinWindowStylePlan&) noexcept = default;
};

[[nodiscard]] constexpr PinWindowStylePlan plan_pin_window_style(
    bool source_has_transparency,
    int alpha,
    bool click_through) noexcept {
    return {
        source_has_transparency ||
            pin_window_needs_layered_style(alpha, click_through),
        source_has_transparency,
    };
}

[[nodiscard]] constexpr bool pin_bitmap_storage_is_valid(
    const Bitmap& bitmap) noexcept {
    if (bitmap.width <= 0 || bitmap.height <= 0) {
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(bitmap.width);
    const std::size_t height = static_cast<std::size_t>(bitmap.height);
    if (width > std::numeric_limits<std::size_t>::max() /
                    Bitmap::bytes_per_pixel) {
        return false;
    }
    const std::size_t stride = width * Bitmap::bytes_per_pixel;
    return height <= std::numeric_limits<std::size_t>::max() / stride &&
           bitmap.pixels.size() == stride * height;
}

[[nodiscard]] inline bool write_pin_visual_pixels(
    const Bitmap& source,
    PinVisualEffects effects,
    std::span<std::uint8_t> destination) noexcept {
    if (!pin_bitmap_storage_is_valid(source) ||
        destination.size() != source.pixels.size()) {
        return false;
    }
    const bool has_defined_alpha =
        summarize_pin_bitmap_alpha(source).alpha_is_defined;
    for (std::size_t offset = 0;
         offset < source.pixels.size();
         offset += Bitmap::bytes_per_pixel) {
        std::uint8_t blue = source.pixels[offset];
        std::uint8_t green = source.pixels[offset + 1];
        std::uint8_t red = source.pixels[offset + 2];
        if (effects.grayscale) {
            const unsigned int gray =
                (29U * blue + 150U * green + 77U * red + 128U) >> 8U;
            blue = green = red = static_cast<std::uint8_t>(gray);
        }
        if (effects.inverted) {
            blue = static_cast<std::uint8_t>(255U - blue);
            green = static_cast<std::uint8_t>(255U - green);
            red = static_cast<std::uint8_t>(255U - red);
        }
        destination[offset] = blue;
        destination[offset + 1] = green;
        destination[offset + 2] = red;
        destination[offset + 3] =
            has_defined_alpha ? source.pixels[offset + 3] : 255U;
    }
    return true;
}

struct PinPremultipliedPixel {
    std::uint8_t blue{};
    std::uint8_t green{};
    std::uint8_t red{};
    std::uint8_t alpha{};
};

[[nodiscard]] inline PinPremultipliedPixel pin_premultiplied_visual_pixel(
    const Bitmap& source,
    std::size_t offset,
    PinVisualEffects effects,
    bool alpha_is_defined) noexcept {
    std::uint8_t blue = source.pixels[offset];
    std::uint8_t green = source.pixels[offset + 1];
    std::uint8_t red = source.pixels[offset + 2];
    if (effects.grayscale) {
        const unsigned int gray =
            (29U * blue + 150U * green + 77U * red + 128U) >> 8U;
        blue = green = red = static_cast<std::uint8_t>(gray);
    }
    if (effects.inverted) {
        blue = static_cast<std::uint8_t>(255U - blue);
        green = static_cast<std::uint8_t>(255U - green);
        red = static_cast<std::uint8_t>(255U - red);
    }
    const std::uint8_t alpha =
        alpha_is_defined ? source.pixels[offset + 3] : 255U;
    const auto premultiply = [alpha](std::uint8_t channel) noexcept {
        return static_cast<std::uint8_t>(
            (static_cast<unsigned int>(channel) * alpha + 127U) / 255U);
    };
    return {
        premultiply(blue),
        premultiply(green),
        premultiply(red),
        alpha,
    };
}

inline constexpr std::size_t kMaximumLayeredPinPresentationBytes =
    256U * 1024U * 1024U;

// UpdateLayeredWindow consumes premultiplied BGRA at the window's actual size.
// This renderer keeps that platform-specific representation separate from the
// source Bitmap, which remains straight-alpha and lossless for copy/save.
[[nodiscard]] inline Bitmap render_pin_layered_bitmap(
    const Bitmap& source,
    PinVisualEffects effects,
    int target_width,
    int target_height,
    bool smooth_scaling) {
    if (!pin_bitmap_storage_is_valid(source)) {
        return {};
    }
    const auto target_bytes =
        Bitmap::checked_byte_size(target_width, target_height);
    if (!target_bytes ||
        *target_bytes > kMaximumLayeredPinPresentationBytes) {
        return {};
    }
    Bitmap target(target_width, target_height);
    if (!target.valid()) {
        return {};
    }

    const bool alpha_is_defined =
        summarize_pin_bitmap_alpha(source).alpha_is_defined;
    const auto sample = [&](int x, int y) noexcept {
        const std::size_t offset =
            (static_cast<std::size_t>(y) *
                 static_cast<std::size_t>(source.width) +
             static_cast<std::size_t>(x)) *
            Bitmap::bytes_per_pixel;
        return pin_premultiplied_visual_pixel(
            source,
            offset,
            effects,
            alpha_is_defined);
    };
    const auto store = [&](int x,
                           int y,
                           PinPremultipliedPixel pixel) noexcept {
        auto row = target.row(y);
        const std::size_t offset =
            static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
        row[offset] = pixel.blue;
        row[offset + 1] = pixel.green;
        row[offset + 2] = pixel.red;
        row[offset + 3] = pixel.alpha;
    };

    if (!smooth_scaling) {
        for (int y = 0; y < target_height; ++y) {
            const int source_y = std::min(
                source.height - 1,
                static_cast<int>(
                    ((static_cast<long long>(y) * 2LL + 1LL) *
                     source.height) /
                    (static_cast<long long>(target_height) * 2LL)));
            for (int x = 0; x < target_width; ++x) {
                const int source_x = std::min(
                    source.width - 1,
                    static_cast<int>(
                        ((static_cast<long long>(x) * 2LL + 1LL) *
                         source.width) /
                        (static_cast<long long>(target_width) * 2LL)));
                store(x, y, sample(source_x, source_y));
            }
        }
        return target;
    }

    for (int y = 0; y < target_height; ++y) {
        const double source_y =
            (static_cast<double>(y) + 0.5) * source.height /
                target_height -
            0.5;
        int y0 = static_cast<int>(std::floor(source_y));
        double fy = source_y - y0;
        if (y0 < 0) {
            y0 = 0;
            fy = 0.0;
        }
        const int y1 = std::min(source.height - 1, y0 + 1);
        if (y0 >= source.height - 1) {
            y0 = source.height - 1;
            fy = 0.0;
        }
        for (int x = 0; x < target_width; ++x) {
            const double source_x =
                (static_cast<double>(x) + 0.5) * source.width /
                    target_width -
                0.5;
            int x0 = static_cast<int>(std::floor(source_x));
            double fx = source_x - x0;
            if (x0 < 0) {
                x0 = 0;
                fx = 0.0;
            }
            const int x1 = std::min(source.width - 1, x0 + 1);
            if (x0 >= source.width - 1) {
                x0 = source.width - 1;
                fx = 0.0;
            }

            const PinPremultipliedPixel top_left = sample(x0, y0);
            const PinPremultipliedPixel top_right = sample(x1, y0);
            const PinPremultipliedPixel bottom_left = sample(x0, y1);
            const PinPremultipliedPixel bottom_right = sample(x1, y1);
            const auto interpolate = [&](std::uint8_t tl,
                                         std::uint8_t tr,
                                         std::uint8_t bl,
                                         std::uint8_t br) noexcept {
                const double top = tl + (tr - tl) * fx;
                const double bottom = bl + (br - bl) * fx;
                return static_cast<std::uint8_t>(std::clamp(
                    std::lround(top + (bottom - top) * fy),
                    0L,
                    255L));
            };
            store(
                x,
                y,
                {
                    interpolate(
                        top_left.blue,
                        top_right.blue,
                        bottom_left.blue,
                        bottom_right.blue),
                    interpolate(
                        top_left.green,
                        top_right.green,
                        bottom_left.green,
                        bottom_right.green),
                    interpolate(
                        top_left.red,
                        top_right.red,
                        bottom_left.red,
                        bottom_right.red),
                    interpolate(
                        top_left.alpha,
                        top_right.alpha,
                        bottom_left.alpha,
                        bottom_right.alpha),
                });
        }
    }
    return target;
}

[[nodiscard]] inline Bitmap render_pin_visual_bitmap(
    const Bitmap& source,
    PinVisualEffects effects) {
    if (!pin_bitmap_storage_is_valid(source)) {
        return {};
    }
    try {
        Bitmap result = source;
        if (!write_pin_visual_pixels(source, effects, result.pixels)) {
            return {};
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
}

[[nodiscard]] constexpr int clamp_pin_scale_percent(int percent) noexcept {
    return std::clamp(percent, 10, 1000);
}

[[nodiscard]] constexpr double pin_scale_factor_from_percent(
    int percent) noexcept {
    return static_cast<double>(clamp_pin_scale_percent(percent)) / 100.0;
}

[[nodiscard]] constexpr int pin_scale_percent_from_factor(
    double scale) noexcept {
    if (!(scale > 0.0)) {
        return 100;
    }
    const double percent = scale * 100.0;
    if (percent <= 10.0) {
        return 10;
    }
    if (percent >= 1000.0) {
        return 1000;
    }
    return static_cast<int>(percent + 0.5);
}

[[nodiscard]] constexpr std::optional<int> parse_pin_scale_percent(
    std::wstring_view value) noexcept {
    while (!value.empty() &&
           (value.front() == L' ' || value.front() == L'\t' ||
            value.front() == L'\r' || value.front() == L'\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == L' ' || value.back() == L'\t' ||
            value.back() == L'\r' || value.back() == L'\n')) {
        value.remove_suffix(1);
    }
    if (!value.empty() && value.back() == L'%') {
        value.remove_suffix(1);
        while (!value.empty() &&
               (value.back() == L' ' || value.back() == L'\t')) {
            value.remove_suffix(1);
        }
    }
    if (value.empty()) {
        return std::nullopt;
    }
    int percent = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const int digit = character - L'0';
        if (percent > (1000 - digit) / 10) {
            return std::nullopt;
        }
        percent = percent * 10 + digit;
    }
    if (percent < 10 || percent > 1000) {
        return std::nullopt;
    }
    return percent;
}

struct PinScalePromptPlan {
    bool apply{};
    int percent{100};
};

[[nodiscard]] constexpr PinScalePromptPlan plan_pin_scale_prompt(
    std::optional<int> submitted_percent) noexcept {
    if (!submitted_percent || *submitted_percent < 10 ||
        *submitted_percent > 1000) {
        return {};
    }
    return {true, *submitted_percent};
}

inline constexpr std::size_t kMaximumPinCount = 32;
inline constexpr std::size_t kMaximumPinBytes =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaximumTotalPinBytes =
    192ULL * 1024ULL * 1024ULL;

enum class PinLifecycleState {
    visible,
    hidden,
    destroyed,
};

enum class PinLifecycleAction {
    hide,
    show,
    destroy,
};

[[nodiscard]] constexpr PinLifecycleState transition_pin_lifecycle(
    PinLifecycleState state,
    PinLifecycleAction action) noexcept {
    if (state == PinLifecycleState::destroyed) {
        return state;
    }
    switch (action) {
        case PinLifecycleAction::hide:
            return PinLifecycleState::hidden;
        case PinLifecycleAction::show:
            return PinLifecycleState::visible;
        case PinLifecycleAction::destroy:
            return PinLifecycleState::destroyed;
    }
    return state;
}

[[nodiscard]] constexpr bool should_restore_pin_after_capture(
    PinLifecycleState state,
    bool visible_before_capture) noexcept {
    return visible_before_capture && state == PinLifecycleState::visible;
}

enum class PinBudgetStatus {
    allowed,
    invalid_bitmap,
    pin_count_limit,
    single_pin_limit,
    total_pin_limit,
};

struct PinBudgetPlan {
    PinBudgetStatus status{PinBudgetStatus::invalid_bitmap};
    std::size_t total_bytes_after{};

    [[nodiscard]] constexpr bool allowed() const noexcept {
        return status == PinBudgetStatus::allowed;
    }
};

// current_bytes is zero when creating a new pin and is the existing pin's
// allocation when replacing it. total_bytes includes current_bytes.
[[nodiscard]] constexpr PinBudgetPlan plan_pin_bitmap_change(
    std::size_t pin_count,
    std::size_t total_bytes,
    std::size_t current_bytes,
    std::size_t candidate_bytes) noexcept {
    if (candidate_bytes == 0 || current_bytes > total_bytes) {
        return {PinBudgetStatus::invalid_bitmap, total_bytes};
    }
    if (candidate_bytes > kMaximumPinBytes) {
        return {PinBudgetStatus::single_pin_limit, total_bytes};
    }
    if (current_bytes == 0 && pin_count >= kMaximumPinCount) {
        return {PinBudgetStatus::pin_count_limit, total_bytes};
    }
    const std::size_t retained_bytes = total_bytes - current_bytes;
    if (candidate_bytes > kMaximumTotalPinBytes ||
        retained_bytes > kMaximumTotalPinBytes - candidate_bytes) {
        return {PinBudgetStatus::total_pin_limit, total_bytes};
    }
    return {
        PinBudgetStatus::allowed,
        retained_bytes + candidate_bytes,
    };
}

struct PinStateView {
    bool hidden{};
    bool click_through{};
    bool contains_cursor{};
};

struct PinStateCounts {
    std::size_t total{};
    std::size_t visible{};
    std::size_t hidden{};
    std::size_t click_through{};
};

[[nodiscard]] constexpr PinStateCounts summarize_pin_states(
    std::span<const PinStateView> pins) noexcept {
    PinStateCounts result;
    result.total = pins.size();
    for (const PinStateView& pin : pins) {
        if (pin.hidden) {
            ++result.hidden;
        } else {
            ++result.visible;
        }
        if (pin.click_through) {
            ++result.click_through;
        }
    }
    return result;
}

enum class PinToggleAction {
    none,
    toggle_target,
    restore_all,
};

struct PinTogglePlan {
    PinToggleAction action{PinToggleAction::none};
    std::size_t target_index{std::numeric_limits<std::size_t>::max()};
};

// pins must be ordered from the top of the desktop Z order to the bottom.
[[nodiscard]] constexpr PinTogglePlan plan_pin_toggle(
    std::span<const PinStateView> pins) noexcept {
    for (std::size_t index = 0; index < pins.size(); ++index) {
        if (!pins[index].hidden && pins[index].contains_cursor) {
            return {PinToggleAction::toggle_target, index};
        }
    }
    for (const PinStateView& pin : pins) {
        if (pin.click_through) {
            return {PinToggleAction::restore_all, {}};
        }
    }
    return {};
}

}  // namespace airshot
