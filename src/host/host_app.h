#pragma once

#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/feature.h"
#include "airshot/ipc.h"
#include "airshot/bitmap.h"
#include "airshot/overlay.h"

#include <shellapi.h>

#include <winrt/Windows.Data.Json.h>

namespace airshot {

class HostApp {
public:
    HostApp(HINSTANCE instance, bool transient);
    ~HostApp();

    int run();

private:
    struct RequestContext {
        std::wstring request;
        std::wstring response;
    };

    bool initialize();
    void shutdown();
    void apply_shell();
    void sync_startup_task();
    void add_tray();
    void remove_tray();
    void register_hotkeys();
    void unregister_hotkeys();
    void show_tray_menu();
    void show_settings();
    void notify(std::wstring_view title, std::wstring_view message);
    void capture_region(RegionAction action);
    CommandResponse execute_request(std::wstring_view request_json);
    CommandResponse execute_capture(const winrt::Windows::Data::Json::JsonObject& request);
    CommandResponse execute_ocr(const winrt::Windows::Data::Json::JsonObject& request);
    CommandResponse execute_module(const winrt::Windows::Data::Json::JsonObject& request);
    CommandResponse execute_app(const winrt::Windows::Data::Json::JsonObject& request);
    CommandResponse output_bitmap(
        Bitmap bitmap, std::wstring_view output, std::wstring_view requested_path, std::wstring_view success_message);

    LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    HINSTANCE instance_{};
    HWND window_{};
    bool transient_{};
    bool tray_added_{};
    bool shutting_down_{};
    HANDLE mutex_{};
    AppConfig config_;
    FeatureRegistry features_;
    PipeServer pipe_server_;
    NOTIFYICONDATAW tray_{};
};

}  // namespace airshot
