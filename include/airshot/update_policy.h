#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace airshot {

inline constexpr std::uint32_t kUpdateInitialDelayMs = 90'000;
inline constexpr std::int64_t kUpdateIntervalSeconds = 24 * 60 * 60;
inline constexpr std::uint32_t kUpdateRetryInitialDelayMs =
    15 * 60 * 1'000;
inline constexpr std::uint32_t kUpdateRetrySecondDelayMs =
    60 * 60 * 1'000;
inline constexpr std::uint32_t kUpdateRetryMaxDelayMs =
    6 * 60 * 60 * 1'000;
// Retained as the conservative retry delay for callers that have not yet
// started tracking consecutive failures.
inline constexpr std::uint32_t kUpdateRetryDelayMs =
    kUpdateRetryMaxDelayMs;

inline constexpr std::int32_t kUpdateMinIdleMinutes = 5;
inline constexpr std::int32_t kUpdateDefaultIdleMinutes = 15;
inline constexpr std::int32_t kUpdateMaxIdleMinutes = 120;
inline constexpr std::array<std::int32_t, 4> kUpdateIdleMinuteChoices{
    5,
    15,
    30,
    60,
};
inline constexpr std::uint32_t kUpdateStagedSettleMs = 2 * 60 * 1'000;
inline constexpr std::uint32_t kUpdateActivationProbeMs = 30 * 1'000;

[[nodiscard]] constexpr std::int32_t normalized_update_idle_minutes(
    std::int64_t idle_minutes) noexcept {
    if (idle_minutes < kUpdateMinIdleMinutes) {
        return kUpdateMinIdleMinutes;
    }
    if (idle_minutes > kUpdateMaxIdleMinutes) {
        return kUpdateMaxIdleMinutes;
    }
    return static_cast<std::int32_t>(idle_minutes);
}

// `consecutive_failures` is the number of failed attempts including the most
// recent one. Zero is treated like the first failure so an accidental call
// cannot create an immediate retry loop.
[[nodiscard]] constexpr std::uint32_t automatic_update_retry_delay_ms(
    std::uint32_t consecutive_failures) noexcept {
    if (consecutive_failures <= 1) {
        return kUpdateRetryInitialDelayMs;
    }
    if (consecutive_failures == 2) {
        return kUpdateRetrySecondDelayMs;
    }
    return kUpdateRetryMaxDelayMs;
}

[[nodiscard]] std::uint32_t automatic_update_delay_ms(
    std::int64_t last_check_unix,
    std::int64_t now_unix,
    std::uint32_t fallback_delay_ms);

[[nodiscard]] std::wstring update_target_key(
    const std::filesystem::path& target);

[[nodiscard]] bool should_show_update_target_warning(
    bool user_triggered,
    std::wstring_view warned_target,
    std::wstring_view current_target);

}  // namespace airshot
