#pragma once

#include "airshot/config.h"

#include <functional>
#include <optional>

namespace airshot {

using SettingsWindowCompletion = std::function<void(std::optional<AppConfig>)>;

HWND show_settings_window_async(HWND owner, AppConfig config, SettingsWindowCompletion completion);
bool show_settings_window(HWND owner, AppConfig& config);

}  // namespace airshot
