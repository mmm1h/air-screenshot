#pragma once

#include "airshot/common.h"

#include <limits>

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

enum class PrivacyEffectResult {
    applied,
    no_change,
    invalid_argument,
    resource_limit,
};

void pixelate_circle(Bitmap& bitmap, POINT center, int radius, int block_size);
void blur_circle(Bitmap& bitmap, POINT center, int radius, int blur_radius);
void pixelate_rect(Bitmap& bitmap, const RectI& rect, int block_size);
// The bitmap is not changed unless the complete blurred rectangle can be
// prepared. `maximum_temporary_pixels` lets tests and constrained callers
// impose a tighter allocation budget without weakening the default path.
[[nodiscard]] PrivacyEffectResult blur_rect(
    Bitmap& bitmap,
    const RectI& rect,
    int blur_radius,
    std::size_t maximum_temporary_pixels =
        std::numeric_limits<std::size_t>::max()) noexcept;

// Privacy brush strokes are rasterized into one union coverage mask before the
// bitmap is changed. Consequently, self-intersections and densely sampled
// pointer input never apply an effect more than once to the same pixel.
enum class PrivacyEffectMode {
    mosaic,
    blur,
    solid,
};

struct PrivacyEffectOptions {
    PrivacyEffectMode mode{PrivacyEffectMode::mosaic};

    // Mosaic: globally anchored block side in pixels (2..512).
    // Blur: box-blur radius in pixels (1..512).
    // Solid: fill opacity (1..255). A zero value is always a no-op.
    int strength{};
    COLORREF solid_color{RGB(0, 0, 0)};
};

struct PrivacyMaskLimits {
    // Hard ceilings cannot be raised by a caller. The default mask ceiling
    // covers a full 7680 x 4320 (8K UHD) bitmap while bounding temporary
    // allocations. Lower values are useful to enforce a tighter call budget.
    static constexpr std::size_t hard_max_input_points = 100'000;
    static constexpr std::size_t hard_max_raster_points = 4'096;
    static constexpr std::size_t hard_max_mask_pixels = 32U * 1024U * 1024U;
    static constexpr std::size_t hard_max_raster_work = 128U * 1024U * 1024U;
    static constexpr int hard_max_brush_radius = 1'024;
    static constexpr int hard_max_effect_strength = 512;

    std::size_t max_input_points{hard_max_input_points};
    std::size_t max_raster_points{hard_max_raster_points};
    std::size_t max_mask_pixels{hard_max_mask_pixels};
    std::size_t max_raster_work{hard_max_raster_work};
};

// Applies one freehand privacy stroke. `brush_radius` is measured in physical
// bitmap pixels. On invalid input or a resource-limit result, `bitmap` is left
// byte-for-byte unchanged. Covered output pixels are made opaque.
[[nodiscard]] PrivacyEffectResult apply_privacy_stroke(
    Bitmap& bitmap,
    std::span<const POINT> points,
    int brush_radius,
    const PrivacyEffectOptions& options,
    const PrivacyMaskLimits& limits = {}) noexcept;

// Applies an antialiased rounded-rectangle alpha mask in-place. The radius is
// measured in physical pixels and clamped to half of the shortest edge. A
// non-positive radius leaves the bitmap unchanged.
void apply_rounded_corner_mask(Bitmap& bitmap, int radius) noexcept;
[[nodiscard]] Bitmap rotate_90_cw(const Bitmap& source);
[[nodiscard]] Bitmap rotate_90_ccw(const Bitmap& source);
[[nodiscard]] Bitmap flip_horizontal(const Bitmap& source);
[[nodiscard]] Bitmap flip_vertical(const Bitmap& source);

}  // namespace airshot
