#pragma once
#include <algorithm>
#include <array>
#include <charconv>
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

// Drawing helpers shared by the parts of the window.
namespace lyren {

    inline constexpr ui::Color white{1, 1, 1, 1};
    // What stays dark whatever the theme: the modules of a QR code, the body of a record.
    inline constexpr ui::Color ink{0.07f, 0.07f, 0.08f, 1};

    // Stack-backed formatted string for hot per-frame UI paths (zero heap allocations).
    template <size_t N = 24>
    struct SmallText {
        std::array<char, N> buf{};
        size_t len = 0;

        operator std::string_view() const {
            return {buf.data(), len};
        }
    };

    template <size_t N, class... Args>
    SmallText<N> small_format(std::format_string<Args...> fmt, Args &&...args) {
        SmallText<N> out;
        auto result = std::format_to_n(out.buf.data(), N, fmt, std::forward<Args>(args)...);
        out.len = static_cast<size_t>(result.out - out.buf.data());
        return out;
    }

    inline SmallText<16> mmss(double seconds) {
        SmallText<16> out;
        const auto whole = static_cast<uint32_t>(std::max(0.0, seconds));
        const auto res = std::format_to_n(out.buf.data(), out.buf.size(), "{}:{:02}", whole / 60, whole % 60);
        out.len = static_cast<size_t>(res.out - out.buf.data());
        return out;
    }

    inline SmallText<24> int_text(int64_t n) {
        SmallText<24> out;
        const auto res = std::to_chars(out.buf.data(), out.buf.data() + out.buf.size(), n);
        out.len = static_cast<size_t>(res.ptr - out.buf.data());
        return out;
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

    // Shared-element layout morph: glides a component from its previous rect to its new target whenever its layout
    // mode flips (e.g. side rail <-> inline column), while ordinary window resizing stays immediate without lag.
    struct SharedSlot {
        ui::Rect current{};
        ui::Rect from{};
        double since = -10;
        int mode = -1;

        ui::Rect resolve(ui::Ui &ui, ui::Rect target, int next_mode, float ms = 320) {
            if (mode < 0) {
                mode = next_mode;
                current = from = target;
                return target;
            }
            if (next_mode != mode) {
                mode = next_mode;
                from = current;
                since = ui.now;
            }
            const float k = ui.progress(since, ms, ui::ease::emphasized);
            current = k < 1 ? mix(from, target, k) : target;
            return current;
        }
        void reset() {
            mode = -1;
            since = -10;
        }
    };

    // The original stage view (95b21a6) starts the highlight fade 200 ms before the lyric cue so it is visible when the vocal begins.
    // The desktop island and comment quotes follow the same highlighted line.
    inline constexpr double lyric_lead = 0.2;

    // Where the wipe of a word-timed line stands at time within text, one of its wrapped lines: px from the start
    // of text, a soft edge's width before it while it is unsung and past its end once sung.
    inline float lyric_sung_x(ui::Painter &p, const LyricLine &line, std::string_view text, float size, float soft, double time, uint8_t features = 0) {
        const int k = word_at(line, time);
        if (k < 0) {
            return -soft;
        }
        const LyricWord &word = line.words[static_cast<size_t>(k)];
        const auto from = static_cast<uint32_t>(text.data() - line.text.data()), to = from + static_cast<uint32_t>(text.size());
        const text::Shaped &shaped = p.shape(text, {.size = size, .features = features});
        auto x = [&](uint32_t byte) {
            if (byte <= from) {
                return -soft;
            }
            if (byte >= to) {
                return shaped.width * size + soft;
            }
            auto glyph = std::ranges::find_if(shaped.glyphs, [&](const text::Glyph &g) {
                return g.cluster >= byte - from;
            });
            return (glyph == shaped.glyphs.end() ? shaped.width : glyph->x) * size;
        };
        const float sung = word.duration() > 0 ? static_cast<float>(std::clamp((time - word.time()) / word.duration(), 0.0, 1.0)) : 1;
        return x(word.from) + (x(word.to) - x(word.from)) * sung;
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

    // Short month label for calendar emblems and timelines (0..11), zero-allocation.
    inline std::string_view month_short(int month) {
        static constexpr std::array<std::string_view, 12> en{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        static constexpr std::array<std::string_view, 12> zh{"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"};
        const auto idx = static_cast<size_t>(std::clamp(month, 0, 11));
        return language() == Language::zh ? zh[idx] : en[idx];
    }

    // When a post was made, as near as matters: the time today, the day this year, the date before that.
    inline SmallText<16> post_time(int64_t ms) {
        if (ms <= 0) {
            return {};
        }
        const std::time_t at = static_cast<std::time_t>(ms / 1000), now = std::time(nullptr);
        static thread_local std::time_t cached_sec = -1;
        static thread_local std::tm cached_now{};
        if (now != cached_sec) {
            localtime_r(&now, &cached_now);
            cached_sec = now;
        }
        std::tm a{};
        localtime_r(&at, &a);
        if (a.tm_year == cached_now.tm_year && a.tm_yday == cached_now.tm_yday) {
            return small_format<16>("{:02}:{:02}", a.tm_hour, a.tm_min);
        }
        if (a.tm_year == cached_now.tm_year) {
            return small_format<16>("{}/{} {:02}:{:02}", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
        }
        return small_format<16>("{}/{}/{}", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
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

    // The columns of a track row, and of the header above the rows.
    struct TrackColumns {
        ui::Rect index, title, album, like, duration;
    };

    inline TrackColumns track_columns(ui::Rect r) {
        TrackColumns c;
        r = r.inset(12, 0);
        c.index = ui::cut_left(r, 40);
        ui::cut_left(r, 4);
        c.duration = ui::cut_right(r, 48);
        c.like = ui::cut_right(r, 44);
        if (r.w > 460) {
            c.album = ui::cut_right(r, std::round(r.w * 0.36f));
            ui::cut_right(r, 16);
        }
        c.title = r;
        return c;
    }

    // Cards in as many columns as fit. Row and column changes are animated; ordinary resizing and scrolling stay under
    // the pointer.
    struct CardGrid {
        static constexpr float gap = 20, caption = 54, reflow_ms = 280;
        ui::Ui &ui;
        ui::Id id;
        int cols;
        float cw, ch;
        bool reflowing = false;
        CardGrid(ui::Ui &ui, ui::Id id, float width, float min_w = 150)
        : ui(ui),
          id(id) {
            cols = columns(width, min_w);
            const float count = ui.spring(ui::sub_id(id, 0xc0), static_cast<float>(cols), reflow_ms);
            reflowing = std::abs(count - static_cast<float>(cols)) > 0.01f;
            cw = (width + gap) / count - gap;
            ch = cw + caption;
        }
        static int columns(float width, float min_w = 150) {
            return std::max(1, static_cast<int>((width + gap) / (min_w + gap)));
        }
        // The height n cards take once they have settled, without animating anything.
        static float settled_height(float width, size_t n, float min_w = 150) {
            if (!n) {
                return 0;
            }
            const auto cols = static_cast<size_t>(columns(width, min_w));
            const float cw = (width + gap) / static_cast<float>(cols) - gap;
            return static_cast<float>((n + cols - 1) / cols) * (cw + caption + gap) - gap;
        }
        float height(size_t n) const {
            if (!n) {
                return 0;
            }
            const float last = ui.spring(ui::sub_id(id, 0xc1), static_cast<float>((n - 1) / static_cast<size_t>(cols)), reflow_ms);
            return last * (ch + gap) + ch;
        }
        ui::Rect cell(float x, float y, size_t i, ui::Id item) const {
            const float col = ui.spring(ui::sub_id(item, 0xc0), static_cast<float>(i % static_cast<size_t>(cols)), reflow_ms);
            const float row = ui.spring(ui::sub_id(item, 0xc1), static_cast<float>(i / static_cast<size_t>(cols)), reflow_ms);
            return {x + col * (cw + gap), y + row * (ch + gap), cw, ch};
        }
    };

    // A page's readable content column: generous inset normally, snug on narrow windows, and centered at max_w on
    // wide monitors so grids, track lists, and headers keep balanced proportions.
    inline ui::Rect page_frame(ui::Rect r, float max_w = 1380) {
        const float pad = r.w < 680 ? 20.f : 32.f;
        ui::Rect body = r.inset(pad, 0);
        if (body.w > max_w) {
            body = ui::center(body, max_w, body.h);
        }
        return body;
    }

    // A page's title and quiet subtitle, aligned with the content column.
    inline void page_title(ui::Ui &ui, ui::Rect body, float y, std::string_view title, std::string_view subtitle = {}) {
        const auto &t = ui.theme();
        ui.label({body.x, y, body.w, 38}, title, t.style(ui::type::display, t.text, text::semibold));
        if (!subtitle.empty()) {
            ui.label({body.x, y + 42, body.w, 20}, subtitle, t.style(ui::type::label, t.text_dim));
        }
    }

    // A section heading with an optional quiet count or note beside it.
    inline void section_heading(ui::Ui &ui, float x, float y, float w, std::string_view title, std::string_view note = {}) {
        const auto &t = ui.theme();
        const auto style = t.style(ui::type::heading, t.text, text::semibold);
        const float tw = std::min(std::ceil(ui.painter().measure(title, style)) + 1, w);
        ui.label({x, y, tw, 26}, title, style);
        if (!note.empty() && tw + 12 < w) {
            ui.label({x + tw + 8, y + 1, w - tw - 8, 26}, note, t.style(ui::type::label, t.faint(), text::tabular_nums));
        }
    }

    // A dialog's heading in r's top: its title and, under it, a quieter line.
    inline void dialog_title(ui::Ui &ui, ui::Rect r, std::string_view title, std::string_view subtitle = {}) {
        const auto &t = ui.theme();
        ui.label({r.x, r.y, r.w - 40, 30}, title, t.style(ui::type::title, t.text, text::semibold));
        if (!subtitle.empty()) {
            ui.label({r.x, r.y + 31, r.w - 40, 20}, subtitle, t.style(ui::type::label, t.text_dim));
        }
    }

    // The buttons across a dialog's foot: Cancel, which closes it, and the one that goes ahead, which answers only
    // while enabled. True when that one is pressed.
    inline bool dialog_actions(ui::Ui &ui, ui::Rect r, std::string_view action, bool enabled = true, std::string_view icon = {}, ui::ButtonKind kind = ui::ButtonKind::primary) {
        const float h = ui::control::large;
        const float ok_w = std::max(96.f, ui.button_width(action, !icon.empty())), cancel_w = std::max(84.f, ui.button_width(tr("Cancel")));
        const ui::Rect ok{r.right() - ok_w, r.bottom() - h, ok_w, h};
        if (ui.button(ui.id("dialog-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, h}, tr("Cancel"), ui::ButtonKind::ghost)) {
            ui.close_modal();
        }
        ui::Enabled possible(ui, enabled);
        return ui.button(ui.id("dialog-ok"), ok, action, kind, icon);
    }

    // Circular play/pause button whose icons morph into each other; optionally shows a loading ring while buffering.
    inline bool play_morph_button(ui::Ui &ui, ui::Id id, ui::Rect r, bool playing, float icon_size, bool accent = false, bool waiting = false) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = ui.feel(id, it);
        const ui::Rect disc = r.inset(-1.5f * hot + 2.f * down);
        const ui::Color bg = accent ? t.accent.mix(white, 0.08f * hot) : t.text;
        const ui::Color fg = accent ? t.on_accent() : t.bg;
        p.rect(disc, bg, disc.w / 2);
        const float morph = ui.anim(ui::sub_id(id, 1), playing ? 1.f : 0.f, 240, ui::ease::emphasized);
        {
            // The triangle's weight sits left of its box, so it stands a little right of the centre.
            Fade fade(p, 1 - morph);
            p.set_zoom((icon_size - 5 * morph) / icon_size, disc.x + disc.w / 2, disc.y + disc.h / 2);
            ui::Rect glyph = ui::center(disc, icon_size, icon_size);
            glyph.x += 1;
            p.icon("play-fill", glyph, fg);
            p.set_zoom(1);
        }
        {
            Fade fade(p, morph);
            p.set_zoom((icon_size - 5 + 5 * morph) / icon_size, disc.x + disc.w / 2, disc.y + disc.h / 2);
            p.icon("pause-fill", ui::center(disc, icon_size, icon_size), fg);
            p.set_zoom(1);
        }
        if (const float ring = ui.anim(ui::sub_id(id, 2), waiting ? 1.f : 0.f, 200); ring > 0.01f) {
            Fade fade(p, ring);
            ui.spinner(disc.inset(-4), t.accent, 2);
        }
        return it.clicked;
    }

    // With thousands separators: 12,345.
    inline SmallText<16> grouped(int n) {
        const auto digits = int_text(n);
        SmallText<16> out;
        for (size_t i = 0; i < digits.len; ++i) {
            if (i > static_cast<size_t>(n < 0) && (digits.len - i) % 3 == 0) {
                out.buf[out.len++] = ',';
            }
            out.buf[out.len++] = digits.buf[i];
        }
        return out;
    }

    // A count in r, worded by text. One that goes up or down by one rolls over: the number leaving slides out as the
    // one arriving slides in after it. Any other change shows at once.
    template <class Text>
    void rolling_count(ui::Ui &ui, ui::Id id, ui::Rect r, int count, Text &&text, const ui::TextStyle &style, ui::Align align = ui::Align::left) {
        const auto target = static_cast<float>(count);
        const bool step = std::abs(target - std::round(ui.spring_value(id, target))) <= 1;
        const float value = ui.spring(id, target, step ? 260 : 0);
        const float low = std::floor(value), turn = value - low;
        if (turn == 0) {
            return ui.label(r, text(static_cast<int>(low)), style, align);
        }
        const std::string from = text(static_cast<int>(low)), to = text(static_cast<int>(std::ceil(value)));
        if (from == to) {
            return ui.label(r, to, style, align);
        }
        ui.push_clip(r);
        {
            Fade fade(ui.painter(), 1 - turn);
            ui.label({r.x, r.y - 10 * turn, r.w, r.h}, from, style, align);
        }
        {
            Fade fade(ui.painter(), turn);
            ui.label({r.x, r.y + 10 * (1 - turn), r.w, r.h}, to, style, align);
        }
        ui.pop_clip();
    }

} // namespace lyren
