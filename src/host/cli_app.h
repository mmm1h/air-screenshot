#pragma once

#include <span>
#include <string>
#include <vector>

namespace airshot {

int run_cli(std::span<const std::wstring> arguments);

}  // namespace airshot
