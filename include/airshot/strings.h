#pragma once

#include <string_view>

namespace airshot::strings {

inline constexpr std::wstring_view tray_capture = L"区域截图";
inline constexpr std::wstring_view tray_settings = L"设置";
inline constexpr std::wstring_view tray_update = L"检查更新";
inline constexpr std::wstring_view tray_exit = L"退出";

inline constexpr std::wstring_view settings_title = L"Air Screenshot 设置";
inline constexpr std::wstring_view settings_annotation = L"启用轻量标注模块";
inline constexpr std::wstring_view settings_ocr = L"启用 OCR 识别";
inline constexpr std::wstring_view settings_shell = L"启用后台服务与全局快捷键";
inline constexpr std::wstring_view settings_tray_icon = L"显示系统托盘图标";
inline constexpr std::wstring_view settings_startup = L"登录 Windows 后自动启动";
inline constexpr std::wstring_view settings_global_ocr = L"启用全局 OCR 快捷键";
inline constexpr std::wstring_view settings_capture_hotkey = L"普通截图快捷键";
inline constexpr std::wstring_view settings_global_ocr_hotkey = L"全局 OCR 快捷键";
inline constexpr std::wstring_view settings_note = L"截图完成后可按 Shift+C 直接 OCR 并复制。全局 OCR 默认关闭。";
inline constexpr std::wstring_view settings_save = L"保存";
inline constexpr std::wstring_view settings_cancel = L"取消";
inline constexpr std::wstring_view settings_invalid_hotkey = L"快捷键格式无效，例如 Ctrl+Alt+A。";

inline constexpr std::wstring_view prompt_text_title = L"输入标注文字";
inline constexpr std::wstring_view common_confirm = L"确定";
inline constexpr std::wstring_view common_cancel = L"取消";

inline constexpr std::wstring_view toolbar_rectangle = L"框";
inline constexpr std::wstring_view toolbar_ellipse = L"圆";
inline constexpr std::wstring_view toolbar_line = L"线";
inline constexpr std::wstring_view toolbar_arrow = L"箭";
inline constexpr std::wstring_view toolbar_pen = L"笔";
inline constexpr std::wstring_view toolbar_mosaic = L"糊";
inline constexpr std::wstring_view toolbar_highlight = L"亮";
inline constexpr std::wstring_view toolbar_text = L"字";
inline constexpr std::wstring_view toolbar_serial = L"号";
inline constexpr std::wstring_view toolbar_eraser = L"擦";
inline constexpr std::wstring_view toolbar_lock = L"连";
inline constexpr std::wstring_view toolbar_undo = L"撤";
inline constexpr std::wstring_view toolbar_redo = L"重";
inline constexpr std::wstring_view toolbar_ocr = L"识";
inline constexpr std::wstring_view toolbar_copy = L"复";
inline constexpr std::wstring_view toolbar_save = L"存";
inline constexpr std::wstring_view toolbar_close = L"关";

inline constexpr std::wstring_view ocr_success = L"OCR 文本已复制";
inline constexpr std::wstring_view capture_failed = L"截图失败，受保护内容或硬件覆盖层可能无法捕获";
inline constexpr std::wstring_view hotkey_conflict = L"全局快捷键注册失败，可能已被其他程序占用";

}  // namespace airshot::strings
