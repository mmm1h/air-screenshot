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

}  // namespace airshot
