#include "airshot/command.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

bool is_option(std::wstring_view value, std::wstring_view expected) {
    return _wcsicmp(std::wstring(value).c_str(), std::wstring(expected).c_str()) == 0;
}

bool valid_launch_nonce(std::wstring_view value) {
    return value.size() == 32U &&
           std::ranges::all_of(
               value,
               [](wchar_t character) {
                   return (character >= L'0' && character <= L'9') ||
                          (character >= L'a' && character <= L'f');
               });
}

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

ParsedCli error_result(
    std::wstring message,
    bool json_output,
    ExitCode code = ExitCode::invalid_arguments,
    std::wstring error_type = {}) {
    ParsedCli result;
    result.code = code;
    result.local_only = true;
    result.json = json_output;
    if (json_output) {
        CommandResponse response;
        response.code = result.code;
        response.message = std::move(message);
        response.error_type =
            error_type.empty() ? std::wstring(default_error_type(code)) : std::move(error_type);
        result.local_text = response_to_json(response);
    } else {
        result.local_text = std::move(message);
    }
    return result;
}

ParsedCli local_result(std::wstring text, bool json_output) {
    ParsedCli result;
    result.local_only = true;
    result.json = json_output;
    if (json_output) {
        CommandResponse response;
        response.message = L"ok";
        response.text = std::move(text);
        result.local_text = response_to_json(response);
    } else {
        result.local_text = std::move(text);
    }
    return result;
}

std::wstring capture_mode_name(CaptureMode mode) {
    switch (mode) {
        case CaptureMode::region: return L"region";
        case CaptureMode::window: return L"window";
        case CaptureMode::screen: return L"screen";
    }
    return L"region";
}

std::wstring capture_output_name(CaptureOutput output) {
    switch (output) {
        case CaptureOutput::interactive: return {};
        case CaptureOutput::clipboard: return L"clipboard";
        case CaptureOutput::file: return L"file";
    }
    return {};
}

std::wstring monitor_name(const MonitorTarget& monitor) {
    switch (monitor.kind) {
        case MonitorTargetKind::all: return L"all";
        case MonitorTargetKind::primary: return L"primary";
        case MonitorTargetKind::cursor: return L"cursor";
        case MonitorTargetKind::index: return std::to_wstring(monitor.index);
    }
    return L"all";
}

std::wstring module_action_name(ModuleAction action) {
    switch (action) {
        case ModuleAction::list: return L"list";
        case ModuleAction::enable: return L"enable";
        case ModuleAction::disable: return L"disable";
    }
    return L"list";
}

std::wstring module_name(ModuleId module) {
    switch (module) {
        case ModuleId::annotation: return L"annotation";
        case ModuleId::ocr: return L"ocr";
        case ModuleId::shell: return L"shell";
    }
    return L"annotation";
}

std::wstring app_action_name(AppAction action) {
    switch (action) {
        case AppAction::start: return L"start";
        case AppAction::stop: return L"stop";
        case AppAction::status: return L"status";
        case AppAction::settings: return L"settings";
    }
    return L"status";
}

std::optional<MonitorTarget> parse_monitor(std::wstring_view value) {
    const std::wstring normalized = lower(value);
    if (normalized == L"all") {
        return MonitorTarget{MonitorTargetKind::all};
    }
    if (normalized == L"primary") {
        return MonitorTarget{MonitorTargetKind::primary};
    }
    if (normalized == L"cursor") {
        return MonitorTarget{MonitorTargetKind::cursor};
    }
    if (normalized.empty() ||
        !std::ranges::all_of(normalized, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(normalized, &consumed, 10);
        if (consumed != normalized.size() || parsed > std::numeric_limits<unsigned int>::max()) {
            return std::nullopt;
        }
        return MonitorTarget{MonitorTargetKind::index, static_cast<unsigned int>(parsed)};
    } catch (...) {
        return std::nullopt;
    }
}

bool has_trailing_separator(std::wstring_view value) {
    return !value.empty() && (value.back() == L'\\' || value.back() == L'/');
}

std::optional<std::filesystem::path> normalize_output_path(std::wstring_view value, std::wstring& error) {
    if (value.empty()) {
        error = L"--path 不能为空。";
        return std::nullopt;
    }
    try {
        std::filesystem::path path(value);
        path = std::filesystem::absolute(path).lexically_normal();

        std::error_code filesystem_error;
        const bool directory =
            has_trailing_separator(value) || std::filesystem::is_directory(path, filesystem_error);
        if (directory) {
            path /= L"";
        } else if (!path.has_extension()) {
            path += L".png";
        } else if (lower(path.extension().wstring()) != L".png") {
            error = L"--path 只支持 .png 文件。";
            return std::nullopt;
        }
        return path;
    } catch (const std::exception&) {
        error = L"--path 不是有效路径。";
        return std::nullopt;
    }
}

ParsedCli request_result(Command command, bool json_output) {
    ParsedCli result;
    result.json = json_output;
    result.request_json = command_to_json(command, json_output);
    result.command = std::move(command);
    if (result.request_json.empty()) {
        return error_result(L"无法创建命令请求。", json_output, ExitCode::unknown_error, L"internal");
    }
    return result;
}

bool valid_exit_code(int code) noexcept {
    return code >= static_cast<int>(ExitCode::success) && code <= static_cast<int>(ExitCode::ipc_failed);
}

void set_command_error(std::wstring* error, std::wstring message) {
    if (error) {
        *error = std::move(message);
    }
}

bool has_only_keys(const JsonObject& object, std::initializer_list<std::wstring_view> allowed) {
    std::uint32_t present = 0;
    for (const auto name : allowed) {
        if (object.HasKey(name)) {
            ++present;
        }
    }
    return object.Size() == present;
}

bool read_required_string(
    const JsonObject& object, std::wstring_view name, std::wstring& value, std::wstring* error) {
    if (!object.HasKey(name)) {
        set_command_error(error, std::format(L"请求缺少 {}。", name));
        return false;
    }
    const auto json_value = object.GetNamedValue(name);
    if (json_value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
        set_command_error(error, std::format(L"请求字段 {} 必须是字符串。", name));
        return false;
    }
    value = json_value.GetString().c_str();
    return true;
}

bool read_required_boolean(
    const JsonObject& object, std::wstring_view name, bool& value, std::wstring* error) {
    if (!object.HasKey(name)) {
        set_command_error(error, std::format(L"请求缺少 {}。", name));
        return false;
    }
    const auto json_value = object.GetNamedValue(name);
    if (json_value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Boolean) {
        set_command_error(error, std::format(L"请求字段 {} 必须是布尔值。", name));
        return false;
    }
    value = json_value.GetBoolean();
    return true;
}

bool read_optional_string(
    const JsonObject& object, std::wstring_view name, std::optional<std::wstring>& value, std::wstring* error) {
    if (!object.HasKey(name)) {
        value.reset();
        return true;
    }
    std::wstring parsed;
    if (!read_required_string(object, name, parsed, error)) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

std::optional<std::filesystem::path> parse_protocol_path(std::wstring_view value, std::wstring* error) {
    if (value.empty() || value.find(L'\0') != std::wstring_view::npos) {
        set_command_error(error, L"请求字段 path 不是有效路径。");
        return std::nullopt;
    }
    try {
        std::filesystem::path path(value);
        if (!path.is_absolute()) {
            set_command_error(error, L"请求字段 path 必须是绝对路径。");
            return std::nullopt;
        }
        path = path.lexically_normal();
        std::error_code filesystem_error;
        const bool directory =
            has_trailing_separator(value) || std::filesystem::is_directory(path, filesystem_error);
        if (directory) {
            path /= L"";
        } else if (!path.has_extension() || lower(path.extension().wstring()) != L".png") {
            set_command_error(error, L"请求字段 path 必须是目录或 .png 文件。");
            return std::nullopt;
        }
        return path;
    } catch (...) {
        set_command_error(error, L"请求字段 path 不是有效路径。");
        return std::nullopt;
    }
}

}  // namespace

std::wstring help_text() {
    return LR"(Air Screenshot Portable / CLI

用法:
  AirScreenshot.exe capture region [--output clipboard|file] [--path <路径>] [--json]
  AirScreenshot.exe capture window [--output clipboard|file] [--path <路径>] [--json]
  AirScreenshot.exe capture screen [--monitor all|primary|cursor|编号] [--output clipboard|file] [--path <路径>] [--json]
  AirScreenshot.exe ocr region [--copy] [--json]
  AirScreenshot.exe module list [--json]
  AirScreenshot.exe module enable|disable <annotation|ocr|shell> [--json]
  AirScreenshot.exe app start|stop|status|settings [--json]
  AirScreenshot.exe --help [--json]
  AirScreenshot.exe --version [--json]

说明:
  显示器编号从 0 开始，按物理坐标 left、top、deviceName 稳定排序。
  --path 中已存在的目录或尾随分隔符表示目录；无扩展名文件自动补 .png。
)";
}

ParsedCli parse_cli(std::span<const std::wstring> arguments) {
    const ScopedWinrtApartment apartment;

    bool json_output = false;
    std::vector<std::wstring> command_arguments;
    command_arguments.reserve(arguments.size());
    for (const auto& argument : arguments) {
        if (is_option(argument, L"--json")) {
            if (json_output) {
                return error_result(L"--json 不能重复指定。", true);
            }
            json_output = true;
        } else {
            command_arguments.push_back(argument);
        }
    }

    if (!apartment.available()) {
        return error_result(L"无法初始化 Windows Runtime。", json_output, ExitCode::unknown_error, L"internal");
    }
    if (command_arguments.empty()) {
        return local_result(help_text(), json_output);
    }
    if (is_option(command_arguments[0], L"--help") || is_option(command_arguments[0], L"-h") ||
        is_option(command_arguments[0], L"help")) {
        if (command_arguments.size() != 1) {
            return error_result(L"help 不接受额外参数。", json_output);
        }
        return local_result(help_text(), json_output);
    }
    if (is_option(command_arguments[0], L"--version") || is_option(command_arguments[0], L"version")) {
        if (command_arguments.size() != 1) {
            return error_result(L"version 不接受额外参数。", json_output);
        }
        return local_result(L"AirScreenshot " + from_utf8(AIRSHOT_VERSION), json_output);
    }

    if (is_option(command_arguments[0], L"capture")) {
        if (command_arguments.size() < 2) {
            return error_result(L"capture 需要 region、window 或 screen。", json_output);
        }

        CaptureCommand command;
        if (is_option(command_arguments[1], L"region")) {
            command.mode = CaptureMode::region;
        } else if (is_option(command_arguments[1], L"window")) {
            command.mode = CaptureMode::window;
        } else if (is_option(command_arguments[1], L"screen")) {
            command.mode = CaptureMode::screen;
        } else {
            return error_result(L"未知截图模式。", json_output);
        }

        std::optional<CaptureOutput> requested_output;
        bool saw_output = false;
        bool saw_path = false;
        bool saw_monitor = false;
        for (std::size_t index = 2; index < command_arguments.size(); ++index) {
            if (is_option(command_arguments[index], L"--output")) {
                if (saw_output) {
                    return error_result(L"--output 不能重复指定。", json_output);
                }
                saw_output = true;
                if (++index >= command_arguments.size()) {
                    return error_result(L"--output 缺少值。", json_output);
                }
                if (is_option(command_arguments[index], L"clipboard")) {
                    requested_output = CaptureOutput::clipboard;
                } else if (is_option(command_arguments[index], L"file")) {
                    requested_output = CaptureOutput::file;
                } else {
                    return error_result(L"--output 只支持 clipboard 或 file。", json_output);
                }
            } else if (is_option(command_arguments[index], L"--path")) {
                if (saw_path) {
                    return error_result(L"--path 不能重复指定。", json_output);
                }
                saw_path = true;
                if (++index >= command_arguments.size()) {
                    return error_result(L"--path 缺少值。", json_output);
                }
                std::wstring path_error;
                const auto path = normalize_output_path(command_arguments[index], path_error);
                if (!path) {
                    return error_result(std::move(path_error), json_output);
                }
                command.path = *path;
            } else if (is_option(command_arguments[index], L"--monitor")) {
                if (command.mode != CaptureMode::screen) {
                    return error_result(L"--monitor 只适用于 capture screen。", json_output);
                }
                if (saw_monitor) {
                    return error_result(L"--monitor 不能重复指定。", json_output);
                }
                saw_monitor = true;
                if (++index >= command_arguments.size()) {
                    return error_result(L"--monitor 缺少值。", json_output);
                }
                const auto monitor = parse_monitor(command_arguments[index]);
                if (!monitor) {
                    return error_result(L"--monitor 只支持 all、primary、cursor 或非负编号。", json_output);
                }
                command.monitor = *monitor;
            } else {
                return error_result(std::format(L"未知参数：{}", command_arguments[index]), json_output);
            }
        }

        if (!command.path.empty() && requested_output == CaptureOutput::clipboard) {
            return error_result(L"--path 不能与 --output clipboard 同时使用。", json_output);
        }
        if (!command.path.empty()) {
            command.output = CaptureOutput::file;
        } else if (requested_output) {
            command.output = *requested_output;
        } else if (command.mode != CaptureMode::region) {
            command.output = CaptureOutput::clipboard;
        }
        return request_result(std::move(command), json_output);
    }

    if (is_option(command_arguments[0], L"ocr")) {
        if (command_arguments.size() < 2 || !is_option(command_arguments[1], L"region")) {
            return error_result(L"ocr 首版只支持 region。", json_output);
        }
        OcrCommand command;
        bool saw_copy = false;
        for (std::size_t index = 2; index < command_arguments.size(); ++index) {
            if (!is_option(command_arguments[index], L"--copy")) {
                return error_result(std::format(L"未知参数：{}", command_arguments[index]), json_output);
            }
            if (saw_copy) {
                return error_result(L"--copy 不能重复指定。", json_output);
            }
            saw_copy = true;
            command.copy = true;
        }
        return request_result(command, json_output);
    }

    if (is_option(command_arguments[0], L"module")) {
        if (command_arguments.size() < 2) {
            return error_result(L"module 需要 list、enable 或 disable。", json_output);
        }
        ModuleCommand command;
        if (is_option(command_arguments[1], L"list")) {
            command.action = ModuleAction::list;
            if (command_arguments.size() != 2) {
                return error_result(L"module list 不接受额外参数。", json_output);
            }
        } else {
            if (is_option(command_arguments[1], L"enable")) {
                command.action = ModuleAction::enable;
            } else if (is_option(command_arguments[1], L"disable")) {
                command.action = ModuleAction::disable;
            } else {
                return error_result(L"未知 module 操作。", json_output);
            }
            if (command_arguments.size() != 3) {
                return error_result(L"module enable/disable 需要且只接受一个模块名。", json_output);
            }
            if (is_option(command_arguments[2], L"annotation")) {
                command.module = ModuleId::annotation;
            } else if (is_option(command_arguments[2], L"ocr")) {
                command.module = ModuleId::ocr;
            } else if (is_option(command_arguments[2], L"shell")) {
                command.module = ModuleId::shell;
            } else {
                return error_result(L"未知模块。", json_output);
            }
        }
        return request_result(std::move(command), json_output);
    }

    if (is_option(command_arguments[0], L"app")) {
        if (command_arguments.size() != 2) {
            return error_result(L"app 需要且只接受 start、stop、status 或 settings。", json_output);
        }
        AppCommand command;
        if (is_option(command_arguments[1], L"start")) {
            command.action = AppAction::start;
        } else if (is_option(command_arguments[1], L"stop")) {
            command.action = AppAction::stop;
        } else if (is_option(command_arguments[1], L"status")) {
            command.action = AppAction::status;
        } else if (is_option(command_arguments[1], L"settings")) {
            command.action = AppAction::settings;
        } else {
            return error_result(L"未知 app 操作。", json_output);
        }
        return request_result(command, json_output);
    }

    return error_result(L"未知命令。\n\n" + help_text(), json_output);
}

std::wstring command_to_json(
    const Command& command, bool json_output, std::wstring_view launch_nonce) {
    if (!launch_nonce.empty() &&
        !valid_launch_nonce(launch_nonce)) {
        return {};
    }
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return {};
    }

    try {
        JsonObject request;
        request.SetNamedValue(L"v", JsonValue::CreateNumberValue(1));
        request.SetNamedValue(L"json", JsonValue::CreateBooleanValue(json_output));
        if (!launch_nonce.empty()) {
            request.SetNamedValue(
                L"launchNonce", JsonValue::CreateStringValue(std::wstring(launch_nonce)));
        }
        std::visit(
            [&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, CaptureCommand>) {
                    request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"capture"));
                    request.SetNamedValue(L"mode", JsonValue::CreateStringValue(capture_mode_name(value.mode)));
                    const std::wstring output = capture_output_name(value.output);
                    if (!output.empty()) {
                        request.SetNamedValue(L"output", JsonValue::CreateStringValue(output));
                    }
                    if (!value.path.empty()) {
                        request.SetNamedValue(L"path", JsonValue::CreateStringValue(value.path.wstring()));
                    }
                    if (value.mode == CaptureMode::screen) {
                        request.SetNamedValue(L"monitor", JsonValue::CreateStringValue(monitor_name(value.monitor)));
                    }
                } else if constexpr (std::is_same_v<Value, OcrCommand>) {
                    request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"ocr"));
                    request.SetNamedValue(L"mode", JsonValue::CreateStringValue(L"region"));
                    request.SetNamedValue(L"copy", JsonValue::CreateBooleanValue(value.copy));
                } else if constexpr (std::is_same_v<Value, ModuleCommand>) {
                    request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"module"));
                    request.SetNamedValue(L"action", JsonValue::CreateStringValue(module_action_name(value.action)));
                    if (value.module) {
                        request.SetNamedValue(L"module", JsonValue::CreateStringValue(module_name(*value.module)));
                    }
                } else if constexpr (std::is_same_v<Value, AppCommand>) {
                    request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"app"));
                    request.SetNamedValue(L"action", JsonValue::CreateStringValue(app_action_name(value.action)));
                }
            },
            command);
        return request.Stringify().c_str();
    } catch (...) {
        return {};
    }
}

std::optional<Command> command_from_json(std::wstring_view json_text, std::wstring* error) {
    if (error) {
        error->clear();
    }
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        set_command_error(error, L"无法初始化 Windows Runtime。");
        return std::nullopt;
    }

    try {
        const JsonObject request = JsonObject::Parse(json_text);
        if (!request.HasKey(L"v") ||
            request.GetNamedValue(L"v").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number ||
            request.GetNamedNumber(L"v") != 1.0) {
            set_command_error(error, L"请求字段 v 必须是数字 1。");
            return std::nullopt;
        }

        [[maybe_unused]] bool json_output = false;
        if (!read_required_boolean(request, L"json", json_output, error)) {
            return std::nullopt;
        }
        std::optional<std::wstring> launch_nonce;
        if (!read_optional_string(request, L"launchNonce", launch_nonce, error)) {
            return std::nullopt;
        }
        if (launch_nonce &&
            !valid_launch_nonce(*launch_nonce)) {
            set_command_error(
                error,
                L"请求字段 launchNonce 必须是 32 位小写十六进制字符串。");
            return std::nullopt;
        }
        std::wstring command_name;
        if (!read_required_string(request, L"command", command_name, error)) {
            return std::nullopt;
        }

        if (command_name == L"capture") {
            if (!has_only_keys(request, {L"v", L"json", L"command", L"mode", L"output", L"path", L"monitor", L"launchNonce"})) {
                set_command_error(error, L"capture 请求包含未知字段。");
                return std::nullopt;
            }

            std::wstring mode;
            if (!read_required_string(request, L"mode", mode, error)) {
                return std::nullopt;
            }
            CaptureCommand command;
            if (mode == L"region") {
                command.mode = CaptureMode::region;
            } else if (mode == L"window") {
                command.mode = CaptureMode::window;
            } else if (mode == L"screen") {
                command.mode = CaptureMode::screen;
            } else {
                set_command_error(error, L"capture.mode 不是受支持的枚举值。");
                return std::nullopt;
            }

            std::optional<std::wstring> output;
            if (!read_optional_string(request, L"output", output, error)) {
                return std::nullopt;
            }
            if (!output) {
                command.output =
                    command.mode == CaptureMode::region ? CaptureOutput::interactive : CaptureOutput::clipboard;
            } else if (*output == L"clipboard") {
                command.output = CaptureOutput::clipboard;
            } else if (*output == L"file") {
                command.output = CaptureOutput::file;
            } else {
                set_command_error(error, L"capture.output 不是受支持的枚举值。");
                return std::nullopt;
            }

            std::optional<std::wstring> path;
            if (!read_optional_string(request, L"path", path, error)) {
                return std::nullopt;
            }
            if (path) {
                const auto parsed_path = parse_protocol_path(*path, error);
                if (!parsed_path) {
                    return std::nullopt;
                }
                command.path = *parsed_path;
                if (command.output != CaptureOutput::file) {
                    set_command_error(error, L"capture.path 只能与 file 输出一起使用。");
                    return std::nullopt;
                }
            }

            std::optional<std::wstring> monitor;
            if (!read_optional_string(request, L"monitor", monitor, error)) {
                return std::nullopt;
            }
            if (command.mode == CaptureMode::screen) {
                if (!monitor) {
                    set_command_error(error, L"capture screen 请求缺少 monitor。");
                    return std::nullopt;
                }
                const auto parsed_monitor = parse_monitor(*monitor);
                if (!parsed_monitor || monitor_name(*parsed_monitor) != *monitor) {
                    set_command_error(error, L"capture.monitor 不是规范的显示器枚举或编号。");
                    return std::nullopt;
                }
                command.monitor = *parsed_monitor;
            } else if (monitor) {
                set_command_error(error, L"capture.monitor 只适用于 screen。");
                return std::nullopt;
            }
            return Command{std::move(command)};
        }

        if (command_name == L"ocr") {
            if (!has_only_keys(request, {L"v", L"json", L"command", L"mode", L"copy", L"launchNonce"})) {
                set_command_error(error, L"ocr 请求包含未知字段。");
                return std::nullopt;
            }
            std::wstring mode;
            if (!read_required_string(request, L"mode", mode, error) || mode != L"region") {
                if (error && error->empty()) {
                    *error = L"ocr.mode 只支持 region。";
                }
                return std::nullopt;
            }
            OcrCommand command;
            if (!read_required_boolean(request, L"copy", command.copy, error)) {
                return std::nullopt;
            }
            return Command{command};
        }

        if (command_name == L"module") {
            if (!has_only_keys(request, {L"v", L"json", L"command", L"action", L"module", L"launchNonce"})) {
                set_command_error(error, L"module 请求包含未知字段。");
                return std::nullopt;
            }
            std::wstring action;
            if (!read_required_string(request, L"action", action, error)) {
                return std::nullopt;
            }
            ModuleCommand command;
            if (action == L"list") {
                command.action = ModuleAction::list;
                if (request.HasKey(L"module")) {
                    set_command_error(error, L"module list 不接受 module 字段。");
                    return std::nullopt;
                }
                return Command{command};
            }
            if (action == L"enable") {
                command.action = ModuleAction::enable;
            } else if (action == L"disable") {
                command.action = ModuleAction::disable;
            } else {
                set_command_error(error, L"module.action 不是受支持的枚举值。");
                return std::nullopt;
            }
            std::wstring module;
            if (!read_required_string(request, L"module", module, error)) {
                return std::nullopt;
            }
            if (module == L"annotation") {
                command.module = ModuleId::annotation;
            } else if (module == L"ocr") {
                command.module = ModuleId::ocr;
            } else if (module == L"shell") {
                command.module = ModuleId::shell;
            } else {
                set_command_error(error, L"module.module 不是受支持的枚举值。");
                return std::nullopt;
            }
            return Command{command};
        }

        if (command_name == L"app") {
            if (!has_only_keys(request, {L"v", L"json", L"command", L"action", L"launchNonce"})) {
                set_command_error(error, L"app 请求包含未知字段。");
                return std::nullopt;
            }
            std::wstring action;
            if (!read_required_string(request, L"action", action, error)) {
                return std::nullopt;
            }
            AppCommand command;
            if (action == L"start") {
                command.action = AppAction::start;
            } else if (action == L"stop") {
                command.action = AppAction::stop;
            } else if (action == L"status") {
                command.action = AppAction::status;
            } else if (action == L"settings") {
                command.action = AppAction::settings;
            } else {
                set_command_error(error, L"app.action 不是受支持的枚举值。");
                return std::nullopt;
            }
            return Command{command};
        }

        set_command_error(error, L"请求字段 command 不是受支持的枚举值。");
        return std::nullopt;
    } catch (...) {
        set_command_error(error, L"请求不是有效的 v1 JSON 对象。");
        return std::nullopt;
    }
}

std::wstring_view default_error_type(ExitCode code) noexcept {
    switch (code) {
        case ExitCode::success: return {};
        case ExitCode::unknown_error: return L"internal";
        case ExitCode::invalid_arguments: return L"invalid_arguments";
        case ExitCode::user_cancelled: return L"user_cancelled";
        case ExitCode::module_unavailable: return L"module_unavailable";
        case ExitCode::operation_failed: return L"operation_failed";
        case ExitCode::ipc_failed: return L"ipc_failed";
    }
    return L"internal";
}

std::wstring response_to_json(const CommandResponse& response) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return LR"({"v":1,"ok":false,"code":1,"message":"无法初始化 Windows Runtime。","error":{"type":"internal"}})";
    }
    JsonObject json;
    json.SetNamedValue(L"v", JsonValue::CreateNumberValue(1));
    json.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(response.code == ExitCode::success));
    json.SetNamedValue(L"code", JsonValue::CreateNumberValue(static_cast<int>(response.code)));
    json.SetNamedValue(L"message", JsonValue::CreateStringValue(response.message));
    if (!response.path.empty()) {
        json.SetNamedValue(L"path", JsonValue::CreateStringValue(response.path));
    }
    if (!response.text.empty()) {
        json.SetNamedValue(L"text", JsonValue::CreateStringValue(response.text));
    }
    if (!response.data_json.empty()) {
        try {
            json.SetNamedValue(L"data", JsonValue::Parse(response.data_json));
        } catch (...) {
            json.SetNamedValue(L"data", JsonValue::CreateStringValue(response.data_json));
        }
    }
    if (response.code != ExitCode::success) {
        JsonObject error;
        const std::wstring_view type =
            response.error_type.empty() ? default_error_type(response.code) : std::wstring_view(response.error_type);
        error.SetNamedValue(L"type", JsonValue::CreateStringValue(type));
        json.SetNamedValue(L"error", error);
    }
    return json.Stringify().c_str();
}

CommandResponse response_from_json(std::wstring_view json_text) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return {ExitCode::unknown_error, L"无法初始化 Windows Runtime。", {}, {}, {}, L"internal"};
    }
    try {
        const JsonObject json = JsonObject::Parse(json_text);
        if (!json.HasKey(L"v") ||
            json.GetNamedValue(L"v").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number ||
            json.GetNamedNumber(L"v") != 1.0 ||
            !json.HasKey(L"ok") ||
            json.GetNamedValue(L"ok").ValueType() != winrt::Windows::Data::Json::JsonValueType::Boolean ||
            !json.HasKey(L"code") ||
            json.GetNamedValue(L"code").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number ||
            !json.HasKey(L"message") ||
            json.GetNamedValue(L"message").ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
            throw winrt::hresult_invalid_argument();
        }
        const double numeric_code = json.GetNamedNumber(L"code");
        if (std::trunc(numeric_code) != numeric_code ||
            numeric_code < static_cast<double>(std::numeric_limits<int>::min()) ||
            numeric_code > static_cast<double>(std::numeric_limits<int>::max())) {
            throw winrt::hresult_invalid_argument();
        }
        const int code = static_cast<int>(numeric_code);
        if (!valid_exit_code(code) ||
            json.GetNamedBoolean(L"ok") != (code == static_cast<int>(ExitCode::success))) {
            throw winrt::hresult_invalid_argument();
        }

        CommandResponse response;
        response.code = static_cast<ExitCode>(code);
        response.message = json.GetNamedString(L"message").c_str();
        if (json.HasKey(L"path")) {
            if (json.GetNamedValue(L"path").ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
                throw winrt::hresult_invalid_argument();
            }
            response.path = json.GetNamedString(L"path").c_str();
        }
        if (json.HasKey(L"text")) {
            if (json.GetNamedValue(L"text").ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
                throw winrt::hresult_invalid_argument();
            }
            response.text = json.GetNamedString(L"text").c_str();
        }
        if (json.HasKey(L"data")) {
            response.data_json = json.GetNamedValue(L"data").Stringify().c_str();
        }
        if (json.HasKey(L"error")) {
            if (response.code == ExitCode::success) {
                throw winrt::hresult_invalid_argument();
            }
            const JsonObject error_object = json.GetNamedObject(L"error");
            if (!error_object.HasKey(L"type") ||
                error_object.GetNamedValue(L"type").ValueType() !=
                    winrt::Windows::Data::Json::JsonValueType::String) {
                throw winrt::hresult_invalid_argument();
            }
            response.error_type = error_object.GetNamedString(L"type").c_str();
            if (response.error_type.empty()) {
                throw winrt::hresult_invalid_argument();
            }
        }
        if (response.code != ExitCode::success && response.error_type.empty()) {
            response.error_type = default_error_type(response.code);
        }
        return response;
    } catch (...) {
        return {ExitCode::ipc_failed, L"宿主返回了无效响应。", {}, {}, {}, L"invalid_response"};
    }
}

}  // namespace airshot
