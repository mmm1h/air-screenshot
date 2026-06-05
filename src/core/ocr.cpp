#include "airshot/ocr.h"
#include "airshot/output.h"
#include "airshot/config.h"

#include <windows.h>
#include <filesystem>
#include <string>
#include <format>
#include <vector>

namespace airshot {
namespace {

std::wstring get_wechat_install_path() {
    HKEY hKey;
    std::wstring install_path;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Tencent\\WeChat", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[512]{};
        DWORD type = REG_SZ;
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(hKey, L"InstallPath", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS) {
            install_path = buf;
        }
        RegCloseKey(hKey);
    }
    return install_path;
}

std::wstring find_wechat_ocr_exe_dir() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata) {
        std::filesystem::path ocr_root = std::filesystem::path(appdata) / L"Tencent" / L"WeChat" / L"XPlugin" / L"Plugins" / L"ocr";
        if (std::filesystem::exists(ocr_root)) {
            std::filesystem::path best_dir;
            int best_ver = -1;
            for (const auto& entry : std::filesystem::directory_iterator(ocr_root)) {
                if (entry.is_directory()) {
                    std::filesystem::path exe_path = entry.path() / L"WeChatOCR.exe";
                    if (std::filesystem::exists(exe_path)) {
                        try {
                            int ver = std::stoi(entry.path().filename().wstring());
                            if (ver > best_ver) {
                                best_ver = ver;
                                best_dir = entry.path();
                            }
                        } catch (...) {
                            if (best_ver == -1) {
                                best_dir = entry.path();
                            }
                        }
                    }
                }
            }
            return best_dir.wstring();
        }
    }
    return L"";
}

OcrOutput run_ocr_process(const std::wstring& cmd_line) {
    HANDLE h_child_stdout_rd = nullptr;
    HANDLE h_child_stdout_wr = nullptr;
    
    SECURITY_ATTRIBUTES sa_attr;
    sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr.bInheritHandle = TRUE;
    sa_attr.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&h_child_stdout_rd, &h_child_stdout_wr, &sa_attr, 0)) {
        return {false, {}, L"创建 IPC 管道失败。"};
    }

    if (!SetHandleInformation(h_child_stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(h_child_stdout_rd);
        CloseHandle(h_child_stdout_wr);
        return {false, {}, L"设置管道句柄继承失败。"};
    }

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = h_child_stdout_wr;
    si.hStdError = h_child_stdout_wr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::wstring cmd_copy = cmd_line;

    BOOL success = CreateProcessW(
        nullptr,
        cmd_copy.data(),
        nullptr,
        nullptr,
        TRUE, // Inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    // Close the write end of the pipe immediately so parent's ReadFile doesn't hang
    CloseHandle(h_child_stdout_wr);

    if (!success) {
        CloseHandle(h_child_stdout_rd);
        return {false, {}, L"无法启动 airshot_ocr.exe 辅助进程。"};
    }

    std::vector<char> buffer;
    char temp_buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(h_child_stdout_rd, temp_buf, sizeof(temp_buf), &bytes_read, nullptr) && bytes_read > 0) {
        buffer.insert(buffer.end(), temp_buf, temp_buf + bytes_read);
    }

    CloseHandle(h_child_stdout_rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0) {
        std::wstring err_msg(reinterpret_cast<const wchar_t*>(buffer.data()), buffer.size() / sizeof(wchar_t));
        return {false, {}, err_msg.empty() ? L"OCR 子进程执行失败。" : err_msg};
    }

    std::wstring result_text(reinterpret_cast<const wchar_t*>(buffer.data()), buffer.size() / sizeof(wchar_t));
    return {true, result_text, {}};
}

}  // namespace

std::wstring join_ocr_lines(std::span<const std::wstring> lines) {
    std::wstring result;
    for (const auto& line : lines) {
        if (!result.empty()) {
            result += L"\r\n";
        }
        result += line;
    }
    return result;
}

OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config) {
    if (bitmap.empty()) {
        return {false, {}, L"OCR 图像为空。"};
    }

    // Save temporary image
    std::filesystem::path temp_png = config_directory() / std::format(L"ocr_temp_{}_{}.png", GetCurrentProcessId(), GetCurrentThreadId());
    std::wstring save_error;
    if (!save_png(bitmap, temp_png, &save_error)) {
        return {false, {}, L"无法保存临时 OCR 选区图像: " + save_error};
    }

    // Locate helper executable
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
    std::filesystem::path ocr_exe = exe_dir / L"airshot_ocr.exe";

    if (!std::filesystem::exists(ocr_exe)) {
        std::filesystem::remove(temp_png);
        return {false, {}, L"未找到 airshot_ocr.exe，请重新编译或安装程序。"};
    }

    // Construct command line
    std::wstring cmd_line = L"\"" + ocr_exe.wstring() + L"\"";
    if (config.ocr_engine == 0) {
        // WinRT System
        cmd_line += L" --engine winrt --image \"" + temp_png.wstring() + L"\"";
    } else if (config.ocr_engine == 1) {
        // WeChat OCR
        std::wstring wechat_dir = get_wechat_install_path();
        std::wstring ocr_dir = find_wechat_ocr_exe_dir();
        cmd_line += L" --engine wechat --image \"" + temp_png.wstring() + L"\"";
        cmd_line += L" --wechat-dir \"" + wechat_dir + L"\" --ocr-dir \"" + ocr_dir + L"\"";
    } else if (config.ocr_engine == 2) {
        // Local ONNX OCR
        std::filesystem::path model_dir = config_directory() / L"ocr_onnx" / L"models";
        cmd_line += L" --engine onnx --image \"" + temp_png.wstring() + L"\"";
        cmd_line += L" --model-dir \"" + model_dir.wstring() + L"\"";
    } else {
        std::filesystem::remove(temp_png);
        return {false, {}, L"未知的 OCR 引擎配置。"};
    }

    // Execute and capture output
    OcrOutput result = run_ocr_process(cmd_line);

    // Clean up temporary image
    std::error_code ignored_ec;
    std::filesystem::remove(temp_png, ignored_ec);

    return result;
}

}  // namespace airshot
