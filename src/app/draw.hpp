#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

#include "core/i18n.hpp"
#include "media/lyrics.hpp"
#include "ui/ui.hpp"

// Drawing helpers shared by the parts of the shell.
namespace lyren {

    inline std::string mmss(double seconds) {
        const auto whole = static_cast<uint32_t>(std::max(0.0, seconds));
        return std::format("{}:{:02}", whole / 60, whole % 60);
    }

    // Multiplies the painter's alpha for one scope.
    struct Fade {
        ui::Painter &p;
        float saved;
        Fade(ui::Painter &painter, float a)
        : p(painter),
          saved(painter.alpha()) {
            p.set_alpha(saved * a);
        }
        ~Fade() {
            p.set_alpha(saved);
        }
    };

    // Widgets in the scope draw with another theme, such as the stage's.
    struct UseTheme {
        ui::Ui &ui;
        const ui::Theme &saved;
        UseTheme(ui::Ui &u, const ui::Theme &t)
        : ui(u),
          saved(u.use_theme(t)) {}
        ~UseTheme() {
            ui.use_theme(saved);
        }
    };

    // Quintic smoothstep of t, clamped to 0..1.
    inline float smooth01(float t) {
        t = std::clamp(t, 0.f, 1.f);
        return t * t * t * (t * (6 * t - 15) + 10);
    }

    inline ui::Rect mix(ui::Rect a, ui::Rect b, float k) {
        return {std::lerp(a.x, b.x, k), std::lerp(a.y, b.y, k), std::lerp(a.w, b.w, k), std::lerp(a.h, b.h, k)};
    }

    // Each cluster rises and settles in time, independently of the highlight stopping between words. Shape the whole
    // line so a word split across wrapped rows keeps the same timing. The returned offsets are relative to s.
    inline void fill_lyric_wave(std::vector<ui::Painter::Lift> &out, const text::Shaped &shaped, const LyricLine &line, std::string_view s, float size, double now, float amount) {
        out.clear();
        if (amount <= 0) {
            return;
        }
        const auto from = static_cast<uint32_t>(s.data() - line.text.data()), to = from + static_cast<uint32_t>(s.size());
        const float rise = size * 0.13f * amount, swell = 0.07f * amount;
        auto x = [&](uint32_t offset) {
            auto glyph = std::ranges::lower_bound(shaped.glyphs, offset, {}, &text::Glyph::cluster);
            return glyph == shaped.glyphs.end() ? shaped.width : glyph->x;
        };
        for (size_t i = 0; i < shaped.glyphs.size();) {
            const text::Glyph &g = shaped.glyphs[i++];
            while (i < shaped.glyphs.size() && shaped.glyphs[i].cluster == g.cluster) {
                ++i;
            }
            if (g.cluster < from || g.cluster >= to) {
                continue;
            }
            float k = 0;
            auto word = std::ranges::upper_bound(line.words, g.cluster, {}, &LyricWord::from);
            if (word != line.words.begin() && g.cluster < word[-1].to) {
                const LyricWord &w = word[-1];
                const float left = x(w.from), right = x(w.to), next = i < shaped.glyphs.size() ? shaped.glyphs[i].x : shaped.width;
                const double start = w.time() + w.duration() * (right > left ? (g.x - left) / (right - left) : 0);
                const double duration = w.duration() * (right > left ? (std::min(next, right) - g.x) / (right - left) : 1);
                constexpr double attack = 0.28;
                const double peak = start + std::min(0.12, duration * 0.35), release = std::max(0.32, start + duration - peak + 0.18);
                // Even a zero-duration syllable has a soft attack and release; a long note keeps settling instead of
                // holding a raised glyph still until the next word arrives.
                k = now < peak ? smooth01(static_cast<float>((now - peak + attack) / attack)) : 1 - smooth01(static_cast<float>((now - peak) / release));
            }
            out.push_back({g.cluster - from, rise * k, 1 + swell * k});
        }
    }

    // Counts as "1.2k", or "1.2万" in Chinese.
    inline std::string short_count(int n) {
        if (language() == Language::zh) {
            return n >= 10000 ? trf("{:.1f}w", n / 1e4) : std::to_string(n);
        }
        return n >= 1000 ? std::format("{:.1f}k", n / 1e3) : std::to_string(n);
    }

    // With thousands separators: 12,345.
    inline std::string grouped(int n) {
        std::string s = std::to_string(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
            s.insert(static_cast<size_t>(i), ",");
        }
        return s;
    }

    // Width of a button that fits its label and icon.
    inline float button_width(ui::Ui &ui, std::string_view label, bool icon) {
        float w = ui.painter().measure(label, {.size = ui.theme().fs(13)});
        return std::round(w + (icon ? 22 : 0) + (label.empty() ? 24 : 36));
    }

} // namespace lyren
