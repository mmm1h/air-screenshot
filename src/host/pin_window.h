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

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);
    void paint();
    void show_context_menu(POINT screen_pos);

    HWND hwnd_{};
    Bitmap bitmap_{};
    HBITMAP hbitmap_{};
    double scale_{1.0};
};

} // namespace airshot
