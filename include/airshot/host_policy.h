#pragma once

#include "airshot/update_policy.h"

#include <cstdint>

namespace airshot {

struct SeamlessUpdateActivationContext {
    bool update_ready{};
    bool automatic_updates_enabled{};
    bool seamless_updates_enabled{};
    bool app_idle{};
    // False while a presentation or full-screen foreground app should not be
    // interrupted. The platform-specific detection stays outside this policy.
    bool presentation_interruption_allowed{};
    std::int64_t defer_until_unix{};
    std::int64_t now_unix{};
    std::uint64_t staged_elapsed_ms{};
    std::uint64_t user_idle_ms{};
    std::int64_t idle_threshold_minutes{kUpdateDefaultIdleMinutes};
};

[[nodiscard]] constexpr bool seamless_update_activation_allowed(
    const SeamlessUpdateActivationContext& context) noexcept {
    if (!context.update_ready || !context.automatic_updates_enabled ||
        !context.seamless_updates_enabled || !context.app_idle ||
        !context.presentation_interruption_allowed) {
        return false;
    }

    if (context.defer_until_unix > 0 &&
        (context.now_unix <= 0 ||
         context.now_unix < context.defer_until_unix)) {
        return false;
    }

    if (context.staged_elapsed_ms < kUpdateStagedSettleMs) {
        return false;
    }

    const auto idle_minutes = static_cast<std::uint64_t>(
        normalized_update_idle_minutes(context.idle_threshold_minutes));
    constexpr std::uint64_t kMillisecondsPerMinute = 60 * 1'000;
    const std::uint64_t required_user_idle_ms =
        idle_minutes * kMillisecondsPerMinute;
    return context.user_idle_ms >= required_user_idle_ms;
}

[[nodiscard]] constexpr bool effective_startup_enabled(
    bool shell_enabled,
    bool start_at_login) noexcept {
    return shell_enabled && start_at_login;
}

[[nodiscard]] constexpr bool startup_sync_required(
    bool current_shell_enabled,
    bool current_start_at_login,
    bool next_shell_enabled,
    bool next_start_at_login) noexcept {
    return effective_startup_enabled(
               current_shell_enabled,
               current_start_at_login) !=
           effective_startup_enabled(
               next_shell_enabled,
               next_start_at_login);
}

[[nodiscard]] constexpr bool keep_settings_launch_transient(
    bool host_is_transient,
    bool shell_enabled) noexcept {
    return host_is_transient && !shell_enabled;
}

[[nodiscard]] constexpr bool launch_pending_update_on_host_exit(
    bool host_initialized,
    bool host_is_transient,
    bool update_helper_launched,
    bool host_exit_succeeded,
    bool system_session_ending = false) noexcept {
    return host_initialized && !host_is_transient &&
           !update_helper_launched && host_exit_succeeded &&
           !system_session_ending;
}

[[nodiscard]] constexpr bool check_pending_update_on_host_startup(
    bool host_owns_instance,
    bool host_is_transient) noexcept {
    return host_owns_instance && !host_is_transient;
}

[[nodiscard]] constexpr bool cancel_update_when_automatic_is_disabled(
    bool update_running,
    bool update_user_triggered) noexcept {
    return update_running && !update_user_triggered;
}

enum class AutomaticUpdateRuntimeAction {
    none,
    schedule,
    disable,
    disable_and_cancel,
};

[[nodiscard]] constexpr AutomaticUpdateRuntimeAction
automatic_update_runtime_action(
    bool previous_enabled,
    bool next_enabled,
    bool update_running,
    bool update_user_triggered) noexcept {
    if (previous_enabled == next_enabled) {
        return AutomaticUpdateRuntimeAction::none;
    }
    if (next_enabled) {
        return AutomaticUpdateRuntimeAction::schedule;
    }
    return cancel_update_when_automatic_is_disabled(
               update_running,
               update_user_triggered)
               ? AutomaticUpdateRuntimeAction::disable_and_cancel
               : AutomaticUpdateRuntimeAction::disable;
}

}  // namespace airshot
