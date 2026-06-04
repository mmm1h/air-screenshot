#include "about_window.h"

#include "resource.h"

#include "airshot/common.h"

#include <mutex>

namespace airshot {
namespace {

constexpr int kCloseButton = 1;

std::wstring resource_text(int identifier) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (!resource) {
        return {};
    }
    HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD size = SizeofResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    return bytes && size ? from_utf8(std::string_view(static_cast<const char*>(bytes), size)) : std::wstring{};
}

LRESULT CALLBACK about_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_CREATE) {
        const std::wstring text =
            std::format(L"Air Screenshot {}\r\n"
                        L"https://github.com/mmm1h/air-screenshot\r\n\r\n"
                        L"LICENSE\r\n=======\r\n{}\r\n\r\n"
                        L"THIRD-PARTY NOTICES\r\n===================\r\n{}",
                        from_utf8(AIRSHOT_VERSION),
                        resource_text(IDR_LICENSE_TEXT),
                        resource_text(IDR_THIRD_PARTY_TEXT));
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                    L"EDIT",
                                    L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                        ES_READONLY,
                                    16,
                                    16,
                                    712,
                                    500,
                                    window,
                                    nullptr,
                                    nullptr,
                                    nullptr);
        SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(text.size() + 1), 0);
        SetWindowTextW(edit, text.c_str());
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        HWND close = CreateWindowExW(0,
                                     L"BUTTON",
                                     L"关闭",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     638,
                                     530,
                                     90,
                                     32,
                                     window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)),
                                     nullptr,
                                     nullptr);
        SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(w_param) == kCloseButton) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

void show_about_window(HWND owner) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = about_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"AirScreenshot.About";
        RegisterClassExW(&window_class);
    });

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.About",
                                  L"关于 Air Screenshot / 许可证",
                                  WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  760,
                                  610,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr);
    if (!window) {
        return;
    }
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

}  // namespace airshot
