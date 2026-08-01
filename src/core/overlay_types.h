#pragma once

#include "airshot/common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
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
    redact,
};

enum class EraserMode {
    object,
    local_stroke,
};

[[nodiscard]] constexpr bool tool_is_privacy_effect(Tool tool) noexcept {
    return tool == Tool::mosaic || tool == Tool::blur ||
           tool == Tool::redact;
}

[[nodiscard]] constexpr bool is_compact_palette_color(
    COLORREF color) noexcept {
    return color == RGB(245, 34, 45) ||
           color == RGB(250, 219, 20) ||
           color == RGB(82, 196, 26) ||
           color == RGB(0, 102, 255);
}

[[nodiscard]] constexpr bool tool_uses_bitmap_effect_preview(
    Tool tool) noexcept {
    return tool_is_privacy_effect(tool) ||
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
    const bool effect_tool = tool_is_privacy_effect(style_tool);
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
        case Tool::redact:
            return std::max(5.0F, safe_width * 3.5F);
        case Tool::highlight:
            return static_cast<float>(std::clamp(
                std::round(static_cast<double>(safe_width) * 1.6),
                4.0,
                4096.0));
        case Tool::eraser:
            // Eraser widths use the same 2 / 4 / 8 values as the other
            // tools, but those values represent size presets rather than a
            // line diameter. Keep the three presets optically distinct and
            // use this exact radius for both hit testing and the cursor.
            return std::clamp(2.0F + safe_width * 2.0F, 4.0F, 128.0F);
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
    // Text geometry is expressed in output-image pixels, never UI DIPs. A
    // zero box width keeps the legacy natural-width/no-soft-wrap behaviour;
    // a positive width enables automatic wrapping inside that logical box.
    int text_box_width_px{};
    RectI measured_text_layout_bounds{};
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
    static_cast<std::size_t>(Tool::redact) + 1;

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
        styles_[tool_style_index(Tool::redact)].color = RGB(0, 0, 0);
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

[[nodiscard]] inline double privacy_segment_distance(
    POINT point,
    POINT start,
    POINT end) noexcept {
    const double delta_x = static_cast<double>(end.x - start.x);
    const double delta_y = static_cast<double>(end.y - start.y);
    const double length_squared = delta_x * delta_x + delta_y * delta_y;
    if (length_squared <= 0.0) {
        return std::hypot(
            static_cast<double>(point.x - start.x),
            static_cast<double>(point.y - start.y));
    }
    const double projection = std::clamp(
        (static_cast<double>(point.x - start.x) * delta_x +
         static_cast<double>(point.y - start.y) * delta_y) /
            length_squared,
        0.0,
        1.0);
    return std::hypot(
        static_cast<double>(point.x) -
            (static_cast<double>(start.x) + projection * delta_x),
        static_cast<double>(point.y) -
            (static_cast<double>(start.y) + projection * delta_y));
}

// Selection and object editing use the same footprint as privacy rendering:
// a filled rectangle in box mode, or a round-capped polyline in brush mode.
// The small outer margin keeps thin objects operable without making distant
// annotations steal pointer hits.
[[nodiscard]] inline bool privacy_annotation_hit_test(
    const Annotation& annotation,
    POINT point,
    double additional_radius = 0.0) noexcept {
    if (!tool_is_privacy_effect(annotation.tool)) {
        return false;
    }
    const double safe_additional_radius =
        std::isfinite(additional_radius)
            ? std::max(0.0, additional_radius)
            : 0.0;
    if (annotation.points.empty()) {
        RectI bounds{
            annotation.start.x,
            annotation.start.y,
            annotation.end.x,
            annotation.end.y,
        };
        bounds = bounds.normalized();
        if (bounds.empty()) {
            return false;
        }
        const int margin = static_cast<int>(
            std::ceil(4.0 + safe_additional_radius));
        bounds.left -= margin;
        bounds.top -= margin;
        bounds.right += margin;
        bounds.bottom += margin;
        return bounds.contains(point);
    }

    const double threshold =
        std::max(
            8.0,
            static_cast<double>(annotation_visual_radius(annotation)) +
                4.0) +
        safe_additional_radius;
    if (annotation.points.size() == 1) {
        return privacy_segment_distance(
                   point,
                   annotation.points.front(),
                   annotation.points.front()) <= threshold;
    }
    for (std::size_t index = 1; index < annotation.points.size(); ++index) {
        if (privacy_segment_distance(
                point,
                annotation.points[index - 1],
                annotation.points[index]) <= threshold) {
            return true;
        }
    }
    return false;
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
        case Tool::redact:
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
    bool checked{};
    bool busy{};
};

inline constexpr int kToolbarButtonDip = 40;
inline constexpr int kToolbarSpacingDip = 4;
inline constexpr int kToolbarPaddingDip = 4;
inline constexpr int kToolbarIconFrameDip = 24;
inline constexpr int kToolbarGlyphDip = 20;
inline constexpr float kToolbarIconStrokeDip = 1.5F;
inline constexpr int kSubToolbarSpacingDip = 2;
inline constexpr int kSubToolbarPaddingDip = 6;
inline constexpr int kSubToolbarSliderDip = 156;
inline constexpr int kSubToolbarSliderTrackLeftDip = 32;
inline constexpr int kSubToolbarSliderTrackRightInsetDip = 36;
inline constexpr int kSubToolbarSliderHitSlopDip = 10;

[[nodiscard]] constexpr std::wstring_view toolbar_group_for_item(
    std::wstring_view id) noexcept {
    if (id == L"rect" || id == L"ellipse" || id == L"line" ||
        id == L"arrow") {
        return L"group_shape";
    }
    if (id == L"pen" || id == L"highlight") {
        return L"group_brush";
    }
    if (id == L"mosaic" || id == L"blur" || id == L"redact") {
        return L"group_privacy";
    }
    if (id == L"text" || id == L"serial" || id == L"watermark") {
        return L"group_mark";
    }
    if (id == L"pin" || id == L"ocr" || id == L"scroll" ||
        id == L"copy") {
        return L"group_capture";
    }
    return {};
}

[[nodiscard]] constexpr bool toolbar_is_group_id(
    std::wstring_view id) noexcept {
    return id == L"group_shape" || id == L"group_brush" ||
           id == L"group_privacy" || id == L"group_mark" ||
           id == L"group_capture";
}

struct ToolbarActivation {
    std::wstring id;
    bool from_sub_toolbar{};
};

// A toolbar command is armed on pointer-down and is only activated when the
// same pointer is released inside the original 40-DIP target. Keeping this
// state as pure geometry makes cancellation behaviour deterministic and easy
// to exercise without creating native windows.
struct ToolbarPressState {
    std::wstring id;
    RectI bounds;
    bool from_sub_toolbar{};
    bool pointer_inside{};

    [[nodiscard]] bool active() const noexcept { return !id.empty(); }

    bool begin(const ToolbarButton& button, bool from_sub) {
        if (!button.enabled || button.id.empty() || button.id == L"|" ||
            button.id == L"drag") {
            return false;
        }
        id = button.id;
        bounds = button.bounds;
        from_sub_toolbar = from_sub;
        pointer_inside = true;
        return true;
    }

    [[nodiscard]] bool update(POINT point) noexcept {
        if (!active()) {
            return false;
        }
        const bool next_inside = bounds.contains(point);
        const bool changed = next_inside != pointer_inside;
        pointer_inside = next_inside;
        return changed;
    }

    [[nodiscard]] std::optional<ToolbarActivation> release(POINT point) {
        if (!active()) {
            return std::nullopt;
        }
        const bool activate = bounds.contains(point);
        ToolbarActivation result{id, from_sub_toolbar};
        cancel();
        return activate ? std::optional<ToolbarActivation>{std::move(result)}
                        : std::nullopt;
    }

    void cancel() noexcept {
        id.clear();
        bounds = {};
        from_sub_toolbar = false;
        pointer_inside = false;
    }
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

struct ToolbarSingleRowPlan {
    ToolbarRow row;
    std::vector<std::pair<std::wstring, std::wstring>> overflow;
};

[[nodiscard]] inline std::vector<std::pair<std::wstring, std::wstring>>
collapse_toolbar_groups(
    const std::vector<std::pair<std::wstring, std::wstring>>& ordered_items) {
    constexpr std::array<std::wstring_view, 5> groups{
        L"group_shape",
        L"group_brush",
        L"group_privacy",
        L"group_mark",
        L"group_capture",
    };
    std::array<std::size_t, groups.size()> counts{};
    for (const auto& item : ordered_items) {
        const std::wstring_view group = toolbar_group_for_item(item.first);
        const auto found = std::ranges::find(groups, group);
        if (found != groups.end()) {
            ++counts[static_cast<std::size_t>(found - groups.begin())];
        }
    }

    std::array<bool, groups.size()> emitted{};
    std::vector<std::pair<std::wstring, std::wstring>> collapsed;
    collapsed.reserve(ordered_items.size());
    for (const auto& item : ordered_items) {
        const std::wstring_view group = toolbar_group_for_item(item.first);
        const auto found = std::ranges::find(groups, group);
        if (found == groups.end()) {
            collapsed.push_back(item);
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(found - groups.begin());
        if (counts[index] == 1) {
            collapsed.push_back(item);
        } else if (!emitted[index]) {
            collapsed.push_back({std::wstring(group), {}});
            emitted[index] = true;
        }
    }
    return collapsed;
}

[[nodiscard]] inline int toolbar_item_width(
    std::wstring_view id,
    const ToolbarMetrics& metrics) noexcept {
    const OverlayUiMetrics ui{metrics.dpi};
    if (id == L"drag") return ui.px(20);
    if (id == L"|") return ui.px(9);
    if (id == L"text_size_btn") return ui.px(78);
    if (id == L"mosaic_strength_slider" ||
        id == L"watermark_opacity_slider") {
        return ui.px(kSubToolbarSliderDip);
    }
    if (id == L"watermark_text") return ui.px(112);
    if (id == L"watermark_apply" || id == L"watermark_clear") {
        return metrics.button_width;
    }
    if (id.starts_with(L"effect_") || id.starts_with(L"mode_")) {
        return metrics.button_width;
    }
    if (id.starts_with(L"fill_") || id.starts_with(L"stroke_") ||
        id.starts_with(L"corner_") || id.starts_with(L"head_")) {
        return metrics.button_width;
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

// The main capture toolbar is deliberately different from the contextual
// property toolbar: it never wraps. Optional middle commands move to a stable
// overflow menu while the leading handle and trailing completion commands stay
// anchored. The caller supplies already-grouped middle commands so saved user
// ordering remains the source of truth.
[[nodiscard]] inline ToolbarSingleRowPlan fit_toolbar_single_row(
    const std::vector<std::pair<std::wstring, std::wstring>>& leading,
    const std::vector<std::pair<std::wstring, std::wstring>>& middle,
    const std::vector<std::pair<std::wstring, std::wstring>>& trailing,
    const ToolbarMetrics& metrics,
    const RectI& bounds,
    std::pair<std::wstring, std::wstring> overflow_button = {L"more", L""}) {
    const int available_width = std::max(
        metrics.button_width,
        bounds.width() - 2 * metrics.padding);

    const auto compose = [&](std::size_t middle_count,
                             bool include_overflow) {
        ToolbarRow row;
        row.items = leading;
        row.items.insert(
            row.items.end(),
            middle.begin(),
            middle.begin() + static_cast<std::ptrdiff_t>(middle_count));
        if (include_overflow) {
            row.items.push_back(overflow_button);
        }
        if (!trailing.empty()) {
            if (!row.items.empty() && row.items.back().first != L"|") {
                row.items.push_back({L"|", L""});
            }
            row.items.insert(row.items.end(), trailing.begin(), trailing.end());
        }
        trim_toolbar_row(row, metrics);
        return row;
    };

    ToolbarSingleRowPlan plan;
    plan.row = compose(middle.size(), false);
    if (plan.row.width <= available_width || middle.empty()) {
        return plan;
    }

    std::size_t visible_middle = 0;
    ToolbarRow best = compose(0, true);
    for (std::size_t count = 1; count <= middle.size(); ++count) {
        ToolbarRow candidate = compose(count, count < middle.size());
        if (candidate.width > available_width) {
            break;
        }
        visible_middle = count;
        best = std::move(candidate);
    }

    // The overflow button is mandatory whenever any middle command is hidden.
    // If even the fixed controls exceed a synthetic tiny monitor, keep the
    // fixed one-row composition intact rather than silently dropping actions.
    if (visible_middle == middle.size()) {
        plan.row = compose(middle.size(), false);
        return plan;
    }
    plan.row = std::move(best);
    plan.overflow.assign(
        middle.begin() + static_cast<std::ptrdiff_t>(visible_middle),
        middle.end());
    return plan;
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
    bool from_center,
    double locked_aspect_ratio = 0.0) noexcept {
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
            std::isfinite(locked_aspect_ratio) && locked_aspect_ratio > 0.0
                ? locked_aspect_ratio
                : static_cast<double>(original.width()) /
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

// Resizes one edge while keeping the orthogonal axis centred. This is used by
// the persistent aspect-ratio lock: unlike a transient Shift gesture, the lock
// must also remain effective on side handles and pixel-precision keyboard
// edits. The result is always clipped to the desktop and never becomes smaller
// than 2 x 2 pixels.
[[nodiscard]] inline RectI resize_selection_from_edge(
    RectI original,
    DragMode mode,
    POINT cursor,
    RectI bounds,
    bool preserve_aspect,
    double locked_aspect_ratio = 0.0) noexcept {
    original = original.normalized();
    bounds = bounds.normalized();
    if (original.width() < 2 || original.height() < 2 || bounds.empty() ||
        original.left < bounds.left || original.top < bounds.top ||
        original.right > bounds.right || original.bottom > bounds.bottom) {
        return original;
    }

    const bool horizontal =
        mode == DragMode::left || mode == DragMode::right;
    const bool vertical = mode == DragMode::top || mode == DragMode::bottom;
    if (!horizontal && !vertical) {
        return original;
    }

    if (!preserve_aspect) {
        switch (mode) {
            case DragMode::left:
                original.left = std::clamp(
                    static_cast<int>(cursor.x),
                    bounds.left,
                    original.right - 2);
                break;
            case DragMode::right:
                original.right = std::clamp(
                    static_cast<int>(cursor.x),
                    original.left + 2,
                    bounds.right);
                break;
            case DragMode::top:
                original.top = std::clamp(
                    static_cast<int>(cursor.y),
                    bounds.top,
                    original.bottom - 2);
                break;
            case DragMode::bottom:
                original.bottom = std::clamp(
                    static_cast<int>(cursor.y),
                    original.top + 2,
                    bounds.bottom);
                break;
            default:
                break;
        }
        return original;
    }

    const double aspect =
        std::isfinite(locked_aspect_ratio) && locked_aspect_ratio > 0.0
            ? locked_aspect_ratio
            : static_cast<double>(original.width()) /
                  static_cast<double>(original.height());
    if (!std::isfinite(aspect) || aspect <= 0.0) {
        return original;
    }

    int requested_width = original.width();
    int requested_height = original.height();
    int maximum_width = bounds.width();
    int maximum_height = bounds.height();
    if (mode == DragMode::left) {
        requested_width = original.right - static_cast<int>(cursor.x);
        maximum_width = original.right - bounds.left;
    } else if (mode == DragMode::right) {
        requested_width = static_cast<int>(cursor.x) - original.left;
        maximum_width = bounds.right - original.left;
    } else if (mode == DragMode::top) {
        requested_height = original.bottom - static_cast<int>(cursor.y);
        maximum_height = original.bottom - bounds.top;
    } else {
        requested_height = static_cast<int>(cursor.y) - original.top;
        maximum_height = bounds.bottom - original.top;
    }

    auto fit_from_width = [&](int width) {
        int fitted_width = std::clamp(width, 2, maximum_width);
        int fitted_height = std::max(
            2,
            static_cast<int>(std::lround(
                static_cast<double>(fitted_width) / aspect)));
        if (fitted_height > maximum_height) {
            fitted_height = maximum_height;
            fitted_width = std::max(
                2,
                static_cast<int>(std::lround(
                    static_cast<double>(fitted_height) * aspect)));
        }
        if (fitted_width > maximum_width) {
            fitted_width = maximum_width;
            fitted_height = std::max(
                2,
                static_cast<int>(std::lround(
                    static_cast<double>(fitted_width) / aspect)));
        }
        return std::pair{
            std::clamp(fitted_width, 2, maximum_width),
            std::clamp(fitted_height, 2, maximum_height)};
    };
    auto fit_from_height = [&](int height) {
        int fitted_height = std::clamp(height, 2, maximum_height);
        int fitted_width = std::max(
            2,
            static_cast<int>(std::lround(
                static_cast<double>(fitted_height) * aspect)));
        if (fitted_width > maximum_width) {
            fitted_width = maximum_width;
            fitted_height = std::max(
                2,
                static_cast<int>(std::lround(
                    static_cast<double>(fitted_width) / aspect)));
        }
        if (fitted_height > maximum_height) {
            fitted_height = maximum_height;
            fitted_width = std::max(
                2,
                static_cast<int>(std::lround(
                    static_cast<double>(fitted_height) * aspect)));
        }
        return std::pair{
            std::clamp(fitted_width, 2, maximum_width),
            std::clamp(fitted_height, 2, maximum_height)};
    };

    const auto [width, height] = horizontal
        ? fit_from_width(requested_width)
        : fit_from_height(requested_height);
    const int center_x = original.left + original.width() / 2;
    const int center_y = original.top + original.height() / 2;
    int left = std::clamp(
        center_x - width / 2,
        bounds.left,
        bounds.right - width);
    int top = std::clamp(
        center_y - height / 2,
        bounds.top,
        bounds.bottom - height);
    if (mode == DragMode::left) {
        left = original.right - width;
    } else if (mode == DragMode::right) {
        left = original.left;
    } else if (mode == DragMode::top) {
        top = original.bottom - height;
    } else if (mode == DragMode::bottom) {
        top = original.top;
    }
    return {left, top, left + width, top + height};
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
        const int natural_width = std::max(
            24,
            static_cast<int>(std::ceil(
                std::max(1.0, longest_line) *
                std::max(1.0F, annotation.width) * 0.72F)));
        const int width = annotation.text_box_width_px > 0
                              ? std::max(1, annotation.text_box_width_px)
                              : natural_width;
        if (annotation.text_box_width_px > 0 && natural_width > width) {
            line_count = std::max<std::size_t>(
                line_count,
                static_cast<std::size_t>(
                    (natural_width + width - 1) / width));
        }
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
    if (annotation.tool == Tool::text) {
        return !annotation.measured_text_layout_bounds.empty()
                   ? annotation.measured_text_layout_bounds
                   : annotation_bounds(annotation);
    }
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
        case Tool::redact:
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
        case Tool::redact:
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
    if (annotation.tool == Tool::text) {
        const RectI bounds = annotation_control_bounds(annotation);
        if (bounds.width() < 2 || bounds.height() < 2) {
            return handles;
        }
        const long middle_y =
            bounds.top + (bounds.bottom - bounds.top) / 2;
        handles.push_unique(
            AnnotationHandle::top_left,
            {bounds.left, bounds.top});
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
            AnnotationHandle::bottom_left,
            {bounds.left, bounds.bottom});
        handles.push_unique(
            AnnotationHandle::left,
            {bounds.left, middle_y});
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
    if (!annotation.measured_text_layout_bounds.empty()) {
        annotation.measured_text_layout_bounds.left += delta_x;
        annotation.measured_text_layout_bounds.right += delta_x;
        annotation.measured_text_layout_bounds.top += delta_y;
        annotation.measured_text_layout_bounds.bottom += delta_y;
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
        (first.tool == Tool::text &&
         (first.width != second.width ||
          first.text_box_width_px != second.text_box_width_px)) ||
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

inline constexpr float kMinimumTextSizePx = 12.0F;
inline constexpr float kMaximumTextSizePx = 96.0F;

[[nodiscard]] constexpr UINT text_draw_flags(
    int text_box_width_px,
    bool calculate = false) noexcept {
    UINT flags = DT_LEFT | DT_TOP | DT_NOPREFIX;
    if (text_box_width_px > 0) {
        flags |= DT_WORDBREAK | DT_EDITCONTROL;
    }
    if (calculate) {
        flags |= DT_CALCRECT;
    }
    return flags;
}

[[nodiscard]] inline std::wstring text_size_label_px(float value) {
    const float safe = std::isfinite(value)
                           ? std::clamp(
                                 value,
                                 kMinimumTextSizePx,
                                 kMaximumTextSizePx)
                           : 18.0F;
    return std::to_wstring(static_cast<int>(std::lround(safe))) + L"px";
}

[[nodiscard]] inline int minimum_text_box_width_px(
    float font_size_px) noexcept {
    const float safe_size = std::isfinite(font_size_px)
                                ? std::clamp(
                                      font_size_px,
                                      kMinimumTextSizePx,
                                      kMaximumTextSizePx)
                                : 18.0F;
    return std::max(
        24,
        static_cast<int>(std::ceil(safe_size * 2.0F)));
}

struct TextAnnotationResizePlan {
    bool valid{};
    POINT start{};
    float font_size_px{18.0F};
    int box_width_px{};
    POINT fixed_anchor{};
    bool anchor_right{};
    bool anchor_bottom{};
};

[[nodiscard]] inline POINT text_resize_anchor_delta(
    const TextAnnotationResizePlan& plan,
    RectI measured_layout_bounds) noexcept {
    if (!plan.valid || measured_layout_bounds.empty()) {
        return {};
    }
    const POINT actual{
        plan.anchor_right
            ? measured_layout_bounds.right
            : measured_layout_bounds.left,
        plan.anchor_bottom
            ? measured_layout_bounds.bottom
            : measured_layout_bounds.top,
    };
    return {
        plan.fixed_anchor.x - actual.x,
        plan.fixed_anchor.y - actual.y,
    };
}

[[nodiscard]] inline TextAnnotationResizePlan
plan_text_annotation_resize(
    const Annotation& original,
    AnnotationHandle handle,
    POINT cursor,
    int canvas_width,
    int canvas_height) noexcept {
    TextAnnotationResizePlan plan;
    if (original.tool != Tool::text ||
        canvas_width <= 0 || canvas_height <= 0) {
        return plan;
    }
    const bool left_edge =
        handle == AnnotationHandle::left ||
        handle == AnnotationHandle::top_left ||
        handle == AnnotationHandle::bottom_left;
    const bool right_edge =
        handle == AnnotationHandle::right ||
        handle == AnnotationHandle::top_right ||
        handle == AnnotationHandle::bottom_right;
    const bool top_edge =
        handle == AnnotationHandle::top_left ||
        handle == AnnotationHandle::top_right;
    const bool bottom_edge =
        handle == AnnotationHandle::bottom_left ||
        handle == AnnotationHandle::bottom_right;
    const bool corner =
        (left_edge || right_edge) && (top_edge || bottom_edge);
    if ((!left_edge && !right_edge) ||
        (!corner && handle != AnnotationHandle::left &&
         handle != AnnotationHandle::right)) {
        return plan;
    }

    const RectI source = annotation_control_bounds(original);
    if (source.width() < 2 || source.height() < 2) {
        return plan;
    }
    cursor.x = std::clamp(
        cursor.x, 0L, static_cast<long>(canvas_width));
    cursor.y = std::clamp(
        cursor.y, 0L, static_cast<long>(canvas_height));
    const float original_size = std::isfinite(original.width)
                                    ? std::clamp(
                                          original.width,
                                          kMinimumTextSizePx,
                                          kMaximumTextSizePx)
                                    : 18.0F;

    plan.valid = true;
    plan.start = original.start;
    plan.font_size_px = original_size;
    if (!corner) {
        const int fixed_x = left_edge ? source.right : source.left;
        const int minimum_width = minimum_text_box_width_px(original_size);
        const int available = left_edge ? fixed_x : canvas_width - fixed_x;
        if (available < minimum_width) {
            return {};
        }
        plan.box_width_px = std::clamp(
            std::abs(static_cast<int>(cursor.x) - fixed_x),
            minimum_width,
            available);
        plan.start.x = left_edge
                           ? fixed_x - plan.box_width_px
                           : fixed_x;
        plan.fixed_anchor = {fixed_x, source.top};
        plan.anchor_right = left_edge;
        plan.anchor_bottom = false;
        return plan;
    }

    const int fixed_x = left_edge ? source.right : source.left;
    const int fixed_y = top_edge ? source.bottom : source.top;
    const int horizontal_space = left_edge
                                     ? fixed_x
                                     : canvas_width - fixed_x;
    const int vertical_space = top_edge
                                   ? fixed_y
                                   : canvas_height - fixed_y;
    const int base_width = original.text_box_width_px > 0
                               ? original.text_box_width_px
                               : source.width();
    const double source_width = std::max(1, base_width);
    const double source_height = std::max(1, source.height());
    const double requested_width =
        std::abs(static_cast<double>(cursor.x - fixed_x));
    const double requested_height =
        std::abs(static_cast<double>(cursor.y - fixed_y));
    const double denominator =
        source_width * source_width + source_height * source_height;
    const double requested_scale = denominator > 0.0
                                       ? (requested_width * source_width +
                                          requested_height * source_height) /
                                             denominator
                                       : 1.0;
    const double minimum_scale = std::max(
        static_cast<double>(kMinimumTextSizePx / original_size),
        static_cast<double>(minimum_text_box_width_px(kMinimumTextSizePx)) /
            source_width);
    const double maximum_scale = std::min({
        static_cast<double>(kMaximumTextSizePx / original_size),
        static_cast<double>(std::max(0, horizontal_space)) / source_width,
        static_cast<double>(std::max(0, vertical_space)) / source_height,
    });
    if (!std::isfinite(requested_scale) || maximum_scale < minimum_scale) {
        return {};
    }
    const double scale = std::clamp(
        requested_scale,
        minimum_scale,
        maximum_scale);
    plan.font_size_px = std::clamp(
        static_cast<float>(std::lround(original_size * scale)),
        kMinimumTextSizePx,
        kMaximumTextSizePx);
    const double effective_scale = plan.font_size_px / original_size;
    const int minimum_box_width =
        minimum_text_box_width_px(plan.font_size_px);
    if (horizontal_space < minimum_box_width) {
        return {};
    }
    plan.box_width_px = std::clamp(
        static_cast<int>(std::lround(source_width * effective_scale)),
        minimum_box_width,
        horizontal_space);
    const int target_height = std::max(
        1,
        static_cast<int>(std::lround(source_height * effective_scale)));
    plan.start = {
        left_edge ? fixed_x - plan.box_width_px : fixed_x,
        top_edge ? fixed_y - target_height : fixed_y,
    };
    plan.fixed_anchor = {fixed_x, fixed_y};
    plan.anchor_right = left_edge;
    plan.anchor_bottom = top_edge;
    return plan;
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
         annotation.tool != Tool::blur &&
         annotation.tool != Tool::redact) ||
        annotation.points.size() <= 1) {
        return;
    }
    const double spacing =
        tool_is_privacy_effect(annotation.tool)
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

[[nodiscard]] constexpr bool annotation_supports_local_erase(
    const Annotation& annotation) noexcept {
    return !annotation.points.empty() &&
           (annotation.tool == Tool::pen ||
            annotation.tool == Tool::highlight ||
            annotation.tool == Tool::mosaic ||
            annotation.tool == Tool::blur ||
            annotation.tool == Tool::redact);
}

inline constexpr std::size_t kMaximumEraserInputPoints = 200000;
inline constexpr std::size_t kMaximumEraserOutputPoints = 200000;
inline constexpr std::size_t kMaximumEraserFragments = 4096;
inline constexpr std::size_t kMaximumEraserDocumentFragments = 8192;
inline constexpr std::size_t kMaximumEraserDocumentAnnotations = 65536;

namespace eraser_detail {

struct PointD {
    double x{};
    double y{};
};

[[nodiscard]] inline PointD to_point_d(POINT point) noexcept {
    return {
        static_cast<double>(point.x),
        static_cast<double>(point.y),
    };
}

[[nodiscard]] inline PointD interpolate(
    PointD start,
    PointD end,
    double amount) noexcept {
    return {
        start.x + (end.x - start.x) * amount,
        start.y + (end.y - start.y) * amount,
    };
}

[[nodiscard]] inline POINT rounded_point(PointD point) noexcept {
    return {
        static_cast<long>(std::lround(point.x)),
        static_cast<long>(std::lround(point.y)),
    };
}

inline void append_unique_point(
    std::vector<POINT>& points,
    POINT point) {
    if (points.empty() || points.back().x != point.x ||
        points.back().y != point.y) {
        points.push_back(point);
    }
}

[[nodiscard]] inline double point_segment_distance(
    PointD point,
    PointD start,
    PointD end) noexcept {
    const double delta_x = end.x - start.x;
    const double delta_y = end.y - start.y;
    const double length_squared =
        delta_x * delta_x + delta_y * delta_y;
    if (length_squared <= 1.0e-12) {
        return std::hypot(point.x - start.x, point.y - start.y);
    }
    const double projection = std::clamp(
        ((point.x - start.x) * delta_x +
         (point.y - start.y) * delta_y) /
            length_squared,
        0.0,
        1.0);
    return std::hypot(
        point.x - (start.x + projection * delta_x),
        point.y - (start.y + projection * delta_y));
}

[[nodiscard]] inline double cross(
    PointD first,
    PointD second,
    PointD third) noexcept {
    return (second.x - first.x) * (third.y - first.y) -
           (second.y - first.y) * (third.x - first.x);
}

[[nodiscard]] inline bool within_segment_bounds(
    PointD point,
    PointD start,
    PointD end) noexcept {
    constexpr double epsilon = 1.0e-9;
    return point.x >= std::min(start.x, end.x) - epsilon &&
           point.x <= std::max(start.x, end.x) + epsilon &&
           point.y >= std::min(start.y, end.y) - epsilon &&
           point.y <= std::max(start.y, end.y) + epsilon;
}

[[nodiscard]] inline bool segments_intersect(
    PointD first_start,
    PointD first_end,
    PointD second_start,
    PointD second_end) noexcept {
    constexpr double epsilon = 1.0e-9;
    const double first_a = cross(first_start, first_end, second_start);
    const double first_b = cross(first_start, first_end, second_end);
    const double second_a = cross(second_start, second_end, first_start);
    const double second_b = cross(second_start, second_end, first_end);
    if (((first_a > epsilon && first_b < -epsilon) ||
         (first_a < -epsilon && first_b > epsilon)) &&
        ((second_a > epsilon && second_b < -epsilon) ||
         (second_a < -epsilon && second_b > epsilon))) {
        return true;
    }
    return (std::abs(first_a) <= epsilon &&
            within_segment_bounds(second_start, first_start, first_end)) ||
           (std::abs(first_b) <= epsilon &&
            within_segment_bounds(second_end, first_start, first_end)) ||
           (std::abs(second_a) <= epsilon &&
            within_segment_bounds(first_start, second_start, second_end)) ||
           (std::abs(second_b) <= epsilon &&
            within_segment_bounds(first_end, second_start, second_end));
}

[[nodiscard]] inline double segment_segment_distance(
    PointD first_start,
    PointD first_end,
    PointD second_start,
    PointD second_end) noexcept {
    if (segments_intersect(
            first_start,
            first_end,
            second_start,
            second_end)) {
        return 0.0;
    }
    return std::min({
        point_segment_distance(first_start, second_start, second_end),
        point_segment_distance(first_end, second_start, second_end),
        point_segment_distance(second_start, first_start, first_end),
        point_segment_distance(second_end, first_start, first_end),
    });
}

[[nodiscard]] inline bool point_in_capsule(
    PointD point,
    PointD sweep_start,
    PointD sweep_end,
    double radius) noexcept {
    return point_segment_distance(point, sweep_start, sweep_end) <=
           radius + 1.0e-9;
}

inline void append_breakpoint(
    std::vector<double>& breakpoints,
    double value) {
    constexpr double epsilon = 1.0e-10;
    if (!std::isfinite(value) || value < -epsilon ||
        value > 1.0 + epsilon) {
        return;
    }
    breakpoints.push_back(std::clamp(value, 0.0, 1.0));
}

inline void append_circle_breakpoints(
    std::vector<double>& breakpoints,
    PointD segment_start,
    PointD segment_end,
    PointD center,
    double radius) {
    const double delta_x = segment_end.x - segment_start.x;
    const double delta_y = segment_end.y - segment_start.y;
    const double relative_x = segment_start.x - center.x;
    const double relative_y = segment_start.y - center.y;
    const double quadratic = delta_x * delta_x + delta_y * delta_y;
    if (quadratic <= 1.0e-12) {
        return;
    }
    const double linear =
        2.0 * (relative_x * delta_x + relative_y * delta_y);
    const double constant =
        relative_x * relative_x + relative_y * relative_y -
        radius * radius;
    double discriminant =
        linear * linear - 4.0 * quadratic * constant;
    if (discriminant < -1.0e-9) {
        return;
    }
    discriminant = std::max(0.0, discriminant);
    const double root = std::sqrt(discriminant);
    append_breakpoint(
        breakpoints,
        (-linear - root) / (2.0 * quadratic));
    append_breakpoint(
        breakpoints,
        (-linear + root) / (2.0 * quadratic));
}

// Returns every parameter at which a source segment can enter or leave the
// swept eraser capsule. Extra candidates from the capsule's primitive
// boundaries are harmless; midpoint classification determines the retained
// intervals and keeps tangent contact stable.
[[nodiscard]] inline std::vector<double> capsule_breakpoints(
    PointD segment_start,
    PointD segment_end,
    PointD sweep_start,
    PointD sweep_end,
    double radius) {
    std::vector<double> breakpoints{0.0, 1.0};
    append_circle_breakpoints(
        breakpoints,
        segment_start,
        segment_end,
        sweep_start,
        radius);

    const double sweep_delta_x = sweep_end.x - sweep_start.x;
    const double sweep_delta_y = sweep_end.y - sweep_start.y;
    const double sweep_length =
        std::hypot(sweep_delta_x, sweep_delta_y);
    if (sweep_length > 1.0e-9) {
        append_circle_breakpoints(
            breakpoints,
            segment_start,
            segment_end,
            sweep_end,
            radius);
        const double unit_x = sweep_delta_x / sweep_length;
        const double unit_y = sweep_delta_y / sweep_length;
        const auto local_coordinates = [&](PointD point) {
            const double relative_x = point.x - sweep_start.x;
            const double relative_y = point.y - sweep_start.y;
            return PointD{
                relative_x * unit_x + relative_y * unit_y,
                -relative_x * unit_y + relative_y * unit_x,
            };
        };
        const PointD local_start = local_coordinates(segment_start);
        const PointD local_end = local_coordinates(segment_end);
        const double local_delta_x = local_end.x - local_start.x;
        const double local_delta_y = local_end.y - local_start.y;
        if (std::abs(local_delta_x) > 1.0e-12) {
            append_breakpoint(
                breakpoints,
                -local_start.x / local_delta_x);
            append_breakpoint(
                breakpoints,
                (sweep_length - local_start.x) / local_delta_x);
        }
        if (std::abs(local_delta_y) > 1.0e-12) {
            append_breakpoint(
                breakpoints,
                (-radius - local_start.y) / local_delta_y);
            append_breakpoint(
                breakpoints,
                (radius - local_start.y) / local_delta_y);
        }
    }

    std::ranges::sort(breakpoints);
    breakpoints.erase(
        std::unique(
            breakpoints.begin(),
            breakpoints.end(),
            [](double first, double second) {
                return std::abs(first - second) <= 1.0e-9;
            }),
        breakpoints.end());
    return breakpoints;
}

[[nodiscard]] inline bool sweep_hits_expanded_rect(
    PointD sweep_start,
    PointD sweep_end,
    RectI bounds,
    double radius) noexcept {
    bounds = bounds.normalized();
    if (bounds.empty()) {
        return false;
    }
    const double left = static_cast<double>(bounds.left) - radius;
    const double top = static_cast<double>(bounds.top) - radius;
    const double right = static_cast<double>(bounds.right) + radius;
    const double bottom = static_cast<double>(bounds.bottom) + radius;
    const auto inside = [&](PointD point) {
        return point.x >= left && point.x <= right &&
               point.y >= top && point.y <= bottom;
    };
    if (inside(sweep_start) || inside(sweep_end)) {
        return true;
    }
    const PointD top_left{left, top};
    const PointD top_right{right, top};
    const PointD bottom_right{right, bottom};
    const PointD bottom_left{left, bottom};
    return segments_intersect(
               sweep_start, sweep_end, top_left, top_right) ||
           segments_intersect(
               sweep_start, sweep_end, top_right, bottom_right) ||
           segments_intersect(
               sweep_start, sweep_end, bottom_right, bottom_left) ||
           segments_intersect(
               sweep_start, sweep_end, bottom_left, top_left);
}

}  // namespace eraser_detail

struct LocalEraseResult {
    // Fragments are populated only when changed is true. Callers retain the
    // source annotation themselves on a miss, avoiding a deep copy of points
    // for every WM_MOUSEMOVE.
    bool changed{};
    std::vector<Annotation> fragments;
};

// annotation_bounds already includes the rendered stroke radius. Expanding
// the swept-segment AABB by the eraser radius provides a cheap rejection
// before the more expensive per-segment capsule clipping.
[[nodiscard]] inline bool eraser_sweep_overlaps_annotation_bounds(
    const Annotation& annotation,
    POINT sweep_start,
    POINT sweep_end,
    double eraser_radius) noexcept {
    const RectI bounds = annotation_bounds(annotation).normalized();
    if (bounds.empty()) {
        return false;
    }
    const double safe_radius =
        std::isfinite(eraser_radius)
            ? std::max(0.0, eraser_radius)
            : 0.0;
    const double sweep_left =
        static_cast<double>(std::min(sweep_start.x, sweep_end.x)) -
        safe_radius;
    const double sweep_top =
        static_cast<double>(std::min(sweep_start.y, sweep_end.y)) -
        safe_radius;
    const double sweep_right =
        static_cast<double>(std::max(sweep_start.x, sweep_end.x)) +
        safe_radius;
    const double sweep_bottom =
        static_cast<double>(std::max(sweep_start.y, sweep_end.y)) +
        safe_radius;
    return sweep_right >= static_cast<double>(bounds.left) &&
           sweep_bottom >= static_cast<double>(bounds.top) &&
           sweep_left <= static_cast<double>(bounds.right) &&
           sweep_top <= static_cast<double>(bounds.bottom);
}

// Clips the centerline of a freehand annotation against the eraser's swept
// circle. The annotation's rendered radius is added to the eraser radius, so
// the visible stroke and the cursor agree. Retained fragments are independent
// valid annotations with all visual/style properties copied from the source.
[[nodiscard]] inline LocalEraseResult split_annotation_by_eraser_sweep(
    const Annotation& annotation,
    POINT sweep_start,
    POINT sweep_end,
    double eraser_radius,
    bool bounds_already_checked = false) {
    LocalEraseResult result;
    if (!annotation_supports_local_erase(annotation)) {
        return result;
    }
    if (annotation.points.size() > kMaximumEraserInputPoints) {
        // UI-created strokes are already bounded to this same limit. Refuse
        // to expand a malformed/external annotation beyond the product cap;
        // object-delete mode remains available for recovery.
        return result;
    }
    if (!bounds_already_checked &&
        !eraser_sweep_overlaps_annotation_bounds(
            annotation,
            sweep_start,
            sweep_end,
            eraser_radius)) {
        return result;
    }

    const double safe_eraser_radius =
        std::isfinite(eraser_radius)
            ? std::max(0.0, eraser_radius)
            : 0.0;
    const double combined_radius =
        safe_eraser_radius +
        std::max(
            0.0,
            static_cast<double>(annotation_visual_radius(annotation)));
    const eraser_detail::PointD eraser_start =
        eraser_detail::to_point_d(sweep_start);
    const eraser_detail::PointD eraser_end =
        eraser_detail::to_point_d(sweep_end);

    if (annotation.points.size() == 1) {
        if (eraser_detail::point_in_capsule(
                eraser_detail::to_point_d(annotation.points.front()),
                eraser_start,
                eraser_end,
                combined_radius)) {
            result.changed = true;
        }
        return result;
    }

    // Detect a real erased interval before materializing any retained POINT
    // arrays. Tangent/near-miss WM_MOUSEMOVE events therefore perform geometry
    // only and never deep-copy the source stroke.
    bool has_erased_interval = false;
    for (std::size_t index = 1;
         index < annotation.points.size() && !has_erased_interval;
         ++index) {
        const eraser_detail::PointD source_start =
            eraser_detail::to_point_d(annotation.points[index - 1]);
        const eraser_detail::PointD source_end =
            eraser_detail::to_point_d(annotation.points[index]);
        const auto breakpoints = eraser_detail::capsule_breakpoints(
            source_start,
            source_end,
            eraser_start,
            eraser_end,
            combined_radius);
        for (std::size_t part = 1; part < breakpoints.size(); ++part) {
            const double first = breakpoints[part - 1];
            const double second = breakpoints[part];
            if (second - first <= 1.0e-10) {
                continue;
            }
            const eraser_detail::PointD midpoint =
                eraser_detail::interpolate(
                    source_start,
                    source_end,
                    (first + second) * 0.5);
            if (eraser_detail::point_in_capsule(
                    midpoint,
                    eraser_start,
                    eraser_end,
                    combined_radius)) {
                has_erased_interval = true;
                break;
            }
        }
    }
    if (!has_erased_interval) {
        return result;
    }

    std::vector<std::vector<POINT>> retained_point_sets;
    retained_point_sets.reserve(std::min(
        annotation.points.size(),
        kMaximumEraserFragments));
    std::vector<POINT> current_points;
    std::size_t retained_point_count = 0;
    bool limit_exceeded = false;
    const auto finish_fragment = [&] {
        if (!current_points.empty()) {
            if (retained_point_sets.size() >=
                    kMaximumEraserFragments ||
                current_points.size() >
                    kMaximumEraserOutputPoints -
                        std::min(
                            retained_point_count,
                            kMaximumEraserOutputPoints)) {
                limit_exceeded = true;
                current_points.clear();
                return;
            }
            retained_point_count += current_points.size();
            retained_point_sets.push_back(
                std::move(current_points));
            current_points.clear();
        }
    };

    bool removed_positive_interval = false;
    for (std::size_t index = 1; index < annotation.points.size(); ++index) {
        const eraser_detail::PointD source_start =
            eraser_detail::to_point_d(annotation.points[index - 1]);
        const eraser_detail::PointD source_end =
            eraser_detail::to_point_d(annotation.points[index]);
        const auto breakpoints = eraser_detail::capsule_breakpoints(
            source_start,
            source_end,
            eraser_start,
            eraser_end,
            combined_radius);
        for (std::size_t part = 1; part < breakpoints.size(); ++part) {
            if (limit_exceeded) {
                break;
            }
            const double first = breakpoints[part - 1];
            const double second = breakpoints[part];
            if (second - first <= 1.0e-10) {
                continue;
            }
            const eraser_detail::PointD midpoint =
                eraser_detail::interpolate(
                    source_start,
                    source_end,
                    (first + second) * 0.5);
            if (eraser_detail::point_in_capsule(
                    midpoint,
                    eraser_start,
                    eraser_end,
                    combined_radius)) {
                removed_positive_interval = true;
                finish_fragment();
                continue;
            }

            eraser_detail::append_unique_point(
                current_points,
                eraser_detail::rounded_point(
                    eraser_detail::interpolate(
                        source_start,
                        source_end,
                        first)));
            eraser_detail::append_unique_point(
                current_points,
                eraser_detail::rounded_point(
                    eraser_detail::interpolate(
                        source_start,
                        source_end,
                        second)));
            if (current_points.size() > kMaximumEraserOutputPoints) {
                limit_exceeded = true;
            }
        }
        if (limit_exceeded) {
            break;
        }
    }
    if (!limit_exceeded) {
        finish_fragment();
    }

    if (limit_exceeded) {
        // A pathologically alternating stroke could otherwise turn one
        // object into hundreds of thousands of tiny allocations. If an erase
        // interval was already found, bounded degradation is whole-object
        // deletion; otherwise leave the source untouched.
        result.changed = removed_positive_interval;
        return result;
    }

    if (!removed_positive_interval) {
        return result;
    }

    result.changed = true;
    result.fragments.reserve(retained_point_sets.size());
    for (auto& points : retained_point_sets) {
        if (points.empty()) {
            continue;
        }
        Annotation fragment = annotation;
        fragment.points = std::move(points);
        fragment.start = fragment.points.front();
        fragment.end = fragment.points.back();
        result.fragments.push_back(std::move(fragment));
    }
    return result;
}

[[nodiscard]] inline bool eraser_sweep_hits_annotation(
    const Annotation& annotation,
    POINT sweep_start,
    POINT sweep_end,
    double eraser_radius,
    bool bounds_already_checked = false) noexcept {
    if (!bounds_already_checked &&
        !eraser_sweep_overlaps_annotation_bounds(
            annotation,
            sweep_start,
            sweep_end,
            eraser_radius)) {
        return false;
    }
    using eraser_detail::PointD;
    const double safe_radius =
        std::isfinite(eraser_radius)
            ? std::max(0.0, eraser_radius)
            : 0.0;
    const PointD eraser_start = eraser_detail::to_point_d(sweep_start);
    const PointD eraser_end = eraser_detail::to_point_d(sweep_end);

    if (!annotation.points.empty()) {
        const double radius =
            safe_radius +
            std::max(
                0.0,
                static_cast<double>(annotation_visual_radius(annotation)));
        if (annotation.points.size() == 1) {
            return eraser_detail::point_segment_distance(
                       eraser_detail::to_point_d(annotation.points.front()),
                       eraser_start,
                       eraser_end) <= radius;
        }
        for (std::size_t index = 1;
             index < annotation.points.size();
             ++index) {
            if (eraser_detail::segment_segment_distance(
                    eraser_start,
                    eraser_end,
                    eraser_detail::to_point_d(annotation.points[index - 1]),
                    eraser_detail::to_point_d(annotation.points[index])) <=
                radius) {
                return true;
            }
        }
        return false;
    }

    if (annotation.tool == Tool::line ||
        annotation.tool == Tool::arrow) {
        const double stroke_radius =
            std::max(
                0.5,
                static_cast<double>(annotation.width) * 0.5);
        if (eraser_detail::segment_segment_distance(
                eraser_start,
                eraser_end,
                eraser_detail::to_point_d(annotation.start),
                eraser_detail::to_point_d(annotation.end)) <=
            safe_radius + stroke_radius) {
            return true;
        }
        if (annotation.tool == Tool::arrow) {
            const auto hit_head = [&](POINT tip, POINT tail) {
                const ArrowHeadWings wings =
                    arrow_head_wings(tip, tail, annotation.width);
                return eraser_detail::segment_segment_distance(
                           eraser_start,
                           eraser_end,
                           eraser_detail::to_point_d(tip),
                           eraser_detail::to_point_d(wings.first)) <=
                           safe_radius + stroke_radius ||
                       eraser_detail::segment_segment_distance(
                           eraser_start,
                           eraser_end,
                           eraser_detail::to_point_d(tip),
                           eraser_detail::to_point_d(wings.second)) <=
                           safe_radius + stroke_radius;
            };
            if ((arrow_has_start_head(annotation.arrow_head_style) &&
                 hit_head(annotation.start, annotation.end)) ||
                (arrow_has_end_head(annotation.arrow_head_style) &&
                 hit_head(annotation.end, annotation.start))) {
                return true;
            }
        }
        return false;
    }

    if (annotation.tool == Tool::serial) {
        return eraser_detail::point_segment_distance(
                   eraser_detail::to_point_d(annotation.start),
                   eraser_start,
                   eraser_end) <=
               safe_radius +
                   static_cast<double>(annotation_visual_radius(annotation));
    }

    if (annotation.tool == Tool::rectangle) {
        const RectI bounds = RectI{
            annotation.start.x,
            annotation.start.y,
            annotation.end.x,
            annotation.end.y,
        }.normalized();
        if (annotation.fill_style == ShapeFillStyle::translucent) {
            return eraser_detail::sweep_hits_expanded_rect(
                eraser_start,
                eraser_end,
                bounds,
                safe_radius);
        }
        const double radius =
            safe_radius +
            std::max(
                0.5,
                static_cast<double>(annotation.width) * 0.5);
        const PointD top_left{
            static_cast<double>(bounds.left),
            static_cast<double>(bounds.top),
        };
        const PointD top_right{
            static_cast<double>(bounds.right),
            static_cast<double>(bounds.top),
        };
        const PointD bottom_right{
            static_cast<double>(bounds.right),
            static_cast<double>(bounds.bottom),
        };
        const PointD bottom_left{
            static_cast<double>(bounds.left),
            static_cast<double>(bounds.bottom),
        };
        return eraser_detail::segment_segment_distance(
                   eraser_start, eraser_end, top_left, top_right) <= radius ||
               eraser_detail::segment_segment_distance(
                   eraser_start, eraser_end, top_right, bottom_right) <= radius ||
               eraser_detail::segment_segment_distance(
                   eraser_start, eraser_end, bottom_right, bottom_left) <= radius ||
               eraser_detail::segment_segment_distance(
                   eraser_start, eraser_end, bottom_left, top_left) <= radius;
    }

    if (annotation.tool == Tool::ellipse) {
        const RectI bounds = RectI{
            annotation.start.x,
            annotation.start.y,
            annotation.end.x,
            annotation.end.y,
        }.normalized();
        if (bounds.empty()) {
            return false;
        }
        const double center_x =
            (static_cast<double>(bounds.left) + bounds.right) * 0.5;
        const double center_y =
            (static_cast<double>(bounds.top) + bounds.bottom) * 0.5;
        const double radius_x =
            std::max(0.5, static_cast<double>(bounds.width()) * 0.5);
        const double radius_y =
            std::max(0.5, static_cast<double>(bounds.height()) * 0.5);
        const auto point_inside = [&](PointD point) {
            const double x = (point.x - center_x) / radius_x;
            const double y = (point.y - center_y) / radius_y;
            return x * x + y * y <= 1.0;
        };
        if (annotation.fill_style == ShapeFillStyle::translucent &&
            (point_inside(eraser_start) || point_inside(eraser_end))) {
            return true;
        }
        const double hit_radius =
            safe_radius +
            std::max(
                0.5,
                static_cast<double>(annotation.width) * 0.5);
        const double circumference =
            3.14159265358979323846 *
            (3.0 * (radius_x + radius_y) -
             std::sqrt(
                 std::max(
                     0.0,
                     (3.0 * radius_x + radius_y) *
                         (radius_x + 3.0 * radius_y))));
        const int segment_count = std::clamp(
            static_cast<int>(std::ceil(
                circumference / std::max(1.0, hit_radius * 0.5))),
            64,
            512);
        constexpr double two_pi = 6.28318530717958647692;
        PointD previous{center_x + radius_x, center_y};
        for (int index = 1; index <= segment_count; ++index) {
            const double angle =
                two_pi * static_cast<double>(index) /
                static_cast<double>(segment_count);
            const PointD current{
                center_x + std::cos(angle) * radius_x,
                center_y + std::sin(angle) * radius_y,
            };
            if (eraser_detail::segment_segment_distance(
                    eraser_start,
                    eraser_end,
                    previous,
                    current) <= hit_radius) {
                return true;
            }
            previous = current;
        }
        return false;
    }

    return eraser_detail::sweep_hits_expanded_rect(
        eraser_start,
        eraser_end,
        annotation_bounds(annotation),
        safe_radius);
}

struct EraserSweepResult {
    bool changed{};
    std::size_t affected_annotations{};
    int selected_annotation_idx{-1};
    std::size_t bounds_rejected_annotations{};
    std::size_t local_split_attempts{};
    std::size_t untouched_annotations_copied{};
    bool document_materialized{};
};

// Applies one pointer-move sweep to a complete annotation document. In local
// mode supported freehand strokes are split, while every other object retains
// object-delete semantics. Selection is preserved only for an untouched
// object and is remapped across insertions/deletions.
[[nodiscard]] inline EraserSweepResult erase_annotations_with_sweep(
    std::vector<Annotation>& annotations,
    POINT sweep_start,
    POINT sweep_end,
    double eraser_radius,
    EraserMode mode,
    int selected_annotation_idx = -1) {
    EraserSweepResult result;
    std::vector<Annotation> next;
    bool document_materialized = false;
    int mapped_selection = -1;
    std::size_t local_fragment_count = 0;
    const std::size_t document_annotation_limit = std::max(
        annotations.size(),
        kMaximumEraserDocumentAnnotations);

    const auto materialize_prefix = [&](std::size_t end_index) {
        if (document_materialized) {
            return;
        }
        next.reserve(annotations.size());
        next.insert(
            next.end(),
            annotations.begin(),
            annotations.begin() +
                static_cast<std::ptrdiff_t>(end_index));
        result.untouched_annotations_copied += end_index;
        result.document_materialized = true;
        document_materialized = true;
        if (selected_annotation_idx >= 0 &&
            selected_annotation_idx < static_cast<int>(end_index)) {
            mapped_selection = selected_annotation_idx;
        }
    };

    const auto append_untouched = [&](std::size_t index) {
        if (!document_materialized) {
            return;
        }
        if (static_cast<int>(index) == selected_annotation_idx) {
            mapped_selection = static_cast<int>(next.size());
        }
        next.push_back(annotations[index]);
        ++result.untouched_annotations_copied;
    };

    for (std::size_t index = 0; index < annotations.size(); ++index) {
        const Annotation& annotation = annotations[index];
        const bool bounds_overlap =
            eraser_sweep_overlaps_annotation_bounds(
                annotation,
                sweep_start,
                sweep_end,
                eraser_radius);
        if (!bounds_overlap) {
            ++result.bounds_rejected_annotations;
            append_untouched(index);
            continue;
        }

        bool affected = false;
        LocalEraseResult local;
        const bool local_candidate =
            mode == EraserMode::local_stroke &&
            annotation_supports_local_erase(annotation);
        if (local_candidate) {
            ++result.local_split_attempts;
            local = split_annotation_by_eraser_sweep(
                annotation,
                sweep_start,
                sweep_end,
                eraser_radius,
                true);
            if (local.changed) {
                affected = true;
                materialize_prefix(index);
                const bool fragment_budget_exceeded =
                    local.fragments.size() >
                    kMaximumEraserDocumentFragments -
                        std::min(
                            local_fragment_count,
                            kMaximumEraserDocumentFragments);
                const bool annotation_budget_exceeded =
                    local.fragments.size() >
                    document_annotation_limit -
                        std::min(
                            next.size(),
                            document_annotation_limit);
                if (fragment_budget_exceeded ||
                    annotation_budget_exceeded) {
                    // Re-evaluate the untouched source document using the
                    // bounded object-delete behavior. This is an atomic
                    // degradation: callers never observe a prefix of local
                    // fragments followed by a different mode.
                    return erase_annotations_with_sweep(
                        annotations,
                        sweep_start,
                        sweep_end,
                        eraser_radius,
                        EraserMode::object,
                        selected_annotation_idx);
                }
                local_fragment_count += local.fragments.size();
                next.insert(
                    next.end(),
                    std::make_move_iterator(local.fragments.begin()),
                    std::make_move_iterator(local.fragments.end()));
            }
        } else if (eraser_sweep_hits_annotation(
                       annotation,
                       sweep_start,
                       sweep_end,
                       eraser_radius,
                       true)) {
            affected = true;
            materialize_prefix(index);
        }

        if (affected) {
            result.changed = true;
            ++result.affected_annotations;
            continue;
        }

        append_untouched(index);
    }

    if (mode == EraserMode::local_stroke &&
        next.size() > document_annotation_limit) {
        return erase_annotations_with_sweep(
            annotations,
            sweep_start,
            sweep_end,
            eraser_radius,
            EraserMode::object,
            selected_annotation_idx);
    }

    if (!result.changed) {
        result.selected_annotation_idx =
            selected_annotation_idx >= 0 &&
                    selected_annotation_idx <
                        static_cast<int>(annotations.size())
                ? selected_annotation_idx
                : -1;
        return result;
    }

    annotations = std::move(next);
    renumber_serial_annotations(annotations);
    result.selected_annotation_idx = mapped_selection;
    return result;
}

}  // namespace airshot::overlay_detail
