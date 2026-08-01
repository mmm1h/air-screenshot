#pragma once

#include "airshot/bitmap.h"
#include "airshot/config.h"
#include "overlay_types.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace airshot::overlay_detail {

[[nodiscard]] Bitmap render_annotations(Bitmap base,
                                        const std::vector<Annotation>& annotations,
                                        const AppConfig& config);

struct TextAnnotationMeasurement {
    RectI layout_bounds{};
    RectI visual_bounds{};
};

// Uses the exact GDI font construction and DrawText flags used by
// render_annotations. A zero rectangle means the annotation cannot be
// measured (for example, it is not a non-empty text annotation).
[[nodiscard]] TextAnnotationMeasurement measure_text_annotation(
    const Annotation& annotation,
    const AppConfig& config) noexcept;
[[nodiscard]] RectI measure_text_annotation_bounds(
    const Annotation& annotation,
    const AppConfig& config) noexcept;
[[nodiscard]] bool refresh_text_annotation_bounds(
    Annotation& annotation,
    const AppConfig& config) noexcept;
[[nodiscard]] bool fit_text_annotation_to_canvas(
    Annotation& annotation,
    const AppConfig& config,
    int canvas_width,
    int canvas_height) noexcept;

[[nodiscard]] COLORREF parse_hex_color(std::wstring_view hex, COLORREF fallback);
[[nodiscard]] std::wstring format_hex_color(COLORREF color);

void show_rgb_picker_popup(HWND parent_hwnd,
                           COLORREF initial_color,
                           const RectI& button_bounds,
                           const RectI& monitor_bounds,
                           std::function<void(COLORREF)> on_color_changed,
                           std::function<void()> on_closed = {});

using TextPromptCompletion = std::function<void(std::optional<std::wstring>)>;

// Keep the text editor's close semantics independent from the native window
// procedure. In particular, losing activation is not a cancellation signal:
// toolbar clicks, Alt+Tab and IME-owned UI can all deactivate the popup for a
// short time while the user's draft must remain intact.
enum class TextPromptEvent {
    enter,
    escape,
    deactivated,
    close_request,
};

enum class TextPromptAction {
    keep_editing,
    pass_to_editor,
    accept,
    cancel,
};

[[nodiscard]] constexpr TextPromptAction text_prompt_action(
    TextPromptEvent event,
    bool shift_down = false,
    bool ime_composing = false) noexcept {
    switch (event) {
        case TextPromptEvent::enter:
            if (shift_down || ime_composing) {
                return TextPromptAction::pass_to_editor;
            }
            return TextPromptAction::accept;
        case TextPromptEvent::escape:
            if (ime_composing) {
                return TextPromptAction::pass_to_editor;
            }
            return TextPromptAction::cancel;
        case TextPromptEvent::close_request:
            return TextPromptAction::cancel;
        case TextPromptEvent::deactivated:
            return TextPromptAction::keep_editing;
    }
    return TextPromptAction::keep_editing;
}

[[nodiscard]] HWND show_text_prompt(HWND owner,
                                    POINT position,
                                    COLORREF color,
                                    float text_size,
                                    bool is_light_theme,
                                    TextPromptCompletion completion,
                                    std::wstring initial_text = {},
                                    std::wstring_view font_family = L"Microsoft YaHei",
                                    bool font_bold = false,
                                    bool font_italic = false,
                                    TextStyle text_style = TextStyle::normal,
                                    int text_box_width_px = 0);
[[nodiscard]] std::optional<std::wstring> prompt_text(HWND owner, POINT position, COLORREF color, float text_size, bool is_light_theme);

struct SelectionSizeInput {
    int x{};
    int y{};
    int width{};
    int height{};
    SelectionSizeAnchor anchor{SelectionSizeAnchor::center};
    bool aspect_ratio_locked{};
    int corner_radius{};
};

using SelectionSizeCompletion =
    std::function<void(std::optional<SelectionSizeInput>)>;

[[nodiscard]] HWND show_selection_size_prompt(
    HWND owner,
    POINT position,
    RectI current_selection,
    RectI desktop_bounds,
    SelectionSizeAnchor current_anchor,
    bool aspect_ratio_locked,
    int current_corner_radius,
    bool is_light_theme,
    SelectionSizeCompletion completion);

struct ScrollControlState {
    bool finished{false};
    bool cancelled{false};
    bool paused{false};
    bool can_resume{true};
    int hover_button{};
    int pressed_button{};
    int blink_counter{};
    enum class MatchQuality {
        waiting,
        success,
        low_confidence,
        failed,
    };
    struct Progress {
        int capture_width{};
        int stitched_height{};
        int stitched_frames{};
        MatchQuality match_quality{MatchQuality::waiting};
    } progress;
    std::function<void()> on_tick;
    std::function<void()> on_toggle_pause;
    std::function<void()> on_finish;
    std::function<void()> on_cancel;
};

using ScrollMatchQuality = ScrollControlState::MatchQuality;

enum class ScrollMatchOutcome {
    success,
    unchanged,
    mismatch,
    hard_failure,
};

struct ScrollMatchFeedback {
    ScrollMatchQuality quality{ScrollMatchQuality::waiting};
    int consecutive_failures{};
    bool safe_pause{};
};

inline constexpr int kScrollMatchFailurePauseThreshold = 4;

// Converts capture outcomes into one product-level feedback policy. Transient
// mismatches are reported as low confidence; the fourth consecutive mismatch
// becomes a safe pause. A successful/unchanged frame resets the streak.
[[nodiscard]] constexpr ScrollMatchFeedback advance_scroll_match_feedback(
    int consecutive_failures,
    ScrollMatchOutcome outcome) noexcept {
    const int normalized_failures =
        consecutive_failures > 0 ? consecutive_failures : 0;
    switch (outcome) {
        case ScrollMatchOutcome::success:
            return {ScrollMatchQuality::success, 0, false};
        case ScrollMatchOutcome::unchanged:
            return {ScrollMatchQuality::waiting, 0, false};
        case ScrollMatchOutcome::mismatch: {
            const int next_failures =
                normalized_failures >= kScrollMatchFailurePauseThreshold
                    ? kScrollMatchFailurePauseThreshold
                    : normalized_failures + 1;
            return {
                next_failures >= kScrollMatchFailurePauseThreshold
                    ? ScrollMatchQuality::failed
                    : ScrollMatchQuality::low_confidence,
                next_failures,
                next_failures >= kScrollMatchFailurePauseThreshold};
        }
        case ScrollMatchOutcome::hard_failure:
            return {
                ScrollMatchQuality::failed,
                normalized_failures,
                true};
    }
    return {ScrollMatchQuality::failed, normalized_failures, true};
}

[[nodiscard]] constexpr bool scroll_capture_bounds_match(
    const RectI& expected,
    const RectI& current) noexcept {
    return expected.left == current.left &&
           expected.top == current.top &&
           expected.right == current.right &&
           expected.bottom == current.bottom &&
           expected.right > expected.left &&
           expected.bottom > expected.top;
}

[[nodiscard]] constexpr bool scroll_target_frame_is_stable(
    bool identity_matches,
    const RectI& expected,
    const RectI& before_capture,
    const RectI& after_capture) noexcept {
    return identity_matches &&
           scroll_capture_bounds_match(expected, before_capture) &&
           scroll_capture_bounds_match(expected, after_capture);
}

[[nodiscard]] constexpr bool scroll_frame_commit_allowed(
    bool stop_requested,
    bool target_stable) noexcept {
    return !stop_requested && target_stable;
}

inline constexpr UINT_PTR kScrollBlinkTimer = 1;
inline constexpr UINT_PTR kScrollCaptureTimer = 2;

enum class ScrollKeyboardCommand {
    none,
    toggle_pause,
    finish,
    cancel,
};

[[nodiscard]] inline ScrollKeyboardCommand scroll_keyboard_command(
    WPARAM key) noexcept {
    if (key == 'P' || key == VK_SPACE) {
        return ScrollKeyboardCommand::toggle_pause;
    }
    if (key == VK_RETURN) {
        return ScrollKeyboardCommand::finish;
    }
    if (key == VK_ESCAPE) {
        return ScrollKeyboardCommand::cancel;
    }
    return ScrollKeyboardCommand::none;
}

[[nodiscard]] HWND create_scroll_border_window(HINSTANCE instance, HWND parent, const RectI& bounds);
[[nodiscard]] HWND create_scroll_control_window(HINSTANCE instance, HWND parent, const RectI& selection, ScrollControlState* state);

[[nodiscard]] bool is_bitmap_static(const Bitmap& bmp1, const Bitmap& bmp2);
[[nodiscard]] int find_best_template_y(const Bitmap& frame, int direction);

struct ScrollResult {
    bool matched{false};
    int direction{0};
    int offset{0};

    enum class Status {
        mismatch,
        unchanged,
        moved_down,
        moved_up,
    };

    [[nodiscard]] Status status() const noexcept {
        if (!matched) return Status::mismatch;
        if (direction > 0 && offset > 0) return Status::moved_down;
        if (direction < 0 && offset > 0) return Status::moved_up;
        return Status::unchanged;
    }
};

struct ScrollSearchStats {
    std::size_t candidates_evaluated{};
};

inline constexpr std::size_t kMaxScrollSearchCandidates = 1024;
[[nodiscard]] ScrollResult detect_scroll(const Bitmap& last_frame,
                                         const Bitmap& new_frame,
                                         int locked_direction,
                                         ScrollSearchStats* stats = nullptr);

// Materialization needs two full pixel buffers. Clipboard output can retain the
// source, an opaque copy, three bitmap representations, and three PNG buffers.
// One additional copy-sized margin covers PNG scanline/container overhead.
inline constexpr std::size_t kMaxScrollWorkingSetBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kMaxScrollResidentBufferCopies = 9;
inline constexpr std::size_t kMaxScrollBitmapBytes =
    kMaxScrollWorkingSetBytes / kMaxScrollResidentBufferCopies;
inline constexpr int kMaxScrollBitmapHeight = 65'535;

[[nodiscard]] bool scroll_bitmap_fits_budget(int width, int height) noexcept;
// A resume frame becomes the new comparison baseline only when it is a valid
// capture of the exact same pixel region. Failure leaves the previous baseline
// untouched so the caller can remain paused and retry safely.
[[nodiscard]] bool replace_scroll_resume_baseline(
    Bitmap& baseline,
    Bitmap candidate) noexcept;

enum class StitchStatus {
    success,
    invalid_input,
    dimension_mismatch,
    direction_mismatch,
    limit_reached,
    allocation_failed,
};

class ScrollStitcher {
public:
    explicit ScrollStitcher(Bitmap initial_frame);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] int frame_count() const noexcept;
    [[nodiscard]] int direction() const noexcept;

    [[nodiscard]] StitchStatus append(const Bitmap& new_frame, int offset);
    [[nodiscard]] StitchStatus prepend(const Bitmap& new_frame, int offset);
    [[nodiscard]] StitchStatus materialize(Bitmap& output) const;

private:
    [[nodiscard]] StitchStatus add_strip(const Bitmap& new_frame, int offset, int direction);

    Bitmap initial_frame_;
    std::vector<Bitmap> prepended_strips_;
    std::vector<Bitmap> appended_strips_;
    int width_{};
    int height_{};
    int direction_{};
    bool valid_{false};
};

// Compatibility helpers. ScrollStitcher should be used for a capture session so
// upward scrolling does not repeatedly move the entire accumulated bitmap.
// Aliasing stitched and new_frame is rejected and leaves stitched unchanged.
void append_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d);
void prepend_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d);

}  // namespace airshot::overlay_detail
