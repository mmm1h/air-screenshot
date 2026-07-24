#pragma once

#include "airshot/common.h"

namespace airshot {

inline constexpr std::wstring_view kOcrEngineRapidV5Fast = L"rapidocr-v5-fast";
inline constexpr std::wstring_view kOcrEngineRapidV5Accurate = L"rapidocr-v5-accurate";
inline constexpr std::wstring_view kOcrEngineRapidV4Compat = L"rapidocr-v4-compat";
inline constexpr std::wstring_view kDefaultOcrEngine = kOcrEngineRapidV5Fast;
inline constexpr wchar_t kRapidOcrOnnxPackageId[] = L"rapidocr-onnx";
inline constexpr wchar_t kDefaultOcrDependencyManifestUrl[] =
    L"https://mmm1h.github.io/air-screenshot/ocr-dependencies.json";
inline constexpr int kCurrentConfigSchemaVersion = 2;

struct AppConfig {
    int schema_version{kCurrentConfigSchemaVersion};
    bool annotation_enabled{true};
    bool annotation_locked_tool{true};
    bool ocr_enabled{true};
    bool shell_enabled{true};
    bool start_at_login{false};
    bool global_ocr_enabled{false};
    bool notifications_enabled{false};
    std::wstring ocr_engine{std::wstring(kDefaultOcrEngine)};
    std::wstring ocr_download_url{kDefaultOcrDependencyManifestUrl};
    std::wstring capture_hotkey{L"Ctrl+Alt+A"};
    std::wstring global_ocr_hotkey{L"Ctrl+Alt+O"};
    std::wstring capture_ocr_shortcut{L"Shift+C"};
    std::wstring default_output{L"clipboard"};
    std::wstring custom_color{L"#8000FF"};
    std::wstring annotation_hidden_tools;
    std::wstring theme{L"system"};
    std::wstring toolbar_order{L"rect,ellipse,line,arrow,pen,text,serial,mosaic,highlight,watermark,pin,ocr,select,scroll,eraser,undo,redo,save,close,copy"};
    std::wstring text_font_family{L"Microsoft YaHei"};
    bool text_font_bold{false};
    bool text_font_italic{false};
    int annotation_highlight_alpha{96};
    int annotation_next_serial{1};
    std::wstring tool_shortcut_select{L"S"};
    std::wstring tool_shortcut_rectangle{L"R"};
    std::wstring tool_shortcut_ellipse{L"E"};
    std::wstring tool_shortcut_line{L"L"};
    std::wstring tool_shortcut_arrow{L"A"};
    std::wstring tool_shortcut_pen{L"P"};
    std::wstring tool_shortcut_mosaic{L"M"};
    std::wstring tool_shortcut_blur{L"B"};
    std::wstring tool_shortcut_highlight{L"H"};
    std::wstring tool_shortcut_text{L"T"};
    std::wstring tool_shortcut_serial{L"N"};
    std::wstring tool_shortcut_eraser{L"D"};

    // Persistence metadata. These fields are not emitted as configuration keys.
    std::wstring preserved_json;
    bool write_protected{};
};

struct Hotkey {
    UINT modifiers{};
    UINT virtual_key{};
};

[[nodiscard]] bool is_windows_system_light_theme();
[[nodiscard]] bool should_use_light_theme(std::wstring_view theme_config);
[[nodiscard]] std::optional<Hotkey> parse_hotkey(std::wstring_view value);
[[nodiscard]] bool validate_global_hotkeys(
    const AppConfig& config,
    std::wstring* error = nullptr);
[[nodiscard]] std::wstring normalize_ocr_engine(std::wstring_view value);
[[nodiscard]] std::wstring normalize_annotation_hidden_tools(std::wstring_view value);
[[nodiscard]] bool annotation_tool_hidden(std::wstring_view hidden_tools, std::wstring_view tool_id);
[[nodiscard]] std::wstring config_to_json(const AppConfig& config);
[[nodiscard]] std::optional<AppConfig> config_from_json(
    std::wstring_view json,
    std::wstring* error = nullptr);
[[nodiscard]] std::filesystem::path config_directory();

class ConfigStore {
public:
    ConfigStore();
    explicit ConfigStore(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] std::filesystem::path path() const;
    [[nodiscard]] std::filesystem::path legacy_path() const;
    [[nodiscard]] std::optional<AppConfig> load(std::wstring* error = nullptr) const;
    bool save(const AppConfig& config, std::wstring* error = nullptr) const;

private:
    std::filesystem::path directory_;
};

[[nodiscard]] AppConfig load_config();

}  // namespace airshot
