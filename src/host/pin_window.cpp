#include "pin_window.h"
#include "airshot/output.h"
#include <windowsx.h>
#include <mutex>
#include <cstring>
#include <algorithm>

namespace airshot {

void PinWindow::register_class(HINSTANCE instance) {
    static std::once_flag flag;
    std::call_once(flag, [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // We draw the entire area
        wc.lpszClassName = L"AirScreenshot.Pin";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
    });
}

std::unique_ptr<PinWindow> PinWindow::create(HINSTANCE instance, HWND parent, const Bitmap& bitmap, int x, int y) {
    register_class(instance);

    auto pin = std::make_unique<PinWindow>(nullptr, bitmap);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"AirScreenshot.Pin",
        L"Air Screenshot Pin",
        WS_POPUP | WS_VISIBLE,
        x, y, bitmap.width, bitmap.height,
        parent,
        nullptr,
        instance,
        pin.get()
    );

    if (!hwnd) {
        return nullptr;
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    return pin;
}

PinWindow::PinWindow(HWND hwnd, const Bitmap& bitmap) : hwnd_(hwnd), bitmap_(bitmap) {
    HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap_.width;
    info.bmiHeader.biHeight = -bitmap_.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    hbitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbitmap_ && bits) {
        std::memcpy(bits, bitmap_.pixels.data(), bitmap_.pixels.size());
    }
    ReleaseDC(nullptr, screen);
}

PinWindow::~PinWindow() {
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

LRESULT CALLBACK PinWindow::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    PinWindow* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<PinWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        return self->handle_message(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT PinWindow::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_PAINT:
            paint();
            return 0;

        case WM_ERASEBKGND:
            return 1; // Prevent flickering since we paint everything

        case WM_NCHITTEST:
            // If click-through is enabled, we should let mouse messages pass through (HTTRANSPARENT)
            {
                const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
                if (style & WS_EX_TRANSPARENT) {
                    return HTTRANSPARENT;
                }
            }
            return HTCAPTION; // Allow dragging the window

        case WM_NCLBUTTONDBLCLK:
            // Double click client area (translated to caption due to NCHITTEST) closes the window
            DestroyWindow(hwnd_);
            return 0;

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                alpha_ = std::clamp(alpha_ + (delta > 0 ? 15 : -15), 30, 255);
                SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha_), LWA_ALPHA);
                return 0;
            }
            POINT cursor_screen{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

            RECT rect_window;
            GetWindowRect(hwnd_, &rect_window);

            int width = rect_window.right - rect_window.left;
            int height = rect_window.bottom - rect_window.top;

            double rx = 0.5;
            double ry = 0.5;
            if (width > 0 && height > 0) {
                rx = static_cast<double>(cursor_screen.x - rect_window.left) / width;
                ry = static_cast<double>(cursor_screen.y - rect_window.top) / height;
            }

            double old_scale = scale_;
            if (delta > 0) {
                scale_ *= 1.1;
            } else {
                scale_ /= 1.1;
            }
            scale_ = std::clamp(scale_, 0.1, 10.0);

            if (scale_ != old_scale) {
                int new_width = static_cast<int>(bitmap_.width * scale_);
                int new_height = static_cast<int>(bitmap_.height * scale_);

                int new_left = cursor_screen.x - static_cast<int>(rx * new_width);
                int new_top = cursor_screen.y - static_cast<int>(ry * new_height);

                SetWindowPos(hwnd_, nullptr, new_left, new_top, new_width, new_height, SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                std::wstring error;
                if (!copy_bitmap_to_clipboard(bitmap_, &error)) {
                    MessageBoxW(hwnd_, error.c_str(), L"Air Screenshot", MB_OK | MB_ICONERROR);
                }
                return 0;
            } else if (wparam == VK_ESCAPE) {
                DestroyWindow(hwnd_);
                return 0;
            } else if (wparam == 'R') {
                rotate(true);
                return 0;
            } else if (wparam == 'L') {
                rotate(false);
                return 0;
            } else if (wparam == 'H') {
                flip(true);
                return 0;
            } else if (wparam == 'V') {
                flip(false);
                return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            POINT pt;
            pt.x = GET_X_LPARAM(lparam);
            pt.y = GET_Y_LPARAM(lparam);
            if (pt.x == -1 && pt.y == -1) {
                RECT rect;
                GetWindowRect(hwnd_, &rect);
                pt.x = rect.left + (rect.right - rect.left) / 2;
                pt.y = rect.top + (rect.bottom - rect.top) / 2;
            }
            show_context_menu(pt);
            return 0;
        }

        case WM_DESTROY: {
            HWND parent = GetParent(hwnd_);
            if (parent) {
                PostMessageW(parent, WM_PIN_WINDOW_CLOSED, 0, reinterpret_cast<LPARAM>(this));
            }
            hwnd_ = nullptr;
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PinWindow::paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rect;
    GetClientRect(hwnd_, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    HDC mem_dc = CreateCompatibleDC(hdc);
    HGDIOBJ old_bitmap = SelectObject(mem_dc, hbitmap_);

    // Set high-quality stretching mode
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);

    StretchBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, bitmap_.width, bitmap_.height, SRCCOPY);

    // Draw 1px border around the pinned image
    HPEN border_pen = CreatePen(PS_INSIDEFRAME, 1, RGB(180, 180, 180));
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, 0, 0, width, height);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);

    SelectObject(mem_dc, old_bitmap);
    DeleteDC(mem_dc);

    EndPaint(hwnd_, &ps);
}

void PinWindow::show_context_menu(POINT screen_pos) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"复制 (Copy)\tCtrl+C");
    AppendMenuW(menu, MF_STRING, 2, L"保存 (Save...)");
    AppendMenuW(menu, MF_STRING, 4, L"鼠标穿透 (Click-through)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"顺时针旋转 90° (Rotate 90° CW)\tR");
    AppendMenuW(menu, MF_STRING, 6, L"逆时针旋转 90° (Rotate 90° CCW)\tL");
    AppendMenuW(menu, MF_STRING, 7, L"水平翻转 (Flip Horizontal)\tH");
    AppendMenuW(menu, MF_STRING, 8, L"垂直翻转 (Flip Vertical)\tV");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"关闭 (Close)\tEsc / 双击");

    int selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen_pos.x, screen_pos.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (selection == 1) {
        std::wstring error;
        if (!copy_bitmap_to_clipboard(bitmap_, &error)) {
            MessageBoxW(hwnd_, error.c_str(), L"Air Screenshot", MB_OK | MB_ICONERROR);
        }
    } else if (selection == 2) {
        std::optional<std::filesystem::path> path = prompt_png_path(hwnd_);
        if (path) {
            std::wstring error;
            if (!save_png(bitmap_, *path, &error)) {
                MessageBoxW(hwnd_, error.c_str(), L"Air Screenshot", MB_OK | MB_ICONERROR);
            }
        }
    } else if (selection == 3) {
        DestroyWindow(hwnd_);
    } else if (selection == 4) {
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        style |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        MessageBoxW(hwnd_, L"贴图已设置为鼠标穿透。若要恢复，请右键托盘菜单选择“销毁所有贴图”或重启程序。", L"Air Screenshot", MB_OK | MB_ICONINFORMATION);
    } else if (selection == 5) {
        rotate(true);
    } else if (selection == 6) {
        rotate(false);
    } else if (selection == 7) {
        flip(true);
    } else if (selection == 8) {
        flip(false);
    }
}

void PinWindow::rotate(bool cw) {
    if (bitmap_.empty()) return;
    
    bitmap_ = cw ? rotate_90_cw(bitmap_) : rotate_90_ccw(bitmap_);
    
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    
    HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap_.width;
    info.bmiHeader.biHeight = -bitmap_.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    hbitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbitmap_ && bits) {
        std::memcpy(bits, bitmap_.pixels.data(), bitmap_.pixels.size());
    }
    ReleaseDC(nullptr, screen);
    
    RECT rect_window;
    GetWindowRect(hwnd_, &rect_window);
    int new_width = static_cast<int>(bitmap_.width * scale_);
    int new_height = static_cast<int>(bitmap_.height * scale_);
    
    SetWindowPos(hwnd_, nullptr, rect_window.left, rect_window.top, new_width, new_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PinWindow::flip(bool horizontal) {
    if (bitmap_.empty()) return;
    
    bitmap_ = horizontal ? flip_horizontal(bitmap_) : flip_vertical(bitmap_);
    
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    
    HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap_.width;
    info.bmiHeader.biHeight = -bitmap_.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    hbitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbitmap_ && bits) {
        std::memcpy(bits, bitmap_.pixels.data(), bitmap_.pixels.size());
    }
    ReleaseDC(nullptr, screen);
    
    InvalidateRect(hwnd_, nullptr, TRUE);
}

} // namespace airshot
