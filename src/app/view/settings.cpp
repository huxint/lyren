#include "app/view/settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    namespace {
        // Accents and lyric colours that read on both themes and over a wallpaper.
        constexpr std::array<std::string_view, 8> accents{"#e5484d", "#f76b15", "#e2a336", "#30a46c", "#12a594", "#0090ff", "#6e56cf", "#d6409f"};
        constexpr std::array<std::string_view, 5> sung_colors{"#ffffff", "#ffd166", "#ff8fab", "#7ee0e6", "#9be9a8"};
        constexpr std::array<std::string_view, 4> rest_colors{"#ffffff", "#c9c9d2", "#ffe7ad", "#cdeaff"};
        constexpr std::string_view outline_on = "\"#000000a0\"", outline_off = "\"#00000000\"";
        constexpr std::array<double, 4> text_sizes{13, 14, 15, 16};
        constexpr std::array<double, 2> motions{1, 1.5};
        constexpr std::array<int, 6> frame_rates{0, 30, 60, 90, 120, 144};

        struct Category {
            std::string_view icon, title, subtitle;
        };
        constexpr std::array<Category, 4> categories{{
            {"sparkles", "Appearance", "Theme colors, text size, accent and motion"},
            {"headphones", "Playback", "Audio quality and listening behavior"},
            {"captions", "Lyrics", "Show the line being sung over your desktop"},
            {"settings", "General", "Language, rendering and configuration"},
        }};

        bool same_color(std::string_view a, std::string_view b) {
            auto x = ui::parse_color(a), y = ui::parse_color(b);
            return x && y && ui::format_color(*x) == ui::format_color(*y);
        }
    } // namespace

    void SettingsPage::draw(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();

        // A small dead band keeps resizing near the breakpoint from flipping between the side rail and top tabs.
        if (r.w >= 872) {
            rail_ = true;
        } else if (r.w < 848) {
            rail_ = false;
        }

        const std::array<std::string_view, 4> tab_labels{lyren::tr(categories[0].title), lyren::tr(categories[1].title), lyren::tr(categories[2].title),
                                                         lyren::tr(categories[3].title)};

        auto select_tab = [&](int next) {
            if (next == tab_) {
                return;
            }
            tab_dir_ = next > tab_ ? 1 : -1;
            tab_ = next;
            tab_since_ = ui.now;
            ui.scroll_to(ui::sub_id(ui.id("settings-pane"), static_cast<uint64_t>(tab_)), 0);
        };

        Rect viewport;
        float content_x = 0, content_w = 0;
        const bool show_header_in_pane = rail_;
        if (rail_) {
            constexpr float rail_w = 204, rail_gap = 40;
            const float canvas = std::min(r.w - 64.f, 964.f);
            const float left = r.x + std::round((r.w - canvas) / 2);
            const Rect nav{left, r.y + 28, rail_w, r.h - 56};

            ui.label({nav.x + 8, nav.y, nav.w - 8, 38}, lyren::tr("Settings"), t.style(type::display, t.text, text::semibold));
            constexpr float nav_top = 58, item_h = 38, step = 42;
            const float target_y = nav_top + static_cast<float>(tab_) * step;
            const float pill_y = ui.spring(ui.id("settings-rail-sel"), target_y, 240);
            p.rect({nav.x, nav.y + pill_y, nav.w, item_h}, t.fill(1.45f), t.radius_s());
            const ui::Id mark_top_id = ui.id("settings-mark-top"), mark_bot_id = ui.id("settings-mark-bot");
            const bool moving_down = target_y + 9 > ui.spring_value(mark_top_id, target_y + 9);
            const float mt = ui.spring(mark_top_id, target_y + 9, moving_down ? 340 : 190);
            const float mb = ui.spring(mark_bot_id, target_y + item_h - 9, moving_down ? 190 : 340);
            p.rect({nav.x + 2, nav.y + mt, 3, mb - mt}, t.accent, 1.5f);

            for (size_t i = 0; i < categories.size(); ++i) {
                const Rect item{nav.x, nav.y + nav_top + static_cast<float>(i) * step, nav.w, item_h};
                const ui::Id id = ui::sub_id(ui.id("settings-nav"), i);
                const auto it = ui.interact(id, item);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const auto [hot, down] = ui.feel(id, it, pace::quick);
                const bool on = static_cast<int>(i) == tab_;
                if (hot > 0 && !on) {
                    p.rect(item.inset(down), t.fill(hot), t.radius_s());
                }
                p.icon(categories[i].icon, {item.x + 14, item.y + (item.h - 17) / 2, 17, 17}, on ? t.accent : t.text_dim.mix(t.text, hot * 0.7f));
                const ui::Color ink_color = on ? t.text : t.text_dim.mix(t.text, 0.45f + 0.55f * hot);
                ui.label({item.x + 42, item.y, item.w - 50, item.h}, tab_labels[i], t.style(type::body, ink_color, on ? text::medium : 0));
                if (it.clicked) {
                    select_tab(static_cast<int>(i));
                }
            }

            p.rect({nav.right() + rail_gap / 2, r.y + 28, 1, r.h - 56}, t.line(0.55f));
            content_x = nav.right() + rail_gap;
            content_w = canvas - rail_w - rail_gap;
            viewport = {content_x - 8, r.y, content_w + 16, r.h};
        } else {
            const float pad = r.w < 580 ? 20.f : 32.f;
            content_w = std::min(r.w - 2 * pad, 720.f);
            content_x = r.x + std::round((r.w - content_w) / 2);
            ui.label({content_x, r.y + 24, content_w, 38}, lyren::tr("Settings"), t.style(type::display, t.text, text::semibold));
            constexpr float bar_y = 74, bar_h = 40;
            int picked = tab_;
            if (ui.tabs(ui.id("settings-tabs"), {content_x, r.y + bar_y, content_w, bar_h}, tab_labels, picked)) {
                select_tab(picked);
            }
            p.rect({r.x, r.y + bar_y + bar_h - 1, r.w, 1}, t.line(0.85f));
            viewport = {content_x - 8, r.y + bar_y + bar_h, content_w + 16, std::max(0.f, r.h - bar_y - bar_h)};
        }

        const ui::Id pane_scroll = ui::sub_id(ui.id("settings-pane"), static_cast<uint64_t>(tab_));
        const float known_h = heights_[static_cast<size_t>(tab_)];
        const float off = ui.begin_scroll(pane_scroll, viewport, std::max(known_h, viewport.h));
        const float enter = ui.progress(std::max(browser_.entered, tab_since_), pace::arrive);

        float x = content_x, w = content_w;
        const float start_y = viewport.y + (show_header_in_pane ? 28.f : 22.f) - off + (1 - enter) * 8;
        float y = start_y;
        const float page_alpha = p.alpha();
        Fade tab_fade(p, enter);

        if (show_header_in_pane) {
            const auto &cat = categories[static_cast<size_t>(tab_)];
            ui.label({x, y, w, 30}, lyren::tr(cat.title), t.style(type::title, t.text, text::semibold));
            ui.label({x, y + 32, w, 20}, lyren::tr(cat.subtitle), t.style(type::label, t.text_dim));
            y += 68;
        }

        const float radius = t.radius_l() * 0.8f;
        const ui::Color card_fill = t.dark ? t.fill(0.92f) : t.bg.mix(t.surface, 0.35f);
        float card_top = y;
        bool in_card = false;
        int card_index = 0;

        auto begin_card = [&] {
            if (card_index > 0) {
                y += 16;
            }
            card_top = y;
            in_card = true;
            ++card_index;
        };
        auto end_card = [&] {
            if (in_card && y > card_top) {
                p.rect({x, card_top, w, y - card_top}, {}, radius, t.surface_edge(0.95f), 1);
            }
            in_card = false;
        };
        auto row = [&](float h, bool first, bool last) {
            if (first) {
                begin_card();
            }
            const Rect rr{x, y, w, h};
            p.rect(rr, card_fill, {first ? radius : 0, first ? radius : 0, last ? radius : 0, last ? radius : 0});
            if (!first) {
                p.rect({x + 20, y, w - 40, 1}, t.line(0.85f));
            }
            y += h;
            if (last) {
                end_card();
            }
            return rr;
        };
        auto caption = [&](Rect rr, std::string_view title, std::string_view hint, float control_w, float alpha = 1) {
            const float tw = std::max(0.f, rr.w - 40 - control_w - (control_w > 0 ? 16.f : 0.f));
            if (hint.empty()) {
                ui.label({rr.x + 20, rr.y, tw, rr.h}, title, t.style(type::body, t.text.with_alpha(alpha)));
                return;
            }
            const float mid = rr.y + rr.h / 2;
            p.text(rr.x + 20, mid - 3, title, t.style(type::body, t.text.with_alpha(alpha)), tw);
            p.text(rr.x + 20, mid + 16, hint, t.style(type::caption, t.text_dim.with_alpha(alpha)), tw);
        };
        auto toggle_row = [&](std::string_view id, std::string_view title, std::string_view hint, bool &on, bool first, bool last, bool enabled = true) {
            const Rect rr = row(hint.empty() ? 56 : 68, first, last);
            const float a = ui.anim(ui.id(std::string(id) + "-enabled"), enabled ? 1 : 0.4f, pace::steady);
            caption(rr, title, hint, 44, a);
            Fade dim(p, a);
            ui::Enabled live(ui, enabled, 1);
            return ui.toggle(ui.id(id), {rr.right() - 64, rr.y + (rr.h - 24) / 2, 44, 24}, on);
        };
        auto segmented_row = [&](std::string_view id, std::string_view title, std::string_view hint, std::span<const std::string_view> labels, int &selected, bool first,
                                 bool last) {
            float widest = 0;
            for (auto l : labels) {
                widest = std::max(widest, p.measure(l, t.style(type::label, {}, text::medium)));
            }
            const float ideal = (widest + 28) * static_cast<float>(labels.size());
            const float label_w = std::max(p.measure(title, t.style(type::body, {})), p.measure(hint, t.style(type::caption, {})));
            const bool stacked = w - 40 - ideal - 20 < std::min(label_w, 180.f);
            if (stacked) {
                const float head_h = hint.empty() ? 42.f : 56.f;
                const Rect rr = row(head_h + 48, first, last);
                caption({rr.x, rr.y + 4, rr.w, head_h}, title, hint, 0);
                return ui.segmented(ui.id(id), {rr.x + 20, rr.y + head_h + 4, rr.w - 40, 32}, labels, selected);
            }
            const float cw = std::min(ideal, w * 0.62f);
            const Rect rr = row(hint.empty() ? 58 : 68, first, last);
            caption(rr, title, hint, cw);
            return ui.segmented(ui.id(id), {rr.right() - 20 - cw, rr.y + (rr.h - 32) / 2, cw, 32}, labels, selected);
        };
        auto slider_row = [&](std::string_view id, std::string_view title, std::string_view hint, double current, double low, double high, double step, auto display,
                              std::string_view section, std::string_view key, bool first, bool last) {
            const Rect rr = row(64, first, last);
            const float cw = std::min(260.f, w * 0.48f);
            caption(rr, title, hint, cw);
            auto draft = sliders_.find(id);
            float value = draft == sliders_.end() ? static_cast<float>((current - low) / (high - low)) : draft->second;
            const ui::Id slider_id = ui.id(id);
            if (ui.slider(slider_id, {rr.right() - 20 - cw, rr.y + (rr.h - 18) / 2, cw - 52, 18}, value)) {
                sliders_[id] = value;
            }
            const double result = std::round((low + static_cast<double>(value) * (high - low)) / step) * step;
            ui.label({rr.right() - 20 - 46, rr.y, 46, rr.h}, display(result), t.style(type::label, t.text_dim, text::tabular_nums), Align::right);
            if (!ui.is_active(slider_id) && sliders_.contains(id)) {
                shell_.set_setting(section, key, std::format("{:.2f}", result));
                sliders_.erase(id);
            }
        };
        auto button_row = [&](std::string_view id, std::string_view title, std::string_view hint, std::string_view label, std::string_view icon, bool first, bool last) {
            const Rect rr = row(68, first, last);
            const float bw = ui.button_width(label, !icon.empty());
            caption(rr, title, hint, bw);
            return ui.button(ui.id(id), {rr.right() - 20 - bw, rr.y + (rr.h - control::medium) / 2, bw, control::medium}, label, ui::ButtonKind::secondary, icon);
        };
        auto swatch_row = [&](std::string_view id, std::string_view title, std::string_view hint, size_t count, int chosen, bool first, bool last, auto paint) {
            const float n = static_cast<float>(count);
            const float label_w = std::max(p.measure(title, t.style(type::body, {})), p.measure(hint, t.style(type::caption, {})));
            const bool stacked = w - 40 - std::min(label_w, w * 0.4f) - 24 < n * 22 + (n - 1) * 6;
            const float head_h = hint.empty() ? 42.f : 56.f;
            const Rect rr = row(stacked ? head_h + 44 : (hint.empty() ? 60 : 68), first, last);
            float d = 24, gap = 10, x0 = 0, cy = 0;
            if (stacked) {
                caption({rr.x, rr.y + 4, rr.w, head_h}, title, hint, 0);
                const float avail = rr.w - 40;
                d = std::clamp((avail - 6 * (n - 1)) / n, 18.f, 24.f);
                gap = std::clamp((avail - d * n) / std::max(n - 1, 1.f), 6.f, 12.f);
                x0 = rr.x + 20;
                cy = rr.y + head_h + 18;
            } else {
                const float space = rr.w - 40 - std::min(label_w, rr.w * 0.42f) - 24;
                d = std::clamp((space - 6 * (n - 1)) / n, 18.f, 24.f);
                gap = std::clamp((space - d * n) / std::max(n - 1, 1.f), 6.f, 12.f);
                const float span = d * n + gap * (n - 1);
                x0 = rr.right() - 20 - span;
                cy = rr.y + rr.h / 2;
                caption(rr, title, hint, span);
            }
            int clicked = -1;
            ui.push_id(ui.id(id));
            for (size_t i = 0; i < count; ++i) {
                const Rect s{x0 + static_cast<float>(i) * (d + gap), cy - d / 2, d, d};
                const ui::Id sid = ui.id(static_cast<uint64_t>(i));
                const auto it = ui.interact(sid, s.inset(-3));
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const auto [hot, down] = ui.feel(sid, it, pace::quick);
                const Rect face = s.inset(-1.2f * hot + 1.f * down);
                paint(i, face);
                p.rect(face, {}, face.w / 2, t.text.with_alpha(0.14f), 1);
                if (it.clicked) {
                    clicked = static_cast<int>(i);
                }
            }
            const float ring_x = x0 + ui.spring(ui.id("ring-x"), static_cast<float>(std::max(chosen, 0)), pace::glide) * (d + gap);
            const float ring_a = ui.anim(ui.id("ring-a"), chosen >= 0 ? 1 : 0, pace::quick);
            if (ring_a > 0) {
                p.rect({ring_x - 4, cy - d / 2 - 4, d + 8, d + 8}, {}, d / 2 + 4, t.text.with_alpha(0.85f * ring_a), 2);
            }
            ui.pop_id();
            return clicked;
        };
        auto check_badge = [&](Rect tile, float lit) {
            if (lit <= 0.01f) {
                return;
            }
            const Rect badge{tile.right() - 26, tile.y + 8, 18, 18};
            Fade fade(p, lit);
            p.rect(badge, t.accent, 9);
            p.icon("check", ui::center(badge, 11, 11), t.on_accent());
        };

        bool moving = false;

        if (tab_ == 0) {
            // Appearance: theme studio card, style/motion card, and reset card.
            Config variant = env_.config;
            std::string errors;
            variant.theme.mode = ThemeMode::dark;
            ui::Theme dark = ui::Theme::from(variant, errors);
            variant.theme.mode = ThemeMode::light;
            ui::Theme light = ui::Theme::from(variant, errors);
            if (t.auto_accent) {
                dark.accent = shell_.accent(ui.now, true, moving), light.accent = shell_.accent(ui.now, false, moving);
            }
            const float tw = (w - 40 - 16) / 2;
            const float tile_h = std::round(std::clamp(tw * 0.56f, 112.f, 162.f));
            const Rect rr = row(tile_h + 68, true, true);
            for (size_t i = 0; i < 2; ++i) {
                const ui::Theme &look = i == 0 ? dark : light;
                const Rect tile{rr.x + 20 + static_cast<float>(i) * (tw + 16), rr.y + 20, tw, tile_h};
                const ui::Id id = ui.id(i == 0 ? "theme-dark" : "theme-light");
                const auto it = ui.interact(id, tile);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const bool on = (env_.config.theme.mode == ThemeMode::dark) == (i == 0);
                const auto [hot, down] = ui.feel(id, it, pace::steady);
                const float lit = ui.anim(ui::sub_id(id, 1), on ? 1 : 0, pace::steady);
                const Rect lifted = Rect{tile.x, tile.y - 1.5f * hot, tile.w, tile.h}.inset(down * 1.2f);
                p.shadow({lifted.x, lifted.y + 6, lifted.w, lifted.h}, 12, 16, t.shadow.with_alpha(0.22f + 0.30f * hot));
                theme_mock(ui, lifted, look);
                p.rect(lifted.inset(-3), {}, 14, t.accent.with_alpha(lit), 2);
                p.rect(lifted, {}, 12, look.surface_edge(1.3f).with_alpha(1 - lit), 1);
                check_badge(lifted, lit);
                const std::string_view name = i == 0 ? lyren::tr("Dark") : lyren::tr("Light");
                ui.label({tile.x, tile.bottom() + 10, tile.w, 24}, name, t.style(type::label, on ? t.text : t.text_dim, on ? text::medium : 0), Align::center);
                if (it.clicked && !on) {
                    shell_.set_setting("theme", "mode", i == 0 ? "\"dark\"" : "\"light\"");
                }
            }

            {
                const bool automatic = env_.config.theme.accent == "auto";
                int chosen = automatic ? 0 : -1;
                for (size_t i = 0; i < accents.size(); ++i) {
                    if (same_color(env_.config.theme.accent, accents[i])) {
                        chosen = static_cast<int>(i + 1);
                    }
                }
                const std::string_view hint = automatic ? lyren::tr("Follows the album cover") : chosen > 0 ? lyren::tr("Fixed color") : std::string_view(env_.config.theme.accent);
                const int picked = swatch_row("accent", lyren::tr("Accent color"), hint, accents.size() + 1, chosen, true, false, [&](size_t i, Rect s) {
                    if (i > 0) {
                        p.rect(s, *ui::parse_color(accents[i - 1]), s.w / 2);
                        return;
                    }
                    p.rect(s, shell_.accent(ui.now, t.dark, moving), s.w / 2);
                    if (const Track *track = playback_.current()) {
                        cover(ui, s, track->cover, s.w / 2, "");
                    }
                });
                if (picked == 0 && !automatic) {
                    shell_.set_setting("theme", "accent", "\"auto\"");
                } else if (picked > 0 && picked != chosen) {
                    shell_.set_setting("theme", "accent", '"' + std::string(accents[static_cast<size_t>(picked - 1)]) + '"');
                }
            }
            {
                const std::array sizes{lyren::tr("Small"), lyren::tr("Default"), lyren::tr("Large"), lyren::tr("Larger")};
                int size = static_cast<int>(std::ranges::find(text_sizes, env_.config.theme.font_size) - text_sizes.begin());
                size = size < static_cast<int>(text_sizes.size()) ? size : -1;
                if (segmented_row("text-size", lyren::tr("Text size"), {}, sizes, size, false, false)) {
                    shell_.set_setting("theme", "font_size", std::to_string(text_sizes[static_cast<size_t>(size)]));
                }
            }
            {
                const std::array labels{lyren::tr("Standard"), lyren::tr("Relaxed")};
                int motion = static_cast<int>(std::ranges::find(motions, env_.config.theme.motion) - motions.begin());
                motion = motion < static_cast<int>(motions.size()) ? motion : -1;
                if (segmented_row("motion", lyren::tr("Motion"), {}, labels, motion, false, false)) {
                    shell_.set_setting("theme", "motion", std::format("{:.1f}", motions[static_cast<size_t>(motion)]));
                }
            }
            {
                const std::array fills{lyren::tr("Wave"), lyren::tr("Line")};
                int fill = env_.config.interface.progress == ProgressStyle::wave ? 0 : 1;
                if (segmented_row("progress-fill", lyren::tr("Progress bar"), {}, fills, fill, false, true)) {
                    shell_.set_setting("interface", "progress", fill == 0 ? "\"wave\"" : "\"line\"");
                }
            }

            if (button_row("reset", lyren::tr("Reset appearance"), lyren::tr("Theme colors, text size, accent and motion"), lyren::tr("Reset"), "rotate-ccw", true, true)) {
                shell_.set_setting("theme", "accent", "\"auto\"");
                shell_.set_setting("theme", "font", "\"sans-serif\"");
                shell_.set_setting("theme", "font_size", "14");
                shell_.set_setting("theme", "radius", "10");
                shell_.set_setting("theme", "motion", "1.0");
                for (std::string_view key : {"bg", "surface", "elevated", "border", "text", "text_dim", "danger"}) {
                    shell_.set_setting("theme.colors", key, {});
                }
                ui.toast(std::string(lyren::tr("Appearance reset")), "rotate-ccw");
            }
        } else if (tab_ == 1) {
            // Playback: audio quality card + behavior card.
            const std::array<std::string_view, 5> names{lyren::tr("Standard"), lyren::tr("High"), lyren::tr("Very high"), lyren::tr("Lossless"), "Hi-Res"};
            const std::array<std::string_view, 5> values{"standard", "higher", "exhigh", "lossless", "hires"};
            int quality = static_cast<int>(env_.config.playback.quality);
            if (segmented_row("quality", lyren::tr("Audio quality"), {}, names, quality, true, true)) {
                shell_.set_setting("playback", "quality", std::format("\"{}\"", values[static_cast<size_t>(quality)]));
            }

            bool skip = env_.config.playback.trial == TrialMode::skip;
            if (toggle_row("trial", lyren::tr("Skip previews"), lyren::tr("Pass over songs that only play a short clip"), skip, true, false)) {
                shell_.set_setting("playback", "trial", skip ? "\"skip\"" : "\"play\"");
            }
            bool normalize = env_.config.playback.normalize;
            if (toggle_row("normalize", lyren::tr("Volume normalization"), lyren::tr("Even out loudness between songs"), normalize, false, true)) {
                shell_.set_setting("playback", "normalize", normalize ? "true" : "false");
            }
        } else if (tab_ == 2) {
            // Lyrics: master toggle + style chooser, then a single focused live preview & settings card for the active style.
            bool shown = shell_.desktop.enabled();
            if (toggle_row("desktop-lyrics", lyren::tr("Desktop lyrics"), lyren::tr("Show the line being sung over your desktop"), shown, true, false)) {
                shell_.desktop.set_enabled(shown);
            }
            const float tw = (w - 40 - 16) / 2;
            const float tile_h = std::round(std::clamp(tw * 0.46f, 96.f, 134.f));
            const Rect rr = row(tile_h + 62, false, true);
            for (size_t i = 0; i < 2; ++i) {
                const bool island = i == 1;
                const Rect tile{rr.x + 20 + static_cast<float>(i) * (tw + 16), rr.y + 16, tw, tile_h};
                const ui::Id id = ui.id(island ? "mode-island" : "mode-floating");
                const auto it = ui.interact(id, tile);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const bool on = shell_.desktop.is_island() == island;
                const auto [hot, down] = ui.feel(id, it, pace::steady);
                const float lit = ui.anim(ui::sub_id(id, 1), on ? 1 : 0, pace::steady);
                const Rect lifted = Rect{tile.x, tile.y - 1.5f * hot, tile.w, tile.h}.inset(down * 1.2f);
                desktop_mock(ui, lifted, island);
                p.rect(lifted.inset(-3), {}, 14, t.accent.with_alpha(lit), 2);
                p.rect(lifted, {}, 12, t.surface_edge(1.1f).with_alpha(1 - lit), 1);
                check_badge(lifted, lit);
                ui.label({tile.x, tile.bottom() + 10, tile.w, 24}, island ? lyren::tr("Dynamic Island") : lyren::tr("Floating lyrics"),
                         t.style(type::label, on ? t.text : t.text_dim, on ? text::medium : 0), Align::center);
                if (it.clicked) {
                    shell_.desktop.set_island(island);
                    if (!shell_.desktop.enabled()) {
                        shell_.desktop.set_enabled(true);
                    }
                }
            }

            const int mode = shell_.desktop.is_island() ? 1 : 0;
            if (lyrics_mode_ != mode) {
                if (lyrics_mode_ >= 0) {
                    lyrics_mode_since_ = ui.now;
                }
                lyrics_mode_ = mode;
            }
            const float mode_in = ui.progress(lyrics_mode_since_, pace::arrive);
            Fade mode_fade(p, mode_in);
            y += (1 - mode_in) * 6;

            if (mode == 0) {
                const float size = t.lyric_size, sub = std::round(size * 0.58f);
                const bool translated = shell_.stage.translation() && std::ranges::any_of(playback_.lyrics(), [](const LyricLine &line) {
                                            return !line.translation.empty();
                                        });
                const float row_height = std::round(size * 1.35f) + (translated ? std::round(sub * 1.35f) : 0) + 10;
                const Rect stage_r = row(2 * row_height + 2 * std::round(size * 0.24f) + 28, true, false);
                const Rect wall = stage_r.inset(14);
                wallpaper(ui, wall, radius - 4);
                ui.push_clip(wall);
                {
                    UseTheme on_desktop(ui, shell_.stage_theme());
                    shell_.desktop.floating(ui, wall, false);
                }
                ui.pop_clip();
                p.rect(wall, {}, radius - 4, t.surface_edge(0.85f), 1);

                slider_row(
                    "lyric-size", lyren::tr("Text size"), {}, env_.config.desktop_lyrics.font_size, 16, 72, 1,
                    [](double v) {
                        return std::format("{:.0f}", v);
                    },
                    "desktop_lyrics", "font_size", false, false);
                {
                    const bool follows = env_.config.desktop_lyrics.highlight == "accent";
                    int chosen = follows ? 0 : -1;
                    for (size_t i = 0; i < sung_colors.size(); ++i) {
                        if (same_color(env_.config.desktop_lyrics.highlight, sung_colors[i])) {
                            chosen = static_cast<int>(i + 1);
                        }
                    }
                    const int picked = swatch_row("sung", lyren::tr("Sung words"), {}, sung_colors.size() + 1, chosen, false, false, [&](size_t i, Rect s) {
                        p.rect(s, i == 0 ? shell_.stage_theme().accent : *ui::parse_color(sung_colors[i - 1]), s.w / 2);
                        if (i == 0) {
                            p.icon("sparkles", ui::center(s, 12, 12), white.with_alpha(0.9f));
                        }
                    });
                    if (picked >= 0 && picked != chosen) {
                        shell_.set_setting("desktop_lyrics", "highlight", picked == 0 ? "\"accent\"" : '"' + std::string(sung_colors[static_cast<size_t>(picked - 1)]) + '"');
                    }
                }
                {
                    int chosen = -1;
                    for (size_t i = 0; i < rest_colors.size(); ++i) {
                        if (same_color(env_.config.desktop_lyrics.color, rest_colors[i])) {
                            chosen = static_cast<int>(i);
                        }
                    }
                    const int picked = swatch_row("rest", lyren::tr("Other words"), {}, rest_colors.size(), chosen, false, false, [&](size_t i, Rect s) {
                        p.rect(s, *ui::parse_color(rest_colors[i]), s.w / 2);
                    });
                    if (picked >= 0 && picked != chosen) {
                        shell_.set_setting("desktop_lyrics", "color", '"' + std::string(rest_colors[static_cast<size_t>(picked)]) + '"');
                    }
                }
                bool outlined = t.lyric_outline.a > 0;
                if (toggle_row("outline", lyren::tr("Outline"), lyren::tr("Keeps the words readable over bright wallpapers"), outlined, false, true)) {
                    shell_.set_setting("desktop_lyrics", "outline", outlined ? outline_on : outline_off);
                }
            } else {
                ui.push_id(ui.id("island-preview"));
                const float open = std::clamp(ui.spring_value(ui.id("island-open"), 0), 0.f, 1.2f);
                const auto &island_options = env_.config.desktop_lyrics.island;
                const Rect stage_r = row(std::lerp(100.f, 272.f, open), true, false);
                const Rect wall = stage_r.inset(14);
                wallpaper(ui, wall, radius - 4);
                ui.push_clip(wall);
                {
                    UseTheme on_desktop(ui, shell_.stage_theme());
                    shell_.desktop.island(ui, wall.x + wall.w / 2, wall.y + 18, false);
                }
                ui.pop_clip();
                p.rect(wall, {}, radius - 4, t.surface_edge(0.85f), 1);
                ui.pop_id();

                bool cover_on = island_options.cover;
                if (toggle_row("island-cover", lyren::tr("Cover"), lyren::tr("The album cover at the start of the island"), cover_on, false, false)) {
                    shell_.set_setting("desktop_lyrics.island", "cover", cover_on ? "true" : "false");
                }
                bool ring = island_options.cover && island_options.progress;
                if (toggle_row("island-ring", lyren::tr("Progress ring"), island_options.cover ? lyren::tr("Playback progress around the cover") : lyren::tr("Needs the cover"),
                               ring, false, true, island_options.cover)) {
                    shell_.set_setting("desktop_lyrics.island", "progress", ring ? "true" : "false");
                }
            }
        } else {
            // General: language, frame rate, graphics, and config file.
            const std::array<std::string_view, 2> language_labels{"中文", "English"};
            int language = env_.config.interface.language == Language::zh ? 0 : 1;
            if (segmented_row("language", lyren::tr("Language"), {}, language_labels, language, true, false)) {
                shell_.set_setting("interface", "language", language == 0 ? "\"zh\"" : "\"en\"");
            }
            {
                const Rect rr = row(68, false, false);
                const float cw = 152;
                caption(rr, lyren::tr("Animation frame rate"), lyren::tr("Lower rates save power"), cw);
                auto name = [](int fps) {
                    return fps == 0 ? std::string(lyren::tr("Display rate")) : std::to_string(fps) + " fps";
                };
                const Rect pill{rr.right() - 20 - cw, rr.y + (rr.h - control::medium) / 2, cw, control::medium};
                const ui::Id id = ui.id("frame-rate");
                const auto it = ui.interact(id, pill);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const auto [hot, down] = ui.feel(id, it, pace::quick);
                const Rect box = pill.inset(down);
                p.rect(box, t.fill(1 + 0.75f * hot), t.round(box.h), t.surface_edge(0.8f + 0.45f * hot), 1);
                ui.label({box.x + 14, box.y, box.w - 38, box.h}, name(env_.config.render.max_fps), t.style(type::label, t.text, text::medium));
                p.icon("chevron-down", {box.right() - 24, box.y + (box.h - 12) / 2, 12, 12}, t.text_dim.mix(t.text, hot));
                if (it.clicked) {
                    std::vector<ui::MenuItem> menu;
                    for (int fps : frame_rates) {
                        menu.push_back({env_.config.render.max_fps == fps ? "check" : "", name(fps), [this, fps] {
                                            shell_.set_setting("render", "max_fps", std::to_string(fps));
                                        }});
                    }
                    ui.open_menu(pill.x, pill.bottom() + 6, std::move(menu));
                }
            }
            const std::array gpus{lyren::tr("Integrated"), lyren::tr("Dedicated")};
            int gpu = static_cast<int>(env_.config.gpu.device);
            if (segmented_row("gpu", lyren::tr("Graphics"), lyren::tr("Takes effect after a restart"), gpus, gpu, false, true)) {
                shell_.set_setting("gpu", "device", gpu == 0 ? "\"integrated\"" : "\"discrete\"");
            }

            if (button_row("config-file", lyren::tr("Config file"), lyren::tr("Fonts, corner radius and individual colors live here"), lyren::tr("Open"), "file-cog", true, true)) {
                shell_.open_config();
            }
        }

        if (moving) {
            ui.keep_animating();
        }
        p.set_alpha(page_alpha);
        const float total_h = (y - start_y) + (show_header_in_pane ? 28.f : 22.f) + 36;
        if (float &stored = heights_[static_cast<size_t>(tab_)]; std::abs(stored - total_h) > 0.5f) {
            stored = total_h;
            ui.redraw();
        }
        if (off > 0.5f && t.surface.a > 0.9f) {
            p.gradient({viewport.x, viewport.y, viewport.w, 14}, t.surface.with_alpha(0.95f), t.surface.with_alpha(0));
        }
        ui.end_scroll();
    }

    // A window in miniature, in the colours of a theme: sidebar, page with covers, player bar.
    void SettingsPage::theme_mock(ui::Ui &ui, Rect m, const ui::Theme &look) {
        auto &p = ui.painter();
        p.rect(m, look.bg, 12);
        const Rect side{m.x + 10, m.y + 14, m.w * 0.22f, m.h - 44};
        for (int i = 0; i < 4; ++i) {
            const float bw = side.w - 6 - static_cast<float>(i % 2) * 8;
            p.rect({side.x, side.y + static_cast<float>(i) * 12, bw, 6}, i == 0 ? look.accent.with_alpha(0.9f) : look.text.with_alpha(0.16f), 3);
        }
        const Rect page{side.right() + 8, m.y + 8, m.right() - side.right() - 16, m.h - 38};
        p.rect(page, look.surface, 7, look.surface_edge(), 1);
        p.rect({page.x + 10, page.y + 10, page.w * 0.34f, 7}, look.text.with_alpha(0.75f), 3.5f);
        const float cw = (page.w - 20 - 16) / 3;
        for (int i = 0; i < 3; ++i) {
            const Rect c{page.x + 10 + static_cast<float>(i) * (cw + 8), page.y + 26, cw, std::min(cw, page.h - 34)};
            if (i == 0 && playback_.current()) {
                cover(ui, c, playback_.current()->cover, 4, "");
            } else {
                p.rect(c, ui::hsl(0.58f + 0.17f * static_cast<float>(i), 0.42f, look.dark ? 0.38f : 0.72f), 4);
            }
        }
        const Rect console{m.x + 8, m.bottom() - 24, m.w - 16, 18};
        p.rect(console, look.surface, 6, look.surface_edge(), 1);
        const float by = console.y + console.h / 2;
        p.rect({console.x + 6, by - 5, 10, 10}, look.text.with_alpha(0.22f), 2.5f);
        p.rect({console.x + 21, by - 2, m.w * 0.14f, 4}, look.text.with_alpha(0.3f), 2);
        p.rect({m.x + m.w * 0.42f, by - 1, m.w * 0.34f, 2}, look.text.with_alpha(0.16f), 1);
        p.rect({m.x + m.w * 0.42f, by - 1, m.w * 0.13f, 2}, look.accent, 1);
        p.rect({m.x + m.w * 0.42f - 12, by - 4, 8, 8}, look.text.with_alpha(0.85f), 4);
    }

    // The current cover, blurred, standing in for a desktop wallpaper.
    void SettingsPage::wallpaper(ui::Ui &ui, Rect r, float radius) {
        auto &p = ui.painter();
        p.gradient(r, ui::hsl(0.62f, 0.35f, 0.42f), ui::hsl(0.7f, 0.3f, 0.2f), radius);
        if (const Track *track = playback_.current()) {
            const auto &e = env_.images.get(track->cover, 128, true);
            if (e.ready) {
                const float aspect = r.w / r.h;
                const Rect uv = aspect > 1 ? Rect{0, (1 - 1 / aspect) / 2, 1, 1 / aspect} : Rect{(1 - aspect) / 2, 0, aspect, 1};
                Fade fade(p, ui.progress(e.ready_at, 400));
                p.image(r, e.tex, radius, white, uv);
            }
        }
        p.gradient(r, {0, 0, 0, 0.05f}, {0, 0, 0, 0.25f}, radius);
    }

    // A desktop in miniature with lyrics on it: a line near the bottom, or the island at the top.
    void SettingsPage::desktop_mock(ui::Ui &ui, Rect m, bool island) {
        auto &p = ui.painter();
        wallpaper(ui, m, 12);
        if (island) {
            const Rect pill{m.x + m.w * 0.3f, m.y + 8, m.w * 0.4f, 18};
            p.shadow({pill.x, pill.y + 2, pill.w, pill.h}, 9, 8, {0, 0, 0, 0.3f});
            p.rect(pill, ink, 9);
            if (env_.config.desktop_lyrics.island.cover) {
                p.rect({pill.x + 4, pill.y + 4, 10, 10}, white.with_alpha(0.75f), 5);
            }
            p.rect({pill.x + (env_.config.desktop_lyrics.island.cover ? 20.f : 10.f), pill.y + 7, pill.w * 0.5f, 4}, white.with_alpha(0.85f), 2);
            return;
        }
        const float cx = m.x + m.w / 2, by = m.bottom() - 26;
        p.rect({cx - m.w * 0.3f - 1.5f, by - 1.5f, m.w * 0.6f + 3, 9}, {0, 0, 0, 0.35f}, 4.5f);
        p.rect({cx - m.w * 0.3f, by, m.w * 0.6f, 6}, white.with_alpha(0.95f), 3);
        p.rect({cx - m.w * 0.18f, by + 12, m.w * 0.36f, 4}, white.with_alpha(0.7f), 2);
    }

} // namespace lyren
