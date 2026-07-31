#pragma once

namespace airshot::settings_detail {

[[nodiscard]] constexpr bool settings_use_terminal_ocr_status(
    bool has_terminal_status,
    bool terminal_ready,
    bool terminal_cancelled) noexcept {
    return has_terminal_status && !terminal_ready && !terminal_cancelled;
}

[[nodiscard]] constexpr bool settings_theme_resources_need_refresh(
    bool current_light,
    bool current_high_contrast,
    bool next_light,
    bool next_high_contrast,
    bool resources_available,
    bool system_colors_changed) noexcept {
    return system_colors_changed || !resources_available ||
           current_light != next_light ||
           current_high_contrast != next_high_contrast;
}

}  // namespace airshot::settings_detail
