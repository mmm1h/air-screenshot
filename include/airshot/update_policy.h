#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace airshot {

inline constexpr std::uint32_t kUpdateInitialDelayMs = 90'000;
inline constexpr std::int64_t kUpdateIntervalSeconds = 24 * 60 * 60;
inline constexpr std::uint32_t kUpdateRetryDelayMs = 6 * 60 * 60 * 1'000;

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
