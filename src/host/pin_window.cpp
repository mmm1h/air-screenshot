#include "pin_window.h"
#include "airshot/clipboard.h"
#include "airshot/output.h"
#include "airshot/pin_layout.h"
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

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

class DropFilesScope {
public:
    explicit DropFilesScope(HDROP value) noexcept : value_(value) {}
    ~DropFilesScope() {
        if (value_) {
            DragFinish(value_);
        }
    }
    DropFilesScope(const DropFilesScope&) = delete;
    DropFilesScope& operator=(const DropFilesScope&) = delete;

private:
    HDROP value_{};
};

[[nodiscard]] bool set_extended_style(
    HWND window,
    LONG_PTR style) noexcept {
    SetLastError(ERROR_SUCCESS);
    return SetWindowLongPtrW(window, GWL_EXSTYLE, style) != 0 ||
           GetLastError() == ERROR_SUCCESS;
}

void notify_extended_style_changed(HWND window) noexcept {
    SetWindowPos(
        window,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

[[nodiscard]] bool present_layered_bitmap(
    HWND window,
    const Bitmap& premultiplied,
    int alpha) noexcept {
    if (!window || !IsWindow(window) || !premultiplied.valid()) {
        return false;
    }
    ScreenDc screen;
    if (!screen.get()) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = premultiplied.width;
    info.bmiHeader.biHeight = -premultiplied.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    OwnedGdiObject surface(CreateDIBSection(
        screen.get(),
        &info,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0));
    if (!surface.get() || !bits) {
        return false;
    }
    std::memcpy(
        bits,
        premultiplied.pixels.data(),
        premultiplied.pixels.size());

    MemoryDc source_dc(screen.get());
    SelectedObject selected(source_dc.get(), surface.get());
    RECT window_rect{};
    if (!source_dc.get() || !selected.valid() ||
        !GetWindowRect(window, &window_rect)) {
        return false;
    }
    POINT destination{window_rect.left, window_rect.top};
    SIZE size{premultiplied.width, premultiplied.height};
    POINT source{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(
        std::clamp(alpha, 0, 255));
    blend.AlphaFormat = AC_SRC_ALPHA;
    return UpdateLayeredWindow(
               window,
               screen.get(),
               &destination,
               &size,
               source_dc.get(),
               &source,
               0,
               &blend,
               ULW_ALPHA) != FALSE;
}

constexpr wchar_t kScalePromptClass[] =
    L"AirScreenshot.PinScalePrompt";

struct ScalePromptState {
    HWND window{};
    HWND edit{};
    int initial_percent{100};
    std::optional<int> result;
    bool done{};
};

[[nodiscard]] int scale_for_dpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

LRESULT CALLBACK scale_prompt_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<ScalePromptState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<ScalePromptState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
        case WM_CREATE: {
            const UINT dpi = std::max<UINT>(96, GetDpiForWindow(window));
            const HFONT font = static_cast<HFONT>(
                GetStockObject(DEFAULT_GUI_FONT));
            const auto create_child = [&](const wchar_t* class_name,
                                          const wchar_t* text,
                                          DWORD style,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          int id) {
                HWND child = CreateWindowExW(
                    0,
                    class_name,
                    text,
                    WS_CHILD | WS_VISIBLE | style,
                    scale_for_dpi(x, dpi),
                    scale_for_dpi(y, dpi),
                    scale_for_dpi(width, dpi),
                    scale_for_dpi(height, dpi),
                    window,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(id)),
                    nullptr,
                    nullptr);
                if (child && font) {
                    SendMessageW(
                        child,
                        WM_SETFONT,
                        reinterpret_cast<WPARAM>(font),
                        TRUE);
                }
                return child;
            };
            (void)create_child(
                L"STATIC",
                L"输入缩放百分比（10–1000）：",
                0,
                16,
                14,
                236,
                20,
                0);
            state->edit = create_child(
                L"EDIT",
                L"",
                WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                16,
                39,
                176,
                26,
                100);
            (void)create_child(
                L"STATIC",
                L"%",
                0,
                199,
                43,
                28,
                20,
                0);
            (void)create_child(
                L"BUTTON",
                L"确定",
                WS_TABSTOP | BS_DEFPUSHBUTTON,
                78,
                77,
                82,
                28,
                IDOK);
            (void)create_child(
                L"BUTTON",
                L"取消",
                WS_TABSTOP,
                168,
                77,
                82,
                28,
                IDCANCEL);
            if (!state->edit) {
                DestroyWindow(window);
                return -1;
            }
            const std::wstring initial =
                std::to_wstring(state->initial_percent);
            SetWindowTextW(state->edit, initial.c_str());
            SendMessageW(state->edit, EM_SETLIMITTEXT, 4, 0);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            SetFocus(state->edit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                std::array<wchar_t, 16> text{};
                GetWindowTextW(
                    state->edit,
                    text.data(),
                    static_cast<int>(text.size()));
                const auto parsed = parse_pin_scale_percent(text.data());
                if (!parsed) {
                    MessageBoxW(
                        window,
                        L"请输入 10 到 1000 之间的整数百分比。",
                        L"缩放百分比",
                        MB_OK | MB_ICONWARNING);
                    SetFocus(state->edit);
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                    return 0;
                }
                state->result = *parsed;
                DestroyWindow(window);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested) {
                SetWindowPos(
                    window,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_DESTROY:
            state->done = true;
            state->window = nullptr;
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] std::optional<int> prompt_pin_scale_percent(
    HWND owner,
    int initial_percent) {
    if (!owner || !IsWindow(owner)) {
        return std::nullopt;
    }
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    static std::once_flag class_flag;
    std::call_once(class_flag, [instance] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = scale_prompt_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);
        window_class.lpszClassName = kScalePromptClass;
        RegisterClassExW(&window_class);
    });

    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(owner));
    RECT client{
        0,
        0,
        scale_for_dpi(268, dpi),
        scale_for_dpi(120, dpi),
    };
    AdjustWindowRectExForDpi(
        &client,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        FALSE,
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
        dpi);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    RECT owner_bounds{};
    GetWindowRect(owner, &owner_bounds);
    int left = owner_bounds.left +
               (owner_bounds.right - owner_bounds.left - width) / 2;
    int top = owner_bounds.top +
              (owner_bounds.bottom - owner_bounds.top - height) / 2;
    const HMONITOR monitor = MonitorFromWindow(
        owner,
        MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        left = std::clamp(
            left,
            static_cast<int>(monitor_info.rcWork.left),
            std::max(
                static_cast<int>(monitor_info.rcWork.left),
                static_cast<int>(monitor_info.rcWork.right) - width));
        top = std::clamp(
            top,
            static_cast<int>(monitor_info.rcWork.top),
            std::max(
                static_cast<int>(monitor_info.rcWork.top),
                static_cast<int>(monitor_info.rcWork.bottom) - height));
    }

    ScalePromptState state;
    state.initial_percent = clamp_pin_scale_percent(initial_percent);
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
        kScalePromptClass,
        L"缩放百分比",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        left,
        top,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state);
    if (!window) {
        return std::nullopt;
    }
    (void)SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    bool repost_quit = false;
    WPARAM quit_code = 0;
    while (!state.done) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                repost_quit = true;
                quit_code = message.wParam;
            }
            break;
        }
        if (message.message == WM_KEYDOWN &&
            (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) {
            SendMessageW(
                window,
                WM_COMMAND,
                message.wParam == VK_RETURN ? IDOK : IDCANCEL,
                0);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.window && IsWindow(state.window)) {
        DestroyWindow(state.window);
    }
    if (IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (repost_quit) {
        PostQuitMessage(static_cast<int>(quit_code));
    }
    return state.result;
}

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

std::unique_ptr<PinWindow> PinWindow::create(
    HINSTANCE instance,
    HWND parent,
    Bitmap bitmap,
    int x,
    int y,
    bool click_through_available,
    ReplacementGuard replacement_guard) {
    if (!bitmap.valid()) {
        return nullptr;
    }
    register_class(instance);

    std::unique_ptr<PinWindow> pin;
    try {
        pin = std::make_unique<PinWindow>(
            nullptr,
            std::move(bitmap));
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (const std::length_error&) {
        return nullptr;
    }
    pin->owner_ = parent;
    pin->click_through_available_ = click_through_available;
    pin->replacement_guard_ = std::move(replacement_guard);
    if (!pin->rebuild_native_bitmap()) {
        return nullptr;
    }
    const POINT requested_position{x, y};
    const HMONITOR monitor =
        MonitorFromPoint(requested_position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        pin->scale_ = fit_pin_scale(
            pin->bitmap_.width,
            pin->bitmap_.height,
            RectI::from_native(monitor_info.rcWork));
    }
    const auto initial_size = pin->scaled_size(pin->scale_);
    if (!initial_size) {
        return nullptr;
    }
    const PinWindowStylePlan initial_style = plan_pin_window_style(
        pin->source_has_transparency_,
        pin->alpha_,
        false);
    pin->per_pixel_presentation_active_ =
        initial_style.per_pixel_alpha;
    const DWORD extended_style =
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        (initial_style.layered ? WS_EX_LAYERED : 0U);
    HWND hwnd = CreateWindowExW(
        extended_style,
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

    if (!pin->refresh_window_presentation()) {
        DestroyWindow(hwnd);
        return nullptr;
    }

    // Keep Air Screenshot's own reference windows out of subsequent captures.
    // Older Windows releases safely degrade this affinity to WDA_MONITOR.
    (void)SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    DragAcceptFiles(hwnd, TRUE);
    pin->notify_owner_ = true;
    pin->ensure_visible();

    return pin;
}

PinWindow::PinWindow(HWND hwnd, Bitmap bitmap)
    : hwnd_(hwnd), bitmap_(std::move(bitmap)) {}

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
    if (!write_pin_visual_pixels(
            bitmap_,
            visual_effects_,
            std::span<std::uint8_t>(
                static_cast<std::uint8_t*>(bits),
                bitmap_.pixels.size()))) {
        DeleteObject(replacement);
        return false;
    }
    if (hbitmap_) {
        DeleteObject(hbitmap_);
    }
    hbitmap_ = replacement;
    source_has_transparency_ =
        summarize_pin_bitmap_alpha(bitmap_).has_transparency;
    return true;
}

bool PinWindow::refresh_window_presentation() noexcept {
    return update_window_presentation(
        alpha_,
        click_through(),
        source_has_transparency_);
}

bool PinWindow::update_window_presentation(
    int alpha,
    bool click_through_enabled,
    bool source_has_transparency) noexcept {
    if (!hwnd_ || !IsWindow(hwnd_) || !bitmap_.valid()) {
        return false;
    }

    const PinWindowStylePlan plan = plan_pin_window_style(
        source_has_transparency,
        alpha,
        click_through_enabled);
    Bitmap layered_frame;
    if (plan.per_pixel_alpha) {
        RECT client{};
        if (!GetClientRect(hwnd_, &client)) {
            return false;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        layered_frame = render_pin_layered_bitmap(
            bitmap_,
            visual_effects_,
            width,
            height,
            smooth_scaling_);
        if (!layered_frame.valid()) {
            return false;
        }
    }

    const LONG_PTR original_style =
        GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR desired_style = original_style;
    if (click_through_enabled) {
        desired_style |= WS_EX_TRANSPARENT;
    } else {
        desired_style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    if (plan.layered) {
        desired_style |= WS_EX_LAYERED;
    } else {
        desired_style &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
    }

    const bool mode_changed =
        plan.per_pixel_alpha != per_pixel_presentation_active_;
    bool style_was_reset = false;
    if (mode_changed && (original_style & WS_EX_LAYERED) != 0) {
        if (!set_extended_style(
                hwnd_,
                original_style &
                    ~static_cast<LONG_PTR>(WS_EX_LAYERED))) {
            return false;
        }
        style_was_reset = true;
    }
    if ((style_was_reset || desired_style != original_style) &&
        !set_extended_style(hwnd_, desired_style)) {
        (void)set_extended_style(hwnd_, original_style);
        notify_extended_style_changed(hwnd_);
        return false;
    }
    if (style_was_reset || desired_style != original_style) {
        notify_extended_style_changed(hwnd_);
    }

    bool presented = true;
    if (plan.per_pixel_alpha) {
        presented = present_layered_bitmap(
            hwnd_,
            layered_frame,
            alpha);
    } else if (plan.layered) {
        presented = SetLayeredWindowAttributes(
                        hwnd_,
                        0,
                        static_cast<BYTE>(std::clamp(alpha, 0, 255)),
                        LWA_ALPHA) != FALSE;
    } else {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    if (!presented) {
        (void)set_extended_style(hwnd_, original_style);
        notify_extended_style_changed(hwnd_);
        if (!per_pixel_presentation_active_ &&
            (original_style & WS_EX_LAYERED) != 0) {
            (void)SetLayeredWindowAttributes(
                hwnd_,
                0,
                static_cast<BYTE>(alpha_),
                LWA_ALPHA);
        } else {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return false;
    }

    per_pixel_presentation_active_ = plan.per_pixel_alpha;
    if (!plan.per_pixel_alpha) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
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
    if (!write_pin_visual_pixels(
            bitmap,
            visual_effects_,
            std::span<std::uint8_t>(
                static_cast<std::uint8_t*>(bits),
                bitmap.pixels.size()))) {
        DeleteObject(replacement);
        return false;
    }
    const bool replacement_has_transparency =
        summarize_pin_bitmap_alpha(bitmap).has_transparency;
    Bitmap previous_bitmap = std::move(bitmap_);
    HBITMAP previous_native_bitmap = hbitmap_;
    const bool previous_has_transparency = source_has_transparency_;
    bitmap_ = std::move(bitmap);
    hbitmap_ = replacement;
    source_has_transparency_ = replacement_has_transparency;
    if (hwnd_ && IsWindow(hwnd_) &&
        !refresh_window_presentation()) {
        bitmap_ = std::move(previous_bitmap);
        hbitmap_ = previous_native_bitmap;
        source_has_transparency_ = previous_has_transparency;
        DeleteObject(replacement);
        (void)refresh_window_presentation();
        return false;
    }
    if (previous_native_bitmap) {
        DeleteObject(previous_native_bitmap);
    }
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
    lifecycle_ = transition_pin_lifecycle(
        lifecycle_,
        PinLifecycleAction::destroy);
    DestroyWindow(hwnd_);
}

void PinWindow::request_hide() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_) ||
        lifecycle_ == PinLifecycleState::destroyed) {
        return;
    }
    lifecycle_ = transition_pin_lifecycle(
        lifecycle_,
        PinLifecycleAction::hide);
    visible_before_capture_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void PinWindow::request_show() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_) ||
        lifecycle_ == PinLifecycleState::destroyed) {
        return;
    }
    lifecycle_ = transition_pin_lifecycle(
        lifecycle_,
        PinLifecycleAction::show);
    if (capture_suspend_depth_ != 0) {
        visible_before_capture_ = true;
        return;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(
        hwnd_,
        topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ensure_visible();
}

bool PinWindow::hidden() const noexcept {
    return lifecycle_ == PinLifecycleState::hidden;
}

bool PinWindow::click_through() const noexcept {
    return hwnd_ &&
           IsWindow(hwnd_) &&
           (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) &
            WS_EX_TRANSPARENT) != 0;
}

bool PinWindow::set_click_through(bool enabled) noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return false;
    }
    if (enabled && !click_through_available_) {
        return false;
    }
    return update_window_presentation(
        alpha_,
        enabled,
        source_has_transparency_);
}

void PinWindow::set_click_through_available(bool available) noexcept {
    click_through_available_ = available;
    if (!available && click_through()) {
        (void)set_click_through(false);
    }
}

bool PinWindow::topmost() const noexcept {
    return hwnd_ && IsWindow(hwnd_) && topmost_;
}

bool PinWindow::set_topmost(bool enabled) noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return false;
    }
    if (!SetWindowPos(
            hwnd_,
            enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        return false;
    }
    topmost_ = enabled;
    return true;
}

void PinWindow::suspend_for_capture() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return;
    }
    if (capture_suspend_depth_++ == 0) {
        visible_before_capture_ = IsWindowVisible(hwnd_) == TRUE;
        if (visible_before_capture_) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }
}

void PinWindow::resume_after_capture() noexcept {
    if (capture_suspend_depth_ == 0) {
        return;
    }
    --capture_suspend_depth_;
    if (capture_suspend_depth_ == 0) {
        const bool restore = should_restore_pin_after_capture(
            lifecycle_,
            visible_before_capture_);
        visible_before_capture_ = false;
        if (restore && hwnd_ && IsWindow(hwnd_)) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetWindowPos(
                hwnd_,
                topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

bool PinWindow::set_alpha(int alpha) noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return false;
    }
    const int candidate = std::clamp(alpha, 30, 255);
    if (!update_window_presentation(
            candidate,
            click_through(),
            source_has_transparency_)) {
        return false;
    }
    alpha_ = candidate;
    return true;
}

void PinWindow::zoom_by_steps(double steps) {
    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect)) {
        return;
    }
    const POINT center{
        rect.left + (rect.right - rect.left) / 2,
        rect.top + (rect.bottom - rect.top) / 2,
    };
    (void)resize_to_scale(scale_ * std::pow(1.1, steps), center);
}

void PinWindow::toggle_smooth_scaling() noexcept {
    const bool previous = smooth_scaling_;
    smooth_scaling_ = !smooth_scaling_;
    if (hwnd_ && IsWindow(hwnd_) &&
        !refresh_window_presentation()) {
        smooth_scaling_ = previous;
        (void)refresh_window_presentation();
    }
}

Bitmap PinWindow::visible_bitmap() const {
    return render_pin_visual_bitmap(bitmap_, visual_effects_);
}

void PinWindow::toggle_visual_effect(
    PinVisualEffectAction action) {
    const PinVisualEffects previous = visual_effects_;
    visual_effects_ = transition_pin_visual_effects(
        visual_effects_,
        action);
    if (!rebuild_native_bitmap() ||
        !refresh_window_presentation()) {
        visual_effects_ = previous;
        (void)rebuild_native_bitmap();
        (void)refresh_window_presentation();
        show_message(
            L"无法切换贴图显示效果：内存或 GDI 资源不足。",
            MB_OK | MB_ICONERROR);
        return;
    }
}

void PinWindow::set_scale_percent(int percent) {
    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect)) {
        return;
    }
    const POINT center{
        rect.left + (rect.right - rect.left) / 2,
        rect.top + (rect.bottom - rect.top) / 2,
    };
    (void)resize_to_scale(
        pin_scale_factor_from_percent(percent),
        center);
}

void PinWindow::notify_click_through_enabled() const noexcept {
    if (owner_ && IsWindow(owner_)) {
        PostMessageW(owner_, WM_PIN_CLICK_THROUGH_ENABLED, 0, 0);
    }
}

void PinWindow::ensure_visible() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        return;
    }
    RECT native_window{};
    if (!GetWindowRect(hwnd_, &native_window)) {
        return;
    }
    const HMONITOR monitor =
        MonitorFromRect(
            &native_window,
            MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const RectI current =
        RectI::from_native(native_window);
    const RectI recovered =
        recover_pin_bounds(
            current,
            RectI::from_native(info.rcWork));
    if (recovered.left != current.left ||
        recovered.top != current.top) {
        SetWindowPos(
            hwnd_,
            nullptr,
            recovered.left,
            recovered.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOACTIVATE);
    }
}

bool PinWindow::resize_to_scale(
    double scale,
    POINT anchor_screen) noexcept {
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !std::isfinite(scale)) {
        return false;
    }
    const double candidate_scale =
        std::clamp(scale, 0.02, 20.0);
    const auto candidate_size = scaled_size(candidate_scale);
    RECT window_rect{};
    if (!candidate_size ||
        !GetWindowRect(hwnd_, &window_rect)) {
        return false;
    }

    const int current_width =
        window_rect.right - window_rect.left;
    const int current_height =
        window_rect.bottom - window_rect.top;
    const double ratio_x =
        current_width > 0
            ? std::clamp(
                  static_cast<double>(
                      anchor_screen.x - window_rect.left) /
                      current_width,
                  0.0,
                  1.0)
            : 0.5;
    const double ratio_y =
        current_height > 0
            ? std::clamp(
                  static_cast<double>(
                      anchor_screen.y - window_rect.top) /
                      current_height,
                  0.0,
                  1.0)
            : 0.5;

    const long long new_left =
        static_cast<long long>(anchor_screen.x) -
        static_cast<long long>(
            std::llround(ratio_x * candidate_size->cx));
    const long long new_top =
        static_cast<long long>(anchor_screen.y) -
        static_cast<long long>(
            std::llround(ratio_y * candidate_size->cy));
    const int clamped_left = static_cast<int>(
        std::clamp<long long>(
            new_left,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
    const int clamped_top = static_cast<int>(
        std::clamp<long long>(
            new_top,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
    if (!SetWindowPos(
            hwnd_,
            nullptr,
            clamped_left,
            clamped_top,
            candidate_size->cx,
            candidate_size->cy,
            SWP_NOZORDER | SWP_NOACTIVATE)) {
        return false;
    }
    if (!refresh_window_presentation()) {
        (void)SetWindowPos(
            hwnd_,
            nullptr,
            window_rect.left,
            window_rect.top,
            current_width,
            current_height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        (void)refresh_window_presentation();
        return false;
    }
    scale_ = candidate_scale;
    ensure_visible();
    return true;
}

void PinWindow::fit_to_work_area() {
    if (!hwnd_ || !IsWindow(hwnd_) || !bitmap_.valid()) {
        return;
    }
    RECT window_rect{};
    if (!GetWindowRect(hwnd_, &window_rect)) {
        return;
    }
    const POINT center{
        window_rect.left +
            (window_rect.right - window_rect.left) / 2,
        window_rect.top +
            (window_rect.bottom - window_rect.top) / 2,
    };
    const HMONITOR monitor =
        MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return;
    }
    (void)resize_to_scale(
        fit_pin_scale(
            bitmap_.width,
            bitmap_.height,
            RectI::from_native(info.rcWork)),
        center);
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
            // A hidden pin remains recoverable from the tray. Destruction is
            // deliberately reserved for Esc or the explicit menu command.
            request_hide();
            return 0;

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                (void)set_alpha(alpha_ + (delta > 0 ? 15 : -15));
                return 0;
            }
            POINT cursor_screen{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            const double wheel_steps =
                static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA);
            (void)resize_to_scale(
                scale_ * std::pow(1.1, wheel_steps),
                cursor_screen);
            return 0;
        }

        case WM_KEYDOWN: {
            const bool control =
                (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                std::wstring error;
                Bitmap visible = visible_bitmap();
                if (visible.empty()) {
                    error = L"无法生成当前可见贴图效果。";
                } else if (!copy_bitmap_to_clipboard(visible, &error)) {
                    if (error.empty()) {
                        error = L"无法复制当前可见贴图效果。";
                    }
                }
                if (!error.empty()) {
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
            } else if (wparam == 'T') {
                (void)set_topmost(!topmost());
                return 0;
            } else if (wparam == 'S' && !control) {
                toggle_smooth_scaling();
                return 0;
            } else if (wparam == 'G' && !control) {
                toggle_visual_effect(
                    PinVisualEffectAction::toggle_grayscale);
                return 0;
            } else if (wparam == 'I' && !control) {
                toggle_visual_effect(
                    PinVisualEffectAction::toggle_inverted);
                return 0;
            } else if (wparam == '0' && control) {
                (void)set_alpha(255);
                return 0;
            } else if (wparam == '0') {
                fit_to_work_area();
                return 0;
            } else if (wparam == '1') {
                set_scale_percent(100);
                return 0;
            } else if (wparam == VK_ADD || wparam == VK_OEM_PLUS) {
                if (control) {
                    (void)set_alpha(alpha_ + 15);
                } else {
                    zoom_by_steps(1.0);
                }
                return 0;
            } else if (wparam == VK_SUBTRACT || wparam == VK_OEM_MINUS) {
                if (control) {
                    (void)set_alpha(alpha_ - 15);
                } else {
                    zoom_by_steps(-1.0);
                }
                return 0;
            }
            break;
        }

        case WM_DROPFILES:
            replace_from_drop(reinterpret_cast<HDROP>(wparam));
            return 0;

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

        case WM_DISPLAYCHANGE:
            ensure_visible();
            return 0;

        case WM_DPICHANGED: {
            const auto* suggested =
                reinterpret_cast<const RECT*>(lparam);
            if (suggested) {
                SetWindowPos(
                    hwnd_,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    0,
                    0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ensure_visible();
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) {
                ensure_visible();
            }
            break;

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

    if (per_pixel_presentation_active_) {
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
            SetStretchBltMode(
                hdc,
                smooth_scaling_ ? HALFTONE : COLORONCOLOR);
            if (smooth_scaling_) {
                SetBrushOrgEx(hdc, 0, 0, nullptr);
            }

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
    AppendMenuW(
        menu.get(),
        MF_STRING,
        1,
        L"复制当前可见效果\tCtrl+C");
    AppendMenuW(menu.get(), MF_STRING, 2, L"保存当前可见效果…");
    AppendMenuW(
        menu.get(),
        MF_STRING |
            (topmost() ? MF_CHECKED : MF_UNCHECKED),
        9,
        L"始终置顶 (Always on top)\tT");
    AppendMenuW(
        menu.get(),
        MF_STRING |
            (click_through() ? MF_CHECKED : MF_UNCHECKED) |
            (click_through_available_ ? MF_ENABLED : MF_GRAYED),
        4,
        click_through_available_
            ? L"鼠标穿透 (Click-through)"
            : L"鼠标穿透（需显示托盘图标）");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    const std::wstring scale_status = std::format(
        L"当前缩放：{}%",
        static_cast<int>(std::llround(scale_ * 100.0)));
    const std::wstring alpha_status = std::format(
        L"当前透明度：{}%",
        static_cast<int>(std::llround(alpha_ * 100.0 / 255.0)));
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 20, scale_status.c_str());
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 21, alpha_status.c_str());
    AppendMenuW(menu.get(), MF_STRING, 15, L"放大\t+");
    AppendMenuW(menu.get(), MF_STRING, 16, L"缩小\t-");
    const int rounded_scale = static_cast<int>(
        std::llround(scale_ * 100.0));
    AppendMenuW(
        menu.get(),
        MF_STRING | (rounded_scale == 25 ? MF_CHECKED : MF_UNCHECKED),
        22,
        L"缩放 25%");
    AppendMenuW(
        menu.get(),
        MF_STRING | (rounded_scale == 50 ? MF_CHECKED : MF_UNCHECKED),
        23,
        L"缩放 50%");
    AppendMenuW(
        menu.get(),
        MF_STRING | (rounded_scale == 100 ? MF_CHECKED : MF_UNCHECKED),
        10,
        L"缩放 100%\t1");
    AppendMenuW(
        menu.get(),
        MF_STRING | (rounded_scale == 200 ? MF_CHECKED : MF_UNCHECKED),
        24,
        L"缩放 200%");
    AppendMenuW(menu.get(), MF_STRING, 25, L"输入缩放百分比…");
    AppendMenuW(menu.get(), MF_STRING, 11, L"适应屏幕\t0");
    AppendMenuW(menu.get(), MF_STRING, 14, L"透明度重置 100%\tCtrl+0");
    AppendMenuW(
        menu.get(),
        MF_STRING | (smooth_scaling_ ? MF_CHECKED : MF_UNCHECKED),
        13,
        smooth_scaling_
            ? L"平滑缩放：开\tS"
            : L"平滑缩放：关（像素）\tS");
    AppendMenuW(
        menu.get(),
        MF_STRING |
            (visual_effects_.grayscale ? MF_CHECKED : MF_UNCHECKED),
        17,
        L"灰度显示\tG");
    AppendMenuW(
        menu.get(),
        MF_STRING |
            (visual_effects_.inverted ? MF_CHECKED : MF_UNCHECKED),
        18,
        L"反色显示\tI");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 5, L"顺时针旋转 90° (Rotate 90° CW)\tR");
    AppendMenuW(menu.get(), MF_STRING, 6, L"逆时针旋转 90° (Rotate 90° CCW)\tL");
    AppendMenuW(menu.get(), MF_STRING, 7, L"水平翻转 (Flip Horizontal)\tH");
    AppendMenuW(menu.get(), MF_STRING, 8, L"垂直翻转 (Flip Vertical)\tV");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 12, L"隐藏（可从托盘恢复）\t双击");
    AppendMenuW(menu.get(), MF_STRING, 3, L"销毁\tEsc");

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
        Bitmap visible = visible_bitmap();
        if (visible.empty()) {
            error = L"无法生成当前可见贴图效果。";
        } else if (!copy_bitmap_to_clipboard(visible, &error) &&
                   error.empty()) {
            error = L"无法复制当前可见贴图效果。";
        }
        if (!error.empty()) {
            show_message(error, MB_OK | MB_ICONERROR);
        }
    } else if (selection == 2) {
        std::optional<std::filesystem::path> path = prompt_png_path(hwnd_);
        if (!close_pending_ && path) {
            std::wstring error;
            Bitmap visible = visible_bitmap();
            if (visible.empty()) {
                error = L"无法生成当前可见贴图效果。";
            } else if (!save_png(visible, *path, &error) &&
                       error.empty()) {
                error = L"无法保存当前可见贴图效果。";
            }
            if (!error.empty()) {
                show_message(error, MB_OK | MB_ICONERROR);
            }
        }
    } else if (selection == 3) {
        request_close();
    } else if (selection == 12) {
        request_hide();
    } else if (selection == 4) {
        const bool enabling = !click_through();
        if (!set_click_through(enabling)) {
            show_message(
                L"无法更改贴图的鼠标穿透状态。",
                MB_OK | MB_ICONERROR);
        } else if (enabling) {
            notify_click_through_enabled();
        }
    } else if (selection == 9) {
        if (!set_topmost(!topmost())) {
            show_message(
                L"无法更改贴图的置顶状态。",
                MB_OK | MB_ICONERROR);
        }
    } else if (selection == 10) {
        set_scale_percent(100);
    } else if (selection == 11) {
        fit_to_work_area();
    } else if (selection == 13) {
        toggle_smooth_scaling();
    } else if (selection == 14) {
        (void)set_alpha(255);
    } else if (selection == 15) {
        zoom_by_steps(1.0);
    } else if (selection == 16) {
        zoom_by_steps(-1.0);
    } else if (selection == 17) {
        toggle_visual_effect(
            PinVisualEffectAction::toggle_grayscale);
    } else if (selection == 18) {
        toggle_visual_effect(
            PinVisualEffectAction::toggle_inverted);
    } else if (selection == 22) {
        set_scale_percent(25);
    } else if (selection == 23) {
        set_scale_percent(50);
    } else if (selection == 24) {
        set_scale_percent(200);
    } else if (selection == 25) {
        const PinScalePromptPlan prompt_plan = plan_pin_scale_prompt(
            prompt_pin_scale_percent(
                hwnd_,
                pin_scale_percent_from_factor(scale_)));
        if (prompt_plan.apply) {
            set_scale_percent(prompt_plan.percent);
        }
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

void PinWindow::replace_from_drop(HDROP drop) {
    DropFilesScope drop_scope(drop);
    if (!drop) {
        return;
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    const UINT length =
        count > 0 ? DragQueryFileW(drop, 0, nullptr, 0) : 0;
    std::filesystem::path path;
    if (length > 0 && length < 32768) {
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(
                drop,
                0,
                value.data(),
                static_cast<UINT>(value.size())) == length) {
            value.resize(length);
            path = std::move(value);
        }
    }
    if (path.empty()) {
        show_message(L"拖入内容不包含可读取的本地图片文件。", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring error;
    auto replacement = decode_local_image_file(path, &error);
    if (!replacement) {
        show_message(
            error.empty() ? L"无法读取拖入的图片文件。" : error,
            MB_OK | MB_ICONERROR);
        return;
    }
    if (replacement_guard_) {
        if (auto budget_error = replacement_guard_(
                bitmap_.pixels.size(), *replacement)) {
            show_message(*budget_error, MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (!replace_bitmap(std::move(*replacement))) {
        show_message(
            L"无法替换贴图：内存或 GDI 资源不足，原贴图已保留。",
            MB_OK | MB_ICONERROR);
        return;
    }
    fit_to_work_area();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PinWindow::rotate(bool cw) {
    if (bitmap_.empty()) return;

    Bitmap transformed = cw ? rotate_90_cw(bitmap_) : rotate_90_ccw(bitmap_);
    if (transformed.empty() || !replace_bitmap(std::move(transformed))) {
        show_message(L"无法旋转贴图：内存或 GDI 资源不足。",
                     MB_OK | MB_ICONERROR);
        return;
    }
    RECT rect_window{};
    if (GetWindowRect(hwnd_, &rect_window)) {
        const POINT anchor{rect_window.left, rect_window.top};
        if (!resize_to_scale(scale_, anchor)) {
            show_message(
                L"贴图已旋转，但无法按新尺寸刷新透明显示。",
                MB_OK | MB_ICONWARNING);
        }
    }
    ensure_visible();
}

void PinWindow::flip(bool horizontal) {
    if (bitmap_.empty()) return;

    Bitmap transformed = horizontal
                             ? flip_horizontal(bitmap_)
                             : flip_vertical(bitmap_);
    if (transformed.empty() || !replace_bitmap(std::move(transformed))) {
        show_message(
            L"无法翻转贴图：内存或 GDI 资源不足。",
            MB_OK | MB_ICONERROR);
        return;
    }
}

} // namespace airshot
