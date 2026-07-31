#include "overlay_helpers.h"

#include "airshot/strings.h"

#include <algorithm>
#include <array>
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
    std::wstring font_family{L"Microsoft YaHei"};
    bool font_bold{};
    bool font_italic{};
    TextStyle text_style{TextStyle::normal};
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
            state->font_bold ? FW_BOLD : FW_NORMAL,
            state->font_italic ? TRUE : FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            state->font_family.empty()
                ? L"Microsoft YaHei"
                : state->font_family.c_str()
        );
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);

        const COLORREF background =
            state->text_style == TextStyle::dark
                ? RGB(31, 35, 41)
                : (state->is_light_theme
                       ? RGB(255, 255, 255)
                       : RGB(0x1c, 0x1e, 0x22));
        state->bg_brush = CreateSolidBrush(background);

        SetFocus(state->edit);
        return 0;
    }
    if (message == WM_CTLCOLOREDIT) {
        HDC hdc = reinterpret_cast<HDC>(w_param);
        const bool dark_text = state->text_style == TextStyle::dark;
        SetTextColor(hdc, dark_text ? RGB(255, 255, 255) : state->color);
        SetBkColor(
            hdc,
            dark_text
                ? RGB(31, 35, 41)
                : (state->is_light_theme
                       ? RGB(255, 255, 255)
                       : RGB(0x1c, 0x1e, 0x22)));
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
                      std::wstring initial_text,
                      std::wstring_view font_family,
                      bool font_bold,
                      bool font_italic,
                      TextStyle text_style) {
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
    state->font_family = font_family.empty()
                             ? L"Microsoft YaHei"
                             : std::wstring(font_family);
    state->font_bold = font_bold;
    state->font_italic = font_italic;
    state->text_style = text_style;
    state->is_light_theme = is_light_theme;
    state->completion = std::move(completion);
    state->text = std::move(initial_text);

    if (state->text_style == TextStyle::dark) {
        state->is_light_theme = false;
    }

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

namespace {

constexpr int kSizeWidthEdit = 201;
constexpr int kSizeHeightEdit = 202;
constexpr int kSizeCenterCheck = 203;
constexpr int kSizeErrorLabel = 204;

struct SelectionSizePromptState {
    HWND window{};
    HWND width_edit{};
    HWND height_edit{};
    HWND center_check{};
    HWND error_label{};
    int current_width{};
    int current_height{};
    int maximum_width{};
    int maximum_height{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool is_light_theme{};
    bool accepted{};
    SelectionSizeInput input;
    HFONT font{};
    HBRUSH background_brush{};
    HBRUSH edit_brush{};
    SelectionSizeCompletion completion;
};

[[nodiscard]] int size_scale(
    const SelectionSizePromptState& state,
    int value) noexcept {
    return MulDiv(
        value,
        static_cast<int>(state.dpi),
        USER_DEFAULT_SCREEN_DPI);
}

void set_size_prompt_font(
    const SelectionSizePromptState& state,
    HWND control) noexcept {
    if (control && state.font) {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(state.font),
            TRUE);
    }
}

LRESULT CALLBACK size_edit_subclass_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param,
    UINT_PTR subclass_id,
    DWORD_PTR ref_data) {
    (void)subclass_id;
    (void)ref_data;
    if (message == WM_KEYDOWN) {
        if (w_param == VK_RETURN) {
            PostMessageW(
                GetParent(window),
                WM_COMMAND,
                MAKEWPARAM(IDOK, BN_CLICKED),
                reinterpret_cast<LPARAM>(window));
            return 0;
        }
        if (w_param == VK_ESCAPE) {
            PostMessageW(
                GetParent(window),
                WM_COMMAND,
                MAKEWPARAM(IDCANCEL, BN_CLICKED),
                reinterpret_cast<LPARAM>(window));
            return 0;
        }
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

void show_size_prompt_error(
    SelectionSizePromptState& state,
    SelectionSizeParseError error) {
    std::wstring message;
    HWND focus = state.width_edit;
    switch (error) {
        case SelectionSizeParseError::invalid_width:
            message = L"宽度必须是整数。";
            break;
        case SelectionSizeParseError::invalid_height:
            message = L"高度必须是整数。";
            focus = state.height_edit;
            break;
        case SelectionSizeParseError::width_out_of_range:
            message = std::format(
                L"宽度范围：2–{} px。",
                state.maximum_width);
            break;
        case SelectionSizeParseError::height_out_of_range:
            message = std::format(
                L"高度范围：2–{} px。",
                state.maximum_height);
            focus = state.height_edit;
            break;
        case SelectionSizeParseError::invalid_limits:
            message = L"当前虚拟桌面尺寸不可用。";
            break;
        case SelectionSizeParseError::none:
            break;
    }
    SetWindowTextW(state.error_label, message.c_str());
    if (focus) {
        SetFocus(focus);
        SendMessageW(focus, EM_SETSEL, 0, -1);
    }
    MessageBeep(MB_ICONWARNING);
}

LRESULT CALLBACK selection_size_prompt_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    auto* state = reinterpret_cast<SelectionSizePromptState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<SelectionSizePromptState*>(
            create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    if (message == WM_CREATE) {
        const auto s = [state](int value) {
            return size_scale(*state, value);
        };
        const COLORREF background = state->is_light_theme
                                        ? RGB(248, 249, 252)
                                        : RGB(28, 30, 34);
        const COLORREF edit_background = state->is_light_theme
                                             ? RGB(255, 255, 255)
                                             : RGB(39, 42, 48);
        state->background_brush = CreateSolidBrush(background);
        state->edit_brush = CreateSolidBrush(edit_background);
        state->font = CreateFontW(
            -s(14),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");

        const HWND width_label = CreateWindowExW(
            0,
            L"STATIC",
            L"宽度",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(20),
            s(48),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->width_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_width).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            s(70),
            s(16),
            s(92),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeWidthEdit)),
            nullptr,
            nullptr);
        const HWND width_unit = CreateWindowExW(
            0,
            L"STATIC",
            L"px",
            WS_CHILD | WS_VISIBLE,
            s(166),
            s(20),
            s(28),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        const HWND height_label = CreateWindowExW(
            0,
            L"STATIC",
            L"高度",
            WS_CHILD | WS_VISIBLE,
            s(198),
            s(20),
            s(48),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->height_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_height).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            s(250),
            s(16),
            s(92),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeHeightEdit)),
            nullptr,
            nullptr);
        const HWND height_unit = CreateWindowExW(
            0,
            L"STATIC",
            L"px",
            WS_CHILD | WS_VISIBLE,
            s(346),
            s(20),
            s(28),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->center_check = CreateWindowExW(
            0,
            L"BUTTON",
            L"以中心调整（取消勾选则固定左上角）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            s(18),
            s(58),
            s(330),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeCenterCheck)),
            nullptr,
            nullptr);
        state->error_label = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(91),
            s(356),
            s(24),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeErrorLabel)),
            nullptr,
            nullptr);
        const HWND ok = CreateWindowExW(
            0,
            L"BUTTON",
            L"确认",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            s(214),
            s(122),
            s(76),
            s(30),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDOK)),
            nullptr,
            nullptr);
        const HWND cancel = CreateWindowExW(
            0,
            L"BUTTON",
            L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            s(298),
            s(122),
            s(76),
            s(30),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDCANCEL)),
            nullptr,
            nullptr);

        for (const HWND control : {
                 width_label,
                 state->width_edit,
                 width_unit,
                 height_label,
                 state->height_edit,
                 height_unit,
                 state->center_check,
                 state->error_label,
                 ok,
                 cancel}) {
            set_size_prompt_font(*state, control);
        }
        SendMessageW(
            state->center_check,
            BM_SETCHECK,
            BST_CHECKED,
            0);
        SendMessageW(state->width_edit, EM_SETLIMITTEXT, 10, 0);
        SendMessageW(state->height_edit, EM_SETLIMITTEXT, 10, 0);
        SetWindowSubclass(
            state->width_edit,
            size_edit_subclass_proc,
            1,
            0);
        SetWindowSubclass(
            state->height_edit,
            size_edit_subclass_proc,
            1,
            0);
        SetFocus(state->width_edit);
        SendMessageW(state->width_edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == IDOK) {
            std::array<wchar_t, 32> width{};
            std::array<wchar_t, 32> height{};
            GetWindowTextW(
                state->width_edit,
                width.data(),
                static_cast<int>(width.size()));
            GetWindowTextW(
                state->height_edit,
                height.data(),
                static_cast<int>(height.size()));
            const SelectionSizeParseResult parsed = parse_selection_size(
                width.data(),
                height.data(),
                state->maximum_width,
                state->maximum_height);
            if (!parsed) {
                show_size_prompt_error(*state, parsed.error);
                return 0;
            }
            const bool center_checked =
                SendMessageW(
                    state->center_check,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;
            state->input = {
                parsed.width,
                parsed.height,
                center_checked &&
                        (GetKeyState(VK_SHIFT) & 0x8000) == 0
                    ? SelectionSizeAnchor::center
                    : SelectionSizeAnchor::top_left};
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkMode(dc, TRANSPARENT);
        const HWND control = reinterpret_cast<HWND>(l_param);
        const bool error_label =
            control && GetDlgCtrlID(control) == kSizeErrorLabel;
        SetTextColor(
            dc,
            error_label
                ? RGB(229, 72, 77)
                : (state->is_light_theme ? RGB(36, 40, 48)
                                         : RGB(234, 238, 245)));
        return reinterpret_cast<LRESULT>(state->background_brush);
    }
    if (message == WM_CTLCOLOREDIT) {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkColor(
            dc,
            state->is_light_theme ? RGB(255, 255, 255)
                                  : RGB(39, 42, 48));
        SetTextColor(
            dc,
            state->is_light_theme ? RGB(24, 28, 34)
                                  : RGB(245, 247, 250));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    if (message == WM_ERASEBKGND) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(
            reinterpret_cast<HDC>(w_param),
            &bounds,
            state->background_brush);
        return 1;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        if (state->width_edit) {
            RemoveWindowSubclass(
                state->width_edit,
                size_edit_subclass_proc,
                1);
        }
        if (state->height_edit) {
            RemoveWindowSubclass(
                state->height_edit,
                size_edit_subclass_proc,
                1);
        }
        if (state->font) DeleteObject(state->font);
        if (state->background_brush) DeleteObject(state->background_brush);
        if (state->edit_brush) DeleteObject(state->edit_brush);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        auto completion = std::move(state->completion);
        std::optional<SelectionSizeInput> result;
        if (state->accepted) {
            result = state->input;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const LRESULT default_result =
            DefWindowProcW(window, message, w_param, l_param);
        delete state;
        if (completion) {
            completion(std::move(result));
        }
        return default_result;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

HWND show_selection_size_prompt(
    HWND owner,
    POINT position,
    int current_width,
    int current_height,
    int maximum_width,
    int maximum_height,
    bool is_light_theme,
    SelectionSizeCompletion completion) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = selection_size_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = L"AirScreenshot.SelectionSizePrompt";
        RegisterClassExW(&window_class);
    });

    auto* state = new (std::nothrow) SelectionSizePromptState;
    if (!state) {
        if (completion) {
            completion(std::nullopt);
        }
        return nullptr;
    }
    state->current_width = current_width;
    state->current_height = current_height;
    state->maximum_width = maximum_width;
    state->maximum_height = maximum_height;
    state->is_light_theme = is_light_theme;
    state->completion = std::move(completion);
    state->dpi = owner && IsWindow(owner)
                     ? GetDpiForWindow(owner)
                     : GetDpiForSystem();
    if (state->dpi == 0) {
        state->dpi = USER_DEFAULT_SCREEN_DPI;
    }

    const int client_width = size_scale(*state, 392);
    const int client_height = size_scale(*state, 168);
    RECT window_bounds{0, 0, client_width, client_height};
    AdjustWindowRectExForDpi(
        &window_bounds,
        WS_CAPTION | WS_SYSMENU,
        FALSE,
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        state->dpi);
    const int window_width = window_bounds.right - window_bounds.left;
    const int window_height = window_bounds.bottom - window_bounds.top;
    int x = position.x;
    int y = position.y;
    const HMONITOR monitor = MonitorFromPoint(
        position,
        MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        x = std::clamp(
            x,
            static_cast<int>(monitor_info.rcWork.left),
            std::max(
                static_cast<int>(monitor_info.rcWork.left),
                static_cast<int>(monitor_info.rcWork.right) - window_width));
        y = std::clamp(
            y,
            static_cast<int>(monitor_info.rcWork.top),
            std::max(
                static_cast<int>(monitor_info.rcWork.top),
                static_cast<int>(monitor_info.rcWork.bottom) - window_height));
    }

    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"AirScreenshot.SelectionSizePrompt",
        L"调整选区尺寸 · F2",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
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

}  // namespace airshot::overlay_detail
