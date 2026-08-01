#include "airshot/bitmap.h"
#include "airshot/capture.h"
#include "airshot/output.h"
#include "airshot/output_decorations.h"
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

[[nodiscard]] airshot::Bitmap patterned_bitmap(int width, int height) {
    airshot::Bitmap bitmap(width, height);
    for (int y = 0; y < height; ++y) {
        auto row = bitmap.row(y);
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            row[offset] = static_cast<std::uint8_t>((x * 31 + y * 7) & 0xFF);
            row[offset + 1] = static_cast<std::uint8_t>((x * 11 + y * 23) & 0xFF);
            row[offset + 2] = static_cast<std::uint8_t>((x * 3 + y * 37) & 0xFF);
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

void test_rounded_corner_alpha_mask() {
    airshot::Bitmap bitmap = solid_bitmap(12, 8, RGB(12, 34, 56));
    airshot::apply_rounded_corner_mask(bitmap, 4);

    expect(channel(bitmap, 0, 0, 3) == 0,
           L"rounded output clears the extreme corner");
    expect(channel(bitmap, 1, 1, 3) > 0 && channel(bitmap, 1, 1, 3) < 255,
           L"rounded output antialiases the curved edge");
    expect(channel(bitmap, 5, 0, 3) == 255 && channel(bitmap, 5, 4, 3) == 255,
           L"rounded output preserves straight edges and the center");
    expect(channel(bitmap, 11, 0, 3) == channel(bitmap, 0, 0, 3) &&
               channel(bitmap, 11, 7, 3) == channel(bitmap, 0, 0, 3),
           L"rounded output is symmetric across all corners");

    airshot::Bitmap unchanged = solid_bitmap(5, 3, RGB(1, 2, 3));
    airshot::apply_rounded_corner_mask(unchanged, 0);
    expect(channel(unchanged, 0, 0, 3) == 255,
           L"zero corner radius preserves opaque output");

    airshot::Bitmap clamped = solid_bitmap(4, 2, RGB(1, 2, 3));
    airshot::apply_rounded_corner_mask(clamped, 200);
    expect(channel(clamped, 0, 0, 3) < 255 &&
               channel(clamped, 1, 0, 3) == 255,
           L"oversized corner radius is safely clamped");
}

void test_alpha_compositing_for_legacy_outputs() {
    airshot::Bitmap bitmap(2, 1);
    bitmap.pixels = {
        20, 40, 60, 0,
        10, 30, 50, 128,
    };
    airshot::composite_onto_background(bitmap, 255, 255, 255);

    expect(channel(bitmap, 0, 0, 0) == 255 &&
               channel(bitmap, 0, 0, 1) == 255 &&
               channel(bitmap, 0, 0, 2) == 255 &&
               channel(bitmap, 0, 0, 3) == 255,
           L"fully transparent legacy pixels become the background");
    expect(channel(bitmap, 1, 0, 0) == 132 &&
               channel(bitmap, 1, 0, 1) == 142 &&
               channel(bitmap, 1, 0, 2) == 152 &&
               channel(bitmap, 1, 0, 3) == 255,
           L"partial alpha is composited instead of discarded");
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
    expect(
        airshot::blur_rect(source, region, 4) ==
            airshot::PrivacyEffectResult::applied,
        L"rectangle blur reports a completed privacy effect");

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
    expect(
        airshot::blur_rect(
            performance_source,
            {0, 0, 640, 360},
            128) == airshot::PrivacyEffectResult::applied,
        L"large-radius rectangle blur completes within its default budget");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(elapsed < std::chrono::seconds(5), L"large-radius blur remains linear-time");

    auto constrained_source = patterned_bitmap(32, 24);
    const auto constrained_original = constrained_source.pixels;
    expect(
        airshot::blur_rect(
            constrained_source,
            {2, 3, 30, 22},
            4,
            1) == airshot::PrivacyEffectResult::resource_limit,
        L"rectangle blur reports a resource limit before changing pixels");
    expect(
        constrained_source.pixels == constrained_original,
        L"failed rectangle blur leaves privacy output byte-for-byte unchanged");
}

void test_privacy_stroke_uses_one_union_mask() {
    using airshot::PrivacyEffectMode;
    using airshot::PrivacyEffectOptions;
    using airshot::PrivacyEffectResult;

    const std::vector<POINT> sparse{{8, 32}, {119, 32}};
    std::vector<POINT> dense;
    for (int x = 8; x <= 119; ++x) {
        dense.push_back({x, 32});
    }
    const std::vector<POINT> self_crossing{
        {8, 32}, {119, 32}, {8, 32}, {119, 32}};

    for (const PrivacyEffectOptions options : {
             PrivacyEffectOptions{PrivacyEffectMode::mosaic, 8, RGB(0, 0, 0)},
             PrivacyEffectOptions{PrivacyEffectMode::blur, 4, RGB(0, 0, 0)},
             PrivacyEffectOptions{PrivacyEffectMode::solid, 255, RGB(17, 43, 91)},
         }) {
        auto sparse_result = patterned_bitmap(128, 64);
        auto dense_result = sparse_result;
        auto self_crossing_result = sparse_result;
        const auto sparse_status = airshot::apply_privacy_stroke(
            sparse_result, sparse, 6, options);
        const auto dense_status = airshot::apply_privacy_stroke(
            dense_result, dense, 6, options);
        const auto crossing_status = airshot::apply_privacy_stroke(
            self_crossing_result, self_crossing, 6, options);
        expect(
            sparse_status == PrivacyEffectResult::applied &&
                dense_status == PrivacyEffectResult::applied &&
                crossing_status == PrivacyEffectResult::applied,
            L"all privacy modes accept sparse, dense, and self-crossing strokes");
        expect(
            sparse_result.pixels == dense_result.pixels,
            L"privacy coverage is independent of pointer sampling density");
        expect(
            sparse_result.pixels == self_crossing_result.pixels,
            L"self-intersections apply each privacy effect at most once per pixel");
    }
}

void test_privacy_mosaic_is_globally_anchored() {
    airshot::Bitmap bitmap(16, 12);
    for (int y = 0; y < bitmap.height; ++y) {
        auto row = bitmap.row(y);
        for (int x = 0; x < bitmap.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            row[offset] = static_cast<std::uint8_t>(x + y * 16);
            row[offset + 1] = static_cast<std::uint8_t>(x * 3 + y);
            row[offset + 2] = static_cast<std::uint8_t>(x + y * 5);
        }
    }
    const airshot::Bitmap original = bitmap;
    const std::vector<POINT> points{{5, 5}, {6, 5}};
    const auto status = airshot::apply_privacy_stroke(
        bitmap,
        points,
        2,
        {airshot::PrivacyEffectMode::mosaic, 4, RGB(0, 0, 0)});

    std::array<unsigned int, 3> sums{};
    for (int y = 4; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            for (int value = 0; value < 3; ++value) {
                sums[static_cast<std::size_t>(value)] +=
                    channel(original, x, y, value);
            }
        }
    }
    expect(status == airshot::PrivacyEffectResult::applied,
           L"globally anchored mosaic stroke is applied");
    expect(channel(bitmap, 5, 5, 0) == sums[0] / 16U &&
               channel(bitmap, 5, 5, 1) == sums[1] / 16U &&
               channel(bitmap, 5, 5, 2) == sums[2] / 16U,
           L"mosaic samples the global 4x4 grid instead of the stroke bounds");
    expect(channel(bitmap, 1, 1, 0) == channel(original, 1, 1, 0),
           L"globally anchored mosaic preserves pixels outside the union mask");
}

void test_privacy_solid_fill_and_boundary_clipping() {
    auto solid = patterned_bitmap(20, 20);
    for (std::size_t offset = 3; offset < solid.pixels.size(); offset += 4) {
        solid.pixels[offset] = 19;
    }
    const auto solid_original = solid;
    const auto status = airshot::apply_privacy_stroke(
        solid,
        std::array<POINT, 1>{POINT{10, 10}},
        4,
        {airshot::PrivacyEffectMode::solid, 255, RGB(12, 34, 56)});
    bool exact_interior = true;
    for (int y = 7; y <= 13; ++y) {
        for (int x = 7; x <= 13; ++x) {
            const int delta_x = x - 10;
            const int delta_y = y - 10;
            if (delta_x * delta_x + delta_y * delta_y > 12) {
                continue;
            }
            exact_interior = exact_interior &&
                             channel(solid, x, y, 0) == 56 &&
                             channel(solid, x, y, 1) == 34 &&
                             channel(solid, x, y, 2) == 12 &&
                             channel(solid, x, y, 3) == 255;
        }
    }
    expect(status == airshot::PrivacyEffectResult::applied && exact_interior,
           L"opaque solid privacy fill writes every fully covered BGRA pixel exactly");
    expect(channel(solid, 14, 10, 0) == 56 &&
               channel(solid, 14, 10, 1) == 34 &&
               channel(solid, 14, 10, 2) == 12 &&
               channel(solid, 14, 10, 3) == 255,
           L"opaque solid fill fully redacts partially covered antialiased edge pixels");
    expect(channel(solid, 15, 10, 0) == channel(solid_original, 15, 10, 0) &&
               channel(solid, 15, 10, 3) == 19,
           L"opaque solid fill preserves pixels outside the coverage edge");

    for (const airshot::PrivacyEffectOptions options : {
             airshot::PrivacyEffectOptions{
                 airshot::PrivacyEffectMode::mosaic, 6, RGB(0, 0, 0)},
             airshot::PrivacyEffectOptions{
                 airshot::PrivacyEffectMode::blur, 3, RGB(0, 0, 0)},
             airshot::PrivacyEffectOptions{
                 airshot::PrivacyEffectMode::solid, 255, RGB(1, 2, 3)},
         }) {
        auto clipped = patterned_bitmap(24, 16);
        const auto original = clipped;
        const std::array<POINT, 2> boundary_points{
            POINT{-8, -5}, POINT{4, 3}};
        expect(
            airshot::apply_privacy_stroke(clipped, boundary_points, 5, options) ==
                airshot::PrivacyEffectResult::applied,
            L"privacy stroke clips safely at the top-left bitmap boundary");
        expect(
            channel(clipped, 23, 15, 0) == channel(original, 23, 15, 0) &&
                channel(clipped, 23, 15, 1) == channel(original, 23, 15, 1) &&
                channel(clipped, 23, 15, 2) == channel(original, 23, 15, 2),
            L"boundary clipping does not touch the opposite bitmap corner");
    }
}

void test_privacy_noop_and_resource_contract() {
    using airshot::PrivacyEffectMode;
    using airshot::PrivacyEffectOptions;
    using airshot::PrivacyEffectResult;

    const auto original = patterned_bitmap(40, 24);
    for (const PrivacyEffectMode mode : {
             PrivacyEffectMode::mosaic,
             PrivacyEffectMode::blur,
             PrivacyEffectMode::solid,
         }) {
        auto zero_strength = original;
        expect(
            airshot::apply_privacy_stroke(
                zero_strength,
                std::array<POINT, 2>{POINT{2, 2}, POINT{30, 20}},
                4,
                PrivacyEffectOptions{mode, 0, RGB(4, 5, 6)}) ==
                    PrivacyEffectResult::no_change &&
                zero_strength.pixels == original.pixels,
            L"zero-strength privacy effects are byte-for-byte no-ops");

        auto empty_mask = original;
        expect(
            airshot::apply_privacy_stroke(
                empty_mask,
                {},
                4,
                PrivacyEffectOptions{mode, 8, RGB(4, 5, 6)}) ==
                    PrivacyEffectResult::no_change &&
                empty_mask.pixels == original.pixels,
            L"an empty privacy mask is a byte-for-byte no-op");
    }

    airshot::PrivacyMaskLimits tiny_limits;
    tiny_limits.max_mask_pixels = 64;
    auto limited = original;
    expect(
        airshot::apply_privacy_stroke(
            limited,
            std::array<POINT, 2>{POINT{0, 0}, POINT{39, 23}},
            5,
            {PrivacyEffectMode::solid, 255, RGB(0, 0, 0)},
            tiny_limits) == PrivacyEffectResult::resource_limit &&
            limited.pixels == original.pixels,
        L"an oversized union mask is rejected before the bitmap changes");

    std::vector<POINT> too_many(
        airshot::PrivacyMaskLimits::hard_max_input_points + 1,
        POINT{8, 8});
    auto point_limited = original;
    expect(
        airshot::apply_privacy_stroke(
            point_limited,
            too_many,
            3,
            {PrivacyEffectMode::solid, 255, RGB(0, 0, 0)}) ==
                PrivacyEffectResult::resource_limit &&
            point_limited.pixels == original.pixels,
        L"input beyond the documented hard point cap is rejected without mutation");
}

void test_privacy_large_stroke_is_bounded() {
    static_assert(
        airshot::PrivacyMaskLimits::hard_max_mask_pixels >=
        static_cast<std::size_t>(7680) * 4320,
        "the default privacy mask budget must cover one 8K UHD frame");

    auto bitmap = patterned_bitmap(3840, 2160);
    std::vector<POINT> points;
    points.reserve(airshot::PrivacyMaskLimits::hard_max_input_points);
    for (std::size_t index = 0;
         index < airshot::PrivacyMaskLimits::hard_max_input_points;
         ++index) {
        const int x = static_cast<int>(
            index * 3839U /
            (airshot::PrivacyMaskLimits::hard_max_input_points - 1));
        const int y = 1080 + static_cast<int>(index % 7U) - 3;
        points.push_back({x, y});
    }

    const auto start = std::chrono::steady_clock::now();
    const auto status = airshot::apply_privacy_stroke(
        bitmap,
        points,
        5,
        {airshot::PrivacyEffectMode::solid, 255, RGB(9, 19, 29)});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(status == airshot::PrivacyEffectResult::applied && bitmap.valid(),
           L"a 100,000-point stroke on a 4K bitmap is resampled and applied");
    expect(elapsed < std::chrono::seconds(15),
           L"a maximum-size input stroke has bounded rasterization time");
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

    Annotation self_overlapping = sparse;
    self_overlapping.points = {
        {4, 16}, {59, 16}, {4, 16}, {59, 16}, {4, 16}};
    const auto self_overlapping_result =
        airshot::overlay_detail::render_annotations(
            solid_bitmap(64, 32, RGB(255, 255, 255)),
            {self_overlapping},
            config);
    expect(
        self_overlapping_result.valid() &&
            channel(self_overlapping_result, 32, 16, 0) == 159 &&
            channel(self_overlapping_result, 32, 16, 1) == 159 &&
            channel(self_overlapping_result, 32, 16, 2) == 159,
        L"dense and self-crossing highlight strokes apply alpha once through one coverage mask");

    Annotation normalized_sparse = sparse;
    Annotation normalized_dense = dense;
    airshot::overlay_detail::normalize_annotation_stroke(normalized_sparse);
    airshot::overlay_detail::normalize_annotation_stroke(normalized_dense);
    const bool same_normalized_points =
        normalized_sparse.points.size() == normalized_dense.points.size() &&
        std::equal(
            normalized_sparse.points.begin(),
            normalized_sparse.points.end(),
            normalized_dense.points.begin(),
            [](POINT left, POINT right) {
                return left.x == right.x && left.y == right.y;
            });
    expect(
        same_normalized_points,
        L"highlight preview and commit normalization is independent of pointer event density");
}

void test_effect_strokes_are_sampling_independent() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;
    using airshot::overlay_detail::resample_polyline;

    const std::vector<POINT> sparse{{8, 24}, {87, 24}};
    std::vector<POINT> dense;
    for (int x = 8; x <= 87; ++x) {
        dense.push_back({x, 24});
    }

    const auto normalized_sparse = resample_polyline(sparse, 7.0);
    const auto normalized_dense = resample_polyline(dense, 7.0);
    const auto points_equal = [](const std::vector<POINT>& left,
                                 const std::vector<POINT>& right) {
        return left.size() == right.size() &&
               std::equal(
                   left.begin(),
                   left.end(),
                   right.begin(),
                   [](POINT first, POINT second) {
                       return first.x == second.x && first.y == second.y;
                   });
    };
    expect(points_equal(normalized_sparse, normalized_dense),
           L"stroke resampling is independent of pointer event density");
    expect(!normalized_sparse.empty() &&
               normalized_sparse.front().x == sparse.front().x &&
               normalized_sparse.front().y == sparse.front().y &&
               normalized_sparse.back().x == sparse.back().x &&
               normalized_sparse.back().y == sparse.back().y,
           L"stroke resampling preserves both endpoints");

    const airshot::AppConfig config;
    for (const Tool tool : {Tool::mosaic, Tool::blur}) {
        Annotation normalized;
        normalized.tool = tool;
        normalized.points = normalized_sparse;
        normalized.width = 4.0F;
        normalized.alpha = 60;

        Annotation from_dense = normalized;
        from_dense.points = normalized_dense;

        const auto sparse_result = airshot::overlay_detail::render_annotations(
            patterned_bitmap(96, 48),
            {normalized},
            config);
        const auto dense_result = airshot::overlay_detail::render_annotations(
            patterned_bitmap(96, 48),
            {from_dense},
            config);
        expect(sparse_result.valid() && dense_result.valid(),
               L"effect renderer accepts normalized brush strokes");
        expect(sparse_result.pixels == dense_result.pixels,
               tool == Tool::mosaic
                   ? L"mosaic output does not depend on pointer event density"
                   : L"blur output does not depend on pointer event density");
    }
}

void test_text_measurement_matches_final_gdi_rendering() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::TextStyle;
    using airshot::overlay_detail::Tool;

    airshot::AppConfig config;
    config.text_font_family = L"Microsoft YaHei";
    config.text_font_bold = true;
    config.text_font_italic = true;

    Annotation latin;
    latin.tool = Tool::text;
    latin.start = {24, 20};
    latin.end = latin.start;
    latin.text = L"Air Screenshot 2026";
    latin.color = RGB(0, 0, 0);
    latin.width = 24.0F;
    expect(
        airshot::overlay_detail::refresh_text_annotation_bounds(
            latin, config) &&
            !latin.measured_text_bounds.empty(),
        L"bold italic Latin text is measured through the final GDI typography path");
    const auto measured_latin =
        airshot::overlay_detail::measure_text_annotation_bounds(latin, config);
    const auto latin_bounds =
        airshot::overlay_detail::annotation_bounds(latin);
    expect(
        measured_latin.left == latin_bounds.left &&
            measured_latin.top == latin_bounds.top &&
            measured_latin.right == latin_bounds.right &&
            measured_latin.bottom == latin_bounds.bottom,
        L"cached text selection bounds exactly match a fresh GDI measurement");

    Annotation cjk_single = latin;
    cjk_single.text = L"截图工具";
    cjk_single.measured_text_bounds = {};
    Annotation cjk_multiline = latin;
    cjk_multiline.text = L"截图工具\nAir 2026";
    cjk_multiline.measured_text_bounds = {};
    expect(
        airshot::overlay_detail::refresh_text_annotation_bounds(
            cjk_single, config) &&
            airshot::overlay_detail::refresh_text_annotation_bounds(
                cjk_multiline, config) &&
            cjk_single.measured_text_bounds.width() > 0 &&
            cjk_multiline.measured_text_bounds.width() > 0 &&
            cjk_multiline.measured_text_bounds.height() >
                cjk_single.measured_text_bounds.height(),
        L"GDI measurement handles CJK and mixed multiline text with bold italic typography");

    Annotation normal = cjk_multiline;
    normal.text_style = TextStyle::normal;
    normal.measured_text_bounds = {};
    Annotation dark = normal;
    dark.text_style = TextStyle::dark;
    Annotation outline = normal;
    outline.text_style = TextStyle::outline;
    const bool measured_styles =
        airshot::overlay_detail::refresh_text_annotation_bounds(
            normal, config) &&
        airshot::overlay_detail::refresh_text_annotation_bounds(
            dark, config) &&
        airshot::overlay_detail::refresh_text_annotation_bounds(
            outline, config);
    expect(
        measured_styles &&
            dark.measured_text_bounds.left ==
                normal.measured_text_bounds.left &&
            dark.measured_text_bounds.top ==
                normal.measured_text_bounds.top &&
            dark.measured_text_bounds.right >=
                normal.measured_text_bounds.right &&
            dark.measured_text_bounds.right <=
                normal.measured_text_bounds.right + 8 &&
            dark.measured_text_bounds.bottom ==
                normal.measured_text_bounds.bottom + 6 &&
            outline.measured_text_bounds.left ==
                normal.measured_text_bounds.left - 2 &&
            outline.measured_text_bounds.top ==
                normal.measured_text_bounds.top - 2 &&
            outline.measured_text_bounds.right ==
                normal.measured_text_bounds.right + 2 &&
            outline.measured_text_bounds.bottom ==
                normal.measured_text_bounds.bottom + 2,
        L"normal, dark, and outline measurements include the exact final-render padding");

    const auto rendered = airshot::overlay_detail::render_annotations(
        solid_bitmap(320, 160, RGB(255, 255, 255)),
        {latin},
        config);
    bool changed_pixel = false;
    bool changed_outside_measurement = false;
    if (rendered.valid()) {
        for (int y = 0; y < rendered.height; ++y) {
            for (int x = 0; x < rendered.width; ++x) {
                const bool changed =
                    channel(rendered, x, y, 0) != 255 ||
                    channel(rendered, x, y, 1) != 255 ||
                    channel(rendered, x, y, 2) != 255;
                if (!changed) {
                    continue;
                }
                changed_pixel = true;
                if (!latin.measured_text_bounds.contains(POINT{x, y})) {
                    changed_outside_measurement = true;
                }
            }
        }
    }
    expect(
        rendered.valid() && changed_pixel && !changed_outside_measurement,
        L"final GDI text pixels stay inside the bounds used for selection and hit testing");
}

void test_single_click_pen_draws_dot() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    Annotation pen;
    pen.tool = Tool::pen;
    pen.points = {{12, 12}};
    pen.color = RGB(255, 0, 0);
    pen.width = 6.0F;

    const auto result = airshot::overlay_detail::render_annotations(
        solid_bitmap(24, 24, RGB(255, 255, 255)),
        {pen},
        airshot::AppConfig{});
    expect(result.valid(), L"single-click pen rendering succeeds");
    expect(channel(result, 12, 12, 2) > 240 &&
               channel(result, 12, 12, 1) < 32 &&
               channel(result, 12, 12, 0) < 32,
           L"single-click pen produces a visible dot");
}

void test_shape_tools_are_outline_only() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    Annotation rectangle;
    rectangle.tool = Tool::rectangle;
    rectangle.start = {4, 4};
    rectangle.end = {28, 28};
    rectangle.color = RGB(255, 0, 0);
    rectangle.width = 4.0F;

    Annotation ellipse = rectangle;
    ellipse.tool = Tool::ellipse;
    ellipse.start = {36, 4};
    ellipse.end = {60, 28};

    const auto result = airshot::overlay_detail::render_annotations(
        solid_bitmap(64, 32, RGB(255, 255, 255)),
        {rectangle, ellipse},
        airshot::AppConfig{});
    expect(result.valid(), L"shape outline rendering succeeds");
    expect(channel(result, 4, 16, 2) > 240 &&
               channel(result, 4, 16, 1) < 32,
           L"rectangle border uses the selected color");
    expect(channel(result, 16, 16, 0) == 255 &&
               channel(result, 16, 16, 1) == 255 &&
               channel(result, 16, 16, 2) == 255,
           L"rectangle interior remains transparent");
    expect(channel(result, 48, 16, 0) == 255 &&
               channel(result, 48, 16, 1) == 255 &&
               channel(result, 48, 16, 2) == 255,
           L"ellipse interior remains transparent");
}

void test_product_shape_styles_and_zero_strength_effects() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::ShapeFillStyle;
    using airshot::overlay_detail::StrokePattern;
    using airshot::overlay_detail::Tool;

    Annotation filled_rectangle;
    filled_rectangle.tool = Tool::rectangle;
    filled_rectangle.start = {4, 4};
    filled_rectangle.end = {36, 36};
    filled_rectangle.color = RGB(255, 0, 0);
    filled_rectangle.width = 2.0F;
    filled_rectangle.fill_style = ShapeFillStyle::translucent;

    Annotation rounded_rectangle = filled_rectangle;
    rounded_rectangle.start = {44, 4};
    rounded_rectangle.end = {76, 36};
    rounded_rectangle.rounded_rectangle = true;

    Annotation filled_ellipse = filled_rectangle;
    filled_ellipse.tool = Tool::ellipse;
    filled_ellipse.start = {84, 4};
    filled_ellipse.end = {116, 36};

    const auto filled = airshot::overlay_detail::render_annotations(
        solid_bitmap(120, 40, RGB(255, 255, 255)),
        {filled_rectangle, rounded_rectangle, filled_ellipse},
        airshot::AppConfig{});
    expect(filled.valid(), L"translucent shape rendering succeeds");
    expect(
        channel(filled, 20, 20, 2) == 255 &&
            channel(filled, 20, 20, 1) == 191 &&
            channel(filled, 20, 20, 0) == 191 &&
            channel(filled, 100, 20, 1) == 191,
        L"rectangle and ellipse use the same exact 25 percent fill");
    expect(
        channel(filled, 45, 5, 0) == 255 &&
            channel(filled, 45, 5, 1) == 255 &&
            channel(filled, 45, 5, 2) == 255,
        L"rounded rectangle leaves its outside corner untouched");

    Annotation dashed_line;
    dashed_line.tool = Tool::line;
    dashed_line.start = {4, 8};
    dashed_line.end = {91, 8};
    dashed_line.color = RGB(0, 0, 0);
    dashed_line.width = 2.0F;
    dashed_line.stroke_pattern = StrokePattern::dashed;
    const auto dashed = airshot::overlay_detail::render_annotations(
        solid_bitmap(96, 16, RGB(255, 255, 255)),
        {dashed_line},
        airshot::AppConfig{});
    int dark_pixels = 0;
    int light_pixels = 0;
    if (dashed.valid()) {
        for (int x = 5; x < 91; ++x) {
            if (channel(dashed, x, 8, 0) < 64) {
                ++dark_pixels;
            } else if (channel(dashed, x, 8, 0) > 240) {
                ++light_pixels;
            }
        }
    }
    expect(
        dashed.valid() && dark_pixels > 0 && light_pixels > 0,
        L"dashed strokes contain visible marks and gaps");

    const auto base = patterned_bitmap(64, 32);
    for (const Tool tool : {Tool::mosaic, Tool::blur}) {
        Annotation zero_strength;
        zero_strength.tool = tool;
        zero_strength.points = {{8, 16}, {56, 16}};
        zero_strength.width = 8.0F;
        zero_strength.alpha = 0;
        const auto result = airshot::overlay_detail::render_annotations(
            base,
            {zero_strength},
            airshot::AppConfig{});
        expect(
            result.valid() && result.pixels == base.pixels,
            tool == Tool::mosaic
                ? L"zero-strength mosaic is visibly a no-op"
                : L"zero-strength blur is visibly a no-op");
    }
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

void test_text_prompt_focus_policy() {
    using airshot::overlay_detail::TextPromptAction;
    using airshot::overlay_detail::TextPromptEvent;
    using airshot::overlay_detail::text_prompt_action;

    expect(
        text_prompt_action(TextPromptEvent::deactivated) ==
            TextPromptAction::keep_editing,
        L"text prompt retains its draft across temporary deactivation");
    expect(
        text_prompt_action(TextPromptEvent::enter) ==
                TextPromptAction::accept &&
            text_prompt_action(TextPromptEvent::enter, true, false) ==
                TextPromptAction::pass_to_editor &&
            text_prompt_action(TextPromptEvent::enter, false, true) ==
                TextPromptAction::pass_to_editor,
        L"text prompt accepts plain Enter but leaves newline and IME Enter to the editor");
    expect(
        text_prompt_action(TextPromptEvent::escape, false, true) ==
                TextPromptAction::pass_to_editor &&
            text_prompt_action(TextPromptEvent::escape) ==
                TextPromptAction::cancel,
        L"IME Escape cancels composition before a later Escape cancels the prompt");
    expect(
        text_prompt_action(TextPromptEvent::close_request) ==
            TextPromptAction::cancel,
        L"closing the text prompt explicitly cancels its draft");
}

void test_text_prompt_deactivation_preserves_draft() {
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
    wchar_t retained[32]{};
    if (edit && IsWindow(edit)) {
        GetWindowTextW(edit, retained, static_cast<int>(std::size(retained)));
    }
    expect(!completed && IsWindow(prompt) && std::wstring_view(retained) == L"draft",
           L"deactivating a text prompt keeps the window and draft alive");

    SendMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
    expect(completed && result && *result == L"draft" && !IsWindow(prompt),
           L"a retained draft can still be committed after reactivation");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }
}

void test_text_prompt_escape_explicitly_cancels() {
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
        },
        L"do not commit");
    expect(prompt != nullptr, L"text prompt is created for explicit cancel testing");
    if (!prompt) {
        return;
    }

    const HWND edit = GetDlgItem(prompt, 100);
    expect(edit != nullptr, L"text prompt edit exists for explicit cancel testing");
    if (edit) {
        SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
    }
    MSG message{};
    while (!completed && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    expect(completed && !result.has_value() && !IsWindow(prompt),
           L"Escape explicitly cancels without committing the retained draft");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }
}

void test_text_prompt_forced_destroy_completes_once() {
    int completion_count = 0;
    std::optional<std::wstring> result;
    const HWND prompt = airshot::overlay_detail::show_text_prompt(
        nullptr,
        POINT{20, 20},
        RGB(255, 255, 255),
        16.0F,
        false,
        [&](std::optional<std::wstring> text) {
            result = std::move(text);
            ++completion_count;
        },
        L"retained draft");
    expect(prompt != nullptr, L"text prompt is created for forced-destroy testing");
    if (!prompt) {
        return;
    }

    SendMessageW(prompt, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
    DestroyWindow(prompt);
    expect(completion_count == 1 && !result.has_value() && !IsWindow(prompt),
           L"owner shutdown cancels a retained draft and completes exactly once");

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    expect(completion_count == 1,
           L"destroyed text prompt does not leave a late completion callback");
}

void test_text_prompt_supports_editing_existing_text() {
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
        },
        L"first line\nsecond line");
    expect(prompt != nullptr, L"text prompt is created for editing testing");
    if (!prompt) {
        return;
    }

    const HWND edit = GetDlgItem(prompt, 100);
    wchar_t initial[64]{};
    if (edit) {
        GetWindowTextW(edit, initial, static_cast<int>(std::size(initial)));
    }
    std::wstring normalized_initial = initial;
    std::erase(normalized_initial, L'\r');
    expect(
        edit != nullptr && normalized_initial == L"first line\nsecond line",
        L"text prompt restores existing multiline text");
    if (edit) {
        SetWindowTextW(edit, L"updated\r\ncontent");
    }
    SendMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
    expect(
        completed && result && *result == L"updated\r\ncontent" &&
            !IsWindow(prompt),
        L"text prompt commits edited multiline text");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }
}

void test_text_prompt_uses_annotation_typography_and_style() {
    bool completed = false;
    const HWND prompt = airshot::overlay_detail::show_text_prompt(
        nullptr,
        POINT{20, 20},
        RGB(255, 0, 0),
        22.0F,
        true,
        [&](std::optional<std::wstring>) { completed = true; },
        L"styled",
        L"Consolas",
        true,
        true,
        airshot::overlay_detail::TextStyle::dark);
    expect(prompt != nullptr, L"styled text prompt is created");
    if (!prompt) {
        return;
    }

    const HWND edit = GetDlgItem(prompt, 100);
    const HFONT font = edit
                           ? reinterpret_cast<HFONT>(
                                 SendMessageW(edit, WM_GETFONT, 0, 0))
                           : nullptr;
    LOGFONTW description{};
    const int font_bytes = font
                               ? GetObjectW(
                                     font,
                                     static_cast<int>(sizeof(description)),
                                     &description)
                               : 0;
    expect(
        edit != nullptr && font_bytes == sizeof(description) &&
            _wcsicmp(description.lfFaceName, L"Consolas") == 0 &&
            description.lfWeight == FW_BOLD && description.lfItalic != 0,
        L"text prompt uses configured font family, bold, italic, and size");

    COLORREF prompt_text_color = CLR_INVALID;
    COLORREF prompt_background = CLR_INVALID;
    if (edit) {
        HDC dc = GetDC(edit);
        if (dc) {
            SendMessageW(
                prompt,
                WM_CTLCOLOREDIT,
                reinterpret_cast<WPARAM>(dc),
                reinterpret_cast<LPARAM>(edit));
            prompt_text_color = GetTextColor(dc);
            prompt_background = GetBkColor(dc);
            ReleaseDC(edit, dc);
        }
    }
    expect(
        prompt_text_color == RGB(255, 255, 255) &&
            prompt_background == RGB(31, 35, 41),
        L"dark text prompt matches the final dark annotation style");

    SendMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
    expect(completed && !IsWindow(prompt), L"styled text prompt closes cleanly");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }
}

void test_selection_size_prompt_validates_without_side_effects() {
    bool completed = false;
    std::optional<airshot::overlay_detail::SelectionSizeInput> result;
    const HWND prompt =
        airshot::overlay_detail::show_selection_size_prompt(
            nullptr,
            POINT{20, 20},
            {-320, -20, 0, 160},
            {-1920, -200, 1920, 1080},
            airshot::SelectionSizeAnchor::center,
            false,
            0,
            true,
            [&](std::optional<
                    airshot::overlay_detail::SelectionSizeInput> input) {
                result = std::move(input);
                completed = true;
            });
    expect(
        prompt != nullptr,
        L"selection size prompt is created for F2 dimension entry");
    if (!prompt) {
        return;
    }

    const HWND x = GetDlgItem(prompt, 205);
    const HWND y = GetDlgItem(prompt, 206);
    const HWND width = GetDlgItem(prompt, 201);
    const HWND height = GetDlgItem(prompt, 202);
    const HWND center = GetDlgItem(prompt, 203);
    const HWND error = GetDlgItem(prompt, 204);
    const HWND aspect = GetDlgItem(prompt, 207);
    const HWND rounded = GetDlgItem(prompt, 208);
    const HWND corner_radius = GetDlgItem(prompt, 209);
    expect(
        x && y && width && height && center && error && aspect &&
            rounded && corner_radius &&
            SendMessageW(center, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            SendMessageW(aspect, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
            SendMessageW(rounded, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
            !IsWindowEnabled(corner_radius),
        L"selection geometry prompt exposes geometry, aspect, and output-corner controls");

    if (width && x) {
        SetWindowTextW(width, L"640");
        std::array<wchar_t, 16> centered_x{};
        GetWindowTextW(
            x,
            centered_x.data(),
            static_cast<int>(centered_x.size()));
        expect(
            std::wstring_view(centered_x.data()) == L"-480",
            L"center anchoring immediately exposes the resolved X coordinate");
        SetWindowTextW(width, L"320");
    }

    if (x && y) {
        SetFocus(x);
        SendMessageW(x, WM_KEYDOWN, VK_TAB, 0);
        expect(
            GetFocus() == y,
            L"Tab reaches every geometry field without a mouse");
        SetWindowTextW(x, L"left");
    }
    SendMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    expect(
        IsWindow(prompt) && !completed &&
            error && GetWindowTextLengthW(error) > 0,
        L"invalid signed coordinates keep the prompt open without side effects");

    if (x) {
        SetWindowTextW(x, L"-800");
    }
    if (y) {
        SetWindowTextW(y, L"-100");
    }
    if (aspect) {
        SendMessageW(aspect, BM_CLICK, 0, 0);
    }
    if (width) {
        SetWindowTextW(width, L"640");
    }
    std::array<wchar_t, 16> linked_height{};
    if (height) {
        GetWindowTextW(
            height,
            linked_height.data(),
            static_cast<int>(linked_height.size()));
    }
    expect(
        std::wstring_view(linked_height.data()) == L"360",
        L"locked aspect ratio updates height from width immediately");
    if (height) {
        SetWindowTextW(height, L"540");
    }
    std::array<wchar_t, 16> linked_width{};
    if (width) {
        GetWindowTextW(
            width,
            linked_width.data(),
            static_cast<int>(linked_width.size()));
    }
    expect(
        std::wstring_view(linked_width.data()) == L"960",
        L"locked aspect ratio updates width from height immediately");
    if (center) {
        SendMessageW(center, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    if (rounded && corner_radius) {
        SendMessageW(rounded, BM_CLICK, 0, 0);
        SetWindowTextW(corner_radius, L"24");
    }
    SendMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    expect(
        completed && result && result->x == -800 && result->y == -100 &&
            result->width == 960 && result->height == 540 &&
            result->anchor ==
                airshot::SelectionSizeAnchor::top_left &&
            result->aspect_ratio_locked && result->corner_radius == 24 &&
            !IsWindow(prompt),
        L"valid negative coordinates and linked dimensions commit once");
    if (IsWindow(prompt)) {
        DestroyWindow(prompt);
    }

    bool cancel_completed = false;
    std::optional<airshot::overlay_detail::SelectionSizeInput> cancel_result;
    const HWND cancel_prompt =
        airshot::overlay_detail::show_selection_size_prompt(
            nullptr,
            POINT{20, 20},
            {0, 0, 100, 100},
            {-1920, -200, 1920, 1080},
            airshot::SelectionSizeAnchor::top_left,
            true,
            0,
            false,
            [&](std::optional<
                    airshot::overlay_detail::SelectionSizeInput> input) {
                cancel_result = std::move(input);
                cancel_completed = true;
            });
    expect(
        cancel_prompt != nullptr,
        L"selection size prompt is created for cancellation testing");
    if (cancel_prompt) {
        expect(
            SendMessageW(GetDlgItem(cancel_prompt, 203), BM_GETCHECK, 0, 0) ==
                BST_UNCHECKED,
            L"selection size prompt remembers the previous anchor mode");
        SendMessageW(
            cancel_prompt,
            WM_COMMAND,
            MAKEWPARAM(IDCANCEL, BN_CLICKED),
            0);
        expect(
            cancel_completed && !cancel_result &&
                !IsWindow(cancel_prompt),
            L"cancelling size entry closes without changing the selection");
        if (IsWindow(cancel_prompt)) {
            DestroyWindow(cancel_prompt);
        }
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

void test_output_decorations_disabled_is_identity() {
    airshot::Bitmap source = patterned_bitmap(4, 3);
    source.row(0)[3] = 0;
    source.row(1)[7] = 96;
    const airshot::Bitmap original = source;

    const auto result = airshot::decorate_output_bitmap(source);
    expect(
        result.status == airshot::OutputDecorationStatus::no_change &&
            result.bitmap.valid() &&
            result.bitmap.width == source.width &&
            result.bitmap.height == source.height &&
            result.bitmap.pixels == source.pixels,
        L"disabled output decorations return a byte-exact source copy");
    expect(
        source.width == original.width &&
            source.height == original.height &&
            source.pixels == original.pixels,
        L"output decoration never mutates its source bitmap");

    airshot::Bitmap transparent(2, 2);
    std::fill(
        transparent.pixels.begin(),
        transparent.pixels.end(),
        std::uint8_t{0});
    airshot::OutputDecorationOptions transparent_options;
    transparent_options.border_width = 3;
    transparent_options.shadow_blur_radius = 2;
    transparent_options.shadow_offset_x = 2;
    transparent_options.shadow_offset_y = 1;
    transparent_options.shadow_opacity = 160;
    const auto transparent_result = airshot::decorate_output_bitmap(
        transparent,
        transparent_options);
    expect(
        transparent_result.status ==
                airshot::OutputDecorationStatus::no_change &&
            transparent_result.bitmap.width == transparent.width &&
            transparent_result.bitmap.height == transparent.height &&
            transparent_result.bitmap.pixels == transparent.pixels,
        L"a fully transparent source produces neither a border nor a shadow");
}

void test_output_border_follows_source_alpha() {
    airshot::Bitmap source(5, 5);
    std::fill(source.pixels.begin(), source.pixels.end(), std::uint8_t{0});
    auto center = source.row(2);
    const std::size_t center_offset = 2U * airshot::Bitmap::bytes_per_pixel;
    center[center_offset] = 100;
    center[center_offset + 1] = 90;
    center[center_offset + 2] = 80;
    center[center_offset + 3] = 255;
    const airshot::Bitmap original = source;

    airshot::OutputDecorationOptions options;
    options.border_width = 1;
    options.border_color = RGB(10, 20, 30);
    const auto result = airshot::decorate_output_bitmap(source, options);

    expect(
        result.status == airshot::OutputDecorationStatus::applied &&
            result.bitmap.valid() && result.bitmap.width == 7 &&
            result.bitmap.height == 7,
        L"an alpha-derived border expands the output canvas exactly once");
    if (result.bitmap.valid()) {
        expect(
            channel(result.bitmap, 3, 3, 0) == 100 &&
                channel(result.bitmap, 3, 3, 1) == 90 &&
                channel(result.bitmap, 3, 3, 2) == 80 &&
                channel(result.bitmap, 3, 3, 3) == 255,
            L"the source pixel remains unchanged above its border");
        expect(
            channel(result.bitmap, 2, 3, 0) == 30 &&
                channel(result.bitmap, 2, 3, 1) == 20 &&
                channel(result.bitmap, 2, 3, 2) == 10 &&
                channel(result.bitmap, 2, 3, 3) == 255,
            L"the border uses the configured COLORREF around visible alpha");
        expect(
            channel(result.bitmap, 1, 1, 3) == 0 &&
                channel(result.bitmap, 0, 0, 3) == 0,
            L"transparent source corners are not treated as an opaque rectangle");
    }
    expect(
        source.pixels == original.pixels,
        L"border generation leaves the alpha source byte-for-byte unchanged");
}

void test_output_shadow_range_and_alpha_composition() {
    airshot::Bitmap ranged_source(1, 1);
    ranged_source.pixels = {255, 255, 255, 255};
    airshot::OutputDecorationOptions ranged_options;
    ranged_options.shadow_blur_radius = 1;
    ranged_options.shadow_offset_x = 3;
    ranged_options.shadow_color = RGB(0, 0, 0);
    ranged_options.shadow_opacity = 180;
    const auto ranged = airshot::decorate_output_bitmap(
        ranged_source,
        ranged_options);
    expect(
        ranged.status == airshot::OutputDecorationStatus::applied &&
            ranged.bitmap.valid() && ranged.bitmap.width == 5 &&
            ranged.bitmap.height == 3,
        L"shadow offset and blur support determine the expanded canvas bounds");
    if (ranged.bitmap.valid()) {
        expect(
            channel(ranged.bitmap, 0, 1, 3) == 255 &&
                channel(ranged.bitmap, 1, 1, 3) == 0,
            L"a separated shadow does not bridge pixels outside its blur range");
        expect(
            channel(ranged.bitmap, 2, 1, 3) > 0 &&
                channel(ranged.bitmap, 3, 1, 3) > 0 &&
                channel(ranged.bitmap, 4, 1, 3) > 0 &&
                channel(ranged.bitmap, 3, 1, 0) == 0 &&
                channel(ranged.bitmap, 3, 1, 1) == 0 &&
                channel(ranged.bitmap, 3, 1, 2) == 0,
            L"soft shadow alpha stays inside the planned support and uses its color");
    }

    airshot::Bitmap translucent_source(1, 1);
    translucent_source.pixels = {0, 0, 255, 128};
    airshot::OutputDecorationOptions composite_options;
    composite_options.shadow_color = RGB(0, 0, 0);
    composite_options.shadow_opacity = 128;
    const auto composite = airshot::decorate_output_bitmap(
        translucent_source,
        composite_options);
    expect(
        composite.status == airshot::OutputDecorationStatus::applied &&
            composite.bitmap.valid() &&
            composite.bitmap.pixels ==
                std::vector<std::uint8_t>({0, 0, 204, 160}),
        L"straight-alpha source-over composition preserves translucent color correctly");
}

void test_output_decoration_limits_fail_safely() {
    airshot::Bitmap invalid(2, 2);
    invalid.pixels.pop_back();
    expect(
        airshot::decorate_output_bitmap(invalid).status ==
            airshot::OutputDecorationStatus::invalid_input,
        L"output decorations reject malformed and empty bitmaps");

    airshot::Bitmap source = patterned_bitmap(4, 4);
    const airshot::Bitmap original = source;
    airshot::OutputDecorationOptions invalid_options;
    invalid_options.border_width =
        airshot::OutputDecorationOptions::maximum_border_width + 1;
    expect(
        airshot::decorate_output_bitmap(source, invalid_options).status ==
            airshot::OutputDecorationStatus::invalid_options,
        L"output decorations reject an excessive border width");
    invalid_options = {};
    invalid_options.shadow_blur_radius =
        airshot::OutputDecorationOptions::maximum_shadow_blur_radius + 1;
    expect(
        airshot::decorate_output_bitmap(source, invalid_options).status ==
            airshot::OutputDecorationStatus::invalid_options,
        L"output decorations reject an excessive shadow radius");
    invalid_options = {};
    invalid_options.shadow_offset_x = std::numeric_limits<int>::min();
    invalid_options.shadow_opacity = 128;
    expect(
        airshot::decorate_output_bitmap(source, invalid_options).status ==
            airshot::OutputDecorationStatus::invalid_options,
        L"output decorations reject extreme offsets before doing arithmetic");
    invalid_options = {};
    invalid_options.shadow_opacity = 256;
    expect(
        airshot::decorate_output_bitmap(source, invalid_options).status ==
            airshot::OutputDecorationStatus::invalid_options,
        L"output decorations reject shadow opacity outside byte range");

    airshot::OutputDecorationOptions bounded_options;
    bounded_options.border_width = 1;
    airshot::OutputDecorationLimits limits;
    limits.max_output_pixels = 20;
    const auto limited = airshot::decorate_output_bitmap(
        source,
        bounded_options,
        limits);
    expect(
        limited.status == airshot::OutputDecorationStatus::resource_limit &&
            limited.bitmap.empty() && source.pixels == original.pixels,
        L"output canvas budget fails without allocating or changing the source");
    limits.max_output_pixels =
        airshot::OutputDecorationLimits::hard_max_output_pixels;
    limits.max_working_bytes = 16;
    const auto working_limited = airshot::decorate_output_bitmap(
        source,
        bounded_options,
        limits);
    expect(
        working_limited.status ==
                airshot::OutputDecorationStatus::resource_limit &&
            working_limited.bitmap.empty() && source.pixels == original.pixels,
        L"output working-memory budget covers masks as well as final pixels");
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
    test_rounded_corner_alpha_mask();
    test_alpha_compositing_for_legacy_outputs();
    test_blit_and_holes();
    test_linear_blur();
    test_privacy_stroke_uses_one_union_mask();
    test_privacy_mosaic_is_globally_anchored();
    test_privacy_solid_fill_and_boundary_clipping();
    test_privacy_noop_and_resource_contract();
    test_privacy_large_stroke_is_bounded();
    test_ordered_annotation_rendering();
    test_highlight_alpha_is_sampling_independent();
    test_effect_strokes_are_sampling_independent();
    test_text_measurement_matches_final_gdi_rendering();
    test_single_click_pen_draws_dot();
    test_shape_tools_are_outline_only();
    test_product_shape_styles_and_zero_strength_effects();
    test_overlay_close_lifecycle();
    test_text_prompt_focus_policy();
    test_text_prompt_deactivation_preserves_draft();
    test_text_prompt_escape_explicitly_cancels();
    test_text_prompt_forced_destroy_completes_once();
    test_text_prompt_supports_editing_existing_text();
    test_text_prompt_uses_annotation_typography_and_style();
    test_selection_size_prompt_validates_without_side_effects();
    test_watermark_alpha();
    test_output_decorations_disabled_is_identity();
    test_output_border_follows_source_alpha();
    test_output_shadow_range_and_alpha_composition();
    test_output_decoration_limits_fail_safely();
    test_atomic_png_output();
    if (failures == 0) {
        std::wcout << L"All image pipeline tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
