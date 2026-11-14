#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <utility>

#include "core/i18n.hpp"
#include "core/utf8.hpp"
#include "media/lyrics.hpp"
#include "media/types.hpp"
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

    // Widget ids for a track or a card, hashed from its identity without building its key string every frame.
    inline ui::Id item_id(ui::Ui &ui, const Track &t) {
        return ui::sub_id(ui.id(t.id), std::to_underlying(t.source));
    }
    inline ui::Id item_id(ui::Ui &ui, const Card &c) {
        return ui::sub_id(ui.id(c.id), std::to_underlying(c.source) << 8 | std::to_underlying(c.kind));
    }

    // Quintic smoothstep of t, clamped to 0..1.
    inline float smooth01(float t) {
        t = std::clamp(t, 0.f, 1.f);
        return t * t * t * (t * (6 * t - 15) + 10);
    }

    // The part of an image of aspect `image` that fills a frame of aspect `frame`, centred.
    inline ui::Rect cover_uv(float image, float frame) {
        if (image > frame) {
            float u = frame / image;
            return {(1 - u) / 2, 0, u, 1};
        }
        float v = image / frame;
        return {0, (1 - v) / 2, 1, v};
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

    // When a post was made, as near as matters: the time today, the day this year, the date before that.
    inline std::string post_time(int64_t ms) {
        if (ms <= 0) {
            return {};
        }
        const std::time_t at = static_cast<std::time_t>(ms / 1000), now = std::time(nullptr);
        std::tm a, n;
        localtime_r(&at, &a);
        localtime_r(&now, &n);
        if (a.tm_year == n.tm_year && a.tm_yday == n.tm_yday) {
            return std::format("{:02}:{:02}", a.tm_hour, a.tm_min);
        }
        if (a.tm_year == n.tm_year) {
            return std::format("{}/{} {:02}:{:02}", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
        }
        return std::format("{}/{}/{}", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
    }

    // Topics (#…#) and mentions (@…), as byte ranges of s.
    inline std::vector<std::pair<size_t, size_t>> highlights(std::string_view s) {
        std::vector<std::pair<size_t, size_t>> out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '#') {
                const size_t end = s.find('#', i + 1);
                if (end != s.npos && end - i > 1 && end - i <= 96 && s.substr(i, end - i).find('\n') == s.npos) {
                    out.emplace_back(i, end + 1);
                    i = end;
                }
            } else if (s[i] == '@') {
                // A name runs to a space or a punctuation mark, ASCII or full-width.
                size_t end = i + 1;
                while (end < s.size() && !std::strchr(" \t\n:,.!?@", s[end]) && !s.substr(end).starts_with("\xef\xbc") && !s.substr(end).starts_with("\xe3\x80")) {
                    end = utf8::next(s, end);
                }
                if (end > i + 1) {
                    out.emplace_back(i, end);
                    i = end - 1;
                }
            }
        }
        return out;
    }

    // One wrapped line of s, from offset, the marked parts in mark.
    inline void rich_line(ui::Painter &p, float x, float baseline, std::string_view s, std::string_view line, const std::vector<std::pair<size_t, size_t>> &marks,
                          ui::TextStyle style, ui::Color mark) {
        const auto from = static_cast<size_t>(line.data() - s.data()), to = from + line.size();
        size_t at = from;
        for (auto [a, b] : marks) {
            if (b <= at || a >= to) {
                continue;
            }
            a = std::max(a, at), b = std::min(b, to);
            if (a > at) {
                x += p.text(x, baseline, s.substr(at, a - at), style);
            }
            ui::TextStyle lit = style;
            lit.color = mark;
            x += p.text(x, baseline, s.substr(a, b - a), lit);
            at = b;
        }
        if (at < to) {
            p.text(x, baseline, s.substr(at, to - at), style);
        }
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
