#include "airshot/host_policy.h"

#include <iostream>

int wmain() {
    int failures = 0;
    const auto expect = [&failures](bool condition, const wchar_t* message) {
        if (!condition) {
            std::wcerr << L"FAIL: " << message << L'\n';
            ++failures;
        }
    };

    using namespace airshot;
    expect(
        !startup_sync_required(true, true, true, true),
        L"unchanged startup state does not touch the registry");
    expect(
        !startup_sync_required(false, true, false, false),
        L"start-at-login changes are inert while shell is disabled");
    expect(
        startup_sync_required(true, false, true, true),
        L"enabling startup requires synchronization");
    expect(
        startup_sync_required(true, true, false, true),
        L"disabling shell removes an effective startup entry");

    expect(
        keep_settings_launch_transient(true, false),
        L"settings-only host remains transient when shell is disabled");
    expect(
        !keep_settings_launch_transient(true, true),
        L"saving shell enabled promotes the settings host");
    expect(
        !keep_settings_launch_transient(false, false),
        L"an existing persistent host does not become transient");

    expect(
        launch_pending_update_on_host_exit(true, false, false, true),
        L"an initialized persistent host checks pending updates on exit");
    expect(
        !launch_pending_update_on_host_exit(false, false, false, true),
        L"a host that failed initialization cannot apply pending updates");
    expect(
        !launch_pending_update_on_host_exit(true, true, false, true),
        L"a transient host never applies pending updates on exit");
    expect(
        !launch_pending_update_on_host_exit(true, false, true, true),
        L"a host does not launch a second update helper");
    expect(
        !launch_pending_update_on_host_exit(true, false, false, false),
        L"an abnormal host exit preserves the pending update for recovery");
    expect(
        !launch_pending_update_on_host_exit(true, false, false, true, true),
        L"a system logoff or shutdown preserves the pending update for next startup");
    expect(
        check_pending_update_on_host_startup(true, false),
        L"the owning persistent instance may apply a pending startup update");
    expect(
        !check_pending_update_on_host_startup(false, false),
        L"a second host instance cannot launch the update helper");
    expect(
        !check_pending_update_on_host_startup(true, true),
        L"a transient host never applies pending updates at startup");

    expect(
        cancel_update_when_automatic_is_disabled(true, false),
        L"an in-flight automatic update is cancelled");
    expect(
        !cancel_update_when_automatic_is_disabled(true, true),
        L"a user-triggered update is allowed to finish");
    expect(
        !cancel_update_when_automatic_is_disabled(false, false),
        L"an idle updater requires no cancellation");

    expect(
        automatic_update_runtime_action(false, true, false, false) ==
            AutomaticUpdateRuntimeAction::schedule,
        L"enabling automatic updates schedules the background policy");
    expect(
        automatic_update_runtime_action(true, false, false, false) ==
            AutomaticUpdateRuntimeAction::disable,
        L"disabling an idle updater still removes its timer");
    expect(
        automatic_update_runtime_action(true, false, true, false) ==
            AutomaticUpdateRuntimeAction::disable_and_cancel,
        L"disabling an automatic worker removes the timer and stops the worker");
    expect(
        automatic_update_runtime_action(true, false, true, true) ==
            AutomaticUpdateRuntimeAction::disable,
        L"disabling automatic updates preserves a manual worker");
    expect(
        automatic_update_runtime_action(true, true, true, false) ==
            AutomaticUpdateRuntimeAction::none,
        L"an unchanged preference has no runtime side effect");

    SeamlessUpdateActivationContext seamless{
        .update_ready = true,
        .automatic_updates_enabled = true,
        .seamless_updates_enabled = true,
        .app_idle = true,
        .presentation_interruption_allowed = true,
        .defer_until_unix = 0,
        .now_unix = 2'000'000,
        .staged_elapsed_ms = kUpdateStagedSettleMs,
        .user_idle_ms = 15 * 60 * 1'000,
        .idle_threshold_minutes = 15,
    };
    expect(
        seamless_update_activation_allowed(seamless),
        L"a settled update activates after the configured idle threshold");

    seamless.update_ready = false;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"activation requires a staged update");
    seamless.update_ready = true;
    seamless.automatic_updates_enabled = false;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"activation respects the automatic-update preference");
    seamless.automatic_updates_enabled = true;
    seamless.seamless_updates_enabled = false;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"activation respects the seamless-update preference");
    seamless.seamless_updates_enabled = true;
    seamless.app_idle = false;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"active capture work blocks seamless activation");
    seamless.app_idle = true;
    seamless.presentation_interruption_allowed = false;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"a presentation or full-screen app blocks interruption");
    seamless.presentation_interruption_allowed = true;

    seamless.defer_until_unix = seamless.now_unix + 1;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"a deferral remains active until its exact expiry");
    seamless.now_unix = seamless.defer_until_unix;
    expect(
        seamless_update_activation_allowed(seamless),
        L"activation becomes eligible at the exact deferral expiry");
    seamless.now_unix = 0;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"an invalid or rolled-back clock cannot bypass a future deferral");
    seamless.now_unix = 2'000'000;
    seamless.defer_until_unix = 0;

    seamless.staged_elapsed_ms = kUpdateStagedSettleMs - 1;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"a newly staged payload waits the full two-minute settle period");
    seamless.staged_elapsed_ms = kUpdateStagedSettleMs;
    seamless.user_idle_ms = 15 * 60 * 1'000 - 1;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"user idle time must reach the configured threshold");
    seamless.user_idle_ms = 15 * 60 * 1'000;
    expect(
        seamless_update_activation_allowed(seamless),
        L"the idle-time boundary is inclusive");

    seamless.idle_threshold_minutes = 0;
    seamless.user_idle_ms = 5 * 60 * 1'000 - 1;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"a zero threshold is safely clamped to five minutes");
    seamless.user_idle_ms = 5 * 60 * 1'000;
    expect(
        seamless_update_activation_allowed(seamless),
        L"the minimum clamped threshold activates at five minutes");
    seamless.idle_threshold_minutes = 9'223'372'036'854'775'807LL;
    seamless.user_idle_ms = 119ULL * 60 * 1'000;
    expect(
        !seamless_update_activation_allowed(seamless),
        L"an excessive threshold is clamped without multiplication overflow");
    seamless.user_idle_ms = 120ULL * 60 * 1'000;
    expect(
        seamless_update_activation_allowed(seamless),
        L"the maximum clamped threshold activates at two hours");

    if (failures == 0) {
        std::wcout << L"Host policy tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
