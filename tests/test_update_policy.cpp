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

    if (failures == 0) {
        std::wcout << L"update policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
