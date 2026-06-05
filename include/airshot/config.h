#pragma once

#include "airshot/common.h"

namespace airshot {

enum class OcrEngine : int {
    system = 0,
    wechat = 1,
    rapid_ocr = 2,
};

inline constexpr int kDefaultOcrEngine = static_cast<int>(OcrEngine::rapid_ocr);
inline constexpr wchar_t kRapidOcrPackageId[] = L"rapidocr-ppocrv5-mobile-v1";
inline constexpr wchar_t kDefaultOcrDependencyManifestUrl[] =
    L"https://mmm1h.github.io/air-screenshot/ocr-dependencies.json";

struct AppConfig {
    int schema_version{1};
    bool annotation_enabled{true};
    bool annotation_locked_tool{true};
    bool ocr_enabled{true};
    bool shell_enabled{true};
    bool start_at_login{true};
    bool global_ocr_enabled{false};
    bool notifications_enabled{false};
    int ocr_engine{kDefaultOcrEngine};
    std::wstring ocr_download_url{kDefaultOcrDependencyManifestUrl};
    std::wstring capture_hotkey{L"Ctrl+Alt+A"};
    std::wstring global_ocr_hotkey{L"Ctrl+Alt+O"};
    std::wstring capture_ocr_shortcut{L"Shift+C"};
    std::wstring default_output{L"clipboard"};
    std::wstring custom_color{L"#8000FF"};
    std::wstring annotation_hidden_tools;
    std::wstring toolbar_order{L"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,text,serial,eraser,undo,redo,ocr,scroll,pin,copy,save,close"};
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
