#include "airshot/capture.h"

#include "capture_modern.h"

#include <dwmapi.h>

#include <cstring>
#include <cwctype>
#include <limits>
#include <tuple>

namespace airshot {
namespace {

struct CheckedRect {
    RectI rect;
    int width{};
    int height{};
};

[[nodiscard]] std::optional<CheckedRect> normalize_checked(const RectI& rect) noexcept {
    const std::int64_t left = std::min<std::int64_t>(rect.left, rect.right);
    const std::int64_t top = std::min<std::int64_t>(rect.top, rect.bottom);
    const std::int64_t right = std::max<std::int64_t>(rect.left, rect.right);
    const std::int64_t bottom = std::max<std::int64_t>(rect.top, rect.bottom);
    const std::int64_t width = right - left;
    const std::int64_t height = bottom - top;
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return CheckedRect{
        RectI{
            static_cast<int>(left),
            static_cast<int>(top),
            static_cast<int>(right),
            static_cast<int>(bottom),
        },
        static_cast<int>(width),
        static_cast<int>(height),
    };
}

[[nodiscard]] std::optional<RectI> intersect_checked(const RectI& first, const RectI& second) noexcept {
    const RectI value{
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom),
    };
    if (value.left >= value.right || value.top >= value.bottom) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<int> checked_difference(int first, int second) noexcept {
    const std::int64_t value = static_cast<std::int64_t>(first) - second;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

[[nodiscard]] bool bitmap_matches_rect(
    const Bitmap& bitmap,
    const RectI& rect) noexcept {
    return bitmap.valid() &&
           bitmap.width == rect.width() &&
           bitmap.height == rect.height();
}

class ScreenDc {
public:
    explicit ScreenDc(HWND window) noexcept : window_(window), value_(GetDC(window)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(window_, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HWND window_{};
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

class OwnedBitmap {
public:
    explicit OwnedBitmap(HBITMAP value = nullptr) noexcept : value_(value) {}
    ~OwnedBitmap() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedBitmap(const OwnedBitmap&) = delete;
    OwnedBitmap& operator=(const OwnedBitmap&) = delete;

    [[nodiscard]] HBITMAP get() const noexcept { return value_; }

private:
    HBITMAP value_{};
};

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) noexcept : dc_(dc), previous_(SelectObject(dc, object)) {}
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

[[nodiscard]] Bitmap capture_with_gdi(const RectI& requested_rect) {
    const auto checked = normalize_checked(requested_rect);
    if (!checked) {
        return {};
    }

    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    ScreenDc screen_dc(nullptr);
    if (!screen_dc.get()) {
        return {};
    }
    MemoryDc memory_dc(screen_dc.get());
    if (!memory_dc.get()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(result.pixels.size());

    void* bits = nullptr;
    OwnedBitmap dib(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib.get() || !bits) {
        return {};
    }
    SelectedObject selection(memory_dc.get(), dib.get());
    if (!selection.valid()) {
        return {};
    }

    if (!BitBlt(memory_dc.get(),
                0,
                0,
                result.width,
                result.height,
                screen_dc.get(),
                checked->rect.left,
                checked->rect.top,
                SRCCOPY | CAPTUREBLT) ||
        !GdiFlush()) {
        return {};
    }

    std::memcpy(result.pixels.data(), bits, result.pixels.size());
    result.make_opaque();
    return result;
}

[[nodiscard]] std::optional<RectI> virtual_desktop_bounds() noexcept {
    const std::int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const std::int64_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const std::int64_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const std::int64_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const std::int64_t right = left + width;
    const std::int64_t bottom = top + height;
    if (width <= 0 || height <= 0 ||
        right < std::numeric_limits<int>::min() || right > std::numeric_limits<int>::max() ||
        bottom < std::numeric_limits<int>::min() || bottom > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return RectI{
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(right),
        static_cast<int>(bottom),
    };
}

[[nodiscard]] std::optional<RectI> window_bounds(HWND window) {
    if (!window) {
        return std::nullopt;
    }
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
        !GetWindowRect(window, &rect)) {
        return std::nullopt;
    }
    const RectI result = RectI::from_native(rect);
    return normalize_checked(result) ? std::optional(result) : std::nullopt;
}

BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto* monitors = reinterpret_cast<std::vector<MonitorSnapshot>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) {
        return TRUE;
    }

    MonitorSnapshot snapshot;
    snapshot.handle = monitor;
    snapshot.bounds = RectI::from_native(*rect);
    snapshot.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    snapshot.device_name = info.szDevice;
    monitors->push_back(std::move(snapshot));
    return TRUE;
}

[[nodiscard]] std::vector<MonitorSnapshot> enumerate_monitors() {
    std::vector<MonitorSnapshot> result;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&result))) {
        return {};
    }
    std::ranges::sort(result, [](const MonitorSnapshot& first, const MonitorSnapshot& second) {
        return std::tie(first.bounds.left, first.bounds.top, first.device_name) <
               std::tie(second.bounds.left, second.bounds.top, second.device_name);
    });
    return result;
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
    if (!bounds) {
        return TRUE;
    }
    const auto checked = normalize_checked(*bounds);
    if (!checked || checked->width <= 6 || checked->height <= 6) {
        return TRUE;
    }
    auto* windows = reinterpret_cast<std::vector<WindowCandidate>*>(context);
    windows->push_back({window, *bounds});
    return TRUE;
}

[[nodiscard]] Bitmap capture_from_monitors(const std::vector<MonitorSnapshot>& monitors,
                                           const RectI& selection) {
    const auto checked = normalize_checked(selection);
    if (!checked) {
        return {};
    }
    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    for (const auto& monitor : monitors) {
        const auto overlap = intersect_checked(monitor.bounds, checked->rect);
        if (!overlap) {
            continue;
        }
        Bitmap captured = capture_with_gdi(*overlap);
        if (captured.empty()) {
            return {};
        }
        const auto target_x = checked_difference(overlap->left, checked->rect.left);
        const auto target_y = checked_difference(overlap->top, checked->rect.top);
        if (!target_x || !target_y) {
            return {};
        }
        const RectI source_rect{0, 0, captured.width, captured.height};
        const POINT target_origin{*target_x, *target_y};
        blit(captured, source_rect, result, target_origin);
    }
    return result;
}

}  // namespace

std::vector<MonitorSnapshot> capture_monitors() {
    auto result = enumerate_monitors();
    for (auto& monitor : result) {
        monitor.bitmap =
            capture_detail::capture_monitor_modern(monitor.handle);
        if (!bitmap_matches_rect(
                monitor.bitmap,
                monitor.bounds)) {
            monitor.bitmap = capture_with_gdi(monitor.bounds);
        }
    }
    return result;
}

std::vector<WindowCandidate> enumerate_window_candidates() {
    std::vector<WindowCandidate> result;
    if (!EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&result))) {
        return {};
    }
    return result;
}

Bitmap capture_rect(const RectI& rect) {
    const auto monitors = enumerate_monitors();
    return monitors.empty() ? Bitmap{} : capture_from_monitors(monitors, rect);
}

Bitmap capture_virtual_desktop() {
    const auto bounds = virtual_desktop_bounds();
    auto monitors = enumerate_monitors();
    if (!bounds || monitors.empty()) {
        return {};
    }
    for (auto& monitor : monitors) {
        monitor.bitmap =
            capture_detail::capture_monitor_modern(monitor.handle);
        if (!bitmap_matches_rect(
                monitor.bitmap,
                monitor.bounds)) {
            monitor.bitmap = capture_with_gdi(monitor.bounds);
        }
    }
    return compose_selection(monitors, *bounds);
}

std::optional<std::pair<Bitmap, RectI>> capture_active_window() {
    const HWND window = GetForegroundWindow();
    auto bounds = window_bounds(window);
    if (!bounds) {
        return std::nullopt;
    }
    Bitmap bitmap = capture_detail::capture_window_modern(window);
    if (!bitmap.empty() &&
        (bitmap.width != bounds->width() ||
         bitmap.height != bounds->height())) {
        // WGC follows the compositor's capture item size, which can differ
        // from DWM's extended-frame rectangle by invisible resize borders.
        // Keep the returned coordinate space aligned with the pixels.
        bounds->right = bounds->left + bitmap.width;
        bounds->bottom = bounds->top + bitmap.height;
    }
    if (bitmap.empty()) {
        bitmap = capture_rect(*bounds);
    }
    if (bitmap.empty()) {
        return std::nullopt;
    }
    return std::pair{std::move(bitmap), *bounds};
}

std::optional<std::pair<Bitmap, RectI>> capture_monitor(std::wstring_view selector) {
    if (selector.empty() || _wcsicmp(std::wstring(selector).c_str(), L"all") == 0) {
        const auto bounds = virtual_desktop_bounds();
        if (!bounds) {
            return std::nullopt;
        }
        Bitmap bitmap = capture_virtual_desktop();
        return bitmap.empty() ? std::nullopt : std::optional(std::pair{std::move(bitmap), *bounds});
    }

    auto monitors = enumerate_monitors();
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
        if (!GetCursorPos(&cursor)) {
            return std::nullopt;
        }
        const HMONITOR target = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const auto found =
            std::ranges::find_if(monitors, [target](const auto& monitor) { return monitor.handle == target; });
        index = found == monitors.end() ? 0U : static_cast<std::size_t>(std::distance(monitors.begin(), found));
    } else {
        try {
            std::size_t parsed_characters = 0;
            const unsigned long parsed = std::stoul(value, &parsed_characters);
            if (parsed_characters != value.size()) {
                return std::nullopt;
            }
            index = static_cast<std::size_t>(parsed);
        } catch (...) {
            return std::nullopt;
        }
        if (index >= monitors.size()) {
            return std::nullopt;
        }
    }

    Bitmap bitmap =
        capture_detail::capture_monitor_modern(monitors[index].handle);
    if (!bitmap_matches_rect(
            bitmap,
            monitors[index].bounds)) {
        bitmap = capture_with_gdi(monitors[index].bounds);
    }
    return bitmap.empty() ? std::nullopt
                          : std::optional(std::pair{std::move(bitmap), monitors[index].bounds});
}

Bitmap compose_selection(const std::vector<MonitorSnapshot>& monitors, const RectI& selection) {
    const auto checked = normalize_checked(selection);
    if (!checked) {
        return {};
    }
    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    for (const auto& monitor : monitors) {
        const auto overlap = intersect_checked(monitor.bounds, checked->rect);
        if (!overlap || monitor.bitmap.empty()) {
            continue;
        }
        const auto source_left = checked_difference(overlap->left, monitor.bounds.left);
        const auto source_top = checked_difference(overlap->top, monitor.bounds.top);
        const auto source_right = checked_difference(overlap->right, monitor.bounds.left);
        const auto source_bottom = checked_difference(overlap->bottom, monitor.bounds.top);
        const auto target_x = checked_difference(overlap->left, checked->rect.left);
        const auto target_y = checked_difference(overlap->top, checked->rect.top);
        if (!source_left || !source_top || !source_right || !source_bottom || !target_x || !target_y) {
            continue;
        }
        const RectI source_rect{
            *source_left,
            *source_top,
            *source_right,
            *source_bottom,
        };
        const POINT target_origin{*target_x, *target_y};
        blit(monitor.bitmap, source_rect, result, target_origin);
    }
    return result;
}

}  // namespace airshot
