#include "overlay_helpers.h"

#include <commctrl.h>

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

LRESULT CALLBACK scroll_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
                COLORREF ok_bg = RGB(0, 102, 255);
                if (pressed_btn == 1) ok_bg = RGB(0, 77, 204);
                else if (hover_btn == 1) ok_bg = RGB(51, 136, 255);

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


}  // namespace airshot::overlay_detail
