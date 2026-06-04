#pragma once

#include "airshot/common.h"

namespace airshot {

struct ParsedCli {
    ExitCode code{ExitCode::success};
    bool local_only{};
    bool json{};
    std::wstring local_text;
    std::wstring request_json;
};

struct CommandResponse {
    ExitCode code{ExitCode::success};
    std::wstring message;
    std::wstring path;
    std::wstring text;
    std::wstring data_json;
};

[[nodiscard]] ParsedCli parse_cli(std::span<const std::wstring> arguments);
[[nodiscard]] std::wstring response_to_json(const CommandResponse& response);
[[nodiscard]] CommandResponse response_from_json(std::wstring_view json);
[[nodiscard]] std::wstring help_text();

}  // namespace airshot
