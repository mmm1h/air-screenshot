#include "overlay_helpers.h"

#include <commctrl.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <string>

namespace airshot::overlay_detail {

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



}  // namespace airshot::overlay_detail
