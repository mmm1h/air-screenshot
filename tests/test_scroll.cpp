#include "../src/core/overlay_helpers.h"
#include "airshot/overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

using airshot::Bitmap;
using airshot::overlay_detail::ScrollResult;
using airshot::overlay_detail::ScrollStitcher;
using airshot::overlay_detail::StitchStatus;

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << message << L'\n';
    }
}

Bitmap make_document(int width, int height) {
    Bitmap document(width, height);
    for (int y = 0; y < height; ++y) {
        auto row = document.row(y);
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(x) * 4U;
            row[index] = static_cast<std::uint8_t>((y * 37 + x * 11 + (y / 7) * 19) & 0xFF);
            row[index + 1] = static_cast<std::uint8_t>((y * 17 + x * 29 + (x / 5) * 31) & 0xFF);
            row[index + 2] = static_cast<std::uint8_t>((y * 53 + x * 7 + (y / 3) * 13) & 0xFF);
            row[index + 3] = 255;
        }
    }
    return document;
}

Bitmap frame_from(const Bitmap& document, int top, int height) {
    return airshot::crop(document, {0, top, document.width, top + height});
}

Bitmap make_periodic_document(int width, int height, int period) {
    const Bitmap tile = make_document(width, period);
    Bitmap document(width, height);
    for (int y = 0; y < height; ++y) {
        std::copy(tile.row(y % period).begin(),
                  tile.row(y % period).end(),
                  document.row(y).begin());
    }
    return document;
}

void copy_rows(const Bitmap& source,
               int source_y,
               Bitmap& destination,
               int destination_y,
               int row_count) {
    for (int row = 0; row < row_count; ++row) {
        std::copy(source.row(source_y + row).begin(),
                  source.row(source_y + row).end(),
                  destination.row(destination_y + row).begin());
    }
}

void test_small_frames() {
    for (int height = 1; height <= 24; ++height) {
        Bitmap frame = make_document(1 + height % 7, height);
        const auto unchanged = airshot::overlay_detail::detect_scroll(frame, frame, 0);
        expect(unchanged.status() == ScrollResult::Status::unchanged,
               L"small unchanged frame is recognized");

        const int template_y = airshot::overlay_detail::find_best_template_y(frame, 0);
        expect(template_y >= 0 && template_y < height,
               L"small frame template remains in bounds");
    }

    Bitmap malformed;
    malformed.width = std::numeric_limits<int>::max();
    malformed.height = std::numeric_limits<int>::max();
    malformed.pixels.resize(4);
    expect(airshot::overlay_detail::find_best_template_y(malformed, 0) == 0,
           L"malformed frame does not overflow template calculation");
    expect(airshot::overlay_detail::detect_scroll(malformed, malformed, 0).status() ==
               ScrollResult::Status::mismatch,
           L"malformed frame is rejected");

    Bitmap trailing_bytes(2, 2);
    trailing_bytes.pixels.push_back(0);
    expect(airshot::overlay_detail::detect_scroll(
               trailing_bytes, trailing_bytes, 0).status() ==
               ScrollResult::Status::mismatch,
           L"frame with trailing bytes is rejected");
    expect(!ScrollStitcher(std::move(trailing_bytes)).valid(),
           L"stitcher rejects a frame with trailing bytes");
}

void test_scroll_detection() {
    const Bitmap document = make_document(32, 220);
    const Bitmap first = frame_from(document, 60, 72);

    const Bitmap down = frame_from(document, 71, 72);
    const auto down_result = airshot::overlay_detail::detect_scroll(first, down, 1);
    expect(down_result.status() == ScrollResult::Status::moved_down &&
               down_result.direction == 1 && down_result.offset == 11,
           L"downward scroll displacement is detected");

    const Bitmap up = frame_from(document, 51, 72);
    const auto up_result = airshot::overlay_detail::detect_scroll(first, up, -1);
    expect(up_result.status() == ScrollResult::Status::moved_up &&
               up_result.direction == -1 && up_result.offset == 9,
           L"upward scroll displacement is detected");

    Bitmap unrelated(32, 72);
    std::fill(
        unrelated.pixels.begin(),
        unrelated.pixels.end(),
        static_cast<std::uint8_t>(255));
    expect(airshot::overlay_detail::detect_scroll(first, unrelated, 0).status() ==
               ScrollResult::Status::mismatch,
           L"unrelated frame is a mismatch");

    Bitmap wrong_size(31, 72);
    expect(airshot::overlay_detail::detect_scroll(first, wrong_size, 0).status() ==
               ScrollResult::Status::mismatch,
           L"different frame dimensions are a mismatch");

    const Bitmap tall_document = make_document(64, 1800);
    const Bitmap tall_first = frame_from(tall_document, 500, 720);
    const Bitmap tall_down = frame_from(tall_document, 820, 720);
    airshot::overlay_detail::ScrollSearchStats fallback_stats;
    const auto tall_result =
        airshot::overlay_detail::detect_scroll(tall_first, tall_down, 1, &fallback_stats);
    expect(tall_result.status() == ScrollResult::Status::moved_down &&
               tall_result.offset == 320,
           L"hierarchical fallback preserves large-scroll matching");
    expect(fallback_stats.candidates_evaluated <=
               airshot::overlay_detail::kMaxScrollSearchCandidates,
           L"large-scroll matching obeys its candidate budget");
}

void test_global_search_ambiguity() {
    constexpr int frame_height = 1400;
    const Bitmap periodic_document =
        make_periodic_document(48, frame_height + 160, 320);
    const Bitmap periodic_first = frame_from(periodic_document, 0, frame_height);
    const Bitmap periodic_second = frame_from(periodic_document, 160, frame_height);
    airshot::overlay_detail::ScrollSearchStats periodic_stats;
    const auto periodic_result = airshot::overlay_detail::detect_scroll(
        periodic_first, periodic_second, 1, &periodic_stats);
    expect(periodic_result.status() == ScrollResult::Status::mismatch,
           L"periodic templates remain ambiguous across global candidate basins");
    expect(periodic_stats.candidates_evaluated >
               airshot::overlay_detail::kMaxScrollSearchCandidates / 2,
           L"periodic ambiguity exercises hierarchical global search");
    expect(periodic_stats.candidates_evaluated <=
               airshot::overlay_detail::kMaxScrollSearchCandidates,
           L"periodic ambiguity search obeys its candidate budget");

    Bitmap source = make_document(48, frame_height);
    Bitmap equivalent_targets(48, frame_height);
    std::fill(equivalent_targets.pixels.begin(),
              equivalent_targets.pixels.end(),
              static_cast<std::uint8_t>(255));
    const int template_y =
        airshot::overlay_detail::find_best_template_y(source, 1);
    constexpr int template_height = 24;
    copy_rows(source, template_y, equivalent_targets, 40, template_height);
    copy_rows(source, template_y, equivalent_targets, 400, template_height);

    airshot::overlay_detail::ScrollSearchStats equivalent_stats;
    const auto equivalent_result = airshot::overlay_detail::detect_scroll(
        source, equivalent_targets, 1, &equivalent_stats);
    expect(equivalent_result.status() == ScrollResult::Status::mismatch,
           L"distant equivalent templates are retained through refinement");
    expect(equivalent_stats.candidates_evaluated >
               airshot::overlay_detail::kMaxScrollSearchCandidates / 2,
           L"distant equivalence exercises hierarchical global search");
    expect(equivalent_stats.candidates_evaluated <=
               airshot::overlay_detail::kMaxScrollSearchCandidates,
           L"distant-equivalence search obeys its candidate budget");
}

void test_single_template_does_not_validate_an_unrelated_frame() {
    constexpr int frame_height = 720;
    Bitmap source = make_document(64, frame_height);

    const int template_y =
        airshot::overlay_detail::find_best_template_y(source, 1);
    constexpr int template_height = 24;
    const std::array impostor_positions{
        template_y,
        template_y - 1,
        template_y - 80,
    };
    expect(impostor_positions.back() >= 0,
           L"single-template regression fixture remains in bounds");
    if (impostor_positions.back() >= 0) {
        for (const int impostor_y : impostor_positions) {
            Bitmap impostor(64, frame_height);
            std::fill(impostor.pixels.begin(),
                      impostor.pixels.end(),
                      static_cast<std::uint8_t>(255));
            copy_rows(source,
                      template_y,
                      impostor,
                      impostor_y,
                      template_height);
            const auto result =
                airshot::overlay_detail::detect_scroll(source, impostor, 1);
            expect(result.status() == ScrollResult::Status::mismatch,
                   L"a single matching template cannot validate an unrelated overlap");
        }
    }
}

void test_overlap_validation_tolerates_fixed_chrome() {
    const Bitmap document = make_document(64, 260);
    Bitmap first = frame_from(document, 60, 120);
    Bitmap second = frame_from(document, 80, 120);

    for (int y = 0; y < first.height; ++y) {
        for (int x = 0; x < 8; ++x) {
            const auto index = static_cast<std::size_t>(x) * 4U;
            const std::uint8_t value =
                static_cast<std::uint8_t>((y * 41 + x * 17) & 0xFF);
            first.row(y)[index] = value;
            second.row(y)[index] = value;
        }
    }
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < first.width; ++x) {
            const auto index = static_cast<std::size_t>(x) * 4U;
            const std::uint8_t value =
                static_cast<std::uint8_t>((x * 23 + y * 7) & 0xFF);
            first.row(y)[index + 1] = value;
            second.row(y)[index + 1] = value;
        }
    }

    const auto result =
        airshot::overlay_detail::detect_scroll(first, second, 1);
    expect(result.status() == ScrollResult::Status::moved_down &&
               result.offset == 20,
           L"overlap validation tolerates a small fixed header and sidebar");
}

void test_bounded_large_frame_search() {
    Bitmap first = make_document(3840, 2160);
    Bitmap unrelated = first;
    for (std::size_t index = 0; index < unrelated.pixels.size();
         index += Bitmap::bytes_per_pixel) {
        unrelated.pixels[index] ^= 0x5A;
        unrelated.pixels[index + 1] ^= 0xA5;
        unrelated.pixels[index + 2] ^= 0x3C;
    }

    airshot::overlay_detail::ScrollSearchStats stats;
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        airshot::overlay_detail::detect_scroll(first, unrelated, 0, &stats);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(result.status() == ScrollResult::Status::mismatch,
           L"unrelated 4K frame remains a mismatch");
    expect(stats.candidates_evaluated > 0 &&
               stats.candidates_evaluated <=
                   airshot::overlay_detail::kMaxScrollSearchCandidates,
           L"4K fallback search obeys its candidate budget");
    expect(elapsed < std::chrono::seconds(10),
           L"4K fallback search remains bounded in wall time");
}

void test_chunked_stitching() {
    const Bitmap document = make_document(24, 180);

    const Bitmap initial_down = frame_from(document, 50, 40);
    ScrollStitcher downward(initial_down);
    expect(downward.valid() && downward.height() == 40 &&
               downward.frame_count() == 1 && downward.direction() == 0,
           L"downward stitcher initializes");
    expect(downward.append(frame_from(document, 57, 40), 7) == StitchStatus::success,
           L"downward strip appends");
    expect(downward.direction() == 1 && downward.height() == 47 &&
               downward.frame_count() == 2,
           L"downward stitcher tracks direction and height");
    expect(downward.prepend(frame_from(document, 43, 40), 7) ==
               StitchStatus::direction_mismatch,
           L"downward stitcher rejects direction reversal");

    Bitmap materialized_down;
    expect(downward.materialize(materialized_down) == StitchStatus::success,
           L"downward stitcher materializes");
    expect(materialized_down.pixels == frame_from(document, 50, 47).pixels,
           L"downward stitcher preserves document rows");

    ScrollStitcher upward(frame_from(document, 50, 40));
    expect(upward.prepend(frame_from(document, 43, 40), 7) == StitchStatus::success,
           L"first upward strip prepends");
    expect(upward.prepend(frame_from(document, 36, 40), 7) == StitchStatus::success,
           L"second upward strip prepends without materializing prior strips");
    expect(upward.frame_count() == 3,
           L"upward stitcher exposes the authoritative accumulated frame count");

    Bitmap materialized_up;
    expect(upward.materialize(materialized_up) == StitchStatus::success,
           L"upward stitcher materializes");
    expect(materialized_up.pixels == frame_from(document, 36, 54).pixels,
           L"upward strips materialize in document order");

    const int old_height = upward.height();
    expect(upward.prepend(frame_from(document, 29, 40), 41) == StitchStatus::invalid_input &&
               upward.height() == old_height,
           L"invalid strip leaves stitcher unchanged");
}

void test_stitch_limits_and_compatibility() {
    expect(airshot::overlay_detail::kMaxScrollBitmapBytes *
                   airshot::overlay_detail::kMaxScrollResidentBufferCopies <=
               airshot::overlay_detail::kMaxScrollWorkingSetBytes,
           L"scroll bitmap cap accounts for peak resident copies");
    constexpr int budget_test_width = 1024;
    constexpr std::size_t budget_test_stride =
        static_cast<std::size_t>(budget_test_width) * Bitmap::bytes_per_pixel;
    const int budget_rows = static_cast<int>(
        airshot::overlay_detail::kMaxScrollBitmapBytes / budget_test_stride);
    expect(airshot::overlay_detail::scroll_bitmap_fits_budget(
               budget_test_width, budget_rows),
           L"scroll bitmap accepts dimensions at the byte budget");
    expect(!airshot::overlay_detail::scroll_bitmap_fits_budget(
               budget_test_width, budget_rows + 1),
           L"scroll bitmap rejects dimensions above the byte budget");

    Bitmap maximum_height(1, airshot::overlay_detail::kMaxScrollBitmapHeight);
    ScrollStitcher limited(std::move(maximum_height));
    Bitmap one_row(1, 1);
    expect(limited.append(one_row, 1) == StitchStatus::limit_reached,
           L"height budget is enforced");

    Bitmap document = make_document(8, 40);
    Bitmap stitched = frame_from(document, 10, 12);
    airshot::overlay_detail::append_to_stitched(stitched, frame_from(document, 14, 12), 4);
    expect(stitched.pixels == frame_from(document, 10, 16).pixels,
           L"compatibility append preserves pixels");

    stitched = frame_from(document, 10, 12);
    airshot::overlay_detail::prepend_to_stitched(stitched, frame_from(document, 6, 12), 4);
    expect(stitched.pixels == frame_from(document, 6, 16).pixels,
           L"compatibility prepend preserves pixels");

    const Bitmap before = stitched;
    Bitmap wrong_width(7, 12);
    airshot::overlay_detail::prepend_to_stitched(stitched, wrong_width, 4);
    airshot::overlay_detail::append_to_stitched(stitched, wrong_width, 13);
    expect(stitched.width == before.width && stitched.height == before.height &&
               stitched.pixels == before.pixels,
           L"invalid compatibility operations leave bitmap unchanged");

    stitched = frame_from(document, 10, 12);
    const Bitmap before_alias = stitched;
    airshot::overlay_detail::append_to_stitched(stitched, stitched, 4);
    airshot::overlay_detail::prepend_to_stitched(stitched, stitched, 4);
    expect(stitched.width == before_alias.width &&
               stitched.height == before_alias.height &&
               stitched.pixels == before_alias.pixels,
           L"compatibility stitching rejects aliased input");
}

void test_resume_baseline_and_keyboard_commands() {
    Bitmap baseline(4, 4);
    std::fill(
        baseline.pixels.begin(),
        baseline.pixels.end(),
        static_cast<std::uint8_t>(17));
    Bitmap refreshed(4, 4);
    std::fill(
        refreshed.pixels.begin(),
        refreshed.pixels.end(),
        static_cast<std::uint8_t>(93));
    expect(
        airshot::overlay_detail::replace_scroll_resume_baseline(
            baseline,
            std::move(refreshed)) &&
            baseline.valid() && baseline.pixels.front() == 93,
        L"resume replaces the comparison baseline with a fresh matching frame");

    const Bitmap preserved = baseline;
    Bitmap wrong_size(3, 4);
    expect(
        !airshot::overlay_detail::replace_scroll_resume_baseline(
            baseline,
            std::move(wrong_size)) &&
            baseline.width == preserved.width &&
            baseline.height == preserved.height &&
            baseline.pixels == preserved.pixels,
        L"resume rejects a changed capture size without losing the old baseline");
    expect(
        !airshot::overlay_detail::replace_scroll_resume_baseline(
            baseline,
            Bitmap{}) &&
            baseline.pixels == preserved.pixels,
        L"resume rejects a failed capture without losing the old baseline");

    using airshot::overlay_detail::ScrollKeyboardCommand;
    expect(
        airshot::overlay_detail::scroll_keyboard_command('P') ==
                ScrollKeyboardCommand::toggle_pause &&
            airshot::overlay_detail::scroll_keyboard_command(VK_SPACE) ==
                ScrollKeyboardCommand::toggle_pause &&
            airshot::overlay_detail::scroll_keyboard_command(VK_RETURN) ==
                ScrollKeyboardCommand::finish &&
            airshot::overlay_detail::scroll_keyboard_command(VK_ESCAPE) ==
                ScrollKeyboardCommand::cancel &&
            airshot::overlay_detail::scroll_keyboard_command('Q') ==
                ScrollKeyboardCommand::none,
        L"overlay keyboard forwarding maps P, Space, Enter, and Escape deterministically");
}

void test_scroll_feedback_and_resume_context() {
    using airshot::overlay_detail::ScrollMatchOutcome;
    using airshot::overlay_detail::ScrollMatchQuality;
    using airshot::overlay_detail::advance_scroll_match_feedback;

    int streak = 0;
    for (int attempt = 1;
         attempt < airshot::overlay_detail::kScrollMatchFailurePauseThreshold;
         ++attempt) {
        const auto feedback = advance_scroll_match_feedback(
            streak,
            ScrollMatchOutcome::mismatch);
        streak = feedback.consecutive_failures;
        expect(
            feedback.quality == ScrollMatchQuality::low_confidence &&
                !feedback.safe_pause && streak == attempt,
            L"transient scroll mismatch remains low confidence without stopping capture");
    }
    const auto failed = advance_scroll_match_feedback(
        streak,
        ScrollMatchOutcome::mismatch);
    expect(
        failed.quality == ScrollMatchQuality::failed &&
            failed.safe_pause &&
            failed.consecutive_failures ==
                airshot::overlay_detail::kScrollMatchFailurePauseThreshold,
        L"fourth consecutive mismatch triggers a deterministic safe pause");

    const auto recovered = advance_scroll_match_feedback(
        failed.consecutive_failures,
        ScrollMatchOutcome::success);
    const auto unchanged = advance_scroll_match_feedback(
        failed.consecutive_failures,
        ScrollMatchOutcome::unchanged);
    const auto hard_failure = advance_scroll_match_feedback(
        2,
        ScrollMatchOutcome::hard_failure);
    expect(
        recovered.quality == ScrollMatchQuality::success &&
            recovered.consecutive_failures == 0 &&
            !recovered.safe_pause &&
            unchanged.quality == ScrollMatchQuality::waiting &&
            unchanged.consecutive_failures == 0 &&
            !unchanged.safe_pause &&
            hard_failure.quality == ScrollMatchQuality::failed &&
            hard_failure.consecutive_failures == 2 &&
            hard_failure.safe_pause,
        L"feedback resets on recovery and immediately pauses on hard failure");

    const airshot::RectI capture_bounds{100, 120, 500, 420};
    expect(
        airshot::overlay_detail::scroll_capture_bounds_match(
            capture_bounds,
            capture_bounds) &&
            !airshot::overlay_detail::scroll_capture_bounds_match(
                capture_bounds,
                {101, 120, 501, 420}) &&
            !airshot::overlay_detail::scroll_capture_bounds_match(
                capture_bounds,
                {100, 120, 500, 421}),
        L"resume requires the original pixel bounds, not merely a similar region");
    expect(
        airshot::overlay_detail::scroll_target_frame_is_stable(
            true,
            capture_bounds,
            capture_bounds,
            capture_bounds) &&
            !airshot::overlay_detail::scroll_target_frame_is_stable(
                false,
                capture_bounds,
                capture_bounds,
                capture_bounds) &&
            !airshot::overlay_detail::scroll_target_frame_is_stable(
                true,
                capture_bounds,
                {101, 120, 501, 420},
                capture_bounds) &&
            !airshot::overlay_detail::scroll_target_frame_is_stable(
                true,
                capture_bounds,
                capture_bounds,
                {100, 120, 500, 421}),
        L"every scroll frame rejects identity, one-pixel move, and one-pixel resize drift");
    expect(
        airshot::overlay_detail::scroll_frame_commit_allowed(false, true) &&
            !airshot::overlay_detail::scroll_frame_commit_allowed(true, true) &&
            !airshot::overlay_detail::scroll_frame_commit_allowed(false, false),
        L"scroll detection cannot commit after pause cancellation or target drift");

    const Bitmap document = make_document(24, 100);
    ScrollStitcher stitcher(frame_from(document, 20, 40));
    expect(
        stitcher.append(frame_from(document, 27, 40), 7) ==
            StitchStatus::success,
        L"safe-pause fixture contains already stitched content");
    Bitmap before_pause;
    Bitmap after_pause;
    expect(
        stitcher.materialize(before_pause) == StitchStatus::success,
        L"safe-pause fixture materializes before failures");
    streak = 0;
    for (int attempt = 0;
         attempt < airshot::overlay_detail::kScrollMatchFailurePauseThreshold;
         ++attempt) {
        streak = advance_scroll_match_feedback(
                     streak,
                     ScrollMatchOutcome::mismatch)
                     .consecutive_failures;
    }
    expect(
        stitcher.materialize(after_pause) == StitchStatus::success &&
            stitcher.frame_count() == 2 &&
            after_pause.width == before_pause.width &&
            after_pause.height == before_pause.height &&
            after_pause.pixels == before_pause.pixels,
        L"safe pause preserves every strip stitched before matching failed");
}

void test_scroll_control_clicks() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto click_button = [](HWND window, int center_dip) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        const int x = MulDiv(bounds.right, center_dip, 640);
        const int y = bounds.bottom / 2;
        SendMessageW(
            window,
            WM_LBUTTONDOWN,
            MK_LBUTTON,
            MAKELPARAM(x, y));
        SendMessageW(
            window,
            WM_LBUTTONUP,
            0,
            MAKELPARAM(x, y));
    };

    airshot::overlay_detail::ScrollControlState pause_state;
    bool pause_toggled = false;
    int capture_ticks = 0;
    pause_state.on_tick = [&] { ++capture_ticks; };
    pause_state.on_toggle_pause = [&] {
        pause_toggled = true;
        pause_state.paused = !pause_state.paused;
    };
    const HWND pause_window = airshot::overlay_detail::create_scroll_control_window(
        instance, nullptr, {100, 100, 400, 400}, &pause_state);
    expect(pause_window != nullptr, L"pause control window is created");
    if (pause_window) {
        expect(
            SendMessageW(
                pause_window,
                WM_MOUSEACTIVATE,
                reinterpret_cast<WPARAM>(pause_window),
                MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
            L"scroll controls never steal focus from the captured target");
        click_button(pause_window, 375);
        expect(
            pause_toggled && pause_state.paused &&
                !pause_state.finished && !pause_state.cancelled,
            L"pause click toggles capture without completing it");
        SendMessageW(
            pause_window,
            WM_TIMER,
            airshot::overlay_detail::kScrollCaptureTimer,
            0);
        expect(
            capture_ticks == 0,
            L"paused scroll controls suppress scheduled frame capture");
        pause_toggled = false;
        pause_state.can_resume = false;
        click_button(pause_window, 375);
        expect(
            !pause_toggled && pause_state.paused,
            L"hard pause disables the continue action");
        DestroyWindow(pause_window);
    }

    airshot::overlay_detail::ScrollControlState finish_state;
    bool finished = false;
    finish_state.on_finish = [&finished] { finished = true; };
    const HWND finish_window = airshot::overlay_detail::create_scroll_control_window(
        instance, nullptr, {100, 100, 400, 400}, &finish_state);
    expect(finish_window != nullptr, L"finish control window is created");
    if (finish_window) {
        click_button(finish_window, 483);
        expect(finished && finish_state.finished && !finish_state.cancelled,
               L"finish click survives capture release");
        DestroyWindow(finish_window);
    }

    airshot::overlay_detail::ScrollControlState cancel_state;
    bool cancelled = false;
    cancel_state.on_cancel = [&cancelled] { cancelled = true; };
    const HWND cancel_window = airshot::overlay_detail::create_scroll_control_window(
        instance, nullptr, {100, 100, 400, 400}, &cancel_state);
    expect(cancel_window != nullptr, L"cancel control window is created");
    if (cancel_window) {
        click_button(cancel_window, 585);
        expect(cancelled && cancel_state.cancelled && !cancel_state.finished,
               L"cancel click survives capture release");
        DestroyWindow(cancel_window);
    }
}

void test_scroll_completion_does_not_inherit_pin_action() {
    using airshot::ExitCode;
    using airshot::RegionAction;
    using airshot::resolve_region_result_action;

    expect(
        resolve_region_result_action(
            ExitCode::success,
            RegionAction::clipboard,
            RegionAction::pin) == RegionAction::clipboard &&
            resolve_region_result_action(
                ExitCode::success,
                RegionAction::file,
                RegionAction::pin) == RegionAction::file,
        L"scroll completion keeps its explicit clipboard/file action instead of becoming a pin");
    expect(
        resolve_region_result_action(
            ExitCode::success,
            RegionAction::interactive,
            RegionAction::pin) == RegionAction::interactive,
        L"successful unspecified completion never silently inherits the session pin action");
}

}  // namespace

int wmain() {
    test_small_frames();
    test_scroll_detection();
    test_global_search_ambiguity();
    test_single_template_does_not_validate_an_unrelated_frame();
    test_overlap_validation_tolerates_fixed_chrome();
    test_bounded_large_frame_search();
    test_chunked_stitching();
    test_stitch_limits_and_compatibility();
    test_resume_baseline_and_keyboard_commands();
    test_scroll_feedback_and_resume_context();
    test_scroll_control_clicks();
    test_scroll_completion_does_not_inherit_pin_action();
    if (failures == 0) {
        std::wcout << L"All scroll tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
