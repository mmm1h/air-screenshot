#pragma once

#include "airshot/bitmap.h"
#include <windows.h>
#include <memory>

namespace airshot {

// Custom message posted to the owner when PinWindow is destroyed
constexpr UINT WM_PIN_WINDOW_CLOSED = WM_APP + 10;

class PinWindow {
public:
    static void register_class(HINSTANCE instance);

    static std::unique_ptr<PinWindow> create(HINSTANCE instance, HWND parent, const Bitmap& bitmap, int x, int y);

    PinWindow(HWND hwnd, const Bitmap& bitmap);
    ~PinWindow();

    HWND hwnd() const { return hwnd_; }
    void request_close() noexcept;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);
    void enter_modal() noexcept;
    [[nodiscard]] bool leave_modal() noexcept;
    void show_message(std::wstring_view message, UINT flags);
    void paint();
    void show_context_menu(POINT screen_pos);
    void rotate(bool cw);
    void flip(bool horizontal);
    [[nodiscard]] bool rebuild_native_bitmap();
    [[nodiscard]] bool replace_bitmap(Bitmap bitmap);
    [[nodiscard]] std::optional<SIZE> scaled_size(double scale) const noexcept;

    HWND hwnd_{};
    HWND owner_{};
    Bitmap bitmap_{};
    HBITMAP hbitmap_{};
    void* bitmap_bits_{};
    double scale_{1.0};
    int alpha_{255};
    bool notify_owner_{};
    unsigned int modal_depth_{};
    bool close_pending_{};
};

} // namespace airshot
