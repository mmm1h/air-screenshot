#include "airshot/bitmap.h"
#include "airshot/pin_layout.h"
#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/ocr.h"
#include "airshot/portable.h"
#include "airshot/output.h"

#include "../src/core/overlay_types.h"
#include "../src/core/overlay_annotation_history.h"

#include <winrt/base.h>

#include <cmath>
#include <iostream>
#include <fstream>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << message << L'\n';
    }
}

void test_rect_and_bitmap() {
    const airshot::RectI value{10, 20, -5, 3};
    const auto normalized = value.normalized();
    expect(normalized.left == -5 && normalized.top == 3 && normalized.right == 10 && normalized.bottom == 20,
           L"RectI normalization");
    const auto overlap = airshot::intersect({-10, -10, 10, 10}, {0, 0, 20, 20});
    expect(overlap && overlap->left == 0 && overlap->top == 0 && overlap->right == 10 && overlap->bottom == 10,
           L"negative-coordinate intersection");

    airshot::Bitmap source(4, 4);
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        source.pixels[index] = static_cast<std::uint8_t>(index);
    }
    airshot::Bitmap target(2, 2);
    airshot::blit(source, {1, 1, 3, 3}, target, POINT{0, 0});
    expect(target.pixels[0] == source.pixels[20], L"bitmap blit");
    const auto cropped = airshot::crop(source, {1, 1, 3, 3});
    expect(cropped.width == 2 && cropped.height == 2 && cropped.pixels == target.pixels, L"bitmap crop");

    // Test rotation and flipping
    airshot::Bitmap rot_source(2, 3);
    rot_source.pixels = {
        1, 0, 0, 255,   2, 0, 0, 255,
        3, 0, 0, 255,   4, 0, 0, 255,
        5, 0, 0, 255,   6, 0, 0, 255
    };
    const auto rot_cw = airshot::rotate_90_cw(rot_source);
    expect(rot_cw.width == 3 && rot_cw.height == 2, L"rotate_90_cw dimensions");
    expect(rot_cw.pixels[0] == 5 && rot_cw.pixels[4] == 3 && rot_cw.pixels[8] == 1, L"rotate_90_cw row 0");
    expect(rot_cw.pixels[12] == 6 && rot_cw.pixels[16] == 4 && rot_cw.pixels[20] == 2, L"rotate_90_cw row 1");

    const auto rot_ccw = airshot::rotate_90_ccw(rot_source);
    expect(rot_ccw.width == 3 && rot_ccw.height == 2, L"rotate_90_ccw dimensions");
    expect(rot_ccw.pixels[0] == 2 && rot_ccw.pixels[4] == 4 && rot_ccw.pixels[8] == 6, L"rotate_90_ccw row 0");
    expect(rot_ccw.pixels[12] == 1 && rot_ccw.pixels[16] == 3 && rot_ccw.pixels[20] == 5, L"rotate_90_ccw row 1");

    const auto flip_h = airshot::flip_horizontal(rot_source);
    expect(flip_h.width == 2 && flip_h.height == 3, L"flip_horizontal dimensions");
    expect(flip_h.pixels[0] == 2 && flip_h.pixels[4] == 1, L"flip_horizontal row 0");
    expect(flip_h.pixels[8] == 4 && flip_h.pixels[12] == 3, L"flip_horizontal row 1");
    expect(flip_h.pixels[16] == 6 && flip_h.pixels[20] == 5, L"flip_horizontal row 2");

    const auto flip_v = airshot::flip_vertical(rot_source);
    expect(flip_v.width == 2 && flip_v.height == 3, L"flip_vertical dimensions");
    expect(flip_v.pixels[0] == 5 && flip_v.pixels[4] == 6, L"flip_vertical row 0");
    expect(flip_v.pixels[8] == 3 && flip_v.pixels[12] == 4, L"flip_vertical row 1");
    expect(flip_v.pixels[16] == 1 && flip_v.pixels[20] == 2, L"flip_vertical row 2");

    // Test blur_circle
    airshot::Bitmap blur_source(5, 5);
    std::fill(blur_source.pixels.begin(), blur_source.pixels.end(), static_cast<uint8_t>(0));
    blur_source.pixels[(2 * 5 + 2) * 4] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 1] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 2] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 3] = 255;
    airshot::blur_circle(blur_source, POINT{2, 2}, 2, 1);
    expect(blur_source.pixels[(2 * 5 + 2) * 4] < 200 && blur_source.pixels[(2 * 5 + 2) * 4] > 0, L"blur_circle center pixel blurred");
    expect(blur_source.pixels[(2 * 5 + 1) * 4] > 0, L"blur_circle spreads to neighbors");

    const auto temp_png = std::filesystem::temp_directory_path() / L"airshot-test-save.png";
    std::wstring save_error;
    expect(airshot::save_png(source, temp_png, &save_error), L"save_png works");
    expect(std::filesystem::exists(temp_png), L"saved png file exists");
    std::error_code ignored;
    std::filesystem::remove(temp_png, ignored);
}

void test_pin_recovery_layout() {
    const airshot::RectI work{0, 0, 1920, 1080};
    const auto recovered =
        airshot::recover_pin_bounds(
            {2000, 1100, 2300, 1300},
            work);
    expect(
        recovered.left == 1620 &&
            recovered.top == 880 &&
            recovered.right == 1920 &&
            recovered.bottom == 1080,
        L"off-screen pin is restored fully inside the work area");

    const auto oversized =
        airshot::recover_pin_bounds(
            {2000, 2000, 5000, 4000},
            work);
    expect(
        oversized.left == 1872 &&
            oversized.top == 1032 &&
            oversized.width() == 3000 &&
            oversized.height() == 2000,
        L"oversized pin keeps a draggable edge visible");

    const auto negative_monitor =
        airshot::recover_pin_bounds(
            {-2500, -100, -2200, 100},
            {-1920, 0, 0, 1080});
    expect(
        negative_monitor.left == -1920 &&
            negative_monitor.top == 0 &&
            negative_monitor.right == -1620 &&
            negative_monitor.bottom == 200,
        L"pin recovery supports negative monitor coordinates");

    expect(
        airshot::fit_pin_scale(640, 480, work) == 1.0,
        L"small pins keep their original scale");
    expect(
        std::abs(
            airshot::fit_pin_scale(4000, 2000, work) -
            0.3936) < 0.0001,
        L"large pins fit within the configured work-area coverage");
    expect(
        airshot::fit_pin_scale(0, 2000, work) == 1.0,
        L"invalid pin dimensions keep a safe default scale");
}

void test_serial_counter_is_scoped_to_capture_session() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    std::vector<Annotation> first_capture;
    expect(
        airshot::overlay_detail::next_serial_number(first_capture) == 1,
           L"first serial in a capture session starts at one");
    Annotation first;
    first.tool = Tool::serial;
    first.serial = 1;
    first_capture.push_back(first);
    expect(
        airshot::overlay_detail::next_serial_number(first_capture) == 2,
           L"serials increment within one capture session");

    Annotation third = first;
    third.serial = 3;
    first_capture.push_back(third);
    airshot::overlay_detail::renumber_serial_annotations(first_capture);
    expect(
        first_capture[0].serial == 1 &&
            first_capture[1].serial == 2 &&
            airshot::overlay_detail::next_serial_number(first_capture) == 3,
        L"deleting a middle serial keeps numbering continuous");

    std::vector<Annotation> next_capture;
    expect(
        airshot::overlay_detail::next_serial_number(next_capture) == 1,
           L"a new capture session resets serial numbering to one");
}

void test_annotation_geometry_and_history() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::AnnotationHistory;
    using airshot::overlay_detail::Tool;

    expect(
        airshot::overlay_detail::tool_cursor_radius(
            Tool::pen,
            12.0F) == 6.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::mosaic,
                12.0F) == 42.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::highlight,
                12.0F) == 19.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::eraser,
                3.0F) == 8.0F,
        L"brush cursor matches each tool's real editing footprint");

    const auto square =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::rectangle,
            POINT{10, 10},
            POINT{14, 20},
            true,
            false);
    expect(
        square.start.x == 10 && square.start.y == 10 &&
            square.end.x == 20 && square.end.y == 20,
        L"Shift constrains rectangles to a square");

    const auto centered =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::ellipse,
            POINT{20, 20},
            POINT{24, 26},
            true,
            true);
    expect(
        centered.start.x == 14 && centered.start.y == 14 &&
            centered.end.x == 26 && centered.end.y == 26,
        L"Alt draws constrained shapes from their center");

    const auto horizontal =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::arrow,
            POINT{0, 0},
            POINT{10, 3},
            true,
            false);
    expect(
        horizontal.end.y == 0,
        L"Shift snaps lines and arrows to 45 degree increments");

    const auto centered_near_edge =
        airshot::overlay_detail::fit_annotation_geometry_to_canvas(
            airshot::overlay_detail::constrained_annotation_geometry(
                Tool::rectangle,
                POINT{10, 50},
                POINT{60, 80},
                false,
                true),
            POINT{10, 50},
            100,
            100,
            true,
            false);
    expect(
        centered_near_edge.start.x == 0 &&
            centered_near_edge.end.x == 20 &&
            centered_near_edge.start.y == 20 &&
            centered_near_edge.end.y == 80,
        L"Alt-centered shapes clamp each axis without distorting the other");

    const std::vector<POINT> long_stroke{{0, 0}, {100, 0}};
    const auto capped_stroke =
        airshot::overlay_detail::resample_polyline(long_stroke, 1.0, 2);
    expect(
        capped_stroke.size() == 2 &&
            capped_stroke.front().x == 0 &&
            capped_stroke.back().x == 100,
        L"bounded stroke resampling always preserves the final endpoint");

    Annotation rectangle;
    rectangle.tool = Tool::rectangle;
    rectangle.start = {10, 10};
    rectangle.end = {30, 30};
    std::vector<Annotation> annotations;
    AnnotationHistory history;
    history.record(annotations);
    annotations.push_back(rectangle);
    expect(
        history.can_undo() && history.undo(annotations) &&
            annotations.empty() && history.can_redo(),
        L"annotation creation participates in undo");
    expect(
        history.redo(annotations) && annotations.size() == 1,
        L"annotation creation participates in redo");

    history.record(annotations);
    airshot::overlay_detail::translate_annotation(
        annotations.front(),
        15,
        5);
    expect(
        history.undo(annotations) &&
            annotations.front().start.x == 10 &&
            annotations.front().start.y == 10,
        L"annotation movement is restored atomically");

    const POINT clamped =
        airshot::overlay_detail::clamp_annotation_translation(
            rectangle,
            -100,
            100,
            100,
            100);
    expect(
        clamped.x == -8 && clamped.y == 68,
        L"annotation movement keeps the full stroke inside the capture");

    const auto handles =
        airshot::overlay_detail::annotation_control_handles(
            rectangle);
    expect(
        handles.count == 8 &&
            airshot::overlay_detail::
                hit_test_annotation_control_handle(
                    rectangle,
                    POINT{30, 30},
                    4) ==
                airshot::overlay_detail::AnnotationHandle::
                    bottom_right,
        L"selected shapes expose reliable resize handles");

    const Annotation resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            rectangle,
            airshot::overlay_detail::AnnotationHandle::
                bottom_right,
            POINT{50, 40},
            100,
            100);
    expect(
        resized.start.x == 10 &&
            resized.start.y == 10 &&
            resized.end.x == 50 &&
            resized.end.y == 40,
        L"shape handles resize the selected annotation");

    const Annotation aspect_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            rectangle,
            airshot::overlay_detail::AnnotationHandle::
                bottom_right,
            POINT{50, 30},
            100,
            100,
            true);
    expect(
        aspect_resized.end.x == 50 &&
            aspect_resized.end.y == 50,
        L"Shift preserves aspect ratio while resizing annotations");

    Annotation arrow = rectangle;
    arrow.tool = Tool::arrow;
    arrow.start = {30, 30};
    arrow.end = {50, 50};
    const Annotation endpoint_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            arrow,
            airshot::overlay_detail::AnnotationHandle::
                end_point,
            POINT{75, 62},
            100,
            100);
    expect(
        endpoint_resized.start.x == 30 &&
            endpoint_resized.start.y == 30 &&
            endpoint_resized.end.x == 75 &&
            endpoint_resized.end.y == 62,
        L"line and arrow endpoints can be re-edited");

    const POINT clone_offset =
        airshot::overlay_detail::preferred_clone_translation(
            rectangle,
            12,
            100,
            100);
    expect(
        clone_offset.x == 12 && clone_offset.y == 12,
        L"annotation duplication uses a visible in-bounds offset");
}

void test_config() {
    expect(airshot::AppConfig{}.ocr_engine == airshot::kDefaultOcrEngine &&
               airshot::kDefaultOcrEngine == airshot::kOcrEngineRapidV5Fast,
           L"default OCR engine is local high accuracy");

    airshot::AppConfig config;
    config.annotation_enabled = false;
    config.annotation_locked_tool = false;
    config.annotation_hidden_tools = L"pen,banana,rect,pen,close";
    config.annotation_highlight_alpha = 300;
    config.global_ocr_enabled = true;
    config.capture_hotkey = L"Ctrl+Shift+F9";
    config.global_ocr_hotkey = L"Ctrl+Alt+O";
    config.custom_color = L"#123456";
    config.tool_shortcut_select = L"Shift+S";
    config.tool_shortcut_rectangle = L"Shift+R";
    config.toolbar_order = L"rect,pen,close";
    config.text_font_family = L"Consolas";
    config.text_font_bold = true;
    config.text_font_italic = true;
    config.ocr_engine = std::wstring(airshot::kOcrEngineRapidV5Accurate);
    config.ocr_download_url = L"https://example.com/ocr-dependencies.json";
    const auto parsed = airshot::config_from_json(airshot::config_to_json(config));
    expect(parsed.has_value(), L"config JSON round trip parses");
    expect(parsed && !parsed->annotation_enabled && parsed->global_ocr_enabled &&
               !parsed->annotation_locked_tool && parsed->annotation_hidden_tools == L"rect,pen,close" &&
               parsed->annotation_highlight_alpha == 192 &&
               parsed->capture_hotkey == L"Ctrl+Shift+F9" && parsed->global_ocr_hotkey == L"Ctrl+Alt+O" &&
               parsed->custom_color == L"#123456" &&
               parsed->tool_shortcut_select == L"Shift+S" &&
               parsed->tool_shortcut_rectangle == L"Shift+R" &&
               parsed->toolbar_order == L"rect,pen,close" &&
               parsed->text_font_family == L"Consolas" &&
               parsed->text_font_bold == true &&
               parsed->text_font_italic == true &&
               parsed->ocr_engine == airshot::kOcrEngineRapidV5Accurate &&
               parsed->ocr_download_url == L"https://example.com/ocr-dependencies.json",
           L"config JSON round trip values");

    const auto future = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"enabled":false},"future":[null,true,{"name":"\u4E2D"}]})");
    expect(future && future->schema_version == 2 && !future->annotation_enabled &&
               future->annotation_locked_tool && future->annotation_hidden_tools.empty() &&
               future->annotation_highlight_alpha == 96 &&
               future->text_font_family == L"Microsoft YaHei" && !future->text_font_bold && !future->text_font_italic &&
               future->ocr_engine == airshot::kDefaultOcrEngine,
           L"config accepts unknown future fields and keeps annotation defaults");
    const auto legacy_serial = airshot::config_from_json(
        LR"({"annotation":{"nextSerial":37}})");
    expect(
        legacy_serial.has_value(),
        L"legacy persisted serial counters remain readable");
    expect(
        legacy_serial &&
            airshot::config_to_json(*legacy_serial).find(L"nextSerial") ==
                std::wstring::npos,
        L"legacy persisted serial counters are removed on save");
    expect(!airshot::config_from_json(L"{\"annotation\":[}"), L"config rejects malformed JSON");
    const auto old_numeric_ocr = airshot::config_from_json(LR"({"ocr":{"engine":1}})");
    expect(old_numeric_ocr && old_numeric_ocr->ocr_engine == airshot::kDefaultOcrEngine,
           L"config migrates old numeric OCR engine");
    const auto invalid_ocr = airshot::config_from_json(LR"({"ocr":{"engine":"banana"}})");
    expect(invalid_ocr && invalid_ocr->ocr_engine == airshot::kDefaultOcrEngine,
           L"config clamps invalid OCR engine");
    expect(airshot::normalize_ocr_engine(L"wechat") == airshot::kOcrEngineRapidV5Fast,
           L"config migrates legacy WeChat OCR engine");

    expect(airshot::normalize_annotation_hidden_tools(L"pen,unknown,RECT;pen close") == L"rect,pen,close",
           L"hidden annotation tools normalize, dedupe, and skip unknown values");
    expect(airshot::annotation_tool_hidden(L"rect,pen", L"PEN") &&
               !airshot::annotation_tool_hidden(L"rect,pen", L"arrow"),
           L"hidden annotation tool lookup");

    const auto hotkey = airshot::parse_hotkey(L"Ctrl+Alt+A");
    expect(hotkey && (hotkey->modifiers & MOD_CONTROL) && (hotkey->modifiers & MOD_ALT) &&
               hotkey->virtual_key == L'A',
           L"hotkey parser");
    expect(!airshot::parse_hotkey(L"Ctrl+Banana"), L"hotkey parser rejects invalid values");
}

void test_cli() {
    const std::vector<std::wstring> arguments{
        L"capture", L"screen", L"--monitor", L"primary", L"--output", L"file", L"--path", L"C:\\shot.png", L"--json"};
    const auto parsed = airshot::parse_cli(arguments);
    expect(parsed.code == airshot::ExitCode::success && parsed.json && !parsed.request_json.empty(),
           L"CLI screen command");

    const std::vector<std::wstring> invalid{L"capture", L"banana"};
    expect(airshot::parse_cli(invalid).code == airshot::ExitCode::invalid_arguments, L"CLI rejects invalid mode");

    airshot::CommandResponse response;
    response.code = airshot::ExitCode::success;
    response.message = L"ok";
    response.path = L"C:\\shot.png";
    const auto round_trip = airshot::response_from_json(airshot::response_to_json(response));
    expect(round_trip.code == airshot::ExitCode::success && round_trip.message == L"ok" &&
               round_trip.path == L"C:\\shot.png",
           L"response JSON round trip");
}

void test_ocr_join() {
    const std::vector<std::wstring> lines{L"第一行", L"second"};
    expect(airshot::join_ocr_lines(lines) == L"第一行\r\nsecond", L"OCR line join");
}

void test_ocr_dependency_manifest() {
    const auto manifest = airshot::parse_ocr_dependency_manifest(
        LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[)"
        LR"({"path":"rapidocr_api.dll","url":"https://example.com/rapidocr_api.dll","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1},)"
        LR"({"path":"onnxruntime.dll","url":"https://example.com/onnxruntime.dll","sha256":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB","size":2},)"
        LR"({"path":"rapidocr_runner.exe","url":"https://example.com/rapidocr_runner.exe","sha256":"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC","size":3},)"
        LR"({"path":"models/rapidocr-v5-fast/det.onnx","url":"https://example.com/v5-fast-det.onnx","sha256":"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD","size":4},)"
        LR"({"path":"models/rapidocr-v5-fast/rec.onnx","url":"https://example.com/v5-fast-rec.onnx","sha256":"EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE","size":5},)"
        LR"({"path":"models/rapidocr-v5-fast/cls.onnx","url":"https://example.com/v5-fast-cls.onnx","sha256":"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF","size":6},)"
        LR"({"path":"models/rapidocr-v5-fast/dict.txt","url":"https://example.com/v5-fast-dict.txt","sha256":"1111111111111111111111111111111111111111111111111111111111111111","size":7},)"
        LR"({"path":"models/rapidocr-v5-accurate/det.onnx","url":"https://example.com/v5-accurate-det.onnx","sha256":"2222222222222222222222222222222222222222222222222222222222222222","size":8},)"
        LR"({"path":"models/rapidocr-v5-accurate/rec.onnx","url":"https://example.com/v5-accurate-rec.onnx","sha256":"3333333333333333333333333333333333333333333333333333333333333333","size":9},)"
        LR"({"path":"models/rapidocr-v5-accurate/cls.onnx","url":"https://example.com/v5-accurate-cls.onnx","sha256":"4444444444444444444444444444444444444444444444444444444444444444","size":10},)"
        LR"({"path":"models/rapidocr-v5-accurate/dict.txt","url":"https://example.com/v5-accurate-dict.txt","sha256":"5555555555555555555555555555555555555555555555555555555555555555","size":11},)"
        LR"({"path":"models/rapidocr-v4-compat/det.onnx","url":"https://example.com/v4-compat-det.onnx","sha256":"6666666666666666666666666666666666666666666666666666666666666666","size":12},)"
        LR"({"path":"models/rapidocr-v4-compat/rec.onnx","url":"https://example.com/v4-compat-rec.onnx","sha256":"7777777777777777777777777777777777777777777777777777777777777777","size":13},)"
        LR"({"path":"models/rapidocr-v4-compat/cls.onnx","url":"https://example.com/v4-compat-cls.onnx","sha256":"8888888888888888888888888888888888888888888888888888888888888888","size":14},)"
        LR"({"path":"models/rapidocr-v4-compat/dict.txt","url":"https://example.com/v4-compat-dict.txt","sha256":"9999999999999999999999999999999999999999999999999999999999999999","size":15})"
        LR"(]})");
    expect(manifest && manifest->package_id == airshot::kRapidOcrOnnxPackageId && manifest->files.size() == 15,
           L"OCR dependency manifest parses required files");

    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"../rapidocr_api.dll","url":"https://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1}]})"),
           L"OCR dependency manifest rejects unsafe paths");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"http://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1}]})"),
           L"OCR dependency manifest rejects non-HTTPS URLs");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"https://example.com/a","sha256":"0000000000000000000000000000000000000000000000000000000000000000","size":1}]})"),
           L"OCR dependency manifest rejects zero SHA256");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"https://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":0}]})"),
           L"OCR dependency manifest rejects zero size");
}

void test_portable_runtime() {
    expect(airshot::version_is_newer(L"0.1.6", L"0.2.0"), L"portable version detects newer release");
    expect(!airshot::version_is_newer(L"0.2.0", L"0.1.6"), L"portable version rejects older release");
    expect(!airshot::version_is_newer(L"0.2", L"0.2.0"), L"portable version rejects malformed version");

    const auto manifest = airshot::parse_update_manifest(
        LR"({"version":"0.2.0","url":"https://example.com/AirScreenshot.exe","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","size":3})");
    expect(manifest && manifest->version == L"0.2.0" && manifest->size == 3 &&
               manifest->sha256 == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
           L"portable update manifest parses and normalizes");
    expect(!airshot::parse_update_manifest(
               LR"({"version":"0.2","url":"http://example.com/a.exe","sha256":"bad","size":0})"),
           L"portable update manifest rejects unsafe values");

    const auto path = std::filesystem::temp_directory_path() / L"airshot-sha256-test.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "abc";
    }
    expect(airshot::sha256_file(path) == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
           L"portable SHA256");
    airshot::UpdateManifest unsigned_manifest{
        L"0.2.0",
        L"https://example.com/AirScreenshot.exe",
        L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        3};
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects unsigned executable");
    unsigned_manifest.sha256 = L"AA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects wrong hash");
    unsigned_manifest.sha256 = L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    unsigned_manifest.size = 4;
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects wrong size");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    wchar_t windows_directory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH) > 0) {
        const auto wrong_signer = std::filesystem::path(windows_directory) / L"System32" / L"notepad.exe";
        if (std::filesystem::exists(wrong_signer)) {
            airshot::UpdateManifest wrong_signer_manifest{
                L"0.2.0",
                L"https://example.com/AirScreenshot.exe",
                airshot::sha256_file(wrong_signer),
                std::filesystem::file_size(wrong_signer)};
            expect(!airshot::verify_portable_executable(wrong_signer, wrong_signer_manifest),
                   L"portable update rejects wrong signer");
        }
    }

    expect(airshot::portable_startup_command(L"C:\\Apps\\Air Screenshot\\AirScreenshot.exe") ==
               L"\"C:\\Apps\\Air Screenshot\\AirScreenshot.exe\"",
           L"portable startup command quotes executable path");
}

}  // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    test_rect_and_bitmap();
    test_pin_recovery_layout();
    test_serial_counter_is_scoped_to_capture_session();
    test_annotation_geometry_and_history();
    test_config();
    test_cli();
    test_ocr_join();
    test_ocr_dependency_manifest();
    test_portable_runtime();
    if (failures == 0) {
        std::wcout << L"All tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
