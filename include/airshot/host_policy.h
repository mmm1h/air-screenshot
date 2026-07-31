#pragma once

namespace airshot {

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
    bool update_helper_launched) noexcept {
    return host_initialized && !host_is_transient &&
           !update_helper_launched;
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
