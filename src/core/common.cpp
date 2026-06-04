#include "airshot/common.h"

#include <roapi.h>

#include <iomanip>
#include <sstream>

#include <winrt/base.h>

namespace airshot {

ScopedWinrtApartment::ScopedWinrtApartment(bool single_threaded) noexcept
    : result_(RoInitialize(single_threaded ? RO_INIT_SINGLETHREADED : RO_INIT_MULTITHREADED)) {}

ScopedWinrtApartment::~ScopedWinrtApartment() {
    if (SUCCEEDED(result_)) {
        winrt::clear_factory_cache();
        RoUninitialize();
    }
}

bool ScopedWinrtApartment::available() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        size,
                        nullptr,
                        nullptr);
    return result;
}

std::wstring from_utf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring windows_error_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr,
                                      error,
                                      0,
                                      reinterpret_cast<wchar_t*>(&buffer),
                                      0,
                                      nullptr);
    std::wstring result = size > 0 && buffer ? std::wstring(buffer, size) : std::format(L"Windows error {}", error);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::wstring timestamp_for_file() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}",
                       time.wYear,
                       time.wMonth,
                       time.wDay,
                       time.wHour,
                       time.wMinute,
                       time.wSecond,
                       time.wMilliseconds);
}

}  // namespace airshot
