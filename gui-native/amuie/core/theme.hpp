#pragma once
#include "core/types.hpp"
#include <algorithm>
#include <cstdint>

// Design tokens. All widgets resolve their colors/spacing/radii from here;
// switching themes is a pointer swap on UIContext.
struct Theme {
    Color bg_app;
    Color bg_header;
    Color bg_toolbar;
    Color bg_panel;
    Color bg_raised;
    Color bg_control;
    Color bg_control_hover;
    Color border;
    Color border_strong;

    Color accent;
    Color accent_hover;
    Color accent_pressed;
    Color accent_dim;
    Color on_accent;

    Color text;
    Color text_muted;
    Color text_dim;
    Color danger;
    Color success;

    Color bg_window;  // desktop/clear color outside the rounded window frame

    float radius_sm;
    float radius_md;
    float radius_lg;
    float space_xs;
    float space_sm;
    float space_md;
    float space_lg;
    float font_sm;
    float font_md;
    float font_lg;
    float font_xl;

    // Responsive layout thresholds. The demo reflows its sidebars when the
    // logical width crosses these (measured from the reference proportions).
    float bp_sidebar_collapsed = 900.0f;   // below: hide right sidebar
    float bp_sidebar_narrow = 1200.0f;     // below: narrow left sidebar
};

inline Color color_lighten(Color c, int amount) {
    auto add = [amount](uint8_t v) {
        return static_cast<uint8_t>(std::min(255, static_cast<int>(v) + amount));
    };
    return Color{add(c.r), add(c.g), add(c.b), c.a};
}

inline Color color_darken(Color c, int amount) {
    auto sub = [amount](uint8_t v) {
        return static_cast<uint8_t>(std::max(0, static_cast<int>(v) - amount));
    };
    return Color{sub(c.r), sub(c.g), sub(c.b), c.a};
}

inline Color color_alpha(Color c, uint8_t a) { return Color{c.r, c.g, c.b, a}; }

inline Color color_mix(Color a, Color b, float t) {
    auto mix = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

inline constexpr Theme kThemeFaceForge = {
    /*bg_app*/            {0x15, 0x17, 0x1c, 0xff},
    /*bg_header*/         {0x17, 0x19, 0x1d, 0xff},
    /*bg_toolbar*/        {0x1e, 0x20, 0x25, 0xff},
    /*bg_panel*/          {0x1b, 0x1d, 0x22, 0xff},
    /*bg_raised*/         {0x21, 0x24, 0x29, 0xff},
    /*bg_control*/        {0x2b, 0x2e, 0x35, 0xff},
    /*bg_control_hover*/  {0x35, 0x39, 0x42, 0xff},
    /*border*/            {0x2a, 0x2d, 0x34, 0xff},
    /*border_strong*/     {0x3a, 0x3f, 0x48, 0xff},

    /*accent*/            {0x42, 0x8a, 0xf5, 0xff},
    /*accent_hover*/      {0x5a, 0x9b, 0xff, 0xff},
    /*accent_pressed*/    {0x2e, 0x6f, 0xd6, 0xff},
    /*accent_dim*/        {0x2e, 0x5d, 0xa3, 0xff},
    /*on_accent*/         {0xff, 0xff, 0xff, 0xff},

    /*text*/              {0xf4, 0xf5, 0xf6, 0xff},
    /*text_muted*/        {0xc1, 0xc8, 0xd1, 0xff},
    /*text_dim*/          {0x9e, 0xa5, 0xaf, 0xff},
    /*danger*/            {0xe5, 0x53, 0x4b, 0xff},
    /*success*/           {0x3f, 0xb9, 0x50, 0xff},

    /*bg_window*/         {0x07, 0x08, 0x0a, 0xff},

    /*radius_sm*/ 6.0f,
    /*radius_md*/ 8.0f,
    /*radius_lg*/ 12.0f,
    /*space_xs*/  4.0f,
    /*space_sm*/  8.0f,
    /*space_md*/  12.0f,
    /*space_lg*/  16.0f,
    /*font_sm*/   13.0f,
    /*font_md*/   14.0f,
    /*font_lg*/   16.0f,
    /*font_xl*/   20.0f,
    /*bp_sidebar_collapsed*/ 900.0f,
    /*bp_sidebar_narrow*/    1200.0f
};

inline constexpr Theme kThemeLight = {
    {0xf4, 0xf5, 0xf7, 0xff}, {0xfb, 0xfc, 0xfd, 0xff}, {0xee, 0xf0, 0xf3, 0xff},
    {0xff, 0xff, 0xff, 0xff}, {0xf7, 0xf8, 0xfa, 0xff}, {0xe8, 0xeb, 0xef, 0xff},
    {0xdb, 0xdf, 0xe5, 0xff}, {0xd5, 0xd9, 0xdf, 0xff}, {0xb8, 0xbe, 0xc7, 0xff},
    {0x2f, 0x74, 0xd8, 0xff}, {0x3f, 0x88, 0xf6, 0xff}, {0x25, 0x5d, 0xb8, 0xff},
    {0x2e, 0x5d, 0xa3, 0xff}, {0xff, 0xff, 0xff, 0xff},
    {0x1b, 0x1f, 0x26, 0xff}, {0x59, 0x62, 0x70, 0xff}, {0x8a, 0x92, 0x9e, 0xff},
    {0xc0, 0x39, 0x2b, 0xff}, {0x1f, 0x8a, 0x3d, 0xff},
    {0xdf, 0xe2, 0xe7, 0xff},
    6.0f, 8.0f, 12.0f, 4.0f, 8.0f, 12.0f, 16.0f,
    12.0f, 14.0f, 16.0f, 20.0f,
    900.0f, 1200.0f
};

inline const Theme& theme_faceforge() { return kThemeFaceForge; }
inline const Theme& theme_light()     { return kThemeLight; }
