#include "airshot/pin_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace airshot {
namespace {

[[nodiscard]] int clamp_origin(
    int origin,
    int size,
    int work_start,
    int work_end,
    int minimum_visible) noexcept {
    const std::int64_t work_size =
        static_cast<std::int64_t>(work_end) - work_start;
    if (size <= 0 || work_size <= 0) {
        return origin;
    }

    std::int64_t minimum = work_start;
    std::int64_t maximum =
        static_cast<std::int64_t>(work_end) - size;
    if (size > work_size) {
        const std::int64_t visible =
            std::clamp<std::int64_t>(
                minimum_visible,
                1,
                std::min<std::int64_t>(size, work_size));
        minimum =
            static_cast<std::int64_t>(work_start) -
            size +
            visible;
        maximum =
            static_cast<std::int64_t>(work_end) -
            visible;
    }
    const std::int64_t clamped =
        std::clamp<std::int64_t>(
            origin,
            minimum,
            maximum);
    return static_cast<int>(
        std::clamp<std::int64_t>(
            clamped,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
}

}  // namespace

RectI recover_pin_bounds(
    const RectI& window,
    const RectI& work_area,
    int minimum_visible) noexcept {
    if (window.empty() || work_area.empty()) {
        return window;
    }
    const int width = window.width();
    const int height = window.height();
    const int left = clamp_origin(
        window.left,
        width,
        work_area.left,
        work_area.right,
        minimum_visible);
    const int top = clamp_origin(
        window.top,
        height,
        work_area.top,
        work_area.bottom,
        minimum_visible);
    return {
        left,
        top,
        left + width,
        top + height,
    };
}

double fit_pin_scale(
    int bitmap_width,
    int bitmap_height,
    const RectI& work_area,
    double maximum_coverage) noexcept {
    if (bitmap_width <= 0 || bitmap_height <= 0 ||
        work_area.empty() || !std::isfinite(maximum_coverage) ||
        maximum_coverage <= 0.0) {
        return 1.0;
    }
    const double coverage =
        std::clamp(maximum_coverage, 0.1, 1.0);
    const double horizontal =
        static_cast<double>(work_area.width()) * coverage /
        static_cast<double>(bitmap_width);
    const double vertical =
        static_cast<double>(work_area.height()) * coverage /
        static_cast<double>(bitmap_height);
    const double scale = std::min({1.0, horizontal, vertical});
    return std::isfinite(scale) && scale > 0.0
               ? std::max(0.02, scale)
               : 1.0;
}

}  // namespace airshot
