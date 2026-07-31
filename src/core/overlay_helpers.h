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

// Uses the exact GDI font construction and DrawText flags used by
// render_annotations. A zero rectangle means the annotation cannot be
// measured (for example, it is not a non-empty text annotation).
[[nodiscard]] RectI measure_text_annotation_bounds(
    const Annotation& annotation,
    const AppConfig& config) noexcept;
[[nodiscard]] bool refresh_text_annotation_bounds(
    Annotation& annotation,
    const AppConfig& config) noexcept;

[[nodiscard]] COLORREF parse_hex_color(std::wstring_view hex, COLORREF fallback);
[[nodiscard]] std::wstring format_hex_color(COLORREF color);

void show_rgb_picker_popup(HWND parent_hwnd,
                           COLORREF initial_color,
                           const RectI& button_bounds,
                           const RectI& monitor_bounds,
                           std::function<void(COLORREF)> on_color_changed,
                           std::function<void()> on_closed = {});

using TextPromptCompletion = std::function<void(std::optional<std::wstring>)>;

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
                                    TextStyle text_style = TextStyle::normal);
[[nodiscard]] std::optional<std::wstring> prompt_text(HWND owner, POINT position, COLORREF color, float text_size, bool is_light_theme);

struct SelectionSizeInput {
    int width{};
    int height{};
    SelectionSizeAnchor anchor{SelectionSizeAnchor::center};
};

using SelectionSizeCompletion =
    std::function<void(std::optional<SelectionSizeInput>)>;

[[nodiscard]] HWND show_selection_size_prompt(
    HWND owner,
    POINT position,
    int current_width,
    int current_height,
    int maximum_width,
    int maximum_height,
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
    std::function<void()> on_tick;
    std::function<void()> on_toggle_pause;
    std::function<void()> on_finish;
    std::function<void()> on_cancel;
};

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
