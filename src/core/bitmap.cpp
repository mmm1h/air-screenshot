#include "airshot/bitmap.h"

#include <cstring>

namespace airshot {

Bitmap::Bitmap(int bitmap_width, int bitmap_height)
    : width(bitmap_width),
      height(bitmap_height),
      pixels(static_cast<std::size_t>(std::max(0, bitmap_width)) *
             static_cast<std::size_t>(std::max(0, bitmap_height)) * 4U) {}

bool Bitmap::empty() const noexcept {
    return width <= 0 || height <= 0 || pixels.empty();
}

int Bitmap::stride() const noexcept {
    return width * 4;
}

std::span<std::uint8_t> Bitmap::row(int y) {
    return std::span(pixels).subspan(static_cast<std::size_t>(y * stride()), static_cast<std::size_t>(stride()));
}

std::span<const std::uint8_t> Bitmap::row(int y) const {
    return std::span(pixels).subspan(static_cast<std::size_t>(y * stride()), static_cast<std::size_t>(stride()));
}

void blit(const Bitmap& source, const RectI& source_rect, Bitmap& target, POINT target_origin) {
    const RectI normalized = source_rect.normalized();
    if (source.empty() || target.empty() || normalized.empty()) {
        return;
    }
    for (int y = 0; y < normalized.height(); ++y) {
        const int source_y = normalized.top + y;
        const int target_y = target_origin.y + y;
        if (source_y < 0 || source_y >= source.height || target_y < 0 || target_y >= target.height) {
            continue;
        }
        int source_x = normalized.left;
        int target_x = target_origin.x;
        int count = normalized.width();
        if (source_x < 0) {
            target_x -= source_x;
            count += source_x;
            source_x = 0;
        }
        if (target_x < 0) {
            source_x -= target_x;
            count += target_x;
            target_x = 0;
        }
        count = std::min(count, source.width - source_x);
        count = std::min(count, target.width - target_x);
        if (count <= 0) {
            continue;
        }
        const auto* source_data = source.row(source_y).data() + source_x * 4;
        auto* target_data = target.row(target_y).data() + target_x * 4;
        std::memcpy(target_data, source_data, static_cast<std::size_t>(count) * 4U);
    }
}

Bitmap crop(const Bitmap& source, const RectI& rect) {
    const RectI normalized = rect.normalized();
    Bitmap result(normalized.width(), normalized.height());
    blit(source, normalized, result, POINT{0, 0});
    return result;
}

void pixelate_circle(Bitmap& bitmap, POINT center, int radius, int block_size) {
    if (bitmap.empty() || radius <= 0 || block_size <= 1) {
        return;
    }
    const int center_x = static_cast<int>(center.x);
    const int center_y = static_cast<int>(center.y);
    const int min_x = std::max(0, center_x - radius);
    const int min_y = std::max(0, center_y - radius);
    const int max_x = std::min(bitmap.width, center_x + radius + 1);
    const int max_y = std::min(bitmap.height, center_y + radius + 1);
    const int radius_squared = radius * radius;

    for (int block_y = min_y; block_y < max_y; block_y += block_size) {
        for (int block_x = min_x; block_x < max_x; block_x += block_size) {
            std::uint64_t blue = 0;
            std::uint64_t green = 0;
            std::uint64_t red = 0;
            std::uint64_t alpha = 0;
            std::uint64_t count = 0;
            const int end_y = std::min(max_y, block_y + block_size);
            const int end_x = std::min(max_x, block_x + block_size);
            for (int y = block_y; y < end_y; ++y) {
                for (int x = block_x; x < end_x; ++x) {
                    const int dx = x - center_x;
                    const int dy = y - center_y;
                    if (dx * dx + dy * dy > radius_squared) {
                        continue;
                    }
                    const auto* pixel = bitmap.row(y).data() + x * 4;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                    alpha += pixel[3];
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const std::uint8_t values[4] = {
                static_cast<std::uint8_t>(blue / count),
                static_cast<std::uint8_t>(green / count),
                static_cast<std::uint8_t>(red / count),
                static_cast<std::uint8_t>(alpha / count),
            };
            for (int y = block_y; y < end_y; ++y) {
                for (int x = block_x; x < end_x; ++x) {
                    const int dx = x - center_x;
                    const int dy = y - center_y;
                    if (dx * dx + dy * dy > radius_squared) {
                        continue;
                    }
                    auto* pixel = bitmap.row(y).data() + x * 4;
                    std::copy(std::begin(values), std::end(values), pixel);
                }
            }
        }
    }
}

Bitmap rotate_90_cw(const Bitmap& source) {
    if (source.empty()) {
        return {};
    }
    Bitmap result(source.height, source.width);
    for (int y = 0; y < source.height; ++y) {
        const auto* src_row = source.row(y).data();
        for (int x = 0; x < source.width; ++x) {
            const int dest_x = source.height - 1 - y;
            const int dest_y = x;
            auto* dest_pixel = result.row(dest_y).data() + dest_x * 4;
            std::memcpy(dest_pixel, src_row + x * 4, 4);
        }
    }
    return result;
}

Bitmap rotate_90_ccw(const Bitmap& source) {
    if (source.empty()) {
        return {};
    }
    Bitmap result(source.height, source.width);
    for (int y = 0; y < source.height; ++y) {
        const auto* src_row = source.row(y).data();
        for (int x = 0; x < source.width; ++x) {
            const int dest_x = y;
            const int dest_y = source.width - 1 - x;
            auto* dest_pixel = result.row(dest_y).data() + dest_x * 4;
            std::memcpy(dest_pixel, src_row + x * 4, 4);
        }
    }
    return result;
}

Bitmap flip_horizontal(const Bitmap& source) {
    if (source.empty()) {
        return {};
    }
    Bitmap result(source.width, source.height);
    for (int y = 0; y < source.height; ++y) {
        const auto* src_row = source.row(y).data();
        auto* dest_row = result.row(y).data();
        for (int x = 0; x < source.width; ++x) {
            std::memcpy(dest_row + (source.width - 1 - x) * 4, src_row + x * 4, 4);
        }
    }
    return result;
}

Bitmap flip_vertical(const Bitmap& source) {
    if (source.empty()) {
        return {};
    }
    Bitmap result(source.width, source.height);
    for (int y = 0; y < source.height; ++y) {
        const auto* src_row = source.row(y).data();
        auto* dest_row = result.row(source.height - 1 - y).data();
        std::memcpy(dest_row, src_row, static_cast<std::size_t>(source.width) * 4U);
    }
    return result;
}

void blur_circle(Bitmap& bitmap, POINT center, int radius, int blur_radius) {
    if (bitmap.empty() || radius <= 0 || blur_radius <= 0) return;
    const int cx = static_cast<int>(center.x);
    const int cy = static_cast<int>(center.y);
    const int min_x = std::max(0, cx - radius);
    const int min_y = std::max(0, cy - radius);
    const int max_x = std::min(bitmap.width, cx + radius + 1);
    const int max_y = std::min(bitmap.height, cy + radius + 1);
    if (min_x >= max_x || min_y >= max_y) return;
    const int r_sq = radius * radius;

    Bitmap temp = crop(bitmap, RectI{min_x, min_y, max_x, max_y});
    if (temp.empty()) return;

    for (int y = min_y; y < max_y; ++y) {
        auto* dest_row = bitmap.row(y).data();
        for (int x = min_x; x < max_x; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= r_sq) {
                int sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
                for (int wy = -blur_radius; wy <= blur_radius; ++wy) {
                    const int py = y + wy;
                    if (py < min_y || py >= max_y) continue;
                    const auto* temp_row = temp.row(py - min_y).data();
                    for (int wx = -blur_radius; wx <= blur_radius; ++wx) {
                        const int px = x + wx;
                        if (px < min_x || px >= max_x) continue;
                        const auto* pixel = temp_row + (px - min_x) * 4;
                        sum_b += pixel[0];
                        sum_g += pixel[1];
                        sum_r += pixel[2];
                        count++;
                    }
                }
                if (count > 0) {
                    dest_row[x * 4] = static_cast<uint8_t>(sum_b / count);
                    dest_row[x * 4 + 1] = static_cast<uint8_t>(sum_g / count);
                    dest_row[x * 4 + 2] = static_cast<uint8_t>(sum_r / count);
                }
            }
        }
    }
}

}  // namespace airshot
