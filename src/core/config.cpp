#include "airshot/config.h"

#include <appmodel.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

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
    std::wstring number_lexeme;
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
        while (position_ < text_.size() &&
               (text_[position_] == L' ' || text_[position_] == L'\t' ||
                text_[position_] == L'\r' || text_[position_] == L'\n')) {
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
        if (text_[position_] == L'-' ||
            (text_[position_] >= L'0' && text_[position_] <= L'9')) {
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
            if (node.object.contains(*key)) {
                return std::nullopt;
            }
            node.object.emplace(std::move(*key), std::move(*value));
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
                if (is_high_surrogate(current)) {
                    if (position_ >= text_.size() || !is_low_surrogate(text_[position_])) {
                        return std::nullopt;
                    }
                    result.push_back(current);
                    result.push_back(text_[position_++]);
                    continue;
                }
                if (is_low_surrogate(current)) {
                    return std::nullopt;
                }
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
                    const auto code = parse_hex_code_unit();
                    if (!code) {
                        return std::nullopt;
                    }
                    if (is_high_surrogate(*code)) {
                        if (position_ + 2 > text_.size() || text_[position_] != L'\\' ||
                            text_[position_ + 1] != L'u') {
                            return std::nullopt;
                        }
                        position_ += 2;
                        const auto low = parse_hex_code_unit();
                        if (!low || !is_low_surrogate(*low)) {
                            return std::nullopt;
                        }
                        result.push_back(*code);
                        result.push_back(*low);
                    } else {
                        if (is_low_surrogate(*code)) {
                            return std::nullopt;
                        }
                        result.push_back(*code);
                    }
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonNode> parse_number() {
        const std::size_t start = position_;
        if (text_[position_] == L'-') {
            ++position_;
        }
        if (position_ >= text_.size()) {
            return std::nullopt;
        }
        if (text_[position_] == L'0') {
            ++position_;
            if (position_ < text_.size() &&
                text_[position_] >= L'0' && text_[position_] <= L'9') {
                return std::nullopt;
            }
        } else if (text_[position_] >= L'1' && text_[position_] <= L'9') {
            while (position_ < text_.size() &&
                   text_[position_] >= L'0' && text_[position_] <= L'9') {
                ++position_;
            }
        } else {
            return std::nullopt;
        }
        if (position_ < text_.size() && text_[position_] == L'.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= L'0' && text_[position_] <= L'9') {
                ++position_;
            }
            if (position_ == fraction_start) {
                return std::nullopt;
            }
        }
        if (position_ < text_.size() && (text_[position_] == L'e' || text_[position_] == L'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == L'+' || text_[position_] == L'-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= L'0' && text_[position_] <= L'9') {
                ++position_;
            }
            if (position_ == exponent_start) {
                return std::nullopt;
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
            node.number_lexeme = token;
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

    std::optional<wchar_t> parse_hex_code_unit() {
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
        return static_cast<wchar_t>(code);
    }

    static bool is_high_surrogate(wchar_t value) noexcept {
        return value >= 0xD800 && value <= 0xDBFF;
    }

    static bool is_low_surrogate(wchar_t value) noexcept {
        return value >= 0xDC00 && value <= 0xDFFF;
    }

    std::wstring_view text_;
    std::size_t position_{};
};

std::wstring upper(std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) { return static_cast<wchar_t>(::towupper(ch)); });
    return result;
}

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
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
    if (!value || value->kind != JsonKind::number || std::trunc(value->number) != value->number ||
        value->number < static_cast<double>(std::numeric_limits<int>::min()) ||
        value->number > static_cast<double>(std::numeric_limits<int>::max())) {
        return fallback;
    }
    return static_cast<int>(value->number);
}

int named_clamped_integer(const JsonNode& object, std::wstring_view name, int fallback, int minimum, int maximum) {
    return std::clamp(named_integer(object, name, fallback), minimum, maximum);
}

std::optional<int> schema_version(const JsonNode& object) {
    const auto* value = member(object, L"schemaVersion");
    if (!value) {
        return 1;
    }
    if (value->kind != JsonKind::number || value->number_lexeme.empty() ||
        !std::ranges::all_of(value->number_lexeme, [](wchar_t character) {
            return character >= L'0' && character <= L'9';
        }) ||
        value->number_lexeme == L"0") {
        return std::nullopt;
    }

    int parsed = 0;
    for (const wchar_t character : value->number_lexeme) {
        const int digit = character - L'0';
        if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
            return std::numeric_limits<int>::max();
        }
        parsed = parsed * 10 + digit;
    }
    return parsed;
}

void set_config_error(std::wstring* error, std::wstring message) {
    if (error) {
        *error = std::move(message);
    }
}

bool validate_optional_kind(
    const JsonNode& object,
    std::wstring_view section_name,
    std::wstring_view field_name,
    JsonKind expected,
    std::wstring* error) {
    const auto* value = member(object, field_name);
    if (!value) {
        return true;
    }
    if (value->kind == expected) {
        return true;
    }
    set_config_error(
        error,
        std::format(L"配置字段 {}.{} 的类型无效。", section_name, field_name));
    return false;
}

bool is_integer_node(const JsonNode& node) {
    if (node.kind != JsonKind::number || node.number_lexeme.empty()) {
        return false;
    }
    std::size_t index = node.number_lexeme.front() == L'-' ? 1 : 0;
    if (index == node.number_lexeme.size()) {
        return false;
    }
    return std::ranges::all_of(
        node.number_lexeme.substr(index),
        [](wchar_t character) { return character >= L'0' && character <= L'9'; });
}

bool validate_optional_integer(
    const JsonNode& object,
    std::wstring_view section_name,
    std::wstring_view field_name,
    std::wstring* error) {
    const auto* value = member(object, field_name);
    if (!value || is_integer_node(*value)) {
        return true;
    }
    set_config_error(
        error,
        std::format(L"配置字段 {}.{} 必须是整数。", section_name, field_name));
    return false;
}

const JsonNode* validate_optional_section(
    const JsonNode& root,
    std::wstring_view name,
    std::wstring* error,
    bool& valid) {
    const auto* section = member(root, name);
    if (!section) {
        return nullptr;
    }
    if (section->kind == JsonKind::object) {
        return section;
    }
    set_config_error(error, std::format(L"配置字段 {} 必须是对象。", name));
    valid = false;
    return nullptr;
}

bool is_supported_ocr_engine(std::wstring_view value) {
    const std::wstring normalized = lower(value);
    return normalized == kOcrEngineRapidV5Fast ||
           normalized == kOcrEngineRapidV5Accurate ||
           normalized == kOcrEngineRapidV4Compat ||
           normalized == L"rapid_ocr" ||
           normalized == L"rapidocr" ||
           normalized == L"rapidocr-v5" ||
           normalized == L"wechat" ||
           normalized == L"system";
}

bool validate_current_config(const JsonNode& root, std::wstring* error) {
    bool valid = true;
    const auto* annotation = validate_optional_section(root, L"annotation", error, valid);
    if (!valid) {
        return false;
    }
    if (annotation) {
        if (!validate_optional_kind(*annotation, L"annotation", L"enabled", JsonKind::boolean, error) ||
            !validate_optional_kind(*annotation, L"annotation", L"lockedTool", JsonKind::boolean, error) ||
            !validate_optional_kind(*annotation, L"annotation", L"toolbarOrder", JsonKind::string, error) ||
            !validate_optional_kind(*annotation, L"annotation", L"textFontFamily", JsonKind::string, error) ||
            !validate_optional_kind(*annotation, L"annotation", L"textFontBold", JsonKind::boolean, error) ||
            !validate_optional_kind(*annotation, L"annotation", L"textFontItalic", JsonKind::boolean, error) ||
            !validate_optional_integer(*annotation, L"annotation", L"highlightAlpha", error) ||
            !validate_optional_integer(*annotation, L"annotation", L"nextSerial", error)) {
            return false;
        }
        if (const auto* hidden_tools = member(*annotation, L"hiddenTools")) {
            if (hidden_tools->kind != JsonKind::string &&
                hidden_tools->kind != JsonKind::array) {
                set_config_error(error, L"配置字段 annotation.hiddenTools 必须是字符串或字符串数组。");
                return false;
            }
            if (hidden_tools->kind == JsonKind::array &&
                !std::ranges::all_of(hidden_tools->array, [](const JsonNode& item) {
                    return item.kind == JsonKind::string;
                })) {
                set_config_error(error, L"配置字段 annotation.hiddenTools 必须是字符串数组。");
                return false;
            }
        }
    }

    const auto* ocr = validate_optional_section(root, L"ocr", error, valid);
    if (!valid) {
        return false;
    }
    if (ocr) {
        if (!validate_optional_kind(*ocr, L"ocr", L"enabled", JsonKind::boolean, error) ||
            !validate_optional_kind(*ocr, L"ocr", L"engine", JsonKind::string, error) ||
            !validate_optional_kind(*ocr, L"ocr", L"downloadUrl", JsonKind::string, error)) {
            return false;
        }
        if (const auto* engine = member(*ocr, L"engine");
            engine && !is_supported_ocr_engine(engine->string)) {
            set_config_error(error, L"配置字段 ocr.engine 不是受支持的枚举值。");
            return false;
        }
    }

    const auto* shell = validate_optional_section(root, L"shell", error, valid);
    if (!valid) {
        return false;
    }
    if (shell &&
        (!validate_optional_kind(*shell, L"shell", L"enabled", JsonKind::boolean, error) ||
         !validate_optional_kind(*shell, L"shell", L"startAtLogin", JsonKind::boolean, error) ||
         !validate_optional_kind(
             *shell, L"shell", L"notificationsEnabled", JsonKind::boolean, error))) {
        return false;
    }

    const auto* hotkey = validate_optional_section(root, L"hotkey", error, valid);
    if (!valid) {
        return false;
    }
    if (hotkey &&
        (!validate_optional_kind(*hotkey, L"hotkey", L"capture", JsonKind::string, error) ||
         !validate_optional_kind(
             *hotkey, L"hotkey", L"globalOcrEnabled", JsonKind::boolean, error) ||
         !validate_optional_kind(*hotkey, L"hotkey", L"globalOcr", JsonKind::string, error))) {
        return false;
    }

    const auto* shortcut = validate_optional_section(root, L"shortcut", error, valid);
    if (!valid) {
        return false;
    }
    if (shortcut) {
        constexpr std::array<std::wstring_view, 13> shortcut_fields{
            L"captureOcr",
            L"toolSelect",
            L"toolRectangle",
            L"toolEllipse",
            L"toolLine",
            L"toolArrow",
            L"toolPen",
            L"toolMosaic",
            L"toolBlur",
            L"toolHighlight",
            L"toolText",
            L"toolSerial",
            L"toolEraser",
        };
        for (const auto field : shortcut_fields) {
            if (!validate_optional_kind(*shortcut, L"shortcut", field, JsonKind::string, error)) {
                return false;
            }
        }
    }

    const auto* capture = validate_optional_section(root, L"capture", error, valid);
    if (!valid) {
        return false;
    }
    if (capture) {
        if (!validate_optional_kind(
                *capture, L"capture", L"defaultOutput", JsonKind::string, error) ||
            !validate_optional_kind(*capture, L"capture", L"customColor", JsonKind::string, error) ||
            !validate_optional_kind(*capture, L"capture", L"theme", JsonKind::string, error)) {
            return false;
        }
        if (const auto* output = member(*capture, L"defaultOutput");
            output && output->string != L"clipboard" && output->string != L"file") {
            set_config_error(error, L"配置字段 capture.defaultOutput 不是受支持的枚举值。");
            return false;
        }
        if (const auto* theme = member(*capture, L"theme");
            theme && theme->string != L"system" && theme->string != L"light" &&
            theme->string != L"dark") {
            set_config_error(error, L"配置字段 capture.theme 不是受支持的枚举值。");
            return false;
        }
    }
    return true;
}

std::wstring named_ocr_engine(const JsonNode& object, std::wstring_view name) {
    const auto* value = member(object, name);
    if (!value) {
        return std::wstring(kDefaultOcrEngine);
    }
    if (value->kind == JsonKind::string) {
        return normalize_ocr_engine(value->string);
    }
    if (value->kind == JsonKind::number) {
        return std::wstring(kDefaultOcrEngine);
    }
    return std::wstring(kDefaultOcrEngine);
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

constexpr std::array<std::wstring_view, 22> kAnnotationToolbarTools{
    L"lock",
    L"select",
    L"rect",
    L"ellipse",
    L"line",
    L"arrow",
    L"pen",
    L"mosaic",
    L"blur",
    L"highlight",
    L"watermark",
    L"text",
    L"serial",
    L"eraser",
    L"undo",
    L"redo",
    L"ocr",
    L"scroll",
    L"pin",
    L"copy",
    L"save",
    L"close",
};

bool is_hidden_tool_delimiter(wchar_t value) {
    return value == L',' || value == L';' || value == L'|' || std::iswspace(value);
}

std::vector<std::wstring> split_hidden_tools(std::wstring_view value) {
    std::vector<std::wstring> result;
    std::wstring current;
    for (const wchar_t ch : value) {
        if (is_hidden_tool_delimiter(ch)) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        result.push_back(std::move(current));
    }
    return result;
}

bool tool_id_matches(std::wstring_view left, std::wstring_view right) {
    return lower(left) == lower(right);
}

std::wstring hidden_tools_to_json_array(std::wstring_view hidden_tools) {
    const std::wstring normalized = normalize_annotation_hidden_tools(hidden_tools);
    std::wstring result{L"["};
    bool first = true;
    for (const auto& tool : split_hidden_tools(normalized)) {
        if (!first) {
            result += L",";
        }
        first = false;
        result += quote_json(tool);
    }
    result += L"]";
    return result;
}

std::wstring hidden_tools_from_json_node(const JsonNode& node, std::wstring_view fallback) {
    if (node.kind == JsonKind::string) {
        return normalize_annotation_hidden_tools(node.string);
    }
    if (node.kind != JsonKind::array) {
        return normalize_annotation_hidden_tools(fallback);
    }
    std::wstring joined;
    for (const auto& item : node.array) {
        if (item.kind != JsonKind::string) {
            continue;
        }
        if (!joined.empty()) {
            joined += L",";
        }
        joined += item.string;
    }
    return normalize_annotation_hidden_tools(joined);
}

std::wstring serialize_json(const JsonNode& node) {
    switch (node.kind) {
        case JsonKind::null_value: return L"null";
        case JsonKind::string: return quote_json(node.string);
        case JsonKind::number:
            if (!node.number_lexeme.empty()) {
                return node.number_lexeme;
            }
            if (std::trunc(node.number) == node.number) {
                return std::format(L"{:.0f}", node.number);
            }
            return std::format(L"{:.17g}", node.number);
        case JsonKind::boolean: return std::wstring(json_boolean(node.boolean));
        case JsonKind::array: {
            std::wstring result{L"["};
            bool first = true;
            for (const auto& item : node.array) {
                if (!first) {
                    result += L",";
                }
                first = false;
                result += serialize_json(item);
            }
            result += L"]";
            return result;
        }
        case JsonKind::object: {
            std::wstring result{L"{"};
            bool first = true;
            for (const auto& [name, value] : node.object) {
                if (!first) {
                    result += L",";
                }
                first = false;
                result += quote_json(name);
                result += L":";
                result += serialize_json(value);
            }
            result += L"}";
            return result;
        }
    }
    return L"null";
}

void merge_json(JsonNode& destination, const JsonNode& source) {
    if (destination.kind != JsonKind::object || source.kind != JsonKind::object) {
        destination = source;
        return;
    }
    for (const auto& [name, source_value] : source.object) {
        const auto existing = destination.object.find(name);
        if (existing == destination.object.end()) {
            destination.object.emplace(name, source_value);
        } else {
            merge_json(existing->second, source_value);
        }
    }
}

enum class FileReadStatus {
    success,
    missing,
    read_error,
    invalid_utf8,
};

struct FileReadResult {
    FileReadStatus status{FileReadStatus::read_error};
    std::wstring text;
    std::wstring error;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_;
};

std::wstring config_io_error(
    std::wstring_view operation,
    const std::filesystem::path& path,
    DWORD error) {
    return std::format(
        L"{}“{}”失败：{}",
        operation,
        path.wstring(),
        windows_error_message(error));
}

FileReadResult read_utf8_file(const std::filesystem::path& path) {
    const HANDLE raw_handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {FileReadStatus::missing};
        }
        return {
            FileReadStatus::read_error,
            {},
            config_io_error(L"读取配置文件", path, error),
        };
    }
    const ScopedHandle handle(raw_handle);

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) {
        return {
            FileReadStatus::read_error,
            {},
            config_io_error(L"读取配置文件大小", path, GetLastError()),
        };
    }
    constexpr LONGLONG kMaximumConfigBytes = 16LL * 1024LL * 1024LL;
    if (size.QuadPart < 0 || size.QuadPart > kMaximumConfigBytes) {
        return {
            FileReadStatus::read_error,
            {},
            std::format(L"配置文件“{}”超过 16 MiB 限制。", path.wstring()),
        };
    }

    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(handle.get(), bytes.data() + offset, requested, &read, nullptr)) {
            return {
                FileReadStatus::read_error,
                {},
                config_io_error(L"读取配置文件", path, GetLastError()),
            };
        }
        if (read == 0) {
            return {
                FileReadStatus::read_error,
                {},
                std::format(L"配置文件“{}”在读取过程中被截断。", path.wstring()),
            };
        }
        offset += read;
    }

    std::string_view encoded(bytes);
    if (encoded.starts_with("\xEF\xBB\xBF")) {
        encoded.remove_prefix(3);
    }
    if (encoded.empty()) {
        return {FileReadStatus::success, L""};
    }
    const int character_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        encoded.data(),
        static_cast<int>(encoded.size()),
        nullptr,
        0);
    if (character_count <= 0) {
        return {
            FileReadStatus::invalid_utf8,
            {},
            std::format(L"配置文件“{}”不是有效的 UTF-8。", path.wstring()),
        };
    }
    std::wstring text(static_cast<std::size_t>(character_count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            encoded.data(),
            static_cast<int>(encoded.size()),
            text.data(),
            character_count) != character_count) {
        return {
            FileReadStatus::invalid_utf8,
            {},
            std::format(L"配置文件“{}”不是有效的 UTF-8。", path.wstring()),
        };
    }
    return {FileReadStatus::success, std::move(text)};
}

class ConfigDirectoryLock {
public:
    bool acquire(const std::filesystem::path& directory, std::wstring* error) {
        try {
            std::filesystem::create_directories(directory);
        } catch (const std::exception& exception) {
            set_config_error(
                error,
                std::format(
                    L"无法创建配置目录“{}”：{}",
                    directory.wstring(),
                    from_utf8(exception.what())));
            return false;
        }

        const std::filesystem::path lock_path = directory / L"config.v2.lock";
        const ULONGLONG deadline = GetTickCount64() + 10000;
        for (;;) {
            handle_ = CreateFileW(
                lock_path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }
            const DWORD lock_error = GetLastError();
            if ((lock_error != ERROR_SHARING_VIOLATION &&
                 lock_error != ERROR_LOCK_VIOLATION) ||
                GetTickCount64() >= deadline) {
                set_config_error(
                    error,
                    config_io_error(L"获取配置锁", lock_path, lock_error));
                return false;
            }
            Sleep(10);
        }
    }

    ~ConfigDirectoryLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ConfigDirectoryLock(const ConfigDirectoryLock&) = delete;
    ConfigDirectoryLock& operator=(const ConfigDirectoryLock&) = delete;
    ConfigDirectoryLock() = default;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

bool quarantine_corrupt_config(
    const std::filesystem::path& source,
    std::filesystem::path& quarantine,
    std::wstring* error) {
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        quarantine = source.parent_path() /
                     std::format(
                         L"config.v2.corrupt-{}-{}-{}.json",
                         timestamp_for_file(),
                         GetCurrentProcessId(),
                         attempt);
        if (MoveFileExW(source.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        const DWORD move_error = GetLastError();
        if (move_error != ERROR_ALREADY_EXISTS && move_error != ERROR_FILE_EXISTS) {
            set_config_error(
                error,
                config_io_error(L"隔离损坏的配置文件", source, move_error));
            return false;
        }
    }
    set_config_error(error, L"无法为损坏的配置文件生成唯一备份名称。");
    return false;
}

bool write_config_atomically(
    const std::filesystem::path& target,
    const AppConfig& config,
    std::wstring* error) {
    std::filesystem::path temporary;
    try {
        temporary = target.parent_path() /
                    std::format(
                        L"config.v2.tmp-{}-{}",
                        GetCurrentProcessId(),
                        GetCurrentThreadId());

        const std::wstring serialized = config_to_json(config);
        const std::string bytes = to_utf8(serialized);
        if (serialized.empty() || bytes.empty()) {
            throw std::runtime_error("unable to encode config as UTF-8");
        }
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                throw std::runtime_error("unable to open temporary config");
            }
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream) {
                throw std::runtime_error("unable to write temporary config");
            }
        }

        if (!MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(to_utf8(windows_error_message(GetLastError())));
        }
        return true;
    } catch (const std::exception& exception) {
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        set_config_error(
            error,
            std::format(
                L"写入配置文件“{}”失败：{}",
                target.wstring(),
                from_utf8(exception.what())));
        return false;
    }
}

bool save_config_locked(
    const ConfigStore& store,
    const AppConfig& config,
    bool merge_current_unknown,
    std::wstring* error) {
    if (config.write_protected ||
        config.schema_version < 1 ||
        config.schema_version > kCurrentConfigSchemaVersion) {
        set_config_error(
            error,
            std::format(
                L"配置 schema {} 不可由当前 schema {} 写入。",
                config.schema_version,
                kCurrentConfigSchemaVersion));
        return false;
    }
    if (!validate_global_hotkeys(config, error)) {
        return false;
    }

    AppConfig persisted = config;
    persisted.schema_version = kCurrentConfigSchemaVersion;
    persisted.write_protected = false;

    if (merge_current_unknown) {
        const FileReadResult current = read_utf8_file(store.path());
        if (current.status == FileReadStatus::read_error ||
            current.status == FileReadStatus::invalid_utf8) {
            set_config_error(error, current.error);
            return false;
        }
        if (current.status == FileReadStatus::success) {
            std::wstring parse_error;
            const auto current_config = config_from_json(current.text, &parse_error);
            if (!current_config) {
                set_config_error(
                    error,
                    std::format(
                        L"现有配置无效，已拒绝覆盖：{}",
                        parse_error.empty() ? L"无法解析配置。" : parse_error));
                return false;
            }
            if (current_config->write_protected) {
                set_config_error(
                    error,
                    std::format(
                        L"现有配置 schema {} 高于当前支持的 schema {}，已拒绝覆盖。",
                        current_config->schema_version,
                        kCurrentConfigSchemaVersion));
                return false;
            }

            JsonNode merged;
            merged.kind = JsonKind::object;
            if (!persisted.preserved_json.empty()) {
                const auto preserved = JsonParser(persisted.preserved_json).parse();
                if (preserved && preserved->kind == JsonKind::object) {
                    merged = *preserved;
                }
            }
            const auto disk_root = JsonParser(current.text).parse();
            if (!disk_root || disk_root->kind != JsonKind::object) {
                set_config_error(error, L"现有配置无法用于保留扩展字段。");
                return false;
            }
            merge_json(merged, *disk_root);
            persisted.preserved_json = serialize_json(merged);
        }
    }

    return write_config_atomically(store.path(), persisted, error);
}

}  // namespace

std::optional<Hotkey> parse_hotkey(std::wstring_view value) {
    Hotkey hotkey{MOD_NOREPEAT, 0};
    UINT seen_modifiers = 0;
    for (const auto& raw_part : split(value, L'+')) {
        const std::wstring part = upper(raw_part);
        UINT modifier = 0;
        if (part == L"CTRL" || part == L"CONTROL") {
            modifier = MOD_CONTROL;
        } else if (part == L"ALT") {
            modifier = MOD_ALT;
        } else if (part == L"SHIFT") {
            modifier = MOD_SHIFT;
        } else if (part == L"WIN" || part == L"WINDOWS") {
            modifier = MOD_WIN;
        }
        if (modifier != 0) {
            if (hotkey.virtual_key != 0 ||
                (seen_modifiers & modifier) != 0) {
                return std::nullopt;
            }
            seen_modifiers |= modifier;
            hotkey.modifiers |= modifier;
            continue;
        }

        if (hotkey.virtual_key != 0) {
            return std::nullopt;
        }
        if (part == L"PRINTSCREEN" || part == L"PRTSC") {
            hotkey.virtual_key = VK_SNAPSHOT;
        } else if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') ||
                                      (part[0] >= L'0' && part[0] <= L'9'))) {
            hotkey.virtual_key = static_cast<UINT>(part[0]);
        } else if (part.size() >= 2 && part.size() <= 3 && part[0] == L'F') {
            int function_key = 0;
            for (std::size_t index = 1; index < part.size(); ++index) {
                if (part[index] < L'0' || part[index] > L'9') {
                    return std::nullopt;
                }
                function_key = function_key * 10 + static_cast<int>(part[index] - L'0');
            }
            if (part[1] == L'0' || function_key < 1 || function_key > 24) {
                return std::nullopt;
            }
            hotkey.virtual_key = static_cast<UINT>(VK_F1 + function_key - 1);
        } else {
            return std::nullopt;
        }
    }
    if (hotkey.virtual_key == 0) {
        return std::nullopt;
    }
    return hotkey;
}

namespace {

constexpr UINT kGlobalHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_WIN;
constexpr UINT kComparableHotkeyModifiers =
    MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

bool has_global_hotkey_modifier(const Hotkey& hotkey) noexcept {
    return (hotkey.modifiers & kGlobalHotkeyModifiers) != 0;
}

bool same_hotkey(const Hotkey& left, const Hotkey& right) noexcept {
    return left.virtual_key == right.virtual_key &&
           (left.modifiers & kComparableHotkeyModifiers) ==
               (right.modifiers & kComparableHotkeyModifiers);
}

void migrate_legacy_hotkeys(AppConfig& config) {
    const AppConfig defaults;

    auto capture = parse_hotkey(config.capture_hotkey);
    if (!config.capture_hotkey.empty() &&
        (!capture || !has_global_hotkey_modifier(*capture))) {
        config.capture_hotkey = defaults.capture_hotkey;
        capture = parse_hotkey(config.capture_hotkey);
    }

    auto global_ocr = parse_hotkey(config.global_ocr_hotkey);
    const bool invalid_global_ocr =
        config.global_ocr_hotkey.empty() ||
        !global_ocr ||
        !has_global_hotkey_modifier(*global_ocr);
    const bool conflicting_global_ocr =
        config.global_ocr_enabled &&
        capture &&
        global_ocr &&
        same_hotkey(*capture, *global_ocr);
    if (invalid_global_ocr || conflicting_global_ocr) {
        config.global_ocr_enabled = false;
        config.global_ocr_hotkey = defaults.global_ocr_hotkey;
    }
}

}  // namespace

bool validate_global_hotkeys(const AppConfig& config, std::wstring* error) {
    const auto validate = [&](std::wstring_view value,
                              std::wstring_view label,
                              std::optional<Hotkey>& parsed) {
        if (value.empty()) {
            return true;
        }
        parsed = parse_hotkey(value);
        if (!parsed) {
            set_config_error(error, std::format(L"配置字段 {} 不是有效的快捷键。", label));
            return false;
        }
        if (!has_global_hotkey_modifier(*parsed)) {
            set_config_error(
                error,
                std::format(L"配置字段 {} 必须包含 Ctrl、Alt 或 Win 修饰键。", label));
            return false;
        }
        return true;
    };

    if (error) {
        error->clear();
    }
    std::optional<Hotkey> capture;
    std::optional<Hotkey> global_ocr;
    if (!validate(config.capture_hotkey, L"hotkey.capture", capture)) {
        return false;
    }
    if (config.global_ocr_enabled && config.global_ocr_hotkey.empty()) {
        set_config_error(error, L"启用全局 OCR 时，配置字段 hotkey.globalOcr 不能为空。");
        return false;
    }
    if (!validate(config.global_ocr_hotkey, L"hotkey.globalOcr", global_ocr)) {
        return false;
    }
    if (config.global_ocr_enabled &&
        capture &&
        global_ocr &&
        same_hotkey(*capture, *global_ocr)) {
        set_config_error(error, L"全局 OCR 快捷键不能与截图快捷键相同。");
        return false;
    }
    return true;
}

std::wstring normalize_ocr_engine(std::wstring_view value) {
    const std::wstring normalized = lower(value);
    if (normalized == kOcrEngineRapidV5Fast ||
        normalized == L"rapid_ocr" ||
        normalized == L"rapidocr" ||
        normalized == L"rapidocr-v5" ||
        normalized == L"wechat" ||
        normalized == L"system") {
        return std::wstring(kOcrEngineRapidV5Fast);
    }
    if (normalized == kOcrEngineRapidV5Accurate) {
        return std::wstring(kOcrEngineRapidV5Accurate);
    }
    if (normalized == kOcrEngineRapidV4Compat) {
        return std::wstring(kOcrEngineRapidV4Compat);
    }
    return std::wstring(kDefaultOcrEngine);
}

std::wstring normalize_annotation_hidden_tools(std::wstring_view value) {
    std::wstring result;
    for (const auto tool_id : kAnnotationToolbarTools) {
        for (const auto& token : split_hidden_tools(value)) {
            if (tool_id_matches(token, tool_id)) {
                if (!result.empty()) {
                    result += L",";
                }
                result += tool_id;
                break;
            }
        }
    }
    return result;
}

bool annotation_tool_hidden(std::wstring_view hidden_tools, std::wstring_view tool_id) {
    const std::wstring normalized = normalize_annotation_hidden_tools(hidden_tools);
    for (const auto& token : split_hidden_tools(normalized)) {
        if (tool_id_matches(token, tool_id)) {
            return true;
        }
    }
    return false;
}

std::wstring known_config_to_json(const AppConfig& config) {
    std::wstring result;
    result.reserve(384);
    result += L"{\"schemaVersion\":" + std::to_wstring(config.schema_version);
    result += L",\"annotation\":{\"enabled\":" + std::wstring(json_boolean(config.annotation_enabled));
    result += L",\"lockedTool\":" + std::wstring(json_boolean(config.annotation_locked_tool));
    result += L",\"hiddenTools\":" + hidden_tools_to_json_array(config.annotation_hidden_tools);
    result += L",\"toolbarOrder\":" + quote_json(config.toolbar_order);
    result += L",\"textFontFamily\":" + quote_json(config.text_font_family);
    result += L",\"textFontBold\":" + std::wstring(json_boolean(config.text_font_bold));
    result += L",\"textFontItalic\":" + std::wstring(json_boolean(config.text_font_italic));
    result += L",\"highlightAlpha\":" + std::to_wstring(std::clamp(config.annotation_highlight_alpha, 24, 192));
    result += L",\"nextSerial\":" + std::to_wstring(std::max(1, config.annotation_next_serial)) + L"}";
    result += L",\"ocr\":{\"enabled\":" + std::wstring(json_boolean(config.ocr_enabled));
    result += L",\"engine\":" + quote_json(normalize_ocr_engine(config.ocr_engine));
    result += L",\"downloadUrl\":" + quote_json(config.ocr_download_url) + L"}";
    result += L",\"shell\":{\"enabled\":" + std::wstring(json_boolean(config.shell_enabled));
    result += L",\"startAtLogin\":" + std::wstring(json_boolean(config.start_at_login));
    result += L",\"notificationsEnabled\":" + std::wstring(json_boolean(config.notifications_enabled)) + L"}";
    result += L",\"hotkey\":{\"capture\":" + quote_json(config.capture_hotkey);
    result += L",\"globalOcrEnabled\":" + std::wstring(json_boolean(config.global_ocr_enabled));
    result += L",\"globalOcr\":" + quote_json(config.global_ocr_hotkey) + L"}";
    result += L",\"shortcut\":{\"captureOcr\":" + quote_json(config.capture_ocr_shortcut);
    result += L",\"toolSelect\":" + quote_json(config.tool_shortcut_select);
    result += L",\"toolRectangle\":" + quote_json(config.tool_shortcut_rectangle);
    result += L",\"toolEllipse\":" + quote_json(config.tool_shortcut_ellipse);
    result += L",\"toolLine\":" + quote_json(config.tool_shortcut_line);
    result += L",\"toolArrow\":" + quote_json(config.tool_shortcut_arrow);
    result += L",\"toolPen\":" + quote_json(config.tool_shortcut_pen);
    result += L",\"toolMosaic\":" + quote_json(config.tool_shortcut_mosaic);
    result += L",\"toolBlur\":" + quote_json(config.tool_shortcut_blur);
    result += L",\"toolHighlight\":" + quote_json(config.tool_shortcut_highlight);
    result += L",\"toolText\":" + quote_json(config.tool_shortcut_text);
    result += L",\"toolSerial\":" + quote_json(config.tool_shortcut_serial);
    result += L",\"toolEraser\":" + quote_json(config.tool_shortcut_eraser) + L"}";
    result += L",\"capture\":{\"defaultOutput\":" + quote_json(config.default_output);
    result += L",\"customColor\":" + quote_json(config.custom_color);
    result += L",\"theme\":" + quote_json(config.theme) + L"}}";
    return result;
}

std::wstring config_to_json(const AppConfig& config) {
    if (config.write_protected && !config.preserved_json.empty()) {
        return config.preserved_json;
    }

    const std::wstring known_json = known_config_to_json(config);
    auto known = JsonParser(known_json).parse();
    if (!known) {
        return known_json;
    }
    if (config.preserved_json.empty()) {
        return known_json;
    }

    auto preserved = JsonParser(config.preserved_json).parse();
    if (!preserved || preserved->kind != JsonKind::object) {
        return known_json;
    }
    merge_json(*preserved, *known);
    return serialize_json(*preserved);
}

std::optional<AppConfig> config_from_json(std::wstring_view json_text, std::wstring* error) {
    if (error) {
        error->clear();
    }
    const auto root = JsonParser(json_text).parse();
    if (!root || root->kind != JsonKind::object) {
        set_config_error(error, L"配置不是有效的 JSON 对象。");
        return std::nullopt;
    }
    const auto parsed_schema = schema_version(*root);
    if (!parsed_schema) {
        set_config_error(error, L"配置字段 schemaVersion 必须是正十进制整数。");
        return std::nullopt;
    }
    if (*parsed_schema == kCurrentConfigSchemaVersion &&
        !validate_current_config(*root, error)) {
        return std::nullopt;
    }

    AppConfig config;
    config.schema_version = *parsed_schema;
    config.preserved_json = std::wstring(json_text);
    config.write_protected = config.schema_version > kCurrentConfigSchemaVersion;
    config.start_at_login = config.schema_version <= 1;
    if (const auto* annotation = member(*root, L"annotation")) {
        config.annotation_enabled = named_boolean(*annotation, L"enabled", true);
        config.annotation_locked_tool = named_boolean(*annotation, L"lockedTool", true);
        if (const auto* hidden_tools = member(*annotation, L"hiddenTools")) {
            config.annotation_hidden_tools = hidden_tools_from_json_node(*hidden_tools, L"");
        }
        constexpr std::wstring_view feishu_style_order =
            L"rect,ellipse,line,arrow,pen,text,serial,mosaic,highlight,watermark,pin,ocr,select,scroll,eraser,undo,redo,save,close,copy";
        config.toolbar_order = named_string(*annotation, L"toolbarOrder", feishu_style_order);
        if (config.toolbar_order == L"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,text,serial,eraser,undo,redo,ocr,scroll,pin,save,copy" ||
            config.toolbar_order == L"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,text,serial,eraser,undo,redo,ocr,scroll,pin,copy,save,close" ||
            config.toolbar_order == L"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,text,serial,eraser,undo,redo,ocr,scroll,pin,save,close,copy") {
            config.toolbar_order = feishu_style_order;
        }
        config.text_font_family = named_string(*annotation, L"textFontFamily", L"Microsoft YaHei");
        config.text_font_bold = named_boolean(*annotation, L"textFontBold", false);
        config.text_font_italic = named_boolean(*annotation, L"textFontItalic", false);
        config.annotation_highlight_alpha = named_clamped_integer(*annotation, L"highlightAlpha", 96, 24, 192);
        config.annotation_next_serial = std::max(1, named_integer(*annotation, L"nextSerial", 1));
    }
    if (const auto* ocr = member(*root, L"ocr")) {
        config.ocr_enabled = named_boolean(*ocr, L"enabled", true);
        config.ocr_engine = named_ocr_engine(*ocr, L"engine");
        config.ocr_download_url = named_string(*ocr, L"downloadUrl", kDefaultOcrDependencyManifestUrl);
    }
    if (const auto* shell = member(*root, L"shell")) {
        config.shell_enabled = named_boolean(*shell, L"enabled", true);
        config.start_at_login =
            named_boolean(*shell, L"startAtLogin", config.schema_version <= 1);
        config.notifications_enabled = named_boolean(*shell, L"notificationsEnabled", false);
    }
    if (const auto* hotkey = member(*root, L"hotkey")) {
        config.capture_hotkey = named_string(*hotkey, L"capture", L"Ctrl+Alt+A");
        config.global_ocr_enabled = named_boolean(*hotkey, L"globalOcrEnabled", false);
        config.global_ocr_hotkey = named_string(*hotkey, L"globalOcr", L"Ctrl+Alt+O");
    }
    if (const auto* shortcut = member(*root, L"shortcut")) {
        config.capture_ocr_shortcut = named_string(*shortcut, L"captureOcr", L"Shift+C");
        config.tool_shortcut_select = named_string(*shortcut, L"toolSelect", L"S");
        config.tool_shortcut_rectangle = named_string(*shortcut, L"toolRectangle", L"R");
        config.tool_shortcut_ellipse = named_string(*shortcut, L"toolEllipse", L"E");
        config.tool_shortcut_line = named_string(*shortcut, L"toolLine", L"L");
        config.tool_shortcut_arrow = named_string(*shortcut, L"toolArrow", L"A");
        config.tool_shortcut_pen = named_string(*shortcut, L"toolPen", L"P");
        config.tool_shortcut_mosaic = named_string(*shortcut, L"toolMosaic", L"M");
        config.tool_shortcut_blur = named_string(*shortcut, L"toolBlur", L"B");
        config.tool_shortcut_highlight = named_string(*shortcut, L"toolHighlight", L"H");
        config.tool_shortcut_text = named_string(*shortcut, L"toolText", L"T");
        config.tool_shortcut_serial = named_string(*shortcut, L"toolSerial", L"N");
        config.tool_shortcut_eraser = named_string(*shortcut, L"toolEraser", L"D");
    }
    if (const auto* capture = member(*root, L"capture")) {
        config.default_output = named_string(*capture, L"defaultOutput", L"clipboard");
        config.custom_color = named_string(*capture, L"customColor", L"#8000FF");
        config.theme = named_string(*capture, L"theme", L"system");
    }
    if (config.schema_version <= 1 ||
        config.schema_version > kCurrentConfigSchemaVersion) {
        migrate_legacy_hotkeys(config);
    }
    if (config.schema_version == kCurrentConfigSchemaVersion &&
        !validate_global_hotkeys(config, error)) {
        return std::nullopt;
    }
    return config;
}

std::filesystem::path config_directory() {
    try {
        std::wstring override_path(32768, L'\0');
        const DWORD override_length =
            GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", override_path.data(), static_cast<DWORD>(override_path.size()));
        if (override_length > 0 && override_length < override_path.size()) {
            override_path.resize(override_length);
            return std::filesystem::path(override_path);
        }
        PWSTR value = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value)) && value) {
            std::filesystem::path result(value);
            CoTaskMemFree(value);
            return result / L"AirScreenshot";
        }
        return std::filesystem::temp_directory_path() / L"AirScreenshot";
    } catch (...) {
        return std::filesystem::path(L".") / L"AirScreenshot";
    }
}

ConfigStore::ConfigStore() : directory_(config_directory()) {}

ConfigStore::ConfigStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

const std::filesystem::path& ConfigStore::directory() const noexcept {
    return directory_;
}

std::filesystem::path ConfigStore::path() const {
    return directory_ / L"config.v2.json";
}

std::filesystem::path ConfigStore::legacy_path() const {
    return directory_ / L"config.json";
}

std::optional<AppConfig> ConfigStore::load(std::wstring* error) const {
    if (error) {
        error->clear();
    }
    ConfigDirectoryLock lock;
    if (!lock.acquire(directory_, error)) {
        return std::nullopt;
    }

    const auto current_path = path();
    const FileReadResult current = read_utf8_file(current_path);
    if (current.status == FileReadStatus::read_error) {
        set_config_error(error, current.error);
        return std::nullopt;
    }
    if (current.status == FileReadStatus::success) {
        const auto syntax = JsonParser(current.text).parse();
        if (syntax && syntax->kind == JsonKind::object) {
            std::wstring parse_error;
            auto parsed = config_from_json(current.text, &parse_error);
            if (!parsed) {
                set_config_error(
                    error,
                    std::format(
                        L"配置文件“{}”无效：{}",
                        current_path.wstring(),
                        parse_error.empty() ? L"配置 schema 校验失败。" : parse_error));
                return std::nullopt;
            }
            if (parsed->schema_version <= 1) {
                parsed->schema_version = kCurrentConfigSchemaVersion;
                parsed->write_protected = false;
                if (!save_config_locked(*this, *parsed, false, error)) {
                    return std::nullopt;
                }
            }
            return parsed;
        }
    }

    if (current.status == FileReadStatus::invalid_utf8 ||
        current.status == FileReadStatus::success) {
        std::filesystem::path quarantine;
        if (!quarantine_corrupt_config(current_path, quarantine, error)) {
            return std::nullopt;
        }
        AppConfig fresh;
        std::wstring save_error;
        if (!save_config_locked(*this, fresh, false, &save_error)) {
            const bool restored = MoveFileExW(
                                      quarantine.c_str(),
                                      current_path.c_str(),
                                      MOVEFILE_WRITE_THROUGH) == TRUE;
            set_config_error(
                error,
                std::format(
                    L"损坏的配置已隔离到“{}”，但创建新配置失败：{}{}",
                    quarantine.wstring(),
                    save_error,
                    restored ? L"；原文件已恢复。" : L"；原文件仍保留在隔离位置。"));
            return std::nullopt;
        }
        return fresh;
    }

    const auto old_path = legacy_path();
    const FileReadResult legacy = read_utf8_file(old_path);
    if (legacy.status == FileReadStatus::read_error ||
        legacy.status == FileReadStatus::invalid_utf8) {
        set_config_error(error, legacy.error);
        return std::nullopt;
    }
    if (legacy.status == FileReadStatus::success) {
        std::wstring parse_error;
        auto parsed = config_from_json(legacy.text, &parse_error);
        if (!parsed) {
            set_config_error(
                error,
                std::format(
                    L"旧配置文件“{}”无效，未执行迁移：{}",
                    old_path.wstring(),
                    parse_error.empty() ? L"无法解析配置。" : parse_error));
            return std::nullopt;
        }
        if (parsed->schema_version > kCurrentConfigSchemaVersion) {
            parsed->write_protected = true;
            return parsed;
        }
        parsed->schema_version = kCurrentConfigSchemaVersion;
        parsed->write_protected = false;
        if (!save_config_locked(*this, *parsed, false, error)) {
            return std::nullopt;
        }
        return parsed;
    }

    AppConfig fresh;
    if (!save_config_locked(*this, fresh, false, error)) {
        return std::nullopt;
    }
    return fresh;
}

bool ConfigStore::save(const AppConfig& config, std::wstring* error) const {
    if (error) {
        error->clear();
    }
    ConfigDirectoryLock lock;
    if (!lock.acquire(directory_, error)) {
        return false;
    }
    // Known fields are intentionally last-writer-wins. The locked reread preserves
    // extension fields added by another writer since this AppConfig was loaded.
    return save_config_locked(*this, config, true, error);
}

AppConfig load_config() {
    std::wstring error;
    const auto loaded = ConfigStore().load(&error);
    if (!loaded && !error.empty()) {
        OutputDebugStringW((error + L"\n").c_str());
    }
    return loaded.value_or(AppConfig{});
}

bool is_windows_system_light_theme() {
    HKEY hKey;
    DWORD value = 1; // Default to Light if key not found
    DWORD size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(hKey);
    }
    return value == 1;
}

bool should_use_light_theme(std::wstring_view theme_config) {
    if (theme_config == L"light") {
        return true;
    }
    if (theme_config == L"dark") {
        return false;
    }
    return is_windows_system_light_theme();
}

}  // namespace airshot
