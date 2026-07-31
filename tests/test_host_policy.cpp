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
        launch_pending_update_on_host_exit(true, false, false),
        L"an initialized persistent host checks pending updates on exit");
    expect(
        !launch_pending_update_on_host_exit(false, false, false),
        L"a host that failed initialization cannot apply pending updates");
    expect(
        !launch_pending_update_on_host_exit(true, true, false),
        L"a transient host never applies pending updates on exit");
    expect(
        !launch_pending_update_on_host_exit(true, false, true),
        L"a host does not launch a second update helper");
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

    if (failures == 0) {
        std::wcout << L"Host policy tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
