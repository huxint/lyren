#include "ui/theme.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace lyren::ui {

std::optional<Color> parse_color(std::string_view s) {
    if (s.empty() || s[0] != '#') return std::nullopt;
    s.remove_prefix(1);
    auto hex = [&](size_t i, size_t n) {
        unsigned v = 0;
        auto r = std::from_chars(s.data() + i, s.data() + i + n, v, 16);
        return r.ec == std::errc{} && r.ptr == s.data() + i + n ? int(v) : -1;
    };
    int c[4] = {0, 0, 0, 255};
    if (s.size() == 3) {
        for (int i = 0; i < 3; ++i) c[i] = hex(size_t(i), 1) * 17;
    } else if (s.size() == 6 || s.size() == 8) {
        for (size_t i = 0; i < s.size() / 2; ++i) c[i] = hex(i * 2, 2);
    } else {
        return std::nullopt;
    }
    if (std::ranges::any_of(c, [](int v) { return v < 0; })) return std::nullopt;
    return Color{c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
}

static Color hex(const char* s) { return *parse_color(s); }

Theme Theme::from(const Config& config, std::string& errors) {
    const auto& t = config.theme;
    Theme th;
    th.dark = t.mode == ThemeMode::dark;
    if (th.dark) {
        th.bg = hex("#111113"), th.surface = hex("#1a1a1e"), th.elevated = hex("#232329"), th.border = hex("#2c2c33");
        th.text = hex("#ececf1"), th.text_dim = hex("#9a9aa5"), th.danger = hex("#e5484d");
    } else {
        th.bg = hex("#f7f7f8"), th.surface = hex("#ffffff"), th.elevated = hex("#ffffff"), th.border = hex("#e2e2e6");
        th.text = hex("#1c1c21"), th.text_dim = hex("#6b6b76"), th.danger = hex("#dc3e42");
    }
    auto apply = [&](const std::optional<std::string>& v, Color& dst, const char* key) {
        if (!v) return;
        if (auto c = parse_color(*v)) dst = *c;
        else errors += std::format("theme.colors.{}: 无效颜色 \"{}\"\n", key, *v);
    };
    apply(t.colors.bg, th.bg, "bg");
    apply(t.colors.surface, th.surface, "surface");
    apply(t.colors.elevated, th.elevated, "elevated");
    apply(t.colors.border, th.border, "border");
    apply(t.colors.text, th.text, "text");
    apply(t.colors.text_dim, th.text_dim, "text_dim");
    apply(t.colors.danger, th.danger, "danger");

    Color accent = hex("#e5484d");
    th.auto_accent = t.accent == "auto";
    if (!th.auto_accent) {
        if (auto c = parse_color(t.accent)) accent = *c;
        else errors += std::format("theme.accent: 无效颜色 \"{}\"\n", t.accent);
    }
    th.accent = accent;
    th.shadow = th.dark ? Color{0, 0, 0, 0.55f} : Color{0, 0, 0, 0.18f};
    th.hover = th.text.with_alpha(0.07f);
    th.press = th.text.with_alpha(0.12f);
    th.font_size = float(std::clamp(t.font_size, 9.0, 32.0));
    th.radius = float(std::clamp(t.radius, 0.0, 24.0));
    th.motion = float(std::clamp(t.motion, 0.0, 4.0));
    th.opacity = float(std::clamp(config.window.opacity, 0.3, 1.0));

    const auto& d = config.desktop_lyrics;
    th.lyric_text = hex("#ffffff"), th.lyric_outline = hex("#000000a0");
    auto lyric = [&](const std::string& v, Color& dst, const char* key) {
        if (auto c = parse_color(v)) dst = *c;
        else errors += std::format("desktop_lyrics.{}: 无效颜色 \"{}\"\n", key, v);
    };
    lyric(d.color, th.lyric_text, "color");
    lyric(d.outline, th.lyric_outline, "outline");
    if (d.highlight != "accent") {
        Color c = accent;
        lyric(d.highlight, c, "highlight");
        th.lyric_sung = c;
    }
    th.lyric_size = float(std::clamp(d.font_size, 16.0, 72.0));
    return th;
}

}  // namespace lyren::ui
