#include "overlay_window.h"

#include "overlay_session.h"

#include <dwrite.h>
#include <imm.h>

#include <cmath>
#include <mutex>

namespace airshot::overlay_detail {

using Microsoft::WRL::ComPtr;

ComPtr<ID2D1Factory>& d2d_factory() {
    static ComPtr<ID2D1Factory> factory;
    if (!factory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    }
    return factory;
}

ComPtr<IDWriteFactory>& dwrite_factory() {
    static ComPtr<IDWriteFactory> factory;
    if (!factory) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory;
}

void release_overlay_factories() {
    dwrite_factory().Reset();
    d2d_factory().Reset();
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
    background_.Reset();
    render_target_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
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
    const D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT>(monitor_.bounds.width()), static_cast<UINT>(monitor_.bounds.height()));
    HRESULT result = d2d_factory()->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0F, 96.0F),
        D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        render_target_.GetAddressOf());
    if (FAILED(result)) {
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
        return false;
    }
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0.48F), dim_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1677FF), blue_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), white_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x202124, 0.94F), toolbar_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x21252B, 0.94F), toolbar_bg_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 0.15F), toolbar_border_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 0.12F), hover_bg_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1677FF, 0.85F), active_bg_brush_.GetAddressOf());
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

void OverlayWindow::draw_arrow(POINT start, POINT end, ID2D1Brush* brush, float width) {
    const D2D1_POINT_2F first{
        static_cast<float>(start.x - monitor_.bounds.left), static_cast<float>(start.y - monitor_.bounds.top)};
    const D2D1_POINT_2F second{
        static_cast<float>(end.x - monitor_.bounds.left), static_cast<float>(end.y - monitor_.bounds.top)};
    render_target_->DrawLine(first, second, brush, width);
    const double angle = std::atan2(static_cast<double>(end.y - start.y), static_cast<double>(end.x - start.x));
    constexpr double length = 16.0;
    for (double offset : {0.45, -0.45}) {
        const POINT point{
            end.x - static_cast<int>(std::cos(angle + offset) * length),
            end.y - static_cast<int>(std::sin(angle + offset) * length),
        };
        render_target_->DrawLine(second,
                                 D2D1::Point2F(static_cast<float>(point.x - monitor_.bounds.left),
                                              static_cast<float>(point.y - monitor_.bounds.top)),
                                 brush,
                                 width);
    }
}

void OverlayWindow::draw_annotation(const Annotation& annotation, bool preview) {
    const RectI selection = session_.selection();
    POINT start{selection.left + annotation.start.x, selection.top + annotation.start.y};
    POINT end{selection.left + annotation.end.x, selection.top + annotation.end.y};

    COLORREF color = annotation.color;
    float draw_width = annotation.width;
    float alpha = annotation.tool == Tool::highlight ? std::clamp(annotation.alpha / 255.0F, 0.1F, 1.0F) : 1.0F;
    if (preview) {
        color = session_.active_color();
        draw_width = session_.active_width();
        alpha = annotation.tool == Tool::highlight ? std::clamp(session_.active_highlight_alpha() / 255.0F, 0.1F, 1.0F) : alpha;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    render_target_->CreateSolidColorBrush(
        D2D1::ColorF(GetRValue(color) / 255.0F, GetGValue(color) / 255.0F, GetBValue(color) / 255.0F, alpha),
        brush.GetAddressOf()
    );

    if (annotation.tool == Tool::rectangle) {
        render_target_->DrawRectangle(local_rect(RectI{start.x, start.y, end.x, end.y}.normalized()), brush.Get(), draw_width);
    } else if (annotation.tool == Tool::ellipse) {
        const D2D1_RECT_F rect = local_rect(RectI{start.x, start.y, end.x, end.y}.normalized());
        render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) / 2.0F, (rect.top + rect.bottom) / 2.0F),
                                                  std::abs(rect.right - rect.left) / 2.0F,
                                                  std::abs(rect.bottom - rect.top) / 2.0F),
                                    brush.Get(),
                                    draw_width);
    } else if (annotation.tool == Tool::line) {
        render_target_->DrawLine(D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left),
                                               static_cast<float>(start.y - monitor_.bounds.top)),
                                 D2D1::Point2F(static_cast<float>(end.x - monitor_.bounds.left),
                                               static_cast<float>(end.y - monitor_.bounds.top)),
                                 brush.Get(),
                                 draw_width);
    } else if (annotation.tool == Tool::arrow) {
        draw_arrow(start, end, brush.Get(), draw_width);
    } else if (annotation.tool == Tool::pen || annotation.tool == Tool::highlight) {
        const float line_width =
            annotation.tool == Tool::highlight ? std::max(6.0F, draw_width * 3.2F) : draw_width;
        if (annotation.points.empty()) {
            render_target_->DrawLine(D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left),
                                                   static_cast<float>(start.y - monitor_.bounds.top)),
                                     D2D1::Point2F(static_cast<float>(end.x - monitor_.bounds.left),
                                                   static_cast<float>(end.y - monitor_.bounds.top)),
                                     brush.Get(),
                                     line_width);
        } else {
            for (std::size_t index = 1; index < annotation.points.size(); ++index) {
                const POINT first{selection.left + annotation.points[index - 1].x, selection.top + annotation.points[index - 1].y};
                const POINT second{selection.left + annotation.points[index].x, selection.top + annotation.points[index].y};
                render_target_->DrawLine(D2D1::Point2F(static_cast<float>(first.x - monitor_.bounds.left),
                                                       static_cast<float>(first.y - monitor_.bounds.top)),
                                         D2D1::Point2F(static_cast<float>(second.x - monitor_.bounds.left),
                                                       static_cast<float>(second.y - monitor_.bounds.top)),
                                         brush.Get(),
                                         line_width);
            }
        }
    } else if (annotation.tool == Tool::mosaic) {
        for (const POINT relative : annotation.points) {
            POINT point{selection.left + relative.x, selection.top + relative.y};
            RectI block{point.x - 7, point.y - 7, point.x + 7, point.y + 7};
            render_target_->FillRectangle(local_rect(block), dim_brush_.Get());
        }
    } else if (annotation.tool == Tool::text) {
        ComPtr<IDWriteTextFormat> format;
        dwrite_factory()->CreateTextFormat(L"Microsoft YaHei",
                                           nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           22.0F,
                                           L"zh-CN",
                                           format.GetAddressOf());
        const D2D1_RECT_F bounds =
            D2D1::RectF(static_cast<float>(start.x - monitor_.bounds.left),
                        static_cast<float>(start.y - monitor_.bounds.top),
                        static_cast<float>(monitor_.bounds.width()),
                        static_cast<float>(monitor_.bounds.height()));
        render_target_->DrawTextW(annotation.text.c_str(),
                                  static_cast<UINT32>(annotation.text.size()),
                                  format.Get(),
                                  bounds,
                                  brush.Get());
    } else if (annotation.tool == Tool::serial) {
        constexpr float radius = 13.0F;
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F(static_cast<float>(start.x - monitor_.bounds.left), static_cast<float>(start.y - monitor_.bounds.top)),
            radius,
            radius);
        render_target_->FillEllipse(ellipse, brush.Get());
        render_target_->DrawEllipse(ellipse, white_brush_.Get(), 1.5F);

        ComPtr<IDWriteTextFormat> format;
        dwrite_factory()->CreateTextFormat(L"Consolas",
                                           nullptr,
                                           DWRITE_FONT_WEIGHT_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           13.0F,
                                           L"zh-CN",
                                           format.GetAddressOf());
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
    if (!render_target_ && !create_render_target()) {
        return;
    }
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
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
        render_target_->DrawRectangle(local_rect(selected), blue_brush_.Get(), 2.0F);
    } else {
        render_target_->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(monitor_.bounds.width()), static_cast<float>(monitor_.bounds.height())),
            dim_brush_.Get());
    }

    if (session_.selection_complete()) {
        for (const auto& annotation : session_.annotations()) {
            draw_annotation(annotation, false);
        }
        if (const Annotation* preview = session_.preview()) {
            draw_annotation(*preview, true);
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

        ComPtr<IDWriteTextFormat> format;
        dwrite_factory()->CreateTextFormat(L"Consolas",
                                           nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           12.0F,
                                           L"zh-CN",
                                           format.GetAddressOf());
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Draw main toolbar background rounded rectangle card
        if (!session_.toolbar().empty()) {
            D2D1_RECT_F bg_rect = local_rect(padded_buttons_bounds(session_.toolbar(), 6));
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(bg_rect, 6.f, 6.f), toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(bg_rect, 6.f, 6.f), toolbar_border_brush_.Get(), 1.f);

            for (const auto& button : session_.toolbar()) {
                if (!intersect(button.bounds, monitor_.bounds)) {
                    continue;
                }
                const D2D1_RECT_F bounds = local_rect(button.bounds);

                if (button.id == L"|") {
                    float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                    ComPtr<ID2D1SolidColorBrush> sep_brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.38f, 0.43f, 0.6f), sep_brush.GetAddressOf());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, bounds.top + 6.0F),
                        D2D1::Point2F(cx, bounds.bottom - 6.0F),
                        sep_brush.Get(),
                        1.0F
                    );
                    continue;
                }

                bool is_active = (button.id == L"lock" && session_.annotation_locked_tool()) ||
                                 (session_.active_tool() != Tool::none &&
                                  ((button.id == L"rect" && session_.active_tool() == Tool::rectangle) ||
                                   (button.id == L"ellipse" && session_.active_tool() == Tool::ellipse) ||
                                   (button.id == L"line" && session_.active_tool() == Tool::line) ||
                                   (button.id == L"arrow" && session_.active_tool() == Tool::arrow) ||
                                   (button.id == L"pen" && session_.active_tool() == Tool::pen) ||
                                   (button.id == L"mosaic" && session_.active_tool() == Tool::mosaic) ||
                                   (button.id == L"highlight" && session_.active_tool() == Tool::highlight) ||
                                   (button.id == L"text" && session_.active_tool() == Tool::text) ||
                                   (button.id == L"serial" && session_.active_tool() == Tool::serial) ||
                                   (button.id == L"eraser" && session_.active_tool() == Tool::eraser)));

                bool is_hovered = (session_.hovered_button_id() == button.id);

                if (is_active) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), active_bg_brush_.Get());
                } else if (is_hovered) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), hover_bg_brush_.Get());
                }

                const float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                const float cy = bounds.top + (bounds.bottom - bounds.top) / 2.0F;

                if (button.id == L"lock") {
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.0F, cy - 1.0F, cx + 6.0F, cy + 7.0F), 1.5F, 1.5F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.5F, cy - 1.0F), D2D1::Point2F(cx - 4.5F, cy - 4.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 4.5F, cy - 1.0F), D2D1::Point2F(cx + 4.5F, cy - 4.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.5F, cy - 4.0F), D2D1::Point2F(cx + 4.5F, cy - 4.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"rect") {
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 7.5F, cy - 6.5F, cx + 7.5F, cy + 6.5F), 1.5F, 1.5F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"ellipse") {
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0F, 6.5F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"line") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy + 5.0F), D2D1::Point2F(cx + 7.0F, cy - 5.0F), white_brush_.Get(), 1.8F);
                } else if (button.id == L"arrow") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.0F, cy + 4.0F), D2D1::Point2F(cx + 5.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy - 5.0F), D2D1::Point2F(cx - 1.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy - 5.0F), D2D1::Point2F(cx + 5.0F, cy - 1.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 6.0F, cy + 6.0F), 1.8F, 1.8F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"pen") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy + 5.0F), D2D1::Point2F(cx - 2.0F, cy - 4.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 2.0F, cy - 4.0F), D2D1::Point2F(cx + 6.0F, cy + 3.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy + 7.0F), D2D1::Point2F(cx + 6.0F, cy + 7.0F), white_brush_.Get(), 1.2F);
                } else if (button.id == L"mosaic") {
                    float step = 3.5F;
                    // Row 0
                    render_target_->FillRectangle(D2D1::RectF(cx - 7.0F, cy - 7.0F, cx - 7.0F + step, cy - 7.0F + step), white_brush_.Get());
                    render_target_->FillRectangle(D2D1::RectF(cx, cy - 7.0F, cx + step, cy - 7.0F + step), white_brush_.Get());
                    // Row 1
                    render_target_->FillRectangle(D2D1::RectF(cx - 7.0F + step, cy - 7.0F + step, cx, cy), white_brush_.Get());
                    render_target_->FillRectangle(D2D1::RectF(cx + step, cy - 7.0F + step, cx + 7.0F, cy), white_brush_.Get());
                    // Row 2
                    render_target_->FillRectangle(D2D1::RectF(cx - 7.0F, cy, cx - 7.0F + step, cy + step), white_brush_.Get());
                    render_target_->FillRectangle(D2D1::RectF(cx, cy, cx + step, cy + step), white_brush_.Get());
                    // Row 3
                    render_target_->FillRectangle(D2D1::RectF(cx - 7.0F + step, cy + step, cx, cy + 7.0F), white_brush_.Get());
                    render_target_->FillRectangle(D2D1::RectF(cx + step, cy + step, cx + 7.0F, cy + 7.0F), white_brush_.Get());

                    render_target_->DrawRectangle(D2D1::RectF(cx - 7.0F, cy - 7.0F, cx + 7.0F, cy + 7.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"highlight") {
                    ComPtr<ID2D1SolidColorBrush> highlight_brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(0xFADB14, 0.55F), highlight_brush.GetAddressOf());
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.0F, cy - 3.5F, cx + 8.0F, cy + 3.5F), 3.5F, 3.5F), highlight_brush.Get());
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy + 5.0F), D2D1::Point2F(cx + 7.0F, cy + 5.0F), white_brush_.Get(), 1.2F);
                } else if (button.id == L"text") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.0F, cy - 5.0F), D2D1::Point2F(cx + 3.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx, cy - 5.0F), D2D1::Point2F(cx, cy + 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 1.5F, cy + 5.0F), D2D1::Point2F(cx + 1.5F, cy + 5.0F), white_brush_.Get(), 1.5F);

                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy - 7.0F), D2D1::Point2F(cx - 7.0F, cy + 7.0F), white_brush_.Get(), 1.0F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy - 7.0F), D2D1::Point2F(cx - 5.0F, cy - 7.0F), white_brush_.Get(), 1.0F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 7.0F, cy + 7.0F), D2D1::Point2F(cx - 5.0F, cy + 7.0F), white_brush_.Get(), 1.0F);

                    render_target_->DrawLine(D2D1::Point2F(cx + 7.0F, cy - 7.0F), D2D1::Point2F(cx + 7.0F, cy + 7.0F), white_brush_.Get(), 1.0F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 7.0F, cy - 7.0F), D2D1::Point2F(cx + 5.0F, cy - 7.0F), white_brush_.Get(), 1.0F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 7.0F, cy + 7.0F), D2D1::Point2F(cx + 5.0F, cy + 7.0F), white_brush_.Get(), 1.0F);
                } else if (button.id == L"serial") {
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0F, 8.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawTextW(L"1", 1, format.Get(), D2D1::RectF(cx - 8.0F, cy - 8.0F, cx + 8.0F, cy + 8.0F), white_brush_.Get());
                } else if (button.id == L"eraser") {
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.0F, cy - 5.0F, cx + 5.0F, cy + 5.0F), 2.0F, 2.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.0F, cy + 6.5F), D2D1::Point2F(cx + 7.0F, cy + 6.5F), white_brush_.Get(), 1.3F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy), D2D1::Point2F(cx + 5.0F, cy), white_brush_.Get(), 1.0F);
                } else if (button.id == L"undo") {
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy + 4.0F), D2D1::Point2F(cx + 5.0F, cy), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy), D2D1::Point2F(cx + 4.0F, cy - 3.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 4.0F, cy - 3.0F), D2D1::Point2F(cx + 1.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 1.0F, cy - 5.0F), D2D1::Point2F(cx - 4.0F, cy - 4.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.0F, cy - 4.0F), D2D1::Point2F(cx - 4.0F, cy), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.0F, cy - 4.0F), D2D1::Point2F(cx, cy - 4.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"redo") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 5.0F, cy + 4.0F), D2D1::Point2F(cx - 5.0F, cy), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 5.0F, cy), D2D1::Point2F(cx - 4.0F, cy - 3.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.0F, cy - 3.0F), D2D1::Point2F(cx - 1.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 1.0F, cy - 5.0F), D2D1::Point2F(cx + 4.0F, cy - 4.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 4.0F, cy - 4.0F), D2D1::Point2F(cx + 4.0F, cy), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 4.0F, cy - 4.0F), D2D1::Point2F(cx, cy - 4.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"ocr") {
                    ComPtr<IDWriteTextFormat> ocr_format;
                    dwrite_factory()->CreateTextFormat(L"Consolas",
                                                       nullptr,
                                                       DWRITE_FONT_WEIGHT_BOLD,
                                                       DWRITE_FONT_STYLE_NORMAL,
                                                       DWRITE_FONT_STRETCH_NORMAL,
                                                       8.0F,
                                                       L"zh-CN",
                                                       ocr_format.GetAddressOf());
                    ocr_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    ocr_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    constexpr float len = 3.5F;
                    constexpr float r = 7.0F;
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx - r + len, cy - r), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx - r, cy - r + len), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx + r - len, cy - r), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx + r, cy - r + len), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy + r), D2D1::Point2F(cx - r + len, cy + r), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx - r, cy + r), D2D1::Point2F(cx - r, cy + r - len), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy + r), D2D1::Point2F(cx + r - len, cy + r), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx + r, cy + r), D2D1::Point2F(cx + r, cy + r - len), white_brush_.Get(), 1.2F);

                    render_target_->DrawTextW(L"OCR", 3, ocr_format.Get(), D2D1::RectF(cx - 8.0F, cy - 8.0F, cx + 8.0F, cy + 8.0F), white_brush_.Get());
                } else if (button.id == L"copy") {
                    render_target_->DrawRectangle(D2D1::RectF(cx - 3.0F, cy - 7.0F, cx + 7.0F, cy + 3.0F), blue_brush_.Get(), 1.5F);
                    render_target_->FillRectangle(D2D1::RectF(cx - 7.0F, cy - 3.0F, cx + 3.0F, cy + 7.0F), is_active ? active_bg_brush_.Get() : (is_hovered ? hover_bg_brush_.Get() : toolbar_bg_brush_.Get()));
                    render_target_->DrawRectangle(D2D1::RectF(cx - 7.0F, cy - 3.0F, cx + 3.0F, cy + 7.0F), blue_brush_.Get(), 1.5F);
                } else if (button.id == L"save") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy - 6.0F), D2D1::Point2F(cx + 3.0F, cy - 6.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 3.0F, cy - 6.0F), D2D1::Point2F(cx + 6.0F, cy - 3.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 6.0F, cy - 3.0F), D2D1::Point2F(cx + 6.0F, cy + 6.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 6.0F, cy + 6.0F), D2D1::Point2F(cx - 6.0F, cy + 6.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy + 6.0F), D2D1::Point2F(cx - 6.0F, cy - 6.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawRectangle(D2D1::RectF(cx - 3.0F, cy + 1.0F, cx + 3.0F, cy + 6.0F), white_brush_.Get(), 1.5F);
                    render_target_->FillRectangle(D2D1::RectF(cx - 2.0F, cy - 6.0F, cx + 1.0F, cy - 3.0F), white_brush_.Get());
                } else if (button.id == L"scroll") {
                    render_target_->DrawRectangle(D2D1::RectF(cx - 6.0F, cy - 7.0F, cx + 6.0F, cy + 7.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 6.0F, cy - 3.0F), D2D1::Point2F(cx + 6.0F, cy - 3.0F), white_brush_.Get(), 1.2F);
                    render_target_->DrawLine(D2D1::Point2F(cx, cy - 1.0F), D2D1::Point2F(cx, cy + 4.5F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx, cy + 4.5F), D2D1::Point2F(cx - 2.5F, cy + 2.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx, cy + 4.5F), D2D1::Point2F(cx + 2.5F, cy + 2.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"pin") {
                    render_target_->DrawLine(D2D1::Point2F(cx, cy - 1.0F), D2D1::Point2F(cx, cy + 7.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 5.0F, cy - 6.0F), D2D1::Point2F(cx + 5.0F, cy - 6.0F), white_brush_.Get(), 2.0F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.5F, cy - 6.0F), D2D1::Point2F(cx - 3.5F, cy - 1.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 3.5F, cy - 6.0F), D2D1::Point2F(cx + 3.5F, cy - 1.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 3.5F, cy - 1.0F), D2D1::Point2F(cx + 3.5F, cy - 1.0F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"close") {
                    ComPtr<ID2D1SolidColorBrush> red_brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(0xFF4D4F), red_brush.GetAddressOf());
                    render_target_->DrawLine(D2D1::Point2F(cx - 5.0F, cy - 5.0F), D2D1::Point2F(cx + 5.0F, cy + 5.0F), red_brush.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx - 5.0F, cy + 5.0F), D2D1::Point2F(cx + 5.0F, cy - 5.0F), red_brush.Get(), 1.5F);
                }
            }
        }

        // Draw sub-toolbar
        if (!session_.sub_toolbar().empty()) {
            D2D1_RECT_F sub_bg_rect = local_rect(padded_buttons_bounds(session_.sub_toolbar(), 6));
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(sub_bg_rect, 6.f, 6.f), toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(sub_bg_rect, 6.f, 6.f), toolbar_border_brush_.Get(), 1.f);

            for (const auto& button : session_.sub_toolbar()) {
                if (!intersect(button.bounds, monitor_.bounds)) {
                    continue;
                }
                const D2D1_RECT_F bounds = local_rect(button.bounds);
                if (button.id == L"|") {
                    float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                    ComPtr<ID2D1SolidColorBrush> sep_brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.38f, 0.43f, 0.6f), sep_brush.GetAddressOf());
                    render_target_->DrawLine(
                        D2D1::Point2F(cx, bounds.top + 6.0F),
                        D2D1::Point2F(cx, bounds.bottom - 6.0F),
                        sep_brush.Get(),
                        1.0F
                    );
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
                else if (button.id == L"color_blue" && session_.active_color() == RGB(22, 119, 255)) is_selected = true;
                else if (button.id == L"color_yellow" && session_.active_color() == RGB(250, 219, 20)) is_selected = true;
                else if (button.id == L"color_black" && session_.active_color() == RGB(0, 0, 0)) is_selected = true;
                else if (button.id == L"color_white" && session_.active_color() == RGB(255, 255, 255)) is_selected = true;
                else if (button.id == L"color_custom" &&
                         session_.active_color() != RGB(245, 34, 45) &&
                         session_.active_color() != RGB(82, 196, 26) &&
                         session_.active_color() != RGB(22, 119, 255) &&
                         session_.active_color() != RGB(250, 219, 20) &&
                         session_.active_color() != RGB(0, 0, 0) &&
                         session_.active_color() != RGB(255, 255, 255)) {
                    is_selected = true;
                }

                bool is_hovered = (session_.hovered_button_id() == button.id);

                if ((button.id.starts_with(L"width_") || button.id.starts_with(L"alpha_")) && is_selected) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), active_bg_brush_.Get());
                } else if (is_hovered) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), hover_bg_brush_.Get());
                }

                const float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                const float cy = bounds.top + (bounds.bottom - bounds.top) / 2.0F;

                if (button.id.starts_with(L"color_")) {
                    COLORREF color = RGB(22, 119, 255);
                    if (button.id == L"color_red") color = RGB(245, 34, 45);
                    else if (button.id == L"color_green") color = RGB(82, 196, 26);
                    else if (button.id == L"color_yellow") color = RGB(250, 219, 20);
                    else if (button.id == L"color_black") color = RGB(0, 0, 0);
                    else if (button.id == L"color_white") color = RGB(255, 255, 255);
                    else if (button.id == L"color_custom") color = session_.custom_color();

                    ComPtr<ID2D1SolidColorBrush> brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(GetRValue(color) / 255.0F, GetGValue(color) / 255.0F, GetBValue(color) / 255.0F), brush.GetAddressOf());

                    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0F, 8.0F), brush.Get());
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0F, 8.0F), white_brush_.Get(), 1.5F);

                    if (button.id == L"color_custom") {
                        COLORREF plus_color = RGB(255, 255, 255);
                        double brightness = (GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114) / 1000.0;
                        if (brightness > 180.0) {
                            plus_color = RGB(0, 0, 0);
                        }
                        ComPtr<ID2D1SolidColorBrush> plus_brush;
                        render_target_->CreateSolidColorBrush(D2D1::ColorF(GetRValue(plus_color) / 255.0F, GetGValue(plus_color) / 255.0F, GetBValue(plus_color) / 255.0F), plus_brush.GetAddressOf());
                        render_target_->DrawLine(D2D1::Point2F(cx - 3.0F, cy), D2D1::Point2F(cx + 3.0F, cy), plus_brush.Get(), 1.5F);
                        render_target_->DrawLine(D2D1::Point2F(cx, cy - 3.0F), D2D1::Point2F(cx, cy + 3.0F), plus_brush.Get(), 1.5F);
                    }

                    if (is_selected) {
                        render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 11.0F, 11.0F), white_brush_.Get(), 1.5F);
                    }
                } else if (button.id.starts_with(L"width_")) {
                    float w = 2.0F;
                    if (button.id == L"width_medium") w = 4.0F;
                    else if (button.id == L"width_large") w = 8.0F;

                    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w / 2.0F + 1.0F, w / 2.0F + 1.0F), white_brush_.Get());
                } else if (button.id.starts_with(L"alpha_")) {
                    float alpha = 0.25F;
                    if (button.id == L"alpha_medium") alpha = 0.38F;
                    else if (button.id == L"alpha_high") alpha = 0.56F;
                    ComPtr<ID2D1SolidColorBrush> alpha_brush;
                    render_target_->CreateSolidColorBrush(D2D1::ColorF(0xFADB14, alpha), alpha_brush.GetAddressOf());
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 9.0F, cy - 6.0F, cx + 9.0F, cy + 6.0F), 3.0F, 3.0F), alpha_brush.Get());
                    render_target_->DrawTextW(button.label.c_str(), static_cast<UINT32>(button.label.size()), format.Get(), bounds, white_brush_.Get());
                }
            }
        }

        // Draw 8-point handles
        for (const auto& pt : handles) {
            D2D1_RECT_F r = D2D1::RectF(pt.x - 3.5F, pt.y - 3.5F, pt.x + 3.5F, pt.y + 3.5F);
            render_target_->FillRectangle(r, blue_brush_.Get());
            render_target_->DrawRectangle(r, white_brush_.Get(), 1.0F);
        }

        const std::wstring dimensions =
            std::format(L" {} × {} ", session_.selection().width(), session_.selection().height());
        RectI text_bounds{session_.selection().left,
                          session_.selection().top - 28,
                          session_.selection().left + 130,
                          session_.selection().top - 4};
        if (text_bounds.top < monitor_.bounds.top) {
            text_bounds.top = session_.selection().top + 4;
            text_bounds.bottom = text_bounds.top + 24;
        }
        if (intersect(text_bounds, monitor_.bounds)) {
            const auto bounds = local_rect(text_bounds);
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 12.f, 12.f), toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 12.f, 12.f), toolbar_border_brush_.Get(), 1.0f);
            render_target_->DrawTextW(dimensions.c_str(),
                                      static_cast<UINT32>(dimensions.size()),
                                      format.Get(),
                                      bounds,
                                      white_brush_.Get());
        }
    }

    // Draw high-precision pixel magnifier
    if (!session_.selection_complete() || session_.dragging_selection()) {
        POINT cursor_pos = session_.cursor_pos();
        if (monitor_.bounds.contains(cursor_pos)) {
            int cx = cursor_pos.x - monitor_.bounds.left;
            int cy = cursor_pos.y - monitor_.bounds.top;

            constexpr int grid_cells = 15;
            constexpr int cell_size = 10;
            constexpr int grid_size = grid_cells * cell_size;
            constexpr int text_height = 72;
            constexpr int mag_width = grid_size;
            constexpr int mag_height = grid_size + text_height;

            int mx = cx + 20;
            int my = cy + 20;
            if (mx + mag_width > monitor_.bounds.width()) {
                mx = cx - mag_width - 20;
            }
            if (my + mag_height > monitor_.bounds.height()) {
                my = cy - mag_height - 20;
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
                    render_target_->CreateSolidColorBrush(
                        D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f),
                        cell_brush.GetAddressOf()
                    );

                    D2D1_RECT_F cell_rect = D2D1::RectF(
                        static_cast<float>(mx + (dx + grid_cells/2) * cell_size),
                        static_cast<float>(my + (dy + grid_cells/2) * cell_size),
                        static_cast<float>(mx + (dx + grid_cells/2 + 1) * cell_size),
                        static_cast<float>(my + (dy + grid_cells/2 + 1) * cell_size)
                    );
                    render_target_->FillRectangle(cell_rect, cell_brush.Get());
                }
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
                blue_brush_.Get(), 1.0f
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size), static_cast<float>(center_cell_y + cell_size/2)),
                D2D1::Point2F(static_cast<float>(mx + grid_size), static_cast<float>(center_cell_y + cell_size/2)),
                blue_brush_.Get(), 1.0f
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(my)),
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(center_cell_y)),
                blue_brush_.Get(), 1.0f
            );
            render_target_->DrawLine(
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(center_cell_y + cell_size)),
                D2D1::Point2F(static_cast<float>(center_cell_x + cell_size/2), static_cast<float>(my + grid_size)),
                blue_brush_.Get(), 1.0f
            );

            render_target_->DrawRectangle(center_rect, blue_brush_.Get(), 1.0f);

            COLORREF center_color = session_.get_pixel_color(cursor_pos.x, cursor_pos.y);
            std::wstring coord_text = std::format(L"{}, {}", cursor_pos.x, cursor_pos.y);

            std::wstring primary_color;
            if (session_.color_format_hex()) {
                primary_color = std::format(L"#{:02X}{:02X}{:02X}", GetRValue(center_color), GetGValue(center_color), GetBValue(center_color));
            } else {
                primary_color = std::format(L"rgb({},{},{})", GetRValue(center_color), GetGValue(center_color), GetBValue(center_color));
            }

            std::wstring hint_text = L"C 复制 | Shift 切换";

            ComPtr<IDWriteTextFormat> mag_format;
            dwrite_factory()->CreateTextFormat(L"Consolas",
                                               nullptr,
                                               DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               12.0F,
                                               L"zh-CN",
                                               mag_format.GetAddressOf());
            mag_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mag_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            // Line 1: coordinates
            D2D1_RECT_F coord_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + 4),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + grid_size + 24)
            );
            render_target_->DrawTextW(coord_text.c_str(), static_cast<UINT32>(coord_text.size()), mag_format.Get(), coord_rect, white_brush_.Get());

            // Line 2: color value (Hex or RGB)
            D2D1_RECT_F color_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + 24),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + grid_size + 44)
            );
            render_target_->DrawTextW(primary_color.c_str(), static_cast<UINT32>(primary_color.size()), mag_format.Get(), color_rect, white_brush_.Get());

            // Line 3: hint
            ComPtr<IDWriteTextFormat> hint_format;
            dwrite_factory()->CreateTextFormat(L"Microsoft YaHei",
                                               nullptr,
                                               DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               10.5F,
                                               L"zh-CN",
                                               hint_format.GetAddressOf());
            hint_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            hint_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ComPtr<ID2D1SolidColorBrush> hint_brush;
            render_target_->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.65f, 0.7f), hint_brush.GetAddressOf());

            D2D1_RECT_F hint_rect = D2D1::RectF(
                static_cast<float>(mx),
                static_cast<float>(my + grid_size + 44),
                static_cast<float>(mx + mag_width),
                static_cast<float>(my + mag_height - 4)
            );
            render_target_->DrawTextW(hint_text.c_str(), static_cast<UINT32>(hint_text.size()), hint_format.Get(), hint_rect, hint_brush.Get());

            render_target_->DrawRectangle(mag_rect, toolbar_border_brush_.Get(), 1.0f);
        }
    }

    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        background_.Reset();
        render_target_.Reset();
    }
    EndPaint(hwnd_, &paint);
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
    if (message == WM_ERASEBKGND) {
        return 1;
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
        self->session_.on_mouse_up(point);
        return 0;
    }
    if (message == WM_KEYDOWN) {
        self->session_.on_key_down(window, w_param);
        return 0;
    }
    if (message == WM_SETCURSOR) {
        POINT point{};
        GetCursorPos(&point);
        if (self->session_.is_over_toolbar(point)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
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
