#include "app_icon.h"
#include "settings_window_policy.h"

#include "airshot/config.h"

#include <windows.h>

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        std::wcerr << L"FAILED: " << message << L'\n';
        ++failures;
    }
}

SIZE icon_size(HICON icon) {
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon, &info)) {
        return {};
    }

    BITMAP bitmap{};
    const HBITMAP source = info.hbmColor ? info.hbmColor : info.hbmMask;
    if (!source || GetObjectW(source, sizeof(bitmap), &bitmap) != sizeof(bitmap)) {
        if (info.hbmColor) {
            DeleteObject(info.hbmColor);
        }
        if (info.hbmMask) {
            DeleteObject(info.hbmMask);
        }
        return {};
    }

    const SIZE result{
        bitmap.bmWidth,
        info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2,
    };
    if (info.hbmColor) {
        DeleteObject(info.hbmColor);
    }
    if (info.hbmMask) {
        DeleteObject(info.hbmMask);
    }
    return result;
}

void test_theme_refresh_policy() {
    using airshot::settings_detail::settings_theme_resources_need_refresh;

    expect(
        !settings_theme_resources_need_refresh(
            false, true, false, true, true, false),
        L"unchanged theme state reuses an existing render target");
    expect(
        settings_theme_resources_need_refresh(
            false, true, false, true, true, true),
        L"system color changes invalidate high-contrast brushes");
    expect(
        settings_theme_resources_need_refresh(
            false, false, true, false, true, false),
        L"light and dark preference changes invalidate brushes");
    expect(
        settings_theme_resources_need_refresh(
            false, false, false, true, true, false),
        L"high-contrast state changes invalidate brushes");
    expect(
        settings_theme_resources_need_refresh(
            false, false, false, false, false, false),
        L"missing render resources are recreated");
}

void test_automatic_update_draft_round_trip() {
    airshot::AppConfig saved;
    saved.automatic_updates_enabled = true;
    saved.last_update_check_unix = 1'725'000'000;
    saved.warned_update_target = L"C:\\Program Files\\Air Screenshot";

    airshot::AppConfig draft = saved;
    draft.automatic_updates_enabled = false;
    expect(
        saved.automatic_updates_enabled &&
            !draft.automatic_updates_enabled,
        L"automatic update preference remains a draft until settings are saved");

    const auto parsed = airshot::config_from_json(
        airshot::config_to_json(draft));
    expect(
        parsed && !parsed->automatic_updates_enabled &&
            parsed->last_update_check_unix == saved.last_update_check_unix &&
            parsed->warned_update_target == saved.warned_update_target,
        L"automatic update draft round-trips without changing update safety metadata");

    draft.automatic_updates_enabled = true;
    const auto enabled = airshot::config_from_json(
        airshot::config_to_json(draft));
    expect(
        enabled && enabled->automatic_updates_enabled,
        L"automatic update draft can be re-enabled before saving");
}

void test_dpi_specific_window_icon() {
    constexpr wchar_t class_name[] = L"AirScreenshot.SettingsIconTest";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassExW(&window_class);
    expect(atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
           L"icon test window class is available");

    HWND window = CreateWindowExW(
        0,
        class_name,
        L"",
        WS_OVERLAPPED,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        nullptr);
    expect(window != nullptr, L"icon test window is created");
    if (!window) {
        return;
    }

    airshot::apply_app_icon_to_window(
        window,
        airshot::kAppIconPixelConsole,
        96);
    const HICON icon_96 = reinterpret_cast<HICON>(
        SendMessageW(window, WM_GETICON, ICON_BIG, 0));
    const SIZE size_96 = icon_size(icon_96);
    expect(size_96.cx == 32 && size_96.cy == 32,
           L"96 DPI uses a 32 pixel large icon");

    airshot::apply_app_icon_to_window(
        window,
        airshot::kAppIconPixelConsole,
        192);
    const HICON icon_192 = reinterpret_cast<HICON>(
        SendMessageW(window, WM_GETICON, ICON_BIG, 0));
    const SIZE size_192 = icon_size(icon_192);
    expect(size_192.cx == 64 && size_192.cy == 64,
           L"192 DPI reloads a 64 pixel large icon");
    expect(
        icon_192 == airshot::load_app_icon(
                        instance,
                        airshot::kAppIconPixelConsole,
                        64,
                        64),
        L"DPI reload keeps the selected Pixel Console icon resource");

    DestroyWindow(window);
    if (atom != 0) {
        UnregisterClassW(class_name, instance);
    }
}

}  // namespace

int wmain() {
    test_theme_refresh_policy();
    test_automatic_update_draft_round_trip();
    test_dpi_specific_window_icon();
    if (failures == 0) {
        std::wcout << L"settings window tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
