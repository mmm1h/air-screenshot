#include "settings_window.h"

#include "airshot/strings.h"

#include <commctrl.h>
#include <array>
#include <mutex>
#include <algorithm>
#include <vector>

#pragma comment(lib, "comctl32.lib")

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
    static_note = 108,
    static_label_capture = 109,
    static_label_ocr = 110,
    notifications = 111,
    annotation_lock = 112,
    hide_ellipse = 113,
    hide_line = 114,
    hide_pen = 115,
    hide_highlight = 116,
    hide_serial = 117,
    hide_eraser = 118,
    reset_serial = 119,
};

struct SettingsState {
    AppConfig config;
    bool accepted{};
    HWND window{};
    std::array<HWND, 15> controls{};
    HWND focused_control{};
};

struct ControlAnimData {
    double current_alpha{0.0};
    double target_alpha{0.0};
    UINT_PTR timer_id{0};
};

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

HFONT create_ui_font() {
    static HFONT font = CreateFontW(18,
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
                                    DEFAULT_PITCH,
                                    L"Microsoft YaHei");
    return font;
}

HFONT create_ui_bold_font() {
    static HFONT font = CreateFontW(18,
                                    0,
                                    0,
                                    0,
                                    FW_BOLD,
                                    FALSE,
                                    FALSE,
                                    FALSE,
                                    DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH,
                                    L"Microsoft YaHei");
    return font;
}

HFONT create_ui_title_font() {
    static HFONT font = CreateFontW(22,
                                    0,
                                    0,
                                    0,
                                    FW_BOLD,
                                    FALSE,
                                    FALSE,
                                    FALSE,
                                    DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH,
                                    L"Microsoft YaHei");
    return font;
}

void set_font(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(create_ui_font()), TRUE);
}

std::wstring tool_for_hidden_control(int id) {
    switch (id) {
        case hide_ellipse: return L"ellipse";
        case hide_line: return L"line";
        case hide_pen: return L"pen";
        case hide_highlight: return L"highlight";
        case hide_serial: return L"serial";
        case hide_eraser: return L"eraser";
        default: return {};
    }
}

bool is_hidden_tool_control(int id) {
    return !tool_for_hidden_control(id).empty();
}

std::vector<std::wstring> split_hidden_tools(std::wstring_view value) {
    std::vector<std::wstring> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L',', start);
        const std::size_t count = end == std::wstring_view::npos ? value.size() - start : end - start;
        if (count > 0) {
            result.emplace_back(value.substr(start, count));
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

void set_hidden_tool(AppConfig& config, std::wstring_view tool, bool hide) {
    std::wstring next;
    for (const auto& current : split_hidden_tools(normalize_annotation_hidden_tools(config.annotation_hidden_tools))) {
        if (_wcsicmp(current.c_str(), std::wstring(tool).c_str()) != 0) {
            if (!next.empty()) {
                next += L",";
            }
            next += current;
        }
    }
    if (hide) {
        if (!next.empty()) {
            next += L",";
        }
        next += tool;
    }
    config.annotation_hidden_tools = normalize_annotation_hidden_tools(next);
}

bool checkbox_checked(const SettingsState& state, int id) {
    if (id == annotation) return state.config.annotation_enabled;
    if (id == ocr) return state.config.ocr_enabled;
    if (id == shell) return state.config.shell_enabled;
    if (id == startup) return state.config.start_at_login;
    if (id == notifications) return state.config.notifications_enabled;
    if (id == global_ocr) return state.config.global_ocr_enabled;
    if (id == annotation_lock) return state.config.annotation_locked_tool;
    if (is_hidden_tool_control(id)) {
        return annotation_tool_hidden(state.config.annotation_hidden_tools, tool_for_hidden_control(id));
    }
    return false;
}

// Subclass proc to trigger smooth hover fade transitions
LRESULT CALLBACK HoverAnimSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    (void)dwRefData;
    auto* anim_data = reinterpret_cast<ControlAnimData*>(GetPropW(hwnd, L"AnimData"));
    if (!anim_data) {
        anim_data = new ControlAnimData();
        SetPropW(hwnd, L"AnimData", anim_data);
    }
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{ sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            if (anim_data->target_alpha != 1.0) {
                anim_data->target_alpha = 1.0;
                if (anim_data->timer_id == 0) {
                    anim_data->timer_id = SetTimer(hwnd, 1, 16, nullptr);
                }
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (anim_data->target_alpha != 0.0) {
                anim_data->target_alpha = 0.0;
                if (anim_data->timer_id == 0) {
                    anim_data->timer_id = SetTimer(hwnd, 1, 16, nullptr);
                }
            }
            break;
        }
        case WM_TIMER: {
            if (wparam == 1) {
                double diff = anim_data->target_alpha - anim_data->current_alpha;
                if (std::abs(diff) < 0.01) {
                    anim_data->current_alpha = anim_data->target_alpha;
                    KillTimer(hwnd, 1);
                    anim_data->timer_id = 0;
                } else {
                    anim_data->current_alpha += diff * 0.2;
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_NCDESTROY: {
            if (anim_data->timer_id != 0) {
                KillTimer(hwnd, anim_data->timer_id);
            }
            RemovePropW(hwnd, L"AnimData");
            delete anim_data;
            RemoveWindowSubclass(hwnd, HoverAnimSubclassProc, uIdSubclass);
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

HWND add_checkbox(HWND parent, int id, const wchar_t* text, int x, int y, [[maybe_unused]] bool checked, int width = 360) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   x,
                                   y,
                                   width,
                                   28,
                                   parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    set_font(control);
    SetPropW(control, L"AnimData", new ControlAnimData());
    SetWindowSubclass(control, HoverAnimSubclassProc, 0, 0);
    return control;
}

HWND add_edit(HWND parent, int id, int label_id, const wchar_t* label, int x, int y, int label_w, int edit_w, std::wstring_view value) {
    HWND label_control =
        CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, x, y, label_w, 28, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(label_id)), nullptr, nullptr);
    set_font(label_control);
    HWND control = CreateWindowExW(0,
                                   L"EDIT",
                                   std::wstring(value).c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   x + label_w + 10,
                                   y,
                                   edit_w,
                                   28,
                                   parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    set_font(control);
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));
    return control;
}

void draw_card(HDC hdc, const RECT& rect, const wchar_t* title, HFONT title_font) {
    HBRUSH bg_brush = CreateSolidBrush(RGB(30, 32, 36));
    HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(50, 54, 62));
    HGDIOBJ old_brush = SelectObject(hdc, bg_brush);
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);

    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(bg_brush);
    DeleteObject(border_pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ old_font = SelectObject(hdc, title_font);
    RECT text_rect{ rect.left + 20, rect.top + 16, rect.right - 20, rect.top + 40 };
    DrawTextW(hdc, title, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old_font);
}

void draw_edit_border(HWND parent, HWND edit, HDC hdc, bool focused) {
    RECT r{};
    GetWindowRect(edit, &r);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);

    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? RGB(22, 119, 255) : RGB(67, 72, 82));
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
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
            add_checkbox(window, annotation, strings::settings_annotation.data(), 40, 70, state->config.annotation_enabled, 300);
        state->controls[1] =
            add_checkbox(window, ocr, strings::settings_ocr.data(), 40, 110, state->config.ocr_enabled, 300);
        state->controls[2] =
            add_checkbox(window, shell, strings::settings_shell.data(), 40, 150, state->config.shell_enabled, 300);
        state->controls[3] =
            add_checkbox(window, startup, strings::settings_startup.data(), 40, 190, state->config.start_at_login, 300);
        state->controls[4] =
            add_checkbox(window, notifications, L"启用截图与 OCR 成功提示", 40, 230, state->config.notifications_enabled, 300);
        state->controls[5] =
            add_checkbox(window, global_ocr, strings::settings_global_ocr.data(), 390, 70, state->config.global_ocr_enabled, 310);
        state->controls[6] =
            add_edit(window, capture_hotkey, static_label_capture, strings::settings_capture_hotkey.data(), 390, 110, 110, 190, state->config.capture_hotkey);
        state->controls[7] =
            add_edit(window, global_ocr_hotkey, static_label_ocr, strings::settings_global_ocr_hotkey.data(), 390, 148, 110, 190, state->config.global_ocr_hotkey);
        state->controls[8] =
            add_checkbox(window, annotation_lock, L"标注后保持当前工具", 40, 340, state->config.annotation_locked_tool, 300);
        state->controls[9] =
            add_checkbox(window, hide_ellipse, L"隐藏椭圆工具", 40, 385, annotation_tool_hidden(state->config.annotation_hidden_tools, L"ellipse"), 210);
        state->controls[10] =
            add_checkbox(window, hide_line, L"隐藏直线工具", 270, 385, annotation_tool_hidden(state->config.annotation_hidden_tools, L"line"), 210);
        state->controls[11] =
            add_checkbox(window, hide_pen, L"隐藏画笔工具", 500, 385, annotation_tool_hidden(state->config.annotation_hidden_tools, L"pen"), 210);
        state->controls[12] =
            add_checkbox(window, hide_highlight, L"隐藏高亮工具", 40, 425, annotation_tool_hidden(state->config.annotation_hidden_tools, L"highlight"), 210);
        state->controls[13] =
            add_checkbox(window, hide_serial, L"隐藏序号工具", 270, 425, annotation_tool_hidden(state->config.annotation_hidden_tools, L"serial"), 210);
        state->controls[14] =
            add_checkbox(window, hide_eraser, L"隐藏橡皮擦工具", 500, 425, annotation_tool_hidden(state->config.annotation_hidden_tools, L"eraser"), 210);

        HWND reset = CreateWindowExW(0,
                                     L"BUTTON",
                                     L"重置序号",
                                     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                     590,
                                     336,
                                     110,
                                     34,
                                     window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(reset_serial)),
                                     nullptr,
                                     nullptr);
        set_font(reset);
        SetPropW(reset, L"AnimData", new ControlAnimData());
        SetWindowSubclass(reset, HoverAnimSubclassProc, 0, 0);

        HWND note = CreateWindowExW(0,
                                    L"STATIC",
                                    strings::settings_note.data(),
                                    WS_CHILD | WS_VISIBLE,
                                    20,
                                    540,
                                    680,
                                    40,
                                    window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(static_note)),
                                    nullptr,
                                    nullptr);
        set_font(note);

        HWND save = CreateWindowExW(0,
                                    L"BUTTON",
                                    strings::settings_save.data(),
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    500,
                                    600,
                                    90,
                                    34,
                                    window,
                                    reinterpret_cast<HMENU>(IDOK),
                                    nullptr,
                                    nullptr);
        HWND cancel = CreateWindowExW(0,
                                      L"BUTTON",
                                      strings::settings_cancel.data(),
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      610,
                                      600,
                                      90,
                                      34,
                                      window,
                                      reinterpret_cast<HMENU>(IDCANCEL),
                                      nullptr,
                                      nullptr);
        set_font(save);
        set_font(cancel);

        SetPropW(save, L"AnimData", new ControlAnimData());
        SetPropW(cancel, L"AnimData", new ControlAnimData());
        SetWindowSubclass(save, HoverAnimSubclassProc, 0, 0);
        SetWindowSubclass(cancel, HoverAnimSubclassProc, 0, 0);
        return 0;
    }

    if (message == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l_param);
        HDC hdc = dis->hDC;
        RECT rect = dis->rcItem;
        int id = dis->CtlID;

        if (id == annotation || id == ocr || id == shell || id == startup || id == global_ocr || id == notifications ||
            id == annotation_lock || is_hidden_tool_control(id)) {
            bool is_checked = checkbox_checked(*state, id);

            HBRUSH bg_brush = CreateSolidBrush(RGB(30, 32, 36));
            FillRect(hdc, &rect, bg_brush);
            DeleteObject(bg_brush);

            int track_w = 40;
            int track_h = 20;
            int track_x = 0;
            int track_y = (rect.bottom - rect.top - track_h) / 2;

            RECT track_rect{ track_x, track_y, track_x + track_w, track_y + track_h };

            auto* anim_data = reinterpret_cast<ControlAnimData*>(GetPropW(dis->hwndItem, L"AnimData"));
            double alpha = anim_data ? anim_data->current_alpha : 0.0;

            COLORREF track_color_normal = is_checked ? RGB(22, 119, 255) : RGB(58, 63, 71);
            COLORREF track_color_hover = is_checked ? RGB(64, 150, 255) : RGB(76, 82, 93);

            BYTE tr = static_cast<BYTE>(GetRValue(track_color_normal) + alpha * (GetRValue(track_color_hover) - GetRValue(track_color_normal)));
            BYTE tg = static_cast<BYTE>(GetGValue(track_color_normal) + alpha * (GetGValue(track_color_hover) - GetGValue(track_color_normal)));
            BYTE tb = static_cast<BYTE>(GetBValue(track_color_normal) + alpha * (GetBValue(track_color_hover) - GetBValue(track_color_normal)));
            COLORREF track_color = RGB(tr, tg, tb);

            HBRUSH track_brush = CreateSolidBrush(track_color);
            HPEN track_pen = CreatePen(PS_SOLID, 1, track_color);
            HGDIOBJ old_brush = SelectObject(hdc, track_brush);
            HGDIOBJ old_pen = SelectObject(hdc, track_pen);

            RoundRect(hdc, track_rect.left, track_rect.top, track_rect.right, track_rect.bottom, track_h, track_h);

            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(track_brush);
            DeleteObject(track_pen);

            int thumb_d = 16;
            int thumb_x = is_checked ? (track_x + track_w - thumb_d - 2) : (track_x + 2);
            int thumb_y = track_y + 2;

            HBRUSH thumb_brush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN thumb_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            old_brush = SelectObject(hdc, thumb_brush);
            old_pen = SelectObject(hdc, thumb_pen);

            Ellipse(hdc, thumb_x, thumb_y, thumb_x + thumb_d, thumb_y + thumb_d);

            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(thumb_brush);
            DeleteObject(thumb_pen);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(216, 222, 233));

            wchar_t text_buf[256]{};
            GetWindowTextW(dis->hwndItem, text_buf, 256);

            RECT text_rect{ track_x + track_w + 12, rect.top, rect.right, rect.bottom };
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
            HGDIOBJ old_font = SelectObject(hdc, font);

            DrawTextW(hdc, text_buf, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, old_font);
            return TRUE;
        }
        else if (id == IDOK || id == IDCANCEL || id == reset_serial) {
            bool is_pushed = (dis->itemState & ODS_SELECTED) != 0;

            auto* anim_data = reinterpret_cast<ControlAnimData*>(GetPropW(dis->hwndItem, L"AnimData"));
            double alpha = anim_data ? anim_data->current_alpha : 0.0;

            COLORREF base_normal = (id == IDOK) ? RGB(22, 119, 255) : RGB(58, 63, 71);
            COLORREF base_hover = (id == IDOK) ? RGB(64, 150, 255) : RGB(76, 82, 93);
            COLORREF base_pushed = (id == IDOK) ? RGB(9, 88, 217) : RGB(43, 47, 54);

            COLORREF btn_color;
            if (is_pushed) {
                btn_color = base_pushed;
            } else {
                BYTE r = static_cast<BYTE>(GetRValue(base_normal) + alpha * (GetRValue(base_hover) - GetRValue(base_normal)));
                BYTE g = static_cast<BYTE>(GetGValue(base_normal) + alpha * (GetGValue(base_hover) - GetGValue(base_normal)));
                BYTE b = static_cast<BYTE>(GetBValue(base_normal) + alpha * (GetBValue(base_hover) - GetBValue(base_normal)));
                btn_color = RGB(r, g, b);
            }

            HBRUSH brush = CreateSolidBrush(btn_color);
            HPEN pen = CreatePen(PS_SOLID, 1, btn_color);
            HGDIOBJ old_brush = SelectObject(hdc, brush);
            HGDIOBJ old_pen = SelectObject(hdc, pen);

            RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 6, 6);

            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(brush);
            DeleteObject(pen);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));

            wchar_t text_buf[256]{};
            GetWindowTextW(dis->hwndItem, text_buf, 256);

            HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
            HGDIOBJ old_font = SelectObject(hdc, font);

            DrawTextW(hdc, text_buf, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, old_font);
            return TRUE;
        }
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(window, &ps);

        HFONT title_font = create_ui_title_font();

        // Draw Card 1: 常规设置
        RECT card1_rect{ 20, 20, 350, 270 };
        draw_card(hdc, card1_rect, L"常规设置", title_font);

        RECT card2_rect{ 370, 20, 720, 220 };
        draw_card(hdc, card2_rect, L"快捷键配置", title_font);

        RECT card3_rect{ 20, 290, 720, 520 };
        draw_card(hdc, card3_rect, L"标注工具", title_font);

        // Draw Edit Borders dynamically
        if (state->controls[6]) {
            draw_edit_border(window, state->controls[6], hdc, state->focused_control == state->controls[6]);
        }
        if (state->controls[7]) {
            draw_edit_border(window, state->controls[7], hdc, state->focused_control == state->controls[7]);
        }

        EndPaint(window, &ps);
        return 0;
    }

    if (message == WM_CTLCOLOREDIT) {
        HDC hdc = reinterpret_cast<HDC>(w_param);
        SetTextColor(hdc, RGB(229, 233, 240));
        SetBkColor(hdc, RGB(41, 44, 50));

        static HBRUSH edit_bg_brush = CreateSolidBrush(RGB(41, 44, 50));
        return reinterpret_cast<INT_PTR>(edit_bg_brush);
    }

    if (message == WM_CTLCOLORSTATIC) {
        HDC hdc = reinterpret_cast<HDC>(w_param);
        HWND hwnd = reinterpret_cast<HWND>(l_param);
        int id = GetDlgCtrlID(hwnd);

        COLORREF bg_color = RGB(17, 19, 22);
        if (id == static_label_capture || id == static_label_ocr) {
            bg_color = RGB(30, 32, 36);
        }

        SetTextColor(hdc, RGB(216, 222, 233));
        SetBkColor(hdc, bg_color);

        static HBRUSH static_bg_brush = CreateSolidBrush(RGB(17, 19, 22));
        static HBRUSH card_bg_brush = CreateSolidBrush(RGB(30, 32, 36));
        return reinterpret_cast<INT_PTR>(bg_color == RGB(30, 32, 36) ? card_bg_brush : static_bg_brush);
    }

    if (message == WM_CTLCOLORDLG) {
        static HBRUSH dlg_bg_brush = CreateSolidBrush(RGB(17, 19, 22));
        return reinterpret_cast<INT_PTR>(dlg_bg_brush);
    }

    if (message == WM_COMMAND) {
        int id = LOWORD(w_param);
        int code = HIWORD(w_param);

        if (code == EN_SETFOCUS) {
            state->focused_control = reinterpret_cast<HWND>(l_param);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        } else if (code == EN_KILLFOCUS) {
            if (state->focused_control == reinterpret_cast<HWND>(l_param)) {
                state->focused_control = nullptr;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        if (id == annotation) {
            state->config.annotation_enabled = !state->config.annotation_enabled;
            InvalidateRect(state->controls[0], nullptr, TRUE);
            return 0;
        } else if (id == ocr) {
            state->config.ocr_enabled = !state->config.ocr_enabled;
            InvalidateRect(state->controls[1], nullptr, TRUE);
            return 0;
        } else if (id == shell) {
            state->config.shell_enabled = !state->config.shell_enabled;
            InvalidateRect(state->controls[2], nullptr, TRUE);
            return 0;
        } else if (id == startup) {
            state->config.start_at_login = !state->config.start_at_login;
            InvalidateRect(state->controls[3], nullptr, TRUE);
            return 0;
        } else if (id == notifications) {
            state->config.notifications_enabled = !state->config.notifications_enabled;
            InvalidateRect(state->controls[4], nullptr, TRUE);
            return 0;
        } else if (id == global_ocr) {
            state->config.global_ocr_enabled = !state->config.global_ocr_enabled;
            InvalidateRect(state->controls[5], nullptr, TRUE);
            return 0;
        } else if (id == annotation_lock) {
            state->config.annotation_locked_tool = !state->config.annotation_locked_tool;
            InvalidateRect(reinterpret_cast<HWND>(l_param), nullptr, TRUE);
            return 0;
        } else if (is_hidden_tool_control(id)) {
            const std::wstring tool = tool_for_hidden_control(id);
            const bool next_hidden = !annotation_tool_hidden(state->config.annotation_hidden_tools, tool);
            set_hidden_tool(state->config, tool, next_hidden);
            InvalidateRect(reinterpret_cast<HWND>(l_param), nullptr, TRUE);
            return 0;
        } else if (id == reset_serial) {
            state->config.annotation_next_serial = 1;
            InvalidateRect(reinterpret_cast<HWND>(l_param), nullptr, TRUE);
            return 0;
        }

        if (id == IDOK) {
            const std::wstring capture = control_text(state->controls[6]);
            const std::wstring global_ocr_value = control_text(state->controls[7]);
            if (!parse_hotkey(capture) || !parse_hotkey(global_ocr_value)) {
                MessageBoxW(window, strings::settings_invalid_hotkey.data(), kAppName, MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->config.capture_hotkey = capture;
            state->config.global_ocr_hotkey = global_ocr_value;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (id == IDCANCEL) {
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
        window_class.hbrBackground = CreateSolidBrush(RGB(17, 19, 22));
        window_class.lpszClassName = L"AirScreenshot.Settings";
        RegisterClassExW(&window_class);
    });

    SettingsState state;
    state.config = config;
    constexpr int window_width = 740;
    constexpr int window_height = 700;
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    RECT work_area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    }
    auto clamp_position = [](LONG value, LONG size, LONG minimum, LONG maximum) {
        if (size >= maximum - minimum) {
            return static_cast<int>(minimum);
        }
        return static_cast<int>(std::clamp(value, minimum, maximum - size));
    };
    const int x = clamp_position(owner_rect.left + 40L, window_width, work_area.left + 8L, work_area.right - 8L);
    const int y = clamp_position(owner_rect.top + 40L, window_height, work_area.top + 8L, work_area.bottom - 8L);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.Settings",
                                  strings::settings_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  window_width,
                                  window_height,
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
