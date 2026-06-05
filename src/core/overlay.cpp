// Selection-mask and lightweight annotation behavior is adapted from
// xland/ScreenCapture main@1574683043fa5f64b6cd45d9ec2e0db1bafbc15b.
// This implementation was substantially rewritten for Air Screenshot.

#include "airshot/overlay.h"

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

COLORREF parse_hex_color(std::wstring_view hex, COLORREF fallback) {
    if (hex.size() == 7 && hex[0] == L'#') {
        wchar_t* end = nullptr;
        unsigned long val = std::wcstoul(hex.data() + 1, &end, 16);
        if (end == hex.data() + 7) {
            BYTE r = (val >> 16) & 0xFF;
            BYTE g = (val >> 8) & 0xFF;
            BYTE b = val & 0xFF;
            return RGB(r, g, b);
        }
    }
    return fallback;
}

std::wstring format_hex_color(COLORREF color) {
    return std::format(L"#{:02X}{:02X}{:02X}", GetRValue(color), GetGValue(color), GetBValue(color));
}

struct RGBPickerState {
    HWND window{};
    HWND parent_window{};
    COLORREF current_color{};
    std::function<void(COLORREF)> on_color_changed;

    HWND slider_r{};
    HWND slider_g{};
    HWND slider_b{};
    HWND edit_r{};
    HWND edit_g{};
    HWND edit_b{};
    HWND edit_hex{};

    bool updating_controls{false};
    DWORD creation_time{};
};

LRESULT CALLBACK RgbEditSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    (void)dwRefData;
    switch (msg) {
        case WM_CHAR: {
            wchar_t ch = static_cast<wchar_t>(wparam);
            if (ch == 0x08) break; // Backspace
            if (ch >= L'0' && ch <= L'9') break; // Digits
            return 0; // Block
        }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK HexEditSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    (void)dwRefData;
    switch (msg) {
        case WM_CHAR: {
            wchar_t ch = static_cast<wchar_t>(wparam);
            if (ch == 0x08) break; // Backspace
            if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F')) break; // Hex digits
            return 0; // Block
        }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

HFONT get_picker_font() {
    static HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    return font;
}

LRESULT CALLBACK RGBColorPickerProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<RGBPickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<RGBPickerState*>(create->lpCreateParams);
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }

    switch (msg) {
        case WM_CREATE: {
            HFONT font = get_picker_font();

            HWND lbl_r = CreateWindowExW(0, L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 12, 14, 16, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(lbl_r, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            state->slider_r = CreateWindowExW(0, TRACKBAR_CLASS, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 32, 10, 150, 25, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->slider_r, TBM_SETRANGE, TRUE, MAKELONG(0, 255));

            state->edit_r = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_NUMBER, 192, 11, 42, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->edit_r, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(state->edit_r, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(4, 4));

            HWND lbl_g = CreateWindowExW(0, L"STATIC", L"G", WS_CHILD | WS_VISIBLE, 12, 44, 16, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(lbl_g, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            state->slider_g = CreateWindowExW(0, TRACKBAR_CLASS, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 32, 40, 150, 25, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->slider_g, TBM_SETRANGE, TRUE, MAKELONG(0, 255));

            state->edit_g = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_NUMBER, 192, 41, 42, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->edit_g, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(state->edit_g, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(4, 4));

            HWND lbl_b = CreateWindowExW(0, L"STATIC", L"B", WS_CHILD | WS_VISIBLE, 12, 74, 16, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(lbl_b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            state->slider_b = CreateWindowExW(0, TRACKBAR_CLASS, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 32, 70, 150, 25, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->slider_b, TBM_SETRANGE, TRUE, MAKELONG(0, 255));

            state->edit_b = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_NUMBER, 192, 71, 42, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->edit_b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(state->edit_b, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(4, 4));

            HWND lbl_hex = CreateWindowExW(0, L"STATIC", L"#", WS_CHILD | WS_VISIBLE, 12, 114, 16, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(lbl_hex, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            state->edit_hex = CreateWindowExW(0, L"EDIT", L"FFFFFF", WS_CHILD | WS_VISIBLE | ES_UPPERCASE, 32, 111, 70, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(state->edit_hex, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(state->edit_hex, EM_SETLIMITTEXT, 6, 0);
            SendMessageW(state->edit_hex, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(4, 4));

            SetWindowSubclass(state->edit_r, RgbEditSubclassProc, 0, 0);
            SetWindowSubclass(state->edit_g, RgbEditSubclassProc, 0, 0);
            SetWindowSubclass(state->edit_b, RgbEditSubclassProc, 0, 0);
            SetWindowSubclass(state->edit_hex, HexEditSubclassProc, 0, 0);

            state->updating_controls = true;

            int r = GetRValue(state->current_color);
            int g = GetGValue(state->current_color);
            int b = GetBValue(state->current_color);

            SendMessageW(state->slider_r, TBM_SETPOS, TRUE, r);
            SendMessageW(state->slider_g, TBM_SETPOS, TRUE, g);
            SendMessageW(state->slider_b, TBM_SETPOS, TRUE, b);

            SetWindowTextW(state->edit_r, std::to_wstring(r).c_str());
            SetWindowTextW(state->edit_g, std::to_wstring(g).c_str());
            SetWindowTextW(state->edit_b, std::to_wstring(b).c_str());

            wchar_t hex_str[10]{};
            swprintf_s(hex_str, L"%02X%02X%02X", r, g, b);
            SetWindowTextW(state->edit_hex, hex_str);

            state->updating_controls = false;

            RECT wr{};
            GetWindowRect(hwnd, &wr);
            HRGN rgn = CreateRoundRectRgn(0, 0, wr.right - wr.left, wr.bottom - wr.top, 8, 8);
            SetWindowRgn(hwnd, rgn, TRUE);

            SetFocus(state->edit_hex);
            break;
        }
        case WM_ACTIVATE: {
            if (LOWORD(w_param) == WA_INACTIVE) {
                if (GetTickCount() - state->creation_time > 300) {
                    DestroyWindow(hwnd);
                }
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(w_param);
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(43, 47, 54));
            static HBRUSH edit_bg = CreateSolidBrush(RGB(43, 47, 54));
            return reinterpret_cast<INT_PTR>(edit_bg);
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(w_param);
            SetTextColor(hdc, RGB(216, 222, 233));
            SetBkColor(hdc, RGB(26, 29, 33));
            static HBRUSH static_bg = CreateSolidBrush(RGB(26, 29, 33));
            return reinterpret_cast<INT_PTR>(static_bg);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT r{};
            GetClientRect(hwnd, &r);
            HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(76, 82, 93));
            HGDIOBJ old_pen = SelectObject(hdc, border_pen);
            HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8, 8);

            Rectangle(hdc, 191, 10, 192 + 42 + 1, 11 + 22 + 1);
            Rectangle(hdc, 191, 40, 192 + 42 + 1, 41 + 22 + 1);
            Rectangle(hdc, 191, 70, 192 + 42 + 1, 71 + 22 + 1);
            Rectangle(hdc, 31, 110, 32 + 70 + 1, 111 + 22 + 1);

            RECT preview_rect{ 114, 111, 114 + 120, 111 + 22 };
            HBRUSH preview_brush = CreateSolidBrush(state->current_color);
            FillRect(hdc, &preview_rect, preview_brush);
            DeleteObject(preview_brush);

            Rectangle(hdc, 113, 110, 114 + 120 + 1, 111 + 22 + 1);

            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(border_pen);

            EndPaint(hwnd, &ps);
            break;
        }
        case WM_HSCROLL: {
            if (state->updating_controls) break;

            int r = static_cast<int>(SendMessageW(state->slider_r, TBM_GETPOS, 0, 0));
            int g = static_cast<int>(SendMessageW(state->slider_g, TBM_GETPOS, 0, 0));
            int b = static_cast<int>(SendMessageW(state->slider_b, TBM_GETPOS, 0, 0));

            state->current_color = RGB(r, g, b);

            state->updating_controls = true;
            SetWindowTextW(state->edit_r, std::to_wstring(r).c_str());
            SetWindowTextW(state->edit_g, std::to_wstring(g).c_str());
            SetWindowTextW(state->edit_b, std::to_wstring(b).c_str());

            wchar_t hex_str[10]{};
            swprintf_s(hex_str, L"%02X%02X%02X", r, g, b);
            SetWindowTextW(state->edit_hex, hex_str);
            state->updating_controls = false;

            RECT preview_rect{ 113, 110, 114 + 120 + 2, 111 + 22 + 2 };
            InvalidateRect(hwnd, &preview_rect, TRUE);

            if (state->on_color_changed) {
                state->on_color_changed(state->current_color);
            }
            break;
        }
        case WM_COMMAND: {
            if (state->updating_controls) break;

            int id = LOWORD(w_param);
            (void)id;
            int code = HIWORD(w_param);
            HWND ctrl_hwnd = reinterpret_cast<HWND>(l_param);

            if (code == EN_CHANGE) {
                if (ctrl_hwnd == state->edit_r || ctrl_hwnd == state->edit_g || ctrl_hwnd == state->edit_b) {
                    wchar_t r_buf[16]{};
                    wchar_t g_buf[16]{};
                    wchar_t b_buf[16]{};
                    GetWindowTextW(state->edit_r, r_buf, 16);
                    GetWindowTextW(state->edit_g, g_buf, 16);
                    GetWindowTextW(state->edit_b, b_buf, 16);

                    int r = r_buf[0] ? std::clamp(std::wcstol(r_buf, nullptr, 10), 0L, 255L) : 0;
                    int g = g_buf[0] ? std::clamp(std::wcstol(g_buf, nullptr, 10), 0L, 255L) : 0;
                    int b = b_buf[0] ? std::clamp(std::wcstol(b_buf, nullptr, 10), 0L, 255L) : 0;

                    state->current_color = RGB(r, g, b);

                    state->updating_controls = true;
                    SendMessageW(state->slider_r, TBM_SETPOS, TRUE, r);
                    SendMessageW(state->slider_g, TBM_SETPOS, TRUE, g);
                    SendMessageW(state->slider_b, TBM_SETPOS, TRUE, b);

                    wchar_t hex_str[10]{};
                    swprintf_s(hex_str, L"%02X%02X%02X", r, g, b);
                    SetWindowTextW(state->edit_hex, hex_str);
                    state->updating_controls = false;

                    RECT preview_rect{ 113, 110, 114 + 120 + 2, 111 + 22 + 2 };
                    InvalidateRect(hwnd, &preview_rect, TRUE);

                    if (state->on_color_changed) {
                        state->on_color_changed(state->current_color);
                    }
                } else if (ctrl_hwnd == state->edit_hex) {
                    wchar_t hex_buf[16]{};
                    GetWindowTextW(state->edit_hex, hex_buf, 16);
                    if (wcslen(hex_buf) == 6) {
                        wchar_t* end = nullptr;
                        unsigned long val = std::wcstoul(hex_buf, &end, 16);
                        if (end == hex_buf + 6) {
                            int r = (val >> 16) & 0xFF;
                            int g = (val >> 8) & 0xFF;
                            int b = val & 0xFF;

                            state->current_color = RGB(r, g, b);

                            state->updating_controls = true;
                            SendMessageW(state->slider_r, TBM_SETPOS, TRUE, r);
                            SendMessageW(state->slider_g, TBM_SETPOS, TRUE, g);
                            SendMessageW(state->slider_b, TBM_SETPOS, TRUE, b);

                            SetWindowTextW(state->edit_r, std::to_wstring(r).c_str());
                            SetWindowTextW(state->edit_g, std::to_wstring(g).c_str());
                            SetWindowTextW(state->edit_b, std::to_wstring(b).c_str());
                            state->updating_controls = false;

                            RECT preview_rect{ 113, 110, 114 + 120 + 2, 111 + 22 + 2 };
                            InvalidateRect(hwnd, &preview_rect, TRUE);

                            if (state->on_color_changed) {
                                state->on_color_changed(state->current_color);
                            }
                        }
                    }
                }
            }
            break;
        }
        case WM_DESTROY: {
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void show_rgb_picker_popup(HWND parent_hwnd, COLORREF initial_color, const RectI& button_bounds, const RectI& monitor_bounds, std::function<void(COLORREF)> on_color_changed) {
    static std::once_flag picker_class_flag;
    std::call_once(picker_class_flag, [] {
        INITCOMMONCONTROLSEX icex{sizeof(icex)};
        icex.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = RGBColorPickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(26, 29, 33));
        wc.lpszClassName = L"AirScreenshot.RGBColorPicker";
        RegisterClassExW(&wc);
    });

    int popup_w = 260;
    int popup_h = 150;

    int button_center_x = button_bounds.left + button_bounds.width() / 2;
    int px = button_center_x - popup_w / 2;
    int py = button_bounds.top - popup_h - 5;

    if (py < monitor_bounds.top) {
        py = button_bounds.bottom + 5;
    }
    if (px < monitor_bounds.left) {
        px = monitor_bounds.left + 5;
    }
    if (px + popup_w > monitor_bounds.right) {
        px = monitor_bounds.right - popup_w - 5;
    }

    auto* state = new RGBPickerState();
    state->parent_window = parent_hwnd;
    state->current_color = initial_color;
    state->on_color_changed = std::move(on_color_changed);
    state->creation_time = GetTickCount();

    HWND picker_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                       L"AirScreenshot.RGBColorPicker",
                                       L"",
                                       WS_POPUP,
                                       px, py, popup_w, popup_h,
                                       parent_hwnd,
                                       nullptr,
                                       GetModuleHandleW(nullptr),
                                       state);
    if (picker_hwnd) {
        ShowWindow(picker_hwnd, SW_SHOW);
        UpdateWindow(picker_hwnd);
        SetFocus(picker_hwnd);
    } else {
        delete state;
    }
}

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

struct PromptState {
    HWND window{};
    HWND edit{};
    bool accepted{};
    std::wstring text;
};

LRESULT CALLBACK text_prompt_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<PromptState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_CREATE) {
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                      L"EDIT",
                                      L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      12,
                                      12,
                                      380,
                                      26,
                                      window,
                                      reinterpret_cast<HMENU>(100),
                                      nullptr,
                                      nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_confirm.data(),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        226,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDOK),
                        nullptr,
                        nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_cancel.data(),
                        WS_CHILD | WS_VISIBLE,
                        312,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDCANCEL),
                        nullptr,
                        nullptr);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SetFocus(state->edit);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            state->text.resize(static_cast<std::size_t>(length + 1));
            if (length > 0) {
                GetWindowTextW(state->edit, state->text.data(), length + 1);
            }
            state->text.resize(static_cast<std::size_t>(length));
            state->accepted = !state->text.empty();
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

std::optional<std::wstring> prompt_text(HWND owner, POINT position) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = text_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"AirScreenshot.TextPrompt";
        RegisterClassExW(&window_class);
    });

    PromptState state;
    const int x = std::max(0, static_cast<int>(position.x) - 200);
    const int y = std::max(0, static_cast<int>(position.y) - 60);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.TextPrompt",
                                  strings::prompt_text_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  420,
                                  130,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return std::nullopt;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.accepted ? std::optional(state.text) : std::nullopt;
}

class ScrollBorderWindow {
public:
    static HWND create(HINSTANCE instance, HWND parent, const RectI& bounds) {
        static std::once_flag class_flag;
        std::call_once(class_flag, [instance] {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
            wc.lpszClassName = L"AirScreenshot.ScrollBorder";
            RegisterClassExW(&wc);
        });

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"AirScreenshot.ScrollBorder",
            L"",
            WS_POPUP,
            bounds.left - 2, bounds.top - 2, bounds.width() + 4, bounds.height() + 4,
            parent,
            nullptr,
            instance,
            nullptr
        );
        if (hwnd) {
            SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
            SetWindowSubclass(hwnd, [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps;
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT r;
                    GetClientRect(hwnd, &r);

                    // 1. Draw selection border (Blue, 2px)
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(22, 119, 255));
                    HGDIOBJ old_pen = SelectObject(hdc, pen);
                    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                    SelectObject(hdc, old_brush);
                    SelectObject(hdc, old_pen);
                    DeleteObject(pen);

                    // 2. Draw floating prompt bar at the bottom of the selection
                    int width = r.right - r.left;
                    int height = r.bottom - r.top;
                    if (width >= 360 && height >= 100) {
                        int bar_w = 340;
                        int bar_h = 28;
                        int bar_x = (width - bar_w) / 2;
                        int bar_y = height - bar_h - 20;

                        HBRUSH bar_bg = CreateSolidBrush(RGB(17, 19, 22));
                        HPEN bar_border = CreatePen(PS_SOLID, 1, RGB(60, 64, 70));
                        HGDIOBJ prev_pen = SelectObject(hdc, bar_border);
                        HGDIOBJ prev_brush = SelectObject(hdc, bar_bg);

                        RoundRect(hdc, bar_x, bar_y, bar_x + bar_w, bar_y + bar_h, 6, 6);

                        SelectObject(hdc, prev_brush);
                        SelectObject(hdc, prev_pen);
                        DeleteObject(bar_bg);
                        DeleteObject(bar_border);

                        SetTextColor(hdc, RGB(220, 224, 230));
                        SetBkMode(hdc, TRANSPARENT);
                        HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
                        HGDIOBJ old_font = SelectObject(hdc, font);

                        RECT text_rc{bar_x, bar_y, bar_x + bar_w, bar_y + bar_h};
                        DrawTextW(hdc, L"滚动滚轮进行长截图 | 单击自动滚动 | Enter完成 | Esc取消", -1, &text_rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

                        SelectObject(hdc, old_font);
                        DeleteObject(font);
                    }

                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefSubclassProc(hwnd, msg, wp, lp);
            }, 0, 0);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return hwnd;
    }
};

struct ScrollControlState {
    bool finished{false};
    bool cancelled{false};
};

class ScrollControlWindow {
public:
    static HWND create(HINSTANCE instance, HWND parent, const RectI& selection, ScrollControlState* state) {
        static std::once_flag class_flag;
        std::call_once(class_flag, [instance] {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(RGB(17, 19, 22));
            wc.lpszClassName = L"AirScreenshot.ScrollControl";
            RegisterClassExW(&wc);
        });

        int w = 240;
        int h = 50;
        int x = selection.right - w;
        int y = selection.bottom + 6;

        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (x + w > screen_w) x = screen_w - w - 10;
        if (x < 0) x = 10;
        if (y + h > screen_h) y = selection.top - h - 6;
        if (y < 0) y = 10;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"AirScreenshot.ScrollControl",
            L"",
            WS_POPUP,
            x, y, w, h,
            parent,
            nullptr,
            instance,
            state
        );
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
        return hwnd;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* state = reinterpret_cast<ScrollControlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<ScrollControlState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

        static int hover_btn = 0; // 0 = none, 1 = OK, 2 = Cancel
        static int pressed_btn = 0; // 0 = none, 1 = OK, 2 = Cancel
        static int blink_counter = 0;

        switch (msg) {
            case WM_CREATE: {
                SetTimer(hwnd, 1, 500, nullptr);
                hover_btn = 0;
                pressed_btn = 0;
                blink_counter = 0;
                break;
            }
            case WM_TIMER: {
                if (wp == 1) {
                    blink_counter++;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case WM_SETTEXT: {
                LRESULT res = DefWindowProcW(hwnd, msg, wp, lp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return res;
            }
            case WM_MOUSEMOVE: {
                int x = static_cast<short>(LOWORD(lp));
                int y = static_cast<short>(HIWORD(lp));
                int prev_hover = hover_btn;

                if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                    hover_btn = 1;
                } else if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                    hover_btn = 2;
                } else {
                    hover_btn = 0;
                }

                if (hover_btn != prev_hover) {
                    InvalidateRect(hwnd, nullptr, FALSE);

                    TRACKMOUSEEVENT tme{sizeof(tme)};
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                }
                break;
            }
            case WM_MOUSELEAVE: {
                if (hover_btn != 0) {
                    hover_btn = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case WM_LBUTTONDOWN: {
                int x = static_cast<short>(LOWORD(lp));
                int y = static_cast<short>(HIWORD(lp));
                if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                    pressed_btn = 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                    pressed_btn = 2;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                SetCapture(hwnd);
                break;
            }
            case WM_LBUTTONUP: {
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                int x = static_cast<short>(LOWORD(lp));
                int y = static_cast<short>(HIWORD(lp));
                if (pressed_btn == 1) {
                    if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                        state->finished = true;
                        DestroyWindow(hwnd);
                    }
                } else if (pressed_btn == 2) {
                    if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                        state->cancelled = true;
                        DestroyWindow(hwnd);
                    }
                }
                pressed_btn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);

                RECT rc;
                GetClientRect(hwnd, &rc);

                HDC mem_dc = CreateCompatibleDC(hdc);
                HBITMAP mem_bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                HGDIOBJ old_bm = SelectObject(mem_dc, mem_bm);

                HBRUSH bg_brush = CreateSolidBrush(RGB(20, 20, 23));
                FillRect(mem_dc, &rc, bg_brush);
                DeleteObject(bg_brush);

                HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(60, 64, 70));
                HGDIOBJ old_pen = SelectObject(mem_dc, border_pen);
                HGDIOBJ old_brush = SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
                Rectangle(mem_dc, 0, 0, rc.right, rc.bottom);
                SelectObject(mem_dc, old_brush);
                SelectObject(mem_dc, old_pen);
                DeleteObject(border_pen);

                if (blink_counter % 2 == 0) {
                    HBRUSH dot_brush = CreateSolidBrush(RGB(245, 34, 45));
                    HPEN dot_pen = CreatePen(PS_SOLID, 1, RGB(245, 34, 45));
                    HGDIOBJ prev_brush = SelectObject(mem_dc, dot_brush);
                    HGDIOBJ prev_pen = SelectObject(mem_dc, dot_pen);

                    Ellipse(mem_dc, 12, 21, 20, 29);

                    SelectObject(mem_dc, prev_brush);
                    SelectObject(mem_dc, prev_pen);
                    DeleteObject(dot_brush);
                    DeleteObject(dot_pen);
                }

                wchar_t text_buf[128] = L"";
                GetWindowTextW(hwnd, text_buf, 128);

                SetTextColor(mem_dc, RGB(230, 230, 230));
                SetBkMode(mem_dc, TRANSPARENT);
                HFONT font = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
                HGDIOBJ old_font = SelectObject(mem_dc, font);

                RECT text_rc{26, 0, 105, rc.bottom};
                DrawTextW(mem_dc, text_buf, -1, &text_rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

                RECT ok_rc{110, 10, 170, 40};
                COLORREF ok_bg = RGB(22, 119, 255);
                if (pressed_btn == 1) ok_bg = RGB(9, 88, 217);
                else if (hover_btn == 1) ok_bg = RGB(64, 150, 255);

                HBRUSH ok_brush = CreateSolidBrush(ok_bg);
                HPEN ok_pen = CreatePen(PS_SOLID, 1, ok_bg);
                HGDIOBJ prev_ok_pen = SelectObject(mem_dc, ok_pen);
                HGDIOBJ prev_ok_brush = SelectObject(mem_dc, ok_brush);
                RoundRect(mem_dc, ok_rc.left, ok_rc.top, ok_rc.right, ok_rc.bottom, 6, 6);
                SelectObject(mem_dc, prev_ok_brush);
                SelectObject(mem_dc, prev_ok_pen);
                DeleteObject(ok_brush);
                DeleteObject(ok_pen);

                SetTextColor(mem_dc, RGB(255, 255, 255));
                DrawTextW(mem_dc, L"完成", -1, &ok_rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

                RECT cancel_rc{175, 10, 230, 40};
                COLORREF cancel_bg = RGB(35, 38, 41);
                if (pressed_btn == 2) cancel_bg = RGB(20, 22, 24);
                else if (hover_btn == 2) cancel_bg = RGB(50, 55, 60);

                HBRUSH cancel_brush = CreateSolidBrush(cancel_bg);
                HPEN cancel_pen = CreatePen(PS_SOLID, 1, RGB(90, 95, 100));
                HGDIOBJ prev_cancel_pen = SelectObject(mem_dc, cancel_pen);
                HGDIOBJ prev_cancel_brush = SelectObject(mem_dc, cancel_brush);
                RoundRect(mem_dc, cancel_rc.left, cancel_rc.top, cancel_rc.right, cancel_rc.bottom, 6, 6);
                SelectObject(mem_dc, prev_cancel_brush);
                SelectObject(mem_dc, prev_cancel_pen);
                DeleteObject(cancel_brush);
                DeleteObject(cancel_pen);

                SetTextColor(mem_dc, RGB(200, 200, 200));
                DrawTextW(mem_dc, L"取消", -1, &cancel_rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

                SelectObject(mem_dc, old_font);
                DeleteObject(font);

                BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);

                SelectObject(mem_dc, old_bm);
                DeleteObject(mem_bm);
                DeleteDC(mem_dc);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_DESTROY: {
                KillTimer(hwnd, 1);
                break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

bool is_bitmap_static(const Bitmap& bmp1, const Bitmap& bmp2) {
    if (bmp1.empty() || bmp2.empty()) return false;
    if (bmp1.width != bmp2.width || bmp1.height != bmp2.height) return false;

    const int W = bmp1.width;
    const int H = bmp1.height;

    uint64_t diff = 0;
    const int step_y = 4;
    const int step_x = 4;
    int count = 0;

    for (int y = 0; y < H; y += step_y) {
        const auto* r1 = bmp1.row(y).data();
        const auto* r2 = bmp2.row(y).data();
        for (int x = 0; x < W; x += step_x) {
            const int idx = x * 4;
            diff += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
            diff += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
            diff += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            ++count;
        }
    }

    const double avg_diff = static_cast<double>(diff) / (count * 3.0);
    return avg_diff < 1.0;
}

int find_best_template_y(const Bitmap& frame, int direction) {
    const int W = frame.width;
    const int H = frame.height;
    const int th = 24;

    int start_y = H / 4;
    int end_y = 3 * H / 4 - th;

    const int edge_margin = std::max(40, H / 8);

    if (direction == 1) { // Downscroll (content moves up, so we want template from bottom, avoiding bottom edge)
        start_y = H / 2;
        end_y = H - th - edge_margin;
    } else if (direction == -1) { // Upscroll (content moves down, so we want template from top, avoiding top edge)
        start_y = edge_margin;
        end_y = H / 2 - th;
    }

    if (start_y > end_y) {
        start_y = H / 4;
        end_y = 3 * H / 4 - th;
    }

    int best_y = (start_y + end_y) / 2;
    uint64_t max_variance = 0;

    const int col_step = 8;

    for (int y = start_y; y <= end_y; y += 4) {
        uint64_t variance = 0;
        for (int row = 0; row < th - 1; ++row) {
            const auto* r1 = frame.row(y + row).data();
            const auto* r2 = frame.row(y + row + 1).data();
            for (int col = 0; col < W; col += col_step) {
                const int idx = col * 4;
                variance += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
                variance += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
                variance += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            }
        }
        if (variance > max_variance) {
            max_variance = variance;
            best_y = y;
        }
    }

    return best_y;
}

class OverlaySession {
public:
    explicit OverlaySession(RegionRequest request) : request_(std::move(request)) {
        custom_color_ = parse_hex_color(request_.config.custom_color, RGB(128, 0, 255));
    }

    RegionResult run() {
        monitors_ = capture_monitors();
        if (monitors_.empty() || std::ranges::any_of(monitors_, [](const auto& monitor) { return monitor.bitmap.empty(); })) {
            return {ExitCode::operation_failed, std::wstring(strings::capture_failed)};
        }
        window_candidates_ = enumerate_window_candidates();
        virtual_bounds_ = monitors_.front().bounds;
        for (const auto& monitor : monitors_) {
            virtual_bounds_.left = std::min(virtual_bounds_.left, monitor.bounds.left);
            virtual_bounds_.top = std::min(virtual_bounds_.top, monitor.bounds.top);
            virtual_bounds_.right = std::max(virtual_bounds_.right, monitor.bounds.right);
            virtual_bounds_.bottom = std::max(virtual_bounds_.bottom, monitor.bounds.bottom);
        }

        for (auto& monitor : monitors_) {
            auto window = std::make_unique<OverlayWindow>(*this, monitor);
            if (!window->create()) {
                finish({ExitCode::operation_failed, L"无法创建截图窗口。"});
                break;
            }
            windows_.push_back(std::move(window));
        }
        if (!windows_.empty() && !done_) {
            for (const auto& window : windows_) {
                ShowWindow(window->hwnd(), SW_SHOWNOACTIVATE);
            }
            SetForegroundWindow(windows_.front()->hwnd());
            SetFocus(windows_.front()->hwnd());
        }

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        for (auto& window : windows_) {
            window->destroy();
        }
        windows_.clear();
        return result_;
    }

    DragMode hit_test_drag_mode(POINT point) const {
        if (!selection_complete_) {
            return DragMode::none;
        }

        constexpr int threshold = 8;

        bool near_left = std::abs(point.x - selection_.left) <= threshold;
        bool near_right = std::abs(point.x - selection_.right) <= threshold;
        bool near_top = std::abs(point.y - selection_.top) <= threshold;
        bool near_bottom = std::abs(point.y - selection_.bottom) <= threshold;

        if (near_left && near_top) return DragMode::top_left;
        if (near_right && near_top) return DragMode::top_right;
        if (near_left && near_bottom) return DragMode::bottom_left;
        if (near_right && near_bottom) return DragMode::bottom_right;

        bool in_x_range = point.x >= selection_.left - threshold && point.x <= selection_.right + threshold;
        bool in_y_range = point.y >= selection_.top - threshold && point.y <= selection_.bottom + threshold;

        if (near_left && in_y_range) return DragMode::left;
        if (near_right && in_y_range) return DragMode::right;
        if (near_top && in_x_range) return DragMode::top;
        if (near_bottom && in_x_range) return DragMode::bottom;

        if (selection_.contains(point)) {
            if (active_tool_ == Tool::none) {
                return DragMode::move;
            }
            return DragMode::annotate;
        }

        return DragMode::none;
    }

    void on_mouse_down(HWND source, POINT point, bool right) {
        if (right) {
            finish({ExitCode::user_cancelled, L"已取消。"});
            return;
        }
        if (selection_complete_) {
            for (const auto& button : toolbar_) {
                if (button.id == L"|") continue;
                if (button.bounds.contains(point)) {
                    invoke(button.id, source);
                    return;
                }
            }
            for (const auto& button : sub_toolbar_) {
                if (button.bounds.contains(point)) {
                    invoke_sub(button.id, source);
                    invalidate_all();
                    return;
                }
            }

            DragMode mode = hit_test_drag_mode(point);
            if (mode == DragMode::none) {
                return;
            }

            if (mode == DragMode::annotate) {
                POINT relative{point.x - selection_.left, point.y - selection_.top};
                if (active_tool_ == Tool::text) {
                    if (auto text = prompt_text(source, point)) {
                        discard_redo();
                        annotations_.push_back({Tool::text, relative, relative, {}, std::move(*text), active_color_, active_width_});
                        invalidate_all();
                    }
                    return;
                }
                drawing_annotation_ = true;
                preview_ = {active_tool_, relative, relative, {}, {}, active_color_, active_width_};
                if (active_tool_ == Tool::mosaic) {
                    preview_.points.push_back(relative);
                }
                current_drag_mode_ = DragMode::annotate;
                SetCapture(source);
                return;
            }

            dragging_selection_ = true;
            current_drag_mode_ = mode;
            drag_start_ = point;
            original_selection_ = selection_;
            SetCapture(source);
            return;
        }

        dragging_selection_ = true;
        current_drag_mode_ = DragMode::none;
        drag_start_ = point;
        selection_ = {point.x, point.y, point.x, point.y};
        clicked_window_ = hover_;
        SetCapture(source);
        invalidate_all();
    }

    void on_mouse_move(POINT point) {
        cursor_pos_ = point;
        if (selection_complete_ && drawing_annotation_) {
            preview_.end = {point.x - selection_.left, point.y - selection_.top};
            if (preview_.tool == Tool::mosaic && selection_.contains(point)) {
                const POINT relative{point.x - selection_.left, point.y - selection_.top};
                if (preview_.points.empty() || std::abs(relative.x - preview_.points.back().x) > 2 ||
                    std::abs(relative.y - preview_.points.back().y) > 2) {
                    preview_.points.push_back(relative);
                }
            }
            invalidate_all();
            return;
        }
        if (dragging_selection_) {
            if (current_drag_mode_ == DragMode::none) {
                int x1 = drag_start_.x;
                int y1 = drag_start_.y;
                int x2 = snap_coordinate(point.x, true);
                int y2 = snap_coordinate(point.y, false);
                selection_ = RectI{x1, y1, x2, y2}.normalized();
            } else {
                int dx = point.x - drag_start_.x;
                int dy = point.y - drag_start_.y;

                if (current_drag_mode_ == DragMode::move) {
                    int w = original_selection_.width();
                    int h = original_selection_.height();
                    int left = original_selection_.left + dx;
                    int top = original_selection_.top + dy;

                    if (left < virtual_bounds_.left) left = virtual_bounds_.left;
                    if (left + w > virtual_bounds_.right) left = virtual_bounds_.right - w;
                    if (top < virtual_bounds_.top) top = virtual_bounds_.top;
                    if (top + h > virtual_bounds_.bottom) top = virtual_bounds_.bottom - h;

                    selection_ = {left, top, left + w, top + h};
                } else {
                    int left = selection_.left;
                    int top = selection_.top;
                    int right = selection_.right;
                    int bottom = selection_.bottom;

                    switch (current_drag_mode_) {
                        case DragMode::top_left:
                            left = snap_coordinate(original_selection_.left + dx, true);
                            top = snap_coordinate(original_selection_.top + dy, false);
                            break;
                        case DragMode::top:
                            top = snap_coordinate(original_selection_.top + dy, false);
                            break;
                        case DragMode::top_right:
                            right = snap_coordinate(original_selection_.right + dx, true);
                            top = snap_coordinate(original_selection_.top + dy, false);
                            break;
                        case DragMode::right:
                            right = snap_coordinate(original_selection_.right + dx, true);
                            break;
                        case DragMode::bottom_right:
                            right = snap_coordinate(original_selection_.right + dx, true);
                            bottom = snap_coordinate(original_selection_.bottom + dy, false);
                            break;
                        case DragMode::bottom:
                            bottom = snap_coordinate(original_selection_.bottom + dy, false);
                            break;
                        case DragMode::bottom_left:
                            left = snap_coordinate(original_selection_.left + dx, true);
                            bottom = snap_coordinate(original_selection_.bottom + dy, false);
                            break;
                        case DragMode::left:
                            left = snap_coordinate(original_selection_.left + dx, true);
                            break;
                        default:
                            break;
                    }

                    selection_ = {left, top, right, bottom};
                }
            }
            build_toolbar();
            invalidate_all();
            return;
        }
        if (selection_complete_) {
            std::wstring current_hovered;
            for (const auto& button : toolbar_) {
                if (button.id == L"|") continue;
                if (button.bounds.contains(point)) {
                    current_hovered = button.id;
                    break;
                }
            }
            if (current_hovered.empty()) {
                for (const auto& button : sub_toolbar_) {
                    if (button.bounds.contains(point)) {
                        current_hovered = button.id;
                        break;
                    }
                }
            }
            if (current_hovered != hovered_button_id_) {
                hovered_button_id_ = current_hovered;
                invalidate_all();
            }
            return;
        }
        RectI next{};
        for (const auto& candidate : window_candidates_) {
            if (candidate.bounds.contains(point)) {
                next = candidate.bounds;
                break;
            }
        }
        hover_ = next;
        invalidate_all();
    }

    void on_mouse_up(POINT point) {
        ReleaseCapture();
        if (selection_complete_ && drawing_annotation_) {
            drawing_annotation_ = false;
            if (preview_.tool == Tool::mosaic ? preview_.points.size() > 1
                                              : (preview_.start.x != preview_.end.x ||
                                                 preview_.start.y != preview_.end.y)) {
                discard_redo();
                annotations_.push_back(preview_);
            }
            preview_ = {};
            current_drag_mode_ = DragMode::none;
            invalidate_all();
            return;
        }
        if (!dragging_selection_) {
            return;
        }
        dragging_selection_ = false;

        if (current_drag_mode_ == DragMode::none) {
            const int distance = std::abs(point.x - drag_start_.x) + std::abs(point.y - drag_start_.y);
            if (distance <= 4 && !clicked_window_.empty()) {
                selection_ = clicked_window_;
            }
        }

        selection_ = selection_.normalized();
        current_drag_mode_ = DragMode::none;

        const auto clipped = intersect(selection_, virtual_bounds_);
        if (!clipped || clipped->width() < 2 || clipped->height() < 2) {
            selection_ = {};
            selection_complete_ = false;
            invalidate_all();
            return;
        }
        selection_ = *clipped;

        if (!selection_complete_) {
            selection_complete_ = true;
            build_toolbar();
            invalidate_all();

            if (request_.action == RegionAction::clipboard) {
                complete_clipboard();
            } else if (request_.action == RegionAction::file) {
                complete_file(request_.path, nullptr);
            } else if (request_.action == RegionAction::ocr) {
                complete_ocr();
            }
        } else {
            build_toolbar();
            invalidate_all();
        }
    }

    void on_double_click(POINT point) {
        if (selection_complete_ && selection_.contains(point) && !is_over_toolbar(point)) {
            complete_clipboard();
        }
    }

    void on_key_down(HWND source, WPARAM key) {
        if (key == VK_ESCAPE) {
            finish({ExitCode::user_cancelled, L"已取消。"});
            return;
        }

        // Before selection is complete (magnifier is showing): color picking mode
        if (!selection_complete_) {
            // C key: copy color under cursor to clipboard and exit
            if (key == 'C') {
                POINT cursor_pos{};
                GetCursorPos(&cursor_pos);
                COLORREF color = get_pixel_color(cursor_pos.x, cursor_pos.y);
                std::wstring color_str;
                if (color_format_hex_) {
                    color_str = format_hex_color(color);
                } else {
                    color_str = std::format(L"rgb({}, {}, {})", GetRValue(color), GetGValue(color), GetBValue(color));
                }
                (void)copy_text_to_clipboard(color_str);

                custom_color_ = color;
                request_.config.custom_color = format_hex_color(color);

                finish({ExitCode::success, std::format(L"已复制颜色 {} 到剪贴板。", color_str)});
                return;
            }
            // Shift key: toggle between Hex and RGB display format in magnifier
            if (key == VK_SHIFT) {
                color_format_hex_ = !color_format_hex_;
                invalidate_all();
                return;
            }
            return;
        }

        // After selection is complete: normal editing shortcuts
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == 'C') {
            complete_clipboard();
            return;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Z') {
            undo();
            return;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Y') {
            redo();
            return;
        }
        if (request_.config.ocr_enabled && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && key == L'C') {
            complete_ocr();
            return;
        }
        if (key == VK_RETURN) {
            complete_clipboard();
            return;
        }
        if (key == L'S' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            complete_file({}, source);
        }
    }

    void invalidate_all() const {
        for (const auto& window : windows_) {
            window->invalidate();
        }
    }

    [[nodiscard]] RectI display_selection() const {
        if (!selection_.empty()) {
            return selection_;
        }
        return hover_;
    }

    [[nodiscard]] const RectI& selection() const noexcept { return selection_; }
    [[nodiscard]] bool selection_complete() const noexcept { return selection_complete_; }
    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return annotations_; }
    [[nodiscard]] const Annotation* preview() const noexcept { return drawing_annotation_ ? &preview_ : nullptr; }
    [[nodiscard]] const std::vector<ToolbarButton>& toolbar() const noexcept { return toolbar_; }
    [[nodiscard]] const std::vector<ToolbarButton>& sub_toolbar() const noexcept { return sub_toolbar_; }
    [[nodiscard]] COLORREF active_color() const noexcept { return active_color_; }
    [[nodiscard]] COLORREF custom_color() const noexcept { return custom_color_; }
    [[nodiscard]] float active_width() const noexcept { return active_width_; }
    [[nodiscard]] Tool active_tool() const noexcept { return active_tool_; }
    [[nodiscard]] std::wstring hovered_button_id() const noexcept { return hovered_button_id_; }
    [[nodiscard]] bool dragging_selection() const noexcept { return dragging_selection_; }
    [[nodiscard]] POINT cursor_pos() const noexcept { return cursor_pos_; }
    [[nodiscard]] bool color_format_hex() const noexcept { return color_format_hex_; }
    [[nodiscard]] bool is_over_toolbar(POINT point) const noexcept {
        for (const auto& button : toolbar_) {
            if (button.bounds.contains(point)) return true;
        }
        for (const auto& button : sub_toolbar_) {
            if (button.bounds.contains(point)) return true;
        }
        return false;
    }
    [[nodiscard]] COLORREF get_pixel_color(int x, int y) const noexcept {
        for (const auto& monitor : monitors_) {
            if (monitor.bounds.contains({x, y})) {
                int local_x = x - monitor.bounds.left;
                int local_y = y - monitor.bounds.top;
                if (local_x >= 0 && local_x < monitor.bitmap.width &&
                    local_y >= 0 && local_y < monitor.bitmap.height) {
                    const std::size_t index = static_cast<std::size_t>(local_y * monitor.bitmap.width + local_x) * 4;
                    if (index + 2 < monitor.bitmap.pixels.size()) {
                        uint8_t b = monitor.bitmap.pixels[index];
                        uint8_t g = monitor.bitmap.pixels[index + 1];
                        uint8_t r = monitor.bitmap.pixels[index + 2];
                        return RGB(r, g, b);
                    }
                }
            }
        }
        return RGB(0, 0, 0);
    }
    [[nodiscard]] int snap_coordinate(int value, bool is_x, int threshold = 8) const noexcept {
        int best_snap = value;
        int min_diff = threshold + 1;
        for (const auto& monitor : monitors_) {
            if (is_x) {
                if (std::abs(value - monitor.bounds.left) < min_diff) {
                    min_diff = std::abs(value - monitor.bounds.left);
                    best_snap = monitor.bounds.left;
                }
                if (std::abs(value - monitor.bounds.right) < min_diff) {
                    min_diff = std::abs(value - monitor.bounds.right);
                    best_snap = monitor.bounds.right;
                }
            } else {
                if (std::abs(value - monitor.bounds.top) < min_diff) {
                    min_diff = std::abs(value - monitor.bounds.top);
                    best_snap = monitor.bounds.top;
                }
                if (std::abs(value - monitor.bounds.bottom) < min_diff) {
                    min_diff = std::abs(value - monitor.bounds.bottom);
                    best_snap = monitor.bounds.bottom;
                }
            }
        }
        for (const auto& candidate : window_candidates_) {
            if (is_x) {
                if (std::abs(value - candidate.bounds.left) < min_diff) {
                    min_diff = std::abs(value - candidate.bounds.left);
                    best_snap = candidate.bounds.left;
                }
                if (std::abs(value - candidate.bounds.right) < min_diff) {
                    min_diff = std::abs(value - candidate.bounds.right);
                    best_snap = candidate.bounds.right;
                }
            } else {
                if (std::abs(value - candidate.bounds.top) < min_diff) {
                    min_diff = std::abs(value - candidate.bounds.top);
                    best_snap = candidate.bounds.top;
                }
                if (std::abs(value - candidate.bounds.bottom) < min_diff) {
                    min_diff = std::abs(value - candidate.bounds.bottom);
                    best_snap = candidate.bounds.bottom;
                }
            }
        }
        return best_snap;
    }

private:
    void build_toolbar() {
        toolbar_.clear();
        std::vector<std::pair<std::wstring, std::wstring>> items;
        if (request_.config.annotation_enabled) {
            items.push_back({L"rect", std::wstring(strings::toolbar_rectangle)});
            items.push_back({L"arrow", std::wstring(strings::toolbar_arrow)});
            items.push_back({L"mosaic", std::wstring(strings::toolbar_mosaic)});
            items.push_back({L"text", std::wstring(strings::toolbar_text)});
            bool show_undo = !annotations_.empty();
            bool show_redo = !redo_.empty();
            if (show_undo || show_redo) {
                items.push_back({L"|", L""});
                if (show_undo) {
                    items.push_back({L"undo", std::wstring(strings::toolbar_undo)});
                }
                if (show_redo) {
                    items.push_back({L"redo", std::wstring(strings::toolbar_redo)});
                }
            }
            items.push_back({L"|", L""});
        }
        if (request_.config.ocr_enabled) {
            items.push_back({L"ocr", std::wstring(strings::toolbar_ocr)});
        }
        items.push_back({L"scroll", L"长"});
        items.push_back({L"pin", L"钉"});
        items.push_back({L"|", L""});
        items.push_back({L"copy", std::wstring(strings::toolbar_copy)});
        items.push_back({L"save", std::wstring(strings::toolbar_save)});
        items.push_back({L"close", std::wstring(strings::toolbar_close)});

        constexpr int button_width = 36;
        constexpr int button_height = 32;
        constexpr int spacing = 4;
        constexpr int padding = 6;
        constexpr int toolbar_height = button_height + 2 * padding;

        int total_width = 2 * padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            int w = (items[index].first == L"|") ? 12 : button_width;
            total_width += w;
            if (index > 0) {
                total_width += spacing;
            }
        }

        int left = std::min(selection_.right - total_width, virtual_bounds_.right - total_width);
        left = std::max(left, virtual_bounds_.left);
        int top = selection_.bottom + 6;
        if (top + toolbar_height > virtual_bounds_.bottom) {
            top = selection_.top - toolbar_height - 6;
        }
        top = std::max(virtual_bounds_.top, top);

        int current_x = left + padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            int w = (items[index].first == L"|") ? 12 : button_width;
            int y = top + padding;
            toolbar_.push_back({items[index].first, items[index].second, {current_x, y, current_x + w, y + button_height}});
            current_x += w + spacing;
        }
        build_sub_toolbar();
    }

    void build_sub_toolbar() {
        sub_toolbar_.clear();
        if (active_tool_ != Tool::rectangle && active_tool_ != Tool::arrow && active_tool_ != Tool::text) {
            return;
        }
        std::vector<std::pair<std::wstring, std::wstring>> items = {
            {L"color_red", L"红"},
            {L"color_green", L"绿"},
            {L"color_blue", L"蓝"},
            {L"color_yellow", L"黄"},
            {L"color_black", L"黑"},
            {L"color_white", L"白"},
            {L"width_small", L"细"},
            {L"width_medium", L"中"},
            {L"width_large", L"粗"}
        };

        constexpr int button_width = 32;
        constexpr int button_height = 30;
        constexpr int spacing = 4;
        constexpr int padding = 6;
        constexpr int sub_toolbar_height = button_height + 2 * padding;

        if (toolbar_.empty()) return;
        int left = toolbar_.front().bounds.left - padding;

        bool main_above = (toolbar_.front().bounds.top < selection_.top);
        int top = 0;
        if (main_above) {
            top = toolbar_.front().bounds.top - padding - sub_toolbar_height - 6;
        } else {
            top = toolbar_.front().bounds.bottom + padding + 6;
        }

        for (std::size_t i = 0; i < items.size(); ++i) {
            int x = left + padding + static_cast<int>(i) * (button_width + spacing);
            int y = top + padding;
            sub_toolbar_.push_back({items[i].first, items[i].second, {x, y, x + button_width, y + button_height}});
        }
    }

    void invoke_sub(std::wstring_view id, HWND source) {
        if (id == L"color_red") {
            active_color_ = RGB(245, 34, 45);
        } else if (id == L"color_green") {
            active_color_ = RGB(82, 196, 26);
        } else if (id == L"color_blue") {
            active_color_ = RGB(22, 119, 255);
        } else if (id == L"color_yellow") {
            active_color_ = RGB(250, 219, 20);
        } else if (id == L"color_black") {
            active_color_ = RGB(0, 0, 0);
        } else if (id == L"color_white") {
            active_color_ = RGB(255, 255, 255);
        } else if (id == L"color_custom") {
            RectI button_bounds{};
            for (const auto& btn : sub_toolbar_) {
                if (btn.id == L"color_custom") {
                    button_bounds = btn.bounds;
                    break;
                }
            }
            show_rgb_picker_popup(source, custom_color_, button_bounds, virtual_bounds_, [this](COLORREF new_color) {
                custom_color_ = new_color;
                active_color_ = new_color;
                request_.config.custom_color = format_hex_color(new_color);
                invalidate_all();
            });
        } else if (id == L"width_small") {
            active_width_ = 2.0F;
        } else if (id == L"width_medium") {
            active_width_ = 4.0F;
        } else if (id == L"width_large") {
            active_width_ = 8.0F;
        }
    }

    void invoke(std::wstring_view id, HWND source) {
        if (id == L"rect") {
            active_tool_ = Tool::rectangle;
        } else if (id == L"arrow") {
            active_tool_ = Tool::arrow;
        } else if (id == L"mosaic") {
            active_tool_ = Tool::mosaic;
        } else if (id == L"text") {
            active_tool_ = Tool::text;
        } else if (id == L"undo") {
            undo();
        } else if (id == L"redo") {
            redo();
        } else if (id == L"ocr") {
            complete_ocr();
        } else if (id == L"copy") {
            complete_clipboard();
        } else if (id == L"save") {
            complete_file({}, source);
        } else if (id == L"scroll") {
            complete_scroll(source);
        } else if (id == L"pin") {
            complete_pin();
        } else if (id == L"close") {
            finish({ExitCode::user_cancelled, L"已取消。"});
        }
        build_sub_toolbar();
        invalidate_all();
    }

    void undo() {
        if (!annotations_.empty()) {
            redo_.push_back(std::move(annotations_.back()));
            annotations_.pop_back();
            invalidate_all();
        }
    }

    void redo() {
        if (!redo_.empty()) {
            annotations_.push_back(std::move(redo_.back()));
            redo_.pop_back();
            invalidate_all();
        }
    }

    void discard_redo() {
        redo_.clear();
    }

    Bitmap original_selection() const {
        return compose_selection(monitors_, selection_);
    }

    Bitmap rendered_selection() const {
        Bitmap result = original_selection();
        for (const auto& annotation : annotations_) {
            if (annotation.tool == Tool::mosaic) {
                for (const POINT point : annotation.points) {
                    pixelate_circle(result, point, 14, 8);
                }
            }
        }

        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = result.width;
        info.bmiHeader.biHeight = -result.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous_bitmap = SelectObject(dc, dib);
        std::memcpy(bits, result.pixels.data(), result.pixels.size());
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        HFONT font = CreateFontW(22,
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
                                 DEFAULT_PITCH,
                                 L"Microsoft YaHei");
        HGDIOBJ previous_font = SelectObject(dc, font);
        for (const auto& annotation : annotations_) {
            HPEN pen = CreatePen(PS_SOLID, static_cast<int>(annotation.width), annotation.color);
            HGDIOBJ previous_pen = SelectObject(dc, pen);
            SetTextColor(dc, annotation.color);

            if (annotation.tool == Tool::rectangle) {
                Rectangle(dc, annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y);
            } else if (annotation.tool == Tool::arrow) {
                MoveToEx(dc, annotation.start.x, annotation.start.y, nullptr);
                LineTo(dc, annotation.end.x, annotation.end.y);
                const double angle = std::atan2(
                    static_cast<double>(annotation.end.y - annotation.start.y),
                    static_cast<double>(annotation.end.x - annotation.start.x));
                const double length = 10.0 + annotation.width * 2.0;
                for (double offset : {0.45, -0.45}) {
                    MoveToEx(dc, annotation.end.x, annotation.end.y, nullptr);
                    LineTo(dc,
                           annotation.end.x - static_cast<int>(std::cos(angle + offset) * length),
                           annotation.end.y - static_cast<int>(std::sin(angle + offset) * length));
                }
            } else if (annotation.tool == Tool::text) {
                TextOutW(dc,
                         annotation.start.x,
                         annotation.start.y,
                         annotation.text.c_str(),
                         static_cast<int>(annotation.text.size()));
            }
            SelectObject(dc, previous_pen);
            DeleteObject(pen);
        }
        std::memcpy(result.pixels.data(), bits, result.pixels.size());
        SelectObject(dc, previous_font);
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_bitmap);
        DeleteObject(font);
        DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return result;
    }

    void complete_clipboard() {
        std::wstring error;
        if (!copy_bitmap_to_clipboard(rendered_selection(), &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        finish({ExitCode::success, L"截图已复制到剪贴板。"});
    }

    void complete_file(std::wstring_view requested_path, HWND owner) {
        std::optional<std::filesystem::path> path;
        if (requested_path.empty() && request_.action == RegionAction::interactive) {
            path = prompt_png_path(owner);
            if (!path) {
                return;
            }
        } else {
            path = resolve_output_path(requested_path);
        }
        std::wstring error;
        if (!save_png(rendered_selection(), *path, &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        RegionResult result{ExitCode::success, L"截图已保存。"};
        result.path = path->wstring();
        finish(std::move(result));
    }

    void complete_ocr() {
        if (!request_.config.ocr_enabled) {
            finish({ExitCode::module_unavailable, L"OCR 模块已关闭。"});
            return;
        }
        const OcrOutput output = recognize_text(original_selection());
        if (!output.ok) {
            finish({ExitCode::operation_failed, output.error});
            return;
        }
        if (request_.copy_ocr) {
            std::wstring error;
            if (!copy_text_to_clipboard(output.text, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }
        }
        RegionResult result{ExitCode::success, request_.copy_ocr ? std::wstring(strings::ocr_success) : L"OCR 完成。"};
        result.text = output.text;
        finish(std::move(result));
    }

    void complete_pin() {
        RegionResult result{ExitCode::success, L"贴图已创建。"};
        result.action = RegionAction::pin;
        result.bitmap = rendered_selection();
        finish(std::move(result));
    }

    struct ScrollResult {
        bool matched{false};
        int direction{0}; // 1 = down (append), -1 = up (prepend), 0 = none/invalid
        int offset{0};
    };

    ScrollResult detect_scroll(const Bitmap& last_frame, const Bitmap& new_frame, int locked_direction) {
        if (last_frame.empty() || new_frame.empty()) return {};
        if (last_frame.width != new_frame.width || last_frame.height != new_frame.height) return {};

        const int W = last_frame.width;
        const int H = last_frame.height;
        const int th = 24;

        const int template_y = find_best_template_y(last_frame, locked_direction);
        const int col_step = 4;

        uint64_t variance = 0;
        {
            for (int row = 0; row < th - 1; ++row) {
                const auto* r1 = last_frame.row(template_y + row).data();
                const auto* r2 = last_frame.row(template_y + row + 1).data();
                for (int col = 0; col < W; col += col_step) {
                    const int idx = col * 4;
                    variance += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
                    variance += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
                    variance += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
                }
            }
        }
        const double min_required_variance = (W / col_step) * th * 0.5;
        if (variance < min_required_variance) {
            return {false, 0, 0};
        }

        auto search_range = [&](int start_y, int end_y, int& out_best_y, double& out_avg_diff) -> bool {
            int best_y = -1;
            uint64_t min_sad = 0xFFFFFFFFFFFFFFFFULL;
            for (int y = start_y; y <= end_y; ++y) {
                uint64_t sad = 0;
                for (int row = 0; row < th; ++row) {
                    const auto* last_row = last_frame.row(template_y + row).data();
                    const auto* new_row = new_frame.row(y + row).data();
                    for (int col = 0; col < W; col += col_step) {
                        const int idx = col * 4;
                        sad += std::abs(static_cast<int>(last_row[idx]) - static_cast<int>(new_row[idx]));
                        sad += std::abs(static_cast<int>(last_row[idx + 1]) - static_cast<int>(new_row[idx + 1]));
                        sad += std::abs(static_cast<int>(last_row[idx + 2]) - static_cast<int>(new_row[idx + 2]));
                    }
                }
                if (sad < min_sad) {
                    min_sad = sad;
                    best_y = y;
                }
            }
            if (best_y == -1) return false;
            const double num_pixels = th * (W / col_step);
            out_avg_diff = static_cast<double>(min_sad) / (num_pixels * 3.0);
            out_best_y = best_y;
            return true;
        };

        const int max_disp = std::min(240, H / 2);
        int start_narrow = std::max(0, template_y - max_disp);
        int end_narrow = std::min(H - th, template_y + max_disp);

        int best_y = -1;
        double avg_diff = 100.0;

        // Stage 1: Narrow search
        if (search_range(start_narrow, end_narrow, best_y, avg_diff)) {
            if (avg_diff < 15.0) {
                const int diff = template_y - best_y;
                if (std::abs(diff) >= 2) {
                    if (diff > 0 && diff < H - 20) {
                        return {true, 1, diff};
                    } else if (diff < 0 && -diff < H - 20) {
                        return {true, -1, -diff};
                    }
                } else {
                    return {true, 0, 0}; // matched but no significant movement
                }
            }
        }

        // Stage 2: Fallback full search
        if (search_range(0, H - th, best_y, avg_diff)) {
            if (avg_diff < 15.0) {
                const int diff = template_y - best_y;
                if (std::abs(diff) >= 2) {
                    if (diff > 0 && diff < H - 20) {
                        return {true, 1, diff};
                    } else if (diff < 0 && -diff < H - 20) {
                        return {true, -1, -diff};
                    }
                } else {
                    return {true, 0, 0}; // matched but no significant movement
                }
            }
        }

        return {false, 0, 0};
    }

    void append_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
        if (new_frame.empty() || d <= 0) return;

        const int W = new_frame.width;
        const int H = new_frame.height;
        const int old_h = stitched.height;
        const int new_h = old_h + d;

        stitched.pixels.resize(static_cast<std::size_t>(W) * new_h * 4U);
        stitched.height = new_h;

        for (int row = 0; row < d; ++row) {
            const int src_y = H - d + row;
            const int dest_y = old_h + row;
            const auto* src_row = new_frame.row(src_y).data();
            auto* dest_row = stitched.row(dest_y).data();
            std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
        }
    }

    void prepend_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
        if (new_frame.empty() || d <= 0) return;

        const int W = new_frame.width;
        const int old_h = stitched.height;
        const int new_h = old_h + d;

        stitched.pixels.resize(static_cast<std::size_t>(W) * new_h * 4U);
        stitched.height = new_h;

        std::memmove(
            stitched.pixels.data() + static_cast<std::size_t>(W) * d * 4U,
            stitched.pixels.data(),
            static_cast<std::size_t>(W) * old_h * 4U
        );

        for (int row = 0; row < d; ++row) {
            const auto* src_row = new_frame.row(row).data();
            auto* dest_row = stitched.row(row).data();
            std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
        }
    }

    void complete_scroll(HWND source) {
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
        run_scroll_capture(source);
    }

    void run_scroll_capture(HWND source) {
        Bitmap stitched = compose_selection(monitors_, selection_);
        if (stitched.empty()) {
            finish({ExitCode::operation_failed, L"长截图初始化失败。"});
            return;
        }
        Bitmap last_stitched_frame = stitched;
        Bitmap last_cap = stitched;

        HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND border_wnd = ScrollBorderWindow::create(instance, source, selection_);
        ScrollControlState control_state;
        HWND control_wnd = ScrollControlWindow::create(instance, source, selection_, &control_state);
        if (control_wnd) {
            std::wstring progress = std::format(L"{} px", stitched.height);
            SetWindowTextW(control_wnd, progress.c_str());
        }

        UINT_PTR timer_id = SetTimer(source, 999, 80, nullptr);
        int locked_direction = 0; // 0 = undecided, 1 = down, -1 = up
        int consecutive_failures = 0;

        MSG msg{};
        while (IsWindow(control_wnd) && !control_state.finished && !control_state.cancelled) {
            if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
                control_state.finished = true;
                break;
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                control_state.cancelled = true;
                break;
            }
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_TIMER && msg.wParam == 999) {
                    Bitmap new_frame = capture_rect(selection_);
                    if (!new_frame.empty()) {
                        ScrollResult res = detect_scroll(last_stitched_frame, new_frame, locked_direction);
                        if (res.matched) {
                            consecutive_failures = 0;
                            if (res.direction != 0 && res.offset > 0) {
                                if (locked_direction == 0) {
                                    locked_direction = res.direction;
                                }
                                if (res.direction == locked_direction) {
                                    if (locked_direction == 1) {
                                        append_to_stitched(stitched, new_frame, res.offset);
                                    } else {
                                        prepend_to_stitched(stitched, new_frame, res.offset);
                                    }
                                    last_stitched_frame = new_frame;
                                }
                            }
                        } else {
                            if (locked_direction != 0) {
                                consecutive_failures++;
                                if (consecutive_failures >= 4) {
                                    // Lost matching anchor. Re-anchor to current frame.
                                    last_stitched_frame = new_frame;
                                    consecutive_failures = 0;
                                }
                            }
                        }

                        std::wstring progress = std::format(L"{} px", stitched.height);
                        SetWindowTextW(control_wnd, progress.c_str());
                        last_cap = new_frame;
                    }
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } else {
                Sleep(10);
            }
        }

        KillTimer(source, timer_id);
        if (border_wnd) DestroyWindow(border_wnd);
        if (control_wnd) DestroyWindow(control_wnd);

        if (control_state.cancelled) {
            finish({ExitCode::user_cancelled, L"长截图已取消。"});
            return;
        }

        std::wstring error;
        bool saved = false;
        std::wstring path_msg;
        if (request_.action == RegionAction::file || request_.config.default_output == L"file") {
            auto path = prompt_png_path(source);
            if (path) {
                if (save_png(stitched, *path, &error)) {
                    saved = true;
                    path_msg = path->wstring();
                }
            }
        }
        if (!saved) {
            if (!copy_bitmap_to_clipboard(stitched, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }
        }

        RegionResult result{ExitCode::success, saved ? L"长截图已保存。" : L"长截图已复制到剪贴板。"};
        if (saved) {
            result.path = path_msg;
        }
        result.bitmap = stitched;
        finish(std::move(result));
    }

    void finish(RegionResult result) {
        if (done_) {
            return;
        }
        result_ = std::move(result);
        if (result_.bounds.empty()) {
            result_.bounds = selection_;
        }
        if (result_.action == RegionAction::interactive) {
            result_.action = request_.action;
        }
        result_.config = request_.config;
        done_ = true;
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
    }

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
            float tb_left = static_cast<float>(session_.toolbar().front().bounds.left - 6 - monitor_.bounds.left);
            float tb_top = static_cast<float>(session_.toolbar().front().bounds.top - 6 - monitor_.bounds.top);
            float tb_right = static_cast<float>(session_.toolbar().back().bounds.right + 6 - monitor_.bounds.left);
            float tb_bottom = static_cast<float>(session_.toolbar().front().bounds.bottom + 6 - monitor_.bounds.top);

            D2D1_RECT_F bg_rect = D2D1::RectF(tb_left, tb_top, tb_right, tb_bottom);
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

                bool is_active = (session_.active_tool() != Tool::none &&
                                 ((button.id == L"rect" && session_.active_tool() == Tool::rectangle) ||
                                  (button.id == L"arrow" && session_.active_tool() == Tool::arrow) ||
                                  (button.id == L"mosaic" && session_.active_tool() == Tool::mosaic) ||
                                  (button.id == L"text" && session_.active_tool() == Tool::text)));

                bool is_hovered = (session_.hovered_button_id() == button.id);

                if (is_active) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), active_bg_brush_.Get());
                } else if (is_hovered) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.f, 4.f), hover_bg_brush_.Get());
                }

                const float cx = bounds.left + (bounds.right - bounds.left) / 2.0F;
                const float cy = bounds.top + (bounds.bottom - bounds.top) / 2.0F;

                if (button.id == L"rect") {
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 7.5F, cy - 6.5F, cx + 7.5F, cy + 6.5F), 1.5F, 1.5F), white_brush_.Get(), 1.5F);
                } else if (button.id == L"arrow") {
                    render_target_->DrawLine(D2D1::Point2F(cx - 4.0F, cy + 4.0F), D2D1::Point2F(cx + 5.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy - 5.0F), D2D1::Point2F(cx - 1.0F, cy - 5.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawLine(D2D1::Point2F(cx + 5.0F, cy - 5.0F), D2D1::Point2F(cx + 5.0F, cy - 1.0F), white_brush_.Get(), 1.5F);
                    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 6.0F, cy + 6.0F), 1.8F, 1.8F), white_brush_.Get(), 1.5F);
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
            float sub_left = static_cast<float>(session_.sub_toolbar().front().bounds.left - 6 - monitor_.bounds.left);
            float sub_top = static_cast<float>(session_.sub_toolbar().front().bounds.top - 6 - monitor_.bounds.top);
            float sub_right = static_cast<float>(session_.sub_toolbar().back().bounds.right + 6 - monitor_.bounds.left);
            float sub_bottom = static_cast<float>(session_.sub_toolbar().front().bounds.bottom + 6 - monitor_.bounds.top);

            D2D1_RECT_F sub_bg_rect = D2D1::RectF(sub_left, sub_top, sub_right, sub_bottom);
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(sub_bg_rect, 6.f, 6.f), toolbar_bg_brush_.Get());
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(sub_bg_rect, 6.f, 6.f), toolbar_border_brush_.Get(), 1.f);

            for (const auto& button : session_.sub_toolbar()) {
                if (!intersect(button.bounds, monitor_.bounds)) {
                    continue;
                }
                const D2D1_RECT_F bounds = local_rect(button.bounds);
                bool is_selected = false;
                if (button.id == L"width_small" && session_.active_width() == 2.0F) is_selected = true;
                else if (button.id == L"width_medium" && session_.active_width() == 4.0F) is_selected = true;
                else if (button.id == L"width_large" && session_.active_width() == 8.0F) is_selected = true;
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

                if (button.id.starts_with(L"width_") && is_selected) {
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

}  // namespace

RegionResult run_region_capture(const RegionRequest& request) {
    OverlaySession session(request);
    RegionResult result = session.run();
    release_overlay_factories();
    return result;
}

}  // namespace airshot
