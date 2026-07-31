#include "airshot/update_policy.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace airshot {

std::uint32_t automatic_update_delay_ms(
    std::int64_t last_check_unix,
    std::int64_t now_unix,
    std::uint32_t fallback_delay_ms) {
    const std::uint32_t fallback = std::max<std::uint32_t>(1'000, fallback_delay_ms);
    if (last_check_unix <= 0 || now_unix <= 0) {
        return fallback;
    }

    if (last_check_unix > now_unix) {
        return static_cast<std::uint32_t>(kUpdateIntervalSeconds * 1'000);
    }

    const std::int64_t elapsed = now_unix - last_check_unix;
    if (elapsed >= kUpdateIntervalSeconds) {
        return fallback;
    }

    const std::int64_t remaining_ms =
        (kUpdateIntervalSeconds - elapsed) * 1'000;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        remaining_ms,
        1'000,
        std::numeric_limits<std::uint32_t>::max()));
}

std::wstring update_target_key(const std::filesystem::path& target) {
    std::wstring result = target.lexically_normal().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        if (character == L'/') {
            return L'\\';
        }
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

bool should_show_update_target_warning(
    bool user_triggered,
    std::wstring_view warned_target,
    std::wstring_view current_target) {
    return user_triggered || warned_target.empty() ||
           warned_target != current_target;
}

}  // namespace airshot
