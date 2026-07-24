#include "airshot/bitmap.h"

#include <array>
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

void blur_rect(Bitmap& bitmap, const RectI& rect, int blur_radius) {
    if (!bitmap.valid() || blur_radius <= 0) {
        return;
    }
    RectI clipped{};
    if (!clip_to_bitmap(bitmap, rect, clipped)) {
        return;
    }

    const Bitmap source = crop(bitmap, clipped);
    const Bitmap blurred = box_blur(source, blur_radius);
    if (blurred.empty()) {
        return;
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
