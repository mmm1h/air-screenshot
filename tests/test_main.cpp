#include "airshot/bitmap.h"
#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/feature.h"
#include "airshot/ocr.h"

#include <winrt/base.h>

#include <iostream>

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
}

void test_config() {
    airshot::AppConfig config;
    config.annotation_enabled = false;
    config.global_ocr_enabled = true;
    config.capture_hotkey = L"Ctrl+Shift+F9";
    config.global_ocr_hotkey = L"Ctrl+Alt+\\\"O";
    config.custom_color = L"#123456";
    const auto parsed = airshot::config_from_json(airshot::config_to_json(config));
    expect(parsed.has_value(), L"config JSON round trip parses");
    expect(parsed && !parsed->annotation_enabled && parsed->global_ocr_enabled &&
               parsed->capture_hotkey == L"Ctrl+Shift+F9" && parsed->global_ocr_hotkey == L"Ctrl+Alt+\\\"O" &&
               parsed->custom_color == L"#123456",
           L"config JSON round trip values");

    const auto future = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"enabled":false},"future":[null,true,{"name":"\u4E2D"}]})");
    expect(future && future->schema_version == 2 && !future->annotation_enabled,
           L"config accepts unknown future fields");
    expect(!airshot::config_from_json(L"{\"annotation\":[}"), L"config rejects malformed JSON");

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

}  // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    test_rect_and_bitmap();
    test_config();
    test_cli();
    test_ocr_join();
    test_feature_registry();
    if (failures == 0) {
        std::wcout << L"All tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
