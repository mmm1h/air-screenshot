#pragma once

#include "airshot/common.h"

#include <variant>

namespace airshot {

enum class CaptureMode {
    region,
    repeat,
    window,
    screen,
};

enum class CaptureOutput {
    interactive,
    clipboard,
    file,
};

enum class MonitorTargetKind {
    all,
    primary,
    cursor,
    index,
};

struct MonitorTarget {
    MonitorTargetKind kind{MonitorTargetKind::all};
    unsigned int index{};
};

struct CaptureCommand {
    CaptureMode mode{CaptureMode::region};
    CaptureOutput output{CaptureOutput::interactive};
    std::filesystem::path path;
    MonitorTarget monitor;
};

struct OcrCommand {
    bool copy{};
};

enum class PinAction {
    clipboard,
    file,
    restore_interaction,
    toggle_interaction,
};

struct PinCommand {
    PinAction action{PinAction::clipboard};
    std::filesystem::path path;
};

enum class ModuleAction {
    list,
    enable,
    disable,
};

enum class ModuleId {
    annotation,
    ocr,
    shell,
};

struct ModuleCommand {
    ModuleAction action{ModuleAction::list};
    std::optional<ModuleId> module;
};

enum class AppAction {
    start,
    stop,
    status,
    settings,
    tray_show,
};

struct AppCommand {
    AppAction action{AppAction::status};
};

using Command = std::variant<
    CaptureCommand,
    OcrCommand,
    PinCommand,
    ModuleCommand,
    AppCommand>;

struct ParsedCli {
    ExitCode code{ExitCode::success};
    bool local_only{};
    bool json{};
    std::wstring local_text;
    std::wstring request_json;
    std::optional<Command> command;
};

struct CommandResponse {
    ExitCode code{ExitCode::success};
    std::wstring message;
    std::wstring path;
    std::wstring text;
    std::wstring data_json;
    std::wstring error_type;
};

[[nodiscard]] ParsedCli parse_cli(std::span<const std::wstring> arguments);
[[nodiscard]] std::wstring command_to_json(
    const Command& command,
    bool json_output,
    std::wstring_view launch_nonce = {});
[[nodiscard]] std::optional<Command> command_from_json(
    std::wstring_view json, std::wstring* error = nullptr);
[[nodiscard]] bool command_waits_for_user_input(const Command& command) noexcept;
[[nodiscard]] std::wstring response_to_json(const CommandResponse& response);
[[nodiscard]] CommandResponse response_from_json(std::wstring_view json);
[[nodiscard]] std::wstring_view default_error_type(ExitCode code) noexcept;
[[nodiscard]] std::wstring help_text();

}  // namespace airshot
