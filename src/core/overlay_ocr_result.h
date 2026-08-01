#pragma once

#include "airshot/ocr.h"

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace airshot::overlay_detail {

enum class OcrResultAction {
    copy_all,
    retry_fast,
    retry_accurate,
    close,
};

using OcrResultActionCallback =
    std::function<void(OcrResultAction)>;

enum class OcrResultShortcutAction {
    none,
    native_copy,
    copy_all,
    close,
    focus_next,
    focus_previous,
};

// Pure helpers kept public to make summary, keyboard and monitor-edge policy
// independently testable without creating a native window.
[[nodiscard]] std::wstring format_ocr_result_summary(
    const OcrOutput& output);

[[nodiscard]] constexpr OcrResultShortcutAction
resolve_ocr_result_shortcut(
    unsigned int virtual_key,
    bool control_down,
    bool shift_down,
    bool text_has_focus,
    bool text_has_selection) noexcept {
    if (virtual_key == VK_ESCAPE) {
        return OcrResultShortcutAction::close;
    }
    if (virtual_key == VK_TAB && !control_down) {
        return shift_down
                   ? OcrResultShortcutAction::focus_previous
                   : OcrResultShortcutAction::focus_next;
    }
    if (virtual_key == static_cast<unsigned int>('C') &&
        control_down) {
        return text_has_focus && text_has_selection
                   ? OcrResultShortcutAction::native_copy
                   : OcrResultShortcutAction::copy_all;
    }
    return OcrResultShortcutAction::none;
}

[[nodiscard]] SIZE fit_ocr_result_panel_size(
    unsigned int dpi,
    RECT work_area) noexcept;

[[nodiscard]] RECT place_ocr_result_panel(
    POINT anchor,
    SIZE size,
    RECT work_area,
    unsigned int dpi) noexcept;

// Creates and shows a modeless, top-level native result panel. The callback is
// an action request: the caller owns clipboard and OCR retry orchestration.
// The panel remains open after copy_all and closes before a retry/close action
// is delivered. This function must be called on the owning UI thread.
[[nodiscard]] HWND show_ocr_result_panel_async(
    const OcrOutput& output,
    HWND owner,
    POINT position,
    std::wstring_view theme,
    OcrResultActionCallback callback);

}  // namespace airshot::overlay_detail
