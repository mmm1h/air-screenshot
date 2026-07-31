#include "airshot/command.h"
#include "airshot/config.h"

#include <array>
#include <barrier>
#include <fstream>
#include <iostream>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
}

void write_file_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool has_corrupt_backup(const std::filesystem::path& directory) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->path().filename().wstring().starts_with(L"config.v2.corrupt-")) {
            return true;
        }
    }
    return false;
}

void test_command_contract() {
    airshot::CaptureCommand interactive_capture;
    interactive_capture.mode = airshot::CaptureMode::region;
    expect(
        airshot::command_waits_for_user_input(interactive_capture),
        L"region capture is not constrained by the transport operation timeout");
    airshot::CaptureCommand immediate_capture;
    immediate_capture.mode = airshot::CaptureMode::screen;
    expect(
        !airshot::command_waits_for_user_input(immediate_capture),
        L"screen capture retains the bounded operation timeout");
    const auto repeat = airshot::parse_cli(
        std::vector<std::wstring>{L"capture", L"repeat"});
    const auto* repeat_command =
        repeat.command
            ? std::get_if<airshot::CaptureCommand>(&*repeat.command)
            : nullptr;
    expect(
        repeat.code == airshot::ExitCode::success && repeat_command &&
            repeat_command->mode == airshot::CaptureMode::repeat &&
            repeat_command->output == airshot::CaptureOutput::clipboard &&
            !airshot::command_waits_for_user_input(*repeat.command),
        L"repeat capture is an immediate command that defaults to clipboard output");
    std::wstring repeat_error;
    const auto decoded_repeat = airshot::command_from_json(
        repeat.request_json,
        &repeat_error);
    const auto* decoded_repeat_command =
        decoded_repeat
            ? std::get_if<airshot::CaptureCommand>(&*decoded_repeat)
            : nullptr;
    expect(
        decoded_repeat_command &&
            decoded_repeat_command->mode == airshot::CaptureMode::repeat &&
            decoded_repeat_command->output ==
                airshot::CaptureOutput::clipboard &&
            repeat_error.empty(),
        L"repeat capture round trips through the canonical protocol");
    const auto repeat_file = airshot::parse_cli(
        std::vector<std::wstring>{
            L"capture",
            L"repeat",
            L"--path",
            L"repeat-shot"});
    const auto* repeat_file_command =
        repeat_file.command
            ? std::get_if<airshot::CaptureCommand>(&*repeat_file.command)
            : nullptr;
    expect(
        repeat_file.code == airshot::ExitCode::success &&
            repeat_file_command &&
            repeat_file_command->output == airshot::CaptureOutput::file &&
            repeat_file_command->path.is_absolute() &&
            repeat_file_command->path.extension() == L".png",
        L"repeat capture supports an absolute normalized file destination");
    expect(
        airshot::parse_cli(
            std::vector<std::wstring>{
                L"capture",
                L"repeat",
                L"--monitor",
                L"primary"})
                .code == airshot::ExitCode::invalid_arguments &&
            !airshot::command_from_json(
                LR"({"v":1,"json":false,"command":"capture","mode":"repeat","output":"clipboard","monitor":"primary"})",
                &repeat_error),
        L"repeat capture rejects screen-only monitor targeting");
    expect(
        airshot::command_waits_for_user_input(airshot::OcrCommand{}),
        L"OCR selection is not constrained by the transport operation timeout");
    expect(
        !airshot::command_waits_for_user_input(airshot::PinCommand{}),
        L"clipboard pinning remains a bounded immediate command");
    expect(
        !airshot::command_waits_for_user_input(
            airshot::AppCommand{airshot::AppAction::settings}),
        L"settings command completes after opening its window");

    const auto normalized = airshot::parse_cli(
        std::vector<std::wstring>{L"MoDuLe", L"EnAbLe", L"ShElL", L"--JsOn"});
    expect(normalized.code == airshot::ExitCode::success && normalized.json,
           L"module accepts --json and case-insensitive input");
    expect(normalized.request_json.find(LR"("action":"enable")") != std::wstring::npos &&
               normalized.request_json.find(LR"("module":"shell")") != std::wstring::npos,
           L"module request is normalized at the boundary");
    std::wstring protocol_error;
    const auto decoded_module = airshot::command_from_json(normalized.request_json, &protocol_error);
    const auto* module = decoded_module ? std::get_if<airshot::ModuleCommand>(&*decoded_module) : nullptr;
    expect(module && module->action == airshot::ModuleAction::enable &&
               module->module == airshot::ModuleId::shell && protocol_error.empty(),
           L"normalized module request decodes to the typed command");

    const auto app = airshot::parse_cli(
        std::vector<std::wstring>{L"APP", L"STATUS", L"--JSON"});
    expect(app.code == airshot::ExitCode::success && app.json &&
               app.request_json.find(LR"("action":"status")") != std::wstring::npos,
           L"app accepts --json and normalizes its action");
    const auto tray_show = airshot::parse_cli(
        std::vector<std::wstring>{L"app", L"tray", L"show", L"--json"});
    const auto* tray_show_command =
        tray_show.command
            ? std::get_if<airshot::AppCommand>(&*tray_show.command)
            : nullptr;
    expect(
        tray_show.code == airshot::ExitCode::success && tray_show.json &&
            tray_show_command &&
            tray_show_command->action == airshot::AppAction::tray_show &&
            tray_show.request_json.find(LR"("action":"tray-show")") !=
                std::wstring::npos,
        L"hidden tray recovery has a typed canonical command");
    std::wstring tray_show_error;
    const auto decoded_tray_show = airshot::command_from_json(
        tray_show.request_json,
        &tray_show_error);
    const auto* decoded_tray_show_command =
        decoded_tray_show
            ? std::get_if<airshot::AppCommand>(&*decoded_tray_show)
            : nullptr;
    expect(
        decoded_tray_show_command &&
            decoded_tray_show_command->action ==
                airshot::AppAction::tray_show &&
            tray_show_error.empty(),
        L"tray recovery round trips through the protocol");
    expect(
        airshot::parse_cli(
            std::vector<std::wstring>{L"app", L"tray", L"hide"})
                .code == airshot::ExitCode::invalid_arguments,
        L"tray recovery rejects unsupported mutations");

    const auto pin = airshot::parse_cli(
        std::vector<std::wstring>{L"PIN", L"CLIPBOARD", L"--JSON"});
    const auto* pin_command =
        pin.command
            ? std::get_if<airshot::PinCommand>(&*pin.command)
            : nullptr;
    expect(
        pin.code == airshot::ExitCode::success && pin.json &&
            pin_command &&
            pin_command->action == airshot::PinAction::clipboard &&
            pin.request_json.find(LR"("action":"clipboard")") !=
                std::wstring::npos,
        L"clipboard pin command has a typed canonical protocol");
    const auto restore_pin = airshot::parse_cli(
        std::vector<std::wstring>{L"pin", L"restore"});
    const auto* restore_pin_command =
        restore_pin.command
            ? std::get_if<airshot::PinCommand>(&*restore_pin.command)
            : nullptr;
    expect(
        restore_pin_command &&
            restore_pin_command->action ==
                airshot::PinAction::restore_interaction,
        L"pin interaction can be restored even without a tray icon");
    const auto toggle_pin = airshot::parse_cli(
        std::vector<std::wstring>{L"pin", L"toggle", L"--json"});
    const auto* toggle_pin_command =
        toggle_pin.command
            ? std::get_if<airshot::PinCommand>(&*toggle_pin.command)
            : nullptr;
    expect(
        toggle_pin_command &&
            toggle_pin_command->action ==
                airshot::PinAction::toggle_interaction &&
            toggle_pin.request_json.find(LR"("action":"toggle")") !=
                std::wstring::npos,
        L"pin toggle has a typed canonical protocol");

    const auto file_pin = airshot::parse_cli(
        std::vector<std::wstring>{L"pin", L"file", L".\\pin-input.png"});
    const auto* file_pin_command =
        file_pin.command
            ? std::get_if<airshot::PinCommand>(&*file_pin.command)
            : nullptr;
    expect(
        file_pin.code == airshot::ExitCode::success &&
            file_pin_command &&
            file_pin_command->action == airshot::PinAction::file &&
            file_pin_command->path.is_absolute(),
        L"pin file resolves a CLI-relative path before crossing IPC");
    std::wstring file_pin_error;
    const auto decoded_file_pin = airshot::command_from_json(
        file_pin.request_json,
        &file_pin_error);
    const auto* decoded_file_pin_command =
        decoded_file_pin
            ? std::get_if<airshot::PinCommand>(&*decoded_file_pin)
            : nullptr;
    expect(
        decoded_file_pin_command &&
            decoded_file_pin_command->action == airshot::PinAction::file &&
            decoded_file_pin_command->path == file_pin_command->path &&
            file_pin_error.empty(),
        L"pin file absolute path round trips through the command protocol");
    std::wstring relative_pin_error;
    expect(
        !airshot::command_from_json(
            LR"({"v":1,"json":false,"command":"pin","action":"file","path":"relative.png"})",
            &relative_pin_error) &&
            !relative_pin_error.empty(),
        L"pin file protocol rejects paths relative to the host working directory");
    expect(
        airshot::parse_cli(
            std::vector<std::wstring>{L"pin", L"toggle", L"extra"})
                .code == airshot::ExitCode::invalid_arguments &&
            airshot::parse_cli(
                std::vector<std::wstring>{L"pin", L"file"})
                .code == airshot::ExitCode::invalid_arguments,
        L"pin toggle and file reject malformed arity");
    expect(
        airshot::parse_cli(
            std::vector<std::wstring>{L"pin", L"unknown"})
                .code == airshot::ExitCode::invalid_arguments,
        L"pin rejects unknown actions");

    const auto help = airshot::parse_cli(std::vector<std::wstring>{L"--help", L"--json"});
    expect(help.local_only && help.json && help.local_text.find(LR"("v":1)") != std::wstring::npos &&
               help.local_text.find(LR"("ok":true)") != std::wstring::npos,
           L"help supports the response v1 JSON envelope");

    const auto local_error = airshot::parse_cli(
        std::vector<std::wstring>{L"capture", L"banana", L"--json"});
    expect(local_error.code == airshot::ExitCode::invalid_arguments && local_error.json &&
               local_error.local_text.find(LR"("type":"invalid_arguments")") != std::wstring::npos,
           L"local errors use structured JSON");

    expect(airshot::parse_cli(
               std::vector<std::wstring>{L"capture", L"screen", L"--monitor", L"0", L"--monitor", L"1"})
               .code == airshot::ExitCode::invalid_arguments,
           L"duplicate monitor is rejected");
    expect(airshot::parse_cli(
               std::vector<std::wstring>{L"ocr", L"region", L"--copy", L"--copy"})
               .code == airshot::ExitCode::invalid_arguments,
           L"duplicate switch is rejected");
    expect(airshot::parse_cli(
               std::vector<std::wstring>{L"app", L"status", L"--json", L"--json"})
               .code == airshot::ExitCode::invalid_arguments,
           L"duplicate json switch is rejected");
    expect(airshot::parse_cli(
               std::vector<std::wstring>{
                   L"capture", L"window", L"--output", L"clipboard", L"--path", L"shot.png"})
               .code == airshot::ExitCode::invalid_arguments,
           L"clipboard and path are mutually exclusive");
    expect(airshot::parse_cli(
               std::vector<std::wstring>{L"capture", L"window", L"--path", L"shot.jpg"})
               .code == airshot::ExitCode::invalid_arguments,
           L"non-PNG path is rejected");

    const auto default_screen = airshot::parse_cli(
        std::vector<std::wstring>{L"capture", L"screen"});
    const auto* default_screen_capture =
        default_screen.command ? std::get_if<airshot::CaptureCommand>(&*default_screen.command) : nullptr;
    expect(default_screen.code == airshot::ExitCode::success && default_screen_capture &&
               default_screen_capture->monitor.kind == airshot::MonitorTargetKind::all,
           L"screen capture defaults to all monitors");
    expect(default_screen.request_json.find(LR"("monitor":"all")") != std::wstring::npos,
           L"default all-monitor target is explicit on the IPC boundary");

    const auto path = airshot::parse_cli(
        std::vector<std::wstring>{L"capture", L"window", L"--path", L"relative-shot"});
    expect(path.code == airshot::ExitCode::success && path.command.has_value(),
           L"extensionless relative path is accepted");
    if (path.command) {
        const auto* capture = std::get_if<airshot::CaptureCommand>(&*path.command);
        expect(capture && capture->path.is_absolute() && capture->path.extension() == L".png",
               L"relative path is absolutized and receives .png");
        if (capture) {
            const auto decoded = airshot::command_from_json(
                airshot::command_to_json(*capture, true), &protocol_error);
            const auto* decoded_capture =
                decoded ? std::get_if<airshot::CaptureCommand>(&*decoded) : nullptr;
            expect(decoded_capture && decoded_capture->mode == capture->mode &&
                       decoded_capture->output == capture->output &&
                       decoded_capture->path == capture->path,
                   L"capture command protocol round trip is symmetric");
        }
    }

    const auto path_root = std::filesystem::temp_directory_path() /
                           std::format(L"airshot-command-path-test-{}-{}",
                                       GetCurrentProcessId(),
                                       GetTickCount64());
    std::error_code path_error;
    std::filesystem::create_directories(path_root, path_error);
    expect(!path_error, L"command path test directory is created");
    if (!path_error) {
        const auto existing_directory = airshot::parse_cli(
            std::vector<std::wstring>{L"capture", L"screen", L"--path", path_root.wstring()});
        const auto* existing_capture =
            existing_directory.command
                ? std::get_if<airshot::CaptureCommand>(&*existing_directory.command)
                : nullptr;
        expect(existing_capture && existing_capture->output == airshot::CaptureOutput::file &&
                   existing_capture->path.filename().empty() &&
                   existing_capture->path.parent_path() == path_root.lexically_normal(),
               L"an existing directory remains a directory output target");

        const auto nonexistent_directory = path_root / L"not-yet-created";
        const std::wstring trailing_path = nonexistent_directory.wstring() + L"\\";
        const auto trailing_directory = airshot::parse_cli(
            std::vector<std::wstring>{L"capture", L"screen", L"--path", trailing_path});
        const auto* trailing_capture =
            trailing_directory.command
                ? std::get_if<airshot::CaptureCommand>(&*trailing_directory.command)
                : nullptr;
        expect(trailing_capture && trailing_capture->output == airshot::CaptureOutput::file &&
                   trailing_capture->path.filename().empty() &&
                   trailing_capture->path.parent_path() == nonexistent_directory.lexically_normal(),
               L"a nonexistent path with a trailing separator remains a directory output target");
    }
    std::filesystem::remove_all(path_root, path_error);

    expect(!airshot::command_from_json(
               LR"({"v":2,"json":false,"command":"app","action":"status"})", &protocol_error) &&
               !protocol_error.empty(),
           L"protocol rejects unsupported versions");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"app","action":"STATUS"})", &protocol_error),
           L"protocol rejects noncanonical enum values");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"app","action":"status","future":true})", &protocol_error),
           L"protocol rejects unknown request fields");
    expect(airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"app","action":"status","launchNonce":"0123456789abcdef0123456789abcdef"})",
               &protocol_error)
               .has_value(),
           L"protocol accepts a canonical launch nonce");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"app","action":"status","launchNonce":"0123456789ABCDEF0123456789ABCDEF"})",
               &protocol_error),
           L"protocol rejects an uppercase launch nonce");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"app","action":"status","launchNonce":"short"})",
               &protocol_error),
           L"protocol rejects a launch nonce with the wrong length");
    expect(
        airshot::command_to_json(
            airshot::AppCommand{airshot::AppAction::status},
            false,
            L"not-a-launch-nonce")
            .empty(),
        L"protocol refuses to serialize a noncanonical launch nonce");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"capture","mode":"screen","monitor":0})", &protocol_error),
           L"protocol rejects a monitor with the wrong JSON type");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"capture","mode":"screen","monitor":"01"})", &protocol_error),
           L"protocol rejects a noncanonical numeric monitor");
    expect(!airshot::command_from_json(
               LR"({"v":1,"json":false,"command":"capture","mode":"region","output":"file","path":"shot.png"})",
               &protocol_error),
           L"protocol rejects relative output paths");

    for (int code = static_cast<int>(airshot::ExitCode::success);
         code <= static_cast<int>(airshot::ExitCode::ipc_failed);
         ++code) {
        airshot::CommandResponse response;
        response.code = static_cast<airshot::ExitCode>(code);
        response.message = std::format(L"response-{}", code);
        response.path = L"C:\\capture.png";
        response.text = L"payload";
        response.data_json = LR"({"kind":"test"})";
        if (response.code != airshot::ExitCode::success) {
            response.error_type = std::format(L"error-{}", code);
        }
        const auto round_trip = airshot::response_from_json(airshot::response_to_json(response));
        expect(round_trip.code == response.code &&
                   round_trip.message == response.message &&
                   round_trip.path == response.path &&
                   round_trip.text == response.text &&
                   round_trip.data_json == response.data_json &&
                   round_trip.error_type == response.error_type,
               std::format(L"exit code {} response round trip is symmetric", code));
    }
    expect(airshot::response_from_json(
               LR"({"v":1,"ok":true,"code":5,"message":"inconsistent"})")
               .error_type == L"invalid_response",
           L"response parser rejects an inconsistent ok/code pair");
    expect(airshot::response_from_json(
               LR"({"v":1,"ok":false,"code":2.5,"message":"fractional"})")
               .error_type == L"invalid_response",
           L"response parser rejects a fractional exit code");
}

void test_config_contract() {
    const airshot::AppConfig fresh;
    expect(fresh.schema_version == airshot::kCurrentConfigSchemaVersion &&
               !fresh.start_at_login &&
               fresh.pin_hotkey.empty(),
           L"fresh config does not steal an existing system-wide paste shortcut");

    const auto legacy = airshot::config_from_json(LR"({"shell":{"enabled":true}})");
    expect(legacy && legacy->schema_version == 1 && legacy->start_at_login,
           L"v1 missing startAtLogin retains the old true default");
    const auto legacy_start_enabled =
        airshot::config_from_json(LR"({"shell":{"enabled":true,"startAtLogin":true}})");
    const auto legacy_start_disabled =
        airshot::config_from_json(LR"({"shell":{"enabled":true,"startAtLogin":false}})");
    expect(legacy_start_enabled && legacy_start_enabled->schema_version == 1 &&
               legacy_start_enabled->start_at_login,
           L"v1 explicit true startAtLogin is preserved");
    expect(legacy_start_disabled && legacy_start_disabled->schema_version == 1 &&
               !legacy_start_disabled->start_at_login,
           L"v1 explicit false startAtLogin is preserved");

    const auto current = airshot::config_from_json(
        LR"({"schemaVersion":2,"shell":{"enabled":true},"extension":{"mode":"future"},"capture":{"futureFlag":7}})");
    expect(current && !current->start_at_login && !current->write_protected,
           L"schema 2 missing startAtLogin uses the new false default");
    if (current) {
        airshot::AppConfig edited = *current;
        edited.shell_enabled = false;
        edited.tray_icon_visible = false;
        edited.automatic_updates_enabled = false;
        edited.last_update_check_unix = 1'725'000'000;
        edited.warned_update_target = LR"(c:\readonly\airscreenshot.exe)";
        const auto serialized = airshot::config_to_json(edited);
        expect(serialized.find(LR"("extension":{"mode":"future"})") != std::wstring::npos &&
                   serialized.find(LR"("futureFlag":7)") != std::wstring::npos &&
                   serialized.find(LR"("trayIconVisible":false)") != std::wstring::npos &&
                   serialized.find(LR"("automatic":false)") != std::wstring::npos &&
                   serialized.find(LR"("lastCheckUnix":1725000000)") != std::wstring::npos &&
                   serialized.find(LR"("warnedTarget":"c:\\readonly\\airscreenshot.exe")") !=
                       std::wstring::npos,
                L"same-schema unknown keys survive a save");
        const auto round_trip = airshot::config_from_json(serialized);
        expect(round_trip && !round_trip->tray_icon_visible &&
                   !round_trip->automatic_updates_enabled &&
                   round_trip->last_update_check_unix == 1'725'000'000 &&
                   round_trip->warned_update_target ==
                       LR"(c:\readonly\airscreenshot.exe)",
                L"update scheduling state round trips through schema 2");
    }

    airshot::AppConfig style_config;
    airshot::AnnotationToolStyleConfig rectangle_style;
    rectangle_style.color = L"#123ABC";
    rectangle_style.width = 8;
    rectangle_style.text_size = 32;
    rectangle_style.text_style = L"outline";
    rectangle_style.highlight_alpha = 144;
    rectangle_style.effect_strength = 73;
    rectangle_style.effect_rect = true;
    rectangle_style.fill_style = L"translucent";
    rectangle_style.stroke_pattern = L"dashed";
    rectangle_style.arrow_head_style = L"both";
    rectangle_style.rounded_rectangle = true;
    style_config.annotation_tool_styles.emplace(
        L"rect", rectangle_style);
    const std::wstring serialized_styles =
        airshot::config_to_json(style_config);
    const auto style_round_trip =
        airshot::config_from_json(serialized_styles);
    expect(
        style_round_trip &&
            style_round_trip->annotation_tool_styles.contains(L"rect") &&
            style_round_trip->annotation_tool_styles.at(L"rect") ==
                rectangle_style,
        L"every persisted per-tool annotation style field survives a config round trip");

    airshot::AppConfig clamped_style_config;
    airshot::AnnotationToolStyleConfig clamped_style;
    clamped_style.width = 999;
    clamped_style.text_size = -12;
    clamped_style.highlight_alpha = 500;
    clamped_style.effect_strength = -20;
    clamped_style_config.annotation_tool_styles.emplace(
        L"highlight", clamped_style);
    const auto clamped_style_round_trip = airshot::config_from_json(
        airshot::config_to_json(clamped_style_config));
    expect(
        clamped_style_round_trip &&
            clamped_style_round_trip->annotation_tool_styles.at(L"highlight")
                    .width == 50 &&
            clamped_style_round_trip->annotation_tool_styles.at(L"highlight")
                    .text_size == 12 &&
            clamped_style_round_trip->annotation_tool_styles.at(L"highlight")
                    .highlight_alpha == 192 &&
            clamped_style_round_trip->annotation_tool_styles.at(L"highlight")
                    .effect_strength == 0,
        L"serialized per-tool style values are strictly clamped to product limits");

    const auto directly_clamped_styles = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"toolStyles":{"blur":{"width":-40,"textSize":400,"highlightAlpha":-1,"effectStrength":1000}}}})");
    expect(
        directly_clamped_styles &&
            directly_clamped_styles->annotation_tool_styles.at(L"blur")
                    .width == 1 &&
            directly_clamped_styles->annotation_tool_styles.at(L"blur")
                    .text_size == 96 &&
            directly_clamped_styles->annotation_tool_styles.at(L"blur")
                    .highlight_alpha == 24 &&
            directly_clamped_styles->annotation_tool_styles.at(L"blur")
                    .effect_strength == 100,
        L"loaded per-tool style integers are clamped without breaking old configuration files");
    expect(
        legacy && legacy->annotation_tool_styles.empty(),
        L"legacy configs without toolStyles retain the historical in-memory defaults");

    constexpr std::array malformed_tool_styles{
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":[]}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":{"rect":false}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":{"rect":{"color":"red"}}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":{"text":{"textStyle":"glow"}}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":{"arrow":{"arrowHeadStyle":"sideways"}}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"annotation":{"toolStyles":{"pen":{"width":2.5}}}})"),
    };
    for (const auto malformed : malformed_tool_styles) {
        std::wstring style_error;
        expect(
            !airshot::config_from_json(malformed, &style_error) &&
                !style_error.empty(),
            L"schema 2 rejects malformed known per-tool style fields with a diagnostic");
    }

    const auto extended_styles = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"toolStyles":{"rect":{"width":4,"futureDash":17},"futureBrush":{"glow":3}}}})");
    expect(
        extended_styles &&
            airshot::config_to_json(*extended_styles).find(
                LR"("futureDash":17)") != std::wstring::npos &&
            airshot::config_to_json(*extended_styles).find(
                LR"("futureBrush":{"glow":3})") != std::wstring::npos,
        L"unknown future tool-style keys survive a same-schema save");

    const auto region_history = airshot::config_from_json(
        LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":-1920,"top":-10,"width":640,"height":480,"topology":"v1-0123456789abcdef"}}})");
    expect(
        region_history && region_history->last_region_capture &&
            region_history->last_region_capture->bounds.left == -1920 &&
            region_history->last_region_capture->bounds.top == -10 &&
            region_history->last_region_capture->bounds.right == -1280 &&
            region_history->last_region_capture->bounds.bottom == 470 &&
            region_history->last_region_capture->topology_signature ==
                L"v1-0123456789abcdef",
        L"last successful region loads with negative physical coordinates");
    if (region_history) {
        const std::wstring serialized =
            airshot::config_to_json(*region_history);
        const auto round_trip = airshot::config_from_json(serialized);
        expect(
            serialized.find(LR"("lastRegion":{)") !=
                    std::wstring::npos &&
                serialized.find(LR"("topology":"v1-0123456789abcdef")") !=
                    std::wstring::npos,
            L"last successful region is serialized using physical bounds and topology");
        expect(
            round_trip && round_trip->last_region_capture &&
                round_trip->last_region_capture->bounds.right == -1280 &&
                round_trip->last_region_capture->bounds.bottom == 470,
            L"last successful region survives a config serialization round trip");
    }
    const auto no_region_history = airshot::config_from_json(
        LR"({"schemaVersion":2,"capture":{"lastRegion":null}})");
    expect(
        no_region_history && !no_region_history->last_region_capture &&
            airshot::config_to_json(*no_region_history).find(
                LR"("lastRegion":null)") != std::wstring::npos,
        L"missing repeat-region history is explicitly persisted as null");
    constexpr std::array malformed_region_history{
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":[]}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":0,"top":0,"width":10,"height":10}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":0.0,"top":0,"width":10,"height":10,"topology":"v1-0123456789abcdef"}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":0,"top":0,"width":1,"height":10,"topology":"v1-0123456789abcdef"}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":0,"top":0,"width":10,"height":10,"topology":"v1-0123456789ABCDEF"}}})"),
        std::wstring_view(
            LR"({"schemaVersion":2,"capture":{"lastRegion":{"left":2147483647,"top":0,"width":2,"height":10,"topology":"v1-0123456789abcdef"}}})"),
    };
    for (const auto malformed : malformed_region_history) {
        std::wstring history_error;
        expect(
            !airshot::config_from_json(malformed, &history_error) &&
                !history_error.empty(),
            L"malformed repeat-region history is rejected with a diagnostic");
    }

    const auto precise_unknown_numbers = airshot::config_from_json(
        LR"({"schemaVersion":2,"extension":{"largeInteger":9007199254740993,"decimal":1.2300e+02}})");
    expect(precise_unknown_numbers.has_value(),
           L"config accepts unknown numeric extension values");
    if (precise_unknown_numbers) {
        const auto serialized = airshot::config_to_json(*precise_unknown_numbers);
        expect(serialized.find(LR"("largeInteger":9007199254740993)") != std::wstring::npos &&
                   serialized.find(LR"("decimal":1.2300e+02)") != std::wstring::npos,
               L"unknown numeric extension values retain their exact JSON lexemes");
    }

    const auto future = airshot::config_from_json(
        LR"({"schemaVersion":3,"shell":{"enabled":false},"futureValue":42})");
    expect(future && future->write_protected, L"future schema is marked write protected");
    constexpr std::wstring_view future_hotkey_json =
        LR"({"schemaVersion":3,"hotkey":{"capture":"Hyper+Gesture","globalOcrEnabled":true,"globalOcr":"O"},"futureHotkeyMode":"gesture-v2"})";
    std::wstring future_hotkey_error = L"stale";
    const auto future_hotkeys =
        airshot::config_from_json(future_hotkey_json, &future_hotkey_error);
    const airshot::AppConfig future_hotkey_defaults;
    expect(future_hotkeys &&
               future_hotkeys->write_protected &&
               future_hotkeys->capture_hotkey == future_hotkey_defaults.capture_hotkey &&
               !future_hotkeys->global_ocr_enabled &&
               future_hotkeys->global_ocr_hotkey ==
                   future_hotkey_defaults.global_ocr_hotkey &&
               future_hotkeys->preserved_json == future_hotkey_json &&
               airshot::config_to_json(*future_hotkeys) == future_hotkey_json &&
               future_hotkey_error.empty(),
           L"future schema safely degrades unknown hotkeys in memory while preserving its source");
    const auto far_future = airshot::config_from_json(
        LR"({"schemaVersion":2147483648,"futureValue":42})");
    expect(far_future && far_future->write_protected,
           L"future schema beyond the local integer range is still write protected");
    expect(!airshot::config_from_json(LR"({"schemaVersion":2,"schemaVersion":3})"),
           L"config rejects duplicate keys");
    expect(!airshot::config_from_json(LR"({"schemaVersion":02})"),
           L"config rejects nonstandard JSON numbers");
    expect(!airshot::config_from_json(LR"({"schemaVersion":2,"future":"\uD800"})"),
           L"config rejects an unpaired UTF-16 surrogate");
    expect(airshot::config_from_json(
               LR"({"schemaVersion":2,"future":"\uD83D\uDE00"})")
               .has_value(),
           L"config accepts a paired UTF-16 surrogate");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2.0000000000000001,"shell":{"enabled":true}})"),
           L"schema version is validated from its exact integer lexeme");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"shell":{"enabled":"false"}})"),
           L"schema 2 rejects a known boolean with the wrong type");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"shell":{"trayIconVisible":"false"}})"),
           L"schema 2 rejects a malformed tray icon visibility flag");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"capture":{"theme":"sepia"}})"),
           L"schema 2 rejects an invalid known enum");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"capture":{"includeCursor":"true"}})"),
           L"schema 2 rejects a malformed cursor capture flag");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"annotation":{"hiddenTools":["pen",7]}})"),
           L"schema 2 rejects a malformed known string array");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"update":{"automatic":"true"}})"),
           L"schema 2 rejects a malformed automatic update flag");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"update":{"lastCheckUnix":-1}})"),
           L"schema 2 rejects a negative update check timestamp");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"update":{"lastCheckUnix":9223372036854775808}})"),
           L"schema 2 rejects an overflowing update check timestamp");

    const auto strict_hotkey = airshot::parse_hotkey(L"Ctrl+Shift+F24");
    expect(strict_hotkey &&
               strict_hotkey->virtual_key == VK_F24 &&
               (strict_hotkey->modifiers & (MOD_CONTROL | MOD_SHIFT)) ==
                   (MOD_CONTROL | MOD_SHIFT),
           L"hotkey parser accepts one canonical primary key with unique modifiers");
    constexpr std::array malformed_hotkeys{
        std::wstring_view(L"Ctrl+F2junk"),
        std::wstring_view(L"Ctrl+A+B"),
        std::wstring_view(L"Ctrl+A+A"),
        std::wstring_view(L"Ctrl+Ctrl+A"),
        std::wstring_view(L"Ctrl+Control+A"),
        std::wstring_view(L"Win+Windows+F2"),
        std::wstring_view(L"Ctrl++A"),
        std::wstring_view(L"+Ctrl+A"),
        std::wstring_view(L"Ctrl+A+"),
        std::wstring_view(L"A+Ctrl"),
        std::wstring_view(L"Ctrl+F02"),
    };
    for (const auto hotkey : malformed_hotkeys) {
        expect(!airshot::parse_hotkey(hotkey),
               std::format(L"hotkey parser rejects malformed syntax: {}", hotkey));
    }

    std::wstring hotkey_error;
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"capture":"A"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"config rejects a global capture hotkey without Ctrl, Alt, or Win");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"pin":"F3"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"config rejects a global pin hotkey without Ctrl, Alt, or Win");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"capture":"Ctrl+Alt+A","pin":"Alt+Control+A"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"config rejects equivalent capture and pin hotkeys");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"globalOcr":"F2"}})",
               &hotkey_error) &&
                !hotkey_error.empty(),
           L"config rejects a global OCR hotkey without Ctrl, Alt, or Win");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"globalOcrEnabled":true,"globalOcr":""}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"schema 2 rejects an enabled global OCR hotkey with no key");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"capture":"Ctrl+Alt+A","globalOcrEnabled":true,"globalOcr":"Alt+Control+A"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"schema 2 rejects equivalent capture and enabled global OCR hotkeys");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"pin":"Ctrl+Alt+O","globalOcrEnabled":true,"globalOcr":"Alt+Control+O"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"schema 2 rejects equivalent pin and enabled global OCR hotkeys");
    hotkey_error.clear();
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"globalOcrEnabled":false,"globalOcr":"Ctrl+F2junk"}})",
               &hotkey_error) &&
               !hotkey_error.empty(),
           L"schema 2 rejects malformed disabled global OCR hotkey syntax");
    expect(airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"capture":"Ctrl+Alt+A","globalOcrEnabled":false,"globalOcr":"Alt+Control+A"}})")
               .has_value(),
           L"schema 2 permits duplicate global hotkeys while global OCR is disabled");
    expect(airshot::config_from_json(
               LR"({"schemaVersion":2,"hotkey":{"capture":"Ctrl+A","globalOcr":"Win+F2"},"shortcut":{"toolPen":"P"}})")
               .has_value(),
           L"config accepts modified global hotkeys while local shortcuts remain single-key");

    const airshot::AppConfig hotkey_defaults;
    const auto unsafe_legacy_hotkeys = airshot::config_from_json(
        LR"({"schemaVersion":1,"hotkey":{"capture":"F9","globalOcrEnabled":true,"globalOcr":"O"}})");
    expect(unsafe_legacy_hotkeys &&
               unsafe_legacy_hotkeys->capture_hotkey == hotkey_defaults.capture_hotkey &&
               !unsafe_legacy_hotkeys->global_ocr_enabled &&
               unsafe_legacy_hotkeys->global_ocr_hotkey == hotkey_defaults.global_ocr_hotkey,
           L"schema 1 unsafe global hotkeys migrate to safe defaults without blocking load");
    const auto malformed_legacy_hotkeys = airshot::config_from_json(
        LR"({"schemaVersion":1,"hotkey":{"capture":"Ctrl+F2junk","globalOcrEnabled":true,"globalOcr":"Ctrl+A+B"}})");
    expect(malformed_legacy_hotkeys &&
               malformed_legacy_hotkeys->capture_hotkey == hotkey_defaults.capture_hotkey &&
               !malformed_legacy_hotkeys->global_ocr_enabled &&
               malformed_legacy_hotkeys->global_ocr_hotkey == hotkey_defaults.global_ocr_hotkey,
           L"schema 1 malformed global hotkeys migrate to safe defaults");
    const auto conflicting_legacy_hotkeys = airshot::config_from_json(
        LR"({"schemaVersion":1,"hotkey":{"capture":"Ctrl+Alt+O","globalOcrEnabled":true,"globalOcr":"Alt+Control+O"}})");
    expect(conflicting_legacy_hotkeys &&
               conflicting_legacy_hotkeys->capture_hotkey == L"Ctrl+Alt+O" &&
               !conflicting_legacy_hotkeys->global_ocr_enabled &&
               conflicting_legacy_hotkeys->global_ocr_hotkey == hotkey_defaults.global_ocr_hotkey,
           L"schema 1 conflicting global OCR hotkey is safely disabled during migration");
    const auto safe_legacy_hotkeys = airshot::config_from_json(
        LR"({"schemaVersion":1,"hotkey":{"capture":"Ctrl+Shift+F9","globalOcrEnabled":true,"globalOcr":"Win+F2"}})");
    expect(safe_legacy_hotkeys &&
               safe_legacy_hotkeys->capture_hotkey == L"Ctrl+Shift+F9" &&
               safe_legacy_hotkeys->global_ocr_enabled &&
               safe_legacy_hotkeys->global_ocr_hotkey == L"Win+F2",
           L"schema 1 safe global hotkeys are preserved during migration");

    const auto root = std::filesystem::temp_directory_path() /
                      std::format(L"airshot-command-config-test-{}-{}", GetCurrentProcessId(), GetTickCount64());
    const airshot::ConfigStore store(root);
    std::filesystem::create_directories(root);
    {
        const airshot::ConfigStore unsafe_store(root / L"unsafe-hotkey-save");
        airshot::AppConfig unsafe;
        unsafe.capture_hotkey = L"PrintScreen";
        std::wstring error;
        expect(!unsafe_store.save(unsafe, &error) && !error.empty() &&
                   !std::filesystem::exists(unsafe_store.path()),
               L"ConfigStore refuses to persist an unsafe global hotkey");
    }
    {
        std::ofstream stream(store.legacy_path(), std::ios::binary);
        stream << R"({"shell":{"enabled":false},"hotkey":{"capture":"F9","globalOcrEnabled":true,"globalOcr":"O"},"legacyExtension":{"kept":true}})";
    }
    std::wstring load_error;
    const auto migrated = store.load(&load_error);
    expect(migrated && migrated->schema_version == airshot::kCurrentConfigSchemaVersion &&
               migrated->start_at_login &&
               migrated->capture_hotkey == hotkey_defaults.capture_hotkey &&
               !migrated->global_ocr_enabled &&
               migrated->global_ocr_hotkey == hotkey_defaults.global_ocr_hotkey &&
               load_error.empty() &&
               std::filesystem::exists(store.path()) && std::filesystem::exists(store.legacy_path()),
           L"legacy config safely migrates hotkeys to config.v2.json without modifying its source");
    const auto migrated_text = read_file_bytes(store.path());
    const auto migrated_on_disk =
        airshot::config_from_json(airshot::from_utf8(migrated_text));
    expect(migrated_text.find("\"legacyExtension\"") != std::string::npos &&
               migrated_on_disk &&
               migrated_on_disk->schema_version == airshot::kCurrentConfigSchemaVersion &&
               migrated_on_disk->capture_hotkey == hotkey_defaults.capture_hotkey &&
               !migrated_on_disk->global_ocr_enabled &&
               migrated_on_disk->global_ocr_hotkey == hotkey_defaults.global_ocr_hotkey,
           L"migration persists safe hotkeys while retaining unknown legacy keys");

    if (future) {
        std::wstring error;
        expect(!store.save(*future, &error) && !error.empty(),
               L"future schema cannot overwrite schema 2 storage");
    }

    {
        const auto directory = root / L"future-read-only";
        const airshot::ConfigStore future_store(directory);
        std::filesystem::create_directories(directory);
        const std::string future_bytes = airshot::to_utf8(future_hotkey_json);
        write_file_bytes(future_store.path(), future_bytes);
        std::wstring error;
        const auto loaded = future_store.load(&error);
        expect(loaded &&
                   loaded->write_protected &&
                   loaded->capture_hotkey == future_hotkey_defaults.capture_hotkey &&
                   !loaded->global_ocr_enabled &&
                   loaded->global_ocr_hotkey ==
                       future_hotkey_defaults.global_ocr_hotkey &&
                   loaded->preserved_json == future_hotkey_json &&
                   error.empty() &&
                   read_file_bytes(future_store.path()) == future_bytes,
               L"ConfigStore loads future hotkeys safely without rewriting the future schema");
    }

    {
        const auto directory = root / L"semantic-error";
        const airshot::ConfigStore semantic_store(directory);
        std::filesystem::create_directories(directory);
        constexpr std::string_view invalid =
            R"({"schemaVersion":2,"shell":{"enabled":"false"}})";
        write_file_bytes(semantic_store.path(), invalid);
        std::wstring error;
        const auto loaded = semantic_store.load(&error);
        expect(!loaded && !error.empty() &&
                   read_file_bytes(semantic_store.path()) == invalid &&
                   !has_corrupt_backup(directory),
               L"semantic schema errors are diagnosed without replacing the source");
    }

    {
        const auto directory = root / L"read-error";
        const airshot::ConfigStore read_error_store(directory);
        std::filesystem::create_directories(read_error_store.path());
        std::wstring error;
        const auto loaded = read_error_store.load(&error);
        expect(!loaded && !error.empty() &&
                   std::filesystem::is_directory(read_error_store.path()) &&
                   !has_corrupt_backup(directory),
               L"I/O read errors do not quarantine or replace the source");
    }

    {
        const auto directory = root / L"migration-write-error";
        const airshot::ConfigStore migration_store(directory);
        std::filesystem::create_directories(directory);
        constexpr std::string_view schema_one =
            R"({"schemaVersion":1,"shell":{"enabled":true,"startAtLogin":false}})";
        write_file_bytes(migration_store.path(), schema_one);
        const HANDLE blocker = CreateFileW(
            migration_store.path().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        expect(blocker != INVALID_HANDLE_VALUE, L"migration target blocker is opened");
        std::wstring error;
        const auto loaded = migration_store.load(&error);
        expect(!loaded && !error.empty() &&
                   read_file_bytes(migration_store.path()) == schema_one,
               L"a failed schema migration is reported and leaves schema 1 intact");
        if (blocker != INVALID_HANDLE_VALUE) {
            CloseHandle(blocker);
        }
    }

    {
        const auto directory = root / L"corrupt-rename-error";
        const airshot::ConfigStore corrupt_store(directory);
        std::filesystem::create_directories(directory);
        constexpr std::string_view corrupt = R"({"schemaVersion":2)";
        write_file_bytes(corrupt_store.path(), corrupt);
        const HANDLE blocker = CreateFileW(
            corrupt_store.path().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        expect(blocker != INVALID_HANDLE_VALUE, L"corrupt target blocker is opened");
        std::wstring error;
        const auto loaded = corrupt_store.load(&error);
        expect(!loaded && !error.empty() &&
                   read_file_bytes(corrupt_store.path()) == corrupt &&
                   !has_corrupt_backup(directory),
               L"a failed corrupt-file quarantine is reported without writing defaults");
        if (blocker != INVALID_HANDLE_VALUE) {
            CloseHandle(blocker);
        }
    }

    {
        const auto directory = root / L"corrupt-recovery";
        const airshot::ConfigStore recovery_store(directory);
        std::filesystem::create_directories(directory);
        write_file_bytes(recovery_store.path(), "{not-json");
        std::wstring error;
        const auto loaded = recovery_store.load(&error);
        expect(loaded && error.empty() && !loaded->start_at_login &&
                   has_corrupt_backup(directory) &&
                   airshot::config_from_json(
                       airshot::from_utf8(read_file_bytes(recovery_store.path())))
                       .has_value(),
               L"a truly corrupt file is quarantined only after fresh config creation succeeds");
    }

    {
        const auto directory = root / L"concurrent-save";
        const airshot::ConfigStore concurrent_store(directory);
        std::wstring error;
        const auto initial = concurrent_store.load(&error);
        expect(initial && error.empty(), L"concurrent save fixture is initialized");
        if (initial) {
            airshot::AppConfig first = *initial;
            airshot::AppConfig second = *initial;
            first.preserved_json =
                LR"({"schemaVersion":2,"extensionA":{"value":1}})";
            second.preserved_json =
                LR"({"schemaVersion":2,"extensionB":{"value":2}})";
            first.shell_enabled = false;
            second.start_at_login = true;

            std::barrier start(3);
            bool first_saved = false;
            bool second_saved = false;
            std::thread first_writer([&] {
                start.arrive_and_wait();
                first_saved = concurrent_store.save(first);
            });
            std::thread second_writer([&] {
                start.arrive_and_wait();
                second_saved = concurrent_store.save(second);
            });
            start.arrive_and_wait();
            first_writer.join();
            second_writer.join();

            const std::string final_text = read_file_bytes(concurrent_store.path());
            expect(first_saved && second_saved &&
                       final_text.find("\"extensionA\"") != std::string::npos &&
                       final_text.find("\"extensionB\"") != std::string::npos &&
                       airshot::config_from_json(airshot::from_utf8(final_text)).has_value(),
                   L"concurrent saves serialize and retain both writers' unknown keys");
        }
    }

    {
        const auto blocked_directory = root / L"not-a-directory";
        write_file_bytes(blocked_directory, "occupied");
        const airshot::ConfigStore blocked_store(blocked_directory);
        std::wstring error;
        const auto loaded = blocked_store.load(&error);
        expect(!loaded && !error.empty() && read_file_bytes(blocked_directory) == "occupied",
               L"fresh config creation failure is diagnosed without returning defaults");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int wmain() {
    test_command_contract();
    test_config_contract();
    if (failures == 0) {
        std::wcout << L"command/config contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
