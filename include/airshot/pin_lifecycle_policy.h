#pragma once

#include "airshot/pin_workflow.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace airshot {

enum class PinEscapeAction {
    hide,
    confirm_destroy,
};

[[nodiscard]] constexpr PinEscapeAction pin_escape_action(
    bool shift_pressed) noexcept {
    return shift_pressed
               ? PinEscapeAction::confirm_destroy
               : PinEscapeAction::hide;
}

// Presentation settings live beside, but are deliberately independent from,
// the recoverable visibility lifecycle. A hide/show transition must never
// rebuild a pin or reset any of these user-controlled values.
struct PinPresentationState {
    double scale{1.0};
    int alpha{255};
    bool smooth_scaling{true};
    PinVisualEffects visual_effects{};
    bool topmost{true};
    bool click_through{};

    [[nodiscard]] friend constexpr bool operator==(
        const PinPresentationState&,
        const PinPresentationState&) noexcept = default;
};

struct PinRuntimeState {
    PinLifecycleState lifecycle{PinLifecycleState::visible};
    std::uint64_t hidden_order{};
    PinPresentationState presentation{};

    [[nodiscard]] friend constexpr bool operator==(
        const PinRuntimeState&,
        const PinRuntimeState&) noexcept = default;
};

[[nodiscard]] constexpr PinRuntimeState transition_pin_runtime_state(
    PinRuntimeState state,
    PinLifecycleAction action,
    std::uint64_t hidden_order = 0) noexcept {
    const PinLifecycleState previous = state.lifecycle;
    state.lifecycle = transition_pin_lifecycle(state.lifecycle, action);
    if (state.lifecycle == PinLifecycleState::destroyed ||
        state.lifecycle == PinLifecycleState::visible) {
        state.hidden_order = 0;
    } else if (previous != PinLifecycleState::hidden) {
        state.hidden_order = hidden_order;
    }
    return state;
}

// Bulk destruction is deliberately snapshot-based. A modal confirmation pumps
// messages, so pins created while it is open must not be added to the operation
// the user already confirmed. The active flag also prevents nested prompts
// without blocking unrelated pin commands.
[[nodiscard]] constexpr bool can_begin_pin_bulk_destroy(
    bool confirmation_active,
    std::size_t snapshot_size) noexcept {
    return !confirmation_active && snapshot_size != 0;
}

[[nodiscard]] constexpr bool pin_destroy_snapshot_contains(
    std::span<const std::uint64_t> identifiers,
    std::uint64_t candidate) noexcept {
    for (const std::uint64_t identifier : identifiers) {
        if (identifier == candidate) {
            return true;
        }
    }
    return false;
}

}  // namespace airshot
