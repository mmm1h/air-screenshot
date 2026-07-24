#include "airshot/bitmap.h"
#include "airshot/capture.h"
#include "airshot/output.h"
#include "overlay_helpers.h"
#include "overlay_session.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << message << L'\n';
    }
}

[[nodiscard]] std::uint8_t channel(const airshot::Bitmap& bitmap, int x, int y, int value) {
    return bitmap.row(y)[static_cast<std::size_t>(x) * airshot::Bitmap::bytes_per_pixel +
                         static_cast<std::size_t>(value)];
}

[[nodiscard]] airshot::Bitmap solid_bitmap(int width, int height, COLORREF color) {
    airshot::Bitmap bitmap(width, height);
    for (int y = 0; y < height; ++y) {
        auto row = bitmap.row(y);
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            row[offset] = GetBValue(color);
            row[offset + 1] = GetGValue(color);
            row[offset + 2] = GetRValue(color);
            row[offset + 3] = 255;
        }
    }
    return bitmap;
}

void test_bitmap_invariants() {
    airshot::Bitmap bitmap(7, 5);
    expect(bitmap.valid() && !bitmap.empty(), L"valid bitmap construction");
    expect(bitmap.stride() == 28 && bitmap.stride_bytes() == 28, L"checked bitmap stride");
    expect(bitmap.row(-1).empty() && bitmap.row(5).empty(), L"row rejects invalid index");
    for (std::size_t index = 3; index < bitmap.pixels.size(); index += airshot::Bitmap::bytes_per_pixel) {
        expect(bitmap.pixels[index] == 255, L"new bitmap is opaque");
    }

    expect(airshot::Bitmap(-1, 4).empty(), L"negative dimensions rejected");
    expect(airshot::Bitmap(std::numeric_limits<int>::max(), 2).empty(), L"overflowing stride rejected");
    expect(!airshot::Bitmap::checked_byte_size(std::numeric_limits<int>::max(), 2),
           L"overflowing byte size rejected");

    bitmap.pixels.pop_back();
    expect(!bitmap.valid() && bitmap.row(0).empty(), L"mismatched compatibility fields rejected");
}

void test_blit_and_holes() {
    airshot::Bitmap left(2, 2);
    airshot::Bitmap right(2, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            auto left_row = left.row(y);
            auto right_row = right.row(y);
            left_row[static_cast<std::size_t>(x) * 4] = 11;
            right_row[static_cast<std::size_t>(x) * 4] = 22;
        }
    }

    std::vector<airshot::MonitorSnapshot> monitors;
    airshot::MonitorSnapshot first;
    first.bounds = {0, 0, 2, 2};
    first.bitmap = left;
    monitors.push_back(std::move(first));
    airshot::MonitorSnapshot second;
    second.bounds = {4, 0, 6, 2};
    second.bitmap = right;
    monitors.push_back(std::move(second));

    const airshot::Bitmap composed = airshot::compose_selection(monitors, {0, 0, 6, 2});
    expect(composed.valid(), L"monitor composition succeeds");
    expect(channel(composed, 0, 0, 0) == 11 && channel(composed, 5, 0, 0) == 22,
           L"monitor pixels are composed");
    expect(channel(composed, 2, 0, 0) == 0 && channel(composed, 3, 0, 0) == 0,
           L"desktop holes are black");
    expect(channel(composed, 2, 0, 3) == 255 && channel(composed, 3, 0, 3) == 255,
           L"desktop holes are opaque");

    airshot::Bitmap clipped_target(2, 1);
    airshot::Bitmap clipped_source(3, 1);
    clipped_source.row(0)[0] = 1;
    clipped_source.row(0)[4] = 2;
    clipped_source.row(0)[8] = 3;
    clipped_source.row(0)[3] = 0;
    airshot::blit(clipped_source, {0, 0, 3, 1}, clipped_target, POINT{-1, 0});
    expect(channel(clipped_target, 0, 0, 0) == 2 && channel(clipped_target, 1, 0, 0) == 3,
           L"blit clips negative target origin");
    expect(channel(clipped_target, 0, 0, 3) == 255, L"blit normalizes alpha");

    airshot::Bitmap aliased = clipped_source;
    const airshot::Bitmap before_alias = aliased;
    airshot::blit(aliased, {0, 0, 2, 1}, aliased, POINT{1, 0});
    expect(aliased.width == before_alias.width && aliased.height == before_alias.height &&
               aliased.pixels == before_alias.pixels,
           L"blit rejects aliased source and target");
}

[[nodiscard]] int naive_box_channel(const airshot::Bitmap& bitmap,
                                    int left,
                                    int top,
                                    int right,
                                    int bottom,
                                    int x,
                                    int y,
                                    int radius,
                                    int channel_index) {
    int sum = 0;
    int count = 0;
    for (int sample_y = std::max(top, y - radius); sample_y <= std::min(bottom - 1, y + radius); ++sample_y) {
        for (int sample_x = std::max(left, x - radius); sample_x <= std::min(right - 1, x + radius); ++sample_x) {
            sum += channel(bitmap, sample_x, sample_y, channel_index);
            ++count;
        }
    }
    return sum / count;
}

void test_linear_blur() {
    airshot::Bitmap source(17, 13);
    for (int y = 0; y < source.height; ++y) {
        auto row = source.row(y);
        for (int x = 0; x < source.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            row[offset] = static_cast<std::uint8_t>((x * 17 + y * 11) & 0xFF);
            row[offset + 1] = static_cast<std::uint8_t>((x * 3 + y * 29) & 0xFF);
            row[offset + 2] = static_cast<std::uint8_t>((x * 31 + y * 5) & 0xFF);
            row[offset + 3] = 19;
        }
    }
    const airshot::Bitmap original = source;
    const airshot::RectI region{2, 3, 15, 12};
    airshot::blur_rect(source, region, 4);

    expect(source.valid(), L"blur preserves bitmap validity");
    expect(std::equal(source.row(0).begin(), source.row(0).end(), original.row(0).begin()),
           L"blur preserves pixels outside the region");
    for (int y = region.top; y < region.bottom; ++y) {
        for (int x = region.left; x < region.right; ++x) {
            for (int channel_index = 0; channel_index < 3; ++channel_index) {
                const int expected = naive_box_channel(
                    original, region.left, region.top, region.right, region.bottom, x, y, 4, channel_index);
                const int actual = channel(source, x, y, channel_index);
                expect(std::abs(actual - expected) <= 1, L"linear blur matches box blur");
            }
            expect(channel(source, x, y, 3) == 255, L"blur output is opaque");
        }
    }

    airshot::Bitmap performance_source(640, 360);
    const auto start = std::chrono::steady_clock::now();
    airshot::blur_rect(performance_source, {0, 0, 640, 360}, 128);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(elapsed < std::chrono::seconds(5), L"large-radius blur remains linear-time");
}

void test_ordered_annotation_rendering() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    Annotation line;
    line.tool = Tool::line;
    line.start = {2, 16};
    line.end = {30, 16};
    line.color = RGB(255, 0, 0);
    line.width = 5.0F;

    Annotation highlight;
    highlight.tool = Tool::highlight;
    highlight.points = {{16, 2}, {16, 30}};
    highlight.color = RGB(0, 255, 0);
    highlight.width = 4.0F;
    highlight.alpha = 128;

    airshot::AppConfig config;
    const auto cpu_above_gdi = airshot::overlay_detail::render_annotations(
        solid_bitmap(32, 32, RGB(255, 255, 255)),
        {line, highlight},
        config);
    const auto gdi_above_cpu = airshot::overlay_detail::render_annotations(
        solid_bitmap(32, 32, RGB(255, 255, 255)),
        {highlight, line},
        config);

    expect(cpu_above_gdi.valid() && gdi_above_cpu.valid(),
           L"ordered annotation renderer returns valid bitmaps");
    expect(channel(cpu_above_gdi, 16, 16, 1) > 80 &&
               channel(cpu_above_gdi, 16, 16, 2) < 200,
           L"later CPU annotation blends over an earlier GDI annotation");
    expect(channel(gdi_above_cpu, 16, 16, 2) > 240 &&
               channel(gdi_above_cpu, 16, 16, 1) < 20,
           L"later GDI annotation remains above an earlier CPU annotation");
}

void test_highlight_alpha_is_sampling_independent() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    Annotation sparse;
    sparse.tool = Tool::highlight;
    sparse.points = {{4, 16}, {59, 16}};
    sparse.color = RGB(0, 0, 0);
    sparse.width = 4.0F;
    sparse.alpha = 96;

    Annotation dense = sparse;
    dense.points.clear();
    for (int x = 4; x <= 59; ++x) {
        dense.points.push_back({x, 16});
    }

    const airshot::AppConfig config;
    const auto sparse_result = airshot::overlay_detail::render_annotations(
        solid_bitmap(64, 32, RGB(255, 255, 255)),
        {sparse},
        config);
    const auto dense_result = airshot::overlay_detail::render_annotations(
        solid_bitmap(64, 32, RGB(255, 255, 255)),
        {dense},
        config);

    expect(sparse_result.valid() && dense_result.valid(),
           L"highlight renderer accepts sparse and dense sampling");
    expect(sparse_result.pixels == dense_result.pixels,
           L"highlight opacity does not depend on pointer sampling density");
    expect(channel(sparse_result, 32, 16, 0) == 159 &&
               channel(sparse_result, 32, 16, 1) == 159 &&
               channel(sparse_result, 32, 16, 2) == 159,
           L"highlight center applies configured alpha exactly once");
    expect(channel(sparse_result, 32, 16, 3) == 255,
           L"highlight output remains opaque");
}

void test_overlay_close_lifecycle() {
    bool completed = false;
    airshot::RegionResult completion_result;
    airshot::overlay_detail::OverlaySession session(
        airshot::RegionRequest{},
        [&](airshot::RegionResult result) {
            completed = true;
            completion_result = std::move(result);
        });
    airshot::MonitorSnapshot monitor;
    monitor.bounds = {100, 100, 132, 132};
    monitor.bitmap = solid_bitmap(32, 32, RGB(255, 255, 255));
    airshot::overlay_detail::OverlayWindow window(session, monitor);

    expect(window.create(), L"synthetic overlay window is created");
    const HWND close_handle = window.hwnd();
    if (close_handle) {
        SendMessageW(close_handle, WM_CLOSE, 0, 0);
        expect(completed &&
                   completion_result.code == airshot::ExitCode::user_cancelled,
               L"WM_CLOSE cancels the overlay session");
        expect(IsWindow(close_handle),
               L"WM_CLOSE leaves destruction to the session owner");
        window.destroy();
        expect(window.hwnd() == nullptr && !IsWindow(close_handle),
               L"owned overlay destruction clears the cached window handle");
    }

    expect(window.create(), L"overlay window can be recreated after owner cleanup");
    const HWND external_handle = window.hwnd();
    if (external_handle) {
        DestroyWindow(external_handle);
        expect(window.hwnd() == nullptr && !IsWindow(external_handle),
               L"WM_NCDESTROY clears a window destroyed outside the owner");
    }
}

void test_text_prompt_deactivation_cancels_draft() {
    bool completed = false;
    std::optional<std::wstring> result;
    const HWND prompt = airshot::overlay_detail::show_text_prompt(
        nullptr,
        POINT{20, 20},
        RGB(255, 255, 255),
        16.0F,
        false,
        [&](std::optional<std::wstring> text) {
            result = std::move(text);
            completed = true;
        });
    expect(prompt != nullptr, L"text prompt is created for deactivation testing");
    if (!prompt) {
        return;
    }

    const HWND edit = GetDlgItem(prompt, 100);
    expect(edit != nullptr && SetWindowTextW(edit, L"draft"),
           L"text prompt accepts a draft before deactivation");
    SendMessageW(prompt, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);

    MSG message{};
    while (!completed && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    expect(completed && !result.has_value() && !IsWindow(prompt),
           L"deactivating a text prompt cancels instead of committing its draft");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }
}

[[nodiscard]] std::uint64_t total_darkness(const airshot::Bitmap& bitmap) {
    std::uint64_t darkness = 0;
    for (int y = 0; y < bitmap.height; ++y) {
        const auto row = bitmap.row(y);
        for (int x = 0; x < bitmap.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            darkness += 255U - row[offset];
            darkness += 255U - row[offset + 1];
            darkness += 255U - row[offset + 2];
        }
    }
    return darkness;
}

void test_watermark_alpha() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    const auto base = solid_bitmap(320, 160, RGB(255, 255, 255));
    Annotation watermark;
    watermark.tool = Tool::watermark;
    watermark.text = L"Airshot";
    watermark.color = RGB(0, 0, 0);
    watermark.width = 24.0F;

    airshot::AppConfig config;
    watermark.alpha = 0;
    const auto transparent =
        airshot::overlay_detail::render_annotations(base, {watermark}, config);
    watermark.alpha = 32;
    const auto faint = airshot::overlay_detail::render_annotations(base, {watermark}, config);
    watermark.alpha = 192;
    const auto strong = airshot::overlay_detail::render_annotations(base, {watermark}, config);

    expect(transparent.valid() && transparent.pixels == base.pixels,
           L"zero-alpha watermark leaves the image unchanged");
    const std::uint64_t faint_darkness = total_darkness(faint);
    const std::uint64_t strong_darkness = total_darkness(strong);
    expect(faint_darkness > 0, L"nonzero-alpha watermark is visible");
    expect(strong_darkness > faint_darkness * 2,
           L"watermark opacity follows annotation alpha");
}

void test_atomic_png_output() {
    const auto directory = std::filesystem::temp_directory_path() /
                           std::format(L"airshot-image-test-{}-{}", GetCurrentProcessId(), GetTickCount64());
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    expect(!code, L"temporary output directory created");
    if (code) {
        return;
    }

    const auto output = directory / L"capture.png";
    {
        std::ofstream sentinel(output, std::ios::binary | std::ios::trunc);
        sentinel << "original";
    }
    airshot::Bitmap invalid(2, 2);
    invalid.pixels.pop_back();
    std::wstring error;
    expect(!airshot::save_png(invalid, output, &error), L"invalid bitmap is not saved");
    {
        std::ifstream sentinel(output, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(sentinel)), std::istreambuf_iterator<char>());
        expect(contents == "original", L"failed save preserves original file");
    }

    airshot::Bitmap valid(3, 2);
    valid.row(0)[0] = 200;
    expect(airshot::save_png(valid, output, &error), L"valid PNG saves atomically");
    std::ifstream png(output, std::ios::binary);
    unsigned char signature[8]{};
    png.read(reinterpret_cast<char*>(signature), sizeof(signature));
    const unsigned char expected_signature[8]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    expect(std::equal(std::begin(signature), std::end(signature), std::begin(expected_signature)),
           L"saved file has PNG signature");
    const DWORD output_attributes = GetFileAttributesW(output.c_str());
    expect(output_attributes != INVALID_FILE_ATTRIBUTES &&
               (output_attributes & FILE_ATTRIBUTE_TEMPORARY) == 0,
           L"saved PNG does not retain temporary-file attributes");

    expect(airshot::resolve_output_path(L"shot") == std::filesystem::path(L"shot.png"),
           L"extensionless output becomes PNG file");
    expect(!airshot::save_png(valid, directory / L"capture.jpg", &error),
           L"non-PNG extension is rejected");
    error.clear();
    expect(!airshot::save_png(valid, directory / L"CON.png", &error) &&
               error.find(L"保留设备名") != std::wstring::npos,
           L"Windows reserved device names are rejected before file creation");

    const auto existing_directory_output = airshot::resolve_output_path(directory.wstring());
    const auto second_existing_directory_output =
        airshot::resolve_output_path(directory.wstring());
    expect(existing_directory_output.parent_path() == directory &&
               second_existing_directory_output.parent_path() == directory &&
               existing_directory_output.extension() == L".png" &&
               second_existing_directory_output.extension() == L".png" &&
               existing_directory_output != second_existing_directory_output,
           L"consecutive directory outputs receive distinct generated PNG filenames");

    const auto nonexistent_directory = directory / L"created-on-save";
    const auto trailing_directory_output =
        airshot::resolve_output_path(nonexistent_directory.wstring() + L"\\");
    expect(trailing_directory_output.parent_path() == nonexistent_directory &&
               trailing_directory_output.extension() == L".png",
           L"nonexistent trailing-separator output receives a generated PNG filename");
    expect(airshot::save_png(valid, trailing_directory_output, &error) &&
               std::filesystem::is_regular_file(trailing_directory_output),
           L"saving creates a requested trailing-separator directory");
    {
        std::ifstream generated_png(trailing_directory_output, std::ios::binary);
        unsigned char generated_signature[8]{};
        generated_png.read(reinterpret_cast<char*>(generated_signature), sizeof(generated_signature));
        expect(std::equal(std::begin(generated_signature),
                          std::end(generated_signature),
                          std::begin(expected_signature)),
               L"generated directory output has PNG signature");
    }

    bool has_temporary_file = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (entry.path().filename().wstring().starts_with(L".airshot-")) {
            has_temporary_file = true;
        }
    }
    expect(!has_temporary_file, L"temporary files are cleaned up");
    std::filesystem::remove_all(directory, code);
}

}  // namespace

int wmain() {
    test_bitmap_invariants();
    test_blit_and_holes();
    test_linear_blur();
    test_ordered_annotation_rendering();
    test_highlight_alpha_is_sampling_independent();
    test_overlay_close_lifecycle();
    test_text_prompt_deactivation_cancels_draft();
    test_watermark_alpha();
    test_atomic_png_output();
    if (failures == 0) {
        std::wcout << L"All image pipeline tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
