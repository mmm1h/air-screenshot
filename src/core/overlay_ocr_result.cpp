#include "overlay_ocr_result.h"

#include "airshot/ui_theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace airshot::overlay_detail {
namespace {

constexpr wchar_t kPanelClassName[] =
    L"AirScreenshot.OcrResultPanel";
constexpr UINT_PTR kChildSubclassId = 1;
constexpr UINT kDispatchActionMessage = WM_APP + 71;
constexpr int kCopyAllButton = 4201;
constexpr int kRetryFastButton = 4202;
constexpr int kRetryAccurateButton = 4203;
constexpr int kCloseButton = 4204;
constexpr int kPreferredWidthDip = 660;
constexpr int kPreferredHeightDip = 500;
constexpr int kWorkAreaMarginDip = 8;
constexpr std::size_t kMaximumDisplayedCharacters =
    4U * 1024U * 1024U;

struct PanelState {
    HWND window{};
    HWND owner{};
    HWND text_edit{};
    HWND copy_button{};
    HWND retry_fast_button{};
    HWND retry_accurate_button{};
    HWND close_button{};
    unsigned int dpi{96};
    std::wstring theme;
    std::wstring text;
    std::wstring displayed_text;
    std::wstring summary;
    UiPalette palette{};
    OcrResultActionCallback callback;
    OcrResultAction final_action{OcrResultAction::close};
    bool constructing{true};
    HFONT title_font{};
    HFONT body_font{};
    HFONT button_font{};
    HBRUSH background_brush{};
    HBRUSH text_background_brush{};
};

[[nodiscard]] int scale_dip(
    unsigned int dpi,
    int value) noexcept {
    const unsigned int safe_dpi = dpi == 0 ? 96U : dpi;
    const long long scaled =
        static_cast<long long>(value) * safe_dpi;
    return static_cast<int>(std::clamp(
        (scaled + 48LL) / 96LL,
        static_cast<long long>(std::numeric_limits<int>::min()),
        static_cast<long long>(std::numeric_limits<int>::max())));
}

[[nodiscard]] long long effective_axis_margin(
    long long extent,
    unsigned int dpi) noexcept {
    if (extent <= 1) {
        return 0;
    }
    return std::min(
        static_cast<long long>(scale_dip(dpi, kWorkAreaMarginDip)),
        (extent - 1) / 2);
}

[[nodiscard]] COLORREF to_colorref(
    const D2D1_COLOR_F& value) noexcept {
    const auto channel = [](float component) noexcept {
        const float safe = std::isfinite(component)
                               ? std::clamp(component, 0.0F, 1.0F)
                               : 0.0F;
        return static_cast<BYTE>(std::lround(safe * 255.0F));
    };
    return RGB(channel(value.r), channel(value.g), channel(value.b));
}

[[nodiscard]] std::wstring lowercase(
    std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(
        result,
        result.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return result;
}

[[nodiscard]] std::wstring profile_label(
    const OcrOutput& output) {
    const std::wstring profile = lowercase(output.profile);
    if (profile.find(L"accurate") != std::wstring::npos) {
        return L"高精度";
    }
    if (profile.find(L"compat") != std::wstring::npos ||
        profile.find(L"v4") != std::wstring::npos) {
        return L"兼容";
    }
    if (profile.find(L"fast") != std::wstring::npos) {
        return L"极速";
    }
    if (!output.ok) {
        return L"识别未完成";
    }
    return L"标准";
}

[[nodiscard]] std::wstring elapsed_label(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        return L"耗时未知";
    }
    if (milliseconds < 1000.0) {
        return std::format(
            L"{} 毫秒",
            static_cast<long long>(std::llround(milliseconds)));
    }
    return std::format(L"{:.2f} 秒", milliseconds / 1000.0);
}

[[nodiscard]] std::wstring make_displayed_text(
    const OcrOutput& output) {
    std::wstring source;
    if (!output.text.empty()) {
        source = output.text;
    } else if (!output.error.empty()) {
        source = L"识别未完成\r\n\r\n" + output.error;
    } else {
        source = L"未识别到文字。";
    }
    std::ranges::replace(source, L'\0', L'\uFFFD');
    if (source.size() <= kMaximumDisplayedCharacters) {
        return source;
    }
    source.resize(kMaximumDisplayedCharacters);
    source += L"\r\n\r\n— 文本过长，面板仅显示前 4 Mi 个字符；复制全部仍使用完整结果。";
    return source;
}

void delete_gdi_resources(PanelState* state) noexcept {
    if (!state) {
        return;
    }
    if (state->title_font) {
        DeleteObject(state->title_font);
        state->title_font = nullptr;
    }
    if (state->body_font) {
        DeleteObject(state->body_font);
        state->body_font = nullptr;
    }
    if (state->button_font) {
        DeleteObject(state->button_font);
        state->button_font = nullptr;
    }
    if (state->background_brush) {
        DeleteObject(state->background_brush);
        state->background_brush = nullptr;
    }
    if (state->text_background_brush) {
        DeleteObject(state->text_background_brush);
        state->text_background_brush = nullptr;
    }
}

[[nodiscard]] HFONT create_ui_font(
    unsigned int dpi,
    int size_dip,
    int weight) noexcept {
    return CreateFontW(
        -scale_dip(dpi, size_dip),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Text");
}

void apply_dwm_theme(PanelState* state) noexcept {
    if (!state || !state->window) {
        return;
    }
    const BOOL use_dark =
        !state->palette.light && !state->palette.high_contrast;
    (void)DwmSetWindowAttribute(
        state->window,
        20,
        &use_dark,
        sizeof(use_dark));
    (void)DwmSetWindowAttribute(
        state->window,
        19,
        &use_dark,
        sizeof(use_dark));
    constexpr DWORD rounded_corners = 2;
    (void)DwmSetWindowAttribute(
        state->window,
        33,
        &rounded_corners,
        sizeof(rounded_corners));
}

void refresh_theme(PanelState* state) {
    if (!state) {
        return;
    }
    state->palette = resolve_ui_palette(state->theme);
    delete_gdi_resources(state);
    state->title_font = create_ui_font(state->dpi, 20, FW_SEMIBOLD);
    state->body_font = create_ui_font(state->dpi, 14, FW_NORMAL);
    state->button_font = create_ui_font(state->dpi, 13, FW_SEMIBOLD);
    state->background_brush = CreateSolidBrush(
        to_colorref(state->palette.background));
    state->text_background_brush = CreateSolidBrush(
        to_colorref(state->palette.control));

    if (state->text_edit && state->body_font) {
        SendMessageW(
            state->text_edit,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(state->body_font),
            TRUE);
    }
    for (const HWND button : {
             state->copy_button,
             state->retry_fast_button,
             state->retry_accurate_button,
             state->close_button}) {
        if (button && state->button_font) {
            SendMessageW(
                button,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(state->button_font),
                TRUE);
        }
    }
    apply_dwm_theme(state);
    if (state->window) {
        InvalidateRect(state->window, nullptr, TRUE);
    }
    if (state->text_edit) {
        InvalidateRect(state->text_edit, nullptr, TRUE);
    }
}

[[nodiscard]] RECT monitor_work_area(HMONITOR monitor) noexcept {
    MONITORINFO info{sizeof(info)};
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return RECT{
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
    };
}

void position_child(
    HWND child,
    int x,
    int y,
    int width,
    int height) noexcept {
    if (!child) {
        return;
    }
    SetWindowPos(
        child,
        nullptr,
        x,
        y,
        std::max(0, width),
        std::max(0, height),
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void layout_children(PanelState* state) noexcept {
    if (!state || !state->window) {
        return;
    }
    RECT client{};
    GetClientRect(state->window, &client);
    const int width = std::max(0L, client.right - client.left);
    const int height = std::max(0L, client.bottom - client.top);
    const int padding = scale_dip(state->dpi, 20);
    const int header_height = scale_dip(state->dpi, 72);
    const int gap = scale_dip(state->dpi, 8);
    const int button_height = scale_dip(state->dpi, 40);
    const int available_width = std::max(0, width - padding * 2);
    const bool narrow = width < scale_dip(state->dpi, 580);
    const int footer_height = narrow
                                  ? button_height * 2 + gap * 2
                                  : button_height + gap;
    const int edit_top = header_height;
    const int edit_bottom =
        std::max(edit_top, height - padding - footer_height);
    position_child(
        state->text_edit,
        padding,
        edit_top,
        available_width,
        edit_bottom - edit_top);

    if (narrow) {
        const int column_width =
            std::max(0, (available_width - gap) / 2);
        const int row_one = height - padding - button_height * 2 - gap;
        const int row_two = height - padding - button_height;
        position_child(
            state->copy_button,
            padding,
            row_one,
            column_width,
            button_height);
        position_child(
            state->retry_fast_button,
            padding + column_width + gap,
            row_one,
            available_width - column_width - gap,
            button_height);
        position_child(
            state->retry_accurate_button,
            padding,
            row_two,
            column_width,
            button_height);
        position_child(
            state->close_button,
            padding + column_width + gap,
            row_two,
            available_width - column_width - gap,
            button_height);
        return;
    }

    const int copy_width = scale_dip(state->dpi, 96);
    const int fast_width = scale_dip(state->dpi, 104);
    const int accurate_width = scale_dip(state->dpi, 120);
    const int close_width = scale_dip(state->dpi, 72);
    const int total_width =
        copy_width + fast_width + accurate_width + close_width + gap * 3;
    int x = std::max(padding, width - padding - total_width);
    const int y = height - padding - button_height;
    position_child(
        state->copy_button, x, y, copy_width, button_height);
    x += copy_width + gap;
    position_child(
        state->retry_fast_button, x, y, fast_width, button_height);
    x += fast_width + gap;
    position_child(
        state->retry_accurate_button,
        x,
        y,
        accurate_width,
        button_height);
    x += accurate_width + gap;
    position_child(
        state->close_button, x, y, close_width, button_height);
}

void focus_next_control(
    PanelState* state,
    HWND current,
    bool reverse) noexcept {
    if (!state || !state->window) {
        return;
    }
    HWND next = GetNextDlgTabItem(
        state->window,
        current,
        reverse ? TRUE : FALSE);
    if (!next) {
        next = state->text_edit;
    }
    if (next) {
        SetFocus(next);
    }
}

[[nodiscard]] bool edit_has_selection(HWND edit) noexcept {
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(
        edit,
        EM_GETSEL,
        reinterpret_cast<WPARAM>(&start),
        reinterpret_cast<LPARAM>(&end));
    return start != end;
}

LRESULT CALLBACK child_subclass_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) {
    (void)subclass_id;
    auto* state = reinterpret_cast<PanelState*>(reference_data);
    if (!state) {
        return DefSubclassProc(window, message, w_param, l_param);
    }
    if (message == WM_KEYDOWN) {
        const bool control_down =
            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down =
            (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool is_text = window == state->text_edit;
        const auto action = resolve_ocr_result_shortcut(
            static_cast<unsigned int>(w_param),
            control_down,
            shift_down,
            is_text,
            is_text && edit_has_selection(window));
        switch (action) {
            case OcrResultShortcutAction::native_copy:
                return DefSubclassProc(window, message, w_param, l_param);
            case OcrResultShortcutAction::copy_all:
                if (!state->text.empty()) {
                    PostMessageW(
                        state->window,
                        kDispatchActionMessage,
                        static_cast<WPARAM>(OcrResultAction::copy_all),
                        0);
                }
                return 0;
            case OcrResultShortcutAction::close:
                PostMessageW(state->window, WM_CLOSE, 0, 0);
                return 0;
            case OcrResultShortcutAction::focus_next:
                focus_next_control(state, window, false);
                return 0;
            case OcrResultShortcutAction::focus_previous:
                focus_next_control(state, window, true);
                return 0;
            case OcrResultShortcutAction::none:
                break;
        }
        if (is_text && control_down &&
            w_param == static_cast<WPARAM>('A')) {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

[[nodiscard]] HWND create_button(
    PanelState* state,
    int identifier,
    const wchar_t* label) noexcept {
    HWND button = CreateWindowExW(
        0,
        L"BUTTON",
        label,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0,
        0,
        0,
        0,
        state->window,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(identifier)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (button) {
        SetWindowSubclass(
            button,
            child_subclass_proc,
            kChildSubclassId,
            reinterpret_cast<DWORD_PTR>(state));
    }
    return button;
}

void draw_button(
    PanelState* state,
    const DRAWITEMSTRUCT* item) noexcept {
    if (!state || !item || item->CtlType != ODT_BUTTON) {
        return;
    }
    const bool primary = item->CtlID == kCopyAllButton;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF background = to_colorref(
        primary ? state->palette.accent : state->palette.control);
    if (pressed) {
        background = to_colorref(
            primary ? state->palette.accent_hover : state->palette.active);
    }
    COLORREF foreground = to_colorref(
        primary ? state->palette.accent_text : state->palette.text);
    if (disabled) {
        foreground = to_colorref(state->palette.muted);
    }
    const COLORREF border = to_colorref(
        primary ? state->palette.accent : state->palette.cancel_border);

    HBRUSH fill = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ previous_brush =
        fill ? SelectObject(item->hDC, fill) : nullptr;
    HGDIOBJ previous_pen =
        pen ? SelectObject(item->hDC, pen) : nullptr;
    const int radius = scale_dip(state->dpi, 5);
    RoundRect(
        item->hDC,
        item->rcItem.left,
        item->rcItem.top,
        item->rcItem.right,
        item->rcItem.bottom,
        radius * 2,
        radius * 2);
    if (previous_brush) {
        SelectObject(item->hDC, previous_brush);
    }
    if (previous_pen) {
        SelectObject(item->hDC, previous_pen);
    }
    if (fill) {
        DeleteObject(fill);
    }
    if (pen) {
        DeleteObject(pen);
    }

    wchar_t label[64]{};
    GetWindowTextW(item->hwndItem, label, static_cast<int>(std::size(label)));
    HGDIOBJ previous_font = state->button_font
                                 ? SelectObject(item->hDC, state->button_font)
                                 : nullptr;
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, foreground);
    RECT text_rect = item->rcItem;
    DrawTextW(
        item->hDC,
        label,
        -1,
        &text_rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previous_font) {
        SelectObject(item->hDC, previous_font);
    }
    if ((item->itemState & ODS_FOCUS) != 0) {
        RECT focus = item->rcItem;
        const int inset = scale_dip(state->dpi, 3);
        InflateRect(&focus, -inset, -inset);
        DrawFocusRect(item->hDC, &focus);
    }
}

void paint_panel(PanelState* state) noexcept {
    if (!state || !state->window) {
        return;
    }
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(state->window, &paint);
    RECT client{};
    GetClientRect(state->window, &client);
    FillRect(
        dc,
        &client,
        state->background_brush
            ? state->background_brush
            : GetSysColorBrush(COLOR_WINDOW));
    const int padding = scale_dip(state->dpi, 20);
    RECT title_rect{
        padding,
        scale_dip(state->dpi, 14),
        client.right - padding,
        scale_dip(state->dpi, 42),
    };
    HGDIOBJ previous_font = state->title_font
                                 ? SelectObject(dc, state->title_font)
                                 : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, to_colorref(state->palette.text));
    DrawTextW(
        dc,
        L"文字识别",
        -1,
        &title_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (state->body_font) {
        SelectObject(dc, state->body_font);
    }
    RECT summary_rect{
        padding,
        scale_dip(state->dpi, 42),
        client.right - padding,
        scale_dip(state->dpi, 66),
    };
    SetTextColor(dc, to_colorref(state->palette.muted));
    DrawTextW(
        dc,
        state->summary.c_str(),
        static_cast<int>(state->summary.size()),
        &summary_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
            DT_NOPREFIX);
    if (previous_font) {
        SelectObject(dc, previous_font);
    }
    EndPaint(state->window, &paint);
}

void notify_copy_action(PanelState* state) noexcept {
    if (!state || state->text.empty() || !state->callback) {
        return;
    }
    try {
        state->callback(OcrResultAction::copy_all);
    } catch (...) {
        // Never allow a consumer callback to unwind through a Win32 proc.
    }
}

void close_with_action(
    PanelState* state,
    OcrResultAction action) noexcept {
    if (!state || !state->window) {
        return;
    }
    state->final_action = action;
    DestroyWindow(state->window);
}

LRESULT CALLBACK panel_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    auto* state = reinterpret_cast<PanelState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<PanelState*>(create->lpCreateParams);
        if (!state) {
            return FALSE;
        }
        state->window = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_CREATE: {
            state->dpi = GetDpiForWindow(window);
            if (state->dpi == 0) {
                state->dpi = 96;
            }
            state->text_edit = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                    ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_READONLY | ES_NOHIDESEL,
                0,
                0,
                0,
                0,
                window,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            state->copy_button = create_button(
                state, kCopyAllButton, L"复制全部");
            state->retry_fast_button = create_button(
                state, kRetryFastButton, L"极速重试");
            state->retry_accurate_button = create_button(
                state, kRetryAccurateButton, L"高精度重试");
            state->close_button = create_button(
                state, kCloseButton, L"关闭");
            if (!state->text_edit || !state->copy_button ||
                !state->retry_fast_button ||
                !state->retry_accurate_button || !state->close_button) {
                return -1;
            }
            SetWindowSubclass(
                state->text_edit,
                child_subclass_proc,
                kChildSubclassId,
                reinterpret_cast<DWORD_PTR>(state));
            SendMessageW(
                state->text_edit,
                EM_SETLIMITTEXT,
                static_cast<WPARAM>(state->displayed_text.size() + 1U),
                0);
            SetWindowTextW(
                state->text_edit,
                state->displayed_text.c_str());
            SendMessageW(state->text_edit, EM_SETSEL, 0, 0);
            EnableWindow(
                state->copy_button,
                state->text.empty() ? FALSE : TRUE);
            refresh_theme(state);
            layout_children(state);
            return 0;
        }

        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED) {
                switch (LOWORD(w_param)) {
                    case kCopyAllButton:
                        PostMessageW(
                            window,
                            kDispatchActionMessage,
                            static_cast<WPARAM>(
                                OcrResultAction::copy_all),
                            0);
                        return 0;
                    case kRetryFastButton:
                        close_with_action(
                            state, OcrResultAction::retry_fast);
                        return 0;
                    case kRetryAccurateButton:
                        close_with_action(
                            state, OcrResultAction::retry_accurate);
                        return 0;
                    case kCloseButton:
                        PostMessageW(window, WM_CLOSE, 0, 0);
                        return 0;
                    default:
                        break;
                }
            }
            break;

        case kDispatchActionMessage:
            if (static_cast<OcrResultAction>(w_param) ==
                OcrResultAction::copy_all) {
                notify_copy_action(state);
            }
            return 0;

        case WM_DRAWITEM:
            draw_button(
                state,
                reinterpret_cast<const DRAWITEMSTRUCT*>(l_param));
            return TRUE;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(l_param) == state->text_edit) {
                HDC dc = reinterpret_cast<HDC>(w_param);
                SetTextColor(dc, to_colorref(state->palette.text));
                SetBkColor(dc, to_colorref(state->palette.control));
                return reinterpret_cast<LRESULT>(
                    state->text_background_brush
                        ? state->text_background_brush
                        : GetSysColorBrush(COLOR_WINDOW));
            }
            break;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT:
            paint_panel(state);
            return 0;

        case WM_SIZE:
            layout_children(state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
            const HMONITOR monitor = MonitorFromWindow(
                window, MONITOR_DEFAULTTONEAREST);
            const RECT work_area = monitor_work_area(monitor);
            const int margin = scale_dip(
                state->dpi, kWorkAreaMarginDip);
            const LONG available_width = std::max(
                1L,
                work_area.right - work_area.left - margin * 2);
            const LONG available_height = std::max(
                1L,
                work_area.bottom - work_area.top - margin * 2);
            info->ptMinTrackSize.x = std::min(
                static_cast<LONG>(scale_dip(state->dpi, 440)),
                available_width);
            info->ptMinTrackSize.y = std::min(
                static_cast<LONG>(scale_dip(state->dpi, 320)),
                available_height);
            return 0;
        }

        case WM_DPICHANGED: {
            state->dpi = HIWORD(w_param);
            if (state->dpi == 0) {
                state->dpi = 96;
            }
            const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
            const HMONITOR monitor = MonitorFromRect(
                suggested, MONITOR_DEFAULTTONEAREST);
            const RECT work_area = monitor_work_area(monitor);
            SIZE size{
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
            };
            const int margin = scale_dip(
                state->dpi, kWorkAreaMarginDip);
            size.cx = std::min(
                size.cx,
                std::max(1L, work_area.right - work_area.left - margin * 2));
            size.cy = std::min(
                size.cy,
                std::max(1L, work_area.bottom - work_area.top - margin * 2));
            const int anchor_offset = scale_dip(state->dpi, 12);
            const RECT placed = place_ocr_result_panel(
                POINT{
                    suggested->left - anchor_offset,
                    suggested->top - anchor_offset,
                },
                size,
                work_area,
                state->dpi);
            SetWindowPos(
                window,
                nullptr,
                placed.left,
                placed.top,
                placed.right - placed.left,
                placed.bottom - placed.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            refresh_theme(state);
            layout_children(state);
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            refresh_theme(state);
            return 0;

        case WM_CLOSE:
            close_with_action(state, OcrResultAction::close);
            return 0;

        case WM_NCDESTROY: {
            if (state->constructing) {
                delete_gdi_resources(state);
                state->window = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                return DefWindowProcW(
                    window, message, w_param, l_param);
            }
            auto callback = std::move(state->callback);
            const OcrResultAction final_action = state->final_action;
            delete_gdi_resources(state);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result =
                DefWindowProcW(window, message, w_param, l_param);
            delete state;
            if (callback) {
                try {
                    callback(final_action);
                } catch (...) {
                    // Never allow a consumer callback to unwind through a
                    // Win32 window procedure.
                }
            }
            return result;
        }

        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void notify_creation_failure(
    OcrResultActionCallback& callback) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(OcrResultAction::close);
    } catch (...) {
    }
}

}  // namespace

std::wstring format_ocr_result_summary(const OcrOutput& output) {
    return std::format(
        L"{} · {} 个文本块 · {}",
        profile_label(output),
        output.blocks.size(),
        elapsed_label(output.timings.total_ms));
}

SIZE fit_ocr_result_panel_size(
    unsigned int dpi,
    RECT work_area) noexcept {
    const long long work_width = std::max(
        0LL,
        static_cast<long long>(work_area.right) - work_area.left);
    const long long work_height = std::max(
        0LL,
        static_cast<long long>(work_area.bottom) - work_area.top);
    const long long horizontal_margin =
        effective_axis_margin(work_width, dpi);
    const long long vertical_margin =
        effective_axis_margin(work_height, dpi);
    const int available_width = static_cast<int>(std::clamp(
        work_width - horizontal_margin * 2LL,
        1LL,
        static_cast<long long>(std::numeric_limits<int>::max())));
    const int available_height = static_cast<int>(std::clamp(
        work_height - vertical_margin * 2LL,
        1LL,
        static_cast<long long>(std::numeric_limits<int>::max())));
    return SIZE{
        std::min(
            scale_dip(dpi, kPreferredWidthDip),
            available_width),
        std::min(
            scale_dip(dpi, kPreferredHeightDip),
            available_height),
    };
}

RECT place_ocr_result_panel(
    POINT anchor,
    SIZE size,
    RECT work_area,
    unsigned int dpi) noexcept {
    const long long offset = scale_dip(dpi, 12);
    const long long work_width = std::max(
        0LL,
        static_cast<long long>(work_area.right) - work_area.left);
    const long long work_height = std::max(
        0LL,
        static_cast<long long>(work_area.bottom) - work_area.top);
    const long long horizontal_margin =
        effective_axis_margin(work_width, dpi);
    const long long vertical_margin =
        effective_axis_margin(work_height, dpi);
    const long long minimum_x =
        static_cast<long long>(work_area.left) + horizontal_margin;
    const long long minimum_y =
        static_cast<long long>(work_area.top) + vertical_margin;
    const long long available_width = std::max(
        1LL, work_width - horizontal_margin * 2LL);
    const long long available_height = std::max(
        1LL, work_height - vertical_margin * 2LL);
    const long long safe_width = std::min(
        static_cast<long long>(std::max(1L, size.cx)),
        available_width);
    const long long safe_height = std::min(
        static_cast<long long>(std::max(1L, size.cy)),
        available_height);
    const long long maximum_x = std::max(
        minimum_x,
        static_cast<long long>(work_area.right) - horizontal_margin -
            safe_width);
    const long long maximum_y = std::max(
        minimum_y,
        static_cast<long long>(work_area.bottom) - vertical_margin -
            safe_height);
    const long long x = std::clamp(
        static_cast<long long>(anchor.x) + offset,
        minimum_x,
        maximum_x);
    const long long y = std::clamp(
        static_cast<long long>(anchor.y) + offset,
        minimum_y,
        maximum_y);
    return RECT{
        static_cast<LONG>(x),
        static_cast<LONG>(y),
        static_cast<LONG>(x + safe_width),
        static_cast<LONG>(y + safe_height),
    };
}

HWND show_ocr_result_panel_async(
    const OcrOutput& output,
    HWND owner,
    POINT position,
    std::wstring_view theme,
    OcrResultActionCallback callback) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = panel_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        window_class.lpszClassName = kPanelClassName;
        (void)RegisterClassExW(&window_class);
    });

    std::unique_ptr<PanelState> state(
        new (std::nothrow) PanelState);
    if (!state) {
        notify_creation_failure(callback);
        return nullptr;
    }
    try {
        state->owner = owner;
        state->theme = std::wstring(theme);
        state->text = output.text;
        state->displayed_text = make_displayed_text(output);
        state->summary = format_ocr_result_summary(output);
        state->callback = std::move(callback);
    } catch (const std::bad_alloc&) {
        notify_creation_failure(callback);
        return nullptr;
    } catch (const std::length_error&) {
        notify_creation_failure(callback);
        return nullptr;
    }

    const HMONITOR monitor = MonitorFromPoint(
        position, MONITOR_DEFAULTTONEAREST);
    const RECT work_area = monitor_work_area(monitor);
    unsigned int dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (dpi == 0) {
        dpi = 96;
    }
    const SIZE size = fit_ocr_result_panel_size(dpi, work_area);
    const RECT bounds = place_ocr_result_panel(
        position, size, work_area, dpi);

    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        kPanelClassName,
        L"OCR 识别结果",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
            WS_CLIPCHILDREN,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        state.get());
    if (!window) {
        auto failed_callback = std::move(state->callback);
        notify_creation_failure(failed_callback);
        return nullptr;
    }
    auto* live_state = state.get();
    live_state->constructing = false;
    const SIZE actual_size = fit_ocr_result_panel_size(
        live_state->dpi, work_area);
    const RECT actual_bounds = place_ocr_result_panel(
        position, actual_size, work_area, live_state->dpi);
    state.release();
    SetWindowPos(
        window,
        HWND_TOPMOST,
        actual_bounds.left,
        actual_bounds.top,
        actual_bounds.right - actual_bounds.left,
        actual_bounds.bottom - actual_bounds.top,
        SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    live_state = reinterpret_cast<PanelState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (live_state && live_state->text_edit) {
        SetFocus(live_state->text_edit);
    }
    return window;
}

}  // namespace airshot::overlay_detail
