#include "overlay_helpers.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <mutex>

namespace airshot::overlay_detail {

namespace {

constexpr int kScrollControlWidthDip = 328;
constexpr int kScrollControlHeightDip = 52;

struct ScrollControlLayout {
    RECT status{};
    std::array<RECT, 3> buttons{};
    int width{};
    int height{};
    int radius{};
};

[[nodiscard]] UINT window_dpi(HWND window) noexcept {
    if (window && IsWindow(window)) {
        const UINT dpi = GetDpiForWindow(window);
        if (dpi != 0) {
            return dpi;
        }
    }
    return USER_DEFAULT_SCREEN_DPI;
}

[[nodiscard]] int scale_dip(UINT dpi, int value) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] ScrollControlLayout scroll_control_layout(HWND window) noexcept {
    const UINT dpi = window_dpi(window);
    const auto s = [dpi](int value) { return scale_dip(dpi, value); };
    ScrollControlLayout layout;
    layout.width = s(kScrollControlWidthDip);
    layout.height = s(kScrollControlHeightDip);
    layout.radius = std::max(2, s(7));
    layout.status = {s(27), 0, s(104), layout.height};
    layout.buttons = {
        RECT{s(108), s(10), s(174), s(42)},
        RECT{s(180), s(10), s(246), s(42)},
        RECT{s(252), s(10), s(318), s(42)},
    };
    return layout;
}

[[nodiscard]] int scroll_button_at(HWND window, POINT point) noexcept {
    const ScrollControlLayout layout = scroll_control_layout(window);
    for (std::size_t index = 0; index < layout.buttons.size(); ++index) {
        if (PtInRect(&layout.buttons[index], point)) {
            return static_cast<int>(index) + 1;
        }
    }
    return 0;
}

[[nodiscard]] bool pause_button_enabled(
    const ScrollControlState& state) noexcept {
    return !state.paused || state.can_resume;
}

}  // namespace

LRESULT CALLBACK scroll_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

HWND create_scroll_border_window(HINSTANCE instance, HWND parent, const RectI& bounds) {
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
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 102, 255));
                    HGDIOBJ old_pen = SelectObject(hdc, pen);
                    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                    SelectObject(hdc, old_brush);
                    SelectObject(hdc, old_pen);
                    DeleteObject(pen);

                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefSubclassProc(hwnd, msg, wp, lp);
            }, 0, 0);
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return hwnd;
    }

HWND create_scroll_control_window(HINSTANCE instance, HWND parent, const RectI& selection, ScrollControlState* state) {
        static std::once_flag class_flag;
        std::call_once(class_flag, [instance] {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = scroll_control_proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(RGB(17, 19, 22));
            wc.lpszClassName = L"AirScreenshot.ScrollControl";
            RegisterClassExW(&wc);
        });

        const UINT dpi = window_dpi(parent);
        const int w = scale_dip(dpi, kScrollControlWidthDip);
        const int h = scale_dip(dpi, kScrollControlHeightDip);
        const int outer_gap = scale_dip(dpi, 8);
        const int work_margin = scale_dip(dpi, 10);
        int x = selection.right - w;
        int y = selection.bottom + outer_gap;

        RECT work_area{};
        MONITORINFO monitor_info{sizeof(monitor_info)};
        RECT selection_rect = selection.native();
        if (const HMONITOR monitor = MonitorFromRect(&selection_rect, MONITOR_DEFAULTTONEAREST);
            monitor && GetMonitorInfoW(monitor, &monitor_info)) {
            work_area = monitor_info.rcWork;
        } else {
            work_area = {
                GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        }
        const int work_left = static_cast<int>(work_area.left);
        const int work_top = static_cast<int>(work_area.top);
        const int work_right = static_cast<int>(work_area.right);
        const int work_bottom = static_cast<int>(work_area.bottom);
        x = std::clamp(
            x,
            work_left + work_margin,
            std::max(work_left + work_margin, work_right - w - work_margin));
        if (y + h > work_bottom) y = selection.top - h - outer_gap;
        y = std::clamp(
            y,
            work_top + work_margin,
            std::max(work_top + work_margin, work_bottom - h - work_margin));

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hwnd);
        }
        return hwnd;
    }

LRESULT CALLBACK scroll_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* state = reinterpret_cast<ScrollControlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = static_cast<ScrollControlState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

        auto complete = [state](bool cancelled) {
            if (state->finished || state->cancelled) {
                return;
            }
            state->finished = !cancelled;
            state->cancelled = cancelled;
            auto callback = cancelled ? state->on_cancel : state->on_finish;
            if (callback) {
                callback();
            }
        };

        switch (msg) {
            case WM_CREATE: {
                state->hover_button = 0;
                state->pressed_button = 0;
                state->blink_counter = 0;
                if (!SetTimer(hwnd, kScrollBlinkTimer, 500, nullptr)) {
                    return -1;
                }
                if (!SetTimer(hwnd, kScrollCaptureTimer, 80, nullptr)) {
                    KillTimer(hwnd, kScrollBlinkTimer);
                    return -1;
                }
                return 0;
            }
            case WM_TIMER: {
                if (wp == kScrollBlinkTimer) {
                    ++state->blink_counter;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (wp == kScrollCaptureTimer &&
                           !state->paused && state->on_tick) {
                    auto callback = state->on_tick;
                    callback();
                }
                return 0;
            }
            case WM_MOUSEACTIVATE:
                // The user must keep scrolling the captured application after
                // clicking this palette. Never steal its foreground focus.
                return MA_NOACTIVATE;
            case WM_SETTEXT: {
                LRESULT res = DefWindowProcW(hwnd, msg, wp, lp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return res;
            }
            case WM_MOUSEMOVE: {
                const int previous_hover = state->hover_button;
                const POINT point{
                    static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
                state->hover_button = scroll_button_at(hwnd, point);
                if (state->hover_button == 1 &&
                    !pause_button_enabled(*state)) {
                    state->hover_button = 0;
                }

                if (state->hover_button != previous_hover) {
                    InvalidateRect(hwnd, nullptr, FALSE);

                    TRACKMOUSEEVENT tme{sizeof(tme)};
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                }
                return 0;
            }
            case WM_MOUSELEAVE: {
                if (state->hover_button != 0) {
                    state->hover_button = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONDOWN: {
                const POINT point{
                    static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
                state->pressed_button = scroll_button_at(hwnd, point);
                if (state->pressed_button == 1 &&
                    !pause_button_enabled(*state)) {
                    state->pressed_button = 0;
                }
                if (state->pressed_button != 0) {
                    SetCapture(hwnd);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_LBUTTONUP: {
                const int pressed_button = state->pressed_button;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                const POINT point{
                    static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
                state->pressed_button = 0;
                const int released_button = scroll_button_at(hwnd, point);
                if (pressed_button == released_button &&
                    pressed_button == 1 &&
                    pause_button_enabled(*state)) {
                    auto callback = state->on_toggle_pause;
                    if (callback) {
                        callback();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (pressed_button == released_button &&
                    pressed_button == 2) {
                    complete(false);
                    return 0;
                }
                if (pressed_button == released_button &&
                    pressed_button == 3) {
                    complete(true);
                    return 0;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_CAPTURECHANGED: {
                state->pressed_button = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);

                RECT rc;
                GetClientRect(hwnd, &rc);

                HDC mem_dc = CreateCompatibleDC(hdc);
                HBITMAP mem_bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                if (!mem_dc || !mem_bm) {
                    if (mem_bm) DeleteObject(mem_bm);
                    if (mem_dc) DeleteDC(mem_dc);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                HGDIOBJ old_bm = SelectObject(mem_dc, mem_bm);

                const ScrollControlLayout layout =
                    scroll_control_layout(hwnd);
                const UINT dpi = window_dpi(hwnd);
                HIGHCONTRASTW high_contrast_info{
                    sizeof(high_contrast_info)};
                const bool high_contrast =
                    SystemParametersInfoW(
                        SPI_GETHIGHCONTRAST,
                        sizeof(high_contrast_info),
                        &high_contrast_info,
                        0) &&
                    (high_contrast_info.dwFlags & HCF_HIGHCONTRASTON) != 0;
                const COLORREF background = high_contrast
                                                ? GetSysColor(COLOR_WINDOW)
                                                : RGB(20, 23, 29);
                const COLORREF foreground = high_contrast
                                                ? GetSysColor(COLOR_WINDOWTEXT)
                                                : RGB(236, 240, 247);
                const COLORREF border = high_contrast
                                            ? GetSysColor(COLOR_WINDOWTEXT)
                                            : RGB(55, 63, 76);

                HBRUSH bg_brush = CreateSolidBrush(background);
                FillRect(mem_dc, &rc, bg_brush);
                DeleteObject(bg_brush);

                HPEN border_pen = CreatePen(PS_SOLID, 1, border);
                HGDIOBJ old_pen = SelectObject(mem_dc, border_pen);
                HGDIOBJ old_brush = SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
                Rectangle(mem_dc, 0, 0, rc.right, rc.bottom);
                SelectObject(mem_dc, old_brush);
                SelectObject(mem_dc, old_pen);
                DeleteObject(border_pen);

                if (state->paused || state->blink_counter % 2 == 0) {
                    const COLORREF dot_color =
                        state->paused
                            ? (high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHT)
                                   : RGB(242, 174, 68))
                            : (high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHT)
                                   : RGB(66, 213, 221));
                    HBRUSH dot_brush = CreateSolidBrush(dot_color);
                    HPEN dot_pen = CreatePen(PS_SOLID, 1, dot_color);
                    HGDIOBJ prev_brush = SelectObject(mem_dc, dot_brush);
                    HGDIOBJ prev_pen = SelectObject(mem_dc, dot_pen);

                    const int dot_left = scale_dip(dpi, 12);
                    const int dot_top = scale_dip(dpi, 22);
                    const int dot_size = std::max(4, scale_dip(dpi, 8));
                    Ellipse(
                        mem_dc,
                        dot_left,
                        dot_top,
                        dot_left + dot_size,
                        dot_top + dot_size);

                    SelectObject(mem_dc, prev_brush);
                    SelectObject(mem_dc, prev_pen);
                    DeleteObject(dot_brush);
                    DeleteObject(dot_pen);
                }

                wchar_t text_buf[128] = L"";
                GetWindowTextW(hwnd, text_buf, 128);

                SetTextColor(mem_dc, foreground);
                SetBkMode(mem_dc, TRANSPARENT);
                HFONT font = CreateFontW(
                    -std::max(9, scale_dip(dpi, 13)),
                    0,
                    0,
                    0,
                    FW_SEMIBOLD,
                    FALSE,
                    FALSE,
                    FALSE,
                    DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE,
                    L"Microsoft YaHei UI");
                HGDIOBJ old_font = SelectObject(mem_dc, font);

                RECT text_rc = layout.status;
                DrawTextW(
                    mem_dc,
                    text_buf,
                    -1,
                    &text_rc,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

                auto draw_button = [&](int button,
                                       const wchar_t* label,
                                       bool primary,
                                       bool enabled) {
                    const RECT bounds = layout.buttons[
                        static_cast<std::size_t>(button - 1)];
                    COLORREF fill = high_contrast
                                        ? GetSysColor(COLOR_BTNFACE)
                                        : RGB(35, 41, 51);
                    COLORREF outline = high_contrast
                                           ? GetSysColor(COLOR_BTNTEXT)
                                           : RGB(77, 87, 103);
                    COLORREF text = high_contrast
                                        ? GetSysColor(COLOR_BTNTEXT)
                                        : RGB(220, 226, 236);
                    if (!enabled) {
                        fill = high_contrast
                                   ? GetSysColor(COLOR_BTNFACE)
                                   : RGB(30, 34, 42);
                        outline = high_contrast
                                      ? GetSysColor(COLOR_GRAYTEXT)
                                      : RGB(49, 55, 66);
                        text = high_contrast
                                   ? GetSysColor(COLOR_GRAYTEXT)
                                   : RGB(105, 114, 129);
                    } else if (primary) {
                        fill = high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHT)
                                   : RGB(39, 100, 231);
                        outline = fill;
                        text = high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                   : RGB(255, 255, 255);
                    }
                    if (enabled && state->pressed_button == button) {
                        fill = high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHT)
                                   : (primary ? RGB(30, 85, 200)
                                              : RGB(24, 29, 37));
                    } else if (enabled && state->hover_button == button) {
                        fill = high_contrast
                                   ? GetSysColor(COLOR_HIGHLIGHT)
                                   : (primary ? RGB(58, 119, 244)
                                              : RGB(48, 57, 70));
                        if (high_contrast) {
                            text = GetSysColor(COLOR_HIGHLIGHTTEXT);
                        }
                    }

                    HBRUSH brush = CreateSolidBrush(fill);
                    HPEN pen = CreatePen(PS_SOLID, 1, outline);
                    HGDIOBJ previous_pen = SelectObject(mem_dc, pen);
                    HGDIOBJ previous_brush = SelectObject(mem_dc, brush);
                    RoundRect(
                        mem_dc,
                        bounds.left,
                        bounds.top,
                        bounds.right,
                        bounds.bottom,
                        layout.radius,
                        layout.radius);
                    SelectObject(mem_dc, previous_brush);
                    SelectObject(mem_dc, previous_pen);
                    DeleteObject(brush);
                    DeleteObject(pen);

                    SetTextColor(mem_dc, text);
                    RECT label_bounds = bounds;
                    DrawTextW(
                        mem_dc,
                        label,
                        -1,
                        &label_bounds,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                };

                draw_button(
                    1,
                    state->paused ? L"继续" : L"暂停",
                    false,
                    pause_button_enabled(*state));
                draw_button(2, L"完成", true, true);
                draw_button(3, L"取消", false, true);

                SelectObject(mem_dc, old_font);
                DeleteObject(font);

                BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);

                SelectObject(mem_dc, old_bm);
                DeleteObject(mem_bm);
                DeleteDC(mem_dc);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CLOSE: {
                complete(true);
                return 0;
            }
            case WM_DESTROY: {
                KillTimer(hwnd, kScrollBlinkTimer);
                KillTimer(hwnd, kScrollCaptureTimer);
                return 0;
            }
            case WM_NCDESTROY: {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            case WM_DPICHANGED: {
                const auto* suggested =
                    reinterpret_cast<const RECT*>(lp);
                if (suggested) {
                    SetWindowPos(
                        hwnd,
                        nullptr,
                        suggested->left,
                        suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOACTIVATE | SWP_NOZORDER);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            case WM_KEYDOWN: {
                // Fallback for accessibility tools that explicitly direct a
                // key here. Normal keyboard input is forwarded to the hidden
                // overlay owner because this palette never activates.
                switch (scroll_keyboard_command(wp)) {
                    case ScrollKeyboardCommand::finish:
                        complete(false);
                        return 0;
                    case ScrollKeyboardCommand::cancel:
                        complete(true);
                        return 0;
                    case ScrollKeyboardCommand::toggle_pause:
                        if (pause_button_enabled(*state)) {
                            auto callback = state->on_toggle_pause;
                            if (callback) {
                                callback();
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    case ScrollKeyboardCommand::none:
                        break;
                }
                break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
}


}  // namespace airshot::overlay_detail
