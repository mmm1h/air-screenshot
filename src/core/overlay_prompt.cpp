#include "overlay_helpers.h"

#include "airshot/strings.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <commctrl.h>
#include <imm.h>

namespace airshot::overlay_detail {

struct PromptState {
    HWND window{};
    HWND edit{};
    bool accepted{};
    std::wstring text;
    COLORREF color{RGB(255, 255, 255)};
    float text_size{16.0f};
    HFONT font{};
    HBRUSH bg_brush{};
    bool is_light_theme{};
    TextPromptCompletion completion;
};

LRESULT CALLBACK edit_subclass_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param, UINT_PTR subclass_id, DWORD_PTR ref_data) {
    (void)subclass_id;
    (void)ref_data;
    if (message == WM_KEYDOWN) {
        if (w_param == VK_RETURN) {
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                return DefSubclassProc(window, message, w_param, l_param);
            }
            bool composing = false;
            if (HIMC himc = ImmGetContext(window)) {
                LONG size = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                ImmReleaseContext(window, himc);
                if (size > 0) {
                    composing = true;
                }
            }
            if (!composing) {
                PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDOK, 0), reinterpret_cast<LPARAM>(window));
                return 0;
            }
        }
        if (w_param == VK_ESCAPE) {
            PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), reinterpret_cast<LPARAM>(window));
            return 0;
        }
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

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
        int text_height = static_cast<int>(state->text_size);
        int window_height = std::clamp(text_height * 3 + 28, 96, 220);
        int window_width = 360;

        state->edit = CreateWindowExW(0,
                                      L"EDIT",
                                      state->text.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                          ES_MULTILINE | ES_AUTOVSCROLL |
                                          ES_WANTRETURN,
                                      6,
                                      6,
                                      window_width - 12,
                                      window_height - 12,
                                      window,
                                      reinterpret_cast<HMENU>(100),
                                      nullptr,
                                      nullptr);

        SetWindowSubclass(state->edit, edit_subclass_proc, 1, 0);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 4096, 0);

        state->font = CreateFontW(
            -text_height,
            0, 0, 0,
            FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei"
        );
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);

        state->bg_brush = CreateSolidBrush(state->is_light_theme ? RGB(255, 255, 255) : RGB(0x1c, 0x1e, 0x22));

        SetFocus(state->edit);
        return 0;
    }
    if (message == WM_CTLCOLOREDIT) {
        HDC hdc = reinterpret_cast<HDC>(w_param);
        SetTextColor(hdc, state->color);
        SetBkColor(hdc, state->is_light_theme ? RGB(255, 255, 255) : RGB(0x1c, 0x1e, 0x22));
        return reinterpret_cast<INT_PTR>(state->bg_brush);
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(window, &ps);
        RECT rect;
        GetClientRect(window, &rect);
        FillRect(hdc, &rect, state->bg_brush);

        HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(0x00, 0x66, 0xFF));
        HGDIOBJ old_pen = SelectObject(hdc, border_pen);
        HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old_brush);
        DeleteObject(border_pen);

        EndPaint(window, &ps);
        return 0;
    }
    if (message == WM_ACTIVATE) {
        if (LOWORD(w_param) == WA_INACTIVE) {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
            return 0;
        }
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
    if (message == WM_DESTROY) {
        if (state->edit) {
            RemoveWindowSubclass(state->edit, edit_subclass_proc, 1);
        }
        if (state->font) DeleteObject(state->font);
        if (state->bg_brush) DeleteObject(state->bg_brush);
    }
    if (message == WM_NCDESTROY) {
        auto completion = std::move(state->completion);
        std::optional<std::wstring> result;
        if (state->accepted) {
            result = std::move(state->text);
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const LRESULT default_result = DefWindowProcW(window, message, w_param, l_param);
        delete state;
        if (completion) {
            completion(std::move(result));
        }
        return default_result;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

HWND show_text_prompt(HWND owner,
                      POINT position,
                      COLORREF color,
                      float text_size,
                      bool is_light_theme,
                      TextPromptCompletion completion,
                      std::wstring initial_text) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = text_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        window_class.lpszClassName = L"AirScreenshot.TextPrompt";
        RegisterClassExW(&window_class);
    });

    auto* state = new (std::nothrow) PromptState;
    if (!state) {
        if (completion) {
            completion(std::nullopt);
        }
        return nullptr;
    }
    state->color = color;
    state->text_size = text_size;
    state->is_light_theme = is_light_theme;
    state->completion = std::move(completion);
    state->text = std::move(initial_text);

    int text_height = static_cast<int>(text_size);
    int window_height = std::clamp(text_height * 3 + 28, 96, 220);
    int window_width = 360;

    int x = position.x;
    int y = position.y;
    HMONITOR hMonitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
        const RECT& area = monitorInfo.rcWork;
        if (x + window_width > area.right) {
            x = area.right - window_width;
        }
        if (x < area.left) {
            x = area.left;
        }
        if (y + window_height > area.bottom) {
            y = area.bottom - window_height;
        }
        if (y < area.top) {
            y = area.top;
        }
    }

    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.TextPrompt",
                                  L"",
                                  WS_POPUP,
                                  x,
                                  y,
                                  window_width,
                                  window_height,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  state);
    if (!window) {
        auto failed_completion = std::move(state->completion);
        delete state;
        if (failed_completion) {
            failed_completion(std::nullopt);
        }
        return nullptr;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

std::optional<std::wstring> prompt_text(HWND owner, POINT position, COLORREF color, float text_size, bool is_light_theme) {
    bool completed = false;
    std::optional<std::wstring> result;
    HWND window = show_text_prompt(
        owner,
        position,
        color,
        text_size,
        is_light_theme,
        [&](std::optional<std::wstring> text) {
            result = std::move(text);
            completed = true;
        });
    if (!window && !completed) {
        return std::nullopt;
    }

    MSG message{};
    while (!completed) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            if (window && IsWindow(window)) {
                DestroyWindow(window);
            }
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result;
}

}  // namespace airshot::overlay_detail
