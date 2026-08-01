#include "overlay_ocr_result.h"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

void test_summary() {
    airshot::OcrOutput output;
    output.ok = true;
    output.profile = L"rapidocr-v5-fast";
    output.blocks.resize(2);
    output.timings.total_ms = 86.4;
    expect(
        airshot::overlay_detail::format_ocr_result_summary(output) ==
            L"极速 · 2 个文本块 · 86 毫秒",
        "fast OCR summary");

    output.profile = L"rapidocr-v5-accurate";
    output.timings.total_ms = 1250.0;
    expect(
        airshot::overlay_detail::format_ocr_result_summary(output) ==
            L"高精度 · 2 个文本块 · 1.25 秒",
        "accurate OCR summary");

    output.ok = false;
    output.profile.clear();
    output.blocks.clear();
    output.timings.total_ms = std::nan("");
    expect(
        airshot::overlay_detail::format_ocr_result_summary(output) ==
            L"识别未完成 · 0 个文本块 · 耗时未知",
        "failed OCR summary");
}

void test_shortcuts() {
    using airshot::overlay_detail::OcrResultShortcutAction;
    using airshot::overlay_detail::resolve_ocr_result_shortcut;
    expect(
        resolve_ocr_result_shortcut('C', true, false, true, true) ==
            OcrResultShortcutAction::native_copy,
        "Ctrl+C preserves native selected-text copy");
    expect(
        resolve_ocr_result_shortcut('C', true, false, true, false) ==
            OcrResultShortcutAction::copy_all,
        "Ctrl+C without a selection copies all");
    expect(
        resolve_ocr_result_shortcut('C', true, false, false, false) ==
            OcrResultShortcutAction::copy_all,
        "Ctrl+C from an action control copies all");
    expect(
        resolve_ocr_result_shortcut(VK_ESCAPE, false, false, true, false) ==
            OcrResultShortcutAction::close,
        "Escape closes");
    expect(
        resolve_ocr_result_shortcut(VK_TAB, false, false, true, false) ==
            OcrResultShortcutAction::focus_next &&
            resolve_ocr_result_shortcut(VK_TAB, false, true, true, false) ==
                OcrResultShortcutAction::focus_previous,
        "Tab traversal direction");
    expect(
        resolve_ocr_result_shortcut(VK_TAB, true, false, true, false) ==
            OcrResultShortcutAction::none,
        "Ctrl+Tab is not consumed");
}

void test_bounds() {
    using airshot::overlay_detail::fit_ocr_result_panel_size;
    using airshot::overlay_detail::place_ocr_result_panel;

    const RECT work{100, 50, 2020, 1090};
    const SIZE preferred = fit_ocr_result_panel_size(96, work);
    expect(
        preferred.cx == 660 && preferred.cy == 500,
        "preferred panel size");

    const RECT near_bottom_right = place_ocr_result_panel(
        POINT{1980, 1060}, preferred, work, 96);
    expect(
        near_bottom_right.left == 1352 &&
            near_bottom_right.top == 582 &&
            near_bottom_right.right == 2012 &&
            near_bottom_right.bottom == 1082,
        "panel clamps to the work-area margin");

    const RECT small_work{0, 0, 300, 220};
    const SIZE fitted = fit_ocr_result_panel_size(96, small_work);
    expect(
        fitted.cx == 284 && fitted.cy == 204,
        "panel fits a constrained work area");
    const RECT fitted_bounds = place_ocr_result_panel(
        POINT{-1000, -1000}, fitted, small_work, 96);
    expect(
        fitted_bounds.left == 8 && fitted_bounds.top == 8 &&
            fitted_bounds.right == 292 && fitted_bounds.bottom == 212,
        "constrained panel remains inside the work area");

    const RECT negative_work{-1920, 0, 0, 1080};
    const RECT negative_bounds = place_ocr_result_panel(
        POINT{-20, 1060}, SIZE{660, 500}, negative_work, 96);
    expect(
        negative_bounds.left == -668 &&
            negative_bounds.top == 572 &&
            negative_bounds.right == -8 &&
            negative_bounds.bottom == 1072,
        "negative-coordinate monitor placement");

    const SIZE high_dpi = fit_ocr_result_panel_size(
        192, RECT{0, 0, 1200, 900});
    expect(
        high_dpi.cx == 1168 && high_dpi.cy == 868,
        "high-DPI panel still fits the monitor");

    const RECT oversized = place_ocr_result_panel(
        POINT{0, 0}, SIZE{5000, 5000}, RECT{0, 0, 40, 30}, 96);
    expect(
        oversized.left == 8 && oversized.top == 8 &&
            oversized.right == 32 && oversized.bottom == 22,
        "oversized request is reduced to the safe work area");
}

}  // namespace

int main() {
    test_summary();
    test_shortcuts();
    test_bounds();
    if (failures == 0) {
        std::cout << "All OCR result panel tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
