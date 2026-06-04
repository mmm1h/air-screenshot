#include "airshot/capture.h"

#include <dwmapi.h>

#include <cstring>
#include <cwctype>

namespace airshot {
namespace {

Bitmap capture_with_gdi(const RectI& rect) {
    if (rect.empty()) {
        return {};
    }

    Bitmap result(rect.width(), rect.height());
    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return {};
    }
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return {};
    }

    HGDIOBJ previous = SelectObject(memory_dc, dib);
    const BOOL copied = BitBlt(memory_dc,
                               0,
                               0,
                               result.width,
                               result.height,
                               screen_dc,
                               rect.left,
                               rect.top,
                               SRCCOPY | CAPTUREBLT);
    if (copied) {
        std::memcpy(result.pixels.data(), bits, result.pixels.size());
    } else {
        result = {};
    }

    SelectObject(memory_dc, previous);
    DeleteObject(dib);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return result;
}

RectI virtual_desktop_bounds() {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {left,
            top,
            left + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

std::optional<RectI> window_bounds(HWND window) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
        !GetWindowRect(window, &rect)) {
        return std::nullopt;
    }
    RectI result = RectI::from_native(rect);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto* monitors = reinterpret_cast<std::vector<MonitorSnapshot>*>(context);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    MonitorSnapshot snapshot;
    snapshot.handle = monitor;
    snapshot.bounds = RectI::from_native(*rect);
    snapshot.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitors->push_back(std::move(snapshot));
    return TRUE;
}

BOOL CALLBACK collect_windows(HWND window, LPARAM context) {
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindowTextLengthW(window) <= 0) {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }
    const auto bounds = window_bounds(window);
    if (!bounds || bounds->width() <= 6 || bounds->height() <= 6) {
        return TRUE;
    }
    auto* windows = reinterpret_cast<std::vector<WindowCandidate>*>(context);
    windows->push_back({window, *bounds});
    return TRUE;
}

}  // namespace

std::vector<MonitorSnapshot> capture_monitors() {
    std::vector<MonitorSnapshot> result;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&result));
    for (auto& monitor : result) {
        monitor.bitmap = capture_with_gdi(monitor.bounds);
    }
    return result;
}

std::vector<WindowCandidate> enumerate_window_candidates() {
    std::vector<WindowCandidate> result;
    EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&result));
    return result;
}

Bitmap capture_rect(const RectI& rect) {
    return capture_with_gdi(rect.normalized());
}

Bitmap capture_virtual_desktop() {
    return capture_with_gdi(virtual_desktop_bounds());
}

std::optional<std::pair<Bitmap, RectI>> capture_active_window() {
    HWND window = GetForegroundWindow();
    const auto bounds = window_bounds(window);
    if (!window || !bounds) {
        return std::nullopt;
    }
    Bitmap bitmap = capture_with_gdi(*bounds);
    if (bitmap.empty()) {
        return std::nullopt;
    }
    return std::pair{std::move(bitmap), *bounds};
}

std::optional<std::pair<Bitmap, RectI>> capture_monitor(std::wstring_view selector) {
    if (selector.empty() || _wcsicmp(std::wstring(selector).c_str(), L"all") == 0) {
        RectI bounds = virtual_desktop_bounds();
        Bitmap bitmap = capture_with_gdi(bounds);
        return bitmap.empty() ? std::nullopt : std::optional(std::pair{std::move(bitmap), bounds});
    }

    std::vector<MonitorSnapshot> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        return std::nullopt;
    }

    std::size_t index = 0;
    const std::wstring value(selector);
    if (_wcsicmp(value.c_str(), L"primary") == 0) {
        const auto found = std::ranges::find_if(monitors, [](const auto& monitor) { return monitor.primary; });
        index = found == monitors.end() ? 0U : static_cast<std::size_t>(std::distance(monitors.begin(), found));
    } else if (_wcsicmp(value.c_str(), L"cursor") == 0) {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR target = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const auto found =
            std::ranges::find_if(monitors, [target](const auto& monitor) { return monitor.handle == target; });
        index = found == monitors.end() ? 0U : static_cast<std::size_t>(std::distance(monitors.begin(), found));
    } else {
        try {
            index = static_cast<std::size_t>(std::stoul(value));
        } catch (...) {
            return std::nullopt;
        }
        if (index >= monitors.size()) {
            return std::nullopt;
        }
    }

    Bitmap bitmap = capture_with_gdi(monitors[index].bounds);
    return bitmap.empty() ? std::nullopt
                          : std::optional(std::pair{std::move(bitmap), monitors[index].bounds});
}

Bitmap compose_selection(const std::vector<MonitorSnapshot>& monitors, const RectI& selection) {
    const RectI normalized = selection.normalized();
    Bitmap result(normalized.width(), normalized.height());
    for (const auto& monitor : monitors) {
        const auto overlap = intersect(monitor.bounds, normalized);
        if (!overlap || monitor.bitmap.empty()) {
            continue;
        }
        RectI source_rect{
            overlap->left - monitor.bounds.left,
            overlap->top - monitor.bounds.top,
            overlap->right - monitor.bounds.left,
            overlap->bottom - monitor.bounds.top,
        };
        POINT target_origin{overlap->left - normalized.left, overlap->top - normalized.top};
        blit(monitor.bitmap, source_rect, result, target_origin);
    }
    return result;
}

}  // namespace airshot
