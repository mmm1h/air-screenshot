#include "about_window.h"

#include "resource.h"

#include "airshot/common.h"
#include "airshot/config.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <format>
#include <mutex>
#include <new>
#include <windowsx.h>

namespace airshot {
namespace {

constexpr int kCloseButton = 1;
constexpr int kAboutWidth = 760;
constexpr int kAboutHeight = 610;
constexpr int kWorkAreaMargin = 8;

std::wstring resource_text(int identifier) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (!resource) {
        return {};
    }
    HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD size = SizeofResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    return bytes && size ? from_utf8(std::string_view(static_cast<const char*>(bytes), size)) : std::wstring{};
}

struct AboutState {
    HWND window{};
    HWND owner{};
    std::function<void()> completion;
    POINT mouse_pos{};
    bool is_light_theme{};
    std::wstring theme{L"system"};
    HWND details_edit{};
    HFONT details_font{};
    HBRUSH edit_bg_brush{};

    // D2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target;

    // Brushes
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_white_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_grey_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blue_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_blue_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> control_bg_brush;

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format;

    // Logo bitmap
    Microsoft::WRL::ComPtr<ID2D1Bitmap> logo_bitmap;
};

constexpr UINT_PTR kDetailsEditSubclass = 1;

LRESULT CALLBACK about_details_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) {
    auto* state = reinterpret_cast<AboutState*>(reference_data);
    if (message == WM_KEYDOWN &&
        (w_param == VK_RETURN || w_param == VK_ESCAPE)) {
        if (state && state->window && IsWindow(state->window)) {
            PostMessageW(state->window, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, about_details_proc, subclass_id);
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

void discard_resources(AboutState* state);

float about_layout_scale(HWND window) noexcept {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) {
        return 1.0f;
    }
    const float width_scale =
        static_cast<float>(client.right - client.left) / static_cast<float>(kAboutWidth);
    const float height_scale =
        static_cast<float>(client.bottom - client.top) / static_cast<float>(kAboutHeight);
    const float scale = std::min(width_scale, height_scale);
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

int scale_dip(HWND window, int value) noexcept {
    return static_cast<int>(std::lround(value * about_layout_scale(window)));
}

RECT about_work_area(HMONITOR monitor) noexcept {
    MONITORINFO info{sizeof(info)};
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

SIZE about_outer_size(float scale, UINT dpi) noexcept {
    RECT rect{
        0,
        0,
        std::max(1, static_cast<int>(std::floor(kAboutWidth * scale))),
        std::max(1, static_cast<int>(std::floor(kAboutHeight * scale))),
    };
    AdjustWindowRectExForDpi(
        &rect, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW, dpi == 0 ? 96 : dpi);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

SIZE fitted_about_size(UINT dpi, const RECT& work_area) noexcept {
    const LONG available_width =
        std::max(1L, work_area.right - work_area.left - 2L * kWorkAreaMargin);
    const LONG available_height =
        std::max(1L, work_area.bottom - work_area.top - 2L * kWorkAreaMargin);
    float scale = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f;
    SIZE size = about_outer_size(scale, dpi);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (size.cx <= available_width && size.cy <= available_height) {
            break;
        }
        const float fit = std::min(
            static_cast<float>(available_width) / std::max<LONG>(1, size.cx),
            static_cast<float>(available_height) / std::max<LONG>(1, size.cy));
        scale *= fit;
        size = about_outer_size(scale, dpi);
    }
    size.cx = std::min(size.cx, available_width);
    size.cy = std::min(size.cy, available_height);
    return size;
}

void layout_about_children(AboutState* state) {
    if (!state->details_edit || !IsWindow(state->details_edit)) {
        return;
    }
    SetWindowPos(state->details_edit,
                 nullptr,
                 scale_dip(state->window, 24),
                 scale_dip(state->window, 170),
                 scale_dip(state->window, 712),
                 scale_dip(state->window, 360),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int font_height = std::max(
        1, static_cast<int>(std::lround(10.0f * about_layout_scale(state->window) * 96.0f / 72.0f)));
    HFONT font = CreateFontW(-font_height,
                             0,
                             0,
                             0,
                             FW_NORMAL,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei");
    if (font) {
        SendMessageW(state->details_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (state->details_font) {
            DeleteObject(state->details_font);
        }
        state->details_font = font;
    }
}

bool ensure_resources(AboutState* state) {
    if (state->render_target) {
        return true;
    }

    if (state->window) {
        BOOL use_dark = !state->is_light_theme;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
    }

    RECT rect{};
    if (!GetClientRect(state->window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rect.right - rect.left),
                                         static_cast<UINT32>(rect.bottom - rect.top));
    const auto fail = [state] {
        discard_resources(state);
        return false;
    };

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
    if (FAILED(hr)) return fail();

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return fail();

    hr = state->d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(state->window, size),
        state->render_target.GetAddressOf()
    );
    if (FAILED(hr)) return fail();

    const float dpi = 96.0f * about_layout_scale(state->window);
    state->render_target->SetDpi(dpi, dpi);

    // Create Brushes
    if (state->is_light_theme) {
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xF5F6F7), state->bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x1F2329), state->text_white_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x646A73), state->text_grey_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x0066FF), state->blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x3385FF), state->hover_blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xDEE0E3), state->border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), state->control_bg_brush.GetAddressOf());
    } else {
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(18.0f / 255.0f, 19.0f / 255.0f, 22.0f / 255.0f), state->bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f), state->text_white_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(150.0f / 255.0f, 160.0f / 255.0f, 175.0f / 255.0f), state->text_grey_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0.0f / 255.0f, 102.0f / 255.0f, 255.0f / 255.0f), state->blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(51.0f / 255.0f, 136.0f / 255.0f, 255.0f / 255.0f), state->hover_blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(38.0f / 255.0f, 41.0f / 255.0f, 48.0f / 255.0f), state->border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(28.0f / 255.0f, 30.0f / 255.0f, 34.0f / 255.0f), state->control_bg_brush.GetAddressOf());
    }
    if (!state->bg_brush || !state->text_white_brush || !state->text_grey_brush ||
        !state->blue_brush || !state->hover_blue_brush || !state->border_brush ||
        !state->control_bg_brush) {
        return fail();
    }

    // Create Text Formats
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN", state->title_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->text_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", state->small_format.GetAddressOf());
    if (!state->title_format || !state->text_format || !state->small_format) {
        return fail();
    }

    state->title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->title_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    state->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    state->small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // Load logo HICON and convert to D2D bitmap
    HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 96, 96, LR_SHARED);
    if (hIcon) {
        Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_factory.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
            hr = wic_factory->CreateBitmapFromHICON(hIcon, wic_bitmap.GetAddressOf());
            if (SUCCEEDED(hr)) {
                state->render_target->CreateSharedBitmap(__uuidof(IWICBitmap), wic_bitmap.Get(), nullptr, state->logo_bitmap.GetAddressOf());
            }
        }
    }

    return true;
}

void discard_resources(AboutState* state) {
    state->render_target.Reset();
    state->d2d_factory.Reset();
    state->dwrite_factory.Reset();
    state->logo_bitmap.Reset();
    state->bg_brush.Reset();
    state->text_white_brush.Reset();
    state->text_grey_brush.Reset();
    state->blue_brush.Reset();
    state->hover_blue_brush.Reset();
    state->border_brush.Reset();
    state->control_bg_brush.Reset();
    state->title_format.Reset();
    state->text_format.Reset();
    state->small_format.Reset();
    if (state->edit_bg_brush) {
        DeleteObject(state->edit_bg_brush);
        state->edit_bg_brush = nullptr;
    }
}

void refresh_about_theme(AboutState* state) {
    const bool light = should_use_light_theme(state->theme);
    if (state->is_light_theme == light) {
        return;
    }
    state->is_light_theme = light;
    discard_resources(state);
    BOOL use_dark = !light;
    DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
    InvalidateRect(state->window, nullptr, TRUE);
    if (state->details_edit) {
        InvalidateRect(state->details_edit, nullptr, TRUE);
    }
}

void draw_button(AboutState* state, int x1, int y1, int x2, int y2, const wchar_t* label, bool is_hovered) {
    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 4.0f, 4.0f);

    if (is_hovered) {
        state->render_target->FillRoundedRectangle(rounded, state->hover_blue_brush.Get());
        state->render_target->DrawRoundedRectangle(rounded, state->hover_blue_brush.Get(), 1.0f);
    } else {
        state->render_target->FillRoundedRectangle(rounded, state->blue_brush.Get());
        state->render_target->DrawRoundedRectangle(rounded, state->border_brush.Get(), 1.0f);
    }

    ID2D1SolidColorBrush* text_brush =
        state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get();
    state->render_target->DrawTextW(
        label,
        static_cast<UINT32>(wcslen(label)),
        state->text_format.Get(),
        rect,
        text_brush);
}

LRESULT CALLBACK about_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AboutState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<AboutState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_CREATE: {
            const std::wstring text =
                std::format(L"Air Screenshot {}\r\n"
                            L"https://github.com/mmm1h/air-screenshot\r\n\r\n"
                            L"LICENSE\r\n=======\r\n{}\r\n\r\n"
                            L"THIRD-PARTY NOTICES\r\n===================\r\n{}",
                            from_utf8(AIRSHOT_VERSION),
                            resource_text(IDR_LICENSE_TEXT),
                            resource_text(IDR_THIRD_PARTY_TEXT));
            // Create a styled read-only EDIT control (for smooth native scrollbars)
            state->details_edit = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_READONLY,
                0,
                0,
                0,
                0,
                window,
                nullptr,
                nullptr,
                nullptr);
            if (!state->details_edit) {
                return 0;
            }
            SetWindowSubclass(
                state->details_edit,
                about_details_proc,
                kDetailsEditSubclass,
                reinterpret_cast<DWORD_PTR>(state));
            SendMessageW(state->details_edit,
                         EM_SETLIMITTEXT,
                         static_cast<WPARAM>(text.size() + 1),
                         0);
            SetWindowTextW(state->details_edit, text.c_str());
            layout_about_children(state);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(w_param);
            if (state->is_light_theme) {
                SetTextColor(hdc, RGB(0x1F, 0x23, 0x29));
                SetBkColor(hdc, RGB(0xFF, 0xFF, 0xFF));
                if (!state->edit_bg_brush) {
                    state->edit_bg_brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
                }
            } else {
                SetTextColor(hdc, RGB(220, 225, 235));
                SetBkColor(hdc, RGB(28, 30, 34));
                if (!state->edit_bg_brush) {
                    state->edit_bg_brush = CreateSolidBrush(RGB(28, 30, 34));
                }
            }
            return state->edit_bg_brush
                       ? reinterpret_cast<INT_PTR>(state->edit_bg_brush)
                       : DefWindowProcW(window, message, w_param, l_param);
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            if (state->theme == L"system") {
                refresh_about_theme(state);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(window, &ps);
            if (ensure_resources(state)) {
                state->render_target->BeginDraw();
                // Clear background
                if (state->is_light_theme) {
                    state->render_target->Clear(D2D1::ColorF(0xF5F6F7));
                } else {
                    state->render_target->Clear(D2D1::ColorF(18.0f / 255.0f, 19.0f / 255.0f, 22.0f / 255.0f));
                }

                // Draw logo
                if (state->logo_bitmap) {
                    // Draw centered logo
                    D2D1_RECT_F logo_rect = D2D1::RectF(
                        (760.0f - 80.0f) / 2.0f,
                        24.0f,
                        (760.0f + 80.0f) / 2.0f,
                        104.0f
                    );
                    state->render_target->DrawBitmap(state->logo_bitmap.Get(), logo_rect);
                }

                // Draw title
                D2D1_RECT_F title_rect = D2D1::RectF(0.0f, 114.0f, 760.0f, 134.0f);
                state->render_target->DrawTextW(L"Air Screenshot", 14, state->title_format.Get(), title_rect, state->text_white_brush.Get());

                // Draw version
                std::wstring ver_str = L"版本: " + from_utf8(AIRSHOT_VERSION);
                D2D1_RECT_F ver_rect = D2D1::RectF(0.0f, 138.0f, 760.0f, 154.0f);
                state->render_target->DrawTextW(ver_str.c_str(), static_cast<UINT32>(ver_str.size()), state->small_format.Get(), ver_rect, state->text_grey_brush.Get());

                // Draw Close Button (x: 638, y: 546, w: 98, h: 32)
                bool close_hovered = (state->mouse_pos.x >= 638 && state->mouse_pos.x <= 736 && state->mouse_pos.y >= 546 && state->mouse_pos.y <= 578);
                draw_button(state, 638, 546, 736, 578, L"关闭", close_hovered);

                HRESULT end_hr = state->render_target->EndDraw();
                if (end_hr == D2DERR_RECREATE_TARGET) {
                    discard_resources(state);
                }
            }
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const float scale = about_layout_scale(window);
            state->mouse_pos = {
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float scale = about_layout_scale(window);
            POINT pt{
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            // Check Close button click: x: 638..736, y: 546..578
            if (pt.x >= 638 && pt.x <= 736 && pt.y >= 546 && pt.y <= 578) {
                DestroyWindow(window);
                return 0;
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (w_param == VK_RETURN || w_param == VK_ESCAPE) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            break;

        case WM_SIZE: {
            layout_about_children(state);
            discard_resources(state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested_rect = reinterpret_cast<const RECT*>(l_param);
            const HMONITOR monitor =
                MonitorFromRect(suggested_rect, MONITOR_DEFAULTTONEAREST);
            const RECT work_area = about_work_area(monitor);
            const SIZE size = fitted_about_size(HIWORD(w_param), work_area);
            const LONG minimum_x = work_area.left + kWorkAreaMargin;
            const LONG minimum_y = work_area.top + kWorkAreaMargin;
            const LONG maximum_x = work_area.right - kWorkAreaMargin - size.cx;
            const LONG maximum_y = work_area.bottom - kWorkAreaMargin - size.cy;
            const int x = static_cast<int>(std::clamp(
                suggested_rect->left, minimum_x, std::max(minimum_x, maximum_x)));
            const int y = static_cast<int>(std::clamp(
                suggested_rect->top, minimum_y, std::max(minimum_y, maximum_y)));
            SetWindowPos(window,
                         nullptr,
                         x,
                         y,
                         size.cx,
                         size.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout_about_children(state);
            discard_resources(state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }

        case WM_CLOSE: {
            DestroyWindow(window);
            return 0;
        }

        case WM_DESTROY: {
            discard_resources(state);
            if (state->details_font) {
                DeleteObject(state->details_font);
                state->details_font = nullptr;
            }
            return 0;
        }

        case WM_NCDESTROY: {
            const HWND owner = state->owner;
            auto completion = std::move(state->completion);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT default_result = DefWindowProcW(window, message, w_param, l_param);
            if (owner && IsWindow(owner)) {
                EnableWindow(owner, TRUE);
                SetForegroundWindow(owner);
            }
            delete state;
            if (completion) {
                completion();
            }
            return default_result;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

HWND show_about_window_async(HWND owner, std::function<void()> completion) {
    AppConfig config = load_config();
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = about_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
        window_class.hbrBackground = CreateSolidBrush(RGB(18, 19, 22));
        window_class.lpszClassName = L"AirScreenshot.About";
        RegisterClassExW(&window_class);
    });

    auto* state = new (std::nothrow) AboutState;
    if (!state) {
        if (completion) {
            completion();
        }
        return nullptr;
    }
    state->owner = owner;
    state->completion = std::move(completion);
    state->theme = config.theme;
    state->is_light_theme = should_use_light_theme(config.theme);
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (dpi == 0) dpi = 96;

    RECT owner_rect{};
    const bool has_owner_rect =
        owner && IsWindowVisible(owner) && GetWindowRect(owner, &owner_rect);
    const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
    const RECT work_area = about_work_area(monitor);
    const SIZE outer_size = fitted_about_size(dpi, work_area);
    const LONG preferred_x =
        has_owner_rect
            ? owner_rect.left + ((owner_rect.right - owner_rect.left) - outer_size.cx) / 2
            : work_area.left + (work_area.right - work_area.left - outer_size.cx) / 2;
    const LONG preferred_y =
        has_owner_rect
            ? owner_rect.top + ((owner_rect.bottom - owner_rect.top) - outer_size.cy) / 2
            : work_area.top + (work_area.bottom - work_area.top - outer_size.cy) / 2;
    const auto clamp_axis = [](LONG value, int size, LONG minimum, LONG maximum) {
        if (size >= maximum - minimum) {
            return static_cast<int>(minimum);
        }
        return static_cast<int>(std::clamp<LONG>(value, minimum, maximum - size));
    };
    const int x = clamp_axis(preferred_x,
                             outer_size.cx,
                             work_area.left + kWorkAreaMargin,
                             work_area.right - kWorkAreaMargin);
    const int y = clamp_axis(preferred_y,
                             outer_size.cy,
                             work_area.top + kWorkAreaMargin,
                             work_area.bottom - kWorkAreaMargin);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.About",
                                  L"关于 Air Screenshot / 许可证",
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  outer_size.cx,
                                  outer_size.cy,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  state);
    if (!window) {
        auto failed_completion = std::move(state->completion);
        delete state;
        if (failed_completion) {
            failed_completion();
        }
        return nullptr;
    }

    BOOL use_dark = !state->is_light_theme;
    DwmSetWindowAttribute(window, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(window, 19, &use_dark, sizeof(use_dark));
    DWORD corner_preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corner_preference, sizeof(corner_preference));

    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

void show_about_window(HWND owner) {
    bool completed = false;
    HWND window = show_about_window_async(owner, [&] { completed = true; });
    if (!window && !completed) {
        return;
    }

    MSG message{};
    while (!completed) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            if (window && IsWindow(window)) {
                DestroyWindow(window);
            }
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace airshot
