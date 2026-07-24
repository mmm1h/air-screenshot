#include "pin_window.h"
#include "airshot/output.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

namespace airshot {
namespace {

class ScreenDc {
public:
    ScreenDc() noexcept : value_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(nullptr, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class MemoryDc {
public:
    explicit MemoryDc(HDC compatible_with) noexcept : value_(CreateCompatibleDC(compatible_with)) {}
    ~MemoryDc() {
        if (value_) {
            DeleteDC(value_);
        }
    }
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class OwnedGdiObject {
public:
    explicit OwnedGdiObject(HGDIOBJ value = nullptr) noexcept : value_(value) {}
    ~OwnedGdiObject() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedGdiObject(const OwnedGdiObject&) = delete;
    OwnedGdiObject& operator=(const OwnedGdiObject&) = delete;

    [[nodiscard]] HGDIOBJ get() const noexcept { return value_; }

private:
    HGDIOBJ value_{};
};

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ value) noexcept
        : dc_(dc), previous_(dc && value ? SelectObject(dc, value) : nullptr) {}
    ~SelectedObject() {
        if (valid()) {
            SelectObject(dc_, previous_);
        }
    }
    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return previous_ != nullptr && previous_ != HGDI_ERROR;
    }

private:
    HDC dc_{};
    HGDIOBJ previous_{};
};

class OwnedMenu {
public:
    explicit OwnedMenu(HMENU value) noexcept : value_(value) {}
    ~OwnedMenu() {
        if (value_) {
            DestroyMenu(value_);
        }
    }
    OwnedMenu(const OwnedMenu&) = delete;
    OwnedMenu& operator=(const OwnedMenu&) = delete;

    [[nodiscard]] HMENU get() const noexcept { return value_; }

private:
    HMENU value_{};
};

}  // namespace

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
    if (!bitmap.valid()) {
        return nullptr;
    }
    register_class(instance);

    std::unique_ptr<PinWindow> pin;
    try {
        pin = std::make_unique<PinWindow>(nullptr, bitmap);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (const std::length_error&) {
        return nullptr;
    }
    pin->owner_ = parent;
    if (!pin->rebuild_native_bitmap()) {
        return nullptr;
    }
    const auto initial_size = pin->scaled_size(1.0);
    if (!initial_size) {
        return nullptr;
    }
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"AirScreenshot.Pin",
        L"Air Screenshot Pin",
        WS_POPUP | WS_VISIBLE,
        x, y, initial_size->cx, initial_size->cy,
        parent,
        nullptr,
        instance,
        pin.get()
    );

    if (!hwnd) {
        return nullptr;
    }

    if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
        DestroyWindow(hwnd);
        return nullptr;
    }
    pin->notify_owner_ = true;

    return pin;
}

PinWindow::PinWindow(HWND hwnd, const Bitmap& bitmap) : hwnd_(hwnd), bitmap_(bitmap) {}

bool PinWindow::rebuild_native_bitmap() {
    if (!bitmap_.valid()) {
        return false;
    }
    ScreenDc screen;
    if (!screen.get()) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap_.width;
    info.bmiHeader.biHeight = -bitmap_.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP replacement =
        CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!replacement || !bits) {
        if (replacement) {
            DeleteObject(replacement);
        }
        return false;
    }
    std::memcpy(bits, bitmap_.pixels.data(), bitmap_.pixels.size());
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    hbitmap_ = replacement;
    bitmap_bits_ = bits;
    return true;
}

bool PinWindow::replace_bitmap(Bitmap bitmap) {
    if (!bitmap.valid()) {
        return false;
    }
    ScreenDc screen;
    if (!screen.get()) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP replacement =
        CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!replacement || !bits) {
        if (replacement) {
            DeleteObject(replacement);
        }
        return false;
    }
    std::memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size());
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    bitmap_ = std::move(bitmap);
    hbitmap_ = replacement;
    bitmap_bits_ = bits;
    return true;
}

std::optional<SIZE> PinWindow::scaled_size(double scale) const noexcept {
    if (!bitmap_.valid() || !std::isfinite(scale) || scale <= 0.0) {
        return std::nullopt;
    }
    const long double width = static_cast<long double>(bitmap_.width) * scale;
    const long double height = static_cast<long double>(bitmap_.height) * scale;
    if (width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return SIZE{
        std::max(1, static_cast<int>(std::llround(width))),
        std::max(1, static_cast<int>(std::llround(height))),
    };
}

PinWindow::~PinWindow() {
    if (hwnd_ && IsWindow(hwnd_)) {
        notify_owner_ = false;
        DestroyWindow(hwnd_);
    }
    if (hbitmap_) {
        DeleteObject(hbitmap_);
        hbitmap_ = nullptr;
        bitmap_bits_ = nullptr;
    }
}

void PinWindow::request_close() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return;
    }
    if (modal_depth_ != 0) {
        close_pending_ = true;
        return;
    }
    DestroyWindow(hwnd_);
}

void PinWindow::enter_modal() noexcept {
    ++modal_depth_;
}

bool PinWindow::leave_modal() noexcept {
    if (modal_depth_ != 0) {
        --modal_depth_;
    }
    if (modal_depth_ == 0 && close_pending_) {
        close_pending_ = false;
        request_close();
        return false;
    }
    return hwnd_ && IsWindow(hwnd_);
}

void PinWindow::show_message(std::wstring_view message, UINT flags) {
    enter_modal();
    MessageBoxW(hwnd_,
                std::wstring(message).c_str(),
                L"Air Screenshot",
                flags);
    (void)leave_modal();
}

LRESULT CALLBACK PinWindow::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    PinWindow* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<PinWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) {
            self->hwnd_ = nullptr;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
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
            request_close();
            return 0;

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                const int candidate = std::clamp(alpha_ + (delta > 0 ? 15 : -15), 30, 255);
                if (SetLayeredWindowAttributes(
                        hwnd_, 0, static_cast<BYTE>(candidate), LWA_ALPHA)) {
                    alpha_ = candidate;
                }
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

            const double wheel_steps =
                static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA);
            const double candidate_scale =
                std::clamp(scale_ * std::pow(1.1, wheel_steps), 0.1, 10.0);
            const auto candidate_size = scaled_size(candidate_scale);
            if (candidate_size && candidate_scale != scale_) {
                const long long new_left =
                    static_cast<long long>(cursor_screen.x) -
                    static_cast<long long>(std::llround(rx * candidate_size->cx));
                const long long new_top =
                    static_cast<long long>(cursor_screen.y) -
                    static_cast<long long>(std::llround(ry * candidate_size->cy));
                const int clamped_left = static_cast<int>(std::clamp<long long>(
                    new_left,
                    std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max()));
                const int clamped_top = static_cast<int>(std::clamp<long long>(
                    new_top,
                    std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max()));
                if (SetWindowPos(hwnd_,
                                 nullptr,
                                 clamped_left,
                                 clamped_top,
                                 candidate_size->cx,
                                 candidate_size->cy,
                                 SWP_NOZORDER | SWP_NOACTIVATE)) {
                    scale_ = candidate_scale;
                    InvalidateRect(hwnd_, nullptr, TRUE);
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                std::wstring error;
                if (!copy_bitmap_to_clipboard(bitmap_, &error)) {
                    show_message(error, MB_OK | MB_ICONERROR);
                }
                return 0;
            } else if (wparam == VK_ESCAPE) {
                request_close();
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
            close_pending_ = false;
            if (notify_owner_ && owner_ && IsWindow(owner_)) {
                notify_owner_ = false;
                PostMessageW(owner_, WM_PIN_WINDOW_CLOSED, 0, reinterpret_cast<LPARAM>(this));
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PinWindow::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (!hdc) {
        EndPaint(hwnd_, &ps);
        return;
    }

    RECT rect{};
    if (!GetClientRect(hwnd_, &rect)) {
        EndPaint(hwnd_, &ps);
        return;
    }
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || !hbitmap_ || !bitmap_.valid()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    {
        MemoryDc mem_dc(hdc);
        SelectedObject selected_bitmap(mem_dc.get(), hbitmap_);
        if (mem_dc.get() && selected_bitmap.valid()) {
            // Set high-quality stretching mode
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);

            StretchBlt(hdc,
                       0,
                       0,
                       width,
                       height,
                       mem_dc.get(),
                       0,
                       0,
                       bitmap_.width,
                       bitmap_.height,
                       SRCCOPY);

            // Draw 1px border around the pinned image
            OwnedGdiObject border_pen(CreatePen(PS_INSIDEFRAME, 1, RGB(180, 180, 180)));
            SelectedObject selected_pen(hdc, border_pen.get());
            SelectedObject selected_brush(hdc, GetStockObject(NULL_BRUSH));
            if (selected_pen.valid() && selected_brush.valid()) {
                Rectangle(hdc, 0, 0, width, height);
            }
        }
    }

    EndPaint(hwnd_, &ps);
}

void PinWindow::show_context_menu(POINT screen_pos) {
    enter_modal();
    OwnedMenu menu(CreatePopupMenu());
    if (!menu.get()) {
        (void)leave_modal();
        return;
    }
    AppendMenuW(menu.get(), MF_STRING, 1, L"复制 (Copy)\tCtrl+C");
    AppendMenuW(menu.get(), MF_STRING, 2, L"保存 (Save...)");
    AppendMenuW(menu.get(), MF_STRING, 4, L"鼠标穿透 (Click-through)");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 5, L"顺时针旋转 90° (Rotate 90° CW)\tR");
    AppendMenuW(menu.get(), MF_STRING, 6, L"逆时针旋转 90° (Rotate 90° CCW)\tL");
    AppendMenuW(menu.get(), MF_STRING, 7, L"水平翻转 (Flip Horizontal)\tH");
    AppendMenuW(menu.get(), MF_STRING, 8, L"垂直翻转 (Flip Vertical)\tV");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 3, L"关闭 (Close)\tEsc / 双击");

    int selection = TrackPopupMenu(menu.get(),
                                   TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                   screen_pos.x,
                                   screen_pos.y,
                                   0,
                                   hwnd_,
                                   nullptr);

    if (close_pending_) {
        (void)leave_modal();
        return;
    }

    if (selection == 1) {
        std::wstring error;
        if (!copy_bitmap_to_clipboard(bitmap_, &error)) {
            show_message(error, MB_OK | MB_ICONERROR);
        }
    } else if (selection == 2) {
        std::optional<std::filesystem::path> path = prompt_png_path(hwnd_);
        if (!close_pending_ && path) {
            std::wstring error;
            if (!save_png(bitmap_, *path, &error)) {
                show_message(error, MB_OK | MB_ICONERROR);
            }
        }
    } else if (selection == 3) {
        request_close();
    } else if (selection == 4) {
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        style |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha_), LWA_ALPHA);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        show_message(L"贴图已设置为鼠标穿透。若要恢复，请右键托盘菜单选择“销毁所有贴图”或重启程序。",
                     MB_OK | MB_ICONINFORMATION);
    } else if (selection == 5) {
        rotate(true);
    } else if (selection == 6) {
        rotate(false);
    } else if (selection == 7) {
        flip(true);
    } else if (selection == 8) {
        flip(false);
    }
    (void)leave_modal();
}

void PinWindow::rotate(bool cw) {
    if (bitmap_.empty()) return;

    Bitmap transformed = cw ? rotate_90_cw(bitmap_) : rotate_90_ccw(bitmap_);
    if (transformed.empty() || !replace_bitmap(std::move(transformed))) {
        show_message(L"无法旋转贴图：内存或 GDI 资源不足。",
                     MB_OK | MB_ICONERROR);
        return;
    }
    const auto size = scaled_size(scale_);
    RECT rect_window{};
    if (size && GetWindowRect(hwnd_, &rect_window)) {
        SetWindowPos(hwnd_,
                     nullptr,
                     rect_window.left,
                     rect_window.top,
                     size->cx,
                     size->cy,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PinWindow::flip(bool horizontal) {
    if (bitmap_.empty() || !bitmap_bits_) return;

    if (horizontal) {
        for (int y = 0; y < bitmap_.height; ++y) {
            auto row = bitmap_.row(y);
            for (int left = 0, right = bitmap_.width - 1; left < right; ++left, --right) {
                auto* left_pixel =
                    row.data() + static_cast<std::size_t>(left) * Bitmap::bytes_per_pixel;
                auto* right_pixel =
                    row.data() + static_cast<std::size_t>(right) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < Bitmap::bytes_per_pixel; ++channel) {
                    std::swap(left_pixel[channel], right_pixel[channel]);
                }
            }
        }
    } else {
        for (int top = 0, bottom = bitmap_.height - 1; top < bottom; ++top, --bottom) {
            auto top_row = bitmap_.row(top);
            auto bottom_row = bitmap_.row(bottom);
            std::swap_ranges(top_row.begin(), top_row.end(), bottom_row.begin());
        }
    }
    std::memcpy(bitmap_bits_, bitmap_.pixels.data(), bitmap_.pixels.size());
    InvalidateRect(hwnd_, nullptr, TRUE);
}

} // namespace airshot
