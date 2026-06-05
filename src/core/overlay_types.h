#pragma once

#include "airshot/common.h"

#include <string>
#include <vector>

namespace airshot::overlay_detail {

enum class Tool {
    none,
    rectangle,
    arrow,
    mosaic,
    text,
};

enum class DragMode {
    none,
    move,
    top_left,
    top,
    top_right,
    right,
    bottom_right,
    bottom,
    bottom_left,
    left,
    annotate
};

struct Annotation {
    Tool tool{Tool::none};
    POINT start{};
    POINT end{};
    std::vector<POINT> points;
    std::wstring text;
    COLORREF color{RGB(22, 119, 255)};
    float width{3.0F};
};

struct ToolbarButton {
    std::wstring id;
    std::wstring label;
    RectI bounds;
};

}  // namespace airshot::overlay_detail
