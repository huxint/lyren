#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "sys/config.hpp"

namespace lyren::ui {

// Straight-alpha sRGB colour.
struct Color {
    float r = 0, g = 0, b = 0, a = 0;
    constexpr Color with_alpha(float alpha) const { return {r, g, b, a * alpha}; }
    constexpr Color mix(Color o, float t) const { return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t}; }
    float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
};

// "#rgb", "#rrggbb" or "#rrggbbaa".
std::optional<Color> parse_color(std::string_view s);

struct Theme {
    Color bg, surface, elevated, border, text, text_dim, accent, danger;
    Color hover, press, shadow;
    float font_size = 14;  // base size; the scale 12/13/14/16/20/28 follows it
    float radius = 10;     // medium; small and large follow it
    float motion = 1;
    float opacity = 1;
    bool dark = true;
    bool auto_accent = true;  // accent follows the cover of the current track

    float fs(float base14) const { return base14 * font_size / 14; }
    float radius_s() const { return radius * 0.6f; }
    float radius_l() const { return radius * 1.6f; }

    // Builds the theme from config; invalid values fall back to defaults and are described in errors.
    static Theme from(const Config& config, std::string& errors);
};

}  // namespace lyren::ui
