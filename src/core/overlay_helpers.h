#pragma once

#include "airshot/bitmap.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace airshot::overlay_detail {

[[nodiscard]] COLORREF parse_hex_color(std::wstring_view hex, COLORREF fallback);
[[nodiscard]] std::wstring format_hex_color(COLORREF color);

void show_rgb_picker_popup(HWND parent_hwnd,
                           COLORREF initial_color,
                           const RectI& button_bounds,
                           const RectI& monitor_bounds,
                           std::function<void(COLORREF)> on_color_changed);

[[nodiscard]] std::optional<std::wstring> prompt_text(HWND owner, POINT position, COLORREF color, float text_size, bool is_light_theme);

struct ScrollControlState {
    bool finished{false};
    bool cancelled{false};
};

[[nodiscard]] HWND create_scroll_border_window(HINSTANCE instance, HWND parent, const RectI& bounds);
[[nodiscard]] HWND create_scroll_control_window(HINSTANCE instance, HWND parent, const RectI& selection, ScrollControlState* state);

[[nodiscard]] bool is_bitmap_static(const Bitmap& bmp1, const Bitmap& bmp2);
[[nodiscard]] int find_best_template_y(const Bitmap& frame, int direction);

struct ScrollResult {
    bool matched{false};
    int direction{0};
    int offset{0};
};

[[nodiscard]] ScrollResult detect_scroll(const Bitmap& last_frame, const Bitmap& new_frame, int locked_direction);
void append_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d);
void prepend_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d);

}  // namespace airshot::overlay_detail
