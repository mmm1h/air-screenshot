#pragma once

#include "airshot/common.h"

namespace airshot {

struct Bitmap {
    static constexpr std::size_t bytes_per_pixel = 4;

    // Transitional compatibility fields. New code should prefer the accessors
    // below. Pixels are top-down straight-alpha BGRA8. Desktop capture paths
    // normally produce opaque pixels; explicit output effects may introduce
    // transparency.
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;

    Bitmap() = default;
    Bitmap(int bitmap_width, int bitmap_height);

    [[nodiscard]] static std::optional<std::size_t> checked_byte_size(int bitmap_width,
                                                                      int bitmap_height) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int stride() const noexcept;
    [[nodiscard]] std::size_t stride_bytes() const noexcept;
    [[nodiscard]] std::span<std::uint8_t> row(int y);
    [[nodiscard]] std::span<const std::uint8_t> row(int y) const;
    void make_opaque() noexcept;
};

// Composites straight-alpha BGRA pixels over an opaque RGB background. This
// is intended for legacy targets such as CF_DIB/CF_BITMAP that cannot carry
// per-pixel alpha without producing black or stale-colour corners.
void composite_onto_background(Bitmap& bitmap,
                               std::uint8_t red,
                               std::uint8_t green,
                               std::uint8_t blue) noexcept;

// Source and target aliasing is rejected; target is left unchanged.
void blit(const Bitmap& source, const RectI& source_rect, Bitmap& target, POINT target_origin);
[[nodiscard]] Bitmap crop(const Bitmap& source, const RectI& rect);
void pixelate_circle(Bitmap& bitmap, POINT center, int radius, int block_size);
void blur_circle(Bitmap& bitmap, POINT center, int radius, int blur_radius);
void pixelate_rect(Bitmap& bitmap, const RectI& rect, int block_size);
void blur_rect(Bitmap& bitmap, const RectI& rect, int blur_radius);
// Applies an antialiased rounded-rectangle alpha mask in-place. The radius is
// measured in physical pixels and clamped to half of the shortest edge. A
// non-positive radius leaves the bitmap unchanged.
void apply_rounded_corner_mask(Bitmap& bitmap, int radius) noexcept;
[[nodiscard]] Bitmap rotate_90_cw(const Bitmap& source);
[[nodiscard]] Bitmap rotate_90_ccw(const Bitmap& source);
[[nodiscard]] Bitmap flip_horizontal(const Bitmap& source);
[[nodiscard]] Bitmap flip_vertical(const Bitmap& source);

}  // namespace airshot
