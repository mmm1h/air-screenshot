#pragma once

#include "airshot/common.h"

namespace airshot {

// Keeps a pin fully inside the work area when it fits. Oversized pins retain
// enough visible pixels to be dragged back without unexpectedly rescaling.
[[nodiscard]] RectI recover_pin_bounds(
    const RectI& window,
    const RectI& work_area,
    int minimum_visible = 48) noexcept;

// Returns a non-enlarging initial scale that keeps a newly created pin within
// the requested fraction of the work area.
[[nodiscard]] double fit_pin_scale(
    int bitmap_width,
    int bitmap_height,
    const RectI& work_area,
    double maximum_coverage = 0.82) noexcept;

}  // namespace airshot
