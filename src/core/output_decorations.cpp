#include "airshot/output_decorations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace airshot {
namespace {

struct CanvasPlan {
    int width{};
    int height{};
    int source_x{};
    int source_y{};
    std::size_t pixels{};
    std::size_t working_bytes{};
};

[[nodiscard]] bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool valid_options(
    const OutputDecorationOptions& options) noexcept {
    return options.border_width >= 0 &&
           options.border_width <=
               OutputDecorationOptions::maximum_border_width &&
           options.shadow_blur_radius >= 0 &&
           options.shadow_blur_radius <=
               OutputDecorationOptions::maximum_shadow_blur_radius &&
           options.shadow_offset_x >=
               -OutputDecorationOptions::maximum_shadow_offset &&
           options.shadow_offset_x <=
               OutputDecorationOptions::maximum_shadow_offset &&
           options.shadow_offset_y >=
               -OutputDecorationOptions::maximum_shadow_offset &&
           options.shadow_offset_y <=
               OutputDecorationOptions::maximum_shadow_offset &&
           options.shadow_opacity >= 0 &&
           options.shadow_opacity <= 255;
}

[[nodiscard]] bool has_visible_alpha(const Bitmap& source) noexcept {
    for (std::size_t offset = 3;
         offset < source.pixels.size();
         offset += Bitmap::bytes_per_pixel) {
        if (source.pixels[offset] != 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<CanvasPlan> plan_canvas(
    const Bitmap& source,
    const OutputDecorationOptions& options,
    const OutputDecorationLimits& limits) noexcept {
    const bool border_enabled = options.border_width > 0;
    const bool shadow_enabled = options.shadow_opacity > 0;

    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = source.width;
    std::int64_t bottom = source.height;
    if (border_enabled) {
        const std::int64_t width = options.border_width;
        left = std::min(left, -width);
        top = std::min(top, -width);
        right = std::max(right, static_cast<std::int64_t>(source.width) + width);
        bottom = std::max(bottom, static_cast<std::int64_t>(source.height) + width);
    }
    if (shadow_enabled) {
        const std::int64_t radius = options.shadow_blur_radius;
        const std::int64_t offset_x = options.shadow_offset_x;
        const std::int64_t offset_y = options.shadow_offset_y;
        left = std::min(left, offset_x - radius);
        top = std::min(top, offset_y - radius);
        right = std::max(
            right,
            static_cast<std::int64_t>(source.width) + offset_x + radius);
        bottom = std::max(
            bottom,
            static_cast<std::int64_t>(source.height) + offset_y + radius);
    }

    const std::int64_t width = right - left;
    const std::int64_t height = bottom - top;
    const std::int64_t source_x = -left;
    const std::int64_t source_y = -top;
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max() ||
        source_x < 0 || source_y < 0 ||
        source_x > std::numeric_limits<int>::max() ||
        source_y > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    const int output_width = static_cast<int>(width);
    const int output_height = static_cast<int>(height);
    const auto output_bytes =
        Bitmap::checked_byte_size(output_width, output_height);
    if (!output_bytes) {
        return std::nullopt;
    }
    const std::size_t output_pixels =
        *output_bytes / Bitmap::bytes_per_pixel;
    const std::size_t maximum_pixels = std::min(
        limits.max_output_pixels,
        OutputDecorationLimits::hard_max_output_pixels);
    const std::size_t maximum_working_bytes = std::min(
        limits.max_working_bytes,
        OutputDecorationLimits::hard_max_working_bytes);
    if (output_pixels > maximum_pixels) {
        return std::nullopt;
    }

    std::size_t mask_bytes = 0;
    std::size_t working_bytes = *output_bytes;
    if ((border_enabled || shadow_enabled) &&
        (!checked_multiply(output_pixels, 2, mask_bytes) ||
         !checked_add(working_bytes, mask_bytes, working_bytes))) {
        return std::nullopt;
    }
    std::size_t queue_bytes = 0;
    if (border_enabled &&
        (!checked_multiply(
             static_cast<std::size_t>(
                 std::max(output_width, output_height)),
             sizeof(int),
             queue_bytes) ||
         !checked_add(working_bytes, queue_bytes, working_bytes))) {
        return std::nullopt;
    }
    if (working_bytes > maximum_working_bytes) {
        return std::nullopt;
    }

    return CanvasPlan{
        output_width,
        output_height,
        static_cast<int>(source_x),
        static_cast<int>(source_y),
        output_pixels,
        working_bytes,
    };
}

void seed_alpha_mask(
    std::vector<std::uint8_t>& mask,
    int mask_width,
    const Bitmap& source,
    int destination_x,
    int destination_y) noexcept {
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        const std::size_t target_row =
            static_cast<std::size_t>(destination_y + y) *
            static_cast<std::size_t>(mask_width);
        for (int x = 0; x < source.width; ++x) {
            mask[target_row + static_cast<std::size_t>(destination_x + x)] =
                source_row[static_cast<std::size_t>(x) *
                               Bitmap::bytes_per_pixel +
                           3];
        }
    }
}

void box_blur_horizontal(
    const std::vector<std::uint8_t>& source,
    std::vector<std::uint8_t>& target,
    int width,
    int height,
    int radius) noexcept {
    const std::uint64_t divisor =
        static_cast<std::uint64_t>(radius) * 2U + 1U;
    for (int y = 0; y < height; ++y) {
        const std::size_t row =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        std::uint64_t sum = 0;
        const int initial_right = std::min(width - 1, radius);
        for (int x = 0; x <= initial_right; ++x) {
            sum += source[row + static_cast<std::size_t>(x)];
        }
        for (int x = 0; x < width; ++x) {
            target[row + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((sum + divisor / 2U) / divisor);
            const int remove_x = x - radius;
            const int add_x = x + radius + 1;
            if (remove_x >= 0) {
                sum -= source[row + static_cast<std::size_t>(remove_x)];
            }
            if (add_x < width) {
                sum += source[row + static_cast<std::size_t>(add_x)];
            }
        }
    }
}

void box_blur_vertical(
    const std::vector<std::uint8_t>& source,
    std::vector<std::uint8_t>& target,
    int width,
    int height,
    int radius) noexcept {
    const std::uint64_t divisor =
        static_cast<std::uint64_t>(radius) * 2U + 1U;
    for (int x = 0; x < width; ++x) {
        std::uint64_t sum = 0;
        const int initial_bottom = std::min(height - 1, radius);
        for (int y = 0; y <= initial_bottom; ++y) {
            sum += source[static_cast<std::size_t>(y) *
                              static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)];
        }
        for (int y = 0; y < height; ++y) {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            target[index] =
                static_cast<std::uint8_t>((sum + divisor / 2U) / divisor);
            const int remove_y = y - radius;
            const int add_y = y + radius + 1;
            if (remove_y >= 0) {
                sum -= source[static_cast<std::size_t>(remove_y) *
                                  static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)];
            }
            if (add_y < height) {
                sum += source[static_cast<std::size_t>(add_y) *
                                  static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)];
            }
        }
    }
}

void soften_alpha_mask(
    std::vector<std::uint8_t>& mask,
    std::vector<std::uint8_t>& scratch,
    int width,
    int height,
    int radius) noexcept {
    // Three bounded box convolutions approximate a Gaussian while their radii
    // add up to the requested support, keeping canvas planning exact.
    const std::array<int, 3> radii{
        radius / 3,
        (radius + 1) / 3,
        radius - radius / 3 - (radius + 1) / 3,
    };
    for (const int pass_radius : radii) {
        if (pass_radius <= 0) {
            continue;
        }
        box_blur_horizontal(mask, scratch, width, height, pass_radius);
        box_blur_vertical(scratch, mask, width, height, pass_radius);
    }
}

void max_filter_horizontal(
    const std::vector<std::uint8_t>& source,
    std::vector<std::uint8_t>& target,
    int width,
    int height,
    int radius,
    std::vector<int>& queue) noexcept {
    for (int y = 0; y < height; ++y) {
        const std::size_t row =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        int head = 0;
        int tail = 0;
        int next = 0;
        for (int x = 0; x < width; ++x) {
            const int add_through = std::min(width - 1, x + radius);
            while (next <= add_through) {
                const std::uint8_t value =
                    source[row + static_cast<std::size_t>(next)];
                while (tail > head &&
                       source[row + static_cast<std::size_t>(queue[tail - 1])] <=
                           value) {
                    --tail;
                }
                queue[tail++] = next++;
            }
            const int remove_before = x - radius;
            while (tail > head && queue[head] < remove_before) {
                ++head;
            }
            target[row + static_cast<std::size_t>(x)] =
                tail > head
                    ? source[row + static_cast<std::size_t>(queue[head])]
                    : 0;
        }
    }
}

void max_filter_vertical(
    const std::vector<std::uint8_t>& source,
    std::vector<std::uint8_t>& target,
    int width,
    int height,
    int radius,
    std::vector<int>& queue) noexcept {
    for (int x = 0; x < width; ++x) {
        int head = 0;
        int tail = 0;
        int next = 0;
        for (int y = 0; y < height; ++y) {
            const int add_through = std::min(height - 1, y + radius);
            while (next <= add_through) {
                const std::size_t index =
                    static_cast<std::size_t>(next) *
                        static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                const std::uint8_t value = source[index];
                while (tail > head) {
                    const std::size_t queued_index =
                        static_cast<std::size_t>(queue[tail - 1]) *
                            static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x);
                    if (source[queued_index] > value) {
                        break;
                    }
                    --tail;
                }
                queue[tail++] = next++;
            }
            const int remove_before = y - radius;
            while (tail > head && queue[head] < remove_before) {
                ++head;
            }
            const std::size_t target_index =
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            if (tail > head) {
                const std::size_t source_index =
                    static_cast<std::size_t>(queue[head]) *
                        static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                target[target_index] = source[source_index];
            } else {
                target[target_index] = 0;
            }
        }
    }
}

void source_over(
    std::uint8_t* destination,
    std::uint8_t blue,
    std::uint8_t green,
    std::uint8_t red,
    std::uint8_t alpha) noexcept {
    if (alpha == 0) {
        return;
    }
    if (alpha == 255 || destination[3] == 0) {
        destination[0] = blue;
        destination[1] = green;
        destination[2] = red;
        destination[3] = alpha;
        return;
    }

    const std::uint64_t source_alpha = alpha;
    const std::uint64_t destination_alpha = destination[3];
    const std::uint64_t inverse_alpha = 255U - source_alpha;
    const std::uint64_t output_alpha_numerator =
        source_alpha * 255U + destination_alpha * inverse_alpha;
    const std::array<std::uint8_t, 3> source_channels{blue, green, red};
    for (std::size_t channel = 0; channel < source_channels.size(); ++channel) {
        const std::uint64_t output_color_numerator =
            static_cast<std::uint64_t>(source_channels[channel]) *
                source_alpha * 255U +
            static_cast<std::uint64_t>(destination[channel]) *
                destination_alpha * inverse_alpha;
        destination[channel] = static_cast<std::uint8_t>(
            (output_color_numerator + output_alpha_numerator / 2U) /
            output_alpha_numerator);
    }
    destination[3] = static_cast<std::uint8_t>(
        (output_alpha_numerator + 127U) / 255U);
}

void composite_color_mask(
    Bitmap& target,
    const std::vector<std::uint8_t>& mask,
    COLORREF color,
    int opacity) noexcept {
    const std::uint8_t blue = GetBValue(color);
    const std::uint8_t green = GetGValue(color);
    const std::uint8_t red = GetRValue(color);
    for (std::size_t index = 0; index < mask.size(); ++index) {
        const unsigned int alpha =
            (static_cast<unsigned int>(mask[index]) *
                 static_cast<unsigned int>(opacity) +
             127U) /
            255U;
        if (alpha == 0) {
            continue;
        }
        source_over(
            target.pixels.data() + index * Bitmap::bytes_per_pixel,
            blue,
            green,
            red,
            static_cast<std::uint8_t>(alpha));
    }
}

void composite_border(
    Bitmap& target,
    const Bitmap& source,
    const CanvasPlan& plan,
    const std::vector<std::uint8_t>& dilated,
    COLORREF color) noexcept {
    const std::uint8_t blue = GetBValue(color);
    const std::uint8_t green = GetGValue(color);
    const std::uint8_t red = GetRValue(color);
    for (int y = 0; y < plan.height; ++y) {
        for (int x = 0; x < plan.width; ++x) {
            const std::size_t mask_index =
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(plan.width) +
                static_cast<std::size_t>(x);
            std::uint8_t source_alpha = 0;
            const int source_local_x = x - plan.source_x;
            const int source_local_y = y - plan.source_y;
            if (source_local_x >= 0 && source_local_x < source.width &&
                source_local_y >= 0 && source_local_y < source.height) {
                source_alpha = source.row(source_local_y)[
                    static_cast<std::size_t>(source_local_x) *
                        Bitmap::bytes_per_pixel +
                    3];
            }
            if (dilated[mask_index] <= source_alpha) {
                continue;
            }
            source_over(
                target.pixels.data() +
                    mask_index * Bitmap::bytes_per_pixel,
                blue,
                green,
                red,
                static_cast<std::uint8_t>(
                    dilated[mask_index] - source_alpha));
        }
    }
}

void composite_source(
    Bitmap& target,
    const Bitmap& source,
    const CanvasPlan& plan) noexcept {
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        auto target_row = target.row(plan.source_y + y);
        for (int x = 0; x < source.width; ++x) {
            const std::size_t source_offset =
                static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            const std::size_t target_offset =
                static_cast<std::size_t>(plan.source_x + x) *
                Bitmap::bytes_per_pixel;
            source_over(
                target_row.data() + target_offset,
                source_row[source_offset],
                source_row[source_offset + 1],
                source_row[source_offset + 2],
                source_row[source_offset + 3]);
        }
    }
}

[[nodiscard]] OutputDecorationResult copy_unchanged(
    const Bitmap& source,
    const OutputDecorationLimits& limits) {
    const std::size_t pixels =
        source.pixels.size() / Bitmap::bytes_per_pixel;
    const std::size_t maximum_pixels = std::min(
        limits.max_output_pixels,
        OutputDecorationLimits::hard_max_output_pixels);
    const std::size_t maximum_working_bytes = std::min(
        limits.max_working_bytes,
        OutputDecorationLimits::hard_max_working_bytes);
    if (pixels > maximum_pixels ||
        source.pixels.size() > maximum_working_bytes) {
        return {OutputDecorationStatus::resource_limit, {}};
    }
    return {OutputDecorationStatus::no_change, source};
}

}  // namespace

OutputDecorationResult decorate_output_bitmap(
    const Bitmap& source,
    const OutputDecorationOptions& options,
    const OutputDecorationLimits& limits) noexcept {
    if (!source.valid()) {
        return {OutputDecorationStatus::invalid_input, {}};
    }
    if (!valid_options(options)) {
        return {OutputDecorationStatus::invalid_options, {}};
    }

    try {
        const bool border_enabled = options.border_width > 0;
        const bool shadow_enabled = options.shadow_opacity > 0;
        if ((!border_enabled && !shadow_enabled) ||
            !has_visible_alpha(source)) {
            return copy_unchanged(source, limits);
        }

        const auto plan = plan_canvas(source, options, limits);
        if (!plan) {
            return {OutputDecorationStatus::resource_limit, {}};
        }

        Bitmap output(plan->width, plan->height);
        if (!output.valid()) {
            return {OutputDecorationStatus::resource_limit, {}};
        }
        std::fill(
            output.pixels.begin(),
            output.pixels.end(),
            std::uint8_t{0});

        std::vector<std::uint8_t> mask(plan->pixels, 0);
        std::vector<std::uint8_t> scratch(plan->pixels, 0);
        if (shadow_enabled) {
            seed_alpha_mask(
                mask,
                plan->width,
                source,
                plan->source_x + options.shadow_offset_x,
                plan->source_y + options.shadow_offset_y);
            soften_alpha_mask(
                mask,
                scratch,
                plan->width,
                plan->height,
                options.shadow_blur_radius);
            composite_color_mask(
                output,
                mask,
                options.shadow_color,
                options.shadow_opacity);
        }

        if (border_enabled) {
            std::fill(mask.begin(), mask.end(), std::uint8_t{0});
            seed_alpha_mask(
                mask,
                plan->width,
                source,
                plan->source_x,
                plan->source_y);
            std::vector<int> queue(static_cast<std::size_t>(
                std::max(plan->width, plan->height)));
            max_filter_horizontal(
                mask,
                scratch,
                plan->width,
                plan->height,
                options.border_width,
                queue);
            max_filter_vertical(
                scratch,
                mask,
                plan->width,
                plan->height,
                options.border_width,
                queue);
            composite_border(
                output,
                source,
                *plan,
                mask,
                options.border_color);
        }

        composite_source(output, source, *plan);
        return {OutputDecorationStatus::applied, std::move(output)};
    } catch (const std::bad_alloc&) {
        return {OutputDecorationStatus::resource_limit, {}};
    } catch (const std::length_error&) {
        return {OutputDecorationStatus::resource_limit, {}};
    } catch (...) {
        return {OutputDecorationStatus::resource_limit, {}};
    }
}

}  // namespace airshot
