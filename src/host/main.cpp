#include "host_app.h"

#include <shellscalingapi.h>

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command_line, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool transient = command_line && wcsstr(command_line, L"--transient") != nullptr;
    airshot::HostApp app(instance, transient);
    return app.run();
}
