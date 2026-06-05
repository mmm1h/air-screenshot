#include "airshot/bitmap.h"
#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/feature.h"
#include "airshot/ocr.h"
#include "airshot/portable.h"
#include "airshot/output.h"

#include <winrt/base.h>

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

void test_config() {
    airshot::AppConfig config;
    config.annotation_enabled = false;
    config.annotation_locked_tool = false;
    config.annotation_hidden_tools = L"pen,banana,rect,pen,close";
    config.annotation_highlight_alpha = 300;
    config.annotation_next_serial = 12;
    config.global_ocr_enabled = true;
    config.capture_hotkey = L"Ctrl+Shift+F9";
    config.global_ocr_hotkey = L"Ctrl+Alt+\\\"O";
    config.custom_color = L"#123456";
    config.tool_shortcut_select = L"Shift+S";
    config.tool_shortcut_rectangle = L"Shift+R";
    config.toolbar_order = L"rect,pen,close";
    config.text_font_family = L"Consolas";
    config.text_font_bold = true;
    config.text_font_italic = true;
    const auto parsed = airshot::config_from_json(airshot::config_to_json(config));
    expect(parsed.has_value(), L"config JSON round trip parses");
    expect(parsed && !parsed->annotation_enabled && parsed->global_ocr_enabled &&
               !parsed->annotation_locked_tool && parsed->annotation_hidden_tools == L"rect,pen,close" &&
               parsed->annotation_highlight_alpha == 192 && parsed->annotation_next_serial == 12 &&
               parsed->capture_hotkey == L"Ctrl+Shift+F9" && parsed->global_ocr_hotkey == L"Ctrl+Alt+\\\"O" &&
               parsed->custom_color == L"#123456" &&
               parsed->tool_shortcut_select == L"Shift+S" &&
               parsed->tool_shortcut_rectangle == L"Shift+R" &&
               parsed->toolbar_order == L"rect,pen,close" &&
               parsed->text_font_family == L"Consolas" &&
               parsed->text_font_bold == true &&
               parsed->text_font_italic == true,
           L"config JSON round trip values");

    const auto future = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"enabled":false},"future":[null,true,{"name":"\u4E2D"}]})");
    expect(future && future->schema_version == 2 && !future->annotation_enabled &&
               future->annotation_locked_tool && future->annotation_hidden_tools.empty() &&
               future->annotation_highlight_alpha == 96 && future->annotation_next_serial == 1 &&
               future->text_font_family == L"Microsoft YaHei" && !future->text_font_bold && !future->text_font_italic,
           L"config accepts unknown future fields and keeps annotation defaults");
    expect(!airshot::config_from_json(L"{\"annotation\":[}"), L"config rejects malformed JSON");

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

void test_feature_registry() {
    airshot::AppConfig config;
    airshot::FeatureRegistry registry;
    expect(!registry.loaded(L"annotation") && !registry.loaded(L"ocr") && !registry.loaded(L"shell"),
           L"feature modules start unloaded");
    expect(registry.list(config).size() == 3 && !registry.loaded(L"annotation"),
           L"feature listing does not load modules");
    expect(registry.activate(L"annotation", config) != nullptr && registry.loaded(L"annotation"),
           L"feature activates lazily");
    config.annotation_enabled = false;
    registry.unload_disabled(config);
    expect(!registry.loaded(L"annotation") && registry.activate(L"annotation", config) == nullptr,
           L"disabled feature unloads and cannot activate");
}

void test_clipboard_formats() {
    airshot::Bitmap source(2, 2);
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        source.pixels[index] = 255;
    }
    std::wstring error;
    expect(airshot::copy_bitmap_to_clipboard(source, &error), L"copy_bitmap_to_clipboard works");

    if (OpenClipboard(nullptr)) {
        UINT format = 0;
        bool has_png = false;
        bool has_image_png = false;
        while ((format = EnumClipboardFormats(format)) != 0) {
            wchar_t name[256]{};
            if (GetClipboardFormatNameW(format, name, 256) > 0) {
                std::wstring sname(name);
                if (sname == L"PNG") has_png = true;
                if (sname == L"image/png") has_image_png = true;
            }
        }
        CloseClipboard();
        expect(has_png, L"clipboard has PNG format");
        expect(has_image_png, L"clipboard has image/png format");
    }
}

}  // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    test_rect_and_bitmap();
    test_config();
    test_cli();
    test_ocr_join();
    test_portable_runtime();
    test_feature_registry();
    test_clipboard_formats();
    if (failures == 0) {
        std::wcout << L"All tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
