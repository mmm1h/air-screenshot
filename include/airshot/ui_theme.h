#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <string_view>

namespace airshot {

struct UiPalette {
    bool light{};
    bool high_contrast{};
    D2D1_COLOR_F background{};
    D2D1_COLOR_F sidebar{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F muted{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accent_hover{};
    D2D1_COLOR_F accent_text{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F active{};
    D2D1_COLOR_F control{};
    D2D1_COLOR_F switch_off{};
    D2D1_COLOR_F card{};
    D2D1_COLOR_F card_border{};
    D2D1_COLOR_F separator{};
    D2D1_COLOR_F hover{};
    D2D1_COLOR_F cancel_border{};
    D2D1_COLOR_F accent_indicator{};
    D2D1_COLOR_F keycap_shadow{};
    D2D1_COLOR_F switch_glow{};
    D2D1_COLOR_F danger_surface{};
    D2D1_COLOR_F danger{};
};

[[nodiscard]] UiPalette resolve_ui_palette(
    std::wstring_view theme_preference);

[[nodiscard]] std::wstring preferred_ui_font_family(
    IDWriteFactory* factory);

}  // namespace airshot
