#include "airshot/pin_workflow.h"

#include <array>
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

}  // namespace

int main() {
    test_lifecycle_semantics();
    test_budget_transactions();
    test_state_counts_and_toggle_plan();
    test_visual_effects_and_scale_policy();
    if (failures == 0) {
        std::cout << "All pin workflow tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
