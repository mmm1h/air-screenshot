#pragma once

#include "airshot/common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace airshot::overlay_detail {

enum class Tool {
    none,
    select,
    rectangle,
    ellipse,
    line,
    arrow,
    pen,
    mosaic,
    highlight,
    text,
    serial,
    eraser,
    blur,
    watermark,
};

[[nodiscard]] inline float tool_cursor_radius(
    Tool tool,
    float width) noexcept {
    const float safe_width =
        std::isfinite(width)
            ? std::clamp(width, 1.0F, 4096.0F)
            : 1.0F;
    switch (tool) {
        case Tool::pen:
            return safe_width * 0.5F;
        case Tool::mosaic:
        case Tool::blur:
            return std::max(5.0F, safe_width * 3.5F);
        case Tool::highlight:
            return static_cast<float>(std::clamp(
                std::round(static_cast<double>(safe_width) * 1.6),
                4.0,
                4096.0));
        case Tool::eraser:
            return std::max(8.0F, safe_width * 0.5F);
        case Tool::serial:
            return 8.0F + safe_width * 1.5F;
        default:
            return 0.0F;
    }
}

enum class TextStyle {
    normal,
    dark,
    outline,
};

enum class DragMode {
    none,
    move,
    top_left,
    top,
    top_right,
    right,
    bottom_right,
    bottom,
    bottom_left,
    left,
    annotate
};

enum class AnnotationHandle {
    none,
    top_left,
    top,
    top_right,
    right,
    bottom_right,
    bottom,
    bottom_left,
    left,
    start_point,
    end_point,
};

struct Annotation {
    Tool tool{Tool::none};
    POINT start{};
    POINT end{};
    std::vector<POINT> points;
    std::wstring text;
    COLORREF color{RGB(0, 102, 255)};
    float width{3.0F};
    int alpha{255};
    int serial{};
    TextStyle text_style{TextStyle::normal};
};

struct AnnotationControlHandle {
    AnnotationHandle kind{AnnotationHandle::none};
    POINT position{};
};

struct AnnotationControlHandles {
    std::array<AnnotationControlHandle, 8> items{};
    std::size_t count{};

    [[nodiscard]] const AnnotationControlHandle* begin() const noexcept {
        return items.data();
    }
    [[nodiscard]] const AnnotationControlHandle* end() const noexcept {
        return items.data() + count;
    }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }

    void push_unique(AnnotationHandle kind, POINT position) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (items[index].position.x == position.x &&
                items[index].position.y == position.y) {
                return;
            }
        }
        if (count < items.size()) {
            items[count++] = {kind, position};
        }
    }
};

struct ToolbarButton {
    std::wstring id;
    std::wstring label;
    RectI bounds;
    bool enabled{true};
    std::wstring disabled_reason;
};

struct AnnotationGeometry {
    POINT start{};
    POINT end{};
};

[[nodiscard]] inline AnnotationGeometry constrained_annotation_geometry(
    Tool tool,
    POINT anchor,
    POINT cursor,
    bool constrain,
    bool from_center) noexcept {
    long dx = cursor.x - anchor.x;
    long dy = cursor.y - anchor.y;

    if (constrain &&
        (tool == Tool::rectangle || tool == Tool::ellipse)) {
        const long side = std::max(std::abs(dx), std::abs(dy));
        dx = dx < 0 ? -side : side;
        dy = dy < 0 ? -side : side;
    } else if (constrain &&
               (tool == Tool::line || tool == Tool::arrow) &&
               (dx != 0 || dy != 0)) {
        constexpr double kQuarterTurn = 0.78539816339744830962;
        const double length = std::hypot(
            static_cast<double>(dx),
            static_cast<double>(dy));
        const double angle = std::atan2(
            static_cast<double>(dy),
            static_cast<double>(dx));
        const double snapped =
            std::round(angle / kQuarterTurn) * kQuarterTurn;
        dx = static_cast<long>(std::lround(std::cos(snapped) * length));
        dy = static_cast<long>(std::lround(std::sin(snapped) * length));
    }

    if (from_center &&
        (tool == Tool::rectangle || tool == Tool::ellipse ||
         tool == Tool::line || tool == Tool::arrow)) {
        return {
            {anchor.x - dx, anchor.y - dy},
            {anchor.x + dx, anchor.y + dy},
        };
    }
    return {anchor, {anchor.x + dx, anchor.y + dy}};
}

[[nodiscard]] inline AnnotationGeometry fit_annotation_geometry_to_canvas(
    AnnotationGeometry geometry,
    POINT anchor,
    int canvas_width,
    int canvas_height,
    bool from_center,
    bool preserve_aspect = true) noexcept {
    const long dx = geometry.end.x - anchor.x;
    const long dy = geometry.end.y - anchor.y;
    if (!preserve_aspect) {
        const long horizontal_limit =
            std::max(0L, std::min<long>(anchor.x, canvas_width - anchor.x));
        const long vertical_limit =
            std::max(0L, std::min<long>(anchor.y, canvas_height - anchor.y));
        const long fitted_dx =
            from_center
                ? std::clamp(dx, -horizontal_limit, horizontal_limit)
                : std::clamp(
                      dx,
                      -std::max(0L, anchor.x),
                      std::max(0L, static_cast<long>(canvas_width) - anchor.x));
        const long fitted_dy =
            from_center
                ? std::clamp(dy, -vertical_limit, vertical_limit)
                : std::clamp(
                      dy,
                      -std::max(0L, anchor.y),
                      std::max(0L, static_cast<long>(canvas_height) - anchor.y));
        if (from_center) {
            return {
                {anchor.x - fitted_dx, anchor.y - fitted_dy},
                {anchor.x + fitted_dx, anchor.y + fitted_dy},
            };
        }
        return {
            anchor,
            {anchor.x + fitted_dx, anchor.y + fitted_dy},
        };
    }

    double scale = 1.0;
    const auto fit_axis =
        [&](long delta, int negative_space, int positive_space) {
            if (delta > 0) {
                scale = std::min(
                    scale,
                    static_cast<double>(positive_space) /
                        static_cast<double>(delta));
            } else if (delta < 0) {
                scale = std::min(
                    scale,
                    static_cast<double>(negative_space) /
                        static_cast<double>(-delta));
            }
        };

    if (from_center) {
        fit_axis(
            dx,
            std::min(anchor.x, canvas_width - anchor.x),
            std::min(anchor.x, canvas_width - anchor.x));
        fit_axis(
            dy,
            std::min(anchor.y, canvas_height - anchor.y),
            std::min(anchor.y, canvas_height - anchor.y));
    } else {
        fit_axis(dx, anchor.x, canvas_width - anchor.x);
        fit_axis(dy, anchor.y, canvas_height - anchor.y);
    }
    scale = std::clamp(scale, 0.0, 1.0);
    const long fitted_dx =
        static_cast<long>(std::lround(static_cast<double>(dx) * scale));
    const long fitted_dy =
        static_cast<long>(std::lround(static_cast<double>(dy) * scale));
    if (from_center) {
        return {
            {anchor.x - fitted_dx, anchor.y - fitted_dy},
            {anchor.x + fitted_dx, anchor.y + fitted_dy},
        };
    }
    return {
        anchor,
        {anchor.x + fitted_dx, anchor.y + fitted_dy},
    };
}

[[nodiscard]] inline RectI annotation_bounds(
    const Annotation& annotation) noexcept {
    if (!annotation.points.empty()) {
        RectI bounds{
            annotation.points.front().x,
            annotation.points.front().y,
            annotation.points.front().x,
            annotation.points.front().y,
        };
        for (const POINT point : annotation.points) {
            bounds.left = std::min(bounds.left, static_cast<int>(point.x));
            bounds.top = std::min(bounds.top, static_cast<int>(point.y));
            bounds.right = std::max(bounds.right, static_cast<int>(point.x));
            bounds.bottom = std::max(bounds.bottom, static_cast<int>(point.y));
        }
        float radius_scale = 0.5F;
        if (annotation.tool == Tool::mosaic ||
            annotation.tool == Tool::blur) {
            radius_scale = 3.5F;
        } else if (annotation.tool == Tool::highlight) {
            radius_scale = 1.7F;
        }
        const int radius = std::max(
            2,
            static_cast<int>(
                std::ceil(annotation.width * radius_scale)));
        bounds.left -= radius;
        bounds.top -= radius;
        bounds.right += radius + 1;
        bounds.bottom += radius + 1;
        return bounds;
    }

    if (annotation.tool == Tool::text) {
        std::size_t line_count = 1;
        double current_line = 0.0;
        double longest_line = 0.0;
        for (const wchar_t character : annotation.text) {
            if (character == L'\n') {
                longest_line = std::max(longest_line, current_line);
                current_line = 0.0;
                ++line_count;
            } else if (character != L'\r') {
                current_line +=
                    character == L'\t'
                        ? 2.48
                        : (character <= 0x7F ? 0.62 : 1.0);
            }
        }
        longest_line = std::max(longest_line, current_line);
        const int width = std::max(
            24,
            static_cast<int>(std::ceil(
                std::max(1.0, longest_line) *
                std::max(1.0F, annotation.width) * 0.72F)));
        const int height = std::max(
            18,
            static_cast<int>(std::ceil(
                line_count * std::max(1.0F, annotation.width) * 1.35F)));
        return {
            annotation.start.x,
            annotation.start.y,
            annotation.start.x + width,
            annotation.start.y + height,
        };
    }

    if (annotation.tool == Tool::serial) {
        const int radius = static_cast<int>(
            std::ceil(8.0F + annotation.width * 1.5F));
        return {
            annotation.start.x - radius,
            annotation.start.y - radius,
            annotation.start.x + radius + 1,
            annotation.start.y + radius + 1,
        };
    }

    RectI bounds = RectI{
        annotation.start.x,
        annotation.start.y,
        annotation.end.x,
        annotation.end.y,
    }.normalized();
    int padding =
        std::max(2, static_cast<int>(std::ceil(annotation.width * 0.5F)));
    if (annotation.tool == Tool::arrow) {
        padding = std::max(
            padding,
            static_cast<int>(std::ceil(10.0F + annotation.width * 2.0F)));
    }
    if (annotation.tool == Tool::rectangle ||
        annotation.tool == Tool::ellipse ||
        annotation.tool == Tool::line ||
        annotation.tool == Tool::arrow) {
        bounds.left -= padding;
        bounds.top -= padding;
        bounds.right += padding;
        bounds.bottom += padding;
    }
    return bounds;
}

[[nodiscard]] inline RectI annotation_control_bounds(
    const Annotation& annotation) noexcept {
    if (!annotation.points.empty()) {
        RectI bounds{
            annotation.points.front().x,
            annotation.points.front().y,
            annotation.points.front().x,
            annotation.points.front().y,
        };
        for (const POINT point : annotation.points) {
            bounds.left = std::min(bounds.left, static_cast<int>(point.x));
            bounds.top = std::min(bounds.top, static_cast<int>(point.y));
            bounds.right = std::max(bounds.right, static_cast<int>(point.x));
            bounds.bottom = std::max(bounds.bottom, static_cast<int>(point.y));
        }
        return bounds;
    }

    switch (annotation.tool) {
        case Tool::rectangle:
        case Tool::ellipse:
        case Tool::line:
        case Tool::arrow:
        case Tool::pen:
        case Tool::mosaic:
        case Tool::highlight:
        case Tool::blur:
            return RectI{
                annotation.start.x,
                annotation.start.y,
                annotation.end.x,
                annotation.end.y,
            }.normalized();
        default:
            return annotation_bounds(annotation);
    }
}

[[nodiscard]] inline bool annotation_supports_box_resize(
    const Annotation& annotation) noexcept {
    switch (annotation.tool) {
        case Tool::rectangle:
        case Tool::ellipse:
            return true;
        case Tool::pen:
        case Tool::highlight:
            return !annotation.points.empty();
        case Tool::mosaic:
        case Tool::blur:
            return !annotation.points.empty() ||
                   annotation.start.x != annotation.end.x ||
                   annotation.start.y != annotation.end.y;
        default:
            return false;
    }
}

[[nodiscard]] inline AnnotationControlHandles annotation_control_handles(
    const Annotation& annotation) noexcept {
    AnnotationControlHandles handles;
    if (annotation.tool == Tool::line ||
        annotation.tool == Tool::arrow) {
        handles.push_unique(AnnotationHandle::start_point, annotation.start);
        handles.push_unique(AnnotationHandle::end_point, annotation.end);
        return handles;
    }
    if (!annotation_supports_box_resize(annotation)) {
        return handles;
    }

    const RectI bounds = annotation_control_bounds(annotation);
    const long middle_x =
        bounds.left + (bounds.right - bounds.left) / 2;
    const long middle_y =
        bounds.top + (bounds.bottom - bounds.top) / 2;
    const bool has_width = bounds.width() >= 2;
    const bool has_height = bounds.height() >= 2;
    if (has_width && has_height) {
        handles.push_unique(
            AnnotationHandle::top_left,
            {bounds.left, bounds.top});
        handles.push_unique(
            AnnotationHandle::top,
            {middle_x, bounds.top});
        handles.push_unique(
            AnnotationHandle::top_right,
            {bounds.right, bounds.top});
        handles.push_unique(
            AnnotationHandle::right,
            {bounds.right, middle_y});
        handles.push_unique(
            AnnotationHandle::bottom_right,
            {bounds.right, bounds.bottom});
        handles.push_unique(
            AnnotationHandle::bottom,
            {middle_x, bounds.bottom});
        handles.push_unique(
            AnnotationHandle::bottom_left,
            {bounds.left, bounds.bottom});
        handles.push_unique(
            AnnotationHandle::left,
            {bounds.left, middle_y});
    } else if (has_width) {
        handles.push_unique(
            AnnotationHandle::left,
            {bounds.left, middle_y});
        handles.push_unique(
            AnnotationHandle::right,
            {bounds.right, middle_y});
    } else if (has_height) {
        handles.push_unique(
            AnnotationHandle::top,
            {middle_x, bounds.top});
        handles.push_unique(
            AnnotationHandle::bottom,
            {middle_x, bounds.bottom});
    }
    return handles;
}

[[nodiscard]] inline AnnotationHandle hit_test_annotation_control_handle(
    const Annotation& annotation,
    POINT point,
    int radius = 8) noexcept {
    const long long hit_radius =
        static_cast<long long>(std::max(1, radius));
    const long long radius_squared = hit_radius * hit_radius;
    for (const auto& handle : annotation_control_handles(annotation)) {
        const long long dx =
            static_cast<long long>(point.x) - handle.position.x;
        const long long dy =
            static_cast<long long>(point.y) - handle.position.y;
        if (dx * dx + dy * dy <= radius_squared) {
            return handle.kind;
        }
    }
    return AnnotationHandle::none;
}

inline void translate_annotation(
    Annotation& annotation,
    int delta_x,
    int delta_y) noexcept {
    annotation.start.x += delta_x;
    annotation.start.y += delta_y;
    annotation.end.x += delta_x;
    annotation.end.y += delta_y;
    for (POINT& point : annotation.points) {
        point.x += delta_x;
        point.y += delta_y;
    }
}

[[nodiscard]] inline POINT clamp_annotation_translation(
    const Annotation& annotation,
    int delta_x,
    int delta_y,
    int canvas_width,
    int canvas_height) noexcept {
    const RectI bounds = annotation_bounds(annotation);
    const int minimum_x = -bounds.left;
    const int maximum_x = canvas_width - bounds.right;
    const int minimum_y = -bounds.top;
    const int maximum_y = canvas_height - bounds.bottom;
    return {
        minimum_x <= maximum_x
            ? std::clamp(delta_x, minimum_x, maximum_x)
            : 0,
        minimum_y <= maximum_y
            ? std::clamp(delta_y, minimum_y, maximum_y)
            : 0,
    };
}

[[nodiscard]] inline bool annotation_geometry_equal(
    const Annotation& first,
    const Annotation& second) noexcept {
    if (first.start.x != second.start.x ||
        first.start.y != second.start.y ||
        first.end.x != second.end.x ||
        first.end.y != second.end.y ||
        first.points.size() != second.points.size()) {
        return false;
    }
    for (std::size_t index = 0; index < first.points.size(); ++index) {
        if (first.points[index].x != second.points[index].x ||
            first.points[index].y != second.points[index].y) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline Annotation resize_annotation_from_handle(
    const Annotation& original,
    AnnotationHandle handle,
    POINT cursor,
    int canvas_width,
    int canvas_height,
    bool preserve_aspect = false) noexcept {
    constexpr int kMinimumExtent = 2;
    if (handle == AnnotationHandle::none ||
        canvas_width <= 0 ||
        canvas_height <= 0) {
        return original;
    }

    cursor.x = std::clamp(
        cursor.x,
        0L,
        static_cast<long>(canvas_width));
    cursor.y = std::clamp(
        cursor.y,
        0L,
        static_cast<long>(canvas_height));

    if ((original.tool == Tool::line ||
         original.tool == Tool::arrow) &&
        (handle == AnnotationHandle::start_point ||
         handle == AnnotationHandle::end_point)) {
        Annotation updated = original;
        const POINT fixed =
            handle == AnnotationHandle::start_point
                ? original.end
                : original.start;
        if (std::hypot(
                static_cast<double>(cursor.x - fixed.x),
                static_cast<double>(cursor.y - fixed.y)) <
            static_cast<double>(kMinimumExtent)) {
            return original;
        }
        if (handle == AnnotationHandle::start_point) {
            updated.start = cursor;
        } else {
            updated.end = cursor;
        }
        const POINT correction = clamp_annotation_translation(
            updated,
            0,
            0,
            canvas_width,
            canvas_height);
        translate_annotation(updated, correction.x, correction.y);
        return updated;
    }

    if (!annotation_supports_box_resize(original)) {
        return original;
    }

    const RectI source = annotation_control_bounds(original);
    if (source.width() < kMinimumExtent &&
        source.height() < kMinimumExtent) {
        return original;
    }

    const RectI visual = annotation_bounds(original);
    const int safe_left = std::max(0, source.left - visual.left);
    const int safe_top = std::max(0, source.top - visual.top);
    const int safe_right =
        canvas_width - std::max(0, visual.right - source.right);
    const int safe_bottom =
        canvas_height - std::max(0, visual.bottom - source.bottom);
    if (safe_left > safe_right || safe_top > safe_bottom) {
        return original;
    }
    cursor.x = std::clamp(
        cursor.x,
        static_cast<long>(safe_left),
        static_cast<long>(safe_right));
    cursor.y = std::clamp(
        cursor.y,
        static_cast<long>(safe_top),
        static_cast<long>(safe_bottom));

    int left = source.left;
    int top = source.top;
    int right = source.right;
    int bottom = source.bottom;
    const bool moves_left =
        handle == AnnotationHandle::top_left ||
        handle == AnnotationHandle::bottom_left ||
        handle == AnnotationHandle::left;
    const bool moves_right =
        handle == AnnotationHandle::top_right ||
        handle == AnnotationHandle::bottom_right ||
        handle == AnnotationHandle::right;
    const bool moves_top =
        handle == AnnotationHandle::top_left ||
        handle == AnnotationHandle::top ||
        handle == AnnotationHandle::top_right;
    const bool moves_bottom =
        handle == AnnotationHandle::bottom_left ||
        handle == AnnotationHandle::bottom ||
        handle == AnnotationHandle::bottom_right;
    const bool corner =
        (moves_left || moves_right) &&
        (moves_top || moves_bottom);

    if (preserve_aspect && corner &&
        source.width() >= kMinimumExtent &&
        source.height() >= kMinimumExtent) {
        const int fixed_x = moves_left ? source.right : source.left;
        const int fixed_y = moves_top ? source.bottom : source.top;
        const int x_sign = moves_left ? -1 : 1;
        const int y_sign = moves_top ? -1 : 1;
        const int requested_width = std::abs(
            static_cast<int>(cursor.x) - fixed_x);
        const int requested_height = std::abs(
            static_cast<int>(cursor.y) - fixed_y);
        const int maximum_width =
            x_sign < 0 ? fixed_x - safe_left : safe_right - fixed_x;
        const int maximum_height =
            y_sign < 0 ? fixed_y - safe_top : safe_bottom - fixed_y;
        const double minimum_scale = std::max(
            static_cast<double>(kMinimumExtent) / source.width(),
            static_cast<double>(kMinimumExtent) / source.height());
        const double maximum_scale = std::min(
            static_cast<double>(std::max(0, maximum_width)) /
                source.width(),
            static_cast<double>(std::max(0, maximum_height)) /
                source.height());
        if (maximum_scale < minimum_scale) {
            return original;
        }
        const double requested_scale = std::max(
            static_cast<double>(requested_width) / source.width(),
            static_cast<double>(requested_height) / source.height());
        const double scale = std::clamp(
            requested_scale,
            minimum_scale,
            maximum_scale);
        const int target_width = std::max(
            kMinimumExtent,
            static_cast<int>(std::lround(source.width() * scale)));
        const int target_height = std::max(
            kMinimumExtent,
            static_cast<int>(std::lround(source.height() * scale)));
        if (moves_left) left = fixed_x - target_width;
        else right = fixed_x + target_width;
        if (moves_top) top = fixed_y - target_height;
        else bottom = fixed_y + target_height;
    } else {
        if (moves_left && source.width() >= kMinimumExtent) {
            if (safe_left > right - kMinimumExtent) {
                return original;
            }
            left = std::clamp(
                static_cast<int>(cursor.x),
                safe_left,
                right - kMinimumExtent);
        } else if (moves_right && source.width() >= kMinimumExtent) {
            if (left + kMinimumExtent > safe_right) {
                return original;
            }
            right = std::clamp(
                static_cast<int>(cursor.x),
                left + kMinimumExtent,
                safe_right);
        }
        if (moves_top && source.height() >= kMinimumExtent) {
            if (safe_top > bottom - kMinimumExtent) {
                return original;
            }
            top = std::clamp(
                static_cast<int>(cursor.y),
                safe_top,
                bottom - kMinimumExtent);
        } else if (moves_bottom && source.height() >= kMinimumExtent) {
            if (top + kMinimumExtent > safe_bottom) {
                return original;
            }
            bottom = std::clamp(
                static_cast<int>(cursor.y),
                top + kMinimumExtent,
                safe_bottom);
        }
    }

    const auto remap_axis = [](
                                long value,
                                int source_minimum,
                                int source_maximum,
                                int target_minimum,
                                int target_maximum) noexcept {
        if (source_maximum == source_minimum) {
            return value;
        }
        const double ratio =
            static_cast<double>(value - source_minimum) /
            static_cast<double>(source_maximum - source_minimum);
        return static_cast<long>(std::lround(
            target_minimum +
            ratio * (target_maximum - target_minimum)));
    };
    const auto remap_point = [&](POINT point) noexcept {
        point.x = remap_axis(
            point.x,
            source.left,
            source.right,
            left,
            right);
        point.y = remap_axis(
            point.y,
            source.top,
            source.bottom,
            top,
            bottom);
        point.x = std::clamp(
            point.x,
            0L,
            static_cast<long>(canvas_width));
        point.y = std::clamp(
            point.y,
            0L,
            static_cast<long>(canvas_height));
        return point;
    };

    Annotation updated = original;
    updated.start = remap_point(original.start);
    updated.end = remap_point(original.end);
    for (std::size_t index = 0; index < updated.points.size(); ++index) {
        updated.points[index] = remap_point(original.points[index]);
    }
    if (!updated.points.empty()) {
        updated.start = updated.points.front();
        updated.end = updated.points.back();
    }

    const POINT correction = clamp_annotation_translation(
        updated,
        0,
        0,
        canvas_width,
        canvas_height);
    translate_annotation(updated, correction.x, correction.y);
    return updated;
}

[[nodiscard]] inline POINT preferred_clone_translation(
    const Annotation& annotation,
    int offset,
    int canvas_width,
    int canvas_height) noexcept {
    const int distance = std::max(1, std::abs(offset));
    const std::array<POINT, 4> preferred{{
        {distance, distance},
        {-distance, distance},
        {distance, -distance},
        {-distance, -distance},
    }};
    POINT best{};
    long long best_score = 0;
    for (const POINT candidate : preferred) {
        const POINT clamped = clamp_annotation_translation(
            annotation,
            candidate.x,
            candidate.y,
            canvas_width,
            canvas_height);
        const long long score =
            std::abs(static_cast<long long>(clamped.x)) +
            std::abs(static_cast<long long>(clamped.y));
        if (score > best_score) {
            best = clamped;
            best_score = score;
        }
        if (clamped.x == candidate.x &&
            clamped.y == candidate.y) {
            return clamped;
        }
    }
    return best;
}

[[nodiscard]] inline std::vector<POINT> resample_polyline(
    const std::vector<POINT>& points,
    double spacing,
    std::size_t maximum_points = 200000) {
    if (points.size() < 2 || !std::isfinite(spacing) || spacing <= 0.0) {
        return points;
    }
    maximum_points = std::max<std::size_t>(2, maximum_points);
    std::vector<POINT> result;
    result.reserve(std::min(points.size(), maximum_points));
    result.push_back(points.front());

    double carried = 0.0;
    double start_x = static_cast<double>(points.front().x);
    double start_y = static_cast<double>(points.front().y);
    for (std::size_t index = 1;
         index < points.size() && result.size() < maximum_points;
         ++index) {
        const double target_x = static_cast<double>(points[index].x);
        const double target_y = static_cast<double>(points[index].y);
        double dx = target_x - start_x;
        double dy = target_y - start_y;
        double remaining = std::hypot(dx, dy);
        while (remaining > 0.0 &&
               carried + remaining >= spacing &&
               result.size() < maximum_points) {
            const double advance = spacing - carried;
            const double ratio = advance / remaining;
            start_x += dx * ratio;
            start_y += dy * ratio;
            const POINT sampled{
                static_cast<long>(std::lround(start_x)),
                static_cast<long>(std::lround(start_y)),
            };
            if (sampled.x != result.back().x ||
                sampled.y != result.back().y) {
                result.push_back(sampled);
            }
            dx = target_x - start_x;
            dy = target_y - start_y;
            remaining = std::hypot(dx, dy);
            carried = 0.0;
        }
        carried += remaining;
        start_x = target_x;
        start_y = target_y;
    }
    if (result.back().x != points.back().x ||
        result.back().y != points.back().y) {
        if (result.size() < maximum_points) {
            result.push_back(points.back());
        } else {
            result.back() = points.back();
        }
    }
    return result;
}

[[nodiscard]] inline int next_serial_number(
    const std::vector<Annotation>& annotations) noexcept {
    int highest = 0;
    for (const auto& annotation : annotations) {
        if (annotation.tool == Tool::serial) {
            highest = std::max(highest, annotation.serial);
        }
    }
    return highest < std::numeric_limits<int>::max()
               ? highest + 1
               : std::numeric_limits<int>::max();
}

inline void renumber_serial_annotations(
    std::vector<Annotation>& annotations) noexcept {
    int next = 1;
    for (auto& annotation : annotations) {
        if (annotation.tool != Tool::serial) {
            continue;
        }
        annotation.serial = next;
        if (next < std::numeric_limits<int>::max()) {
            ++next;
        }
    }
}

}  // namespace airshot::overlay_detail
