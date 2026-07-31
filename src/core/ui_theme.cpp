#include "airshot/ui_theme.h"

#include "airshot/config.h"

#include <windows.h>
#include <wrl/client.h>

#include <array>

namespace airshot {
namespace {

D2D1_COLOR_F color(unsigned int rgb, float alpha = 1.0F) {
    return D2D1::ColorF(rgb, alpha);
}

D2D1_COLOR_F system_color(int index) {
    const COLORREF value = GetSysColor(index);
    return D2D1::ColorF(
        GetRValue(value) / 255.0F,
        GetGValue(value) / 255.0F,
        GetBValue(value) / 255.0F,
        1.0F);
}

bool is_light_color(const D2D1_COLOR_F& value) {
    const float luminance =
        0.2126F * value.r + 0.7152F * value.g + 0.0722F * value.b;
    return luminance >= 0.5F;
}

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{static_cast<UINT>(sizeof(contrast))};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST,
               sizeof(contrast),
               &contrast,
               0) != FALSE &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

UiPalette high_contrast_palette() {
    const D2D1_COLOR_F background = system_color(COLOR_WINDOW);
    const D2D1_COLOR_F text = system_color(COLOR_WINDOWTEXT);
    const D2D1_COLOR_F accent = system_color(COLOR_HIGHLIGHT);
    const D2D1_COLOR_F accent_text = system_color(COLOR_HIGHLIGHTTEXT);
    const D2D1_COLOR_F muted = system_color(COLOR_GRAYTEXT);
    const D2D1_COLOR_F border = system_color(COLOR_WINDOWFRAME);
    const D2D1_COLOR_F control = system_color(COLOR_BTNFACE);
    const D2D1_COLOR_F shadow = system_color(COLOR_BTNSHADOW);
    return UiPalette{
        .light = is_light_color(background),
        .high_contrast = true,
        .background = background,
        .sidebar = background,
        .text = text,
        .muted = muted,
        .accent = accent,
        .accent_hover = accent,
        .accent_text = accent_text,
        .border = border,
        .active = accent,
        .control = control,
        .switch_off = shadow,
        .card = background,
        .card_border = border,
        .separator = border,
        .hover = control,
        .cancel_border = border,
        .accent_indicator = accent,
        .keycap_shadow = shadow,
        .switch_glow = accent,
        .danger_surface = control,
        .danger = text,
    };
}

}  // namespace

UiPalette resolve_ui_palette(std::wstring_view theme_preference) {
    if (high_contrast_enabled()) {
        return high_contrast_palette();
    }

    if (should_use_light_theme(theme_preference)) {
        return UiPalette{
            .light = true,
            .background = color(0xF2F3F5),
            .sidebar = color(0xF2F3F5),
            .text = color(0x1F2329),
            .muted = color(0x646A73),
            .accent = color(0x0066FF),
            .accent_hover = color(0x3385FF),
            .accent_text = color(0xFFFFFF),
            .border = color(0xDEE0E3),
            .active = color(0xE5E7EB),
            .control = color(0xFFFFFF),
            .switch_off = color(0xD2D6DC),
            .card = color(0xFFFFFF),
            .card_border = color(0xE5E7EB),
            .separator = color(0xF0F1F3),
            .hover = color(0xF5F7FA),
            .cancel_border = color(0xD2D6DC),
            .accent_indicator = color(0x0066FF),
            .keycap_shadow = color(0xD0D3D8),
            .switch_glow = color(0x0066FF, 0.15F),
            .danger_surface = color(0xFFEAEA),
            .danger = color(0xE81123),
        };
    }

    return UiPalette{
        .light = false,
        .background = color(0x16171C),
        .sidebar = color(0x16171C),
        .text = color(0xF0F0F0),
        .muted = color(0x96A0AF),
        .accent = color(0x0066FF),
        .accent_hover = color(0x3388FF),
        .accent_text = color(0xFFFFFF),
        .border = color(0x2D3038),
        .active = color(0x23252C),
        .control = color(0x1C1E22),
        .switch_off = color(0x4C525D),
        .card = color(0x1E2026),
        .card_border = color(0x2D3038),
        .separator = color(0x282B32),
        .hover = color(0x262930),
        .cancel_border = color(0x3C4048),
        .accent_indicator = color(0x3388FF),
        .keycap_shadow = color(0x14161A),
        .switch_glow = color(0x0066FF, 0.25F),
        .danger_surface = color(0x401515),
        .danger = color(0xE81123),
    };
}

std::wstring preferred_ui_font_family(IDWriteFactory* factory) {
    if (!factory) {
        return L"Segoe UI";
    }

    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(collection.GetAddressOf()))) {
        return L"Segoe UI";
    }

    constexpr std::array candidates{
        std::wstring_view(L"Segoe UI Variable Text"),
        std::wstring_view(L"Segoe UI"),
    };
    for (const auto candidate : candidates) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        const std::wstring name(candidate);
        if (SUCCEEDED(collection->FindFamilyName(
                name.c_str(), &index, &exists)) &&
            exists) {
            return name;
        }
    }
    return L"Segoe UI";
}

}  // namespace airshot
