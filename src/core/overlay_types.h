#pragma once

#include "airshot/common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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

[[nodiscard]] constexpr bool tool_uses_bitmap_effect_preview(
    Tool tool) noexcept {
    return tool == Tool::mosaic || tool == Tool::blur ||
           tool == Tool::highlight;
}

[[nodiscard]] constexpr bool should_persist_tool_style(
    Tool active_tool,
    bool has_selected_annotation) noexcept {
    return active_tool != Tool::select || !has_selected_annotation;
}

[[nodiscard]] constexpr bool effect_geometry_mode_available(
    Tool active_tool,
    Tool style_tool,
    bool has_selected_annotation) noexcept {
    const bool effect_tool =
        style_tool == Tool::mosaic || style_tool == Tool::blur;
    return effect_tool &&
           !(active_tool == Tool::select && has_selected_annotation);
}

[[nodiscard]] inline float tool_visual_radius(
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

[[nodiscard]] inline float tool_cursor_radius(
    Tool tool,
    float width) noexcept {
    return tool_visual_radius(tool, width);
}

enum class TextStyle {
    normal,
    dark,
    outline,
};

enum class ShapeFillStyle {
    outline,
    translucent,
};

enum class StrokePattern {
    solid,
    dashed,
};

enum class ArrowHeadStyle {
    forward,
    reverse,
    both,
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

enum class InteractionSettleMode {
    commit,
    cancel,
};

enum class InteractionCommand {
    undo,
    redo,
    copy,
    save,
    ocr,
    close,
};

[[nodiscard]] constexpr bool tool_shortcut_keydown_allowed(
    bool is_auto_repeat) noexcept {
    return !is_auto_repeat;
}

[[nodiscard]] constexpr bool should_discard_ocr_completion(
    bool session_done,
    bool cancellation_requested,
    bool worker_cancelled) noexcept {
    return session_done || cancellation_requested || worker_cancelled;
}

[[nodiscard]] constexpr InteractionSettleMode
interaction_settle_mode(InteractionCommand command) noexcept {
    switch (command) {
        case InteractionCommand::copy:
        case InteractionCommand::save:
        case InteractionCommand::ocr:
            return InteractionSettleMode::commit;
        case InteractionCommand::undo:
        case InteractionCommand::redo:
        case InteractionCommand::close:
            return InteractionSettleMode::cancel;
    }
    return InteractionSettleMode::cancel;
}

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
    ShapeFillStyle fill_style{ShapeFillStyle::outline};
    StrokePattern stroke_pattern{StrokePattern::solid};
    ArrowHeadStyle arrow_head_style{ArrowHeadStyle::forward};
    bool rounded_rectangle{};
    // Text layout is measured with the same GDI font and flags used by the
    // bitmap renderer. Keeping the result on the annotation makes selection,
    // hit testing and the live preview agree with the committed image.
    RectI measured_text_bounds{};
};

struct ToolStyleState {
    COLORREF color{RGB(245, 34, 45)};
    float width{4.0F};
    float text_size{18.0F};
    TextStyle text_style{TextStyle::normal};
    int highlight_alpha{96};
    int effect_strength{50};
    bool effect_rect{};
    ShapeFillStyle fill_style{ShapeFillStyle::outline};
    StrokePattern stroke_pattern{StrokePattern::solid};
    ArrowHeadStyle arrow_head_style{ArrowHeadStyle::forward};
    bool rounded_rectangle{};
};

inline constexpr std::size_t kToolStyleCount =
    static_cast<std::size_t>(Tool::watermark) + 1;

[[nodiscard]] constexpr std::size_t tool_style_index(Tool tool) noexcept {
    const auto index = static_cast<std::size_t>(tool);
    return index < kToolStyleCount ? index : 0;
}

class ToolStylePalette {
public:
    explicit ToolStylePalette(int highlight_alpha = 96) noexcept {
        styles_[tool_style_index(Tool::highlight)].color =
            RGB(250, 219, 20);
        styles_[tool_style_index(Tool::highlight)].highlight_alpha =
            std::clamp(highlight_alpha, 24, 192);
        styles_[tool_style_index(Tool::watermark)].color =
            RGB(255, 150, 150);
    }

    [[nodiscard]] ToolStyleState& for_tool(Tool tool) noexcept {
        return styles_[tool_style_index(tool)];
    }

    [[nodiscard]] const ToolStyleState& for_tool(
        Tool tool) const noexcept {
        return styles_[tool_style_index(tool)];
    }

private:
    std::array<ToolStyleState, kToolStyleCount> styles_{};
};

[[nodiscard]] inline int serial_digit_count(int serial) noexcept {
    unsigned int value = serial < 0
                             ? static_cast<unsigned int>(
                                   -(static_cast<long long>(serial)))
                             : static_cast<unsigned int>(serial);
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits + (serial < 0 ? 1 : 0);
}

[[nodiscard]] inline float serial_font_size(
    float width,
    int serial) noexcept {
    const float base = tool_visual_radius(Tool::serial, width);
    const int extra_digits = std::max(0, serial_digit_count(serial) - 2);
    const float scale = std::max(
        0.55F,
        1.0F - static_cast<float>(extra_digits) * 0.08F);
    return std::max(8.0F, base * scale);
}

[[nodiscard]] inline float serial_visual_radius(
    float width,
    int serial) noexcept {
    const float base = tool_visual_radius(Tool::serial, width);
    const int digits = serial_digit_count(serial);
    const int extra_digits = std::max(0, serial_digit_count(serial) - 2);
    const float expanded =
        base + static_cast<float>(extra_digits) *
                   std::max(2.0F, width * 0.5F);
    const float text_half_width =
        static_cast<float>(digits) * serial_font_size(width, serial) *
            0.32F +
        std::max(2.0F, width * 0.5F);
    return std::max(expanded, text_half_width);
}

[[nodiscard]] inline float annotation_visual_radius(
    const Annotation& annotation) noexcept {
    if (annotation.tool == Tool::serial) {
        return serial_visual_radius(annotation.width, annotation.serial);
    }
    return tool_visual_radius(annotation.tool, annotation.width);
}

[[nodiscard]] inline float arrow_head_length(float width) noexcept {
    const float safe_width =
        std::isfinite(width) ? std::max(1.0F, width) : 1.0F;
    return std::clamp(8.0F + safe_width * 2.0F, 10.0F, 34.0F);
}

struct ArrowHeadWings {
    POINT first{};
    POINT second{};
};

[[nodiscard]] inline ArrowHeadWings arrow_head_wings(
    POINT tip,
    POINT tail,
    float width) noexcept {
    const double dx = static_cast<double>(tip.x - tail.x);
    const double dy = static_cast<double>(tip.y - tail.y);
    const double shaft_length = std::hypot(dx, dy);
    if (shaft_length <= 0.0) {
        return {tip, tip};
    }
    const double angle = std::atan2(dy, dx);
    const double length = std::min(
        static_cast<double>(arrow_head_length(width)),
        std::max(3.0, shaft_length * 0.65));
    constexpr double kWingAngle = 0.48;
    return {
        {
            tip.x - static_cast<long>(
                        std::lround(std::cos(angle + kWingAngle) * length)),
            tip.y - static_cast<long>(
                        std::lround(std::sin(angle + kWingAngle) * length)),
        },
        {
            tip.x - static_cast<long>(
                        std::lround(std::cos(angle - kWingAngle) * length)),
            tip.y - static_cast<long>(
                        std::lround(std::sin(angle - kWingAngle) * length)),
        },
    };
}

[[nodiscard]] constexpr bool arrow_has_start_head(
    ArrowHeadStyle style) noexcept {
    return style == ArrowHeadStyle::reverse || style == ArrowHeadStyle::both;
}

[[nodiscard]] constexpr bool arrow_has_end_head(
    ArrowHeadStyle style) noexcept {
    return style == ArrowHeadStyle::forward || style == ArrowHeadStyle::both;
}

[[nodiscard]] inline float rounded_rectangle_radius(
    RectI bounds,
    float width) noexcept {
    bounds = bounds.normalized();
    const float maximum = static_cast<float>(
        std::max(0, std::min(bounds.width(), bounds.height()))) * 0.5F;
    const float requested = std::clamp(
        4.0F + (std::isfinite(width) ? width : 1.0F),
        5.0F,
        18.0F);
    return std::min(maximum, requested);
}

[[nodiscard]] inline double ellipse_signed_distance(
    RectI bounds,
    POINT point) noexcept {
    bounds = bounds.normalized();
    const double radius_x =
        std::max(0.5, static_cast<double>(bounds.width()) * 0.5);
    const double radius_y =
        std::max(0.5, static_cast<double>(bounds.height()) * 0.5);
    const double center_x =
        (static_cast<double>(bounds.left) + bounds.right) * 0.5;
    const double center_y =
        (static_cast<double>(bounds.top) + bounds.bottom) * 0.5;
    const double x = std::abs(static_cast<double>(point.x) - center_x);
    const double y = std::abs(static_cast<double>(point.y) - center_y);
    const bool inside =
        (x * x) / (radius_x * radius_x) +
            (y * y) / (radius_y * radius_y) <=
        1.0;
    if (x == 0.0 && y == 0.0) {
        const double distance = std::min(radius_x, radius_y);
        return inside ? -distance : distance;
    }

    constexpr double kHalfPi = 1.57079632679489661923;
    double parameter = std::atan2(y * radius_x, x * radius_y);
    parameter = std::clamp(parameter, 0.0, kHalfPi);
    for (int iteration = 0; iteration < 12; ++iteration) {
        const double cosine = std::cos(parameter);
        const double sine = std::sin(parameter);
        const double ellipse_x = radius_x * cosine;
        const double ellipse_y = radius_y * sine;
        const double tangent_x = -radius_x * sine;
        const double tangent_y = radius_y * cosine;
        const double delta_x = ellipse_x - x;
        const double delta_y = ellipse_y - y;
        const double first =
            delta_x * tangent_x + delta_y * tangent_y;
        const double second =
            tangent_x * tangent_x + tangent_y * tangent_y +
            delta_x * (-radius_x * cosine) +
            delta_y * (-radius_y * sine);
        if (!std::isfinite(second) || std::abs(second) < 1.0e-9) {
            break;
        }
        const double next = std::clamp(
            parameter - first / second,
            0.0,
            kHalfPi);
        if (std::abs(next - parameter) < 1.0e-7) {
            parameter = next;
            break;
        }
        parameter = next;
    }
    const double closest_x = radius_x * std::cos(parameter);
    const double closest_y = radius_y * std::sin(parameter);
    const double distance = std::hypot(closest_x - x, closest_y - y);
    return inside ? -distance : distance;
}

[[nodiscard]] inline double rounded_rectangle_signed_distance(
    RectI bounds,
    POINT point,
    double radius) noexcept {
    bounds = bounds.normalized();
    const double half_width =
        std::max(0.5, static_cast<double>(bounds.width()) * 0.5);
    const double half_height =
        std::max(0.5, static_cast<double>(bounds.height()) * 0.5);
    radius = std::clamp(
        std::isfinite(radius) ? radius : 0.0,
        0.0,
        std::min(half_width, half_height));
    const double center_x =
        (static_cast<double>(bounds.left) + bounds.right) * 0.5;
    const double center_y =
        (static_cast<double>(bounds.top) + bounds.bottom) * 0.5;
    const double local_x =
        std::abs(static_cast<double>(point.x) - center_x) -
        (half_width - radius);
    const double local_y =
        std::abs(static_cast<double>(point.y) - center_y) -
        (half_height - radius);
    const double outside = std::hypot(
        std::max(0.0, local_x),
        std::max(0.0, local_y));
    const double inside = std::min(std::max(local_x, local_y), 0.0);
    return outside + inside - radius;
}

[[nodiscard]] inline bool shape_annotation_hit_test(
    const Annotation& annotation,
    POINT point,
    double hit_margin = 6.0) noexcept {
    if (annotation.tool != Tool::rectangle &&
        annotation.tool != Tool::ellipse) {
        return false;
    }
    const RectI bounds = RectI{
        annotation.start.x,
        annotation.start.y,
        annotation.end.x,
        annotation.end.y,
    }.normalized();
    if (bounds.width() <= 0 || bounds.height() <= 0) {
        return false;
    }
    const double safe_width =
        std::isfinite(annotation.width)
            ? std::clamp(static_cast<double>(annotation.width), 1.0, 4096.0)
            : 1.0;
    const double tolerance =
        safe_width * 0.5 +
        std::max(0.0, std::isfinite(hit_margin) ? hit_margin : 0.0);
    const double signed_distance =
        annotation.tool == Tool::ellipse
            ? ellipse_signed_distance(bounds, point)
            : rounded_rectangle_signed_distance(
                  bounds,
                  point,
                  annotation.rounded_rectangle
                      ? rounded_rectangle_radius(bounds, annotation.width)
                      : 0.0);
    return annotation.fill_style == ShapeFillStyle::translucent
               ? signed_distance <= tolerance
               : std::abs(signed_distance) <= tolerance;
}

[[nodiscard]] inline bool annotation_is_committable(
    const Annotation& annotation) noexcept {
    constexpr int kMinimumBoxExtent = 2;
    constexpr double kMinimumLineLength = 3.0;
    const long delta_x = annotation.end.x - annotation.start.x;
    const long delta_y = annotation.end.y - annotation.start.y;

    switch (annotation.tool) {
        case Tool::rectangle:
        case Tool::ellipse:
            return std::abs(delta_x) >= kMinimumBoxExtent &&
                   std::abs(delta_y) >= kMinimumBoxExtent;
        case Tool::line:
        case Tool::arrow:
            return std::hypot(
                       static_cast<double>(delta_x),
                       static_cast<double>(delta_y)) >=
                   kMinimumLineLength;
        case Tool::mosaic:
        case Tool::blur:
            if (annotation.alpha <= 0) {
                return false;
            }
            if (annotation.points.empty()) {
                return std::abs(delta_x) >= kMinimumBoxExtent &&
                       std::abs(delta_y) >= kMinimumBoxExtent;
            }
            return true;
        case Tool::pen:
        case Tool::highlight:
            // A click is an intentional dot for freehand tools.
            return !annotation.points.empty();
        case Tool::text:
            return !annotation.text.empty();
        case Tool::serial:
        case Tool::watermark:
            return true;
        case Tool::eraser:
        case Tool::select:
        case Tool::none:
            return false;
    }
    return false;
}

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

inline constexpr unsigned int kOverlayBaseDpi = 96U;

[[nodiscard]] constexpr int scale_overlay_ui_px(
    int value,
    unsigned int dpi) noexcept {
    const auto effective_dpi = dpi == 0U ? kOverlayBaseDpi : dpi;
    const auto product =
        static_cast<std::int64_t>(value) * effective_dpi;
    const auto half = static_cast<std::int64_t>(kOverlayBaseDpi / 2U);
    const auto rounded = product >= 0
                             ? (product + half) / kOverlayBaseDpi
                             : (product - half) / kOverlayBaseDpi;
    return static_cast<int>(std::clamp<std::int64_t>(
        rounded,
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::max()));
}

struct OverlayUiMetrics {
    unsigned int dpi{kOverlayBaseDpi};

    [[nodiscard]] constexpr int px(int value) const noexcept {
        return scale_overlay_ui_px(value, dpi);
    }

    [[nodiscard]] constexpr float px(float value) const noexcept {
        const auto effective_dpi = dpi == 0U ? kOverlayBaseDpi : dpi;
        return value * static_cast<float>(effective_dpi) /
               static_cast<float>(kOverlayBaseDpi);
    }
};

struct ToolbarMetrics {
    int button_width;
    int button_height;
    int spacing;
    int padding;
    unsigned int dpi{kOverlayBaseDpi};
};

struct ToolbarRow {
    std::vector<std::pair<std::wstring, std::wstring>> items;
    int width{};
};

[[nodiscard]] inline int toolbar_item_width(
    std::wstring_view id,
    const ToolbarMetrics& metrics) noexcept {
    const OverlayUiMetrics ui{metrics.dpi};
    if (id == L"drag") return ui.px(20);
    if (id == L"|") return ui.px(9);
    if (id == L"text_size_btn") return ui.px(86);
    if (id == L"mosaic_strength_slider" ||
        id == L"watermark_opacity_slider") return ui.px(188);
    if (id == L"watermark_text") return ui.px(122);
    if (id == L"watermark_apply" || id == L"watermark_clear") return ui.px(54);
    if (id.starts_with(L"effect_") || id.starts_with(L"mode_")) return ui.px(68);
    if (id.starts_with(L"fill_") || id.starts_with(L"stroke_") ||
        id.starts_with(L"corner_") || id.starts_with(L"head_")) {
        return ui.px(58);
    }
    return metrics.button_width;
}

[[nodiscard]] inline int toolbar_row_width(
    const std::vector<std::pair<std::wstring, std::wstring>>& items,
    const ToolbarMetrics& metrics) noexcept {
    int width = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            width += metrics.spacing;
        }
        width += toolbar_item_width(items[index].first, metrics);
    }
    return width;
}

inline void trim_toolbar_row(
    ToolbarRow& row,
    const ToolbarMetrics& metrics) {
    while (!row.items.empty() && row.items.back().first == L"|") {
        row.items.pop_back();
    }
    row.width = toolbar_row_width(row.items, metrics);
}

[[nodiscard]] inline std::vector<ToolbarRow> wrap_toolbar_items(
    const std::vector<std::pair<std::wstring, std::wstring>>& items,
    const ToolbarMetrics& metrics,
    const RectI& bounds) {
    const int available_width = std::max(
        metrics.button_width,
        bounds.width() - 2 * metrics.padding);
    std::vector<ToolbarRow> rows;
    ToolbarRow current;
    for (const auto& item : items) {
        const bool separator = item.first == L"|";
        if (separator && current.items.empty()) {
            continue;
        }
        const int width = toolbar_item_width(item.first, metrics);
        const int next_width = current.items.empty()
                                   ? width
                                   : current.width + metrics.spacing + width;
        if (!current.items.empty() && next_width > available_width) {
            trim_toolbar_row(current, metrics);
            if (!current.items.empty()) {
                rows.push_back(std::move(current));
            }
            current = {};
            if (separator) {
                continue;
            }
        }
        current.width = current.items.empty()
                            ? width
                            : current.width + metrics.spacing + width;
        current.items.push_back(item);
    }
    trim_toolbar_row(current, metrics);
    if (!current.items.empty()) {
        rows.push_back(std::move(current));
    }
    return rows;
}

[[nodiscard]] inline int toolbar_width(
    const std::vector<ToolbarRow>& rows,
    const ToolbarMetrics& metrics) noexcept {
    int width = 0;
    for (const auto& row : rows) {
        width = std::max(width, row.width);
    }
    return width + 2 * metrics.padding;
}

[[nodiscard]] inline int toolbar_height(
    const std::vector<ToolbarRow>& rows,
    const ToolbarMetrics& metrics) noexcept {
    if (rows.empty()) {
        return 0;
    }
    return static_cast<int>(rows.size()) * metrics.button_height +
           static_cast<int>(rows.size() - 1) * metrics.spacing +
           2 * metrics.padding;
}

[[nodiscard]] inline int clamp_toolbar_axis(
    int value,
    int size,
    int minimum,
    int maximum) noexcept {
    if (size >= maximum - minimum) {
        return minimum;
    }
    return std::clamp(value, minimum, maximum - size);
}

inline void place_toolbar_rows(
    std::vector<ToolbarButton>& target,
    const std::vector<ToolbarRow>& rows,
    const ToolbarMetrics& metrics,
    int left,
    int top) {
    target.clear();
    int y = top + metrics.padding;
    for (const auto& row : rows) {
        int x = left + metrics.padding;
        for (const auto& item : row.items) {
            const int width = toolbar_item_width(item.first, metrics);
            target.push_back({
                item.first,
                item.second,
                {x, y, x + width, y + metrics.button_height},
            });
            x += width + metrics.spacing;
        }
        y += metrics.button_height + metrics.spacing;
    }
}

struct AnnotationGeometry {
    POINT start{};
    POINT end{};
};

[[nodiscard]] inline POINT orthogonal_endpoint(
    POINT anchor,
    POINT cursor) noexcept {
    const long delta_x = cursor.x - anchor.x;
    const long delta_y = cursor.y - anchor.y;
    if (std::abs(delta_x) >= std::abs(delta_y)) {
        return {cursor.x, anchor.y};
    }
    return {anchor.x, cursor.y};
}

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

[[nodiscard]] inline RectI constrained_selection_rect(
    POINT anchor,
    POINT cursor,
    RectI bounds,
    bool constrain_square,
    bool from_center) noexcept {
    bounds = bounds.normalized();
    if (bounds.empty()) {
        return {};
    }

    anchor.x = std::clamp(
        anchor.x,
        static_cast<long>(bounds.left),
        static_cast<long>(bounds.right));
    anchor.y = std::clamp(
        anchor.y,
        static_cast<long>(bounds.top),
        static_cast<long>(bounds.bottom));
    const POINT local_anchor{
        anchor.x - bounds.left,
        anchor.y - bounds.top,
    };
    const POINT local_cursor{
        cursor.x - bounds.left,
        cursor.y - bounds.top,
    };
    const AnnotationGeometry geometry = fit_annotation_geometry_to_canvas(
        constrained_annotation_geometry(
            Tool::rectangle,
            local_anchor,
            local_cursor,
            constrain_square,
            from_center),
        local_anchor,
        bounds.width(),
        bounds.height(),
        from_center,
        constrain_square);
    return RectI{
        static_cast<int>(geometry.start.x) + bounds.left,
        static_cast<int>(geometry.start.y) + bounds.top,
        static_cast<int>(geometry.end.x) + bounds.left,
        static_cast<int>(geometry.end.y) + bounds.top,
    }.normalized();
}

[[nodiscard]] inline RectI resize_selection_from_corner(
    RectI original,
    DragMode mode,
    POINT cursor,
    RectI bounds,
    bool preserve_aspect,
    bool from_center) noexcept {
    original = original.normalized();
    bounds = bounds.normalized();
    const bool moves_left =
        mode == DragMode::top_left || mode == DragMode::bottom_left;
    const bool moves_right =
        mode == DragMode::top_right || mode == DragMode::bottom_right;
    const bool moves_top =
        mode == DragMode::top_left || mode == DragMode::top_right;
    const bool moves_bottom =
        mode == DragMode::bottom_left || mode == DragMode::bottom_right;
    if ((!moves_left && !moves_right) || (!moves_top && !moves_bottom) ||
        original.empty() || bounds.empty()) {
        return original;
    }

    POINT anchor{};
    if (from_center) {
        anchor = {
            original.left + original.width() / 2,
            original.top + original.height() / 2,
        };
    } else {
        anchor = {
            moves_left ? original.right : original.left,
            moves_top ? original.bottom : original.top,
        };
    }

    long delta_x = cursor.x - anchor.x;
    long delta_y = cursor.y - anchor.y;
    if (preserve_aspect && original.height() > 0) {
        const double aspect =
            static_cast<double>(original.width()) /
            static_cast<double>(original.height());
        long absolute_x = std::abs(delta_x);
        long absolute_y = std::abs(delta_y);
        if (static_cast<double>(absolute_x) >=
            static_cast<double>(absolute_y) * aspect) {
            absolute_y = static_cast<long>(std::lround(
                static_cast<double>(absolute_x) / aspect));
        } else {
            absolute_x = static_cast<long>(std::lround(
                static_cast<double>(absolute_y) * aspect));
        }
        const long sign_x =
            delta_x < 0 ? -1L : (delta_x > 0 ? 1L : (moves_left ? -1L : 1L));
        const long sign_y =
            delta_y < 0 ? -1L : (delta_y > 0 ? 1L : (moves_top ? -1L : 1L));
        delta_x = absolute_x * sign_x;
        delta_y = absolute_y * sign_y;
    }

    const POINT local_anchor{
        anchor.x - bounds.left,
        anchor.y - bounds.top,
    };
    AnnotationGeometry geometry{};
    if (from_center) {
        geometry = {
            {local_anchor.x - delta_x, local_anchor.y - delta_y},
            {local_anchor.x + delta_x, local_anchor.y + delta_y},
        };
    } else {
        geometry = {
            local_anchor,
            {local_anchor.x + delta_x, local_anchor.y + delta_y},
        };
    }
    geometry = fit_annotation_geometry_to_canvas(
        geometry,
        local_anchor,
        bounds.width(),
        bounds.height(),
        from_center,
        preserve_aspect);
    return RectI{
        static_cast<int>(geometry.start.x) + bounds.left,
        static_cast<int>(geometry.start.y) + bounds.top,
        static_cast<int>(geometry.end.x) + bounds.left,
        static_cast<int>(geometry.end.y) + bounds.top,
    }.normalized();
}

[[nodiscard]] inline RectI translate_selection_within_bounds(
    RectI selection,
    int delta_x,
    int delta_y,
    RectI bounds) noexcept {
    selection = selection.normalized();
    bounds = bounds.normalized();
    if (selection.empty() || bounds.empty()) {
        return selection;
    }
    const int width = selection.width();
    const int height = selection.height();
    if (width > bounds.width() || height > bounds.height()) {
        return selection;
    }
    const int left = std::clamp(
        selection.left + delta_x,
        bounds.left,
        bounds.right - width);
    const int top = std::clamp(
        selection.top + delta_y,
        bounds.top,
        bounds.bottom - height);
    return {left, top, left + width, top + height};
}

enum class SelectionResizeDirection {
    left,
    up,
    right,
    down,
};

// Mirrors Snipaste's pixel-precise selection shortcuts. Enlarging grows the
// edge in the arrow direction; shrinking moves the opposite edge toward that
// direction. A valid selection is never reduced below 2 x 2 pixels.
[[nodiscard]] inline RectI resize_selection_one_pixel(
    RectI selection,
    RectI bounds,
    SelectionResizeDirection direction,
    bool enlarge) noexcept {
    selection = selection.normalized();
    bounds = bounds.normalized();
    const std::int64_t width =
        static_cast<std::int64_t>(selection.right) - selection.left;
    const std::int64_t height =
        static_cast<std::int64_t>(selection.bottom) - selection.top;
    if (width < 2 || height < 2 ||
        selection.left < bounds.left || selection.top < bounds.top ||
        selection.right > bounds.right ||
        selection.bottom > bounds.bottom) {
        return selection;
    }

    if (enlarge) {
        switch (direction) {
            case SelectionResizeDirection::left:
                if (selection.left > bounds.left) --selection.left;
                break;
            case SelectionResizeDirection::up:
                if (selection.top > bounds.top) --selection.top;
                break;
            case SelectionResizeDirection::right:
                if (selection.right < bounds.right) ++selection.right;
                break;
            case SelectionResizeDirection::down:
                if (selection.bottom < bounds.bottom) ++selection.bottom;
                break;
        }
        return selection;
    }

    switch (direction) {
        case SelectionResizeDirection::left:
            if (width > 2) --selection.right;
            break;
        case SelectionResizeDirection::up:
            if (height > 2) --selection.bottom;
            break;
        case SelectionResizeDirection::right:
            if (width > 2) ++selection.left;
            break;
        case SelectionResizeDirection::down:
            if (height > 2) ++selection.top;
            break;
    }
    return selection;
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
        const int radius = std::max(
            2,
            static_cast<int>(
                std::ceil(annotation_visual_radius(annotation))));
        bounds.left -= radius;
        bounds.top -= radius;
        bounds.right += radius + 1;
        bounds.bottom += radius + 1;
        return bounds;
    }

    if (annotation.tool == Tool::text) {
        if (!annotation.measured_text_bounds.empty()) {
            return annotation.measured_text_bounds;
        }
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
        RectI bounds{
            annotation.start.x,
            annotation.start.y,
            annotation.start.x + width,
            annotation.start.y + height,
        };
        if (annotation.text_style == TextStyle::dark) {
            bounds.right += 8;
            bounds.bottom += 6;
        } else if (annotation.text_style == TextStyle::outline) {
            bounds.left -= 2;
            bounds.top -= 2;
            bounds.right += 2;
            bounds.bottom += 2;
        }
        return bounds;
    }

    if (annotation.tool == Tool::serial) {
        const int radius = static_cast<int>(
            std::ceil(annotation_visual_radius(annotation)));
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
            static_cast<int>(std::ceil(
                arrow_head_length(annotation.width))));
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
    if (!annotation.measured_text_bounds.empty()) {
        annotation.measured_text_bounds.left += delta_x;
        annotation.measured_text_bounds.right += delta_x;
        annotation.measured_text_bounds.top += delta_y;
        annotation.measured_text_bounds.bottom += delta_y;
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
    bool preserve_aspect = false,
    bool from_center = false) noexcept {
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
        if (from_center) {
            const long center_sum_x = original.start.x + original.end.x;
            const long center_sum_y = original.start.y + original.end.y;
            cursor.x = std::clamp(
                cursor.x,
                std::max(0L, center_sum_x - canvas_width),
                std::min(static_cast<long>(canvas_width), center_sum_x));
            cursor.y = std::clamp(
                cursor.y,
                std::max(0L, center_sum_y - canvas_height),
                std::min(static_cast<long>(canvas_height), center_sum_y));
            const POINT opposite{
                center_sum_x - cursor.x,
                center_sum_y - cursor.y,
            };
            if (std::hypot(
                    static_cast<double>(cursor.x - opposite.x),
                    static_cast<double>(cursor.y - opposite.y)) <
                static_cast<double>(kMinimumExtent)) {
                return original;
            }
            if (handle == AnnotationHandle::start_point) {
                updated.start = cursor;
                updated.end = opposite;
            } else {
                updated.start = opposite;
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

    const auto centered_axis = [kMinimumExtent](
                                   int source_minimum,
                                   int source_maximum,
                                   int safe_minimum,
                                   int safe_maximum,
                                   int requested,
                                   bool moves_minimum,
                                   bool moves_maximum,
                                   int* target_minimum,
                                   int* target_maximum) noexcept {
        if ((!moves_minimum && !moves_maximum) ||
            source_maximum - source_minimum < kMinimumExtent) {
            return true;
        }
        const int center_sum = source_minimum + source_maximum;
        if (moves_minimum) {
            const int minimum = std::max(
                safe_minimum,
                center_sum - safe_maximum);
            const int maximum =
                (center_sum - kMinimumExtent) / 2;
            if (minimum > maximum) {
                return false;
            }
            *target_minimum = std::clamp(requested, minimum, maximum);
            *target_maximum = center_sum - *target_minimum;
        } else {
            const int minimum =
                (center_sum + kMinimumExtent + 1) / 2;
            const int maximum = std::min(
                safe_maximum,
                center_sum - safe_minimum);
            if (minimum > maximum) {
                return false;
            }
            *target_maximum = std::clamp(requested, minimum, maximum);
            *target_minimum = center_sum - *target_maximum;
        }
        return true;
    };

    if (preserve_aspect && corner &&
        source.width() >= kMinimumExtent &&
        source.height() >= kMinimumExtent) {
        const int fixed_x = from_center
                                ? source.left + source.right
                                : (moves_left ? source.right : source.left);
        const int fixed_y = from_center
                                ? source.top + source.bottom
                                : (moves_top ? source.bottom : source.top);
        const int x_sign = moves_left ? -1 : 1;
        const int y_sign = moves_top ? -1 : 1;
        const int requested_width = from_center
                                        ? std::abs(
                                              static_cast<int>(cursor.x) * 2 -
                                              fixed_x)
                                        : std::abs(
                                              static_cast<int>(cursor.x) -
                                              fixed_x);
        const int requested_height = from_center
                                         ? std::abs(
                                               static_cast<int>(cursor.y) * 2 -
                                               fixed_y)
                                         : std::abs(
                                               static_cast<int>(cursor.y) -
                                               fixed_y);
        const int maximum_width = from_center
                                      ? std::min(
                                            fixed_x - safe_left * 2,
                                            safe_right * 2 - fixed_x)
                                      : (x_sign < 0
                                             ? fixed_x - safe_left
                                             : safe_right - fixed_x);
        const int maximum_height = from_center
                                       ? std::min(
                                             fixed_y - safe_top * 2,
                                             safe_bottom * 2 - fixed_y)
                                       : (y_sign < 0
                                              ? fixed_y - safe_top
                                              : safe_bottom - fixed_y);
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
        if (from_center) {
            const int requested_x = moves_left
                                        ? (fixed_x - target_width) / 2
                                        : (fixed_x + target_width + 1) / 2;
            const int requested_y = moves_top
                                        ? (fixed_y - target_height) / 2
                                        : (fixed_y + target_height + 1) / 2;
            if (!centered_axis(
                    source.left,
                    source.right,
                    safe_left,
                    safe_right,
                    requested_x,
                    moves_left,
                    moves_right,
                    &left,
                    &right) ||
                !centered_axis(
                    source.top,
                    source.bottom,
                    safe_top,
                    safe_bottom,
                    requested_y,
                    moves_top,
                    moves_bottom,
                    &top,
                    &bottom)) {
                return original;
            }
        } else {
            if (moves_left) left = fixed_x - target_width;
            else right = fixed_x + target_width;
            if (moves_top) top = fixed_y - target_height;
            else bottom = fixed_y + target_height;
        }
    } else if (from_center) {
        if (!centered_axis(
                source.left,
                source.right,
                safe_left,
                safe_right,
                static_cast<int>(cursor.x),
                moves_left,
                moves_right,
                &left,
                &right) ||
            !centered_axis(
                source.top,
                source.bottom,
                safe_top,
                safe_bottom,
                static_cast<int>(cursor.y),
                moves_top,
                moves_bottom,
                &top,
                &bottom)) {
            return original;
        }
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

[[nodiscard]] inline bool append_stroke_point_bounded(
    std::vector<POINT>& points,
    POINT point,
    double minimum_distance,
    std::size_t maximum_points = 200000) {
    maximum_points = std::max<std::size_t>(1, maximum_points);
    if (points.empty()) {
        points.push_back(point);
        return true;
    }
    if (points.size() >= maximum_points) {
        return false;
    }
    const double safe_distance =
        std::isfinite(minimum_distance)
            ? std::max(0.0, minimum_distance)
            : 0.0;
    const double delta_x =
        static_cast<double>(point.x - points.back().x);
    const double delta_y =
        static_cast<double>(point.y - points.back().y);
    if (std::hypot(delta_x, delta_y) < safe_distance) {
        return false;
    }
    points.push_back(point);
    return true;
}

[[nodiscard]] inline std::vector<POINT> smooth_polyline(
    const std::vector<POINT>& points,
    unsigned int passes = 2,
    std::size_t maximum_points = 200000) {
    if (points.size() <= 2 || passes == 0) {
        return points;
    }
    maximum_points = std::max<std::size_t>(2, maximum_points);
    std::vector<POINT> current = points;
    if (current.size() > maximum_points) {
        current.resize(maximum_points);
        current.back() = points.back();
    }
    for (unsigned int pass = 0; pass < passes; ++pass) {
        std::vector<POINT> next;
        next.reserve(current.size());
        next.push_back(current.front());
        for (std::size_t index = 1;
             index + 1 < current.size() && next.size() + 1 < maximum_points;
             ++index) {
            const POINT smoothed{
                static_cast<long>(std::lround(
                    (static_cast<double>(current[index - 1].x) +
                     2.0 * static_cast<double>(current[index].x) +
                     static_cast<double>(current[index + 1].x)) /
                    4.0)),
                static_cast<long>(std::lround(
                    (static_cast<double>(current[index - 1].y) +
                     2.0 * static_cast<double>(current[index].y) +
                     static_cast<double>(current[index + 1].y)) /
                    4.0)),
            };
            if (smoothed.x != next.back().x ||
                smoothed.y != next.back().y) {
                next.push_back(smoothed);
            }
        }
        if (current.back().x != next.back().x ||
            current.back().y != next.back().y) {
            next.push_back(current.back());
        }
        current = std::move(next);
        if (current.size() <= 2) {
            break;
        }
    }
    return current;
}

inline void normalize_annotation_stroke(Annotation& annotation) {
    if ((annotation.tool != Tool::pen &&
         annotation.tool != Tool::highlight &&
         annotation.tool != Tool::mosaic &&
         annotation.tool != Tool::blur) ||
        annotation.points.size() <= 1) {
        return;
    }
    const double spacing =
        annotation.tool == Tool::mosaic || annotation.tool == Tool::blur
            ? std::max(
                  2.0,
                  static_cast<double>(annotation.width) * 1.75)
            : (annotation.tool == Tool::pen
                   ? std::clamp(
                         static_cast<double>(annotation.width) * 0.35,
                         1.5,
                         4.0)
                   : 2.0);
    annotation.points = resample_polyline(annotation.points, spacing);
    if (annotation.tool == Tool::pen) {
        annotation.points = smooth_polyline(annotation.points, 2);
    }
    if (!annotation.points.empty()) {
        annotation.start = annotation.points.front();
        annotation.end = annotation.points.back();
    }
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
