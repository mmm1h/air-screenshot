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
    expect(fresh.schema_version == airshot::kCurrentConfigSchemaVersion && !fresh.start_at_login,
           L"fresh config uses schema 2 with startup disabled");

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
        const auto serialized = airshot::config_to_json(edited);
        expect(serialized.find(LR"("extension":{"mode":"future"})") != std::wstring::npos &&
                   serialized.find(LR"("futureFlag":7)") != std::wstring::npos,
               L"same-schema unknown keys survive a save");
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
               LR"({"schemaVersion":2,"capture":{"theme":"sepia"}})"),
           L"schema 2 rejects an invalid known enum");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"annotation":{"hiddenTools":["pen",7]}})"),
           L"schema 2 rejects a malformed known string array");

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
