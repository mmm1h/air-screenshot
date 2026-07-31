#include "capture_modern.h"
#include "airshot/capture.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

constexpr wchar_t kTargetClass[] =
    L"AirshotCaptureTest.Target";
constexpr wchar_t kOccluderClass[] =
    L"AirshotCaptureTest.Occluder";
constexpr COLORREF kTargetColor = RGB(23, 117, 201);

LRESULT CALLBACK color_window_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        wchar_t class_name[64]{};
        GetClassNameW(
            window,
            class_name,
            static_cast<int>(std::size(class_name)));
        const COLORREF color =
            wcscmp(class_name, kTargetClass) == 0
                ? kTargetColor
                : RGB(4, 5, 6);
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(
        window,
        message,
        w_param,
        l_param);
}

bool register_test_class(
    HINSTANCE instance,
    const wchar_t* name) {
    WNDCLASSEXW window_class{
        sizeof(window_class)};
    window_class.lpfnWndProc = color_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor =
        LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = name;
    return RegisterClassExW(&window_class) != 0;
}

bool channel_close(
    std::uint8_t actual,
    std::uint8_t expected) {
    return std::abs(
               static_cast<int>(actual) -
               static_cast<int>(expected)) <= 6;
}

}  // namespace

int wmain() {
    const HINSTANCE instance =
        GetModuleHandleW(nullptr);
    if (!register_test_class(instance, kTargetClass) ||
        !register_test_class(instance, kOccluderClass)) {
        std::wcerr << L"Unable to register capture test windows.\n";
        return 1;
    }

    HWND target = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kTargetClass,
        L"Air Screenshot capture fixture",
        WS_POPUP,
        80,
        80,
        180,
        130,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!target) {
        return 1;
    }
    ShowWindow(target, SW_SHOWNOACTIVATE);
    UpdateWindow(target);

    HWND child = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE,
        12,
        14,
        72,
        44,
        target,
        nullptr,
        instance,
        nullptr);
    if (!child) {
        DestroyWindow(target);
        return 1;
    }
    UpdateWindow(child);

    const auto candidates =
        airshot::enumerate_window_candidates();
    const auto child_candidate = std::ranges::find_if(
        candidates,
        [child](const airshot::WindowCandidate& candidate) {
            return candidate.handle == child;
        });
    if (child_candidate == candidates.end() ||
        child_candidate->root != target ||
        child_candidate->parent != target ||
        child_candidate->depth < 1) {
        DestroyWindow(target);
        std::wcerr
            << L"Child HWND was not exposed for smart selection.\n";
        return 1;
    }

    HWND occluder = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kOccluderClass,
        L"Air Screenshot capture occluder",
        WS_POPUP,
        80,
        80,
        180,
        130,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!occluder) {
        DestroyWindow(target);
        return 1;
    }
    ShowWindow(occluder, SW_SHOWNOACTIVATE);
    UpdateWindow(occluder);
    DwmFlush();

    const airshot::Bitmap captured =
        airshot::capture_detail::
            capture_window_modern(target);

    DestroyWindow(occluder);
    DestroyWindow(target);
    UnregisterClassW(kOccluderClass, instance);
    UnregisterClassW(kTargetClass, instance);

    if (captured.empty()) {
        std::wcout
            << L"Modern capture is unavailable; fallback remains covered elsewhere.\n";
        return 77;
    }
    if (captured.width < 160 ||
        captured.height < 110) {
        std::wcerr << L"Captured window has an unexpected size.\n";
        return 1;
    }

    const auto row =
        captured.row(captured.height / 2);
    const std::size_t pixel =
        static_cast<std::size_t>(captured.width / 2) *
        airshot::Bitmap::bytes_per_pixel;
    if (!channel_close(
            row[pixel],
            static_cast<std::uint8_t>(201)) ||
        !channel_close(
            row[pixel + 1],
            static_cast<std::uint8_t>(117)) ||
        !channel_close(
            row[pixel + 2],
            static_cast<std::uint8_t>(23)) ||
        row[pixel + 3] != 255) {
        std::wcerr
            << L"Modern capture returned occluder or incorrect pixels.\n";
        return 1;
    }

    std::wcout
        << L"Modern capture test passed.\n";
    return 0;
}
