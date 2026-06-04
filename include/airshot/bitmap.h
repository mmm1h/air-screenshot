#pragma once

#include "airshot/common.h"

namespace airshot {

struct Bitmap {
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;

    Bitmap() = default;
    Bitmap(int bitmap_width, int bitmap_height);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int stride() const noexcept;
    [[nodiscard]] std::span<std::uint8_t> row(int y);
    [[nodiscard]] std::span<const std::uint8_t> row(int y) const;
};

void blit(const Bitmap& source, const RectI& source_rect, Bitmap& target, POINT target_origin);
[[nodiscard]] Bitmap crop(const Bitmap& source, const RectI& rect);
void pixelate_circle(Bitmap& bitmap, POINT center, int radius, int block_size);

}  // namespace airshot
