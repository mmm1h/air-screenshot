#include "overlay_helpers.h"

#include "airshot/strings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
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
    int text_box_width_px{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    HFONT font{};
    HBRUSH bg_brush{};
    bool is_light_theme{};
    TextPromptCompletion completion;
};

[[nodiscard]] int prompt_scale(const PromptState& state, int dip) noexcept {
    return MulDiv(dip, static_cast<int>(state.dpi), USER_DEFAULT_SCREEN_DPI);
}

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
        RECT client{};
        GetClientRect(window, &client);
        const int padding = prompt_scale(*state, 6);
        const DWORD edit_style =
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_WANTRETURN |
            (state->text_box_width_px == 0
                 ? (WS_HSCROLL | ES_AUTOHSCROLL)
                 : 0);

        state->edit = CreateWindowExW(0,
                                      L"EDIT",
                                      state->text.c_str(),
                                      edit_style,
                                      padding,
                                      padding,
                                      std::max(
                                          1,
                                          static_cast<int>(client.right) -
                                              padding * 2),
                                      std::max(
                                          1,
                                          static_cast<int>(client.bottom) -
                                              padding * 2),
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
        if (state->text_box_width_px > 0) {
            RECT formatting{};
            GetClientRect(state->edit, &formatting);
            formatting.right = std::min(
                formatting.right,
                formatting.left + state->text_box_width_px);
            SendMessageW(
                state->edit,
                EM_SETRECTNP,
                0,
                reinterpret_cast<LPARAM>(&formatting));
        }
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
                      TextStyle text_style,
                      int text_box_width_px) {
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
    state->text_box_width_px = std::max(0, text_box_width_px);
    state->is_light_theme = is_light_theme;
    state->completion = std::move(completion);
    state->text = std::move(initial_text);
    state->dpi = owner && IsWindow(owner)
                     ? GetDpiForWindow(owner)
                     : GetDpiForSystem();
    if (state->dpi == 0) {
        state->dpi = USER_DEFAULT_SCREEN_DPI;
    }

    if (state->text_style == TextStyle::dark) {
        state->is_light_theme = false;
    }

    int text_height = static_cast<int>(text_size);
    const int window_height = std::max(
        prompt_scale(*state, 96),
        std::min(
            text_height * 3 + prompt_scale(*state, 28),
            prompt_scale(*state, 220)));
    int window_width = prompt_scale(*state, 360);
    if (state->text_box_width_px > 0) {
        window_width = std::max(
            window_width,
            state->text_box_width_px + prompt_scale(*state, 12));
    }

    int x = position.x;
    int y = position.y;
    HMONITOR hMonitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
        const RECT& area = monitorInfo.rcWork;
        window_width = std::min(
            window_width,
            static_cast<int>(area.right - area.left));
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
constexpr int kSizeXEdit = 205;
constexpr int kSizeYEdit = 206;
constexpr int kSizeAspectCheck = 207;
constexpr int kSizeRoundedCheck = 208;
constexpr int kSizeCornerRadiusEdit = 209;

struct SelectionSizePromptState {
    HWND window{};
    HWND x_edit{};
    HWND y_edit{};
    HWND width_edit{};
    HWND height_edit{};
    HWND center_check{};
    HWND aspect_check{};
    HWND rounded_check{};
    HWND corner_radius_edit{};
    HWND error_label{};
    HWND ok_button{};
    HWND cancel_button{};
    RectI current_selection;
    RectI desktop_bounds;
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool is_light_theme{};
    bool accepted{};
    bool initialized{};
    bool syncing_aspect_ratio{};
    bool syncing_coordinates{};
    bool x_user_edited{};
    bool y_user_edited{};
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
        if (w_param == VK_TAB) {
            const HWND parent = GetParent(window);
            const bool previous =
                (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (const HWND next =
                    GetNextDlgTabItem(parent, window, previous)) {
                SetFocus(next);
            }
            return 0;
        }
        if (w_param == VK_RETURN) {
            const int control_id = GetDlgCtrlID(window);
            PostMessageW(
                GetParent(window),
                WM_COMMAND,
                MAKEWPARAM(
                    control_id == IDCANCEL ? IDCANCEL : IDOK,
                    BN_CLICKED),
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

[[nodiscard]] std::optional<int> read_positive_size_value(
    HWND control) noexcept {
    std::array<wchar_t, 32> text{};
    if (!control ||
        GetWindowTextW(
            control,
            text.data(),
            static_cast<int>(text.size())) <= 0) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const wchar_t character : std::wstring_view(text.data())) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        value = value * 10 + (character - L'0');
        if (value > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<int>(value);
}

void synchronize_aspect_ratio(
    SelectionSizePromptState& state,
    int source_id) noexcept {
    if (!state.initialized || state.syncing_aspect_ratio ||
        SendMessageW(state.aspect_check, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        return;
    }
    const int base_width = state.current_selection.width();
    const int base_height = state.current_selection.height();
    if (base_width < 2 || base_height < 2) {
        return;
    }

    const HWND source = source_id == kSizeHeightEdit
                            ? state.height_edit
                            : state.width_edit;
    const HWND target = source_id == kSizeHeightEdit
                            ? state.width_edit
                            : state.height_edit;
    const auto value = read_positive_size_value(source);
    if (!value) {
        return;
    }
    const std::int64_t numerator =
        static_cast<std::int64_t>(*value) *
        (source_id == kSizeHeightEdit ? base_width : base_height);
    const int denominator =
        source_id == kSizeHeightEdit ? base_height : base_width;
    const std::int64_t linked =
        (numerator + denominator / 2) / denominator;
    if (linked < 0 || linked > std::numeric_limits<int>::max()) {
        return;
    }

    state.syncing_aspect_ratio = true;
    const std::wstring linked_text = std::to_wstring(linked);
    SetWindowTextW(target, linked_text.c_str());
    state.syncing_aspect_ratio = false;
}

void synchronize_anchor_coordinates(
    SelectionSizePromptState& state) noexcept {
    if (!state.initialized || state.syncing_coordinates) {
        return;
    }
    const auto width = read_positive_size_value(state.width_edit);
    const auto height = read_positive_size_value(state.height_edit);
    if (!width || !height) {
        return;
    }
    const bool centered =
        SendMessageW(state.center_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto anchored_coordinate = [centered](
                                         int leading,
                                         int trailing,
                                         int extent) noexcept {
        if (!centered) {
            return leading;
        }
        const std::int64_t total =
            static_cast<std::int64_t>(leading) + trailing - extent;
        const std::int64_t divided =
            total >= 0 || total % 2 == 0 ? total / 2 : total / 2 - 1;
        return static_cast<int>(std::clamp<std::int64_t>(
            divided,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
    };

    state.syncing_coordinates = true;
    if (!state.x_user_edited) {
        const std::wstring value = std::to_wstring(
            anchored_coordinate(
                state.current_selection.left,
                state.current_selection.right,
                *width));
        SetWindowTextW(state.x_edit, value.c_str());
    }
    if (!state.y_user_edited) {
        const std::wstring value = std::to_wstring(
            anchored_coordinate(
                state.current_selection.top,
                state.current_selection.bottom,
                *height));
        SetWindowTextW(state.y_edit, value.c_str());
    }
    state.syncing_coordinates = false;
}

void show_size_prompt_error(
    SelectionSizePromptState& state,
    SelectionGeometryParseError error) {
    std::wstring message;
    HWND focus = state.x_edit;
    switch (error) {
        case SelectionGeometryParseError::invalid_x:
            message = L"X 必须是整数，可使用负坐标。";
            break;
        case SelectionGeometryParseError::invalid_y:
            message = L"Y 必须是整数，可使用负坐标。";
            focus = state.y_edit;
            break;
        case SelectionGeometryParseError::invalid_width:
            message = L"宽度必须是整数。";
            focus = state.width_edit;
            break;
        case SelectionGeometryParseError::invalid_height:
            message = L"高度必须是整数。";
            focus = state.height_edit;
            break;
        case SelectionGeometryParseError::width_out_of_range:
            message = std::format(
                L"宽度范围：2–{} px。",
                state.desktop_bounds.width());
            focus = state.width_edit;
            break;
        case SelectionGeometryParseError::height_out_of_range:
            message = std::format(
                L"高度范围：2–{} px。",
                state.desktop_bounds.height());
            focus = state.height_edit;
            break;
        case SelectionGeometryParseError::horizontal_out_of_range:
            message = std::format(
                L"横向范围必须完全位于虚拟桌面 {}–{} px 内。",
                state.desktop_bounds.left,
                state.desktop_bounds.right);
            break;
        case SelectionGeometryParseError::vertical_out_of_range:
            message = std::format(
                L"纵向范围必须完全位于虚拟桌面 {}–{} px 内。",
                state.desktop_bounds.top,
                state.desktop_bounds.bottom);
            focus = state.y_edit;
            break;
        case SelectionGeometryParseError::invalid_limits:
            message = L"当前虚拟桌面尺寸不可用。";
            break;
        case SelectionGeometryParseError::none:
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

        const HWND x_label = CreateWindowExW(
            0,
            L"STATIC",
            L"X",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(20),
            s(20),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->x_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_selection.left).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            s(42),
            s(16),
            s(116),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeXEdit)),
            nullptr,
            nullptr);
        const HWND y_label = CreateWindowExW(
            0,
            L"STATIC",
            L"Y",
            WS_CHILD | WS_VISIBLE,
            s(180),
            s(20),
            s(20),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->y_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_selection.top).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            s(204),
            s(16),
            s(116),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeYEdit)),
            nullptr,
            nullptr);
        const HWND coordinate_hint = CreateWindowExW(
            0,
            L"STATIC",
            L"虚拟桌面坐标",
            WS_CHILD | WS_VISIBLE,
            s(330),
            s(20),
            s(92),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        const HWND width_label = CreateWindowExW(
            0,
            L"STATIC",
            L"宽度",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(60),
            s(36),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->width_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_selection.width()).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            s(58),
            s(56),
            s(100),
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
            s(164),
            s(60),
            s(24),
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
            s(60),
            s(36),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->height_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(state->current_selection.height()).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            s(238),
            s(56),
            s(100),
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
            s(344),
            s(60),
            s(24),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->center_check = CreateWindowExW(
            0,
            L"BUTTON",
            L"以中心调整宽高",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            s(18),
            s(96),
            s(190),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeCenterCheck)),
            nullptr,
            nullptr);
        state->aspect_check = CreateWindowExW(
            0,
            L"BUTTON",
            L"锁定宽高比",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            s(220),
            s(96),
            s(150),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeAspectCheck)),
            nullptr,
            nullptr);
        state->rounded_check = CreateWindowExW(
            0,
            L"BUTTON",
            L"圆角输出",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            s(18),
            s(132),
            s(112),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeRoundedCheck)),
            nullptr,
            nullptr);
        const HWND corner_radius_label = CreateWindowExW(
            0,
            L"STATIC",
            L"半径",
            WS_CHILD | WS_VISIBLE,
            s(146),
            s(136),
            s(36),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->corner_radius_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            std::to_wstring(
                state->input.corner_radius > 0
                    ? state->input.corner_radius
                    : 16).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER |
                ES_AUTOHSCROLL,
            s(186),
            s(132),
            s(74),
            s(28),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeCornerRadiusEdit)),
            nullptr,
            nullptr);
        const HWND corner_radius_unit = CreateWindowExW(
            0,
            L"STATIC",
            L"px · PNG / 剪贴板透明角",
            WS_CHILD | WS_VISIBLE,
            s(268),
            s(136),
            s(154),
            s(24),
            window,
            nullptr,
            nullptr,
            nullptr);
        const HWND anchor_hint = CreateWindowExW(
            0,
            L"STATIC",
            L"修改 X/Y 时坐标优先；仅改宽高时按所选锚点调整。",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(166),
            s(404),
            s(22),
            window,
            nullptr,
            nullptr,
            nullptr);
        state->error_label = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            s(18),
            s(192),
            s(404),
            s(24),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSizeErrorLabel)),
            nullptr,
            nullptr);
        state->ok_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"确认",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            s(262),
            s(224),
            s(76),
            s(30),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDOK)),
            nullptr,
            nullptr);
        state->cancel_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            s(346),
            s(224),
            s(76),
            s(30),
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDCANCEL)),
            nullptr,
            nullptr);

        for (const HWND control : {
                 x_label,
                 state->x_edit,
                 y_label,
                 state->y_edit,
                 coordinate_hint,
                 width_label,
                 state->width_edit,
                 width_unit,
                 height_label,
                 state->height_edit,
                 height_unit,
                 state->center_check,
                 state->aspect_check,
                 state->rounded_check,
                 corner_radius_label,
                 state->corner_radius_edit,
                 corner_radius_unit,
                 anchor_hint,
                 state->error_label,
                 state->ok_button,
                 state->cancel_button}) {
            set_size_prompt_font(*state, control);
        }
        SendMessageW(
            state->center_check,
            BM_SETCHECK,
            state->input.anchor == SelectionSizeAnchor::center
                ? BST_CHECKED
                : BST_UNCHECKED,
            0);
        SendMessageW(
            state->aspect_check,
            BM_SETCHECK,
            state->input.aspect_ratio_locked ? BST_CHECKED : BST_UNCHECKED,
            0);
        SendMessageW(
            state->rounded_check,
            BM_SETCHECK,
            state->input.corner_radius > 0 ? BST_CHECKED : BST_UNCHECKED,
            0);
        EnableWindow(
            state->corner_radius_edit,
            state->input.corner_radius > 0);
        SendMessageW(state->x_edit, EM_SETLIMITTEXT, 11, 0);
        SendMessageW(state->y_edit, EM_SETLIMITTEXT, 11, 0);
        SendMessageW(state->width_edit, EM_SETLIMITTEXT, 10, 0);
        SendMessageW(state->height_edit, EM_SETLIMITTEXT, 10, 0);
        SendMessageW(state->corner_radius_edit, EM_SETLIMITTEXT, 3, 0);
        for (const HWND control : {
                 state->x_edit,
                 state->y_edit,
                 state->width_edit,
                 state->height_edit,
                 state->center_check,
                 state->aspect_check,
                 state->rounded_check,
                 state->corner_radius_edit,
                 state->ok_button,
                 state->cancel_button}) {
            SetWindowSubclass(
                control,
                size_edit_subclass_proc,
                1,
                0);
        }
        state->initialized = true;
        SetFocus(state->x_edit);
        SendMessageW(state->x_edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int control_id = LOWORD(w_param);
        const int notification = HIWORD(w_param);
        if ((control_id == kSizeWidthEdit ||
             control_id == kSizeHeightEdit) &&
            notification == EN_CHANGE) {
            SetWindowTextW(state->error_label, L"");
            synchronize_aspect_ratio(*state, control_id);
            synchronize_anchor_coordinates(*state);
            return 0;
        }
        if ((control_id == kSizeXEdit || control_id == kSizeYEdit) &&
            notification == EN_CHANGE) {
            SetWindowTextW(state->error_label, L"");
            if (state->initialized && !state->syncing_coordinates) {
                if (control_id == kSizeXEdit) {
                    state->x_user_edited = true;
                } else {
                    state->y_user_edited = true;
                }
            }
            return 0;
        }
        if (control_id == kSizeCenterCheck &&
            notification == BN_CLICKED) {
            SetWindowTextW(state->error_label, L"");
            synchronize_anchor_coordinates(*state);
            return 0;
        }
        if (control_id == kSizeAspectCheck &&
            notification == BN_CLICKED) {
            SetWindowTextW(state->error_label, L"");
            synchronize_aspect_ratio(*state, kSizeWidthEdit);
            synchronize_anchor_coordinates(*state);
            return 0;
        }
        if (control_id == kSizeRoundedCheck &&
            notification == BN_CLICKED) {
            SetWindowTextW(state->error_label, L"");
            const bool enabled =
                SendMessageW(
                    state->rounded_check,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;
            EnableWindow(state->corner_radius_edit, enabled);
            if (enabled) {
                SetFocus(state->corner_radius_edit);
                SendMessageW(
                    state->corner_radius_edit,
                    EM_SETSEL,
                    0,
                    -1);
            }
            return 0;
        }
        if (control_id == kSizeCornerRadiusEdit &&
            notification == EN_CHANGE) {
            SetWindowTextW(state->error_label, L"");
            return 0;
        }
        if (LOWORD(w_param) == IDOK) {
            std::array<wchar_t, 32> x{};
            std::array<wchar_t, 32> y{};
            std::array<wchar_t, 32> width{};
            std::array<wchar_t, 32> height{};
            GetWindowTextW(
                state->x_edit,
                x.data(),
                static_cast<int>(x.size()));
            GetWindowTextW(
                state->y_edit,
                y.data(),
                static_cast<int>(y.size()));
            GetWindowTextW(
                state->width_edit,
                width.data(),
                static_cast<int>(width.size()));
            GetWindowTextW(
                state->height_edit,
                height.data(),
                static_cast<int>(height.size()));
            const bool center_checked =
                SendMessageW(
                    state->center_check,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;
            const SelectionSizeAnchor anchor =
                center_checked
                    ? SelectionSizeAnchor::center
                    : SelectionSizeAnchor::top_left;
            const SelectionGeometryParseResult parsed =
                parse_selection_geometry(
                x.data(),
                y.data(),
                width.data(),
                height.data(),
                state->current_selection,
                state->desktop_bounds,
                anchor);
            if (!parsed) {
                show_size_prompt_error(*state, parsed.error);
                return 0;
            }
            int corner_radius = 0;
            const bool rounded =
                SendMessageW(
                    state->rounded_check,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;
            if (rounded) {
                const auto radius = read_positive_size_value(
                    state->corner_radius_edit);
                const int maximum_radius = std::min(
                    {512, parsed.width / 2, parsed.height / 2});
                if (!radius || *radius < 1 || *radius > maximum_radius) {
                    SetWindowTextW(
                        state->error_label,
                        std::format(
                            L"圆角半径范围：1–{} px。",
                            std::max(1, maximum_radius)).c_str());
                    SetFocus(state->corner_radius_edit);
                    SendMessageW(
                        state->corner_radius_edit,
                        EM_SETSEL,
                        0,
                        -1);
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                corner_radius = *radius;
            }
            state->input = {
                parsed.x,
                parsed.y,
                parsed.width,
                parsed.height,
                anchor,
                SendMessageW(
                    state->aspect_check,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED,
                corner_radius};
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
        for (const HWND control : {
                 state->x_edit,
                 state->y_edit,
                 state->width_edit,
                 state->height_edit,
                 state->center_check,
                 state->aspect_check,
                 state->rounded_check,
                 state->corner_radius_edit,
                 state->ok_button,
                 state->cancel_button}) {
            if (!control) {
                continue;
            }
            RemoveWindowSubclass(
                control,
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
    RectI current_selection,
    RectI desktop_bounds,
    SelectionSizeAnchor current_anchor,
    bool aspect_ratio_locked,
    int current_corner_radius,
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
    state->current_selection = current_selection.normalized();
    state->desktop_bounds = desktop_bounds.normalized();
    state->input.anchor = current_anchor;
    state->input.aspect_ratio_locked = aspect_ratio_locked;
    const int maximum_initial_radius = std::min(
        {512,
         state->current_selection.width() / 2,
         state->current_selection.height() / 2});
    state->input.corner_radius = std::clamp(
        current_corner_radius,
        0,
        std::max(0, maximum_initial_radius));
    state->is_light_theme = is_light_theme;
    state->completion = std::move(completion);
    state->dpi = owner && IsWindow(owner)
                     ? GetDpiForWindow(owner)
                     : GetDpiForSystem();
    if (state->dpi == 0) {
        state->dpi = USER_DEFAULT_SCREEN_DPI;
    }

    const int client_width = size_scale(*state, 440);
    const int client_height = size_scale(*state, 270);
    RECT window_bounds{0, 0, client_width, client_height};
    AdjustWindowRectExForDpi(
        &window_bounds,
        WS_CAPTION | WS_SYSMENU,
        FALSE,
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
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
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        L"AirScreenshot.SelectionSizePrompt",
        L"精确调整选区 · X / Y / W / H",
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
