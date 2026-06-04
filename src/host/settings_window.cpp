#include "settings_window.h"

#include "airshot/strings.h"

#include <commctrl.h>

#include <array>
#include <mutex>

namespace airshot {
namespace {

enum ControlId : int {
    annotation = 101,
    ocr = 102,
    shell = 103,
    startup = 104,
    global_ocr = 105,
    capture_hotkey = 106,
    global_ocr_hotkey = 107,
};

struct SettingsState {
    AppConfig config;
    bool accepted{};
    HWND window{};
    std::array<HWND, 7> controls{};
};

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void set_font(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND add_checkbox(HWND parent, int id, const wchar_t* text, int x, int y, bool checked) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   x,
                                   y,
                                   310,
                                   24,
                                   parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    set_font(control);
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return control;
}

HWND add_edit(HWND parent, int id, const wchar_t* label, int x, int y, std::wstring_view value) {
    HWND label_control =
        CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, x, y + 4, 125, 22, parent, nullptr, nullptr, nullptr);
    set_font(label_control);
    HWND control = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   L"EDIT",
                                   std::wstring(value).c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   x + 130,
                                   y,
                                   190,
                                   25,
                                   parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    set_font(control);
    return control;
}

bool checked(HWND control) {
    return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<SettingsState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_CREATE) {
        state->controls[0] =
            add_checkbox(window, annotation, strings::settings_annotation.data(), 24, 22, state->config.annotation_enabled);
        state->controls[1] =
            add_checkbox(window, ocr, strings::settings_ocr.data(), 24, 52, state->config.ocr_enabled);
        state->controls[2] =
            add_checkbox(window, shell, strings::settings_shell.data(), 24, 82, state->config.shell_enabled);
        state->controls[3] =
            add_checkbox(window, startup, strings::settings_startup.data(), 24, 112, state->config.start_at_login);
        state->controls[4] =
            add_checkbox(window, global_ocr, strings::settings_global_ocr.data(), 24, 142, state->config.global_ocr_enabled);
        state->controls[5] =
            add_edit(window, capture_hotkey, strings::settings_capture_hotkey.data(), 24, 180, state->config.capture_hotkey);
        state->controls[6] =
            add_edit(
                window, global_ocr_hotkey, strings::settings_global_ocr_hotkey.data(), 24, 214, state->config.global_ocr_hotkey);

        HWND note = CreateWindowExW(0,
                                    L"STATIC",
                                    strings::settings_note.data(),
                                    WS_CHILD | WS_VISIBLE,
                                    24,
                                    254,
                                    350,
                                    38,
                                    window,
                                    nullptr,
                                    nullptr,
                                    nullptr);
        set_font(note);

        HWND save = CreateWindowExW(0,
                                    L"BUTTON",
                                    strings::settings_save.data(),
                                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                    214,
                                    302,
                                    78,
                                    30,
                                    window,
                                    reinterpret_cast<HMENU>(IDOK),
                                    nullptr,
                                    nullptr);
        HWND cancel = CreateWindowExW(0,
                                      L"BUTTON",
                                      strings::settings_cancel.data(),
                                      WS_CHILD | WS_VISIBLE,
                                      300,
                                      302,
                                      78,
                                      30,
                                      window,
                                      reinterpret_cast<HMENU>(IDCANCEL),
                                      nullptr,
                                      nullptr);
        set_font(save);
        set_font(cancel);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == IDOK) {
            const std::wstring capture = control_text(state->controls[5]);
            const std::wstring global_ocr_value = control_text(state->controls[6]);
            if (!parse_hotkey(capture) || !parse_hotkey(global_ocr_value)) {
                MessageBoxW(window, strings::settings_invalid_hotkey.data(), kAppName, MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->config.annotation_enabled = checked(state->controls[0]);
            state->config.ocr_enabled = checked(state->controls[1]);
            state->config.shell_enabled = checked(state->controls[2]);
            state->config.start_at_login = checked(state->controls[3]);
            state->config.global_ocr_enabled = checked(state->controls[4]);
            state->config.capture_hotkey = capture;
            state->config.global_ocr_hotkey = global_ocr_value;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

bool show_settings_window(HWND owner, AppConfig& config) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = settings_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"AirScreenshot.Settings";
        RegisterClassExW(&window_class);
    });

    SettingsState state;
    state.config = config;
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int x = std::max(20L, owner_rect.left + 40L);
    const int y = std::max(20L, owner_rect.top + 40L);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.Settings",
                                  strings::settings_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  420,
                                  390,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return false;
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
    if (state.accepted) {
        config = std::move(state.config);
        return true;
    }
    return false;
}

}  // namespace airshot
