cmake_minimum_required(VERSION 3.25)

project(AirScreenshot VERSION 0.1.5.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Air Screenshot only supports Windows.")
endif()

set(AIRSHOT_BUILD_VERSION "" CACHE STRING "Version reported by the Air Screenshot binaries")
if(AIRSHOT_BUILD_VERSION STREQUAL "")
    set(AIRSHOT_BUILD_VERSION "${PROJECT_VERSION}")
endif()

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
)

target_include_directories(airshot_core PUBLIC include)
target_compile_definitions(airshot_core PUBLIC
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    AIRSHOT_VERSION="${AIRSHOT_BUILD_VERSION}"
)
target_compile_options(airshot_core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(airshot_core PUBLIC
    d2d1
    dwrite
    dwmapi
    ole32
    shell32
    shlwapi
    user32
    windowsapp
    windowscodecs
)

add_executable(AirScreenshot WIN32
    src/host/host_app.cpp
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

add_executable(airshot src/cli/main.cpp)
target_compile_options(airshot PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(airshot PRIVATE airshot_core)

if(BUILD_TESTING)
    add_executable(airshot_tests tests/test_main.cpp)
    target_compile_options(airshot_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(airshot_tests PRIVATE airshot_core)
    add_test(NAME airshot_tests COMMAND airshot_tests)
endif()

install(TARGETS AirScreenshot airshot RUNTIME DESTINATION .)
