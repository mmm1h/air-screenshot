#pragma once

#include <windows.h>

#include <string_view>

namespace airshot {

[[nodiscard]] int app_icon_resource_id(std::wstring_view icon_style) noexcept;
[[nodiscard]] HICON load_app_icon(
    HINSTANCE instance,
    std::wstring_view icon_style,
    int width = 0,
    int height = 0) noexcept;
void apply_app_icon_to_window(HWND window, std::wstring_view icon_style) noexcept;
void apply_app_icon_to_window(
    HWND window,
    std::wstring_view icon_style,
    UINT dpi) noexcept;

}  // namespace airshot
