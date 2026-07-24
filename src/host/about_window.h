#pragma once

#include <windows.h>

#include <functional>

namespace airshot {

HWND show_about_window_async(HWND owner, std::function<void()> completion = {});
void show_about_window(HWND owner);

}  // namespace airshot
