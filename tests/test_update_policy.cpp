#include "airshot/host_policy.h"
#include "airshot/update_policy.h"

#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAILED: " << message << L'\n';
        ++failures;
    }
}

}  // namespace

int wmain() {
    constexpr std::int64_t now = 2'000'000;
    expect(
        airshot::automatic_update_delay_ms(
            0, now, airshot::kUpdateInitialDelayMs) ==
            airshot::kUpdateInitialDelayMs,
        L"a missing check timestamp uses the startup delay");
    expect(
        airshot::automatic_update_delay_ms(
            now - 23 * 60 * 60, now, airshot::kUpdateInitialDelayMs) ==
            60 * 60 * 1'000,
        L"a recent check waits for the rest of the daily interval");
    expect(
        airshot::automatic_update_delay_ms(
            now - 25 * 60 * 60, now, airshot::kUpdateInitialDelayMs) ==
            airshot::kUpdateInitialDelayMs,
        L"an overdue check uses the requested fallback delay");
    expect(
        airshot::automatic_update_delay_ms(
            now + 60, now, airshot::kUpdateInitialDelayMs) ==
            airshot::kUpdateIntervalSeconds * 1'000,
        L"a future timestamp caused by clock rollback is capped at one day");

    expect(
        airshot::automatic_update_retry_delay_ms(0) ==
            15 * 60 * 1'000,
        L"a missing retry count still avoids an immediate retry loop");
    expect(
        airshot::automatic_update_retry_delay_ms(1) ==
            15 * 60 * 1'000,
        L"the first consecutive failure retries after fifteen minutes");
    expect(
        airshot::automatic_update_retry_delay_ms(2) ==
            60 * 60 * 1'000,
        L"the second consecutive failure retries after one hour");
    expect(
        airshot::automatic_update_retry_delay_ms(3) ==
            6 * 60 * 60 * 1'000,
        L"the third consecutive failure reaches the six-hour cap");
    expect(
        airshot::automatic_update_retry_delay_ms(100) ==
            airshot::kUpdateRetryMaxDelayMs,
        L"later failures remain capped at six hours");

    expect(
        airshot::normalized_update_idle_minutes(0) ==
            airshot::kUpdateMinIdleMinutes,
        L"an invalid zero idle threshold is clamped to five minutes");
    expect(
        airshot::normalized_update_idle_minutes(15) ==
            airshot::kUpdateDefaultIdleMinutes,
        L"the default idle threshold remains fifteen minutes");
    expect(
        airshot::normalized_update_idle_minutes(121) ==
            airshot::kUpdateMaxIdleMinutes,
        L"an excessive idle threshold is clamped to two hours");
    expect(
        airshot::kUpdateIdleMinuteChoices[0] == 5 &&
            airshot::kUpdateIdleMinuteChoices[1] == 15 &&
            airshot::kUpdateIdleMinuteChoices[2] == 30 &&
            airshot::kUpdateIdleMinuteChoices[3] == 60,
        L"the settings policy exposes the intended common idle choices");
    expect(
        airshot::kUpdateStagedSettleMs == 2 * 60 * 1'000 &&
            airshot::kUpdateActivationProbeMs == 30 * 1'000,
        L"seamless activation settles for two minutes and probes every thirty seconds");

    const auto first =
        airshot::update_target_key(LR"(C:/Apps/AirScreenshot.exe)");
    const auto second =
        airshot::update_target_key(LR"(c:\apps\airscreenshot.exe)");
    expect(first == second, L"Windows update target keys ignore slash and case differences");
    expect(
        airshot::should_show_update_target_warning(false, L"", first),
        L"the first automatic target warning is shown");
    expect(
        !airshot::should_show_update_target_warning(false, first, second),
        L"an automatic warning is deduplicated for the same target");
    expect(
        airshot::should_show_update_target_warning(true, first, second),
        L"a manual check always reports an unusable target");

    expect(
        airshot::cancel_update_when_automatic_is_disabled(true, false),
        L"disabling automatic updates cancels an in-flight automatic task");
    expect(
        !airshot::cancel_update_when_automatic_is_disabled(true, true),
        L"disabling automatic updates preserves an in-flight user task");
    expect(
        !airshot::cancel_update_when_automatic_is_disabled(false, false),
        L"disabling automatic updates does not cancel an idle worker");
    expect(
        airshot::automatic_update_runtime_action(false, true, false, false) ==
            airshot::AutomaticUpdateRuntimeAction::schedule,
        L"enabling automatic updates requests scheduling");
    expect(
        airshot::automatic_update_runtime_action(true, false, true, false) ==
            airshot::AutomaticUpdateRuntimeAction::disable_and_cancel,
        L"disabling automatic updates cancels only an automatic worker");
    expect(
        airshot::automatic_update_runtime_action(true, false, true, true) ==
            airshot::AutomaticUpdateRuntimeAction::disable,
        L"disabling automatic updates keeps a manual worker alive");

    if (failures == 0) {
        std::wcout << L"update policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
