#pragma once

#include "airshot/bitmap.h"

#include <cstddef>

namespace airshot {

// Decoration dimensions are physical output pixels. Colors use Win32
// COLORREF ordering; alpha is supplied separately for the shadow so callers
// cannot accidentally encode two conflicting opacity values.
struct OutputDecorationOptions {
    static constexpr int maximum_border_width = 512;
    static constexpr int maximum_shadow_blur_radius = 256;
    static constexpr int maximum_shadow_offset = 8'192;

    int border_width{};
    COLORREF border_color{RGB(0, 0, 0)};
    int shadow_blur_radius{};
    int shadow_offset_x{};
    int shadow_offset_y{};
    COLORREF shadow_color{RGB(0, 0, 0)};
    int shadow_opacity{};  // 0..255

    [[nodiscard]] friend constexpr bool operator==(
        const OutputDecorationOptions&,
        const OutputDecorationOptions&) noexcept = default;
};

struct OutputDecorationLimits {
    // The default permits an expanded 8K capture while keeping the output and
    // two reusable alpha masks below a deterministic process-memory ceiling.
    static constexpr std::size_t hard_max_output_pixels =
        64U * 1024U * 1024U;
    static constexpr std::size_t hard_max_working_bytes =
        512U * 1024U * 1024U;

    // Callers may lower, but cannot raise, the hard ceilings.
    std::size_t max_output_pixels{hard_max_output_pixels};
    std::size_t max_working_bytes{hard_max_working_bytes};
};

enum class OutputDecorationStatus {
    applied,
    no_change,
    invalid_input,
    invalid_options,
    resource_limit,
};

struct OutputDecorationResult {
    OutputDecorationStatus status{OutputDecorationStatus::invalid_input};
    Bitmap bitmap;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == OutputDecorationStatus::applied ||
               status == OutputDecorationStatus::no_change;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return succeeded();
    }
};

// Returns a new straight-alpha BGRA bitmap and never mutates `source`.
// Invalid input/options, arithmetic overflow, a budget violation, or an
// allocation failure returns an empty bitmap with a non-success status.
// Default options return a byte-for-byte copy with status `no_change`.
[[nodiscard]] OutputDecorationResult decorate_output_bitmap(
    const Bitmap& source,
    const OutputDecorationOptions& options = {},
    const OutputDecorationLimits& limits = {}) noexcept;

}  // namespace airshot
