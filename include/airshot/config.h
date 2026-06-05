#pragma once

#include "airshot/common.h"

namespace airshot {

struct AppConfig {
    int schema_version{1};
    bool annotation_enabled{true};
    bool annotation_locked_tool{true};
    bool ocr_enabled{true};
    bool shell_enabled{true};
    bool start_at_login{true};
    bool global_ocr_enabled{false};
    bool notifications_enabled{false};
    std::wstring capture_hotkey{L"Ctrl+Alt+A"};
    std::wstring global_ocr_hotkey{L"Ctrl+Alt+O"};
    std::wstring capture_ocr_shortcut{L"Shift+C"};
    std::wstring default_output{L"clipboard"};
    std::wstring custom_color{L"#8000FF"};
    std::wstring annotation_hidden_tools;
    int annotation_highlight_alpha{96};
    int annotation_next_serial{1};
};

struct Hotkey {
    UINT modifiers{};
    UINT virtual_key{};
};

[[nodiscard]] std::optional<Hotkey> parse_hotkey(std::wstring_view value);
[[nodiscard]] std::wstring normalize_annotation_hidden_tools(std::wstring_view value);
[[nodiscard]] bool annotation_tool_hidden(std::wstring_view hidden_tools, std::wstring_view tool_id);
[[nodiscard]] std::wstring config_to_json(const AppConfig& config);
[[nodiscard]] std::optional<AppConfig> config_from_json(std::wstring_view json);
[[nodiscard]] std::filesystem::path config_directory();
[[nodiscard]] std::filesystem::path config_path();
[[nodiscard]] AppConfig load_config();
bool save_config(const AppConfig& config, std::wstring* error = nullptr);

}  // namespace airshot
