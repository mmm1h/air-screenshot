#pragma once

#include "airshot/bitmap.h"

#include <algorithm>
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
        destination[offset + 3] = source.pixels[offset + 3];
    }
    return true;
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
