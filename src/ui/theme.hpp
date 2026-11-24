#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sys/config.hpp"

namespace lyren::ui {

    // Straight-alpha sRGB colour.
    struct Color {
        float r = 0, g = 0, b = 0, a = 0;
        constexpr Color with_alpha(float alpha) const {
            return {r, g, b, a * alpha};
        }
        constexpr Color mix(Color o, float t) const {
            return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t};
        }
        float luminance() const {
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
    };

    // "#rgb", "#rrggbb" or "#rrggbbaa".
    std::optional<Color> parse_color(std::string_view s);
    // "#rrggbb", or "#rrggbbaa" when not opaque.
    std::string format_color(Color c);
    Color hsl(float h, float s, float l);
    // An accent for a cover's hue (0..1): the same perceived lightness whatever the hue, as vivid as sRGB allows;
    // lighter on dark backgrounds.
    Color accent_for_hue(float hue, bool dark);

    struct TextStyle {
        float size = 14;
        Color color;
        uint8_t features = 0;
        bool silhouette = false; // colour glyphs (emoji) take the colour as a solid shape, for outlines
    };

    // The type scale, in logical pixels at the default text size; Theme::fs and Theme::style scale it.
    namespace type {
        inline constexpr float numeral = 44; // a figure that stands for something: the day on today's mix
        inline constexpr float display = 28; // what a page is about: its title, a listener, a list, an artist
        inline constexpr float title = 20;   // dialogs and panels
        inline constexpr float heading = 16; // sections within a page
        inline constexpr float lead = 15;    // text meant to be read: posts, comments set large
        inline constexpr float body = 14;
        inline constexpr float label = 13;   // controls and secondary lines
        inline constexpr float caption = 12; // metadata
        inline constexpr float micro = 11;   // timestamps, badges
    } // namespace type

    // How long things take, in ms before Theme::motion scales them.
    namespace pace {
        inline constexpr float snap = 85;    // immediate tactile feedback
        inline constexpr float quick = 120;  // hover and other small changes of state
        inline constexpr float steady = 175; // things that fade or recolour
        inline constexpr float glide = 250;  // things that move to a new place
        inline constexpr float arrive = 340; // content coming in
    } // namespace pace

    // Spatial rhythm, in logical pixels.
    namespace space {
        inline constexpr float xs = 4, s = 8, m = 12, l = 16, xl = 24, xxl = 32;
    } // namespace space

    // Heights of controls, so that rows of them line up.
    namespace control {
        inline constexpr float small = 28, medium = 34, large = 40;
    } // namespace control

    struct Theme {
        Color bg, surface, elevated, border, text, text_dim, accent, danger;
        Color hover, press, shadow;
        float font_size = 14; // base size; the type scale follows it
        float radius = 10;    // medium; small and large follow it
        float motion = 1;
        bool dark = true;
        bool auto_accent = true; // accent follows the cover of the current track
        // Desktop lyrics: the sung part (unset follows the accent), the rest, and an outline that reads over anything.
        std::optional<Color> lyric_sung;
        Color lyric_text, lyric_outline;
        float lyric_size = 30;

        float fs(float base14) const {
            return base14 * font_size / 14;
        }
        float radius_s() const {
            return radius * 0.6f;
        }
        float radius_l() const {
            return radius * 1.6f;
        }
        // The corners of a control h tall: fully round at the default radius, squarer as the theme's radius shrinks.
        float round(float h) const {
            return std::min(h / 2, radius * 2);
        }
        TextStyle style(float size, Color color, uint8_t features = 0) const {
            return {.size = fs(size), .color = color, .features = features};
        }
        // Text a step quieter than text_dim: timestamps, counts, placeholders.
        Color faint() const {
            return text.with_alpha(0.42f);
        }
        // A quiet container on the page; k scales how much it stands out.
        Color fill(float k = 1) const {
            return text.with_alpha((dark ? 0.06f : 0.045f) * k);
        }
        // A hairline between things.
        Color line(float k = 1) const {
            return border.with_alpha(0.55f * k);
        }
        // A quiet 1px structural edge around sheets, cards and floating consoles.
        Color surface_edge(float k = 1) const {
            return text.with_alpha((dark ? 0.05f : 0.045f) * k);
        }
        // Readable on the accent, which follows the cover and can be light.
        Color on_accent() const {
            return accent.luminance() > 0.6f ? Color{0.07f, 0.07f, 0.08f, 1} : Color{1, 1, 1, 1};
        }

        // The colours k of the way to o; sizes stay. For things that move between the window and the stage.
        Theme mix(const Theme &o, float k) const;
        // Light on the cover-lit now-playing stage and the desktop island, whatever the window theme.
        Theme stage(Color accent) const;

        // Builds the theme from config; invalid values fall back to defaults and are described in errors.
        static Theme from(const Config &config, std::string &errors);
    };

} // namespace lyren::ui
