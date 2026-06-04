#include "airshot/config.h"

#include <appmodel.h>
#include <shlobj.h>

#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>

namespace airshot {
namespace {

enum class JsonKind {
    null_value,
    object,
    array,
    string,
    number,
    boolean,
};

struct JsonNode {
    JsonKind kind{JsonKind::null_value};
    std::map<std::wstring, JsonNode> object;
    std::vector<JsonNode> array;
    std::wstring string;
    double number{};
    bool boolean{};
};

class JsonParser {
public:
    explicit JsonParser(std::wstring_view text) : text_(text) {}

    std::optional<JsonNode> parse() {
        auto value = parse_value(0);
        skip_whitespace();
        if (!value || position_ != text_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skip_whitespace() {
        while (position_ < text_.size() && std::iswspace(text_[position_])) {
            ++position_;
        }
    }

    bool consume(wchar_t expected) {
        skip_whitespace();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    std::optional<JsonNode> parse_value(int depth) {
        if (depth > 32) {
            return std::nullopt;
        }
        skip_whitespace();
        if (position_ >= text_.size()) {
            return std::nullopt;
        }
        if (text_[position_] == L'{') {
            return parse_object(depth);
        }
        if (text_[position_] == L'[') {
            return parse_array(depth);
        }
        if (text_[position_] == L'"') {
            auto string = parse_string();
            if (!string) {
                return std::nullopt;
            }
            JsonNode node;
            node.kind = JsonKind::string;
            node.string = std::move(*string);
            return node;
        }
        if (text_[position_] == L'-' || std::iswdigit(text_[position_])) {
            return parse_number();
        }
        if (consume_literal(L"true")) {
            JsonNode node;
            node.kind = JsonKind::boolean;
            node.boolean = true;
            return node;
        }
        if (consume_literal(L"false")) {
            JsonNode node;
            node.kind = JsonKind::boolean;
            return node;
        }
        if (consume_literal(L"null")) {
            return JsonNode{};
        }
        return std::nullopt;
    }

    std::optional<JsonNode> parse_object(int depth) {
        if (!consume(L'{')) {
            return std::nullopt;
        }
        JsonNode node;
        node.kind = JsonKind::object;
        skip_whitespace();
        if (consume(L'}')) {
            return node;
        }
        while (true) {
            auto key = parse_string();
            if (!key || !consume(L':')) {
                return std::nullopt;
            }
            auto value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            node.object.insert_or_assign(std::move(*key), std::move(*value));
            skip_whitespace();
            if (consume(L'}')) {
                return node;
            }
            if (!consume(L',')) {
                return std::nullopt;
            }
        }
    }

    std::optional<JsonNode> parse_array(int depth) {
        if (!consume(L'[')) {
            return std::nullopt;
        }
        JsonNode node;
        node.kind = JsonKind::array;
        skip_whitespace();
        if (consume(L']')) {
            return node;
        }
        while (true) {
            auto value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            node.array.push_back(std::move(*value));
            skip_whitespace();
            if (consume(L']')) {
                return node;
            }
            if (!consume(L',')) {
                return std::nullopt;
            }
        }
    }

    std::optional<std::wstring> parse_string() {
        if (!consume(L'"')) {
            return std::nullopt;
        }
        std::wstring result;
        while (position_ < text_.size()) {
            const wchar_t current = text_[position_++];
            if (current == L'"') {
                return result;
            }
            if (current < 0x20) {
                return std::nullopt;
            }
            if (current != L'\\') {
                result.push_back(current);
                continue;
            }
            if (position_ >= text_.size()) {
                return std::nullopt;
            }
            const wchar_t escaped = text_[position_++];
            switch (escaped) {
                case L'"': result.push_back(L'"'); break;
                case L'\\': result.push_back(L'\\'); break;
                case L'/': result.push_back(L'/'); break;
                case L'b': result.push_back(L'\b'); break;
                case L'f': result.push_back(L'\f'); break;
                case L'n': result.push_back(L'\n'); break;
                case L'r': result.push_back(L'\r'); break;
                case L't': result.push_back(L'\t'); break;
                case L'u': {
                    if (position_ + 4 > text_.size()) {
                        return std::nullopt;
                    }
                    unsigned int code = 0;
                    for (int index = 0; index < 4; ++index) {
                        const int digit = hex_digit(text_[position_++]);
                        if (digit < 0) {
                            return std::nullopt;
                        }
                        code = code * 16U + static_cast<unsigned int>(digit);
                    }
                    result.push_back(static_cast<wchar_t>(code));
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonNode> parse_number() {
        const std::size_t start = position_;
        while (position_ < text_.size()) {
            const wchar_t current = text_[position_];
            if (std::iswdigit(current) || current == L'-' || current == L'+' || current == L'.' ||
                current == L'e' || current == L'E') {
                ++position_;
            } else {
                break;
            }
        }
        try {
            const std::wstring token(text_.substr(start, position_ - start));
            std::size_t consumed = 0;
            const double number = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(number)) {
                return std::nullopt;
            }
            JsonNode node;
            node.kind = JsonKind::number;
            node.number = number;
            return node;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool consume_literal(std::wstring_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    static int hex_digit(wchar_t value) {
        if (value >= L'0' && value <= L'9') {
            return value - L'0';
        }
        if (value >= L'a' && value <= L'f') {
            return value - L'a' + 10;
        }
        if (value >= L'A' && value <= L'F') {
            return value - L'A' + 10;
        }
        return -1;
    }

    std::wstring_view text_;
    std::size_t position_{};
};

std::wstring upper(std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) { return static_cast<wchar_t>(::towupper(ch)); });
    return result;
}

std::vector<std::wstring> split(std::wstring_view value, wchar_t delimiter) {
    std::vector<std::wstring> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        result.emplace_back(value.substr(start, end == std::wstring_view::npos ? value.size() - start : end - start));
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

const JsonNode* member(const JsonNode& object, std::wstring_view name) {
    if (object.kind != JsonKind::object) {
        return nullptr;
    }
    const auto found = object.object.find(std::wstring(name));
    return found == object.object.end() ? nullptr : &found->second;
}

bool named_boolean(const JsonNode& object, std::wstring_view name, bool fallback) {
    const auto* value = member(object, name);
    return value && value->kind == JsonKind::boolean ? value->boolean : fallback;
}

std::wstring named_string(const JsonNode& object, std::wstring_view name, std::wstring_view fallback) {
    const auto* value = member(object, name);
    return value && value->kind == JsonKind::string ? value->string : std::wstring(fallback);
}

int named_integer(const JsonNode& object, std::wstring_view name, int fallback) {
    const auto* value = member(object, name);
    return value && value->kind == JsonKind::number ? static_cast<int>(value->number) : fallback;
}

std::wstring quote_json(std::wstring_view value) {
    std::wstring result{L"\""};
    for (const wchar_t current : value) {
        switch (current) {
            case L'"': result += L"\\\""; break;
            case L'\\': result += L"\\\\"; break;
            case L'\b': result += L"\\b"; break;
            case L'\f': result += L"\\f"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default:
                if (current < 0x20) {
                    result += std::format(L"\\u{:04X}", static_cast<unsigned int>(current));
                } else {
                    result.push_back(current);
                }
        }
    }
    result.push_back(L'"');
    return result;
}

std::wstring_view json_boolean(bool value) {
    return value ? L"true" : L"false";
}

std::optional<std::wstring> current_package_family() {
    UINT32 length = 0;
    if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return std::nullopt;
    }
    std::wstring family(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, family.data()) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    family.resize(wcslen(family.c_str()));
    return family;
}

}  // namespace

std::optional<Hotkey> parse_hotkey(std::wstring_view value) {
    Hotkey hotkey{MOD_NOREPEAT, 0};
    for (const auto& raw_part : split(value, L'+')) {
        const std::wstring part = upper(raw_part);
        if (part == L"CTRL" || part == L"CONTROL") {
            hotkey.modifiers |= MOD_CONTROL;
        } else if (part == L"ALT") {
            hotkey.modifiers |= MOD_ALT;
        } else if (part == L"SHIFT") {
            hotkey.modifiers |= MOD_SHIFT;
        } else if (part == L"WIN" || part == L"WINDOWS") {
            hotkey.modifiers |= MOD_WIN;
        } else if (part == L"PRINTSCREEN" || part == L"PRTSC") {
            hotkey.virtual_key = VK_SNAPSHOT;
        } else if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') ||
                                      (part[0] >= L'0' && part[0] <= L'9'))) {
            hotkey.virtual_key = static_cast<UINT>(part[0]);
        } else if (part.size() >= 2 && part[0] == L'F') {
            try {
                const int function_key = std::stoi(part.substr(1));
                if (function_key < 1 || function_key > 24) {
                    return std::nullopt;
                }
                hotkey.virtual_key = static_cast<UINT>(VK_F1 + function_key - 1);
            } catch (...) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if (hotkey.virtual_key == 0) {
        return std::nullopt;
    }
    return hotkey;
}

std::wstring config_to_json(const AppConfig& config) {
    std::wstring result;
    result.reserve(384);
    result += L"{\"schemaVersion\":" + std::to_wstring(config.schema_version);
    result += L",\"annotation\":{\"enabled\":" + std::wstring(json_boolean(config.annotation_enabled)) + L"}";
    result += L",\"ocr\":{\"enabled\":" + std::wstring(json_boolean(config.ocr_enabled)) + L"}";
    result += L",\"shell\":{\"enabled\":" + std::wstring(json_boolean(config.shell_enabled));
    result += L",\"startAtLogin\":" + std::wstring(json_boolean(config.start_at_login)) + L"}";
    result += L",\"hotkey\":{\"capture\":" + quote_json(config.capture_hotkey);
    result += L",\"globalOcrEnabled\":" + std::wstring(json_boolean(config.global_ocr_enabled));
    result += L",\"globalOcr\":" + quote_json(config.global_ocr_hotkey) + L"}";
    result += L",\"shortcut\":{\"captureOcr\":" + quote_json(config.capture_ocr_shortcut) + L"}";
    result += L",\"capture\":{\"defaultOutput\":" + quote_json(config.default_output);
    result += L",\"customColor\":" + quote_json(config.custom_color) + L"}}";
    return result;
}

std::optional<AppConfig> config_from_json(std::wstring_view json_text) {
    const auto root = JsonParser(json_text).parse();
    if (!root || root->kind != JsonKind::object) {
        return std::nullopt;
    }

    AppConfig config;
    config.schema_version = named_integer(*root, L"schemaVersion", 1);
    if (const auto* annotation = member(*root, L"annotation")) {
        config.annotation_enabled = named_boolean(*annotation, L"enabled", true);
    }
    if (const auto* ocr = member(*root, L"ocr")) {
        config.ocr_enabled = named_boolean(*ocr, L"enabled", true);
    }
    if (const auto* shell = member(*root, L"shell")) {
        config.shell_enabled = named_boolean(*shell, L"enabled", true);
        config.start_at_login = named_boolean(*shell, L"startAtLogin", true);
    }
    if (const auto* hotkey = member(*root, L"hotkey")) {
        config.capture_hotkey = named_string(*hotkey, L"capture", L"Ctrl+Alt+A");
        config.global_ocr_enabled = named_boolean(*hotkey, L"globalOcrEnabled", false);
        config.global_ocr_hotkey = named_string(*hotkey, L"globalOcr", L"Ctrl+Alt+O");
    }
    if (const auto* shortcut = member(*root, L"shortcut")) {
        config.capture_ocr_shortcut = named_string(*shortcut, L"captureOcr", L"Shift+C");
    }
    if (const auto* capture = member(*root, L"capture")) {
        config.default_output = named_string(*capture, L"defaultOutput", L"clipboard");
        config.custom_color = named_string(*capture, L"customColor", L"#8000FF");
    }
    return config;
}

std::filesystem::path config_directory() {
    try {
        PWSTR value = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value)) && value) {
            std::filesystem::path result(value);
            CoTaskMemFree(value);
            if (const auto family = current_package_family()) {
                return result / L"Packages" / *family / L"LocalState";
            }
            return result / L"AirScreenshot";
        }
        return std::filesystem::temp_directory_path() / L"AirScreenshot";
    } catch (...) {
        return std::filesystem::path(L".") / L"AirScreenshot";
    }
}

std::filesystem::path config_path() {
    return config_directory() / L"config.json";
}

AppConfig load_config() {
    const auto path = config_path();
    if (!std::filesystem::exists(path)) {
        AppConfig config;
        save_config(config);
        return config;
    }
    try {
        std::ifstream stream(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto parsed = config_from_json(from_utf8(bytes));
        if (parsed) {
            return *parsed;
        }
        const auto corrupt = path.parent_path() / std::format(L"config.corrupt-{}.json", timestamp_for_file());
        std::filesystem::rename(path, corrupt);
    } catch (...) {
    }
    AppConfig config;
    save_config(config);
    return config;
}

bool save_config(const AppConfig& config, std::wstring* error) {
    try {
        std::filesystem::create_directories(config_directory());
        const auto path = config_path();
        const auto temporary = path.parent_path() / L"config.tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const std::string bytes = to_utf8(config_to_json(config));
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(to_utf8(windows_error_message(GetLastError())));
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = from_utf8(exception.what());
        }
        return false;
    }
}

}  // namespace airshot
