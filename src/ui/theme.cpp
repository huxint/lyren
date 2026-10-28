#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>

namespace lyren::ui {

    std::optional<Color> parse_color(std::string_view s) {
        if (s.empty() || s[0] != '#') {
            return std::nullopt;
        }
        s.remove_prefix(1);
        auto hex = [&](size_t i, size_t n) {
            unsigned v = 0;
            auto r = std::from_chars(s.data() + i, s.data() + i + n, v, 16);
            return r.ec == std::errc{} && r.ptr == s.data() + i + n ? static_cast<int>(v) : -1;
        };
        std::array c = {0, 0, 0, 255};
        if (s.size() == 3) {
            for (int i = 0; i < 3; ++i) {
                c[i] = hex(static_cast<size_t>(i), 1) * 17;
            }
        } else if (s.size() == 6 || s.size() == 8) {
            for (size_t i = 0; i < s.size() / 2; ++i) {
                c[i] = hex(i * 2, 2);
            }
        } else {
            return std::nullopt;
        }
        if (std::ranges::any_of(c, [](int v) {
                return v < 0;
            })) {
            return std::nullopt;
        }
        return Color{c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
    }

    std::string format_color(Color c) {
        auto byte = [](float v) {
            return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255));
        };
        std::string s = std::format("#{:02x}{:02x}{:02x}", byte(c.r), byte(c.g), byte(c.b));
        if (c.a < 1) {
            std::format_to(std::back_inserter(s), "{:02x}", byte(c.a));
        }
        return s;
    }

    Color hsl(float h, float s, float l) {
        h -= std::floor(h);
        auto f = [&](float n) {
            float k = std::fmod(n + h * 12, 12.f);
            return l - s * std::min(l, 1 - l) * std::max(-1.f, std::min({k - 3, 9 - k, 1.f}));
        };
        return {f(0), f(8), f(4), 1};
    }

    // OKLab (Björn Ottosson): perceptual lightness and hue for picking accents.
    namespace {
        struct Lab {
            float l, a, b;
        };

        float linear(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        float gamma(float c) {
            return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
        }

        Lab oklab(Color c) {
            float r = linear(c.r), g = linear(c.g), b = linear(c.b);
            float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
            float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
            float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
            return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                    0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
        }

        // Linear sRGB, possibly out of gamut.
        std::array<float, 3> linear_rgb(Lab c) {
            float l = c.l + 0.3963377774f * c.a + 0.2158037573f * c.b;
            float m = c.l - 0.1055613458f * c.a - 0.0638541728f * c.b;
            float s = c.l - 0.0894841775f * c.a - 1.2914855480f * c.b;
            l = l * l * l, m = m * m * m, s = s * s * s;
            return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                    -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
        }
    } // namespace

    Color accent_for_hue(float hue, bool dark) {
        Lab seed = oklab(hsl(hue, 0.7f, 0.5f));
        float angle = std::atan2(seed.b, seed.a), lightness = dark ? 0.78f : 0.56f;
        std::array<float, 3> rgb{};
        // The most chroma up to a calm maximum that stays inside sRGB.
        for (float chroma = dark ? 0.13f : 0.16f; chroma >= 0; chroma -= 0.005f) {
            rgb = linear_rgb({lightness, chroma * std::cos(angle), chroma * std::sin(angle)});
            if (std::ranges::all_of(rgb, [](float v) {
                    return v >= 0 && v <= 1;
                })) {
                break;
            }
        }
        return {gamma(std::clamp(rgb[0], 0.f, 1.f)), gamma(std::clamp(rgb[1], 0.f, 1.f)), gamma(std::clamp(rgb[2], 0.f, 1.f)), 1};
    }

    static Color hex(const char *s) {
        return *parse_color(s);
    }

    Theme Theme::mix(const Theme &o, float k) const {
        Theme t = *this;
        for (auto member : {&Theme::bg, &Theme::surface, &Theme::elevated, &Theme::border, &Theme::text, &Theme::text_dim, &Theme::accent, &Theme::danger, &Theme::hover,
                            &Theme::press, &Theme::shadow}) {
            t.*member = (this->*member).mix(o.*member, k);
        }
        t.dark = k < 0.5f ? dark : o.dark;
        return t;
    }

    Theme Theme::stage(Color accent_color) const {
        constexpr Color white{1, 1, 1, 1};
        Theme t = *this;
        t.dark = true;
        t.bg = {0.03f, 0.03f, 0.04f, 1}, t.surface = white.with_alpha(0.08f), t.elevated = {0.13f, 0.13f, 0.15f, 0.94f}, t.border = white.with_alpha(0.1f);
        t.text = white.with_alpha(0.96f), t.text_dim = white.with_alpha(0.6f), t.danger = hex("#ff6b6b");
        t.accent = accent_color;
        t.hover = white.with_alpha(0.1f), t.press = white.with_alpha(0.16f), t.shadow = {0, 0, 0, 0.35f};
        return t;
    }

    Theme Theme::from(const Config &config, std::string &errors) {
        const auto &t = config.theme;
        Theme th;
        th.dark = t.mode == ThemeMode::dark;
        if (th.dark) {
            th.bg = hex("#111113"), th.surface = hex("#1a1a1e"), th.elevated = hex("#232329"), th.border = hex("#2c2c33");
            th.text = hex("#ececf1"), th.text_dim = hex("#9a9aa5"), th.danger = hex("#e5484d");
        } else {
            th.bg = hex("#f3f3f6"), th.surface = hex("#ffffff"), th.elevated = hex("#ffffff"), th.border = hex("#e3e3ea");
            th.text = hex("#1c1c21"), th.text_dim = hex("#6b6b76"), th.danger = hex("#d1293d");
        }
        auto apply = [&](const std::optional<std::string> &v, Color &dst, const char *key) {
            if (!v) {
                return;
            }
            if (auto c = parse_color(*v)) {
                dst = *c;
            } else {
                std::format_to(std::back_inserter(errors), "theme.colors.{}: invalid color \"{}\"\n", key, *v);
            }
        };
        apply(t.colors.bg, th.bg, "bg");
        apply(t.colors.surface, th.surface, "surface");
        apply(t.colors.elevated, th.elevated, "elevated");
        apply(t.colors.border, th.border, "border");
        apply(t.colors.text, th.text, "text");
        apply(t.colors.text_dim, th.text_dim, "text_dim");
        apply(t.colors.danger, th.danger, "danger");

        Color accent = accent_for_hue(0.98f, th.dark);
        th.auto_accent = t.accent == "auto";
        if (!th.auto_accent) {
            if (auto c = parse_color(t.accent)) {
                accent = *c;
            } else {
                std::format_to(std::back_inserter(errors), "theme.accent: invalid color \"{}\"\n", t.accent);
            }
        }
        th.accent = accent;
        th.shadow = th.dark ? Color{0, 0, 0, 0.55f} : Color{0.08f, 0.08f, 0.14f, 0.16f};
        th.hover = th.text.with_alpha(th.dark ? 0.07f : 0.05f);
        th.press = th.text.with_alpha(th.dark ? 0.12f : 0.09f);
        th.font_size = static_cast<float>(std::clamp(t.font_size, 9.0, 32.0));
        th.radius = static_cast<float>(std::clamp(t.radius, 0.0, 24.0));
        th.motion = static_cast<float>(std::clamp(t.motion, 0.0, 4.0));
        th.opacity = static_cast<float>(std::clamp(config.window.opacity, 0.3, 1.0));

        const auto &d = config.desktop_lyrics;
        th.lyric_text = hex("#ffffff"), th.lyric_outline = hex("#000000a0");
        auto lyric = [&](const std::string &v, Color &dst, const char *key) {
            if (auto c = parse_color(v)) {
                dst = *c;
            } else {
                std::format_to(std::back_inserter(errors), "desktop_lyrics.{}: invalid color \"{}\"\n", key, v);
            }
        };
        lyric(d.color, th.lyric_text, "color");
        lyric(d.outline, th.lyric_outline, "outline");
        if (d.highlight != "accent") {
            Color c = accent;
            lyric(d.highlight, c, "highlight");
            th.lyric_sung = c;
        }
        th.lyric_size = static_cast<float>(std::clamp(d.font_size, 16.0, 72.0));
        return th;
    }

} // namespace lyren::ui
