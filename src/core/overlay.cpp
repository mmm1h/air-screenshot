// Selection-mask and lightweight annotation behavior is adapted from
// xland/ScreenCapture main@1574683043fa5f64b6cd45d9ec2e0db1bafbc15b.
// This implementation was substantially rewritten for Air Screenshot.

#include "airshot/overlay.h"

#include "overlay_helpers.h"

#include "airshot/ocr.h"
#include "airshot/output.h"
#include "airshot/strings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <commctrl.h>
#include <imm.h>
#include <format>
#include <functional>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;
using namespace overlay_detail;

enum class Tool {
    none,
    rectangle,
    arrow,
    mosaic,
    text,
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

struct Annotation {
    Tool tool{Tool::none};
    POINT start{};
    POINT end{};
    std::vector<POINT> points;
    std::wstring text;
    COLORREF color{RGB(22, 119, 255)};
    float width{3.0F};
};

struct ToolbarButton {
    std::wstring id;
    std::wstring label;
    RectI bounds;
};

class OverlaySession;

class OverlayWindow {
public:
    OverlayWindow(OverlaySession& session, MonitorSnapshot& monitor) : session_(session), monitor_(monitor) {}
    ~OverlayWindow() { destroy(); }

    bool create();
    void destroy();
    void invalidate() const;
    void paint();
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] const RectI& bounds() const noexcept { return monitor_.bounds; }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

private:
    bool create_render_target();
    D2D1_RECT_F local_rect(const RectI& rect) const;
    void draw_annotation(const Annotation& annotation, bool preview);
    void draw_arrow(POINT start, POINT end, ID2D1Brush* brush, float width);

    OverlaySession& session_;
    MonitorSnapshot& monitor_;
    HWND hwnd_{};
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<ID2D1Bitmap> background_;
    ComPtr<ID2D1SolidColorBrush> dim_brush_;
    ComPtr<ID2D1SolidColorBrush> blue_brush_;
    ComPtr<ID2D1SolidColorBrush> white_brush_;
    ComPtr<ID2D1SolidColorBrush> toolbar_brush_;
    ComPtr<ID2D1SolidColorBrush> toolbar_bg_brush_;
    ComPtr<ID2D1SolidColorBrush> toolbar_border_brush_;
    ComPtr<ID2D1SolidColorBrush> hover_bg_brush_;
    ComPtr<ID2D1SolidColorBrush> active_bg_brush_;
};

class OverlaySession {
public:
#include "overlay_session_public.inl"

private:
#include "overlay_session_private.inl"

    RegionRequest request_;
    RegionResult result_;
    std::vector<MonitorSnapshot> monitors_;
    std::vector<WindowCandidate> window_candidates_;
    std::vector<std::unique_ptr<OverlayWindow>> windows_;
    RectI virtual_bounds_;
    RectI selection_;
    RectI hover_;
    RectI clicked_window_;
    POINT drag_start_{};
    bool dragging_selection_{};
    bool selection_complete_{};
    bool drawing_annotation_{};
    bool done_{};
    Tool active_tool_{Tool::none};
    Annotation preview_;
    std::vector<Annotation> annotations_;
    std::vector<Annotation> redo_;
    std::vector<ToolbarButton> toolbar_;

    DragMode current_drag_mode_{DragMode::none};
    RectI original_selection_;
    std::vector<ToolbarButton> sub_toolbar_;
    COLORREF active_color_{RGB(245, 34, 45)};
    COLORREF custom_color_{RGB(128, 0, 255)};
    float active_width_{4.0F};
    std::wstring hovered_button_id_;
    POINT cursor_pos_{};
    bool color_format_hex_{true};

    friend class OverlayWindow;
};

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
    if (preview) {
        color = session_.active_color();
        draw_width = session_.active_width();
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    render_target_->CreateSolidColorBrush(
        D2D1::ColorF(GetRValue(color) / 255.0F, GetGValue(color) / 255.0F, GetBValue(color) / 255.0F),
        brush.GetAddressOf()
    );

    if (annotation.tool == Tool::rectangle) {
        render_target_->DrawRectangle(local_rect(RectI{start.x, start.y, end.x, end.y}.normalized()), brush.Get(), draw_width);
    } else if (annotation.tool == Tool::arrow) {
        draw_arrow(start, end, brush.Get(), draw_width);
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
    }
}

#include "overlay_window_paint.inl"

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

}  // namespace

RegionResult run_region_capture(const RegionRequest& request) {
    OverlaySession session(request);
    RegionResult result = session.run();
    release_overlay_factories();
    return result;
}

}  // namespace airshot
