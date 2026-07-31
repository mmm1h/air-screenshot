#include "app_icon.h"

#include "airshot/config.h"

#include "resource.h"

#include <algorithm>

namespace airshot {

int app_icon_resource_id(std::wstring_view icon_style) noexcept {
    const std::wstring normalized = normalize_app_icon(icon_style);
    if (normalized == kAppIconFlowLens) {
        return IDI_APP_ICON_FLOW;
    }
    if (normalized == kAppIconPixelConsole) {
        return IDI_APP_ICON_PIXEL;
    }
    return IDI_APP_ICON;
}

HICON load_app_icon(
    HINSTANCE instance,
    std::wstring_view icon_style,
    int width,
    int height) noexcept {
    if (!instance) {
        instance = GetModuleHandleW(nullptr);
    }
    return static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(app_icon_resource_id(icon_style)),
        IMAGE_ICON,
        std::max(0, width),
        std::max(0, height),
        LR_SHARED | ((width <= 0 || height <= 0) ? LR_DEFAULTSIZE : 0)));
}

void apply_app_icon_to_window(
    HWND window,
    std::wstring_view icon_style,
    UINT dpi) noexcept {
    if (!window || !IsWindow(window)) {
        return;
    }
    if (dpi == 0) {
        dpi = 96;
    }
    const int large = std::max(16, MulDiv(32, static_cast<int>(dpi), 96));
    const int small = std::max(16, MulDiv(16, static_cast<int>(dpi), 96));
    SendMessageW(
        window,
        WM_SETICON,
        ICON_BIG,
        reinterpret_cast<LPARAM>(load_app_icon(nullptr, icon_style, large, large)));
    SendMessageW(
        window,
        WM_SETICON,
        ICON_SMALL,
        reinterpret_cast<LPARAM>(load_app_icon(nullptr, icon_style, small, small)));
}

void apply_app_icon_to_window(HWND window, std::wstring_view icon_style) noexcept {
    const UINT dpi = window && IsWindow(window) ? GetDpiForWindow(window) : 0;
    apply_app_icon_to_window(window, icon_style, dpi);
}

}  // namespace airshot
