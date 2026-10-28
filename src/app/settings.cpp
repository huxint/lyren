#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// The settings page: cards of settings under a tab bar that sticks to the top and follows the scroll. Settings that
// change how something looks show it live: the themes as small windows, the accent beside the cover it comes from,
// the desktop lyrics and the island as they will appear.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

    namespace {
        constexpr ui::Color white{1, 1, 1, 1};
        // Accents and lyric colours that read on both themes and over a wallpaper.
        constexpr std::array<std::string_view, 8> accents{"#e5484d", "#f76b15", "#e2a336", "#30a46c", "#12a594", "#0090ff", "#6e56cf", "#d6409f"};
        constexpr std::array<std::string_view, 5> sung_colors{"#ffffff", "#ffd166", "#ff8fab", "#7ee0e6", "#9be9a8"};
        constexpr std::array<std::string_view, 4> rest_colors{"#ffffff", "#c9c9d2", "#ffe7ad", "#cdeaff"};
        constexpr std::string_view outline_on = "\"#000000a0\"", outline_off = "\"#00000000\"";
        constexpr std::array<double, 4> text_sizes{13, 14, 15, 16};
        constexpr std::array<double, 3> motions{0, 1, 1.5};
        constexpr std::array<int, 6> frame_rates{0, 30, 60, 90, 120, 144};
        constexpr float bar_h = 52; // the tab bar
        // The cards sit in one column up to 720 wide or, once two of 460 fit, each section's side by side, up to 1160 in
        // all. The nine cards in order: the section each is in and its column when there are two.
        constexpr float one_column = 720, two_columns = 1160, column_gap = 20, two_from = 2 * 460 + column_gap;
        constexpr float card_gap = 14, section_gap = 44;
        constexpr std::array<int, 9> card_section{0, 0, 1, 1, 2, 2, 2, 3, 3}, card_column{0, 1, 0, 1, 0, 1, 0, 0, 1};

        bool same_color(std::string_view a, std::string_view b) {
            auto x = ui::parse_color(a), y = ui::parse_color(b);
            return x && y && ui::format_color(*x) == ui::format_color(*y);
        }
    } // namespace

    void Shell::settings_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const std::array<std::string_view, 4> sections{lyren::tr("Appearance"), lyren::tr("Playback"), lyren::tr("Lyrics"), lyren::tr("General")};
        const ui::Id scroll_id = ui.id("settings");

        // The layout in content coordinates, from the cards' heights as last drawn: the four headings, the nine cards, and
        // last the title and tab bar, whose height is the content's. A 12 px dead band keeps the columns from flipping
        // back and forth.
        const float room = r.w - 64;
        const int columns = room >= two_from + (settings_columns_ == 2 ? -12.f : settings_columns_ == 1 ? 12.f : 0.f) ? 2 : 1;
        const float content_w = std::min(room, columns == 2 ? two_columns : one_column), column_w = columns == 2 ? (content_w - column_gap) / 2 : content_w;
        const float left = 32 + (room - content_w) / 2;
        std::array<Rect, 14> layout;
        {
            float y = 28 + 76 + bar_h + 12;
            for (size_t s = 0; s < sections.size(); ++s) {
                layout[s] = {left, y, content_w, 28};
                settings_tops_[s] = y;
                std::array<float, 2> bottom{y + 44, y + 44};
                for (size_t c = 0; c < card_section.size(); ++c) {
                    if (card_section[c] == static_cast<int>(s)) {
                        const size_t col = columns == 2 ? static_cast<size_t>(card_column[c]) : 0;
                        layout[4 + c] = {left + static_cast<float>(col) * (column_w + column_gap), bottom[col], column_w, settings_heights_[c]};
                        bottom[col] += settings_heights_[c] + card_gap;
                    }
                }
                y = std::max(bottom[0], bottom[1]) - card_gap + section_gap;
            }
            layout[13] = {left, 0, content_w, y - section_gap + 54};
        }
        // Between one column and two, everything glides from where it was drawn, and the section on show keeps its place
        // in the view. A page entered afresh starts in place.
        if (settings_drawn_ < page_time_) {
            settings_columns_ = columns;
            settings_moved_ = -10;
        }
        if (columns != settings_columns_) {
            const auto section = static_cast<size_t>(settings_section_);
            const float view = ui.scroll_offset(scroll_id);
            const float moved = std::clamp(view + layout[section].y - settings_shown_[section].y, 0.f, std::max(0.f, layout[13].h - r.h)) - view;
            ui.scroll_by(scroll_id, moved);
            settings_from_ = settings_shown_;
            for (Rect &from : settings_from_) {
                from.y += moved;
            }
            settings_columns_ = columns, settings_moved_ = ui.now;
        }
        settings_drawn_ = ui.now;
        std::array<Rect, 14> at = layout;
        if (const float k = ui.progress(settings_moved_, 460); k < 1) {
            for (size_t i = 0; i < at.size(); ++i) {
                at[i] = mix(settings_from_[i], layout[i], k);
            }
        }
        settings_shown_ = at;

        const float off = ui.begin_scroll(scroll_id, r, layout[13].h);
        const float origin = r.y - off; // content coordinates start here
        const Rect frame{r.x + at[13].x, origin, at[13].w, 0};
        ui.label({frame.x, origin + 28, frame.w, 38}, lyren::tr("Settings"), {.size = t.fs(28), .color = t.text});
        ui.label({frame.x, origin + 70, frame.w, 20}, lyren::tr("Changes apply right away"), {.size = t.fs(13), .color = t.text_dim});
        // The tab bar sits under the title until it reaches the top, then stays there over the content.
        const float bar_natural = origin + 104, bar_y = std::max(r.y, bar_natural);
        ui.push_clip({r.x, bar_y + bar_h, r.w, std::max(0.f, r.bottom() - bar_y - bar_h)});

        // Each card is drawn where the layout puts it, its rows stacked from its top, and its height kept for the next
        // frame's layout. The cards rise in one after another as the page opens.
        const float page_alpha = p.alpha();
        float x = 0, w = 0, y = 0, top = 0;
        int drawing = -1;
        auto place = [&](int c) {
            if (drawing >= 0) {
                settings_heights_[static_cast<size_t>(drawing)] = y - top;
            }
            drawing = c;
            if (c < 0) {
                p.set_alpha(page_alpha);
                return;
            }
            const Rect &b = at[4 + static_cast<size_t>(c)];
            const float in = ui.progress(page_time_ + 0.03 * c, 420);
            p.set_alpha(page_alpha * in);
            x = r.x + b.x, w = b.w, y = top = origin + b.y + (1 - in) * 12;
        };
        auto heading = [&](size_t s) {
            p.set_alpha(page_alpha);
            ui.label({r.x + at[s].x, origin + at[s].y, at[s].w, 28}, sections[s], {.size = t.fs(20), .color = t.text});
        };

        // Cards are rows stacked edge to edge: the first and last round the corners, hairlines part the rest.
        const float radius = t.radius_l() * 0.8f;
        const ui::Color fill = t.dark ? t.surface.mix(t.text, 0.04f) : t.bg.mix(t.surface, 0.3f);
        auto row = [&](float h, bool first, bool last) {
            const Rect rr{x, y, w, h};
            p.rect(rr, fill, {first ? radius : 0, first ? radius : 0, last ? radius : 0, last ? radius : 0});
            if (!first) {
                p.rect({x + 18, y, w - 36, 1}, t.border.with_alpha(t.dark ? 0.8f : 1));
            }
            y += h;
            return rr;
        };
        // A row's title and hint on the left of a control control_w wide.
        auto caption = [&](Rect rr, std::string_view title, std::string_view hint, float control_w, float alpha = 1) {
            const float tw = rr.w - 36 - control_w - 16;
            if (hint.empty()) {
                ui.label({rr.x + 18, rr.y, tw, rr.h}, title, {.size = t.fs(14), .color = t.text.with_alpha(alpha)});
                return;
            }
            const float mid = rr.y + rr.h / 2;
            p.text(rr.x + 18, mid - 3, title, {.size = t.fs(14), .color = t.text.with_alpha(alpha)}, tw);
            p.text(rr.x + 18, mid + 16, hint, {.size = t.fs(12), .color = t.text_dim.with_alpha(alpha)}, tw);
        };
        auto toggle_row = [&](std::string_view id, std::string_view title, std::string_view hint, bool &on, bool first, bool last, bool enabled = true) {
            const Rect rr = row(hint.empty() ? 54 : 66, first, last);
            const float a = ui.anim(ui.id(std::string(id) + "-enabled"), enabled ? 1 : 0.4f, 200);
            caption(rr, title, hint, 44, a);
            Fade dim(p, a);
            ui.disabled = !enabled;
            const bool changed = ui.toggle(ui.id(id), {rr.right() - 62, rr.y + (rr.h - 24) / 2, 44, 24}, on);
            ui.disabled = false;
            return changed;
        };
        auto segmented_row = [&](std::string_view id, std::string_view title, std::string_view hint, std::span<const std::string_view> labels, int &selected, bool first,
                                 bool last) {
            float widest = 0;
            for (auto l : labels) {
                widest = std::max(widest, p.measure(l, {.size = t.fs(13)}));
            }
            const float cw = std::min((widest + 30) * static_cast<float>(labels.size()), w * 0.56f);
            const Rect rr = row(hint.empty() ? 58 : 66, first, last);
            caption(rr, title, hint, cw);
            return ui.segmented(ui.id(id), {rr.right() - 18 - cw, rr.y + (rr.h - 32) / 2, cw, 32}, labels, selected);
        };
        // A slider that writes its setting on release; display formats the value it would set.
        auto slider_row = [&](std::string_view id, std::string_view title, std::string_view hint, double current, double low, double high, double step, auto display,
                              std::string_view section, std::string_view key, bool first, bool last) {
            const Rect rr = row(66, first, last);
            const float cw = std::min(250.f, w * 0.42f);
            caption(rr, title, hint, cw);
            const std::string draft_key(id);
            auto draft = settings_sliders_.find(draft_key);
            float value = draft == settings_sliders_.end() ? static_cast<float>((current - low) / (high - low)) : draft->second;
            const ui::Id slider_id = ui.id(id);
            if (ui.slider(slider_id, {rr.right() - 18 - cw, rr.y + (rr.h - 18) / 2, cw - 52, 18}, value)) {
                settings_sliders_[draft_key] = value;
            }
            const double result = std::round((low + static_cast<double>(value) * (high - low)) / step) * step;
            ui.label({rr.right() - 18 - 46, rr.y, 46, rr.h}, display(result), {.size = t.fs(13), .color = t.text_dim, .features = text::tabular_nums}, Align::right);
            if (!ui.is_active(slider_id) && settings_sliders_.contains(draft_key)) {
                set_setting_(section, key, std::format("{:.2f}", result));
                settings_sliders_.erase(draft_key);
            }
        };
        auto button_row = [&](std::string_view id, std::string_view title, std::string_view hint, std::string_view label, std::string_view icon, bool first, bool last) {
            const Rect rr = row(66, first, last);
            const float bw = button_width(ui, label, !icon.empty());
            caption(rr, title, hint, bw);
            return ui.button(ui.id(id), {rr.right() - 18 - bw, rr.y + (rr.h - 34) / 2, bw, 34}, label, ui::ButtonKind::secondary, icon);
        };
        // Colour swatches beside their caption, right-aligned and as large as the room the caption leaves allows; one ring
        // glides to the chosen one. paint draws swatch i into its circle. Returns the swatch clicked, or -1.
        auto swatch_row = [&](std::string_view id, std::string_view title, std::string_view hint, size_t count, int chosen, bool first, bool last, auto paint) {
            const Rect rr = row(hint.empty() ? 58 : 66, first, last);
            const float n = static_cast<float>(count);
            const float label = std::max(p.measure(title, {.size = t.fs(14)}), p.measure(hint, {.size = t.fs(12)}));
            const float space = rr.w - 36 - std::min(label, rr.w * 0.4f) - 24;
            const float d = std::clamp((space - 5 * (n - 1)) / n, 18.f, 24.f), gap = std::clamp((space - d * n) / (n - 1), 5.f, 12.f);
            const float span = d * n + gap * (n - 1), x0 = rr.right() - 18 - span, cy = rr.y + rr.h / 2;
            caption(rr, title, hint, span);
            int clicked = -1;
            ui.push_id(ui.id(id));
            for (size_t i = 0; i < count; ++i) {
                const Rect s{x0 + static_cast<float>(i) * (d + gap), cy - d / 2, d, d};
                const ui::Id sid = ui.id(static_cast<uint64_t>(i));
                const auto it = ui.interact(sid, s.inset(-3));
                const float hot = ui.anim(sid, it.hovered ? 1 : 0, 140);
                const Rect face = s.inset(-1.5f * hot);
                paint(i, face);
                p.rect(face, {}, face.w / 2, t.text.with_alpha(0.12f), 1);
                if (it.clicked) {
                    clicked = static_cast<int>(i);
                }
            }
            const float ring_x = x0 + ui.spring(ui.id("ring-x"), static_cast<float>(std::max(chosen, 0)), 280) * (d + gap);
            const float ring_a = ui.anim(ui.id("ring-a"), chosen >= 0 ? 1 : 0, 160);
            if (ring_a > 0) {
                p.rect({ring_x - 4, cy - d / 2 - 4, d + 8, d + 8}, {}, d / 2 + 4, t.text.with_alpha(0.85f * ring_a), 2);
            }
            ui.pop_id();
            return clicked;
        };
        auto group = [&](std::string_view label) {
            ui.label({x + 2, y, w, 18}, label, {.size = t.fs(12), .color = t.text_dim});
            y += 26;
        };
        bool moving = false;

        // Appearance.
        heading(0);
        place(0);
        {
            // The two themes as small windows, in their own colours with the current accent.
            Config variant = config_;
            std::string errors;
            variant.theme.mode = ThemeMode::dark;
            ui::Theme dark = ui::Theme::from(variant, errors);
            variant.theme.mode = ThemeMode::light;
            ui::Theme light = ui::Theme::from(variant, errors);
            if (t.auto_accent) {
                dark.accent = accent(ui.now, true, moving), light.accent = accent(ui.now, false, moving);
            }
            const Rect rr = row(196, true, true);
            const float tw = (rr.w - 36 - 16) / 2;
            for (size_t i = 0; i < 2; ++i) {
                const ui::Theme &look = i == 0 ? dark : light;
                const Rect tile{rr.x + 18 + static_cast<float>(i) * (tw + 16), rr.y + 18, tw, 130};
                const ui::Id id = ui.id(i == 0 ? "theme-dark" : "theme-light");
                const auto it = ui.interact(id, tile);
                const bool on = (config_.theme.mode == ThemeMode::dark) == (i == 0);
                const float hot = ui.anim(id, it.hovered ? 1 : 0, 180), lit = ui.anim(ui::sub_id(id, 1), on ? 1 : 0, 220);
                const Rect lifted{tile.x, tile.y - 2 * hot, tile.w, tile.h};
                p.shadow({lifted.x, lifted.y + 6, lifted.w, lifted.h}, 12, 18, t.shadow.with_alpha(0.25f + 0.35f * hot));
                theme_mock(ui, lifted, look);
                p.rect(lifted.inset(-4), {}, 15, t.accent.with_alpha(lit), 2);
                p.rect(lifted, {}, 12, look.border.with_alpha(1 - lit), 1);
                const std::string_view name = i == 0 ? lyren::tr("Dark") : lyren::tr("Light");
                ui.label({tile.x, tile.bottom() + 10, tile.w, 24}, name, {.size = t.fs(13), .color = on ? t.text : t.text_dim}, Align::center);
                if (it.clicked && !on) {
                    set_setting_("theme", "mode", i == 0 ? "\"dark\"" : "\"light\"");
                }
            }
        }
        place(1);
        {
            // The accent: the cover's own colour first, then a few fixed ones.
            const bool automatic = config_.theme.accent == "auto";
            int chosen = automatic ? 0 : -1;
            for (size_t i = 0; i < accents.size(); ++i) {
                if (same_color(config_.theme.accent, accents[i])) {
                    chosen = static_cast<int>(i + 1);
                }
            }
            const std::string_view hint = automatic ? lyren::tr("Follows the album cover") : chosen > 0 ? lyren::tr("Fixed color") : std::string_view(config_.theme.accent);
            const int picked = swatch_row("accent", lyren::tr("Accent color"), hint, accents.size() + 1, chosen, true, false, [&](size_t i, Rect s) {
                if (i > 0) {
                    p.rect(s, *ui::parse_color(accents[i - 1]), s.w / 2);
                    return;
                }
                p.rect(s, accent(ui.now, t.dark, moving), s.w / 2);
                if (const Track *track = current()) {
                    cover(ui, s.inset(3), track->cover, s.w / 2 - 3, "");
                }
            });
            if (picked == 0 && !automatic) {
                set_setting_("theme", "accent", "\"auto\"");
            } else if (picked > 0 && picked != chosen) {
                set_setting_("theme", "accent", '"' + std::string(accents[static_cast<size_t>(picked - 1)]) + '"');
            }
        }
        {
            const std::array sizes{lyren::tr("Small"), lyren::tr("Default"), lyren::tr("Large"), lyren::tr("Larger")};
            int size = static_cast<int>(std::ranges::find(text_sizes, config_.theme.font_size) - text_sizes.begin());
            size = size < static_cast<int>(text_sizes.size()) ? size : -1;
            if (segmented_row("text-size", lyren::tr("Text size"), {}, sizes, size, false, false)) {
                set_setting_("theme", "font_size", std::to_string(text_sizes[static_cast<size_t>(size)]));
            }
        }
        {
            const std::array labels{lyren::tr("Off"), lyren::tr("Standard"), lyren::tr("Relaxed")};
            int motion = static_cast<int>(std::ranges::find(motions, config_.theme.motion) - motions.begin());
            motion = motion < static_cast<int>(motions.size()) ? motion : -1;
            if (segmented_row("motion", lyren::tr("Motion"), {}, labels, motion, false, false)) {
                set_setting_("theme", "motion", std::format("{:.1f}", motions[static_cast<size_t>(motion)]));
            }
        }
        slider_row(
            "opacity", lyren::tr("Window opacity"), lyren::tr("Below 100% the desktop shows through, blurred"), config_.window.opacity, 0.3, 1, 0.05,
            [](double v) {
                return std::format("{:.0f}%", v * 100);
            },
            "window", "opacity", false, true);

        // Playback.
        heading(1);
        place(2);
        {
            const Rect head = row(60, true, false);
            caption(head, lyren::tr("Audio quality"), lyren::tr("What you get depends on your accounts"), 0);
            const std::array<std::string_view, 5> names{lyren::tr("Standard"), lyren::tr("High"), lyren::tr("Very high"), lyren::tr("Lossless"), "Hi-Res"};
            const std::array<std::string_view, 5> details{"128 kbps", "192 kbps", "320 kbps", "FLAC", "FLAC 24 bit"};
            const std::array<std::string_view, 5> values{"standard", "higher", "exhigh", "lossless", "hires"};
            for (size_t i = 0; i < names.size(); ++i) {
                const Rect rr = row(46, false, i + 1 == names.size());
                const ui::Id id = ui.id(values[i]);
                const auto it = ui.interact(id, rr);
                const bool on = static_cast<size_t>(config_.playback.quality) == i;
                const float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
                if (hot > 0) {
                    p.rect(rr.inset(6, 4), t.hover.with_alpha(hot), t.radius_s());
                }
                ui.label({rr.x + 18, rr.y, 160, rr.h}, names[i], {.size = t.fs(14), .color = on ? t.text : t.text.mix(t.text_dim, 0.35f)});
                ui.label({rr.x + 160, rr.y, rr.w - 220, rr.h}, details[i], {.size = t.fs(12), .color = t.text_dim});
                const float check = ui.anim(ui::sub_id(id, 1), on ? 1 : 0, 180);
                if (check > 0) {
                    p.icon("check", ui::center({rr.right() - 46, rr.y, 28, rr.h}, 17 * (0.6f + 0.4f * check), 17 * (0.6f + 0.4f * check)), t.accent.with_alpha(check));
                }
                if (it.clicked && !on) {
                    set_setting_("playback", "quality", '"' + std::string(values[i]) + '"');
                }
            }
        }
        place(3);
        {
            bool skip = skip_trials();
            if (toggle_row("trial", lyren::tr("Skip previews"), lyren::tr("Pass over songs that only play a short clip"), skip, true, false)) {
                set_setting_("playback", "trial", skip ? "\"skip\"" : "\"play\"");
            }
            bool normalize = config_.playback.normalize;
            if (toggle_row("normalize", lyren::tr("Volume normalization"), lyren::tr("Even out loudness between songs"), normalize, false, true)) {
                set_setting_("playback", "normalize", normalize ? "true" : "false");
            }
        }

        // Lyrics on the desktop.
        heading(2);
        place(4);
        {
            bool shown = dl_enabled_;
            if (toggle_row("desktop-lyrics", lyren::tr("Desktop lyrics"), lyren::tr("Show the line being sung over your desktop"), shown, true, false)) {
                set_desktop_enabled(shown);
            }
            // The two ways to show them, as small desktops.
            const Rect rr = row(168, false, true);
            const float tw = (rr.w - 36 - 16) / 2;
            for (size_t i = 0; i < 2; ++i) {
                const bool island = i == 1;
                const Rect tile{rr.x + 18 + static_cast<float>(i) * (tw + 16), rr.y + 14, tw, 104};
                const ui::Id id = ui.id(island ? "mode-island" : "mode-floating");
                const auto it = ui.interact(id, tile);
                const bool on = dl_island_ == island;
                const float hot = ui.anim(id, it.hovered ? 1 : 0, 180), lit = ui.anim(ui::sub_id(id, 1), on ? 1 : 0, 220);
                const Rect lifted{tile.x, tile.y - 2 * hot, tile.w, tile.h};
                desktop_mock(ui, lifted, island);
                p.rect(lifted.inset(-4), {}, 15, t.accent.with_alpha(lit), 2);
                ui.label({tile.x, tile.bottom() + 10, tile.w, 24}, island ? lyren::tr("Dynamic Island") : lyren::tr("Floating lyrics"),
                         {.size = t.fs(13), .color = on ? t.text : t.text_dim}, Align::center);
                if (it.clicked) {
                    set_desktop_island(island);
                    if (!dl_enabled_) {
                        set_desktop_enabled(true);
                    }
                }
            }
        }
        place(5);
        {
            group(lyren::tr("Floating lyrics"));
            // The line as it shows on the desktop, over the blurred cover.
            const float size = t.lyric_size, sub = std::round(size * 0.58f);
            const Rect rr = row(std::max(112.f, std::round(size * 1.35f) + std::round(sub * 1.35f) + 44), true, false);
            const Rect wall = rr.inset(10);
            wallpaper(ui, wall, radius - 4);
            ui.push_clip(wall);
            {
                UseTheme on_desktop(ui, *stage_);
                const double now = clip_start_ + position();
                const int at_line = lyric_at(lyrics_, now + 0.2);
                const LyricLine *line = at_line >= 0 ? &lyrics_[static_cast<size_t>(at_line)] : nullptr;
                const std::string_view text = line ? std::string_view(line->text) : current() ? std::string_view(current()->title) : lyren::tr("Lyrics appear here");
                const std::string_view below = line ? (lyric_translation_ ? std::string_view(line->translation) : std::string_view{}) : current() ? current()->artist : "";
                const float lh = std::round(size * 1.35f), line_top = wall.y + (wall.h - lh - (below.empty() ? 0 : sub * 1.35f)) / 2;
                const ui::Color sung = t.lyric_sung.value_or(stage_->accent);
                const bool timed = line && !line->words.empty();
                floating_text(ui, wall.x + wall.w / 2, line_top + lh * 0.76f, text, timed ? line : nullptr, size, wall.w - 32, timed ? sung : t.lyric_text, t.lyric_text);
                if (!below.empty()) {
                    floating_text(ui, wall.x + wall.w / 2, line_top + lh + sub * 1.1f, below, nullptr, sub, wall.w - 32, t.lyric_text.with_alpha(0.85f), t.lyric_text);
                }
                lyric_frames(ui, at_line, timed);
            }
            ui.pop_clip();
            slider_row(
                "lyric-size", lyren::tr("Text size"), {}, config_.desktop_lyrics.font_size, 16, 72, 1,
                [](double v) {
                    return std::format("{:.0f}", v);
                },
                "desktop_lyrics", "font_size", false, false);
            {
                const bool follows = config_.desktop_lyrics.highlight == "accent";
                int chosen = follows ? 0 : -1;
                for (size_t i = 0; i < sung_colors.size(); ++i) {
                    if (same_color(config_.desktop_lyrics.highlight, sung_colors[i])) {
                        chosen = static_cast<int>(i + 1);
                    }
                }
                const int picked = swatch_row("sung", lyren::tr("Sung words"), {}, sung_colors.size() + 1, chosen, false, false, [&](size_t i, Rect s) {
                    p.rect(s, i == 0 ? stage_->accent : *ui::parse_color(sung_colors[i - 1]), s.w / 2);
                    if (i == 0) {
                        p.icon("sparkles", ui::center(s, 12, 12), white.with_alpha(0.9f));
                    }
                });
                if (picked >= 0 && picked != chosen) {
                    set_setting_("desktop_lyrics", "highlight", picked == 0 ? "\"accent\"" : '"' + std::string(sung_colors[static_cast<size_t>(picked - 1)]) + '"');
                }
            }
            {
                int chosen = -1;
                for (size_t i = 0; i < rest_colors.size(); ++i) {
                    if (same_color(config_.desktop_lyrics.color, rest_colors[i])) {
                        chosen = static_cast<int>(i);
                    }
                }
                const int picked = swatch_row("rest", lyren::tr("Other words"), {}, rest_colors.size(), chosen, false, false, [&](size_t i, Rect s) {
                    p.rect(s, *ui::parse_color(rest_colors[i]), s.w / 2);
                });
                if (picked >= 0 && picked != chosen) {
                    set_setting_("desktop_lyrics", "color", '"' + std::string(rest_colors[static_cast<size_t>(picked)]) + '"');
                }
            }
            bool outlined = t.lyric_outline.a > 0;
            if (toggle_row("outline", lyren::tr("Outline"), lyren::tr("Keeps the words readable over bright wallpapers"), outlined, false, true)) {
                set_setting_("desktop_lyrics", "outline", outlined ? outline_on : outline_off);
            }
        }
        place(6);
        {
            group(lyren::tr("Dynamic Island"));
            // The island itself, live: point at it to open it. Its space grows as it does.
            ui.push_id(ui.id("island-preview"));
            const float open = std::clamp(ui.spring_value(ui.id("island-open"), 0), 0.f, 1.2f);
            const auto &island_options = config_.desktop_lyrics.island;
            const Rect rr = row(std::lerp(92.f, 268.f, open), true, false);
            const Rect wall = rr.inset(10);
            wallpaper(ui, wall, radius - 4);
            ui.push_clip(wall);
            {
                UseTheme on_desktop(ui, *stage_);
                island(ui, wall.x + wall.w / 2, wall.y + 16);
            }
            ui.pop_clip();
            ui.pop_id();
            bool cover_on = island_options.cover;
            if (toggle_row("island-cover", lyren::tr("Cover"), lyren::tr("The album cover at the start of the island"), cover_on, false, false)) {
                set_setting_("desktop_lyrics.island", "cover", cover_on ? "true" : "false");
            }
            // The ring goes round the cover, so it needs the cover.
            bool ring = island_options.cover && island_options.progress;
            if (toggle_row("island-ring", lyren::tr("Progress ring"), island_options.cover ? lyren::tr("Playback progress around the cover") : lyren::tr("Needs the cover"), ring,
                           false, true, island_options.cover)) {
                set_setting_("desktop_lyrics.island", "progress", ring ? "true" : "false");
            }
        }

        // General.
        heading(3);
        place(7);
        {
            const std::array<std::string_view, 2> language_labels{"中文", "English"};
            int language = config_.interface.language == Language::zh ? 0 : 1;
            if (segmented_row("language", lyren::tr("Language"), {}, language_labels, language, true, false)) {
                set_setting_("interface", "language", language == 0 ? "\"zh\"" : "\"en\"");
            }
            const std::array titles{lyren::tr("Built in"), lyren::tr("System")};
            int title_bar = static_cast<int>(config_.window.decorations);
            if (segmented_row("decorations", lyren::tr("Title bar"), lyren::tr("System draws the desktop's own title bar"), titles, title_bar, false, false)) {
                set_setting_("window", "decorations", title_bar == 0 ? "\"client\"" : "\"server\"");
            }
            {
                // Frame rate: a menu, since it has many choices.
                const Rect rr = row(66, false, false);
                const float cw = 150;
                caption(rr, lyren::tr("Animation frame rate"), lyren::tr("Lower rates save power"), cw);
                auto name = [](int fps) {
                    return fps == 0 ? std::string(lyren::tr("Display rate")) : std::to_string(fps) + " fps";
                };
                const Rect control{rr.right() - 18 - cw, rr.y + (rr.h - 34) / 2, cw, 34};
                const ui::Id id = ui.id("frame-rate");
                const auto it = ui.interact(id, control);
                const float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
                p.rect(control, t.text.with_alpha(0.06f + 0.04f * hot), control.h / 2);
                ui.label({control.x + 14, control.y, control.w - 40, control.h}, name(config_.render.max_fps), {.size = t.fs(13), .color = t.text});
                p.icon("chevron-down", {control.right() - 26, control.y + 10, 14, 14}, t.text_dim);
                if (it.clicked) {
                    std::vector<ui::MenuItem> menu;
                    for (int fps : frame_rates) {
                        menu.push_back({config_.render.max_fps == fps ? "check" : "", name(fps), [this, fps] {
                                            set_setting_("render", "max_fps", std::to_string(fps));
                                        }});
                    }
                    ui.open_menu(control.x, control.bottom() + 6, std::move(menu));
                }
            }
            const std::array gpus{lyren::tr("Integrated"), lyren::tr("Dedicated")};
            int gpu = static_cast<int>(config_.gpu.device);
            if (segmented_row("gpu", lyren::tr("Graphics"), lyren::tr("Takes effect after a restart"), gpus, gpu, false, true)) {
                set_setting_("gpu", "device", gpu == 0 ? "\"integrated\"" : "\"discrete\"");
            }
        }
        place(8);
        if (button_row("config-file", lyren::tr("Config file"), lyren::tr("Fonts, corner radius and individual colors live here"), lyren::tr("Open"), "file-cog", true, false)) {
            open_config_();
        }
        if (button_row("reset", lyren::tr("Reset appearance"), lyren::tr("Theme colors, text size, accent and motion"), lyren::tr("Reset"), "rotate-ccw", false, true)) {
            set_setting_("theme", "accent", "\"auto\"");
            set_setting_("theme", "font", "\"sans-serif\"");
            set_setting_("theme", "font_size", "14");
            set_setting_("theme", "radius", "10");
            set_setting_("theme", "motion", "1.0");
            set_setting_("window", "opacity", "1.0");
            for (std::string_view key : {"bg", "surface", "elevated", "border", "text", "text_dim", "danger"}) {
                set_setting_("theme.colors", key, {});
            }
        }
        place(-1);
        if (moving) {
            ui.keep_animating();
        }
        ui.pop_clip();

        // The tab bar: it follows the scroll, and a tab glides there.
        const float stuck = std::clamp((r.y - bar_natural) / 12, 0.f, 1.f);
        p.rect({r.x, bar_y, r.w, bar_h}, t.surface, {r.y >= bar_y ? t.radius_l() : 0, r.y >= bar_y ? t.radius_l() : 0, 0, 0});
        p.rect({r.x, bar_y + bar_h - 1, r.w, 1}, t.border.with_alpha(stuck));
        const float max_offset = std::max(0.f, layout[13].h - r.h);
        int active = 0;
        for (size_t i = 0; i < settings_tops_.size(); ++i) {
            if (origin + settings_tops_[i] <= bar_y + bar_h + (r.bottom() - bar_y - bar_h) * 0.35f) {
                active = static_cast<int>(i);
            }
        }
        if (off >= max_offset - 1 && max_offset > 0) {
            active = static_cast<int>(sections.size()) - 1;
        }
        if (ui.now < settings_jump_until_) {
            active = settings_jump_;
        }
        settings_section_ = active;
        int picked = active;
        if (ui.tabs(ui.id("settings-tabs"), {frame.x, bar_y + 8, frame.w, 36}, sections, picked)) {
            settings_jump_ = picked, settings_jump_until_ = ui.now + 0.6 * t.motion;
            ui.scroll_to(scroll_id, settings_tops_[static_cast<size_t>(picked)] - bar_h - 20, true);
        }
        ui.end_scroll();
    }

    // A window in miniature, in the colours of a theme: sidebar, page with covers, player bar.
    void Shell::theme_mock(ui::Ui &ui, Rect m, const ui::Theme &look) {
        auto &p = ui.painter();
        p.rect(m, look.bg, 12);
        const Rect side{m.x + 10, m.y + 14, m.w * 0.22f, m.h - 44};
        for (int i = 0; i < 4; ++i) {
            const float bw = side.w - 6 - static_cast<float>(i % 2) * 8;
            p.rect({side.x, side.y + static_cast<float>(i) * 12, bw, 6}, i == 0 ? look.accent.with_alpha(0.9f) : look.text.with_alpha(0.16f), 3);
        }
        const Rect page{side.right() + 8, m.y + 8, m.right() - side.right() - 16, m.h - 38};
        p.rect(page, look.surface, 7);
        p.rect({page.x + 10, page.y + 10, page.w * 0.34f, 7}, look.text.with_alpha(0.75f), 3.5f);
        const float cw = (page.w - 20 - 16) / 3;
        for (int i = 0; i < 3; ++i) {
            const Rect c{page.x + 10 + static_cast<float>(i) * (cw + 8), page.y + 26, cw, std::min(cw, page.h - 34)};
            if (i == 0 && current()) {
                cover(ui, c, current()->cover, 4, "");
            } else {
                p.rect(c, ui::hsl(0.58f + 0.17f * static_cast<float>(i), 0.45f, look.dark ? 0.42f : 0.7f), 4);
            }
        }
        const float by = m.bottom() - 18;
        p.rect({m.x + 10, by - 5, 12, 12}, look.text.with_alpha(0.22f), 3);
        p.rect({m.x + 28, by - 3, m.w * 0.16f, 4}, look.text.with_alpha(0.3f), 2);
        p.rect({m.x + m.w * 0.42f, by, m.w * 0.36f, 2.5f}, look.text.with_alpha(0.16f), 1.25f);
        p.rect({m.x + m.w * 0.42f, by, m.w * 0.13f, 2.5f}, look.accent, 1.25f);
        p.rect({m.x + m.w * 0.42f - 13, by - 4, 9, 9}, look.text.with_alpha(0.85f), 4.5f);
    }

    // The current cover, blurred, standing in for a desktop wallpaper.
    void Shell::wallpaper(ui::Ui &ui, Rect r, float radius) {
        auto &p = ui.painter();
        p.gradient(r, ui::hsl(0.62f, 0.35f, 0.42f), ui::hsl(0.7f, 0.3f, 0.2f), radius);
        if (const Track *track = current()) {
            const auto &e = images_.get(cover_url(track->cover, 128), 128, true);
            if (e.ready) {
                const float aspect = r.w / r.h;
                const Rect uv = aspect > 1 ? Rect{0, (1 - 1 / aspect) / 2, 1, 1 / aspect} : Rect{(1 - aspect) / 2, 0, aspect, 1};
                Fade fade(p, ui.progress(e.ready_at, 400));
                p.image(r, e.tex, radius, {1, 1, 1, 1}, uv);
            }
        }
        p.gradient(r, {0, 0, 0, 0.05f}, {0, 0, 0, 0.25f}, radius);
    }

    // A desktop in miniature with lyrics on it: a line near the bottom, or the island at the top.
    void Shell::desktop_mock(ui::Ui &ui, Rect m, bool island) {
        auto &p = ui.painter();
        wallpaper(ui, m, 12);
        constexpr ui::Color ink{0.03f, 0.03f, 0.035f, 1};
        if (island) {
            const Rect pill{m.x + m.w * 0.3f, m.y + 8, m.w * 0.4f, 18};
            p.shadow({pill.x, pill.y + 2, pill.w, pill.h}, 9, 8, {0, 0, 0, 0.3f});
            p.rect(pill, ink, 9);
            if (config_.desktop_lyrics.island.cover) {
                p.rect({pill.x + 4, pill.y + 4, 10, 10}, ui::Color{1, 1, 1, 0.75f}, 5);
            }
            p.rect({pill.x + (config_.desktop_lyrics.island.cover ? 20.f : 10.f), pill.y + 7, pill.w * 0.5f, 4}, ui::Color{1, 1, 1, 0.85f}, 2);
            return;
        }
        const float cx = m.x + m.w / 2, by = m.bottom() - 26;
        p.rect({cx - m.w * 0.3f - 1.5f, by - 1.5f, m.w * 0.6f + 3, 9}, {0, 0, 0, 0.35f}, 4.5f);
        p.rect({cx - m.w * 0.3f, by, m.w * 0.6f, 6}, ui::Color{1, 1, 1, 0.95f}, 3);
        p.rect({cx - m.w * 0.18f, by + 12, m.w * 0.36f, 4}, ui::Color{1, 1, 1, 0.7f}, 2);
    }

} // namespace lyren
