#pragma once

#include "airshot/bitmap.h"
#include "airshot/pin_workflow.h"
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <memory>

namespace airshot {

// Custom message posted to the owner when PinWindow is destroyed
constexpr UINT WM_PIN_WINDOW_CLOSED = WM_APP + 10;
constexpr UINT WM_PIN_CLICK_THROUGH_ENABLED = WM_APP + 12;

class PinWindow {
public:
    using ReplacementGuard = std::function<std::optional<std::wstring>(
        std::size_t current_bytes,
        const Bitmap& replacement)>;

    static void register_class(HINSTANCE instance);

    static std::unique_ptr<PinWindow> create(
        HINSTANCE instance,
        HWND parent,
        Bitmap bitmap,
        int x,
        int y,
        bool click_through_available = true,
        ReplacementGuard replacement_guard = {});

    PinWindow(HWND hwnd, Bitmap bitmap);
    ~PinWindow();

    HWND hwnd() const { return hwnd_; }
    [[nodiscard]] std::size_t bitmap_bytes() const noexcept {
        return bitmap_.pixels.size();
    }
    void request_close() noexcept;
    void request_hide() noexcept;
    void request_show() noexcept;
    [[nodiscard]] bool hidden() const noexcept;
    [[nodiscard]] bool click_through() const noexcept;
    [[nodiscard]] bool set_click_through(bool enabled) noexcept;
    void set_click_through_available(bool available) noexcept;
    [[nodiscard]] bool topmost() const noexcept;
    [[nodiscard]] bool set_topmost(bool enabled) noexcept;
    void suspend_for_capture() noexcept;
    void resume_after_capture() noexcept;
    void ensure_visible() noexcept;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);
    void enter_modal() noexcept;
    [[nodiscard]] bool leave_modal() noexcept;
    void show_message(std::wstring_view message, UINT flags);
    void paint();
    void show_context_menu(POINT screen_pos);
    void replace_from_drop(HDROP drop);
    void notify_click_through_enabled() const noexcept;
    [[nodiscard]] bool set_alpha(int alpha) noexcept;
    void zoom_by_steps(double steps);
    void toggle_smooth_scaling() noexcept;
    void toggle_visual_effect(PinVisualEffectAction action);
    [[nodiscard]] Bitmap visible_bitmap() const;
    void set_scale_percent(int percent);
    void rotate(bool cw);
    void flip(bool horizontal);
    [[nodiscard]] bool resize_to_scale(double scale, POINT anchor_screen) noexcept;
    void fit_to_work_area();
    [[nodiscard]] bool rebuild_native_bitmap();
    [[nodiscard]] bool replace_bitmap(Bitmap bitmap);
    [[nodiscard]] std::optional<SIZE> scaled_size(double scale) const noexcept;

    HWND hwnd_{};
    HWND owner_{};
    Bitmap bitmap_{};
    HBITMAP hbitmap_{};
    double scale_{1.0};
    int alpha_{255};
    bool smooth_scaling_{true};
    PinVisualEffects visual_effects_{};
    bool topmost_{true};
    bool click_through_available_{true};
    PinLifecycleState lifecycle_{PinLifecycleState::visible};
    ReplacementGuard replacement_guard_;
    unsigned int capture_suspend_depth_{};
    bool visible_before_capture_{};
    bool notify_owner_{};
    unsigned int modal_depth_{};
    bool close_pending_{};
};

} // namespace airshot
