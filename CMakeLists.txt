cmake_minimum_required(VERSION 3.25)

project(AirScreenshot VERSION 0.2.2 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Air Screenshot only supports Windows.")
endif()

set(AIRSHOT_BUILD_VERSION "" CACHE STRING "Version reported by the Air Screenshot binaries")
if(AIRSHOT_BUILD_VERSION STREQUAL "")
    set(AIRSHOT_BUILD_VERSION "${PROJECT_VERSION}")
endif()
if(NOT AIRSHOT_BUILD_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "AIRSHOT_BUILD_VERSION must use X.Y.Z format.")
endif()
string(REPLACE "." ";" AIRSHOT_BUILD_VERSION_PARTS "${AIRSHOT_BUILD_VERSION}")
list(GET AIRSHOT_BUILD_VERSION_PARTS 0 AIRSHOT_VERSION_MAJOR)
list(GET AIRSHOT_BUILD_VERSION_PARTS 1 AIRSHOT_VERSION_MINOR)
list(GET AIRSHOT_BUILD_VERSION_PARTS 2 AIRSHOT_VERSION_PATCH)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(CTest)

add_library(airshot_core STATIC
    src/core/bitmap.cpp
    src/core/capture.cpp
    src/core/command.cpp
    src/core/common.cpp
    src/core/config.cpp
    src/core/feature.cpp
    src/core/ipc.cpp
    src/core/ocr.cpp
    src/core/output.cpp
    src/core/overlay.cpp
    src/core/overlay_color_picker.cpp
    src/core/overlay_prompt.cpp
    src/core/overlay_scroll.cpp
    src/core/overlay_scroll_windows.cpp
    src/core/overlay_session_actions.cpp
    src/core/overlay_session_interaction.cpp
    src/core/overlay_window.cpp
    src/core/portable.cpp
)

target_include_directories(airshot_core PUBLIC include)
target_compile_definitions(airshot_core PUBLIC
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    AIRSHOT_VERSION="${AIRSHOT_BUILD_VERSION}"
    AIRSHOT_VERSION_MAJOR=${AIRSHOT_VERSION_MAJOR}
    AIRSHOT_VERSION_MINOR=${AIRSHOT_VERSION_MINOR}
    AIRSHOT_VERSION_PATCH=${AIRSHOT_VERSION_PATCH}
)
target_compile_options(airshot_core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(airshot_core PUBLIC
    bcrypt
    crypt32
    d2d1
    dwrite
    dwmapi
    imm32
    ole32
    shell32
    shlwapi
    user32
    urlmon
    version
    windowsapp
    windowscodecs
    wintrust
)

add_executable(AirScreenshot WIN32
    src/host/about_window.cpp
    src/host/host_app.cpp
    src/host/cli_app.cpp
    src/host/main.cpp
    src/host/settings_window.cpp
    src/host/pin_window.cpp
    src/host/resources.rc
    src/host/AirScreenshot.exe.manifest
)
target_include_directories(AirScreenshot PRIVATE src/host)
target_compile_options(AirScreenshot PRIVATE /W4 /permissive- /utf-8 "$<$<CONFIG:Release>:/Zi>")
target_link_options(AirScreenshot PRIVATE "$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
target_link_libraries(AirScreenshot PRIVATE airshot_core comctl32)

add_executable(airshot_ocr
    src/core/ocr_cli_main.cpp
)
target_compile_options(airshot_ocr PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(airshot_ocr PRIVATE airshot_core)

if(BUILD_TESTING)
    add_executable(airshot_tests tests/test_main.cpp)
    target_compile_options(airshot_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(airshot_tests PRIVATE airshot_core)
    add_test(NAME airshot_tests COMMAND airshot_tests)
endif()

install(TARGETS AirScreenshot airshot_ocr RUNTIME DESTINATION .)
