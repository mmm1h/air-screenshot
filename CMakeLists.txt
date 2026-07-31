cmake_minimum_required(VERSION 3.25)

function(airshot_validate_version version_value subject)
    if(NOT "${version_value}" MATCHES
       "^(0|[1-9][0-9]?[0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?)$")
        message(FATAL_ERROR "${subject} must use canonical X.Y.Z format without leading zeros.")
    endif()

    string(REPLACE "." ";" AIRSHOT_VALIDATED_VERSION_PARTS "${version_value}")
    list(GET AIRSHOT_VALIDATED_VERSION_PARTS 0 AIRSHOT_VALIDATED_VERSION_MAJOR)
    list(GET AIRSHOT_VALIDATED_VERSION_PARTS 1 AIRSHOT_VALIDATED_VERSION_MINOR)
    list(GET AIRSHOT_VALIDATED_VERSION_PARTS 2 AIRSHOT_VALIDATED_VERSION_PATCH)
    string(LENGTH "${AIRSHOT_VALIDATED_VERSION_MAJOR}" AIRSHOT_VALIDATED_VERSION_MAJOR_LENGTH)
    string(LENGTH "${AIRSHOT_VALIDATED_VERSION_MINOR}" AIRSHOT_VALIDATED_VERSION_MINOR_LENGTH)
    string(LENGTH "${AIRSHOT_VALIDATED_VERSION_PATCH}" AIRSHOT_VALIDATED_VERSION_PATCH_LENGTH)

    if(AIRSHOT_VALIDATED_VERSION_MAJOR_LENGTH GREATER 4 OR
       AIRSHOT_VALIDATED_VERSION_MINOR_LENGTH GREATER 5 OR
       AIRSHOT_VALIDATED_VERSION_PATCH_LENGTH GREATER 5 OR
       AIRSHOT_VALIDATED_VERSION_MAJOR GREATER 9000 OR
       AIRSHOT_VALIDATED_VERSION_MINOR GREATER 65535 OR
       AIRSHOT_VALIDATED_VERSION_PATCH GREATER 65535)
        message(FATAL_ERROR
            "${subject} components must satisfy major <= 9000 and minor/patch <= 65535.")
    endif()
    if(AIRSHOT_VALIDATED_VERSION_MAJOR EQUAL 0 AND
       AIRSHOT_VALIDATED_VERSION_MINOR EQUAL 0 AND
       AIRSHOT_VALIDATED_VERSION_PATCH EQUAL 0)
        message(FATAL_ERROR "${subject} must be greater than 0.0.0.")
    endif()
endfunction()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AIRSHOT_PROJECT_VERSION)
string(STRIP "${AIRSHOT_PROJECT_VERSION}" AIRSHOT_PROJECT_VERSION)
airshot_validate_version("${AIRSHOT_PROJECT_VERSION}" "VERSION")

project(AirScreenshot VERSION "${AIRSHOT_PROJECT_VERSION}" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Air Screenshot only supports Windows.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Air Screenshot requires the Microsoft Visual C++ toolchain.")
endif()
if(MSVC_VERSION LESS 1938)
    message(FATAL_ERROR "Air Screenshot requires Visual Studio 2022 17.8 or newer.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Air Screenshot only supports the x64 target architecture.")
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "
    #include <sdkddkver.h>
    #ifndef NTDDI_WIN10_VB
    #error Windows SDK 10.0.19041 or newer is required.
    #endif
    static_assert(NTDDI_WIN10_VB == 0x0A000008);
    int main() { return 0; }
    "
    AIRSHOT_HAS_WINDOWS_10_2004_SDK
)
if(NOT AIRSHOT_HAS_WINDOWS_10_2004_SDK)
    message(FATAL_ERROR "Air Screenshot requires Windows SDK 10.0.19041 or newer.")
endif()

set(AIRSHOT_BUILD_VERSION "" CACHE STRING "Version reported by the Air Screenshot binaries")
if(AIRSHOT_BUILD_VERSION STREQUAL "")
    set(AIRSHOT_BUILD_VERSION "${PROJECT_VERSION}")
endif()
airshot_validate_version("${AIRSHOT_BUILD_VERSION}" "AIRSHOT_BUILD_VERSION")
string(REPLACE "." ";" AIRSHOT_BUILD_VERSION_PARTS "${AIRSHOT_BUILD_VERSION}")
list(GET AIRSHOT_BUILD_VERSION_PARTS 0 AIRSHOT_VERSION_MAJOR)
list(GET AIRSHOT_BUILD_VERSION_PARTS 1 AIRSHOT_VERSION_MINOR)
list(GET AIRSHOT_BUILD_VERSION_PARTS 2 AIRSHOT_VERSION_PATCH)

set(AIRSHOT_OCR_MIN_SEQUENCE "" CACHE STRING "Minimum trusted OCR manifest sequence")
if(AIRSHOT_OCR_MIN_SEQUENCE STREQUAL "")
    set(AIRSHOT_OCR_MIN_SEQUENCE_IS_DEFAULT 1)
    set(AIRSHOT_OCR_SEQUENCE_MAJOR "${AIRSHOT_VERSION_MAJOR}")
    set(AIRSHOT_OCR_SEQUENCE_MINOR "${AIRSHOT_VERSION_MINOR}")
    set(AIRSHOT_OCR_SEQUENCE_PATCH "${AIRSHOT_VERSION_PATCH}")
    if(AIRSHOT_OCR_SEQUENCE_MAJOR GREATER 9000 OR
       AIRSHOT_OCR_SEQUENCE_MINOR GREATER 65535 OR
       AIRSHOT_OCR_SEQUENCE_PATCH GREATER 65535)
        message(FATAL_ERROR "AIRSHOT_BUILD_VERSION is too large for an exact OCR sequence.")
    endif()
    math(EXPR AIRSHOT_OCR_MIN_SEQUENCE
        "${AIRSHOT_OCR_SEQUENCE_MAJOR} * 1000000000000 + ${AIRSHOT_OCR_SEQUENCE_MINOR} * 1000000 + ${AIRSHOT_OCR_SEQUENCE_PATCH}")
else()
    set(AIRSHOT_OCR_MIN_SEQUENCE_IS_DEFAULT 0)
endif()
if(NOT AIRSHOT_OCR_MIN_SEQUENCE MATCHES "^[0-9]+$" OR
   AIRSHOT_OCR_MIN_SEQUENCE LESS 1 OR
   AIRSHOT_OCR_MIN_SEQUENCE GREATER 9007199254740991)
    message(FATAL_ERROR "AIRSHOT_OCR_MIN_SEQUENCE must be an integer in 1..2^53-1.")
endif()

set(AIRSHOT_OCR_MANIFEST_KEY_ID "" CACHE STRING "Trusted OCR manifest signing key identifier")
set(AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX "" CACHE STRING "ECDSA P-256 OCR manifest public key (x || y)")
if((AIRSHOT_OCR_MANIFEST_KEY_ID STREQUAL "" AND
    NOT AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX STREQUAL "") OR
   (NOT AIRSHOT_OCR_MANIFEST_KEY_ID STREQUAL "" AND
    AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX STREQUAL ""))
    message(FATAL_ERROR "OCR manifest key id and public key must be configured together.")
endif()
if(NOT AIRSHOT_OCR_MANIFEST_KEY_ID STREQUAL "" AND
   NOT AIRSHOT_OCR_MANIFEST_KEY_ID MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "AIRSHOT_OCR_MANIFEST_KEY_ID contains unsupported characters.")
endif()
string(LENGTH "${AIRSHOT_OCR_MANIFEST_KEY_ID}" AIRSHOT_OCR_MANIFEST_KEY_ID_LENGTH)
if(AIRSHOT_OCR_MANIFEST_KEY_ID_LENGTH GREATER 64)
    message(FATAL_ERROR "AIRSHOT_OCR_MANIFEST_KEY_ID cannot exceed 64 characters.")
endif()
if(NOT AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX STREQUAL "" AND
   NOT AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX MATCHES "^[A-Fa-f0-9]+$")
    message(FATAL_ERROR "AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX must contain 128 hexadecimal characters.")
endif()
string(LENGTH "${AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX}" AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_LENGTH)
if(NOT AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX STREQUAL "" AND
   NOT AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_LENGTH EQUAL 128)
    message(FATAL_ERROR "AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX must contain 128 hexadecimal characters.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(CTest)

function(airshot_apply_compile_policy target)
    target_compile_options(${target} PRIVATE
        /W4
        /WX
        /permissive-
        /utf-8
        /sdl
        /guard:cf
        "$<$<CONFIG:Release>:/Zi>"
    )
endfunction()

function(airshot_apply_executable_policy target)
    airshot_apply_compile_policy(${target})
    target_link_options(${target} PRIVATE
        /guard:cf
        "$<$<CONFIG:Release>:/DEBUG:FULL>"
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
    )
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
    )
endfunction()

add_library(airshot_core STATIC
    src/core/bitmap.cpp
    src/core/capture.cpp
    src/core/capture_modern.cpp
    src/core/clipboard.cpp
    src/core/command.cpp
    src/core/common.cpp
    src/core/config.cpp
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
    src/core/pin_layout.cpp
    src/core/portable.cpp
    src/core/ui_theme.cpp
    src/core/update_policy.cpp
)

target_include_directories(airshot_core PUBLIC include)
target_compile_definitions(airshot_core PUBLIC
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000008
    _CRT_SECURE_NO_WARNINGS
    AIRSHOT_VERSION="${AIRSHOT_BUILD_VERSION}"
    AIRSHOT_VERSION_MAJOR=${AIRSHOT_VERSION_MAJOR}
    AIRSHOT_VERSION_MINOR=${AIRSHOT_VERSION_MINOR}
    AIRSHOT_VERSION_PATCH=${AIRSHOT_VERSION_PATCH}
    AIRSHOT_OCR_MIN_SEQUENCE=${AIRSHOT_OCR_MIN_SEQUENCE}
    AIRSHOT_OCR_MIN_SEQUENCE_IS_DEFAULT=${AIRSHOT_OCR_MIN_SEQUENCE_IS_DEFAULT}
    AIRSHOT_OCR_MANIFEST_KEY_ID="${AIRSHOT_OCR_MANIFEST_KEY_ID}"
    AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX="${AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX}"
)
airshot_apply_compile_policy(airshot_core)
target_link_libraries(airshot_core PUBLIC
    advapi32
    bcrypt
    crypt32
    d2d1
    d3d11
    dwrite
    dwmapi
    dxgi
    imm32
    ole32
    shell32
    shlwapi
    user32
    version
    windowsapp
    windowscodecs
    winhttp
    wintrust
)

set(AIRSHOT_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${AIRSHOT_GENERATED_DIRECTORY}")
set(AIRSHOT_MANIFEST_VERSION "${AIRSHOT_BUILD_VERSION}.0")
configure_file(
    src/host/AirScreenshot.exe.manifest.in
    "${AIRSHOT_GENERATED_DIRECTORY}/AirScreenshot.exe.manifest"
    @ONLY
)
set_property(
    SOURCE src/host/resources.rc
    APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/host/app.ico"
)

add_executable(AirScreenshot WIN32
    src/host/about_window.cpp
    src/host/app_icon.cpp
    src/host/host_app.cpp
    src/host/cli_app.cpp
    src/host/main.cpp
    src/host/settings_window_v2.cpp
    src/host/pin_window.cpp
    src/core/ocr_cli_main.cpp
    src/host/resources.rc
    "${AIRSHOT_GENERATED_DIRECTORY}/AirScreenshot.exe.manifest"
)
target_include_directories(AirScreenshot PRIVATE src/host)
airshot_apply_executable_policy(AirScreenshot)
target_link_libraries(AirScreenshot PRIVATE airshot_core comctl32)

if(BUILD_TESTING)
    function(airshot_add_test target source)
        add_executable(${target} ${source})
        airshot_apply_executable_policy(${target})
        target_link_libraries(${target} PRIVATE airshot_core)
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    airshot_add_test(airshot_tests tests/test_main.cpp)
    airshot_add_test(airshot_clipboard_tests tests/test_clipboard.cpp)
    set_tests_properties(airshot_clipboard_tests PROPERTIES SKIP_RETURN_CODE 77)
    airshot_add_test(airshot_capture_tests tests/test_capture.cpp)
    target_include_directories(airshot_capture_tests PRIVATE src/core)
    set_tests_properties(airshot_capture_tests PROPERTIES SKIP_RETURN_CODE 77)
    airshot_add_test(airshot_command_config_tests tests/test_command_config.cpp)
    airshot_add_test(airshot_image_pipeline_tests tests/test_image_pipeline.cpp)
    target_include_directories(airshot_image_pipeline_tests PRIVATE src/core)
    target_link_libraries(airshot_image_pipeline_tests PRIVATE comctl32)
    airshot_add_test(airshot_ipc_tests tests/test_ipc.cpp)
    airshot_add_test(airshot_ocr_tests tests/test_ocr.cpp)
    target_include_directories(airshot_ocr_tests PRIVATE src/core)
    airshot_add_test(airshot_portable_tests tests/test_portable.cpp)
    target_include_directories(airshot_portable_tests PRIVATE src/core)
    airshot_add_test(airshot_scroll_tests tests/test_scroll.cpp)
    target_link_libraries(airshot_scroll_tests PRIVATE comctl32)
    airshot_add_test(airshot_update_policy_tests tests/test_update_policy.cpp)

    add_executable(airshot_settings_preview WIN32
        tests/settings_preview.cpp
        src/host/app_icon.cpp
        src/host/settings_window_v2.cpp
        src/host/resources.rc
    )
    target_include_directories(airshot_settings_preview PRIVATE src/host)
    airshot_apply_executable_policy(airshot_settings_preview)
    target_link_libraries(airshot_settings_preview PRIVATE airshot_core comctl32)
endif()

install(TARGETS AirScreenshot RUNTIME DESTINATION .)
install(FILES "$<TARGET_PDB_FILE:AirScreenshot>" DESTINATION symbols OPTIONAL)
