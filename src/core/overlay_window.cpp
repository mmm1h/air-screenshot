#include "overlay_window.h"

#include "overlay_session.h"

#include <dwrite.h>
#include <imm.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>

namespace airshot::overlay_detail {

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID2D1Factory>& d2d_factory_storage() {
    static ComPtr<ID2D1Factory> factory;
    return factory;
}

ComPtr<IDWriteFactory>& dwrite_factory_storage() {
    static ComPtr<IDWriteFactory> factory;
    return factory;
}

ID2D1Factory* acquire_d2d_factory() {
    auto& factory = d2d_factory_storage();
    if (!factory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) {
            factory.Reset();
        }
    }
    return factory.Get();
}

IDWriteFactory* acquire_dwrite_factory() {
    auto& factory = dwrite_factory_storage();
    if (!factory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_ISOLATED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
            factory.Reset();
        }
    }
    return factory.Get();
}

ComPtr<IDWriteTextFormat> create_text_format(const wchar_t* font_family,
                                             DWRITE_FONT_WEIGHT weight,
                                             DWRITE_FONT_STYLE style,
                                             float size) {
    ComPtr<IDWriteTextFormat> format;
    auto* factory = acquire_dwrite_factory();
    if (!factory ||
        FAILED(factory->CreateTextFormat(font_family,
                                         nullptr,
                                         weight,
                                         style,
                                         DWRITE_FONT_STRETCH_NORMAL,
                                         size,
                                         L"zh-CN",
                                         format.GetAddressOf()))) {
        format.Reset();
    }
    return format;
}

ComPtr<IDWriteTextLayout> create_text_layout(std::wstring_view text,
                                             IDWriteTextFormat* format,
                                             float width,
                                             float height) {
    ComPtr<IDWriteTextLayout> layout;
    auto* factory = acquire_dwrite_factory();
    if (!factory || !format ||
        FAILED(factory->CreateTextLayout(text.data(),
                                         static_cast<UINT32>(text.size()),
                                         format,
                                         width,
                                         height,
                                         layout.GetAddressOf()))) {
        layout.Reset();
    }
    return layout;
}

}  // namespace

void release_overlay_factories() {
    dwrite_factory_storage().Reset();
    d2d_factory_storage().Reset();
}

bool OverlayWindow::create() {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        window_class.lpfnWndProc = OverlayWindow::window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        window_class.lpszClassName = L"AirScreenshot.Overlay";
        RegisterClassExW(&window_class);
    });

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                            L"AirScreenshot.Overlay",
                            L"",
                            WS_POPUP,
                            monitor_.bounds.left,
                            monitor_.bounds.top,
                            monitor_.bounds.width(),
                            monitor_.bounds.height(),
                            nullptr,
                            nullptr,
                            GetModuleHandleW(nullptr),
                            this);
    if (hwnd_) {
        // Disable IME on overlay window so C/Shift keys work directly
        ImmAssociateContext(hwnd_, nullptr);
    }
    return hwnd_ && create_render_target();
}

void OverlayWindow::destroy() {
    discard_device_resources();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OverlayWindow::discard_device_resources() noexcept {
    dim_brush_.Reset();
    blue_brush_.Reset();
    white_brush_.Reset();
    black_brush_.Reset();
    toolbar_brush_.Reset();
    toolbar_bg_brush_.Reset();
    toolbar_border_brush_.Reset();
    hover_bg_brush_.Reset();
    active_bg_brush_.Reset();
    disabled_brush_.Reset();
    toolbar_shadow_brush_.Reset();
    danger_hover_bg_brush_.Reset();
    true_white_brush_.Reset();
    green_brush_.Reset();
    red_brush_.Reset();
    round_stroke_style_.Reset();
    dashed_stroke_style_.Reset();
    rendered_annotations_.Reset();
    rendered_annotation_revision_ = 0;
    rendered_annotation_selection_ = {};
    rendered_annotation_has_effect_preview_ = false;
    background_.Reset();
    render_target_.Reset();
}

void OverlayWindow::invalidate() const {
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool OverlayWindow::create_render_target() {
    if (!hwnd_) {
        return false;
    }
    const bool current_theme_is_light = should_use_light_theme(session_.request_.config.theme);
    if (render_target_) {
        if (is_light_theme_ == current_theme_is_light) {
            return true;
        }
        discard_device_resources();
    }
    is_light_theme_ = current_theme_is_light;

    auto* d2d_factory = acquire_d2d_factory();
    if (!d2d_factory || !acquire_dwrite_factory() ||
        monitor_.bounds.width() <= 0 || monitor_.bounds.height() <= 0 ||
        monitor_.bitmap.empty()) {
        return false;
    }

    const D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT>(monitor_.bounds.width()), static_cast<UINT>(monitor_.bounds.height()));
    HRESULT result = d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0F, 96.0F),
        D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        render_target_.GetAddressOf());
    if (FAILED(result)) {
        discard_device_resources();
        return false;
    }
    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    result = render_target_->CreateBitmap(size,
                                          monitor_.bitmap.pixels.data(),
                                          static_cast<UINT>(monitor_.bitmap.stride()),
                                          properties,
                                          background_.GetAddressOf());
    if (FAILED(result)) {
        discard_device_resources();
        return false;
    }

    const auto create_brush = [this](const D2D1_COLOR_F& color,
                                     ComPtr<ID2D1SolidColorBrush>& brush) {
        return SUCCEEDED(render_target_->CreateSolidColorBrush(color, brush.GetAddressOf())) &&
               brush != nullptr;
    };

    bool brushes_created =
        create_brush(D2D1::ColorF(0, 0.48F), dim_brush_) &&
        create_brush(D2D1::ColorF(0x4D7CFE), blue_brush_) &&
        create_brush(D2D1::ColorF(D2D1::ColorF::White), true_white_brush_) &&
        create_brush(D2D1::ColorF(0x00B634), green_brush_) &&
        create_brush(D2D1::ColorF(0xF05D6F), red_brush_) &&
        create_brush(D2D1::ColorF(D2D1::ColorF::White, 0.32F), disabled_brush_) &&
        create_brush(D2D1::ColorF(D2D1::ColorF::Black, 0.30F), toolbar_shadow_brush_) &&
        create_brush(D2D1::ColorF(0xF05D6F, 0.16F), danger_hover_bg_brush_);

    if (is_light_theme_) {
        brushes_created =
            brushes_created &&
            create_brush(D2D1::ColorF(0x1F2329), white_brush_) &&
            create_brush(D2D1::ColorF(D2D1::ColorF::Black), black_brush_) &&
            create_brush(D2D1::ColorF(0x17191E, 0.97F), toolbar_brush_) &&
            create_brush(D2D1::ColorF(0x17191E, 0.97F), toolbar_bg_brush_) &&
            create_brush(D2D1::ColorF(0x343943, 0.98F), toolbar_border_brush_) &&
            create_brush(D2D1::ColorF(D2D1::ColorF::White, 0.08F), hover_bg_brush_) &&
            create_brush(D2D1::ColorF(0x4D7CFE, 0.20F), active_bg_brush_);
    } else {
        brushes_created =
            brushes_created &&
            create_brush(D2D1::ColorF(D2D1::ColorF::White), white_brush_) &&
            create_brush(D2D1::ColorF(D2D1::ColorF::Black), black_brush_) &&
            create_brush(D2D1::ColorF(0x17191E, 0.97F), toolbar_brush_) &&
            create_brush(D2D1::ColorF(0x17191E, 0.97F), toolbar_bg_brush_) &&
            create_brush(D2D1::ColorF(0x343943, 0.98F), toolbar_border_brush_) &&
            create_brush(D2D1::ColorF(D2D1::ColorF::White, 0.08F), hover_bg_brush_) &&
            create_brush(D2D1::ColorF(0x4D7CFE, 0.20F), active_bg_brush_);
    }
    if (!brushes_created) {
        discard_device_resources();
        return false;
    }

    const D2D1_STROKE_STYLE_PROPERTIES stroke_properties = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND,
        1.0f,
        D2D1_DASH_STYLE_SOLID,
        0.0f
    );
    result = d2d_factory->CreateStrokeStyle(
        stroke_properties, nullptr, 0, round_stroke_style_.GetAddressOf());
    if (FAILED(result) || !round_stroke_style_) {
        discard_device_resources();
        return false;
    }
    D2D1_STROKE_STYLE_PROPERTIES dashed_properties = stroke_properties;
    dashed_properties.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    constexpr std::array<float, 2> dash_pattern{3.0F, 2.0F};
    result = d2d_factory->CreateStrokeStyle(
        dashed_properties,
        dash_pattern.data(),
        static_cast<UINT32>(dash_pattern.size()),
        dashed_stroke_style_.GetAddressOf());
    if (FAILED(result) || !dashed_stroke_style_) {
        discard_device_resources();
        return false;
    }
    return true;
}

D2D1_RECT_F OverlayWindow::local_rect(const RectI& rect) const {
    return D2D1::RectF(static_cast<float>(rect.left - monitor_.bounds.left),
                       static_cast<float>(rect.top - monitor_.bounds.top),
                       static_cast<float>(rect.right - monitor_.bounds.left),
                       static_cast<float>(rect.bottom - monitor_.bounds.top));
}

RectI padded_buttons_bounds(const std::vector<ToolbarButton>& buttons, int padding) {
    if (buttons.empty()) {
        return {};
    }
    RectI bounds = buttons.front().bounds;
    for (const auto& button : buttons) {
        bounds.left = std::min(bounds.left, button.bounds.left);
        bounds.top = std::min(bounds.top, button.bounds.top);
        bounds.right = std::max(bounds.right, button.bounds.right);
        bounds.bottom = std::max(bounds.bottom, button.bounds.bottom);
    }
    bounds.left -= padding;
    bounds.top -= padding;
    bounds.right += padding;
    bounds.bottom += padding;
    return bounds;
}

void OverlayWindow::draw_arrow(POINT start,
                               POINT end,
                               ID2D1Brush* brush,
                               float width,
                               ArrowHeadStyle head_style,
                               ID2D1StrokeStyle* stroke_style) {
    const D2D1_POINT_2F first{
        static_cast<float>(start.x - monitor_.bounds.left), static_cast<float>(start.y - monitor_.bounds.top)};
    const D2D1_POINT_2F second{
        static_cast<float>(end.x - monitor_.bounds.left), static_cast<float>(end.y - monitor_.bounds.top)};
    render_target_->DrawLine(first, second, brush, width, stroke_style);
    const auto draw_head = [&](POINT tip, POINT tail) {
        const ArrowHeadWings wings = arrow_head_wings(tip, tail, width);
        const D2D1_POINT_2F local_tip{
            static_cast<float>(tip.x - monitor_.bounds.left),
            static_cast<float>(tip.y - monitor_.bounds.top),
        };
        for (const POINT wing : {wings.first, wings.second}) {
            render_target_->DrawLine(
                local_tip,
                D2D1::Point2F(
                    static_cast<float>(wing.x - monitor_.bounds.left),
                    static_cast<float>(wing.y - monitor_.bounds.top)),
                brush,
                width,
                stroke_style);
        }
    };
    if (arrow_has_start_head(head_style)) {
        draw_head(start, end);
    }
    if (arrow_has_end_head(head_style)) {
        draw_head(end, start);
    }
}

bool OverlayWindow::draw_rendered_annotations() {
    const bool has_effect_preview = session_.effect_preview_active();
    if (session_.annotations().empty() && !has_effect_preview) {
        rendered_annotations_.Reset();
        rendered_annotation_revision_ = session_.rendered_source_revision();
        rendered_annotation_selection_ = session_.selection();
        rendered_annotation_has_effect_preview_ = false;
        return true;
    }

    const RectI selection = session_.selection();
    const bool same_selection =
        selection.left == rendered_annotation_selection_.left &&
        selection.top == rendered_annotation_selection_.top &&
        selection.right == rendered_annotation_selection_.right &&
        selection.bottom == rendered_annotation_selection_.bottom;
    if (!rendered_annotations_ ||
        rendered_annotation_revision_ != session_.rendered_source_revision() ||
        rendered_annotation_has_effect_preview_ != has_effect_preview ||
        !same_selection) {
        const Bitmap& rendered =
            session_.cached_rendered_selection_for_display();
        if (!rendered.valid()) {
            rendered_annotations_.Reset();
            return false;
        }
        const D2D1_BITMAP_PROPERTIES properties =
            D2D1::BitmapProperties(
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_IGNORE));
        Microsoft::WRL::ComPtr<ID2D1Bitmap> next;
        if (FAILED(render_target_->CreateBitmap(
                D2D1::SizeU(
                    static_cast<UINT32>(rendered.width),
                    static_cast<UINT32>(rendered.height)),
                rendered.pixels.data(),
                static_cast<UINT32>(rendered.stride()),
                properties,
                next.GetAddressOf())) ||
            !next) {
            rendered_annotations_.Reset();
            return false;
        }
        rendered_annotations_ = std::move(next);
        rendered_annotation_revision_ = session_.rendered_source_revision();
        rendered_annotation_selection_ = selection;
        rendered_annotation_has_effect_preview_ = has_effect_preview;
    }

    const auto visible = intersect(selection, monitor_.bounds);
    if (!visible) {
        return true;
    }
    const D2D1_RECT_F destination =
        D2D1::RectF(
            static_cast<float>(visible->left - monitor_.bounds.left),
            static_cast<float>(visible->top - monitor_.bounds.top),
            static_cast<float>(visible->right - monitor_.bounds.left),
            static_cast<float>(visible->bottom - monitor_.bounds.top));
    const D2D1_RECT_F source =
        D2D1::RectF(
            static_cast<float>(visible->left - selection.left),
            static_cast<float>(visible->top - selection.top),
            static_cast<float>(visible->right - selection.left),
            static_cast<float>(visible->bottom - selection.top));
    render_target_->DrawBitmap(
        rendered_annotations_.Get(),
        destination,
        1.0F,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        source);
    return true;
}

void OverlayWindow::draw_annotation(const Annotation& annotation, bool preview) {
    const RectI selection = session_.selection();
    POINT start{selection.left + annotation.start.x, selection.top + annotation.start.y};
    POINT end{selection.left + annotation.end.x, selection.top + annotation.end.y};

    COLORREF color = annotation.color;
    float draw_width = annotation.width;
    float alpha = annotation.tool == Tool::highlight
                      ? std::clamp(annotation.alpha / 255.0F, 0.0F, 1.0F)
                      : 1.0F;
    if (preview) {
        color = session_.active_color();
        draw_width = session_.active_width();
        alpha = annotation.tool == Tool::highlight
                    ? std::clamp(
                          session_.active_highlight_alpha() / 255.0F,
                          0.0F,
                          1.0F)
                    : alpha;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(render_target_->CreateSolidColorBrush(
            D2D1::ColorF(GetRValue(color) / 255.0F,
                         GetGValue(color) / 255.0F,
                         GetBValue(color) / 255.0F,
                         alpha),
            brush.GetAddressOf())) ||
        !brush) {
        return;
    }

    ID2D1StrokeStyle* shape_stroke_style =
        annotation.stroke_pattern == StrokePattern::dashed
            ? dashed_stroke_style_.Get()
            : round_stroke_style_.Get();
    if (annotation.tool == Tool::rectangle) {
        const RectI absolute_bounds =
            RectI{start.x, start.y, end.x, end.y}.normalized();
        const D2D1_RECT_F bounds = local_rect(absolute_bounds);
        ComPtr<ID2D1SolidColorBrush> fill_brush;
        if (annotation.fill_style == ShapeFillStyle::translucent &&
            SUCCEEDED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(
                    GetRValue(color) / 255.0F,
                    GetGValue(color) / 255.0F,
                    GetBValue(color) / 255.0F,
                    64.0F / 255.0F),
                fill_brush.GetAddressOf())) &&
            fill_brush) {
            if (annotation.rounded_rectangle) {
                const float radius = rounded_rectangle_radius(
                    absolute_bounds,
                    draw_width);
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(bounds, radius, radius),
                    fill_brush.Get());
            } else {
                render_target_->FillRectangle(bounds, fill_brush.Get());
            }
        }
        if (annotation.rounded_rectangle) {
            const float radius = rounded_rectangle_radius(
                absolute_bounds,
                draw_width);
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(bounds, radius, radius),
                brush.Get(),
                draw_width,
                shape_stroke_style);
        } else {
            render_target_->DrawRectangle(
                bounds,
                brush.Get(),
                draw_width,
                shape_stroke_style);
        }
    } else if (annotation.tool == Tool::ellipse) {
        const D2D1_RECT_F rect = local_rect(RectI{start.x, start.y, end.x, end.y}.normalized());
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F(
                (rect.left + rect.right) / 2.0F,
                (rect.top + rect.bottom) / 2.0F),
            std::abs(rect.right - rect.left) / 2.0F,
            std::abs(rect.bottom - rect.top) / 2.0F);
        ComPtr<ID2D1SolidColorBrush> fill_brush;
        if (annotation.fill_style == ShapeFillStyle::translucent &&
            SUCCEEDED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(
                    GetRValue(color) / 255.0F,
                    GetGValue(color) / 255.0F,
                    GetBValue(color) / 255.0F,
                    64.0F / 255.0F),
                fill_brush.GetAddressOf())) &&
            fill_brush) {
            render_target_->FillEllipse(ellipse, fill_brush.Get());
        }
        render_target_->DrawEllipse(
            ellipse,
            brush.Get(),
            draw_width,
            shape_stroke_style);
    } else if (annotation.tool == Tool::line) {
        render_target_->DrawLine(D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left),
                                               static_cast<float>(start.y - monitor_.bounds.top)),
                                 D2D1::Point2F(static_cast<float>(end.x - monitor_.bounds.left),
                                               static_cast<float>(end.y - monitor_.bounds.top)),
                                 brush.Get(),
                                 draw_width,
                                 shape_stroke_style);
    } else if (annotation.tool == Tool::arrow) {
        draw_arrow(
            start,
            end,
            brush.Get(),
            draw_width,
            annotation.arrow_head_style,
            shape_stroke_style);
    } else if (annotation.tool == Tool::pen || annotation.tool == Tool::highlight) {
        const float line_width =
            annotation.tool == Tool::highlight
                ? tool_visual_radius(Tool::highlight, draw_width) * 2.0F
                : draw_width;
        std::vector<POINT> preview_points;
        const std::vector<POINT>* points = &annotation.points;
        if (preview && annotation.tool == Tool::pen &&
            annotation.points.size() > 1) {
            const double spacing = std::clamp(
                static_cast<double>(draw_width) * 0.35,
                1.5,
                4.0);
            preview_points = smooth_polyline(
                resample_polyline(annotation.points, spacing),
                2);
            points = &preview_points;
        }
        if (points->empty()) {
            render_target_->DrawLine(D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left),
                                                   static_cast<float>(start.y - monitor_.bounds.top)),
                                     D2D1::Point2F(static_cast<float>(end.x - monitor_.bounds.left),
                                                   static_cast<float>(end.y - monitor_.bounds.top)),
                                     brush.Get(),
                                     line_width,
                                     round_stroke_style_.Get());
        } else if (points->size() == 1) {
            const POINT point{
                selection.left + points->front().x,
                selection.top + points->front().y,
            };
            const float radius = std::max(1.0F, line_width * 0.5F);
            render_target_->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(
                        static_cast<float>(point.x - monitor_.bounds.left),
                        static_cast<float>(point.y - monitor_.bounds.top)),
                    radius,
                    radius),
                brush.Get());
        } else {
            for (std::size_t index = 1; index < points->size(); ++index) {
                const POINT first{
                    selection.left + (*points)[index - 1].x,
                    selection.top + (*points)[index - 1].y};
                const POINT second{
                    selection.left + (*points)[index].x,
                    selection.top + (*points)[index].y};
                render_target_->DrawLine(D2D1::Point2F(static_cast<float>(first.x - monitor_.bounds.left),
                                                       static_cast<float>(first.y - monitor_.bounds.top)),
                                         D2D1::Point2F(static_cast<float>(second.x - monitor_.bounds.left),
                                                       static_cast<float>(second.y - monitor_.bounds.top)),
                                         brush.Get(),
                                         line_width,
                                         round_stroke_style_.Get());
            }
        }
    } else if (annotation.tool == Tool::eraser) {
        const D2D1_POINT_2F center{
            static_cast<float>(end.x - monitor_.bounds.left),
            static_cast<float>(end.y - monitor_.bounds.top),
        };
        const float radius =
            tool_visual_radius(Tool::eraser, annotation.width);
        render_target_->DrawEllipse(
            D2D1::Ellipse(center, radius, radius),
            true_white_brush_.Get(),
            4.0F);
        render_target_->DrawEllipse(
            D2D1::Ellipse(center, radius, radius),
            red_brush_.Get(),
            2.0F);
        for (const float direction : {-1.0F, 1.0F}) {
            render_target_->DrawLine(
                D2D1::Point2F(
                    center.x - 4.0F,
                    center.y + direction * 4.0F),
                D2D1::Point2F(
                    center.x + 4.0F,
                    center.y - direction * 4.0F),
                red_brush_.Get(),
                1.5F,
                round_stroke_style_.Get());
        }
    } else if (annotation.tool == Tool::mosaic) {
        if (annotation.points.empty()) {
            const D2D1_RECT_F bounds =
                local_rect(RectI{start.x, start.y, end.x, end.y}.normalized());
            render_target_->FillRectangle(bounds, dim_brush_.Get());
            render_target_->DrawRectangle(
                bounds, white_brush_.Get(), 1.0F, round_stroke_style_.Get());
        } else {
            const int radius =
                static_cast<int>(tool_visual_radius(
                    Tool::mosaic,
                    annotation.width));
            for (const POINT relative : annotation.points) {
                POINT point{selection.left + relative.x, selection.top + relative.y};
                RectI block{
                    point.x - radius,
                    point.y - radius,
                    point.x + radius,
                    point.y + radius,
                };
                render_target_->FillRectangle(local_rect(block), dim_brush_.Get());
            }
        }
    } else if (annotation.tool == Tool::blur) {
        ComPtr<ID2D1SolidColorBrush> temp_blur_brush;
        if (FAILED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(0.7F, 0.7F, 0.7F, 0.45F),
                temp_blur_brush.GetAddressOf())) ||
            !temp_blur_brush) {
            return;
        }
        if (annotation.points.empty()) {
            const D2D1_RECT_F bounds =
                local_rect(RectI{start.x, start.y, end.x, end.y}.normalized());
            render_target_->FillRectangle(bounds, temp_blur_brush.Get());
            render_target_->DrawRectangle(
                bounds, white_brush_.Get(), 1.0F, round_stroke_style_.Get());
        } else {
            for (const POINT relative : annotation.points) {
                POINT point{selection.left + relative.x, selection.top + relative.y};
                const float radius =
                    tool_visual_radius(Tool::blur, draw_width);
                D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                    D2D1::Point2F(static_cast<float>(point.x - monitor_.bounds.left), static_cast<float>(point.y - monitor_.bounds.top)),
                    radius,
                    radius);
                render_target_->FillEllipse(ellipse, temp_blur_brush.Get());
            }
        }
    } else if (annotation.tool == Tool::text) {
        const float font_size = preview ? session_.active_text_size() : annotation.width;
        const TextStyle text_style = preview ? session_.active_text_style() : annotation.text_style;
        auto format = create_text_format(
            session_.request_.config.text_font_family.c_str(),
            session_.request_.config.text_font_bold
                ? DWRITE_FONT_WEIGHT_BOLD
                : DWRITE_FONT_WEIGHT_NORMAL,
            session_.request_.config.text_font_italic
                ? DWRITE_FONT_STYLE_ITALIC
                : DWRITE_FONT_STYLE_NORMAL,
            font_size);
        if (!format) {
            return;
        }
        const D2D1_RECT_F bounds =
            D2D1::RectF(static_cast<float>(start.x - monitor_.bounds.left),
                        static_cast<float>(start.y - monitor_.bounds.top),
                        static_cast<float>(monitor_.bounds.width()),
                        static_cast<float>(monitor_.bounds.height()));
        if (text_style == TextStyle::dark) {
            const RectI measured = annotation_bounds(annotation);
            const D2D1_RECT_F background = D2D1::RectF(
                static_cast<float>(
                    selection.left + measured.left - monitor_.bounds.left),
                static_cast<float>(
                    selection.top + measured.top - monitor_.bounds.top),
                static_cast<float>(
                    selection.left + measured.right - monitor_.bounds.left),
                static_cast<float>(
                    selection.top + measured.bottom - monitor_.bounds.top));
            ComPtr<ID2D1SolidColorBrush> dark_background;
            if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                    D2D1::ColorF(0x1F2329, 0.90F),
                    dark_background.GetAddressOf())) &&
                dark_background) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(background, 3.0F, 3.0F),
                    dark_background.Get());
            }
            render_target_->DrawTextW(annotation.text.c_str(),
                                      static_cast<UINT32>(annotation.text.size()),
                                      format.Get(),
                                      bounds,
                                      true_white_brush_.Get());
        } else if (text_style == TextStyle::outline) {
            for (float dy : {-2.0F, 0.0F, 2.0F}) {
                for (float dx : {-2.0F, 0.0F, 2.0F}) {
                    if (dx == 0.0F && dy == 0.0F) {
                        continue;
                    }
                    D2D1_RECT_F outline_bounds = bounds;
                    outline_bounds.left += dx;
                    outline_bounds.right += dx;
                    outline_bounds.top += dy;
                    outline_bounds.bottom += dy;
                    render_target_->DrawTextW(annotation.text.c_str(),
                                              static_cast<UINT32>(annotation.text.size()),
                                              format.Get(),
                                              outline_bounds,
                                              true_white_brush_.Get());
                }
            }
            render_target_->DrawTextW(annotation.text.c_str(),
                                      static_cast<UINT32>(annotation.text.size()),
                                      format.Get(),
                                      bounds,
                                      brush.Get());
        } else {
            render_target_->DrawTextW(annotation.text.c_str(),
                                      static_cast<UINT32>(annotation.text.size()),
                                      format.Get(),
                                      bounds,
                                      brush.Get());
        }
    } else if (annotation.tool == Tool::watermark) {
        if (annotation.text.empty()) {
            return;
        }
        auto format = create_text_format(session_.request_.config.text_font_family.c_str(),
                                         DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         annotation.width);
        if (!format) {
            return;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        const float watermark_alpha =
            std::clamp(annotation.alpha / 255.0F, 0.0F, 1.0F);
        ComPtr<ID2D1SolidColorBrush> watermark_brush;
        if (FAILED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(GetRValue(annotation.color) / 255.0F,
                             GetGValue(annotation.color) / 255.0F,
                             GetBValue(annotation.color) / 255.0F,
                             watermark_alpha),
                watermark_brush.GetAddressOf())) ||
            !watermark_brush) {
            return;
        }

        const RectI selected_rect = session_.selection();
        const float left = static_cast<float>(selected_rect.left - monitor_.bounds.left);
        const float top = static_cast<float>(selected_rect.top - monitor_.bounds.top);
        const float width = static_cast<float>(selected_rect.width());
        const float height = static_cast<float>(selected_rect.height());
        float measured_width =
            static_cast<float>(annotation.text.size()) * annotation.width;
        auto layout = create_text_layout(
            annotation.text,
            format.Get(),
            std::max(120.0F, width),
            annotation.width * 2.0F);
        if (layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                measured_width = metrics.widthIncludingTrailingWhitespace;
            }
        }
        const float step_x = std::max(120.0F, measured_width + 72.0F);
        const float step_y = std::max(70.0F, annotation.width * 3.2F);

        render_target_->PushAxisAlignedClip(D2D1::RectF(left, top, left + width, top + height),
                                            D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_MATRIX_3X2_F previous_transform{};
        render_target_->GetTransform(&previous_transform);
        const D2D1_POINT_2F center = D2D1::Point2F(left + width / 2.0F, top + height / 2.0F);
        render_target_->SetTransform(D2D1::Matrix3x2F::Rotation(-18.0F, center) * previous_transform);
        for (float y = top - step_y; y < top + height + step_y; y += step_y) {
            for (float x = left - step_x; x < left + width + step_x; x += step_x) {
                const D2D1_RECT_F text_bounds = D2D1::RectF(x, y, x + step_x, y + annotation.width * 2.0F);
                render_target_->DrawTextW(annotation.text.c_str(),
                                          static_cast<UINT32>(annotation.text.size()),
                                          format.Get(),
                                          text_bounds,
                                          watermark_brush.Get());
            }
        }
        render_target_->SetTransform(previous_transform);
        render_target_->PopAxisAlignedClip();
    } else if (annotation.tool == Tool::serial) {
        const float radius =
            serial_visual_radius(annotation.width, annotation.serial);
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left), static_cast<float>(start.y - monitor_.bounds.top)),
            radius,
            radius);
        render_target_->FillEllipse(ellipse, brush.Get());
        render_target_->DrawEllipse(ellipse, white_brush_.Get(), 1.5F);

        auto format = create_text_format(
            L"Consolas",
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            serial_font_size(annotation.width, annotation.serial));
        if (!format) {
            return;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const std::wstring serial_text = std::to_wstring(annotation.serial);
        const D2D1_RECT_F bounds =
            D2D1::RectF(static_cast<float>(start.x - monitor_.bounds.left) - radius,
                        static_cast<float>(start.y - monitor_.bounds.top) - radius,
                        static_cast<float>(start.x - monitor_.bounds.left) + radius,
                        static_cast<float>(start.y - monitor_.bounds.top) + radius);
        render_target_->DrawTextW(serial_text.c_str(),
                                  static_cast<UINT32>(serial_text.size()),
                                  format.Get(),
                                  bounds,
                                  white_brush_.Get());
    }
}

void OverlayWindow::paint() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    if (!create_render_target()) {
        EndPaint(hwnd_, &paint);
        return;
    }
    auto* d2d_factory = acquire_d2d_factory();
    if (!d2d_factory) {
        EndPaint(hwnd_, &paint);
        return;
    }
    const OverlayUiMetrics ui{
        hwnd_ ? GetDpiForWindow(hwnd_) : kOverlayBaseDpi};
    const float ui_scale = ui.px(1.0F);
    render_target_->BeginDraw();
    render_target_->DrawBitmap(background_.Get());

    const RectI selected = session_.display_selection();
    if (selected.empty()) {
        render_target_->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(monitor_.bounds.width()), static_cast<float>(monitor_.bounds.height())),
            dim_brush_.Get());
    } else if (const auto visible = intersect(selected, monitor_.bounds)) {
        const RectI local{
            visible->left - monitor_.bounds.left,
            visible->top - monitor_.bounds.top,
            visible->right - monitor_.bounds.left,
            visible->bottom - monitor_.bounds.top,
        };
        const float width = static_cast<float>(monitor_.bounds.width());
        const float height = static_cast<float>(monitor_.bounds.height());
        render_target_->FillRectangle(D2D1::RectF(0, 0, width, static_cast<float>(local.top)), dim_brush_.Get());
        render_target_->FillRectangle(
            D2D1::RectF(0, static_cast<float>(local.bottom), width, height), dim_brush_.Get());
        render_target_->FillRectangle(D2D1::RectF(0,
                                                  static_cast<float>(local.top),
                                                  static_cast<float>(local.left),
                                                  static_cast<float>(local.bottom)),
                                      dim_brush_.Get());
        render_target_->FillRectangle(D2D1::RectF(static_cast<float>(local.right),
                                                  static_cast<float>(local.top),
                                                  width,
                                                  static_cast<float>(local.bottom)),
                                      dim_brush_.Get());
        render_target_->DrawRectangle(
            local_rect(selected), blue_brush_.Get(), ui.px(2.0F));
    } else {
        render_target_->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(monitor_.bounds.width()), static_cast<float>(monitor_.bounds.height())),
            dim_brush_.Get());
    }

    if (session_.selection_complete()) {
        bool selected_effect_edit = false;
        if (session_.annotation_transaction_active()) {
            const int selected_index = session_.selected_annotation_idx();
            if (selected_index >= 0 &&
                static_cast<std::size_t>(selected_index) <
                    session_.annotations().size()) {
                const Tool selected_tool = session_.annotations()[
                    static_cast<std::size_t>(selected_index)].tool;
                selected_effect_edit =
                    selected_tool == Tool::mosaic ||
                    selected_tool == Tool::blur;
            }
        }
        if (session_.dragging_selection() ||
            (session_.annotation_transaction_active() &&
             !selected_effect_edit) ||
            !draw_rendered_annotations()) {
            for (const auto& annotation : session_.annotations()) {
                draw_annotation(annotation, false);
            }
        }
        if (const Annotation* preview = session_.preview()) {
            if (!session_.effect_preview_active()) {
                draw_annotation(*preview, true);
            }
        }
        if (const auto visible = intersect(session_.selection(), monitor_.bounds)) {
            render_target_->DrawRectangle(
                local_rect(*visible),
                blue_brush_.Get(),
                ui.px(2.0F));
        }

        // Draw object-level editing controls for the selected annotation.
        if (session_.active_tool() == Tool::select && session_.selected_annotation_idx() != -1) {
            const auto& annotations = session_.annotations();
            std::size_t idx = static_cast<std::size_t>(session_.selected_annotation_idx());
            if (idx < annotations.size()) {
                const auto& annotation = annotations[idx];
                const RectI selection = session_.selection();
                RectI bounds = annotation_control_bounds(annotation);
                if (bounds.empty()) {
                    bounds = annotation_bounds(annotation);
                }
                const int control_padding = ui.px(3);
                bounds.left -= control_padding;
                bounds.top -= control_padding;
                bounds.right += control_padding;
                bounds.bottom += control_padding;

                RectI screen_bounds{
                    selection.left + bounds.left,
                    selection.top + bounds.top,
                    selection.left + bounds.right,
                    selection.top + bounds.bottom
                };

                D2D1_RECT_F local_rect_f = local_rect(screen_bounds);
                render_target_->DrawRectangle(
                    local_rect_f,
                    true_white_brush_.Get(),
                    ui.px(3.0F));
                render_target_->DrawRectangle(
                    local_rect_f,
                    blue_brush_.Get(),
                    ui.px(1.5F));

                for (const auto& handle :
                     annotation_control_handles(annotation)) {
                    const float center_x = static_cast<float>(
                        selection.left + handle.position.x -
                        monitor_.bounds.left);
                    const float center_y = static_cast<float>(
                        selection.top + handle.position.y -
                        monitor_.bounds.top);
                    if (handle.kind == AnnotationHandle::start_point ||
                        handle.kind == AnnotationHandle::end_point) {
                        const D2D1_ELLIPSE endpoint =
                            D2D1::Ellipse(
                                D2D1::Point2F(center_x, center_y),
                                ui.px(5.0F),
                                ui.px(5.0F));
                        render_target_->FillEllipse(
                            endpoint,
                            true_white_brush_.Get());
                        render_target_->DrawEllipse(
                            endpoint,
                            blue_brush_.Get(),
                            ui.px(2.0F));
                    } else {
                        const D2D1_RECT_F resize_handle =
                            D2D1::RectF(
                                center_x - ui.px(4.0F),
                                center_y - ui.px(4.0F),
                                center_x + ui.px(4.0F),
                                center_y + ui.px(4.0F));
                        render_target_->FillRectangle(
                            resize_handle,
                            true_white_brush_.Get());
                        render_target_->DrawRectangle(
                            resize_handle,
                            blue_brush_.Get(),
                            ui.px(1.5F));
                    }
                }
            }
        }

        // Draw 8-point handles
        const RectI& sel = session_.selection();
        float handle_left = static_cast<float>(sel.left - monitor_.bounds.left);
        float handle_top = static_cast<float>(sel.top - monitor_.bounds.top);
        float handle_right = static_cast<float>(sel.right - monitor_.bounds.left);
        float handle_bottom = static_cast<float>(sel.bottom - monitor_.bounds.top);
        float mid_x = handle_left + (handle_right - handle_left) / 2.0F;
        float mid_y = handle_top + (handle_bottom - handle_top) / 2.0F;

        D2D1_POINT_2F handles[] = {
            D2D1::Point2F(handle_left, handle_top),
            D2D1::Point2F(mid_x, handle_top),
            D2D1::Point2F(handle_right, handle_top),
            D2D1::Point2F(handle_right, mid_y),
            D2D1::Point2F(handle_right, handle_bottom),
            D2D1::Point2F(mid_x, handle_bottom),
            D2D1::Point2F(handle_left, handle_bottom),
            D2D1::Point2F(handle_left, mid_y)
        };

        auto format = create_text_format(
            L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, ui.px(12.0F));
        if (format) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // The capture toolbar intentionally stays dark in both app themes so it
        // remains legible over arbitrary screenshot content.
        ComPtr<ID2D1SolidColorBrush> original_overlay_white_brush = white_brush_;
        white_brush_ = true_white_brush_;

        // Draw main toolbar background rounded rectangle card
        if (!session_.toolbar().empty()) {
            D2D1_RECT_F bg_rect = local_rect(
                padded_buttons_bounds(session_.toolbar(), ui.px(8)));
            D2D1_RECT_F shadow_rect = bg_rect;
            shadow_rect.left -= ui.px(1.0F);
            shadow_rect.right += ui.px(1.0F);
            shadow_rect.top += ui.px(2.0F);
            shadow_rect.bottom += ui.px(3.0F);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(shadow_rect, ui.px(9.0F), ui.px(9.0F)),
                toolbar_shadow_brush_.Get());
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(bg_rect, ui.px(8.0F), ui.px(8.0F)),
                toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(bg_rect, ui.px(8.0F), ui.px(8.0F)),
                toolbar_border_brush_.Get(),
                ui.px(1.0F));

            for (const auto& button : session_.toolbar()) {
                if (!intersect(button.bounds, monitor_.bounds)) {
                    continue;
                }
                D2D1_RECT_F bounds = local_rect(button.bounds);
                D2D1_MATRIX_3X2_F previous_transform{};
                render_target_->GetTransform(&previous_transform);
                if (ui_scale != 1.0F) {
                    const float center_x = (bounds.left + bounds.right) * 0.5F;
                    const float center_y = (bounds.top + bounds.bottom) * 0.5F;
                    bounds = D2D1::RectF(
                        center_x + (bounds.left - center_x) / ui_scale,
                        center_y + (bounds.top - center_y) / ui_scale,
                        center_x + (bounds.right - center_x) / ui_scale,
                        center_y + (bounds.bottom - center_y) / ui_scale);
                    render_target_->SetTransform(D2D1::Matrix3x2F::Scale(
                        D2D1::SizeF(ui_scale, ui_scale),
                        D2D1::Point2F(center_x, center_y)));
                }

                if (button.id == L"|") {
                    float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, bounds.top + 11.0F),
                        D2D1::Point2F(cx, bounds.bottom - 11.0F),
                        toolbar_border_brush_.Get(),
                        1.2F
                    );
                    render_target_->SetTransform(previous_transform);
                    continue;
                }

                const bool is_hovered =
                    session_.hovered_button_id() == button.id;
                if (button.id == L"drag") {
                    if (is_hovered || session_.toolbar_dragging()) {
                        render_target_->FillRoundedRectangle(
                            D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                            hover_bg_brush_.Get());
                    }
                    const float handle_cx =
                        bounds.left + (bounds.right - bounds.left) * 0.5F;
                    const float handle_cy =
                        bounds.top + (bounds.bottom - bounds.top) * 0.5F;
                    ID2D1Brush* handle_brush =
                        (is_hovered || session_.toolbar_dragging())
                            ? static_cast<ID2D1Brush*>(true_white_brush_.Get())
                            : static_cast<ID2D1Brush*>(disabled_brush_.Get());
                    for (const float y : {-5.0F, 0.0F, 5.0F}) {
                        render_target_->FillEllipse(
                            D2D1::Ellipse(
                                D2D1::Point2F(handle_cx - 2.5F, handle_cy + y),
                                1.1F,
                                1.1F),
                            handle_brush);
                        render_target_->FillEllipse(
                            D2D1::Ellipse(
                                D2D1::Point2F(handle_cx + 2.5F, handle_cy + y),
                                1.1F,
                                1.1F),
                            handle_brush);
                    }
                    render_target_->SetTransform(previous_transform);
                    continue;
                }

                ComPtr<ID2D1SolidColorBrush> original_white_brush = white_brush_;

                bool is_active = (button.id == L"lock" && session_.annotation_locked_tool()) ||
                                 (session_.active_tool() != Tool::none &&
                                  ((button.id == L"select" && session_.active_tool() == Tool::select) ||
                                   (button.id == L"rect" && session_.active_tool() == Tool::rectangle) ||
                                   (button.id == L"ellipse" && session_.active_tool() == Tool::ellipse) ||
                                   (button.id == L"line" && session_.active_tool() == Tool::line) ||
                                   (button.id == L"arrow" && session_.active_tool() == Tool::arrow) ||
                                   (button.id == L"pen" && session_.active_tool() == Tool::pen) ||
                                   (button.id == L"mosaic" && session_.active_tool() == Tool::mosaic) ||
                                   (button.id == L"blur" && session_.active_tool() == Tool::blur) ||
                                   (button.id == L"highlight" && session_.active_tool() == Tool::highlight) ||
                                   (button.id == L"watermark" && session_.active_tool() == Tool::watermark) ||
                                   (button.id == L"text" && session_.active_tool() == Tool::text) ||
                                   (button.id == L"serial" && session_.active_tool() == Tool::serial) ||
                                   (button.id == L"eraser" && session_.active_tool() == Tool::eraser)));

                if (button.id == L"copy" && button.enabled) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        blue_brush_.Get());
                } else if (button.id == L"close" && is_hovered && button.enabled) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        danger_hover_bg_brush_.Get());
                } else if (is_active && button.enabled) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        active_bg_brush_.Get());
                } else if (is_hovered && button.enabled) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        hover_bg_brush_.Get());
                }

                if (!button.enabled) {
                    white_brush_ = disabled_brush_;
                } else if (button.id == L"close" && is_hovered) {
                    white_brush_ = red_brush_;
                } else if (is_active) {
                    white_brush_ = blue_brush_;
                }

                const float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                const float cy = bounds.top + (bounds.bottom - bounds.top) / 2.0F;

                if (button.id == L"lock") {
                    render_target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(cx - 7.0F, cy - 1.0F, cx + 7.0F, cy + 9.0F),
                            1.5F,
                            1.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    ComPtr<ID2D1PathGeometry> shackle;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(shackle.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(shackle->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(
                                D2D1::Point2F(cx - 5.0F, cy - 1.0F),
                                D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(D2D1::Point2F(cx - 5.0F, cy - 4.0F));
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx + 5.0F, cy - 4.0F),
                                D2D1::SizeF(5.0F, 5.0F),
                                0.0F,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL));
                            sink->AddLine(D2D1::Point2F(cx + 5.0F, cy - 1.0F));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            render_target_->DrawGeometry(
                                shackle.Get(),
                                white_brush_.Get(),
                                1.5F,
                                round_stroke_style_.Get());
                        }
                    }
                    render_target_->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy + 4.0F), 1.35F, 1.35F),
                        white_brush_.Get());
                } else if (button.id == L"select") {
                    constexpr float outer = 9.0F;
                    constexpr float arm = 4.0F;
                    for (const auto& segment : std::array{
                             std::pair{D2D1::Point2F(cx - outer, cy - outer), D2D1::Point2F(cx - outer + arm, cy - outer)},
                             std::pair{D2D1::Point2F(cx - outer, cy - outer), D2D1::Point2F(cx - outer, cy - outer + arm)},
                             std::pair{D2D1::Point2F(cx + outer, cy - outer), D2D1::Point2F(cx + outer - arm, cy - outer)},
                             std::pair{D2D1::Point2F(cx + outer, cy - outer), D2D1::Point2F(cx + outer, cy - outer + arm)},
                             std::pair{D2D1::Point2F(cx - outer, cy + outer), D2D1::Point2F(cx - outer + arm, cy + outer)},
                             std::pair{D2D1::Point2F(cx - outer, cy + outer), D2D1::Point2F(cx - outer, cy + outer - arm)},
                             std::pair{D2D1::Point2F(cx + outer, cy + outer), D2D1::Point2F(cx + outer - arm, cy + outer)},
                             std::pair{D2D1::Point2F(cx + outer, cy + outer), D2D1::Point2F(cx + outer, cy + outer - arm)}}) {
                        render_target_->DrawLine(
                            segment.first,
                            segment.second,
                            white_brush_.Get(),
                            1.5F,
                            round_stroke_style_.Get());
                    }
                    render_target_->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0F, 3.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"rect") {
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 9.0F, cy - 8.0F, cx + 9.0F, cy + 8.0F), 2.0F, 2.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                } else if (button.id == L"ellipse") {
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0F, 9.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                } else if (button.id == L"line") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 9.0F, cy + 9.0F), D2D1::Point2F(cx + 9.0F, cy - 9.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                } else if (button.id == L"arrow") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 9.0F, cy + 9.0F), D2D1::Point2F(cx + 8.0F, cy - 8.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + 8.0F, cy - 8.0F), D2D1::Point2F(cx + 2.0F, cy - 8.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + 8.0F, cy - 8.0F), D2D1::Point2F(cx + 8.0F, cy - 2.0F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                } else if (button.id == L"pen") {
                    ComPtr<ID2D1PathGeometry> pen_geom;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(pen_geom.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(pen_geom->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(
                                D2D1::Point2F(cx - 8.0F, cy + 8.0F),
                                D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(D2D1::Point2F(cx - 5.5F, cy + 1.0F));
                            sink->AddLine(D2D1::Point2F(cx + 4.5F, cy - 9.0F));
                            sink->AddLine(D2D1::Point2F(cx + 9.0F, cy - 4.5F));
                            sink->AddLine(D2D1::Point2F(cx - 1.0F, cy + 5.5F));
                            sink->AddLine(D2D1::Point2F(cx - 8.0F, cy + 8.0F));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            render_target_->DrawGeometry(
                                pen_geom.Get(),
                                white_brush_.Get(),
                                1.5F,
                                round_stroke_style_.Get());
                        }
                    }
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 5.5F, cy + 1.0F),
                        D2D1::Point2F(cx - 1.0F, cy + 5.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"mosaic") {
                    constexpr float cell = 7.0F;
                    render_target_->FillRectangle(
                        D2D1::RectF(cx - cell, cy - cell, cx, cy),
                        white_brush_.Get());
                    render_target_->FillRectangle(
                        D2D1::RectF(cx, cy, cx + cell, cy + cell),
                        white_brush_.Get());
                    render_target_->DrawRectangle(
                        D2D1::RectF(cx, cy - cell, cx + cell, cy),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawRectangle(
                        D2D1::RectF(cx - cell, cy, cx, cy + cell),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"blur") {
                    render_target_->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5F, 8.5F),
                        white_brush_.Get(),
                        1.5F,
                        dashed_stroke_style_.Get());
                    render_target_->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.5F, 4.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"highlight") {
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 5.5F, cy + 3.5F),
                        D2D1::Point2F(cx + 5.0F, cy - 7.0F),
                        white_brush_.Get(),
                        4.0F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx + 5.0F, cy - 7.0F),
                        D2D1::Point2F(cx + 8.0F, cy - 4.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 5.5F, cy + 3.5F),
                        D2D1::Point2F(cx - 8.0F, cy + 6.0F),
                        white_brush_.Get(),
                        2.0F,
                        round_stroke_style_.Get());
                    ComPtr<ID2D1SolidColorBrush> hl_brush;
                    if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                            D2D1::ColorF(0xFADB14, 0.4F),
                            hl_brush.GetAddressOf())) &&
                        hl_brush) {
                        render_target_->DrawLine(D2D1::Point2F(cx - 9.0F, cy + 8.0F), D2D1::Point2F(cx + 9.0F, cy + 8.0F), hl_brush.Get(), 3.0F, round_stroke_style_.Get());
                    }
                } else if (button.id == L"watermark") {
                    render_target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(cx - 8.0F, cy - 8.0F, cx + 8.0F, cy + 8.0F),
                            2.0F,
                            2.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.5F, cy + 4.5F),
                        D2D1::Point2F(cx, cy),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy),
                        D2D1::Point2F(cx + 4.5F, cy + 4.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.5F, cy),
                        D2D1::Point2F(cx, cy - 4.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy - 4.5F),
                        D2D1::Point2F(cx + 4.5F, cy),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"text") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.5F, cy - 7.5F), D2D1::Point2F(cx + 7.5F, cy - 7.5F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx, cy - 7.5F), D2D1::Point2F(cx, cy + 7.5F), white_brush_.Get(), 1.5F, round_stroke_style_.Get());
                } else if (button.id == L"serial") {
                    for (const float y : {-6.0F, 0.0F, 6.0F}) {
                        render_target_->FillEllipse(
                            D2D1::Ellipse(
                                D2D1::Point2F(cx - 6.0F, cy + y),
                                1.75F,
                                1.75F),
                            white_brush_.Get());
                        render_target_->DrawLine(
                            D2D1::Point2F(cx - 1.0F, cy + y),
                            D2D1::Point2F(cx + 8.0F, cy + y),
                            white_brush_.Get(),
                            1.5F,
                            round_stroke_style_.Get());
                    }
                } else if (button.id == L"eraser") {
                    ComPtr<ID2D1PathGeometry> eraser_geom;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(eraser_geom.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(eraser_geom->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(
                                D2D1::Point2F(cx - 9.0F, cy + 2.5F),
                                D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(D2D1::Point2F(cx + 1.5F, cy - 8.0F));
                            sink->AddLine(D2D1::Point2F(cx + 9.0F, cy - 0.5F));
                            sink->AddLine(D2D1::Point2F(cx + 1.0F, cy + 7.5F));
                            sink->AddLine(D2D1::Point2F(cx - 4.0F, cy + 7.5F));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            render_target_->DrawGeometry(
                                eraser_geom.Get(),
                                white_brush_.Get(),
                                1.5F,
                                round_stroke_style_.Get());
                        }
                    }
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.0F, cy - 2.5F),
                        D2D1::Point2F(cx + 3.5F, cy + 5.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.0F, cy + 7.5F),
                        D2D1::Point2F(cx + 8.0F, cy + 7.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"undo") {
                    ComPtr<ID2D1PathGeometry> undo_geom;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(undo_geom.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(undo_geom->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(D2D1::Point2F(cx + 6.5F, cy + 4.5F), D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx - 3.5F, cy - 1.5F),
                                D2D1::SizeF(6.5F, 6.5F),
                                0.0f,
                                D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL
                            ));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            render_target_->DrawGeometry(undo_geom.Get(), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                        }
                    }
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.5F, cy - 1.5F), D2D1::Point2F(cx - 3.5F, cy - 6.0F), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.5F, cy - 1.5F), D2D1::Point2F(cx + 1.0F, cy - 1.5F), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                } else if (button.id == L"redo") {
                    ComPtr<ID2D1PathGeometry> redo_geom;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(redo_geom.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(redo_geom->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(D2D1::Point2F(cx - 6.5F, cy + 4.5F), D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx + 3.5F, cy - 1.5F),
                                D2D1::SizeF(6.5F, 6.5F),
                                0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL
                            ));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            render_target_->DrawGeometry(redo_geom.Get(), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                        }
                    }
                    render_target_->DrawLine(D2D1::Point2F(cx + 3.5F, cy - 1.5F), D2D1::Point2F(cx + 3.5F, cy - 6.0F), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + 3.5F, cy - 1.5F), D2D1::Point2F(cx - 1.0F, cy - 1.5F), white_brush_.Get(), 1.6F, round_stroke_style_.Get());
                } else if (button.id == L"ocr") {
                    constexpr float len = 5.0F;
                    constexpr float r = 9.0F;
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx - r + len, cy - r), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx - r, cy - r + len), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx + r - len, cy - r), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx + r, cy - r + len), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy + r), D2D1::Point2F(cx - r + len, cy + r), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy + r), D2D1::Point2F(cx - r, cy + r - len), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy + r), D2D1::Point2F(cx + r - len, cy + r), white_brush_.Get(), 1.4F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy + r), D2D1::Point2F(cx + r, cy + r - len), white_brush_.Get(), 1.4F, round_stroke_style_.Get());

                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.0F, cy - 4.0F),
                        D2D1::Point2F(cx + 4.0F, cy - 4.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.0F, cy),
                        D2D1::Point2F(cx + 4.0F, cy),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 4.0F, cy + 4.0F),
                        D2D1::Point2F(cx, cy + 4.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"copy") {
                    render_target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(cx - 3.0F, cy - 3.0F, cx + 8.0F, cy + 8.0F),
                            2.0F,
                            2.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    ComPtr<ID2D1PathGeometry> copy_back;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(copy_back.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(copy_back->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(
                                D2D1::Point2F(cx - 6.0F, cy + 3.0F),
                                D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(D2D1::Point2F(cx - 7.0F, cy + 3.0F));
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx - 9.0F, cy + 1.0F),
                                D2D1::SizeF(2.0F, 2.0F),
                                0.0F,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL));
                            sink->AddLine(D2D1::Point2F(cx - 9.0F, cy - 7.0F));
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx - 7.0F, cy - 9.0F),
                                D2D1::SizeF(2.0F, 2.0F),
                                0.0F,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL));
                            sink->AddLine(D2D1::Point2F(cx + 1.0F, cy - 9.0F));
                            sink->AddArc(D2D1::ArcSegment(
                                D2D1::Point2F(cx + 3.0F, cy - 7.0F),
                                D2D1::SizeF(2.0F, 2.0F),
                                0.0F,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL));
                            sink->AddLine(D2D1::Point2F(cx + 3.0F, cy - 6.0F));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            render_target_->DrawGeometry(
                                copy_back.Get(),
                                white_brush_.Get(),
                                1.5F,
                                round_stroke_style_.Get());
                        }
                    }
                } else if (button.id == L"save") {
                    ComPtr<ID2D1PathGeometry> save_geom;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(save_geom.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(save_geom->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(
                                D2D1::Point2F(cx - 8.0F, cy - 9.0F),
                                D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(D2D1::Point2F(cx + 3.0F, cy - 9.0F));
                            sink->AddLine(D2D1::Point2F(cx + 8.0F, cy - 4.0F));
                            sink->AddLine(D2D1::Point2F(cx + 8.0F, cy + 9.0F));
                            sink->AddLine(D2D1::Point2F(cx - 8.0F, cy + 9.0F));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            render_target_->DrawGeometry(
                                save_geom.Get(),
                                white_brush_.Get(),
                                1.5F,
                                round_stroke_style_.Get());
                        }
                    }
                    render_target_->DrawRectangle(
                        D2D1::RectF(cx - 4.5F, cy - 9.0F, cx + 3.0F, cy - 4.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(cx - 4.5F, cy + 1.0F, cx + 4.5F, cy + 9.0F),
                            1.0F,
                            1.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"scroll") {
                    render_target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(cx - 7.0F, cy - 9.0F, cx + 7.0F, cy + 3.0F),
                            1.5F,
                            1.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy + 3.0F),
                        D2D1::Point2F(cx, cy + 9.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy + 9.0F),
                        D2D1::Point2F(cx - 3.0F, cy + 6.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy + 9.0F),
                        D2D1::Point2F(cx + 3.0F, cy + 6.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"pin") {
                    render_target_->DrawLine(
                        D2D1::Point2F(cx + 3.0F, cy - 8.0F),
                        D2D1::Point2F(cx - 8.0F, cy + 3.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, cy - 9.0F),
                        D2D1::Point2F(cx + 9.0F, cy),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx + 5.5F, cy + 1.5F),
                        D2D1::Point2F(cx + 1.5F, cy - 2.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 1.5F, cy - 5.5F),
                        D2D1::Point2F(cx + 2.5F, cy - 1.5F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx - 8.0F, cy + 3.0F),
                        D2D1::Point2F(cx - 10.0F, cy + 10.0F),
                        white_brush_.Get(),
                        1.5F,
                        round_stroke_style_.Get());
                } else if (button.id == L"close") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy - 6.0F), D2D1::Point2F(cx + 6.0F, cy + 6.0F), white_brush_.Get(), 2.0F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy + 6.0F), D2D1::Point2F(cx + 6.0F, cy - 6.0F), white_brush_.Get(), 2.0F, round_stroke_style_.Get());
                }
                render_target_->SetTransform(previous_transform);
                white_brush_ = original_white_brush;
            }
        }

        // Draw sub-toolbar
        if (!session_.sub_toolbar().empty()) {
            D2D1_RECT_F sub_bg_rect =
                local_rect(padded_buttons_bounds(session_.sub_toolbar(), ui.px(8)));
            D2D1_RECT_F sub_shadow_rect = sub_bg_rect;
            sub_shadow_rect.left -= ui.px(1.0F);
            sub_shadow_rect.right += ui.px(1.0F);
            sub_shadow_rect.top += ui.px(2.0F);
            sub_shadow_rect.bottom += ui.px(3.0F);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    sub_shadow_rect, ui.px(9.0F), ui.px(9.0F)),
                toolbar_shadow_brush_.Get());
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    sub_bg_rect, ui.px(8.0F), ui.px(8.0F)),
                toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    sub_bg_rect, ui.px(8.0F), ui.px(8.0F)),
                toolbar_border_brush_.Get(),
                ui.px(1.0F));

            for (const auto& button : session_.sub_toolbar()) {
                if (!intersect(button.bounds, monitor_.bounds)) {
                    continue;
                }
                D2D1_RECT_F bounds = local_rect(button.bounds);
                D2D1_MATRIX_3X2_F previous_transform{};
                render_target_->GetTransform(&previous_transform);
                if (ui_scale != 1.0F) {
                    const float center_x = (bounds.left + bounds.right) * 0.5F;
                    const float center_y = (bounds.top + bounds.bottom) * 0.5F;
                    bounds = D2D1::RectF(
                        center_x + (bounds.left - center_x) / ui_scale,
                        center_y + (bounds.top - center_y) / ui_scale,
                        center_x + (bounds.right - center_x) / ui_scale,
                        center_y + (bounds.bottom - center_y) / ui_scale);
                    render_target_->SetTransform(D2D1::Matrix3x2F::Scale(
                        D2D1::SizeF(ui_scale, ui_scale),
                        D2D1::Point2F(center_x, center_y)));
                }
                if (button.id == L"|") {
                    float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, bounds.top + 8.0F),
                        D2D1::Point2F(cx, bounds.bottom - 8.0F),
                        toolbar_border_brush_.Get(),
                        1.2F,
                        round_stroke_style_.Get()
                    );
                    render_target_->SetTransform(previous_transform);
                    continue;
                }
                bool is_selected = false;
                if (button.id == L"width_small" && session_.active_width() == 2.0F) is_selected = true;
                else if (button.id == L"width_medium" && session_.active_width() == 4.0F) is_selected = true;
                else if (button.id == L"width_large" && session_.active_width() == 8.0F) is_selected = true;
                else if (button.id == L"alpha_low" && session_.active_highlight_alpha() == 64) is_selected = true;
                else if (button.id == L"alpha_medium" && session_.active_highlight_alpha() == 96) is_selected = true;
                else if (button.id == L"alpha_high" && session_.active_highlight_alpha() == 144) is_selected = true;
                else if (button.id == L"color_red" && session_.active_color() == RGB(245, 34, 45)) is_selected = true;
                else if (button.id == L"color_green" && session_.active_color() == RGB(82, 196, 26)) is_selected = true;
                else if (button.id == L"color_blue" && session_.active_color() == RGB(0, 102, 255)) is_selected = true;
                else if (button.id == L"color_yellow" && session_.active_color() == RGB(250, 219, 20)) is_selected = true;
                else if (button.id == L"color_black" && session_.active_color() == RGB(0, 0, 0)) is_selected = true;
                else if (button.id == L"color_gray" && session_.active_color() == RGB(143, 149, 158)) is_selected = true;
                else if (button.id == L"color_white" && session_.active_color() == RGB(255, 255, 255)) is_selected = true;
                else if (button.id == L"text_style_normal" && session_.active_text_style() == TextStyle::normal) is_selected = true;
                else if (button.id == L"text_style_dark" && session_.active_text_style() == TextStyle::dark) is_selected = true;
                else if (button.id == L"text_style_outline" && session_.active_text_style() == TextStyle::outline) is_selected = true;
                else if (button.id == L"effect_mosaic" && !session_.mosaic_is_blur()) is_selected = true;
                else if (button.id == L"effect_blur" && session_.mosaic_is_blur()) is_selected = true;
                else if (button.id == L"mode_smear" && !session_.mosaic_is_rect()) is_selected = true;
                else if (button.id == L"mode_rect" && session_.mosaic_is_rect()) is_selected = true;
                else if (button.id == L"fill_outline" && session_.active_fill_style() == ShapeFillStyle::outline) is_selected = true;
                else if (button.id == L"fill_translucent" && session_.active_fill_style() == ShapeFillStyle::translucent) is_selected = true;
                else if (button.id == L"stroke_solid" && session_.active_stroke_pattern() == StrokePattern::solid) is_selected = true;
                else if (button.id == L"stroke_dashed" && session_.active_stroke_pattern() == StrokePattern::dashed) is_selected = true;
                else if (button.id == L"corner_square" && !session_.active_rectangle_rounded()) is_selected = true;
                else if (button.id == L"corner_round" && session_.active_rectangle_rounded()) is_selected = true;
                else if (button.id == L"head_forward" && session_.active_arrow_head_style() == ArrowHeadStyle::forward) is_selected = true;
                else if (button.id == L"head_reverse" && session_.active_arrow_head_style() == ArrowHeadStyle::reverse) is_selected = true;
                else if (button.id == L"head_both" && session_.active_arrow_head_style() == ArrowHeadStyle::both) is_selected = true;
                else if (button.id == L"color_custom" &&
                         session_.active_color() != RGB(245, 34, 45) &&
                         session_.active_color() != RGB(82, 196, 26) &&
                         session_.active_color() != RGB(0, 102, 255) &&
                         session_.active_color() != RGB(250, 219, 20) &&
                         session_.active_color() != RGB(0, 0, 0) &&
                         session_.active_color() != RGB(143, 149, 158) &&
                         session_.active_color() != RGB(255, 255, 255)) {
                    is_selected = true;
                }

                const bool is_hovered =
                    session_.hovered_button_id() == button.id &&
                    button.enabled;

                if ((button.id.starts_with(L"width_") || button.id.starts_with(L"alpha_") || button.id.starts_with(L"effect_") ||
                     button.id.starts_with(L"mode_") || button.id.starts_with(L"text_style_") ||
                     button.id.starts_with(L"fill_") || button.id.starts_with(L"stroke_") ||
                     button.id.starts_with(L"corner_") || button.id.starts_with(L"head_")) && is_selected) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        active_bg_brush_.Get());
                } else if (is_hovered) {
                    render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(bounds, 6.0F, 6.0F),
                        hover_bg_brush_.Get());
                }

                const float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                const float cy = bounds.top + (bounds.bottom - bounds.top) / 2.0F;
                ComPtr<ID2D1SolidColorBrush> original_sub_toolbar_brush =
                    white_brush_;
                if (!button.enabled) {
                    white_brush_ = disabled_brush_;
                }

                if (button.id.starts_with(L"color_")) {
                    COLORREF color = RGB(0, 102, 255);
                    if (button.id == L"color_red") color = RGB(245, 34, 45);
                    else if (button.id == L"color_green") color = RGB(82, 196, 26);
                    else if (button.id == L"color_yellow") color = RGB(250, 219, 20);
                    else if (button.id == L"color_black") color = RGB(0, 0, 0);
                    else if (button.id == L"color_gray") color = RGB(143, 149, 158);
                    else if (button.id == L"color_white") color = RGB(255, 255, 255);
                    else if (button.id == L"color_custom") color = session_.custom_color();

                    ComPtr<ID2D1SolidColorBrush> brush;
                    if (FAILED(render_target_->CreateSolidColorBrush(
                            D2D1::ColorF(GetRValue(color) / 255.0F,
                                         GetGValue(color) / 255.0F,
                                         GetBValue(color) / 255.0F),
                            brush.GetAddressOf())) ||
                        !brush) {
                        brush = blue_brush_;
                    }

                    const D2D1_ELLIPSE swatch =
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5F, 8.5F);
                    render_target_->FillEllipse(swatch, brush.Get());
                    render_target_->DrawEllipse(
                        swatch,
                        toolbar_border_brush_.Get(),
                        1.0F);
                    if (is_selected) {
                        render_target_->DrawEllipse(
                            D2D1::Ellipse(
                                D2D1::Point2F(cx, cy),
                                11.0F,
                                11.0F),
                            blue_brush_.Get(),
                            2.0F);
                    }

                    if (button.id == L"color_custom") {
                        COLORREF plus_color = RGB(255, 255, 255);
                        double brightness = (GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114) / 1000.0;
                        if (brightness > 180.0) {
                            plus_color = RGB(0, 0, 0);
                        }
                        ComPtr<ID2D1SolidColorBrush> plus_brush;
                        if (FAILED(render_target_->CreateSolidColorBrush(
                                D2D1::ColorF(GetRValue(plus_color) / 255.0F,
                                             GetGValue(plus_color) / 255.0F,
                                             GetBValue(plus_color) / 255.0F),
                                plus_brush.GetAddressOf())) ||
                            !plus_brush) {
                            plus_brush = plus_color == RGB(0, 0, 0)
                                             ? black_brush_
                                             : true_white_brush_;
                        }
                        render_target_->DrawLine(D2D1::Point2F(cx - 3.0F, cy), D2D1::Point2F(cx + 3.0F, cy), plus_brush.Get(), 1.5F);
                        render_target_->DrawLine(D2D1::Point2F(cx, cy - 3.0F), D2D1::Point2F(cx, cy + 3.0F), plus_brush.Get(), 1.5F);
                    }

                } else if (button.id.starts_with(L"width_")) {
                    float w = 2.0F;
                    if (button.id == L"width_medium") w = 4.0F;
                    else if (button.id == L"width_large") w = 8.0F;

                    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w / 2.0F + 1.0F, w / 2.0F + 1.0F), white_brush_.Get());
                } else if (button.id.starts_with(L"text_style_")) {
                    auto style_format = create_text_format(
                        L"Segoe UI",
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        22.0F);
                    if (style_format) {
                        style_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        style_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }

                    ComPtr<ID2D1SolidColorBrush> icon_brush = (is_selected && is_light_theme_) ? blue_brush_ : white_brush_;
                    if (button.id == L"text_style_dark") {
                        render_target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(cx - 11.0F, cy - 11.0F, cx + 11.0F, cy + 11.0F), 3.0F, 3.0F),
                            black_brush_.Get());
                        if (style_format) {
                            render_target_->DrawTextW(L"T", 1, style_format.Get(), D2D1::RectF(cx - 11.0F, cy - 13.0F, cx + 11.0F, cy + 11.0F), true_white_brush_.Get());
                        }
                    } else if (button.id == L"text_style_outline" && style_format) {
                        for (float dy : {-1.5F, 1.5F}) {
                            for (float dx : {-1.5F, 1.5F}) {
                                render_target_->DrawTextW(L"T", 1, style_format.Get(), D2D1::RectF(cx - 11.0F + dx, cy - 13.0F + dy, cx + 11.0F + dx, cy + 11.0F + dy), true_white_brush_.Get());
                            }
                        }
                        render_target_->DrawTextW(L"T", 1, style_format.Get(), D2D1::RectF(cx - 11.0F, cy - 13.0F, cx + 11.0F, cy + 11.0F), icon_brush.Get());
                    } else if (style_format) {
                        render_target_->DrawTextW(L"T", 1, style_format.Get(), D2D1::RectF(cx - 11.0F, cy - 13.0F, cx + 11.0F, cy + 11.0F), icon_brush.Get());
                    }
                } else if (button.id == L"text_size_btn") {
                    auto dropdown_text_format = create_text_format(
                        L"Segoe UI",
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        12.5F);
                    if (dropdown_text_format) {
                        dropdown_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        dropdown_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        render_target_->DrawTextW(button.label.c_str(), static_cast<UINT32>(button.label.size()), dropdown_text_format.Get(), D2D1::RectF(bounds.left, bounds.top, bounds.right - 14.0F, bounds.bottom), white_brush_.Get());
                    }
                    render_target_->DrawLine(D2D1::Point2F(cx + 14.0F, cy - 1.5F), D2D1::Point2F(cx + 17.0F, cy + 1.5F), white_brush_.Get(), 1.2F, round_stroke_style_.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx + 17.0F, cy + 1.5F), D2D1::Point2F(cx + 20.0F, cy - 1.5F), white_brush_.Get(), 1.2F, round_stroke_style_.Get());
                } else if (button.id == L"mosaic_strength_slider" || button.id == L"watermark_opacity_slider") {
                    const bool watermark_slider = button.id == L"watermark_opacity_slider";
                    const int value = watermark_slider ? session_.watermark_opacity() : session_.mosaic_strength();
                    const wchar_t* label = watermark_slider
                                               ? L"水印浓度"
                                               : (session_.mosaic_is_blur()
                                                      ? L"模糊强度"
                                                      : L"马赛克强度");

                    auto slider_format = create_text_format(
                        L"Microsoft YaHei",
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        12.0F);
                    if (slider_format) {
                        slider_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        slider_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        render_target_->DrawTextW(label,
                                                  static_cast<UINT32>(wcslen(label)),
                                                  slider_format.Get(),
                                                  D2D1::RectF(bounds.left + 6.0F, bounds.top, bounds.left + 86.0F, bounds.bottom),
                                                  white_brush_.Get());
                    }

                    const float track_left = bounds.left + 88.0F;
                    const float track_right = bounds.right - 42.0F;
                    const float track_y = cy;
                    ComPtr<ID2D1SolidColorBrush> track_brush;
                    if (FAILED(render_target_->CreateSolidColorBrush(
                            D2D1::ColorF(0xC9CDD4),
                            track_brush.GetAddressOf())) ||
                        !track_brush) {
                        track_brush = toolbar_border_brush_;
                    }
                    render_target_->DrawLine(D2D1::Point2F(track_left, track_y),
                                             D2D1::Point2F(track_right, track_y),
                                             track_brush.Get(),
                                             3.0F,
                                             round_stroke_style_.Get());
                    const float knob_x = track_left + (track_right - track_left) * (static_cast<float>(value) / 100.0F);
                    render_target_->DrawLine(D2D1::Point2F(track_left, track_y),
                                             D2D1::Point2F(knob_x, track_y),
                                             blue_brush_.Get(),
                                             3.0F,
                                             round_stroke_style_.Get());
                    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, track_y), 5.0F, 5.0F), true_white_brush_.Get());
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, track_y), 5.0F, 5.0F), blue_brush_.Get(), 2.0F);

                    const std::wstring value_text = std::format(L"{} %", value);
                    if (slider_format) {
                        slider_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        render_target_->DrawTextW(value_text.c_str(),
                                                  static_cast<UINT32>(value_text.size()),
                                                  slider_format.Get(),
                                                  D2D1::RectF(bounds.right - 40.0F, bounds.top, bounds.right - 4.0F, bounds.bottom),
                                                  white_brush_.Get());
                    }
                } else if (button.id == L"watermark_text" || button.id == L"watermark_apply" || button.id == L"watermark_clear") {
                    auto label_format = create_text_format(
                        L"Microsoft YaHei",
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        12.5F);
                    std::wstring label = button.label;
                    if (button.id == L"watermark_text" && label.size() > 6) {
                        label = label.substr(0, 6) + L"...";
                    }
                    if (label_format) {
                        label_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        label_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        render_target_->DrawTextW(label.c_str(),
                                                  static_cast<UINT32>(label.size()),
                                                  label_format.Get(),
                                                  bounds,
                                                  white_brush_.Get());
                    }
                } else if (button.id.starts_with(L"effect_") || button.id.starts_with(L"mode_") ||
                           button.id.starts_with(L"fill_") || button.id.starts_with(L"stroke_") ||
                           button.id.starts_with(L"corner_") || button.id.starts_with(L"head_")) {
                    auto segment_format = create_text_format(
                        L"Segoe UI",
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        12.0F);
                    if (segment_format) {
                        segment_format->SetTextAlignment(
                            DWRITE_TEXT_ALIGNMENT_CENTER);
                        segment_format->SetParagraphAlignment(
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        render_target_->DrawTextW(
                            button.label.c_str(),
                            static_cast<UINT32>(button.label.size()),
                            segment_format.Get(),
                            bounds,
                            is_selected
                                ? static_cast<ID2D1Brush*>(blue_brush_.Get())
                                : static_cast<ID2D1Brush*>(white_brush_.Get()));
                    }
                } else if (button.id.starts_with(L"alpha_")) {
                    float alpha = 0.25F;
                    if (button.id == L"alpha_medium") alpha = 0.55F;
                    else if (button.id == L"alpha_high") alpha = 0.85F;
                    ComPtr<ID2D1SolidColorBrush> alpha_brush;
                    if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                            D2D1::ColorF(0xFADB14, alpha),
                            alpha_brush.GetAddressOf())) &&
                        alpha_brush) {
                        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.0F, 6.0F), alpha_brush.Get());
                    }
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.0F, 6.0F), white_brush_.Get(), 1.2F, round_stroke_style_.Get());
                }
                render_target_->SetTransform(previous_transform);
                white_brush_ = original_sub_toolbar_brush;
            }
        }

        // The capture rectangle is frozen once annotations exist. Keep object
        // handles visible, but do not advertise the disabled rectangle resize.
        if (session_.annotations().empty()) {
            for (const auto& pt : handles) {
                D2D1_ELLIPSE circle =
                    D2D1::Ellipse(pt, ui.px(4.0F), ui.px(4.0F));
                render_target_->FillEllipse(circle, blue_brush_.Get());
                render_target_->DrawEllipse(
                    circle,
                    true_white_brush_.Get(),
                    ui.px(1.2F));
            }
        }

        const bool size_hovered =
            session_.can_edit_selection_size() &&
            session_.dimension_badge_hovered();
        const std::wstring dimensions = size_hovered
                                            ? std::format(
                                                  L" X {}  Y {}  ·  {} × {}  ·  F2 ",
                                                  session_.selection().left,
                                                  session_.selection().top,
                                                  session_.selection().width(),
                                                  session_.selection().height())
                                            : std::format(
                                                  L" X {}  Y {}  ·  {} × {} ",
                                                  session_.selection().left,
                                                  session_.selection().top,
                                                  session_.selection().width(),
                                                  session_.selection().height());
        const RectI text_bounds = session_.dimension_badge_bounds();
        if (format && intersect(text_bounds, monitor_.bounds)) {
            const auto bounds = local_rect(text_bounds);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(bounds, ui.px(12.0F), ui.px(12.0F)),
                toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(bounds, ui.px(12.0F), ui.px(12.0F)),
                size_hovered
                    ? static_cast<ID2D1Brush*>(blue_brush_.Get())
                    : static_cast<ID2D1Brush*>(toolbar_border_brush_.Get()),
                size_hovered ? ui.px(1.5F) : ui.px(1.0F));
            render_target_->DrawTextW(dimensions.c_str(),
                                      static_cast<UINT32>(dimensions.size()),
                                      format.Get(),
                                      bounds,
                                      white_brush_.Get());
        }
        white_brush_ = original_overlay_white_brush;
    }

    // Draw high-precision pixel magnifier
    if (!session_.selection_complete() || session_.dragging_selection()) {
        POINT cursor_pos = session_.cursor_pos();
        if (monitor_.bounds.contains(cursor_pos)) {
            int cx = cursor_pos.x - monitor_.bounds.left;
            int cy = cursor_pos.y - monitor_.bounds.top;

            constexpr int grid_cells = 15;
            const int cell_size = ui.px(10);
            const int grid_size = grid_cells * cell_size;
            const int text_height = ui.px(72);
            const int mag_width = grid_size;
            const int mag_height = grid_size + text_height;
            const int cursor_gap = ui.px(20);

            int mx = cx + cursor_gap;
            int my = cy + cursor_gap;
            if (mx + mag_width > monitor_.bounds.width()) {
                mx = cx - mag_width - cursor_gap;
            }
            if (my + mag_height > monitor_.bounds.height()) {
                my = cy - mag_height - cursor_gap;
            }

            D2D1_RECT_F mag_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + mag_height)
            );
            render_target_->FillRectangle(mag_rect, toolbar_bg_brush_.Get());

            for (int dy = -grid_cells/2; dy <= grid_cells/2; ++dy) {
                for (int dx = -grid_cells/2; dx <= grid_cells/2; ++dx) {
                    int px = cursor_pos.x + dx;
                    int py = cursor_pos.y + dy;
                    COLORREF color = session_.get_pixel_color(px, py);

                    ComPtr<ID2D1SolidColorBrush> cell_brush;
                    if (FAILED(render_target_->CreateSolidColorBrush(
                            D2D1::ColorF(GetRValue(color) / 255.0f,
                                         GetGValue(color) / 255.0f,
                                         GetBValue(color) / 255.0f),
                            cell_brush.GetAddressOf())) ||
                        !cell_brush) {
                        cell_brush = black_brush_;
                    }

                    D2D1_RECT_F cell_rect = D2D1::RectF(
                        static_cast<float>(mx + (dx + grid_cells/2) * cell_size),
                        static_cast<float>(my + (dy + grid_cells/2) * cell_size),
                        static_cast<float>(mx + (dx + grid_cells/2 + 1) * cell_size),
                        static_cast<float>(my + (dy + grid_cells/2 + 1) * cell_size)
                    );
                    render_target_->FillRectangle(cell_rect, cell_brush.Get());
                }
            }

            // Draw faint grid lines between pixels
            ComPtr<ID2D1SolidColorBrush> grid_line_brush;
            if (FAILED(render_target_->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f),
                    grid_line_brush.GetAddressOf())) ||
                !grid_line_brush) {
                grid_line_brush = true_white_brush_;
            }
            for (int i = 0; i <= grid_cells; ++i) {
                float line_coord = static_cast<float>(i * cell_size);
                // Horizontal line
                render_target_->DrawLine(
                    D2D1::Point2F(static_cast<float>(mx), static_cast<float>(my + line_coord)),
                    D2D1::Point2F(static_cast<float>(mx + grid_size), static_cast<float>(my + line_coord)),
                    grid_line_brush.Get(), ui.px(0.5F));
                // Vertical line
                render_target_->DrawLine(
                    D2D1::Point2F(static_cast<float>(mx + line_coord), static_cast<float>(my)),
                    D2D1::Point2F(static_cast<float>(mx + line_coord), static_cast<float>(my + grid_size)),
                    grid_line_brush.Get(), ui.px(0.5F));
            }

            int center_cell_x = mx + (grid_cells/2) * cell_size;
            int center_cell_y = my + (grid_cells/2) * cell_size;

            D2D1_RECT_F center_rect = D2D1::RectF(
                static_cast<float>(center_cell_x),
                static_cast<float>(center_cell_y),
                static_cast<float>(center_cell_x + cell_size),
                static_cast<float>(center_cell_y + cell_size)
            );

            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(mx), static_cast<float>(center_cell_y + cell_size/2)),
                D2D1::Point2F(static_cast<float>(center_cell_x), static_cast<float>(center_cell_y + cell_size/2)),
                blue_brush_.Get(), ui.px(1.0F)
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size), static_cast<float>(center_cell_y + cell_size/2)),
                D2D1::Point2F(static_cast<float>(mx + grid_size), static_cast<float>(center_cell_y + cell_size/2)),
                blue_brush_.Get(), ui.px(1.0F)
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(my)),
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(center_cell_y)),
                blue_brush_.Get(), ui.px(1.0F)
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(center_cell_y + cell_size)),
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(my + grid_size)),
                blue_brush_.Get(), ui.px(1.0F)
            );

            render_target_->DrawRectangle(
                center_rect, blue_brush_.Get(), ui.px(1.0F));

            COLORREF center_color = session_.get_pixel_color(cursor_pos.x, cursor_pos.y);
            std::wstring coord_text;
            if (!session_.selection().empty()) {
                coord_text = std::format(L"{}, {} ({}x{})", cursor_pos.x, cursor_pos.y, session_.selection().width(), session_.selection().height());
            } else {
                coord_text = std::format(L"{}, {}", cursor_pos.x, cursor_pos.y);
            }

            std::wstring primary_color;
            if (session_.color_format_hex()) {
                primary_color = std::format(L"#{:02X}{:02X}{:02X}", GetRValue(center_color), GetGValue(center_color), GetBValue(center_color));
            } else {
                primary_color = std::format(L"rgb({},{},{})", GetRValue(center_color), GetGValue(center_color), GetBValue(center_color));
            }

            std::wstring hint_text = L"C 复制 | Shift 切换";

            auto mag_format = create_text_format(
                L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, ui.px(12.0F));
            if (mag_format) {
                mag_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                mag_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            // Line 1: coordinates
            D2D1_RECT_F coord_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + ui.px(4)),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + grid_size + ui.px(24))
            );
            if (mag_format) {
                render_target_->DrawTextW(coord_text.c_str(), static_cast<UINT32>(coord_text.size()), mag_format.Get(), coord_rect, white_brush_.Get());
            }

            // Line 2: color value (Hex or RGB)
            D2D1_RECT_F color_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + ui.px(24)),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + grid_size + ui.px(44))
            );
            if (mag_format) {
                render_target_->DrawTextW(primary_color.c_str(), static_cast<UINT32>(primary_color.size()), mag_format.Get(), color_rect, white_brush_.Get());
            }

            // Line 3: hint
            auto hint_format = create_text_format(
                L"Microsoft YaHei",
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                ui.px(10.5F));
            if (hint_format) {
                hint_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                hint_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            ComPtr<ID2D1SolidColorBrush> hint_brush;
            if (FAILED(render_target_->CreateSolidColorBrush(
                    D2D1::ColorF(0.6f, 0.65f, 0.7f),
                    hint_brush.GetAddressOf())) ||
                !hint_brush) {
                hint_brush = white_brush_;
            }

            D2D1_RECT_F hint_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + ui.px(44)),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + mag_height - ui.px(4))
            );
            if (hint_format) {
                render_target_->DrawTextW(hint_text.c_str(), static_cast<UINT32>(hint_text.size()), hint_format.Get(), hint_rect, hint_brush.Get());
            }

            render_target_->DrawRectangle(
                mag_rect, toolbar_border_brush_.Get(), ui.px(1.0F));
        }
    }

    // Draw dynamic brush size cursor indicator
    if (session_.selection_complete() && !session_.is_over_toolbar(session_.cursor_pos())) {
        const float radius = tool_cursor_radius(
            session_.active_tool(),
            session_.active_width());

        if (radius > 0.0F) {
            POINT cursor_pos = session_.cursor_pos();
            if (monitor_.bounds.contains(cursor_pos)) {
                float cx = static_cast<float>(cursor_pos.x - monitor_.bounds.left);
                float cy = static_cast<float>(cursor_pos.y - monitor_.bounds.top);

                D2D1_ELLIPSE cursor_ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);

                // Draw white outer circle and black inner circle for high contrast
                render_target_->DrawEllipse(cursor_ellipse, white_brush_.Get(), 1.5F);
                render_target_->DrawEllipse(cursor_ellipse, black_brush_.Get(), 0.5F);
            }
        }
    }

    // Draw text size dropdown card if open
    if (session_.text_size_dropdown_open()) {
        RectI card_bounds = session_.get_text_size_dropdown_bounds();
        D2D1_RECT_F local_card = local_rect(card_bounds);

        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(local_card, ui.px(6.0F), ui.px(6.0F)),
            toolbar_bg_brush_.Get());
        render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(local_card, ui.px(6.0F), ui.px(6.0F)),
            toolbar_border_brush_.Get(),
            ui.px(1.0F));

        auto dropdown_text_format = create_text_format(
            L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, ui.px(12.5F));
        if (dropdown_text_format) {
            dropdown_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dropdown_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        const std::array<std::pair<std::wstring_view, float>, 13> options{{
            {L"12pt", 12.0F},
            {L"14pt", 14.0F},
            {L"16pt", 16.0F},
            {L"18pt", 18.0F},
            {L"20pt", 20.0F},
            {L"24pt", 24.0F},
            {L"28pt", 28.0F},
            {L"32pt", 32.0F},
            {L"36pt", 36.0F},
            {L"48pt", 48.0F},
            {L"64pt", 64.0F},
            {L"72pt", 72.0F},
            {L"96pt", 96.0F},
        }};

        for (std::size_t i = 0; i < options.size(); ++i) {
            const float row_height = ui.px(32.0F);
            const float row_inset = ui.px(2.0F);
            D2D1_RECT_F item_rect_full = D2D1::RectF(
                local_card.left + row_inset,
                local_card.top + static_cast<float>(i) * row_height + row_inset,
                local_card.right - row_inset,
                local_card.top + static_cast<float>(i + 1) * row_height - row_inset
            );

            bool is_active = (std::abs(session_.active_text_size() - options[i].second) < 0.5F);
            bool is_hovered = (session_.text_size_hovered_idx() == static_cast<int>(i));

            if (is_hovered) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        item_rect_full, ui.px(4.0F), ui.px(4.0F)),
                    hover_bg_brush_.Get());
            }

            ComPtr<ID2D1SolidColorBrush> text_brush = white_brush_;
            if (is_active) {
                text_brush = blue_brush_;
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        item_rect_full, ui.px(4.0F), ui.px(4.0F)),
                    active_bg_brush_.Get());
            }

            if (dropdown_text_format) {
                render_target_->DrawTextW(options[i].first.data(),
                                          static_cast<UINT32>(options[i].first.size()),
                                          dropdown_text_format.Get(),
                                          item_rect_full,
                                          text_brush.Get());
            }
        }
    }

    // Draw hovered tooltips
    if (!session_.hovered_button_id().empty()) {
        std::wstring hovered_id = session_.hovered_button_id();
        RectI button_bounds{};
        bool found_button = false;
        std::wstring disabled_reason;
        for (const auto& btn : session_.toolbar()) {
            if (btn.id == hovered_id) {
                button_bounds = btn.bounds;
                disabled_reason = btn.disabled_reason;
                found_button = true;
                break;
            }
        }
        if (!found_button) {
            for (const auto& btn : session_.sub_toolbar()) {
                if (btn.id == hovered_id) {
                    button_bounds = btn.bounds;
                    disabled_reason = btn.disabled_reason;
                    found_button = true;
                    break;
                }
            }
        }

        if (found_button) {
            auto get_tooltip_text = [&](std::wstring_view id) -> std::wstring {
                const auto& config = session_.request_.config;
                if (id == L"drag") return L"拖动工具栏";
                if (id == L"lock") {
                    return session_.annotation_locked_tool()
                               ? L"连续使用当前工具（已开启）"
                               : L"连续使用当前工具（已关闭）";
                }
                if (id == L"select") {
                    return std::format(
                        L"选择与移动 ({}) · 双击编辑文字 · Delete 删除",
                        config.tool_shortcut_select);
                }
                if (id == L"rect") {
                    return std::format(
                        L"矩形 ({}) · Shift 正方形 · Alt 从中心",
                        config.tool_shortcut_rectangle);
                }
                if (id == L"ellipse") {
                    return std::format(
                        L"椭圆 ({}) · Shift 正圆 · Alt 从中心",
                        config.tool_shortcut_ellipse);
                }
                if (id == L"line") {
                    return std::format(
                        L"直线 ({}) · Shift 吸附 45°",
                        config.tool_shortcut_line);
                }
                if (id == L"arrow") {
                    return std::format(
                        L"箭头 ({}) · Shift 吸附 45°",
                        config.tool_shortcut_arrow);
                }
                if (id == L"pen") {
                    return std::format(
                        L"画笔 ({}) · 单击也可画点",
                        config.tool_shortcut_pen);
                }
                if (id == L"mosaic") {
                    return std::format(
                        L"马赛克 ({}) · 支持涂抹与框选",
                        config.tool_shortcut_mosaic);
                }
                if (id == L"blur") {
                    return std::format(
                        L"高斯模糊 ({}) · 支持涂抹与框选",
                        config.tool_shortcut_blur);
                }
                if (id == L"highlight") {
                    return std::format(
                        L"荧光笔 ({}) · Shift 锁定水平/垂直 · 滚轮调粗细",
                        config.tool_shortcut_highlight);
                }
                if (id == L"watermark") return L"水印";
                if (id == L"text") {
                    return std::format(
                        L"文本 ({}) · Enter 完成 · Shift+Enter 换行",
                        config.tool_shortcut_text);
                }
                if (id == L"serial") {
                    return std::format(
                        L"步骤序号 ({}) · 每次截图从 1 开始",
                        config.tool_shortcut_serial);
                }
                if (id == L"eraser") {
                    return std::format(
                        L"对象橡皮擦 ({}) · 拖动可连续删除",
                        config.tool_shortcut_eraser);
                }
                if (id == L"undo") return L"撤销 (Ctrl+Z)";
                if (id == L"redo") return L"重做 (Ctrl+Y / Ctrl+Shift+Z)";
                if (id == L"ocr") {
                    return std::format(L"屏幕识字 ({})", config.capture_ocr_shortcut);
                }
                if (id == L"scroll") return L"滚动截图";
                if (id == L"pin") return L"贴图";
                if (id == L"copy") {
                    return _wcsicmp(config.default_output.c_str(), L"file") == 0
                               ? L"复制 (Ctrl+C)"
                               : L"复制 (Ctrl+C / Enter)";
                }
                if (id == L"save") {
                    return _wcsicmp(config.default_output.c_str(), L"file") == 0
                               ? L"保存 (Ctrl+S / Enter)"
                               : L"保存 (Ctrl+S)";
                }
                if (id == L"close") return L"关闭 (Esc)";

                if (id == L"effect_mosaic") return L"马赛克效果";
                if (id == L"effect_blur") return L"模糊效果";
                if (id == L"mode_smear") return L"涂抹模式";
                if (id == L"mode_rect") return L"框选模式";
                if (id == L"mosaic_strength_slider") return L"效果强度 · 0% 不产生效果，也不会提交";
                if (id == L"fill_outline") return L"空心：只绘制轮廓";
                if (id == L"fill_translucent") return L"填充：使用 25% 透明色填充";
                if (id == L"stroke_solid") return L"实线轮廓";
                if (id == L"stroke_dashed") return L"虚线轮廓";
                if (id == L"corner_square") return L"直角矩形";
                if (id == L"corner_round") return L"圆角矩形";
                if (id == L"head_forward") return L"箭头位于终点";
                if (id == L"head_reverse") return L"箭头位于起点";
                if (id == L"head_both") return L"起点和终点均显示箭头";
                if (id == L"text_style_normal") return L"普通文字";
                if (id == L"text_style_dark") return L"黑底文字";
                if (id == L"text_style_outline") return L"轮廓高亮";
                if (id == L"watermark_text") return L"设置水印文字";
                if (id == L"watermark_apply") return L"应用水印";
                if (id == L"watermark_clear") return L"清除水印";

                return L"";
            };

            std::wstring tooltip_str = get_tooltip_text(hovered_id);
            if (!tooltip_str.empty() && !disabled_reason.empty()) {
                tooltip_str += L" · " + disabled_reason;
            }
            if (!tooltip_str.empty()) {
                auto tooltip_format = create_text_format(
                    L"Microsoft YaHei",
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    ui.px(13.0F));
                if (tooltip_format) {
                    tooltip_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    tooltip_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                auto layout =
                    create_text_layout(
                        tooltip_str,
                        tooltip_format.Get(),
                        ui.px(400.0F),
                        ui.px(100.0F));
                DWRITE_TEXT_METRICS metrics{};
                if (layout && FAILED(layout->GetMetrics(&metrics))) {
                    metrics = {};
                }
                const float text_width =
                    metrics.width > 0.0F
                        ? metrics.width
                        : static_cast<float>(tooltip_str.size()) * ui.px(13.0F);
                const float text_height =
                    metrics.height > 0.0F ? metrics.height : ui.px(18.0F);

                float bubble_width = text_width + ui.px(20.0F);
                float bubble_height = text_height + ui.px(12.0F);

                float btn_cx = static_cast<float>(button_bounds.left - monitor_.bounds.left) + static_cast<float>(button_bounds.width()) / 2.0F;
                float btn_top = static_cast<float>(button_bounds.top - monitor_.bounds.top);
                float btn_bottom = static_cast<float>(button_bounds.bottom - monitor_.bounds.top);

                float bubble_left = btn_cx - bubble_width / 2.0F;
                float bubble_bottom = btn_top - ui.px(8.0F);
                float bubble_top = bubble_bottom - bubble_height;

                if (bubble_top < 0.0F) {
                    bubble_top = btn_bottom + ui.px(8.0F);
                    bubble_bottom = bubble_top + bubble_height;
                }
                const D2D1_SIZE_F target_size = render_target_->GetSize();
                bubble_left = std::clamp(
                    bubble_left,
                    ui.px(8.0F),
                    std::max(
                        ui.px(8.0F),
                        target_size.width - bubble_width - ui.px(8.0F)));
                if (bubble_bottom > target_size.height - ui.px(8.0F)) {
                    bubble_bottom = btn_top - ui.px(8.0F);
                    bubble_top = bubble_bottom - bubble_height;
                }

                ComPtr<ID2D1SolidColorBrush> tooltip_bg_brush;
                if (FAILED(render_target_->CreateSolidColorBrush(
                        D2D1::ColorF(0x1F2329, 0.95F),
                        tooltip_bg_brush.GetAddressOf())) ||
                    !tooltip_bg_brush) {
                    tooltip_bg_brush = toolbar_bg_brush_;
                }
                ComPtr<ID2D1SolidColorBrush> tooltip_fg_brush;
                if (FAILED(render_target_->CreateSolidColorBrush(
                        D2D1::ColorF(D2D1::ColorF::White),
                        tooltip_fg_brush.GetAddressOf())) ||
                    !tooltip_fg_brush) {
                    tooltip_fg_brush = true_white_brush_;
                }

                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(
                            bubble_left,
                            bubble_top,
                            bubble_left + bubble_width,
                            bubble_bottom),
                        ui.px(4.0F),
                        ui.px(4.0F)),
                    tooltip_bg_brush.Get());

                ComPtr<ID2D1PathGeometry> tri_geom;
                if (SUCCEEDED(d2d_factory->CreatePathGeometry(tri_geom.GetAddressOf()))) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(tri_geom->Open(sink.GetAddressOf()))) {
                        if (bubble_top > btn_bottom) {
                            sink->BeginFigure(D2D1::Point2F(btn_cx, btn_bottom + ui.px(3.0F)), D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(btn_cx - ui.px(4.0F), btn_bottom + ui.px(8.0F)));
                            sink->AddLine(D2D1::Point2F(btn_cx + ui.px(4.0F), btn_bottom + ui.px(8.0F)));
                        } else {
                            sink->BeginFigure(D2D1::Point2F(btn_cx, btn_top - ui.px(3.0F)), D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(btn_cx - ui.px(4.0F), btn_top - ui.px(8.0F)));
                            sink->AddLine(D2D1::Point2F(btn_cx + ui.px(4.0F), btn_top - ui.px(8.0F)));
                        }
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        render_target_->FillGeometry(tri_geom.Get(), tooltip_bg_brush.Get());
                    }
                }

                if (tooltip_format) {
                    render_target_->DrawTextW(tooltip_str.c_str(), static_cast<UINT32>(tooltip_str.size()), tooltip_format.Get(), D2D1::RectF(bubble_left, bubble_top, bubble_left + bubble_width, bubble_bottom), tooltip_fg_brush.Get());
                }
            }
        }
    }

    if (session_.ocr_running()) {
        const D2D1_SIZE_F target_size = render_target_->GetSize();
        constexpr float card_width = 540.0F;
        constexpr float card_height = 76.0F;
        const D2D1_RECT_F card = D2D1::RectF(
            std::max(16.0F, (target_size.width - card_width) * 0.5F),
            std::max(16.0F, (target_size.height - card_height) * 0.5F),
            std::min(target_size.width - 16.0F, (target_size.width + card_width) * 0.5F),
            std::min(target_size.height - 16.0F, (target_size.height + card_height) * 0.5F));
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(card, 9.0F, 9.0F),
            toolbar_bg_brush_.Get());
        render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(card, 9.0F, 9.0F),
            toolbar_border_brush_.Get(),
            1.0F);
        auto status_format = create_text_format(
            L"Segoe UI",
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            14.0F);
        if (status_format) {
            status_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            status_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const std::wstring status = session_.ocr_status_text();
            const D2D1_RECT_F status_bounds = D2D1::RectF(
                card.left + 14.0F,
                card.top + 6.0F,
                card.right - 14.0F,
                card.bottom - 17.0F);
            render_target_->DrawTextW(
                status.data(),
                static_cast<UINT32>(status.size()),
                status_format.Get(),
                status_bounds,
                white_brush_.Get());
        }
        const D2D1_RECT_F progress_track = D2D1::RectF(
            card.left + 16.0F,
            card.bottom - 12.0F,
            card.right - 16.0F,
            card.bottom - 8.0F);
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(progress_track, 2.0F, 2.0F),
            toolbar_border_brush_.Get());
        const int progress = session_.ocr_recognizing()
                                 ? 100
                                 : session_.ocr_progress_percent();
        if (progress > 0) {
            D2D1_RECT_F progress_fill = progress_track;
            progress_fill.right = progress_track.left +
                (progress_track.right - progress_track.left) *
                    static_cast<float>(progress) / 100.0F;
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(progress_fill, 2.0F, 2.0F),
                blue_brush_.Get());
        }
    }

    const HRESULT draw_result = render_target_->EndDraw();
    const bool recreate_target = draw_result == D2DERR_RECREATE_TARGET;
    if (FAILED(draw_result)) {
        discard_device_resources();
    }
    EndPaint(hwnd_, &paint);
    if (recreate_target && hwnd_ && IsWindow(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK OverlayWindow::window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_PAINT) {
        self->paint();
        return 0;
    }
    if (message == kOverlayOcrCompletedMessage) {
        self->session_.handle_ocr_completion();
        return 0;
    }
    if (message == kOverlayOcrProgressMessage) {
        self->session_.invalidate_all();
        return 0;
    }
    if (message == kOverlayScrollFrameCompletedMessage) {
        self->session_.handle_scroll_frame_completion();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_CLOSE) {
        self->session_.cancel();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        self->discard_device_resources();
        self->hwnd_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_down(window, point, message == WM_RBUTTONDOWN);
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_double_click(point);
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_move(point);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_up(window, point);
        return 0;
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        self->session_.on_capture_lost();
        return 0;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        const bool is_auto_repeat =
            (static_cast<std::uint64_t>(l_param) & (1ULL << 30U)) != 0;
        if (message == WM_KEYDOWN) {
            self->session_.on_key_down(window, w_param, is_auto_repeat);
            return 0;
        }
        if (self->session_.on_system_key_down(
                window,
                w_param,
                is_auto_repeat)) {
            return 0;
        }
    }
    if (message == WM_MOUSEWHEEL) {
        short delta = GET_WHEEL_DELTA_WPARAM(w_param);
        self->session_.on_mouse_wheel(delta);
        return 0;
    }
    if (message == WM_SETCURSOR) {
        POINT point{};
        GetCursorPos(&point);
        if (self->session_.can_edit_selection_size() &&
            self->session_.dimension_badge_bounds().contains(point)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (self->session_.is_over_toolbar(point)) {
            SetCursor(LoadCursorW(
                nullptr,
                self->session_.is_over_toolbar_drag_handle(point)
                    ? IDC_SIZEALL
                    : IDC_ARROW));
            return TRUE;
        }
        DragMode mode = self->session_.hit_test_drag_mode(point);

        HCURSOR cursor = nullptr;
        switch (mode) {
            case DragMode::move:
                cursor = LoadCursorW(nullptr, IDC_SIZEALL);
                break;
            case DragMode::top_left:
            case DragMode::bottom_right:
                cursor = LoadCursorW(nullptr, IDC_SIZENWSE);
                break;
            case DragMode::top_right:
            case DragMode::bottom_left:
                cursor = LoadCursorW(nullptr, IDC_SIZENESW);
                break;
            case DragMode::top:
            case DragMode::bottom:
                cursor = LoadCursorW(nullptr, IDC_SIZENS);
                break;
            case DragMode::left:
            case DragMode::right:
                cursor = LoadCursorW(nullptr, IDC_SIZEWE);
                break;
            case DragMode::annotate:
                cursor = LoadCursorW(nullptr, self->session_.active_tool() == Tool::text ? IDC_IBEAM : IDC_CROSS);
                break;
            default:
                cursor = LoadCursorW(nullptr, IDC_CROSS);
                break;
        }
        if (cursor) {
            SetCursor(cursor);
            return TRUE;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace airshot::overlay_detail
