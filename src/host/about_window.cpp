#include "about_window.h"

#include "resource.h"

#include "airshot/common.h"
#include "airshot/config.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <string>
#include <format>
#include <mutex>
#include <windowsx.h>

namespace airshot {
namespace {

constexpr int kCloseButton = 1;

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
    POINT mouse_pos{};
    bool is_light_theme{};
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

void discard_resources(AboutState* state);

bool ensure_resources(AboutState* state) {
    AppConfig config = load_config();
    const bool current_theme_is_light = should_use_light_theme(config.theme);
    if (state->render_target) {
        if (state->is_light_theme == current_theme_is_light) {
            return true;
        }
        discard_resources(state);
    }
    state->is_light_theme = current_theme_is_light;

    if (state->window) {
        BOOL use_dark = !state->is_light_theme;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
    }

    RECT rect{};
    GetClientRect(state->window, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rect.right - rect.left),
                                         static_cast<UINT32>(rect.bottom - rect.top));

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = state->d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(state->window, size),
        state->render_target.GetAddressOf()
    );
    if (FAILED(hr)) return false;

    float dpi = static_cast<float>(GetDpiForWindow(state->window));
    if (dpi == 0.0f) {
        dpi = 96.0f;
    }
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

    // Create Text Formats
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN", state->title_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->text_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", state->small_format.GetAddressOf());

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

    state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->text_format.Get(), rect, state->text_white_brush.Get());
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
            HWND edit = CreateWindowExW(0,
                                        L"EDIT",
                                        L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                            ES_READONLY,
                                        24,
                                        170,
                                        712,
                                        360,
                                        window,
                                        nullptr,
                                        nullptr,
                                        nullptr);
            SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(text.size() + 1), 0);
            SetWindowTextW(edit, text.c_str());
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(w_param);
            if (state->is_light_theme) {
                SetTextColor(hdc, RGB(0x1F, 0x23, 0x29));
                SetBkColor(hdc, RGB(0xFF, 0xFF, 0xFF));
                if (state->edit_bg_brush) {
                    DeleteObject(state->edit_bg_brush);
                }
                state->edit_bg_brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
            } else {
                SetTextColor(hdc, RGB(220, 225, 235));
                SetBkColor(hdc, RGB(28, 30, 34));
                if (state->edit_bg_brush) {
                    DeleteObject(state->edit_bg_brush);
                }
                state->edit_bg_brush = CreateSolidBrush(RGB(28, 30, 34));
            }
            return reinterpret_cast<INT_PTR>(state->edit_bg_brush);
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
            float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            if (scale <= 0.0f) scale = 1.0f;
            state->mouse_pos = {
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            if (scale <= 0.0f) scale = 1.0f;
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

        case WM_CLOSE: {
            DestroyWindow(window);
            return 0;
        }

        case WM_DESTROY: {
            if (state->edit_bg_brush) {
                DeleteObject(state->edit_bg_brush);
                state->edit_bg_brush = nullptr;
            }
            discard_resources(state);
            return 0;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

void show_about_window(HWND owner) {
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

    AboutState state;
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (dpi == 0) dpi = 96;
    float scale = static_cast<float>(dpi) / 96.0f;
    const int scaled_width = static_cast<int>(std::round(760 * scale));
    const int scaled_height = static_cast<int>(std::round(610 * scale));

    RECT rect_win{ 0, 0, scaled_width, scaled_height };
    AdjustWindowRectExForDpi(&rect_win, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW, dpi);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.About",
                                  L"关于 Air Screenshot / 许可证",
                                  WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  rect_win.right - rect_win.left,
                                  rect_win.bottom - rect_win.top,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return;
    }

    BOOL use_dark = !should_use_light_theme(config.theme);
    DwmSetWindowAttribute(window, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(window, 19, &use_dark, sizeof(use_dark));
    DWORD corner_preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corner_preference, sizeof(corner_preference));

    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

}  // namespace airshot
