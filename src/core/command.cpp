#include "airshot/command.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

ParsedCli error_result(std::wstring message) {
    ParsedCli result;
    result.code = ExitCode::invalid_arguments;
    result.local_only = true;
    result.local_text = std::move(message);
    return result;
}

bool is_option(std::wstring_view value, std::wstring_view expected) {
    return _wcsicmp(std::wstring(value).c_str(), std::wstring(expected).c_str()) == 0;
}

ParsedCli request_result(JsonObject request, bool json_output) {
    request.SetNamedValue(L"v", JsonValue::CreateNumberValue(1));
    request.SetNamedValue(L"json", JsonValue::CreateBooleanValue(json_output));
    ParsedCli result;
    result.json = json_output;
    result.request_json = request.Stringify().c_str();
    return result;
}

}  // namespace

std::wstring help_text() {
    return LR"(Air Screenshot CLI

用法:
  airshot capture region [--output clipboard|file] [--path <路径>] [--json]
  airshot capture window [--output clipboard|file] [--path <路径>] [--json]
  airshot capture screen [--monitor all|primary|cursor|编号] [--output clipboard|file] [--path <路径>] [--json]
  airshot ocr region [--copy] [--json]
  airshot module list
  airshot module enable|disable <annotation|ocr|shell>
  airshot app start|stop|status|settings
  airshot --help
  airshot --version
)";
}

ParsedCli parse_cli(std::span<const std::wstring> arguments) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return error_result(L"无法初始化 Windows Runtime。");
    }
    if (arguments.empty() || is_option(arguments[0], L"--help") || is_option(arguments[0], L"-h") ||
        is_option(arguments[0], L"help")) {
        ParsedCli result;
        result.local_only = true;
        result.local_text = help_text();
        return result;
    }
    if (is_option(arguments[0], L"--version") || is_option(arguments[0], L"version")) {
        ParsedCli result;
        result.local_only = true;
        result.local_text = L"airshot " + from_utf8(AIRSHOT_VERSION);
        return result;
    }

    if (is_option(arguments[0], L"capture")) {
        if (arguments.size() < 2) {
            return error_result(L"capture 需要 region、window 或 screen。");
        }
        const std::wstring mode = arguments[1];
        if (!is_option(mode, L"region") && !is_option(mode, L"window") && !is_option(mode, L"screen")) {
            return error_result(L"未知截图模式。");
        }

        std::wstring output;
        std::wstring path;
        std::wstring monitor = L"all";
        bool json_output = false;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (is_option(arguments[index], L"--json")) {
                json_output = true;
            } else if (is_option(arguments[index], L"--output")) {
                if (++index >= arguments.size()) {
                    return error_result(L"--output 缺少值。");
                }
                output = arguments[index];
                if (!is_option(output, L"clipboard") && !is_option(output, L"file")) {
                    return error_result(L"--output 只支持 clipboard 或 file。");
                }
            } else if (is_option(arguments[index], L"--path")) {
                if (++index >= arguments.size()) {
                    return error_result(L"--path 缺少值。");
                }
                path = arguments[index];
            } else if (is_option(arguments[index], L"--monitor") && is_option(mode, L"screen")) {
                if (++index >= arguments.size()) {
                    return error_result(L"--monitor 缺少值。");
                }
                monitor = arguments[index];
            } else {
                return error_result(std::format(L"未知参数：{}", arguments[index]));
            }
        }
        if (!path.empty() && output.empty()) {
            output = L"file";
        }
        if (!is_option(mode, L"region") && output.empty()) {
            output = L"clipboard";
        }

        JsonObject request;
        request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"capture"));
        request.SetNamedValue(L"mode", JsonValue::CreateStringValue(mode));
        if (!output.empty()) {
            request.SetNamedValue(L"output", JsonValue::CreateStringValue(output));
        }
        if (!path.empty()) {
            request.SetNamedValue(L"path", JsonValue::CreateStringValue(path));
        }
        if (is_option(mode, L"screen")) {
            request.SetNamedValue(L"monitor", JsonValue::CreateStringValue(monitor));
        }
        return request_result(std::move(request), json_output);
    }

    if (is_option(arguments[0], L"ocr")) {
        if (arguments.size() < 2 || !is_option(arguments[1], L"region")) {
            return error_result(L"ocr 首版只支持 region。");
        }
        bool copy = false;
        bool json_output = false;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (is_option(arguments[index], L"--copy")) {
                copy = true;
            } else if (is_option(arguments[index], L"--json")) {
                json_output = true;
            } else {
                return error_result(std::format(L"未知参数：{}", arguments[index]));
            }
        }
        JsonObject request;
        request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"ocr"));
        request.SetNamedValue(L"mode", JsonValue::CreateStringValue(L"region"));
        request.SetNamedValue(L"copy", JsonValue::CreateBooleanValue(copy));
        return request_result(std::move(request), json_output);
    }

    if (is_option(arguments[0], L"module")) {
        if (arguments.size() < 2) {
            return error_result(L"module 需要 list、enable 或 disable。");
        }
        const std::wstring action = arguments[1];
        if (!is_option(action, L"list") && !is_option(action, L"enable") && !is_option(action, L"disable")) {
            return error_result(L"未知 module 操作。");
        }
        JsonObject request;
        request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"module"));
        request.SetNamedValue(L"action", JsonValue::CreateStringValue(action));
        if (!is_option(action, L"list")) {
            if (arguments.size() != 3) {
                return error_result(L"module enable/disable 需要模块名。");
            }
            const std::wstring module = arguments[2];
            if (!is_option(module, L"annotation") && !is_option(module, L"ocr") && !is_option(module, L"shell")) {
                return error_result(L"未知模块。");
            }
            request.SetNamedValue(L"module", JsonValue::CreateStringValue(module));
        } else if (arguments.size() != 2) {
            return error_result(L"module list 不接受额外参数。");
        }
        return request_result(std::move(request), false);
    }

    if (is_option(arguments[0], L"app")) {
        if (arguments.size() != 2) {
            return error_result(L"app 需要 start、stop、status 或 settings。");
        }
        const std::wstring action = arguments[1];
        if (!is_option(action, L"start") && !is_option(action, L"stop") && !is_option(action, L"status") &&
            !is_option(action, L"settings")) {
            return error_result(L"未知 app 操作。");
        }
        JsonObject request;
        request.SetNamedValue(L"command", JsonValue::CreateStringValue(L"app"));
        request.SetNamedValue(L"action", JsonValue::CreateStringValue(action));
        return request_result(std::move(request), false);
    }

    return error_result(L"未知命令。\n\n" + help_text());
}

std::wstring response_to_json(const CommandResponse& response) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return LR"({"v":1,"ok":false,"code":1,"message":"无法初始化 Windows Runtime。"})";
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
            json.SetNamedValue(L"data", JsonObject::Parse(response.data_json));
        } catch (...) {
            json.SetNamedValue(L"data", JsonValue::CreateStringValue(response.data_json));
        }
    }
    return json.Stringify().c_str();
}

CommandResponse response_from_json(std::wstring_view json_text) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return {ExitCode::unknown_error, L"无法初始化 Windows Runtime。"};
    }
    try {
        const JsonObject json = JsonObject::Parse(json_text);
        CommandResponse response;
        response.code = static_cast<ExitCode>(static_cast<int>(json.GetNamedNumber(L"code", 1)));
        response.message = json.GetNamedString(L"message", L"").c_str();
        response.path = json.GetNamedString(L"path", L"").c_str();
        response.text = json.GetNamedString(L"text", L"").c_str();
        if (json.HasKey(L"data")) {
            response.data_json = json.GetNamedValue(L"data").Stringify().c_str();
        }
        return response;
    } catch (...) {
        return {ExitCode::ipc_failed, L"宿主返回了无效响应。"};
    }
}

}  // namespace airshot
