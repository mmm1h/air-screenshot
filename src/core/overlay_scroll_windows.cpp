#include "overlay_helpers.h"

#include <commctrl.h>

#include <algorithm>
#include <mutex>

namespace airshot::overlay_detail {

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

        int w = 240;
        int h = 50;
        int x = selection.right - w;
        int y = selection.bottom + 6;

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
        x = std::clamp(x, work_left + 10, std::max(work_left + 10, work_right - w - 10));
        if (y + h > work_bottom) y = selection.top - h - 6;
        y = std::clamp(y, work_top + 10, std::max(work_top + 10, work_bottom - h - 10));

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
                } else if (wp == kScrollCaptureTimer && state->on_tick) {
                    auto callback = state->on_tick;
                    callback();
                }
                return 0;
            }
            case WM_SETTEXT: {
                LRESULT res = DefWindowProcW(hwnd, msg, wp, lp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return res;
            }
            case WM_MOUSEMOVE: {
                int x = static_cast<short>(LOWORD(lp));
                int y = static_cast<short>(HIWORD(lp));
                const int previous_hover = state->hover_button;

                if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                    state->hover_button = 1;
                } else if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                    state->hover_button = 2;
                } else {
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
                int x = static_cast<short>(LOWORD(lp));
                int y = static_cast<short>(HIWORD(lp));
                if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                    state->pressed_button = 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                    state->pressed_button = 2;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                if (state->pressed_button != 0) {
                    SetCapture(hwnd);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                const int pressed_button = state->pressed_button;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                const int x = static_cast<short>(LOWORD(lp));
                const int y = static_cast<short>(HIWORD(lp));
                state->pressed_button = 0;
                if (pressed_button == 1) {
                    if (x >= 110 && x <= 170 && y >= 10 && y <= 40) {
                        complete(false);
                        return 0;
                    }
                } else if (pressed_button == 2) {
                    if (x >= 175 && x <= 230 && y >= 10 && y <= 40) {
                        complete(true);
                        return 0;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_CAPTURECHANGED: {
                state->pressed_button = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
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

                if (state->blink_counter % 2 == 0) {
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
                COLORREF ok_bg = RGB(0, 102, 255);
                if (state->pressed_button == 1) ok_bg = RGB(0, 77, 204);
                else if (state->hover_button == 1) ok_bg = RGB(51, 136, 255);

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
                if (state->pressed_button == 2) cancel_bg = RGB(20, 22, 24);
                else if (state->hover_button == 2) cancel_bg = RGB(50, 55, 60);

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
            case WM_KEYDOWN: {
                if (wp == VK_RETURN) {
                    complete(false);
                    return 0;
                }
                if (wp == VK_ESCAPE) {
                    complete(true);
                    return 0;
                }
                break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
}


}  // namespace airshot::overlay_detail
