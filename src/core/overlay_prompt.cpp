#include "overlay_helpers.h"

#include "airshot/strings.h"

#include <algorithm>
#include <mutex>

namespace airshot::overlay_detail {

struct PromptState {
    HWND window{};
    HWND edit{};
    bool accepted{};
    std::wstring text;
};

LRESULT CALLBACK text_prompt_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<PromptState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_CREATE) {
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                      L"EDIT",
                                      L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      12,
                                      12,
                                      380,
                                      26,
                                      window,
                                      reinterpret_cast<HMENU>(100),
                                      nullptr,
                                      nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_confirm.data(),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        226,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDOK),
                        nullptr,
                        nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_cancel.data(),
                        WS_CHILD | WS_VISIBLE,
                        312,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDCANCEL),
                        nullptr,
                        nullptr);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SetFocus(state->edit);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            state->text.resize(static_cast<std::size_t>(length + 1));
            if (length > 0) {
                GetWindowTextW(state->edit, state->text.data(), length + 1);
            }
            state->text.resize(static_cast<std::size_t>(length));
            state->accepted = !state->text.empty();
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

std::optional<std::wstring> prompt_text(HWND owner, POINT position) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = text_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"AirScreenshot.TextPrompt";
        RegisterClassExW(&window_class);
    });

    PromptState state;
    const int x = std::max(0, static_cast<int>(position.x) - 200);
    const int y = std::max(0, static_cast<int>(position.y) - 60);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.TextPrompt",
                                  strings::prompt_text_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  420,
                                  130,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return std::nullopt;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.accepted ? std::optional(state.text) : std::nullopt;
}



}  // namespace airshot::overlay_detail
