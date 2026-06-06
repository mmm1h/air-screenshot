#pragma once

#include "airshot/common.h"

#include <string>
#include <vector>

namespace airshot::overlay_detail {

enum class Tool {
    none,
    select,
    rectangle,
    ellipse,
    line,
    arrow,
    pen,
    mosaic,
    highlight,
    text,
    serial,
    eraser,
    blur,
    watermark,
};

enum class TextStyle {
    normal,
    dark,
    outline,
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
    COLORREF color{RGB(0, 102, 255)};
    float width{3.0F};
    int alpha{255};
    int serial{};
    TextStyle text_style{TextStyle::normal};
};

struct ToolbarButton {
    std::wstring id;
    std::wstring label;
    RectI bounds;
};

}  // namespace airshot::overlay_detail
