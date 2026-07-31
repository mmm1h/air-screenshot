#pragma once

#include "airshot/config.h"

#include <functional>
#include <optional>

namespace airshot {

using SettingsWindowCompletion = std::function<void(std::optional<AppConfig>)>;
using SettingsWindowValidator =
    std::function<bool(const AppConfig&, std::wstring*)>;

HWND show_settings_window_async(
    HWND owner,
    AppConfig config,
    SettingsWindowCompletion completion,
    SettingsWindowValidator validator = {});
bool show_settings_window(HWND owner, AppConfig& config);

}  // namespace airshot
