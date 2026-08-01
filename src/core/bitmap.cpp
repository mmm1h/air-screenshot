#include "airshot/bitmap.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace airshot {
namespace {

struct WideRect {
    std::int64_t left{};
    std::int64_t top{};
    std::int64_t right{};
    std::int64_t bottom{};
};

[[nodiscard]] WideRect normalize_wide(const RectI& rect) noexcept {
    return {
        std::min<std::int64_t>(rect.left, rect.right),
        std::min<std::int64_t>(rect.top, rect.bottom),
        std::max<std::int64_t>(rect.left, rect.right),
        std::max<std::int64_t>(rect.top, rect.bottom),
    };
}

[[nodiscard]] bool clip_to_bitmap(const Bitmap& bitmap, const RectI& rect, RectI& clipped) noexcept {
    if (!bitmap.valid()) {
        return false;
    }
    const WideRect normalized = normalize_wide(rect);
    const std::int64_t left = std::max<std::int64_t>(0, normalized.left);
    const std::int64_t top = std::max<std::int64_t>(0, normalized.top);
    const std::int64_t right = std::min<std::int64_t>(bitmap.width, normalized.right);
    const std::int64_t bottom = std::min<std::int64_t>(bitmap.height, normalized.bottom);
    if (left >= right || top >= bottom) {
        return false;
    }
    clipped = {
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(right),
        static_cast<int>(bottom),
    };
    return true;
}

[[nodiscard]] std::uint64_t squared_magnitude(std::int64_t value) noexcept {
    const std::uint64_t magnitude =
        value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U : static_cast<std::uint64_t>(value);
    return magnitude * magnitude;
}

[[nodiscard]] bool inside_circle(std::int64_t x,
                                 std::int64_t y,
                                 std::int64_t center_x,
                                 std::int64_t center_y,
                                 std::uint64_t radius_squared) noexcept {
    const std::uint64_t x_squared = squared_magnitude(x - center_x);
    if (x_squared > radius_squared) {
        return false;
    }
    return squared_magnitude(y - center_y) <= radius_squared - x_squared;
}

[[nodiscard]] Bitmap box_blur(const Bitmap& source, int radius) {
    if (!source.valid() || radius <= 0) {
        return {};
    }

    Bitmap horizontal(source.width, source.height);
    Bitmap result(source.width, source.height);
    if (horizontal.empty() || result.empty()) {
        return {};
    }

    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        auto horizontal_row = horizontal.row(y);
        std::array<std::uint64_t, 3> sums{};
        const int initial_right = std::min(source.width - 1, radius);
        for (int x = 0; x <= initial_right; ++x) {
            const auto* pixel = source_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                sums[channel] += pixel[channel];
            }
        }

        int count = initial_right + 1;
        for (int x = 0; x < source.width; ++x) {
            auto* output = horizontal_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                output[channel] = static_cast<std::uint8_t>(sums[channel] / static_cast<std::uint64_t>(count));
            }
            output[3] = 255;

            const std::int64_t remove_x = static_cast<std::int64_t>(x) - radius;
            if (remove_x >= 0) {
                const auto* pixel =
                    source_row.data() + static_cast<std::size_t>(remove_x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] -= pixel[channel];
                }
                --count;
            }
            const std::int64_t add_x = static_cast<std::int64_t>(x) + radius + 1LL;
            if (add_x < source.width) {
                const auto* pixel =
                    source_row.data() + static_cast<std::size_t>(add_x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] += pixel[channel];
                }
                ++count;
            }
        }
    }

    for (int x = 0; x < source.width; ++x) {
        std::array<std::uint64_t, 3> sums{};
        const int initial_bottom = std::min(source.height - 1, radius);
        for (int y = 0; y <= initial_bottom; ++y) {
            const auto row = horizontal.row(y);
            const auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                sums[channel] += pixel[channel];
            }
        }

        int count = initial_bottom + 1;
        for (int y = 0; y < source.height; ++y) {
            auto output_row = result.row(y);
            auto* output = output_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                output[channel] = static_cast<std::uint8_t>(sums[channel] / static_cast<std::uint64_t>(count));
            }
            output[3] = 255;

            const std::int64_t remove_y = static_cast<std::int64_t>(y) - radius;
            if (remove_y >= 0) {
                const auto row = horizontal.row(static_cast<int>(remove_y));
                const auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] -= pixel[channel];
                }
                --count;
            }
            const std::int64_t add_y = static_cast<std::int64_t>(y) + radius + 1LL;
            if (add_y < source.height) {
                const auto row = horizontal.row(static_cast<int>(add_y));
                const auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] += pixel[channel];
                }
                ++count;
            }
        }
    }
    return result;
}

struct PrivacyCoverageMask {
    RectI bounds{};
    std::vector<std::uint8_t> coverage;

    [[nodiscard]] int width() const noexcept { return bounds.width(); }
    [[nodiscard]] int height() const noexcept { return bounds.height(); }
    [[nodiscard]] bool valid() const noexcept {
        if (bounds.empty()) {
            return false;
        }
        const std::size_t mask_width = static_cast<std::size_t>(width());
        const std::size_t mask_height = static_cast<std::size_t>(height());
        return mask_height <= std::numeric_limits<std::size_t>::max() / mask_width &&
               coverage.size() == mask_width * mask_height;
    }
    [[nodiscard]] std::span<std::uint8_t> row(int y) noexcept {
        if (!valid() || y < bounds.top || y >= bounds.bottom) {
            return {};
        }
        const std::size_t offset =
            static_cast<std::size_t>(y - bounds.top) * static_cast<std::size_t>(width());
        return std::span(coverage).subspan(offset, static_cast<std::size_t>(width()));
    }
    [[nodiscard]] std::span<const std::uint8_t> row(int y) const noexcept {
        if (!valid() || y < bounds.top || y >= bounds.bottom) {
            return {};
        }
        const std::size_t offset =
            static_cast<std::size_t>(y - bounds.top) * static_cast<std::size_t>(width());
        return std::span(coverage).subspan(offset, static_cast<std::size_t>(width()));
    }
};

[[nodiscard]] bool same_point(POINT left, POINT right) noexcept {
    return left.x == right.x && left.y == right.y;
}

[[nodiscard]] bool extends_same_line(POINT first, POINT middle, POINT last) noexcept {
    const long double first_x = static_cast<long double>(middle.x) - first.x;
    const long double first_y = static_cast<long double>(middle.y) - first.y;
    const long double second_x = static_cast<long double>(last.x) - middle.x;
    const long double second_y = static_cast<long double>(last.y) - middle.y;
    const long double cross = first_x * second_y - first_y * second_x;
    const long double dot = first_x * second_x + first_y * second_y;
    return cross == 0.0L && dot >= 0.0L;
}

[[nodiscard]] std::vector<POINT> compact_collinear_points(
    std::span<const POINT> points) {
    std::vector<POINT> compact;
    compact.reserve(points.size());
    for (const POINT point : points) {
        if (!compact.empty() && same_point(compact.back(), point)) {
            continue;
        }
        if (compact.size() >= 2 &&
            extends_same_line(compact[compact.size() - 2], compact.back(), point)) {
            compact.back() = point;
            continue;
        }
        compact.push_back(point);
    }
    return compact;
}

[[nodiscard]] POINT interpolate_point(POINT first, POINT second, double ratio) noexcept {
    const double x = static_cast<double>(first.x) +
                     (static_cast<double>(second.x) - first.x) * ratio;
    const double y = static_cast<double>(first.y) +
                     (static_cast<double>(second.y) - first.y) * ratio;
    const double long_min = static_cast<double>(std::numeric_limits<LONG>::min());
    const double long_max = static_cast<double>(std::numeric_limits<LONG>::max());
    return {
        static_cast<LONG>(std::clamp(std::round(x), long_min, long_max)),
        static_cast<LONG>(std::clamp(std::round(y), long_min, long_max)),
    };
}

[[nodiscard]] std::vector<POINT> resample_privacy_points(
    std::span<const POINT> points,
    std::size_t target_count) {
    if (points.size() <= target_count) {
        return std::vector<POINT>(points.begin(), points.end());
    }
    if (target_count <= 1 || points.size() == 1) {
        return {points.front()};
    }

    double total_length = 0.0;
    for (std::size_t index = 1; index < points.size(); ++index) {
        total_length += std::hypot(
            static_cast<double>(points[index].x) - points[index - 1].x,
            static_cast<double>(points[index].y) - points[index - 1].y);
    }
    if (!std::isfinite(total_length) || total_length <= 0.0) {
        return {points.front()};
    }

    std::vector<POINT> result;
    result.reserve(target_count);
    result.push_back(points.front());
    std::size_t segment = 1;
    double traversed = 0.0;
    double segment_length = std::hypot(
        static_cast<double>(points[segment].x) - points[segment - 1].x,
        static_cast<double>(points[segment].y) - points[segment - 1].y);
    for (std::size_t sample = 1; sample + 1 < target_count; ++sample) {
        const double target =
            total_length * static_cast<double>(sample) /
            static_cast<double>(target_count - 1);
        while (segment + 1 < points.size() &&
               traversed + segment_length < target) {
            traversed += segment_length;
            ++segment;
            segment_length = std::hypot(
                static_cast<double>(points[segment].x) - points[segment - 1].x,
                static_cast<double>(points[segment].y) - points[segment - 1].y);
        }
        const double ratio = segment_length > 0.0
                                 ? std::clamp((target - traversed) / segment_length, 0.0, 1.0)
                                 : 0.0;
        const POINT point = interpolate_point(points[segment - 1], points[segment], ratio);
        if (!same_point(result.back(), point)) {
            result.push_back(point);
        }
    }
    if (!same_point(result.back(), points.back())) {
        result.push_back(points.back());
    }
    return compact_collinear_points(result);
}

[[nodiscard]] std::optional<RectI> privacy_mask_bounds(
    const Bitmap& bitmap,
    std::span<const POINT> points,
    int brush_radius) noexcept {
    if (!bitmap.valid() || points.empty()) {
        return std::nullopt;
    }
    std::int64_t minimum_x = points.front().x;
    std::int64_t minimum_y = points.front().y;
    std::int64_t maximum_x = points.front().x;
    std::int64_t maximum_y = points.front().y;
    for (const POINT point : points.subspan(1)) {
        minimum_x = std::min<std::int64_t>(minimum_x, point.x);
        minimum_y = std::min<std::int64_t>(minimum_y, point.y);
        maximum_x = std::max<std::int64_t>(maximum_x, point.x);
        maximum_y = std::max<std::int64_t>(maximum_y, point.y);
    }
    const std::int64_t margin = static_cast<std::int64_t>(brush_radius) + 1;
    const std::int64_t left = std::max<std::int64_t>(0, minimum_x - margin);
    const std::int64_t top = std::max<std::int64_t>(0, minimum_y - margin);
    const std::int64_t right = std::min<std::int64_t>(
        bitmap.width,
        maximum_x + margin + 1);
    const std::int64_t bottom = std::min<std::int64_t>(
        bitmap.height,
        maximum_y + margin + 1);
    if (left >= right || top >= bottom) {
        return std::nullopt;
    }
    return RectI{
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(right),
        static_cast<int>(bottom),
    };
}

[[nodiscard]] std::size_t checked_area(const RectI& bounds) noexcept {
    if (bounds.empty()) {
        return 0;
    }
    const std::size_t width = static_cast<std::size_t>(bounds.width());
    const std::size_t height = static_cast<std::size_t>(bounds.height());
    if (height > std::numeric_limits<std::size_t>::max() / width) {
        return std::numeric_limits<std::size_t>::max();
    }
    return width * height;
}

[[nodiscard]] std::size_t privacy_raster_work(
    const RectI& mask_bounds,
    std::span<const POINT> points,
    int brush_radius,
    std::size_t limit) noexcept {
    if (points.empty()) {
        return 0;
    }
    const std::int64_t margin = static_cast<std::int64_t>(brush_radius) + 1;
    std::size_t work = 0;
    const std::size_t segment_count = std::max<std::size_t>(1, points.size() - 1);
    for (std::size_t index = 0; index < segment_count; ++index) {
        const POINT first = points[index];
        const POINT second = points.size() == 1 ? first : points[index + 1];
        const std::int64_t left = std::max<std::int64_t>(
            mask_bounds.left,
            std::min<std::int64_t>(first.x, second.x) - margin);
        const std::int64_t top = std::max<std::int64_t>(
            mask_bounds.top,
            std::min<std::int64_t>(first.y, second.y) - margin);
        const std::int64_t right = std::min<std::int64_t>(
            mask_bounds.right,
            std::max<std::int64_t>(first.x, second.x) + margin + 1);
        const std::int64_t bottom = std::min<std::int64_t>(
            mask_bounds.bottom,
            std::max<std::int64_t>(first.y, second.y) + margin + 1);
        if (left >= right || top >= bottom) {
            continue;
        }
        const std::uint64_t area =
            static_cast<std::uint64_t>(right - left) *
            static_cast<std::uint64_t>(bottom - top);
        if (area > limit || work > limit - static_cast<std::size_t>(area)) {
            return limit + 1;
        }
        work += static_cast<std::size_t>(area);
    }
    return work;
}

void rasterize_privacy_segment(
    PrivacyCoverageMask& mask,
    POINT first,
    POINT second,
    int brush_radius) noexcept {
    const double delta_x = static_cast<double>(second.x) - first.x;
    const double delta_y = static_cast<double>(second.y) - first.y;
    const double length_squared = delta_x * delta_x + delta_y * delta_y;
    const double inner_radius = std::max(0.0, static_cast<double>(brush_radius) - 0.5);
    const double outer_radius = static_cast<double>(brush_radius) + 0.5;
    const double inner_squared = inner_radius * inner_radius;
    const double outer_squared = outer_radius * outer_radius;
    const std::int64_t margin = static_cast<std::int64_t>(brush_radius) + 1;
    const int left = static_cast<int>(std::max<std::int64_t>(
        mask.bounds.left,
        std::min<std::int64_t>(first.x, second.x) - margin));
    const int top = static_cast<int>(std::max<std::int64_t>(
        mask.bounds.top,
        std::min<std::int64_t>(first.y, second.y) - margin));
    const int right = static_cast<int>(std::min<std::int64_t>(
        mask.bounds.right,
        std::max<std::int64_t>(first.x, second.x) + margin + 1));
    const int bottom = static_cast<int>(std::min<std::int64_t>(
        mask.bounds.bottom,
        std::max<std::int64_t>(first.y, second.y) + margin + 1));

    for (int y = top; y < bottom; ++y) {
        auto coverage_row = mask.row(y);
        for (int x = left; x < right; ++x) {
            double nearest_x = first.x;
            double nearest_y = first.y;
            if (length_squared > 0.0) {
                const double projection = std::clamp(
                    ((static_cast<double>(x) - first.x) * delta_x +
                     (static_cast<double>(y) - first.y) * delta_y) /
                        length_squared,
                    0.0,
                    1.0);
                nearest_x += projection * delta_x;
                nearest_y += projection * delta_y;
            }
            const double distance_x = static_cast<double>(x) - nearest_x;
            const double distance_y = static_cast<double>(y) - nearest_y;
            const double distance_squared =
                distance_x * distance_x + distance_y * distance_y;
            std::uint8_t coverage = 0;
            if (distance_squared <= inner_squared) {
                coverage = 255;
            } else if (distance_squared < outer_squared) {
                const double distance = std::sqrt(distance_squared);
                coverage = static_cast<std::uint8_t>(std::clamp(
                    std::lround((outer_radius - distance) * 255.0),
                    0L,
                    255L));
            }
            auto& destination = coverage_row[static_cast<std::size_t>(x - mask.bounds.left)];
            destination = std::max(destination, coverage);
        }
    }
}

[[nodiscard]] std::uint8_t blend_byte(
    std::uint8_t destination,
    std::uint8_t source,
    unsigned int alpha) noexcept {
    const unsigned int inverse = 255U - alpha;
    return static_cast<std::uint8_t>(
        (static_cast<unsigned int>(destination) * inverse +
         static_cast<unsigned int>(source) * alpha + 127U) /
        255U);
}

void blend_privacy_pixel(
    std::uint8_t* destination,
    const std::array<std::uint8_t, 3>& source,
    unsigned int alpha) noexcept {
    if (alpha == 0U) {
        return;
    }
    for (std::size_t channel = 0; channel < source.size(); ++channel) {
        destination[channel] = blend_byte(destination[channel], source[channel], alpha);
    }
    destination[3] = blend_byte(destination[3], 255U, alpha);
}

void apply_solid_privacy(
    Bitmap& bitmap,
    const PrivacyCoverageMask& mask,
    COLORREF color,
    int opacity) noexcept {
    const std::array<std::uint8_t, 3> source{
        GetBValue(color),
        GetGValue(color),
        GetRValue(color),
    };
    for (int y = mask.bounds.top; y < mask.bounds.bottom; ++y) {
        auto destination_row = bitmap.row(y);
        const auto coverage_row = mask.row(y);
        for (int x = mask.bounds.left; x < mask.bounds.right; ++x) {
            const unsigned int coverage =
                coverage_row[static_cast<std::size_t>(x - mask.bounds.left)];
            // Opaque redaction must not retain source-colour information in an
            // antialiased edge pixel. Lower opacities intentionally blend.
            const unsigned int alpha = opacity == 255 && coverage != 0U
                                           ? 255U
                                           : (coverage * static_cast<unsigned int>(opacity) +
                                              127U) /
                                                 255U;
            auto* destination = destination_row.data() +
                                static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            blend_privacy_pixel(destination, source, alpha);
        }
    }
}

void apply_mosaic_privacy(
    Bitmap& bitmap,
    const PrivacyCoverageMask& mask,
    int block_size) noexcept {
    const int first_block_x = (mask.bounds.left / block_size) * block_size;
    const int first_block_y = (mask.bounds.top / block_size) * block_size;
    for (std::int64_t block_y = first_block_y;
         block_y < mask.bounds.bottom;
         block_y += block_size) {
        const int source_top = static_cast<int>(block_y);
        const int source_bottom = static_cast<int>(
            std::min<std::int64_t>(bitmap.height, block_y + block_size));
        for (std::int64_t block_x = first_block_x;
             block_x < mask.bounds.right;
             block_x += block_size) {
            const int source_left = static_cast<int>(block_x);
            const int source_right = static_cast<int>(
                std::min<std::int64_t>(bitmap.width, block_x + block_size));
            std::array<std::uint64_t, 3> sums{};
            std::uint64_t count = 0;
            for (int y = source_top; y < source_bottom; ++y) {
                const auto source_row = bitmap.row(y);
                for (int x = source_left; x < source_right; ++x) {
                    const auto* source = source_row.data() +
                                         static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                        sums[channel] += source[channel];
                    }
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const std::array<std::uint8_t, 3> source{
                static_cast<std::uint8_t>(sums[0] / count),
                static_cast<std::uint8_t>(sums[1] / count),
                static_cast<std::uint8_t>(sums[2] / count),
            };
            const int apply_left = std::max(mask.bounds.left, source_left);
            const int apply_top = std::max(mask.bounds.top, source_top);
            const int apply_right = std::min(mask.bounds.right, source_right);
            const int apply_bottom = std::min(mask.bounds.bottom, source_bottom);
            for (int y = apply_top; y < apply_bottom; ++y) {
                auto destination_row = bitmap.row(y);
                const auto coverage_row = mask.row(y);
                for (int x = apply_left; x < apply_right; ++x) {
                    const unsigned int coverage =
                        coverage_row[static_cast<std::size_t>(x - mask.bounds.left)];
                    auto* destination = destination_row.data() +
                                        static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    blend_privacy_pixel(destination, source, coverage);
                }
            }
        }
    }
}

[[nodiscard]] bool apply_blur_privacy(
    Bitmap& bitmap,
    const PrivacyCoverageMask& mask,
    int radius,
    std::size_t scratch_pixel_limit) {
    const int sample_top = std::max(0, mask.bounds.top - radius);
    const int sample_bottom = std::min(bitmap.height, mask.bounds.bottom + radius);
    const int output_width = mask.bounds.width();
    const int sample_height = sample_bottom - sample_top;
    const std::size_t scratch_pixels =
        static_cast<std::size_t>(output_width) * static_cast<std::size_t>(sample_height);
    if (scratch_pixels > scratch_pixel_limit ||
        scratch_pixels > std::numeric_limits<std::size_t>::max() / 3U) {
        return false;
    }
    std::vector<std::uint8_t> horizontal(scratch_pixels * 3U);

    for (int y = sample_top; y < sample_bottom; ++y) {
        const auto source_row = bitmap.row(y);
        std::array<std::uint64_t, 3> sums{};
        int source_left = std::max(0, mask.bounds.left - radius);
        int source_right = std::min(bitmap.width - 1, mask.bounds.left + radius);
        for (int x = source_left; x <= source_right; ++x) {
            const auto* source = source_row.data() +
                                 static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                sums[channel] += source[channel];
            }
        }
        int count = source_right - source_left + 1;
        for (int x = mask.bounds.left; x < mask.bounds.right; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y - sample_top) *
                     static_cast<std::size_t>(output_width) +
                 static_cast<std::size_t>(x - mask.bounds.left)) *
                3U;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                horizontal[offset + channel] = static_cast<std::uint8_t>(
                    sums[channel] / static_cast<std::uint64_t>(count));
            }
            const int remove_x = x - radius;
            if (remove_x >= 0) {
                const auto* source = source_row.data() +
                                     static_cast<std::size_t>(remove_x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] -= source[channel];
                }
                --count;
            }
            const int add_x = x + radius + 1;
            if (add_x < bitmap.width) {
                const auto* source = source_row.data() +
                                     static_cast<std::size_t>(add_x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] += source[channel];
                }
                ++count;
            }
        }
    }

    for (int x = mask.bounds.left; x < mask.bounds.right; ++x) {
        std::array<std::uint64_t, 3> sums{};
        int source_top = std::max(sample_top, mask.bounds.top - radius);
        int source_bottom = std::min(sample_bottom - 1, mask.bounds.top + radius);
        for (int y = source_top; y <= source_bottom; ++y) {
            const std::size_t offset =
                (static_cast<std::size_t>(y - sample_top) *
                     static_cast<std::size_t>(output_width) +
                 static_cast<std::size_t>(x - mask.bounds.left)) *
                3U;
            for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                sums[channel] += horizontal[offset + channel];
            }
        }
        int count = source_bottom - source_top + 1;
        for (int y = mask.bounds.top; y < mask.bounds.bottom; ++y) {
            const std::array<std::uint8_t, 3> source{
                static_cast<std::uint8_t>(sums[0] / static_cast<std::uint64_t>(count)),
                static_cast<std::uint8_t>(sums[1] / static_cast<std::uint64_t>(count)),
                static_cast<std::uint8_t>(sums[2] / static_cast<std::uint64_t>(count)),
            };
            const std::size_t mask_offset =
                static_cast<std::size_t>(y - mask.bounds.top) *
                    static_cast<std::size_t>(output_width) +
                static_cast<std::size_t>(x - mask.bounds.left);
            const unsigned int coverage = mask.coverage[mask_offset];
            auto* destination = bitmap.pixels.data() +
                                static_cast<std::size_t>(y) * bitmap.stride_bytes() +
                                static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            blend_privacy_pixel(destination, source, coverage);

            const int remove_y = y - radius;
            if (remove_y >= sample_top) {
                const std::size_t offset =
                    (static_cast<std::size_t>(remove_y - sample_top) *
                         static_cast<std::size_t>(output_width) +
                     static_cast<std::size_t>(x - mask.bounds.left)) *
                    3U;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] -= horizontal[offset + channel];
                }
                --count;
            }
            const int add_y = y + radius + 1;
            if (add_y < sample_bottom) {
                const std::size_t offset =
                    (static_cast<std::size_t>(add_y - sample_top) *
                         static_cast<std::size_t>(output_width) +
                     static_cast<std::size_t>(x - mask.bounds.left)) *
                    3U;
                for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                    sums[channel] += horizontal[offset + channel];
                }
                ++count;
            }
        }
    }
    return true;
}

}  // namespace

std::optional<std::size_t> Bitmap::checked_byte_size(int bitmap_width, int bitmap_height) noexcept {
    if (bitmap_width <= 0 || bitmap_height <= 0 ||
        bitmap_width > std::numeric_limits<int>::max() / static_cast<int>(bytes_per_pixel)) {
        return std::nullopt;
    }

    const std::size_t row_size = static_cast<std::size_t>(bitmap_width) * bytes_per_pixel;
    const std::size_t unsigned_height = static_cast<std::size_t>(bitmap_height);
    if (unsigned_height > std::numeric_limits<std::size_t>::max() / row_size) {
        return std::nullopt;
    }
    const std::size_t byte_size = row_size * unsigned_height;
    if (byte_size > std::numeric_limits<UINT>::max()) {
        return std::nullopt;
    }
    return byte_size;
}

Bitmap::Bitmap(int bitmap_width, int bitmap_height) {
    const auto byte_size = checked_byte_size(bitmap_width, bitmap_height);
    if (!byte_size || *byte_size > pixels.max_size()) {
        return;
    }
    try {
        pixels.resize(*byte_size);
    } catch (const std::bad_alloc&) {
        pixels.clear();
        return;
    } catch (const std::length_error&) {
        pixels.clear();
        return;
    }
    width = bitmap_width;
    height = bitmap_height;
    make_opaque();
}

bool Bitmap::valid() const noexcept {
    const auto byte_size = checked_byte_size(width, height);
    return byte_size && pixels.size() == *byte_size;
}

bool Bitmap::empty() const noexcept {
    return !valid();
}

int Bitmap::stride() const noexcept {
    return valid() ? width * static_cast<int>(bytes_per_pixel) : 0;
}

std::size_t Bitmap::stride_bytes() const noexcept {
    return valid() ? static_cast<std::size_t>(width) * bytes_per_pixel : 0;
}

std::span<std::uint8_t> Bitmap::row(int y) {
    if (!valid() || y < 0 || y >= height) {
        return {};
    }
    const std::size_t row_size = stride_bytes();
    return std::span(pixels).subspan(static_cast<std::size_t>(y) * row_size, row_size);
}

std::span<const std::uint8_t> Bitmap::row(int y) const {
    if (!valid() || y < 0 || y >= height) {
        return {};
    }
    const std::size_t row_size = stride_bytes();
    return std::span(pixels).subspan(static_cast<std::size_t>(y) * row_size, row_size);
}

void Bitmap::make_opaque() noexcept {
    if (!valid()) {
        return;
    }
    for (std::size_t index = 3; index < pixels.size(); index += bytes_per_pixel) {
        pixels[index] = 255;
    }
}

void composite_onto_background(Bitmap& bitmap,
                               std::uint8_t red,
                               std::uint8_t green,
                               std::uint8_t blue) noexcept {
    if (!bitmap.valid()) {
        return;
    }
    const std::array<std::uint8_t, 3> background{blue, green, red};
    for (std::size_t offset = 0;
         offset < bitmap.pixels.size();
         offset += Bitmap::bytes_per_pixel) {
        const unsigned int alpha = bitmap.pixels[offset + 3];
        const unsigned int inverse = 255U - alpha;
        for (std::size_t channel = 0; channel < background.size(); ++channel) {
            bitmap.pixels[offset + channel] = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(bitmap.pixels[offset + channel]) *
                     alpha +
                 static_cast<unsigned int>(background[channel]) * inverse +
                 127U) /
                255U);
        }
        bitmap.pixels[offset + 3] = 255U;
    }
}

void apply_rounded_corner_mask(Bitmap& bitmap, int radius) noexcept {
    if (!bitmap.valid() || radius <= 0) {
        return;
    }

    const int effective_radius =
        std::min({radius, bitmap.width / 2, bitmap.height / 2});
    if (effective_radius <= 0) {
        return;
    }

    const double corner_center = static_cast<double>(effective_radius);
    const double antialias_outer = corner_center + 0.5;
    const double antialias_inner = corner_center - 0.5;
    for (int y = 0; y < effective_radius; ++y) {
        const double dy = corner_center - (static_cast<double>(y) + 0.5);
        for (int x = 0; x < effective_radius; ++x) {
            const double dx = corner_center - (static_cast<double>(x) + 0.5);
            const double distance = std::sqrt(dx * dx + dy * dy);
            std::uint8_t mask = 255;
            if (distance >= antialias_outer) {
                mask = 0;
            } else if (distance > antialias_inner) {
                const double coverage = antialias_outer - distance;
                mask = static_cast<std::uint8_t>(
                    std::clamp(std::lround(coverage * 255.0), 0L, 255L));
            }
            if (mask == 255) {
                continue;
            }

            const std::array<POINT, 4> points{
                POINT{x, y},
                POINT{bitmap.width - 1 - x, y},
                POINT{x, bitmap.height - 1 - y},
                POINT{bitmap.width - 1 - x, bitmap.height - 1 - y},
            };
            for (const POINT point : points) {
                auto row = bitmap.row(point.y);
                auto& alpha = row[static_cast<std::size_t>(point.x) *
                                      Bitmap::bytes_per_pixel +
                                  3];
                alpha = static_cast<std::uint8_t>(
                    (static_cast<unsigned int>(alpha) * mask + 127U) / 255U);
            }
        }
    }
}

void blit(const Bitmap& source, const RectI& source_rect, Bitmap& target, POINT target_origin) {
    if (&source == &target || !source.valid() || !target.valid()) {
        return;
    }

    const WideRect requested = normalize_wide(source_rect);
    std::int64_t source_left = std::max<std::int64_t>(0, requested.left);
    std::int64_t source_top = std::max<std::int64_t>(0, requested.top);
    std::int64_t source_right = std::min<std::int64_t>(source.width, requested.right);
    std::int64_t source_bottom = std::min<std::int64_t>(source.height, requested.bottom);
    std::int64_t target_left = static_cast<std::int64_t>(target_origin.x) + source_left - requested.left;
    std::int64_t target_top = static_cast<std::int64_t>(target_origin.y) + source_top - requested.top;

    if (target_left < 0) {
        source_left -= target_left;
        target_left = 0;
    }
    if (target_top < 0) {
        source_top -= target_top;
        target_top = 0;
    }
    source_right = std::min(source_right, source_left + static_cast<std::int64_t>(target.width) - target_left);
    source_bottom = std::min(source_bottom, source_top + static_cast<std::int64_t>(target.height) - target_top);
    if (source_left >= source_right || source_top >= source_bottom ||
        target_left >= target.width || target_top >= target.height) {
        return;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(source_right - source_left);
    const std::size_t copy_bytes = pixel_count * Bitmap::bytes_per_pixel;
    const int row_count = static_cast<int>(source_bottom - source_top);
    for (int row_index = 0; row_index < row_count; ++row_index) {
        const int source_y = static_cast<int>(source_top) + row_index;
        const int target_y = static_cast<int>(target_top) + row_index;
        const auto source_row = source.row(source_y);
        auto target_row = target.row(target_y);
        const auto* source_data =
            source_row.data() + static_cast<std::size_t>(source_left) * Bitmap::bytes_per_pixel;
        auto* target_data =
            target_row.data() + static_cast<std::size_t>(target_left) * Bitmap::bytes_per_pixel;
        std::memcpy(target_data, source_data, copy_bytes);
        for (std::size_t x = 0; x < pixel_count; ++x) {
            target_data[x * Bitmap::bytes_per_pixel + 3] = 255;
        }
    }
}

Bitmap crop(const Bitmap& source, const RectI& rect) {
    if (!source.valid()) {
        return {};
    }
    const WideRect normalized = normalize_wide(rect);
    const std::int64_t width = normalized.right - normalized.left;
    const std::int64_t height = normalized.bottom - normalized.top;
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        return {};
    }

    Bitmap result(static_cast<int>(width), static_cast<int>(height));
    if (result.empty()) {
        return {};
    }
    const RectI normalized_rect{
        static_cast<int>(normalized.left),
        static_cast<int>(normalized.top),
        static_cast<int>(normalized.right),
        static_cast<int>(normalized.bottom),
    };
    blit(source, normalized_rect, result, POINT{0, 0});
    return result;
}

void pixelate_circle(Bitmap& bitmap, POINT center, int radius, int block_size) {
    if (!bitmap.valid() || radius <= 0 || block_size <= 1) {
        return;
    }
    const std::int64_t center_x = center.x;
    const std::int64_t center_y = center.y;
    const int min_x = static_cast<int>(std::max<std::int64_t>(0, center_x - radius));
    const int min_y = static_cast<int>(std::max<std::int64_t>(0, center_y - radius));
    const int max_x = static_cast<int>(
        std::min<std::int64_t>(bitmap.width, center_x + static_cast<std::int64_t>(radius) + 1));
    const int max_y = static_cast<int>(
        std::min<std::int64_t>(bitmap.height, center_y + static_cast<std::int64_t>(radius) + 1));
    if (min_x >= max_x || min_y >= max_y) {
        return;
    }
    const std::uint64_t radius_squared = static_cast<std::uint64_t>(radius) * static_cast<std::uint64_t>(radius);

    for (std::int64_t block_y = min_y; block_y < max_y; block_y += block_size) {
        for (std::int64_t block_x = min_x; block_x < max_x; block_x += block_size) {
            std::array<std::uint64_t, 3> sums{};
            std::uint64_t count = 0;
            const int end_y = static_cast<int>(
                std::min<std::int64_t>(max_y, block_y + static_cast<std::int64_t>(block_size)));
            const int end_x = static_cast<int>(
                std::min<std::int64_t>(max_x, block_x + static_cast<std::int64_t>(block_size)));
            for (int y = static_cast<int>(block_y); y < end_y; ++y) {
                for (int x = static_cast<int>(block_x); x < end_x; ++x) {
                    if (!inside_circle(x, y, center_x, center_y, radius_squared)) {
                        continue;
                    }
                    const auto row = bitmap.row(y);
                    const auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                        sums[channel] += pixel[channel];
                    }
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const std::array<std::uint8_t, 3> values{
                static_cast<std::uint8_t>(sums[0] / count),
                static_cast<std::uint8_t>(sums[1] / count),
                static_cast<std::uint8_t>(sums[2] / count),
            };
            for (int y = static_cast<int>(block_y); y < end_y; ++y) {
                for (int x = static_cast<int>(block_x); x < end_x; ++x) {
                    if (!inside_circle(x, y, center_x, center_y, radius_squared)) {
                        continue;
                    }
                    auto row = bitmap.row(y);
                    auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    std::copy(values.begin(), values.end(), pixel);
                    pixel[3] = 255;
                }
            }
        }
    }
}

void blur_circle(Bitmap& bitmap, POINT center, int radius, int blur_radius) {
    if (!bitmap.valid() || radius <= 0 || blur_radius <= 0) {
        return;
    }
    const std::int64_t center_x = center.x;
    const std::int64_t center_y = center.y;
    const int min_x = static_cast<int>(std::max<std::int64_t>(0, center_x - radius));
    const int min_y = static_cast<int>(std::max<std::int64_t>(0, center_y - radius));
    const int max_x = static_cast<int>(
        std::min<std::int64_t>(bitmap.width, center_x + static_cast<std::int64_t>(radius) + 1));
    const int max_y = static_cast<int>(
        std::min<std::int64_t>(bitmap.height, center_y + static_cast<std::int64_t>(radius) + 1));
    if (min_x >= max_x || min_y >= max_y) {
        return;
    }

    const RectI bounds{min_x, min_y, max_x, max_y};
    const Bitmap source = crop(bitmap, bounds);
    const Bitmap blurred = box_blur(source, blur_radius);
    if (blurred.empty()) {
        return;
    }

    const std::uint64_t radius_squared = static_cast<std::uint64_t>(radius) * static_cast<std::uint64_t>(radius);
    for (int y = min_y; y < max_y; ++y) {
        auto destination_row = bitmap.row(y);
        const auto source_row = blurred.row(y - min_y);
        for (int x = min_x; x < max_x; ++x) {
            if (!inside_circle(x, y, center_x, center_y, radius_squared)) {
                continue;
            }
            auto* destination =
                destination_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            const auto* source_pixel =
                source_row.data() + static_cast<std::size_t>(x - min_x) * Bitmap::bytes_per_pixel;
            std::copy_n(source_pixel, 3, destination);
            destination[3] = 255;
        }
    }
}

void pixelate_rect(Bitmap& bitmap, const RectI& rect, int block_size) {
    if (!bitmap.valid() || block_size <= 1) {
        return;
    }
    RectI clipped{};
    if (!clip_to_bitmap(bitmap, rect, clipped)) {
        return;
    }

    for (std::int64_t block_y = clipped.top; block_y < clipped.bottom; block_y += block_size) {
        for (std::int64_t block_x = clipped.left; block_x < clipped.right; block_x += block_size) {
            std::array<std::uint64_t, 3> sums{};
            std::uint64_t count = 0;
            const int end_y = static_cast<int>(
                std::min<std::int64_t>(clipped.bottom, block_y + static_cast<std::int64_t>(block_size)));
            const int end_x = static_cast<int>(
                std::min<std::int64_t>(clipped.right, block_x + static_cast<std::int64_t>(block_size)));
            for (int y = static_cast<int>(block_y); y < end_y; ++y) {
                const auto row = bitmap.row(y);
                for (int x = static_cast<int>(block_x); x < end_x; ++x) {
                    const auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    for (std::size_t channel = 0; channel < sums.size(); ++channel) {
                        sums[channel] += pixel[channel];
                    }
                    ++count;
                }
            }
            const std::array<std::uint8_t, 3> values{
                static_cast<std::uint8_t>(sums[0] / count),
                static_cast<std::uint8_t>(sums[1] / count),
                static_cast<std::uint8_t>(sums[2] / count),
            };
            for (int y = static_cast<int>(block_y); y < end_y; ++y) {
                auto row = bitmap.row(y);
                for (int x = static_cast<int>(block_x); x < end_x; ++x) {
                    auto* pixel = row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                    std::copy(values.begin(), values.end(), pixel);
                    pixel[3] = 255;
                }
            }
        }
    }
}

PrivacyEffectResult blur_rect(
    Bitmap& bitmap,
    const RectI& rect,
    int blur_radius,
    std::size_t maximum_temporary_pixels) noexcept {
    if (!bitmap.valid() || blur_radius <= 0) {
        return PrivacyEffectResult::invalid_argument;
    }
    RectI clipped{};
    if (!clip_to_bitmap(bitmap, rect, clipped)) {
        return PrivacyEffectResult::no_change;
    }

    const std::size_t clipped_width =
        static_cast<std::size_t>(clipped.width());
    const std::size_t clipped_height =
        static_cast<std::size_t>(clipped.height());
    if (maximum_temporary_pixels == 0 ||
        clipped_height > maximum_temporary_pixels / clipped_width) {
        return PrivacyEffectResult::resource_limit;
    }

    const Bitmap source = crop(bitmap, clipped);
    const Bitmap blurred = box_blur(source, blur_radius);
    if (blurred.empty()) {
        return PrivacyEffectResult::resource_limit;
    }

    for (int y = clipped.top; y < clipped.bottom; ++y) {
        auto destination_row = bitmap.row(y);
        const auto source_row = blurred.row(y - clipped.top);
        for (int x = clipped.left; x < clipped.right; ++x) {
            auto* destination =
                destination_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
            const auto* source_pixel =
                source_row.data() + static_cast<std::size_t>(x - clipped.left) * Bitmap::bytes_per_pixel;
            std::copy_n(source_pixel, 3, destination);
            destination[3] = 255;
        }
    }
    return PrivacyEffectResult::applied;
}

PrivacyEffectResult apply_privacy_stroke(
    Bitmap& bitmap,
    std::span<const POINT> points,
    int brush_radius,
    const PrivacyEffectOptions& options,
    const PrivacyMaskLimits& limits) noexcept {
    if (!bitmap.valid()) {
        return PrivacyEffectResult::invalid_argument;
    }
    if (points.empty() || options.strength == 0) {
        return PrivacyEffectResult::no_change;
    }
    if (options.strength < 0 || brush_radius <= 0 ||
        brush_radius > PrivacyMaskLimits::hard_max_brush_radius) {
        return PrivacyEffectResult::invalid_argument;
    }
    switch (options.mode) {
        case PrivacyEffectMode::mosaic:
            if (options.strength == 1) {
                return PrivacyEffectResult::no_change;
            }
            if (options.strength > PrivacyMaskLimits::hard_max_effect_strength) {
                return PrivacyEffectResult::invalid_argument;
            }
            break;
        case PrivacyEffectMode::blur:
            if (options.strength > PrivacyMaskLimits::hard_max_effect_strength) {
                return PrivacyEffectResult::invalid_argument;
            }
            break;
        case PrivacyEffectMode::solid:
            if (options.strength > 255) {
                return PrivacyEffectResult::invalid_argument;
            }
            break;
        default:
            return PrivacyEffectResult::invalid_argument;
    }

    const std::size_t input_limit = std::min(
        limits.max_input_points,
        PrivacyMaskLimits::hard_max_input_points);
    const std::size_t raster_point_limit = std::min(
        limits.max_raster_points,
        PrivacyMaskLimits::hard_max_raster_points);
    const std::size_t mask_pixel_limit = std::min(
        limits.max_mask_pixels,
        PrivacyMaskLimits::hard_max_mask_pixels);
    const std::size_t raster_work_limit = std::min(
        limits.max_raster_work,
        PrivacyMaskLimits::hard_max_raster_work);
    if (input_limit == 0 || raster_point_limit == 0 ||
        mask_pixel_limit == 0 || raster_work_limit == 0 ||
        points.size() > input_limit) {
        return PrivacyEffectResult::resource_limit;
    }

    try {
        std::vector<POINT> raster_points = compact_collinear_points(points);
        if (raster_points.empty()) {
            return PrivacyEffectResult::no_change;
        }
        if (raster_points.size() > raster_point_limit) {
            if (raster_point_limit < 2) {
                return PrivacyEffectResult::resource_limit;
            }
            raster_points = resample_privacy_points(raster_points, raster_point_limit);
        }
        const auto bounds = privacy_mask_bounds(bitmap, raster_points, brush_radius);
        if (!bounds) {
            return PrivacyEffectResult::no_change;
        }
        const std::size_t mask_pixels = checked_area(*bounds);
        if (mask_pixels == 0 || mask_pixels > mask_pixel_limit) {
            return PrivacyEffectResult::resource_limit;
        }
        if (privacy_raster_work(
                *bounds,
                raster_points,
                brush_radius,
                raster_work_limit) > raster_work_limit) {
            return PrivacyEffectResult::resource_limit;
        }

        PrivacyCoverageMask mask{*bounds, std::vector<std::uint8_t>(mask_pixels, 0)};
        if (raster_points.size() == 1) {
            rasterize_privacy_segment(
                mask,
                raster_points.front(),
                raster_points.front(),
                brush_radius);
        } else {
            for (std::size_t index = 1; index < raster_points.size(); ++index) {
                rasterize_privacy_segment(
                    mask,
                    raster_points[index - 1],
                    raster_points[index],
                    brush_radius);
            }
        }
        if (std::none_of(mask.coverage.begin(), mask.coverage.end(), [](std::uint8_t value) {
                return value != 0;
            })) {
            return PrivacyEffectResult::no_change;
        }

        switch (options.mode) {
            case PrivacyEffectMode::mosaic:
                apply_mosaic_privacy(bitmap, mask, options.strength);
                break;
            case PrivacyEffectMode::blur:
                if (!apply_blur_privacy(
                        bitmap,
                        mask,
                        options.strength,
                        mask_pixel_limit)) {
                    return PrivacyEffectResult::resource_limit;
                }
                break;
            case PrivacyEffectMode::solid:
                apply_solid_privacy(
                    bitmap,
                    mask,
                    options.solid_color,
                    options.strength);
                break;
        }
        return PrivacyEffectResult::applied;
    } catch (const std::bad_alloc&) {
        return PrivacyEffectResult::resource_limit;
    } catch (const std::length_error&) {
        return PrivacyEffectResult::resource_limit;
    }
}

Bitmap rotate_90_cw(const Bitmap& source) {
    if (!source.valid()) {
        return {};
    }
    Bitmap result(source.height, source.width);
    if (result.empty()) {
        return {};
    }
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        for (int x = 0; x < source.width; ++x) {
            const int destination_x = source.height - 1 - y;
            const int destination_y = x;
            auto destination_row = result.row(destination_y);
            auto* destination =
                destination_row.data() + static_cast<std::size_t>(destination_x) * Bitmap::bytes_per_pixel;
            std::memcpy(destination,
                        source_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel,
                        Bitmap::bytes_per_pixel);
            destination[3] = 255;
        }
    }
    return result;
}

Bitmap rotate_90_ccw(const Bitmap& source) {
    if (!source.valid()) {
        return {};
    }
    Bitmap result(source.height, source.width);
    if (result.empty()) {
        return {};
    }
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        for (int x = 0; x < source.width; ++x) {
            const int destination_x = y;
            const int destination_y = source.width - 1 - x;
            auto destination_row = result.row(destination_y);
            auto* destination =
                destination_row.data() + static_cast<std::size_t>(destination_x) * Bitmap::bytes_per_pixel;
            std::memcpy(destination,
                        source_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel,
                        Bitmap::bytes_per_pixel);
            destination[3] = 255;
        }
    }
    return result;
}

Bitmap flip_horizontal(const Bitmap& source) {
    if (!source.valid()) {
        return {};
    }
    Bitmap result(source.width, source.height);
    if (result.empty()) {
        return {};
    }
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        auto destination_row = result.row(y);
        for (int x = 0; x < source.width; ++x) {
            auto* destination = destination_row.data() +
                                static_cast<std::size_t>(source.width - 1 - x) * Bitmap::bytes_per_pixel;
            std::memcpy(destination,
                        source_row.data() + static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel,
                        Bitmap::bytes_per_pixel);
            destination[3] = 255;
        }
    }
    return result;
}

Bitmap flip_vertical(const Bitmap& source) {
    if (!source.valid()) {
        return {};
    }
    Bitmap result(source.width, source.height);
    if (result.empty()) {
        return {};
    }
    for (int y = 0; y < source.height; ++y) {
        const auto source_row = source.row(y);
        auto destination_row = result.row(source.height - 1 - y);
        std::memcpy(destination_row.data(), source_row.data(), source.stride_bytes());
        for (std::size_t x = 0; x < static_cast<std::size_t>(source.width); ++x) {
            destination_row[x * Bitmap::bytes_per_pixel + 3] = 255;
        }
    }
    return result;
}

}  // namespace airshot
