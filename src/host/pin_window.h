#pragma once

#include "airshot/bitmap.h"
#include "airshot/pin_lifecycle_policy.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace airshot {

// Custom message posted to the owner when PinWindow is destroyed. LPARAM is
// the stable PinWindow::identifier(), never the object's address.
constexpr UINT WM_PIN_WINDOW_CLOSED = WM_APP + 32;
constexpr UINT WM_PIN_CLICK_THROUGH_ENABLED = WM_APP + 33;

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
    [[nodiscard]] std::uint64_t identifier() const noexcept {
        return identifier_;
    }
    [[nodiscard]] std::uint64_t hidden_order() const noexcept {
        return state_.hidden_order;
    }
    void request_close() noexcept;
    void request_destroy_with_confirmation();
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
    [[nodiscard]] bool confirm_destroy();
    void show_message(std::wstring_view message, UINT flags);
    void paint();
    void show_context_menu(POINT screen_pos);
    void replace_from_clipboard();
    void replace_from_drop(HDROP drop);
    void notify_click_through_enabled() const noexcept;
    [[nodiscard]] bool set_alpha(int alpha) noexcept;
    void zoom_by_steps(double steps);
    void toggle_smooth_scaling() noexcept;
    void toggle_visual_effect(PinVisualEffectAction action);
    void toggle_thumbnail_mode();
    [[nodiscard]] Bitmap visible_bitmap() const;
    void set_scale_percent(int percent);
    void rotate(bool cw);
    void flip(bool horizontal);
    [[nodiscard]] bool resize_to_scale(
        double scale,
        POINT anchor_screen,
        bool preserve_thumbnail_mode = false) noexcept;
    [[nodiscard]] bool apply_resize_plan(
        const PinResizePlan& plan,
        bool preserve_thumbnail_mode = false) noexcept;
    void begin_border_resize(PinResizeEdge edge) noexcept;
    void update_border_resize() noexcept;
    void end_border_resize(bool release_capture) noexcept;
    [[nodiscard]] PinResizeEdge resize_edge_at(POINT screen_point) const noexcept;
    [[nodiscard]] std::optional<RectI> current_work_area() const noexcept;
    void fit_to_work_area();
    [[nodiscard]] bool rebuild_native_bitmap();
    [[nodiscard]] bool refresh_window_presentation() noexcept;
    [[nodiscard]] bool update_window_presentation(
        int alpha,
        bool click_through,
        bool source_has_transparency) noexcept;
    [[nodiscard]] bool replace_bitmap(Bitmap bitmap);
    [[nodiscard]] std::optional<SIZE> scaled_size(double scale) const noexcept;

    HWND hwnd_{};
    HWND owner_{};
    std::uint64_t identifier_{};
    Bitmap bitmap_{};
    HBITMAP hbitmap_{};
    PinRuntimeState state_{};
    bool source_has_transparency_{};
    bool per_pixel_presentation_active_{};
    PinThumbnailState thumbnail_state_{};
    PinResizeEdge border_resize_edge_{PinResizeEdge::none};
    RectI border_resize_start_bounds_{};
    RectI border_resize_work_area_{};
    bool border_resize_failed_{};
    bool click_through_available_{true};
    ReplacementGuard replacement_guard_;
    unsigned int capture_suspend_depth_{};
    bool visible_before_capture_{};
    bool notify_owner_{};
    unsigned int modal_depth_{};
    bool close_pending_{};
};

} // namespace airshot
