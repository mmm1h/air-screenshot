#include "settings_window.h"

#include "airshot/config.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <string_view>

namespace {

void open_app_page(HWND window) {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) {
        return;
    }
    const float scale = std::min(
        static_cast<float>(client.right - client.left) / 920.0f,
        static_cast<float>(client.bottom - client.top) / 720.0f);
    const int x = static_cast<int>(100.0f * scale);
    const int y = static_cast<int>((96.0f + 4.0f * 48.0f + 20.0f) * scale);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    airshot::AppConfig config;
    const std::wstring_view command_line(GetCommandLineW());
    if (command_line.find(L"--dark") != std::wstring_view::npos) {
        config.theme = L"dark";
    } else if (command_line.find(L"--light") != std::wstring_view::npos) {
        config.theme = L"light";
    }
    if (command_line.find(L"--flow") != std::wstring_view::npos) {
        config.app_icon = std::wstring(airshot::kAppIconFlowLens);
    } else if (command_line.find(L"--pixel") != std::wstring_view::npos) {
        config.app_icon = std::wstring(airshot::kAppIconPixelConsole);
    }

    bool completed = false;
    HWND window = airshot::show_settings_window_async(
        nullptr,
        config,
        [&](std::optional<airshot::AppConfig>) { completed = true; });
    if (window) {
        open_app_page(window);
    }

    MSG message{};
    while (window && !completed) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (window && IsWindow(window)) {
        DestroyWindow(window);
    }
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return 0;
}
