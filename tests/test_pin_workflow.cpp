#include "airshot/pin_workflow.h"
#include "airshot/pin_lifecycle_policy.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_lifecycle_semantics() {
    using airshot::PinLifecycleAction;
    using airshot::PinLifecycleState;
    using airshot::transition_pin_lifecycle;

    const auto hidden = transition_pin_lifecycle(
        PinLifecycleState::visible,
        PinLifecycleAction::hide);
    expect(
        hidden == PinLifecycleState::hidden,
        "hide retains a recoverable pin state");
    expect(
        transition_pin_lifecycle(hidden, PinLifecycleAction::show) ==
            PinLifecycleState::visible,
        "show restores a hidden pin without recreating it");
    expect(
        !airshot::should_restore_pin_after_capture(hidden, true) &&
            airshot::should_restore_pin_after_capture(
                PinLifecycleState::visible,
                true) &&
            !airshot::should_restore_pin_after_capture(
                PinLifecycleState::visible,
                false),
        "user-hidden state is independent from temporary capture suspension");

    const auto destroyed = transition_pin_lifecycle(
        PinLifecycleState::visible,
        PinLifecycleAction::destroy);
    expect(
        destroyed == PinLifecycleState::destroyed,
        "destroy is distinct from hide");
    expect(
        transition_pin_lifecycle(destroyed, PinLifecycleAction::show) ==
            PinLifecycleState::destroyed,
        "destroyed pins cannot be revived as hidden pins");
}

void test_escape_and_recoverable_runtime_state() {
    using namespace airshot;

    expect(
        pin_escape_action(false) == PinEscapeAction::hide,
        "Esc hides a pin instead of destroying it");
    expect(
        pin_escape_action(true) == PinEscapeAction::confirm_destroy,
        "Shift+Esc is the explicit permanent-destroy gesture");

    PinRuntimeState state;
    state.presentation = {
        1.75,
        143,
        false,
        PinVisualEffects{true, true},
        false,
        true,
    };
    const PinPresentationState retained = state.presentation;

    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::hide,
        41);
    expect(
        state.lifecycle == PinLifecycleState::hidden &&
            state.hidden_order == 41 &&
            state.presentation == retained,
        "Esc hide retains scale, alpha, filters, topmost and click-through state");

    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::hide,
        99);
    expect(
        state.hidden_order == 41 && state.presentation == retained,
        "repeated hide is idempotent and keeps the original recovery order");

    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::show);
    expect(
        state.lifecycle == PinLifecycleState::visible &&
            state.hidden_order == 0 &&
            state.presentation == retained,
        "restoring a hidden pin preserves all presentation state");

    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::hide,
        100);
    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::show);
    expect(
        state.lifecycle == PinLifecycleState::visible &&
            state.presentation == retained,
        "repeated hide and restore cycles do not degrade state");

    state = transition_pin_runtime_state(
        state,
        PinLifecycleAction::destroy);
    const PinRuntimeState after_destroy = transition_pin_runtime_state(
        state,
        PinLifecycleAction::show);
    expect(
        after_destroy.lifecycle == PinLifecycleState::destroyed &&
            after_destroy.hidden_order == 0 &&
            after_destroy.presentation == retained,
        "permanently destroyed pins cannot be restored");
}

void test_bulk_destroy_snapshot_policy() {
    using namespace airshot;

    expect(
        can_begin_pin_bulk_destroy(false, 2),
        "bulk destroy begins when pins exist and no confirmation is active");
    expect(
        !can_begin_pin_bulk_destroy(true, 2),
        "bulk destroy rejects a nested confirmation");
    expect(
        !can_begin_pin_bulk_destroy(false, 0),
        "bulk destroy skips an empty snapshot");

    constexpr std::array<std::uint64_t, 2> confirmed_identifiers{11, 19};
    expect(
        pin_destroy_snapshot_contains(confirmed_identifiers, 11) &&
            pin_destroy_snapshot_contains(confirmed_identifiers, 19),
        "bulk destroy includes every pin present at confirmation time");
    expect(
        !pin_destroy_snapshot_contains(confirmed_identifiers, 23),
        "pins created while confirmation is open are excluded from destruction");
}

void test_budget_transactions() {
    using namespace airshot;

    const auto create = plan_pin_bitmap_change(2, 12, 0, 4);
    expect(
        create.allowed() && create.total_bytes_after == 16,
        "creating a pin accounts for its decoded bytes");

    const auto replace = plan_pin_bitmap_change(2, 12, 4, 6);
    expect(
        replace.allowed() && replace.total_bytes_after == 14,
        "replacement subtracts the old bitmap before adding the new one");

    const auto oversized = plan_pin_bitmap_change(
        2,
        kMaximumPinBytes,
        1024,
        kMaximumPinBytes + 1);
    expect(
        oversized.status == PinBudgetStatus::single_pin_limit &&
            oversized.total_bytes_after == kMaximumPinBytes,
        "denied replacement keeps the original total allocation unchanged");

    const auto total_limit = plan_pin_bitmap_change(
        3,
        kMaximumTotalPinBytes,
        1024,
        2048);
    expect(
        total_limit.status == PinBudgetStatus::total_pin_limit &&
            total_limit.total_bytes_after == kMaximumTotalPinBytes,
        "total-budget failure is transactional and retains the original pin");

    const auto count_limit = plan_pin_bitmap_change(
        kMaximumPinCount,
        1024,
        0,
        1024);
    expect(
        count_limit.status == PinBudgetStatus::pin_count_limit,
        "new pins obey the count limit");
    const auto replacement_at_count_limit = plan_pin_bitmap_change(
        kMaximumPinCount,
        1024,
        1024,
        1024);
    expect(
        replacement_at_count_limit.allowed(),
        "replacing an existing pin is still allowed at the count limit");
}

void test_state_counts_and_toggle_plan() {
    using namespace airshot;

    const std::array states{
        PinStateView{false, false, false},
        PinStateView{true, true, false},
        PinStateView{false, true, true},
    };
    const PinStateCounts counts = summarize_pin_states(states);
    expect(
        counts.total == 3 && counts.visible == 2 &&
            counts.hidden == 1 && counts.click_through == 2,
        "tray counts visible, hidden and click-through pins independently");

    const PinTogglePlan target = plan_pin_toggle(states);
    expect(
        target.action == PinToggleAction::toggle_target &&
            target.target_index == 2,
        "toggle selects the first visible cursor hit in Z order");

    const std::array no_target{
        PinStateView{false, false, false},
        PinStateView{true, true, true},
        PinStateView{false, true, false},
    };
    const PinTogglePlan recovery = plan_pin_toggle(no_target);
    expect(
        recovery.action == PinToggleAction::restore_all,
        "no cursor target restores every click-through pin, including hidden pins");

    const std::array idle{
        PinStateView{false, false, false},
        PinStateView{true, false, false},
    };
    expect(
        plan_pin_toggle(idle).action == PinToggleAction::none,
        "no target and no click-through state is a safe no-op");
}

void test_visual_effects_and_scale_policy() {
    using namespace airshot;

    Bitmap source;
    source.width = 2;
    source.height = 1;
    source.pixels = {
        10, 20, 30, 255,
        200, 100, 0, 128,
    };
    const std::vector<std::uint8_t> original_pixels = source.pixels;

    const Bitmap unchanged = render_pin_visual_bitmap(source, {});
    expect(
        unchanged.width == source.width &&
            unchanged.height == source.height &&
            unchanged.pixels == original_pixels,
        "disabled effects reproduce the current source pixel exactly");

    const Bitmap grayscale = render_pin_visual_bitmap(
        source,
        PinVisualEffects{true, false});
    expect(
        grayscale.pixels == std::vector<std::uint8_t>({
            22, 22, 22, 255,
            81, 81, 81, 128,
        }),
        "grayscale is derived once from source and preserves alpha");

    const Bitmap inverted = render_pin_visual_bitmap(
        source,
        PinVisualEffects{false, true});
    expect(
        inverted.pixels == std::vector<std::uint8_t>({
            245, 235, 225, 255,
            55, 155, 255, 128,
        }),
        "inversion is derived from source and preserves alpha");

    const Bitmap combined = render_pin_visual_bitmap(
        source,
        PinVisualEffects{true, true});
    expect(
        combined.pixels == std::vector<std::uint8_t>({
            233, 233, 233, 255,
            174, 174, 174, 128,
        }),
        "combined grayscale and inversion use one source-derived pass");
    expect(
        source.pixels == original_pixels,
        "rendering display effects never mutates the current source bitmap");

    Bitmap fully_transparent(2, 1);
    fully_transparent.pixels = {
        10, 20, 30, 0,
        40, 50, 60, 0,
    };
    const Bitmap preserved_transparency =
        render_pin_visual_bitmap(fully_transparent, {});
    expect(
        preserved_transparency.valid() &&
            preserved_transparency.pixels == fully_transparent.pixels &&
            summarize_pin_bitmap_alpha(fully_transparent) ==
                PinBitmapAlphaSummary{true, true},
        "PinWindow treats normalized all-zero alpha as intentional transparency");

    Bitmap opaque(2, 1);
    opaque.pixels = {
        10, 20, 30, 255,
        40, 50, 60, 255,
    };
    expect(
        summarize_pin_bitmap_alpha(opaque) ==
            PinBitmapAlphaSummary{true, false} &&
            summarize_pin_bitmap_alpha(Bitmap{}) ==
                PinBitmapAlphaSummary{},
        "alpha summary separates opaque, transparent, and invalid sources");

    const Bitmap premultiplied = render_pin_layered_bitmap(
        source,
        {},
        source.width,
        source.height,
        false);
    expect(
        premultiplied.valid() &&
            premultiplied.pixels == std::vector<std::uint8_t>({
                10, 20, 30, 255,
                100, 50, 0, 128,
            }),
        "layered presentation produces exact premultiplied BGRA pixels");
    const Bitmap nearest_scaled = render_pin_layered_bitmap(
        source,
        {},
        4,
        1,
        false);
    expect(
        nearest_scaled.valid() &&
            nearest_scaled.pixels[3] == 255 &&
            nearest_scaled.pixels[7] == 255 &&
            nearest_scaled.pixels[11] == 128 &&
            nearest_scaled.pixels[15] == 128,
        "nearest layered scaling refreshes alpha at the actual window size");
    const Bitmap smooth_scaled = render_pin_layered_bitmap(
        source,
        {},
        3,
        1,
        true);
    expect(
        smooth_scaled.valid() &&
            smooth_scaled.pixels[3] == 255 &&
            smooth_scaled.pixels[7] == 192 &&
            smooth_scaled.pixels[11] == 128 &&
            smooth_scaled.pixels[4] <= smooth_scaled.pixels[7] &&
            smooth_scaled.pixels[5] <= smooth_scaled.pixels[7] &&
            smooth_scaled.pixels[6] <= smooth_scaled.pixels[7],
        "smooth layered scaling interpolates premultiplied color and alpha");

    expect(
        plan_pin_window_style(false, 255, false) ==
                PinWindowStylePlan{false, false} &&
            plan_pin_window_style(false, 128, false) ==
                PinWindowStylePlan{true, false} &&
            plan_pin_window_style(false, 255, true) ==
                PinWindowStylePlan{true, false} &&
            plan_pin_window_style(true, 255, false) ==
                PinWindowStylePlan{true, true} &&
            plan_pin_window_style(true, 128, true) ==
                PinWindowStylePlan{true, true},
        "style matrix reserves per-pixel layering for transparent sources");
    expect(
        !pin_window_needs_layered_style(255, false) &&
            pin_window_needs_layered_style(254, false) &&
            pin_window_needs_layered_style(255, true),
        "an opaque interactive pin starts on the reliable non-layered paint path");

    PinVisualEffects effects;
    effects = transition_pin_visual_effects(
        effects,
        PinVisualEffectAction::toggle_grayscale);
    effects = transition_pin_visual_effects(
        effects,
        PinVisualEffectAction::toggle_inverted);
    expect(
        effects == PinVisualEffects{true, true},
        "effect shortcuts compose as independent checked states");
    effects = transition_pin_visual_effects(
        effects,
        PinVisualEffectAction::toggle_grayscale);
    expect(
        effects == PinVisualEffects{false, true},
        "disabling grayscale leaves inversion active without accumulated loss");
    effects = transition_pin_visual_effects(
        effects,
        PinVisualEffectAction::toggle_inverted);
    expect(
        effects == PinVisualEffects{},
        "disabling both effects restores exact source semantics");

    const PinVisualEffects active{true, true};
    expect(
        pin_effects_after_source_change(
            active,
            PinSourceChange::replace) == active &&
            pin_effects_after_source_change(
                active,
                PinSourceChange::rotate) == active &&
            pin_effects_after_source_change(
                active,
                PinSourceChange::flip) == active,
        "replace, rotate and flip preserve toggles while changing source once");
    expect(
        pin_copy_uses_visible_effects(),
        "copy policy exports the current visible effect by default");

    std::array<std::uint8_t, 3> wrong_size{};
    expect(
        !write_pin_visual_pixels(source, active, wrong_size),
        "effect renderer rejects mismatched output storage");

    expect(
        parse_pin_scale_percent(L"25") == 25 &&
            parse_pin_scale_percent(L" 200 % ") == 200 &&
            parse_pin_scale_percent(L"1000") == 1000,
        "scale input accepts integer percentages and optional percent suffix");
    expect(
        !parse_pin_scale_percent(L"9") &&
            !parse_pin_scale_percent(L"1001") &&
            !parse_pin_scale_percent(L"12.5") &&
            !parse_pin_scale_percent(L"abc"),
        "scale input rejects out-of-range and non-integer values");
    expect(
        pin_scale_factor_from_percent(25) == 0.25 &&
            pin_scale_factor_from_percent(50) == 0.5 &&
            pin_scale_factor_from_percent(100) == 1.0 &&
            pin_scale_factor_from_percent(200) == 2.0,
        "scale presets map to exact factors");
    const PinScalePromptPlan cancelled = plan_pin_scale_prompt(std::nullopt);
    const PinScalePromptPlan accepted = plan_pin_scale_prompt(375);
    expect(
        !cancelled.apply && accepted.apply && accepted.percent == 375,
        "cancelling custom scale is a no-op while valid input is applied");
}

void test_border_resize_and_thumbnail_policy() {
    using namespace airshot;

    expect(
        pin_resize_hit_test(1, 1, 400, 200, 8) ==
                PinResizeEdge::top_left &&
            pin_resize_hit_test(399, 100, 400, 200, 8) ==
                PinResizeEdge::right &&
            pin_resize_hit_test(200, 199, 400, 200, 8) ==
                PinResizeEdge::bottom &&
            pin_resize_hit_test(200, 100, 400, 200, 8) ==
                PinResizeEdge::none,
        "border hit testing prioritizes corners, exposes side handles, and leaves the interior draggable");
    expect(
        pin_resize_hit_test(0, 0, 5, 5, 20) ==
                PinResizeEdge::top_left &&
            pin_resize_hit_test(-1, 0, 5, 5, 2) ==
                PinResizeEdge::none,
        "resize hit targets stay valid on tiny windows and reject out-of-window points");

    const RectI generous_work_area{-1000, -800, 2000, 1200};
    const PinScaleLimits limits = pin_scale_limits(
        400,
        200,
        {0, 0, 800, 600});
    expect(
        limits.valid && std::abs(limits.minimum - 0.08) < 0.0001 &&
            std::abs(limits.maximum - 2.0) < 0.0001,
        "interactive scale limits combine the public range with the current work area");
    const PinScaleLimits resource_limited = pin_scale_limits(
        100,
        100,
        generous_work_area,
        100U * 100U * Bitmap::bytes_per_pixel);
    expect(
        resource_limited.valid &&
            std::abs(resource_limited.maximum - 1.0) < 0.0001,
        "interactive scaling cannot exceed the rendered-frame resource budget");

    const RectI original{100, 100, 500, 300};
    const PinResizePlan centered = plan_pin_scale_resize(
        400,
        200,
        original,
        {300, 200},
        1.5,
        generous_work_area);
    expect(
        centered.apply && centered.bounds.left == 0 &&
            centered.bounds.top == 50 &&
            centered.bounds.width() == 600 &&
            centered.bounds.height() == 300 &&
            std::abs(centered.scale - 1.5) < 0.0001,
        "wheel and exact scaling preserve the screen anchor and source aspect ratio");

    const PinResizePlan right = plan_pin_drag_resize(
        PinResizeEdge::right,
        400,
        200,
        original,
        {700, 200},
        generous_work_area);
    expect(
        right.apply && right.bounds.left == original.left &&
            right.bounds.width() == 600 &&
            right.bounds.height() == 300 &&
            right.bounds.top == 50,
        "side dragging fixes the opposite side and recentres the proportional axis");

    const PinResizePlan corner = plan_pin_drag_resize(
        PinResizeEdge::top_left,
        400,
        200,
        original,
        {-100, 0},
        generous_work_area);
    expect(
        corner.apply && corner.bounds.right == original.right &&
            corner.bounds.bottom == original.bottom &&
            corner.bounds.width() == 600 &&
            corner.bounds.height() == 300 &&
            std::abs(corner.scale - 1.5) < 0.0001,
        "corner dragging projects the pointer onto the aspect ratio and fixes the opposite corner");

    const PinResizePlan capped = plan_pin_drag_resize(
        PinResizeEdge::bottom_right,
        400,
        200,
        {0, 0, 400, 200},
        {5000, 5000},
        {0, 0, 800, 600});
    expect(
        capped.apply && capped.bounds.width() == 800 &&
            capped.bounds.height() == 400 &&
            std::abs(capped.scale - 2.0) < 0.0001,
        "dragging beyond the monitor clamps to the largest work-area-safe scale");

    const PinThumbnailPlan enter = plan_pin_thumbnail_toggle(
        {},
        1.75,
        800,
        400,
        192,
        generous_work_area);
    expect(
        enter.apply && enter.state.active &&
            std::abs(enter.state.restore_scale - 1.75) < 0.0001 &&
            std::abs(enter.scale - 0.24) < 0.0001,
        "entering fixed-thumbnail mode records the exact previous scale and targets a fixed long edge");
    const PinThumbnailPlan leave = plan_pin_thumbnail_toggle(
        enter.state,
        enter.scale,
        800,
        400,
        192,
        generous_work_area);
    expect(
        leave.apply && !leave.state.active &&
            std::abs(leave.scale - 1.75) < 0.0001,
        "leaving fixed-thumbnail mode restores the pre-thumbnail scale");
    expect(
        std::abs(pin_fixed_thumbnail_scale(
                     400,
                     800,
                     192,
                     generous_work_area) -
                 0.24) < 0.0001,
        "thumbnail sizing follows the rotated source dimensions without changing its long edge");
    expect(
        !plan_pin_thumbnail_toggle(
             {},
             1.0,
             0,
             100,
             192,
             generous_work_area)
             .apply,
        "thumbnail planning rejects invalid sources without changing state");
}

}  // namespace

int main() {
    test_lifecycle_semantics();
    test_escape_and_recoverable_runtime_state();
    test_bulk_destroy_snapshot_policy();
    test_budget_transactions();
    test_state_counts_and_toggle_plan();
    test_visual_effects_and_scale_policy();
    test_border_resize_and_thumbnail_policy();
    if (failures == 0) {
        std::cout << "All pin workflow tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
