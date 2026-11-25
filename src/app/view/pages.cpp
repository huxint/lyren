#include "app/view/pages.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <inplace_vector>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::hsl;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    namespace {
        // Architectural artwork for built-in lists (liked, recent, cloud, radar, daily): a restrained tonal gradient
        // with fine acoustic groove rings.
        void built_in_art(ui::Painter &p, const ui::Theme &t, Rect r, float hue, float radius, std::string_view icon) {
            const ui::Color top = hsl(hue, 0.42f, t.dark ? 0.30f : 0.56f);
            const ui::Color bottom = hsl(hue + 0.06f, 0.36f, t.dark ? 0.14f : 0.38f);
            p.gradient(r, top, bottom, radius);
            for (const float k : {0.14f, 0.27f}) {
                const Rect ring = r.inset(r.w * k);
                p.rect(ring, {}, ring.w / 2, white.with_alpha(t.dark ? 0.08f : 0.14f), 1);
            }
            if (!icon.empty()) {
                const float s = std::round(r.w * 0.34f);
                p.icon(icon, ui::center(r, s, s), white.with_alpha(0.92f));
            }
        }

        // Headed paragraphs in their own scrolling view: an artist's introduction, an album's notes. Laid out every
        // frame; there are only a few, and the shaping is cached.
        void prose(ui::Ui &ui, Rect r, ui::Id id, const std::vector<std::pair<std::string, std::string>> &sections) {
            auto &p = ui.painter();
            const auto &t = ui.theme();
            const TextStyle body = t.style(type::body, t.text.mix(t.text_dim, 0.25f));
            const float lh = std::round(body.size * 1.75f), w = r.w - 16, head_h = 36, gap = 24;
            std::vector<std::vector<std::string_view>> wrapped;
            float content = 0;
            for (const auto &[head, text] : sections) {
                wrapped.push_back(p.wrap_lines(text, body.size, w));
                content += (head.empty() ? 0 : head_h) + static_cast<float>(wrapped.back().size()) * lh + gap;
            }
            const float off = ui.begin_scroll(id, r, content);
            float y = r.y - off;
            const Rect clip = ui.clip();
            for (size_t i = 0; i < sections.size(); ++i) {
                if (!sections[i].first.empty()) {
                    ui.label({r.x, y + 4, w, 24}, sections[i].first, t.style(type::heading, t.text, text::semibold));
                    y += head_h;
                }
                for (auto line : wrapped[i]) {
                    if (y + lh > clip.y && y < clip.bottom()) {
                        p.text(r.x, y + lh * 0.72f, line, body, w);
                    }
                    y += lh;
                }
                y += gap;
            }
            ui.end_scroll();
        }
    } // namespace

    std::string Pages::date_label() const {
        const int month = (browser_.today()[0] - '0') * 10 + browser_.today()[1] - '0';
        const int day = (browser_.today()[3] - '0') * 10 + browser_.today()[4] - '0';
        return std::format("{}/{}", month, day);
    }

    float Pages::play_buttons(ui::Ui &ui, float x, float y, const std::vector<Track> &tracks, float height, bool compact) {
        const auto play_label = lyren::tr("Play all");
        const auto shuffle_label = compact ? std::string_view{} : lyren::tr("Shuffle");
        const float play_w = ui.button_width(play_label, true);
        const float shuffle_w = compact ? height : ui.button_width(shuffle_label, true);
        const float gap = compact ? 8.f : 10.f;
        ui::Enabled possible(ui, !tracks.empty());
        if (ui.button(ui.id("play-all"), {x, y, play_w, height}, play_label, ui::ButtonKind::primary, "play-fill")) {
            if (playback_.mode() == PlayMode::shuffle) {
                playback_.set_mode(PlayMode::list);
            }
            playback_.play(tracks, 0);
        }
        x += play_w + gap;
        if (ui.button(ui.id("shuffle-all"), {x, y, shuffle_w, height}, shuffle_label, ui::ButtonKind::secondary, "shuffle", compact ? lyren::tr("Shuffle") : std::string_view{})) {
            playback_.set_mode(PlayMode::shuffle);
            playback_.play(tracks, static_cast<int>(playback_.random() % tracks.size()));
        }
        return x + shuffle_w + gap;
    }

    bool Pages::page_tabs(ui::Ui &ui, ui::Id id, Rect r, std::span<const std::string_view> labels, int &selected, std::span<const bool> enabled) {
        const int before = selected;
        std::inplace_vector<std::string_view, 8> visible;
        std::inplace_vector<int, 8> indices;
        int tab = 0;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (enabled.empty() || enabled[i]) {
                if (static_cast<int>(i) == selected) {
                    tab = static_cast<int>(visible.size());
                }
                visible.push_back(labels[i]);
                indices.push_back(static_cast<int>(i));
            }
        }
        ui.tabs(id, r, visible, tab);
        selected = indices[tab];
        if (selected == before) {
            return false;
        }
        browser_.reset_selection();
        browser_.tab_since = ui.now;
        browser_.tab_direction = selected > before ? 1 : -1;
        return true;
    }

    float Pages::tab_enter(ui::Ui &ui, Rect &content) const {
        const float k = ui.progress(browser_.tab_since, pace::glide);
        content.x += (1 - k) * 14 * static_cast<float>(browser_.tab_direction);
        return k;
    }

    bool Pages::hero(ui::Ui &ui, ui::Id id, HeroSlot slot, std::string_view big, std::string_view badge, std::string_view title, std::string_view subtitle, float hue,
                     std::string_view icon, std::string_view action_icon) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect r = slot.rect;
        const float dock = std::clamp(slot.dock, 0.f, 1.f);
        const auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = ui.feel(id, it, pace::steady);
        const Rect card = Rect{r.x, r.y - std::lerp(1.5f, 0.5f, dock) * hot, r.w, r.h}.inset(down * std::lerp(1.2f, 0.7f, dock));
        const float radius = std::lerp(t.radius_l(), t.round(36.f), dock);
        const ui::Color tint = ui::accent_for_hue(hue, t.dark);
        const ui::Color full_paper = t.dark ? t.fill(1.05f + 0.35f * hot) : t.bg.mix(t.surface, 0.32f + 0.36f * hot);
        const ui::Color dock_paper = t.dark ? t.fill(0.92f + 0.55f * hot) : t.bg.mix(t.surface, 0.22f + 0.36f * hot);
        p.rect(card, full_paper.mix(dock_paper, dock), radius);
        const float top_a = std::lerp((t.dark ? 0.13f : 0.09f) + 0.045f * hot, (t.dark ? 0.12f : 0.08f) + 0.055f * hot, dock);
        const float bot_a = std::lerp(t.dark ? 0.02f : 0.015f, (t.dark ? 0.05f : 0.03f) + 0.03f * hot, dock);
        p.gradient(card, tint.with_alpha(top_a), tint.with_alpha(bot_a), radius);
        if (slot.icon_only && dock > 0.75f && it.hovered && !it.held) {
            ui.tooltip(id, card, title);
        }

        const Rect emblem_dock = slot.icon_only ? ui::center(card, 24, 24) : Rect{card.x + 6, card.y + (card.h - 24) / 2, 24, 24};
        if (icon.empty()) {
            if (const float big_a = 1 - smooth01(dock / 0.42f); big_a > 0.01f) {
                Fade f(p, big_a);
                const auto num_style = t.style(36, t.text, text::semibold | text::tabular_nums);
                const float nw = std::ceil(p.measure(big, num_style)) + 1;
                const float nx = std::lerp(card.x + 20, emblem_dock.x, dock);
                const float ny = std::lerp(card.y + 12, emblem_dock.y - 6, dock);
                if (dock > 0.01f) {
                    p.set_zoom(std::lerp(1.f, 0.65f, dock), nx, ny + 22);
                }
                ui.label({nx, ny, nw, 44}, big, num_style);
                p.set_zoom(1);
                if (!badge.empty()) {
                    if (const float badge_a = 1 - smooth01(dock / 0.32f); badge_a > 0.01f) {
                        Fade bf(p, badge_a);
                        const auto badge_style = t.style(type::micro, tint, text::medium | text::tabular_nums);
                        const float bw = std::ceil(p.measure(badge, badge_style)) + 14;
                        const Rect pill{nx + nw + 10, ny + 12, bw, 20};
                        p.rect(pill, tint.with_alpha(t.dark ? 0.15f : 0.11f), 10);
                        ui.label(pill, badge, badge_style, Align::center);
                    }
                }
            }
            if (const float chip_a = smooth01((dock - 0.22f) / 0.45f); chip_a > 0.01f) {
                Fade f(p, chip_a);
                const Rect chip = mix({card.x + 20, card.y + 18, 34, 34}, emblem_dock, dock);
                p.rect(chip, tint.with_alpha(t.dark ? 0.22f : 0.15f), chip.h / 2);
                const float chip_fs = std::min(t.fs(type::caption), chip.h * 0.54f);
                ui.label(chip, big, {.size = chip_fs, .color = tint, .features = text::semibold | text::tabular_nums}, Align::center);
            }
        } else {
            const Rect emblem = mix({card.x + 20, card.y + 18, 38, 38}, emblem_dock, dock);
            const float emblem_r = std::lerp(11.f, emblem.h / 2, dock);
            p.rect(emblem, tint.with_alpha(std::lerp(t.dark ? 0.16f : 0.11f, t.dark ? 0.22f : 0.15f, dock)), emblem_r);
            if (dock >= 0.99f) {
                p.icon(icon, ui::center(emblem, 13, 13), tint);
            } else if (dock <= 0.01f) {
                p.icon(icon, ui::center(emblem, 19, 19), tint);
            } else {
                p.set_zoom(std::lerp(1.f, 13.f / 19.f, dock), emblem.x + emblem.w / 2, emblem.y + emblem.h / 2);
                p.icon(icon, ui::center(emblem, 19, 19), tint);
                p.set_zoom(1);
            }
        }

        if (const float sub_a = 1 - smooth01(dock / 0.30f); sub_a > 0.01f) {
            const float sub_y = std::min(card.y + 93 - 28 * dock, card.bottom() - 20);
            if (sub_y >= card.y) {
                Fade f(p, sub_a);
                ui.label({card.x + 20, sub_y, std::max(0.f, card.w - 78), 18}, subtitle, t.style(type::caption, t.text_dim));
            }
        }

        const Rect title_full{card.x + 20, card.y + 68, std::max(0.f, card.w - 78), 24};
        if (slot.icon_only) {
            if (const float title_a = 1 - smooth01(dock / 0.45f); title_a > 0.01f) {
                Fade f(p, title_a);
                ui.label(title_full, title, t.style(type::heading, t.text, text::semibold));
            }
        } else {
            const float tx = emblem_dock.right() + 8;
            const Rect title_dock{tx, card.y + (card.h - 20) / 2, std::max(0.f, card.right() - tx - (slot.show_action ? 34.f : 10.f)), 20};
            const Rect tr = mix(title_full, title_dock, dock);
            if (dock >= 0.99f) {
                ui.label(title_dock, title, t.style(type::label, t.text, text::medium));
            } else if (dock <= 0.01f) {
                ui.label(title_full, title, t.style(type::heading, t.text, text::semibold));
            } else {
                const float s = std::lerp(1.f, type::label / type::heading, dock);
                p.set_zoom(s, tr.x, tr.y + tr.h / 2);
                ui.label({tr.x, tr.y, tr.w / s, tr.h}, title, t.style(type::heading, t.text, text::semibold));
                p.set_zoom(1);
            }
        }

        const bool keep_disc = slot.show_action && !slot.icon_only;
        const float disc_a = keep_disc ? 1.f : 1 - smooth01(dock / 0.45f);
        if (disc_a > 0.01f) {
            Fade f(p, disc_a);
            const Rect disc_full = Rect{card.right() - 50, card.bottom() - 50, 34, 34}.inset(-1.f * hot);
            const Rect disc_dock = Rect{card.right() - 29, card.y + (card.h - 22) / 2, 22, 22}.inset(-0.5f * hot);
            const Rect disc = keep_disc ? mix(disc_full, disc_dock, dock) : disc_full;
            p.rect(disc, t.fill(std::lerp(1.25f, 0.85f, dock)).mix(tint, hot), disc.w / 2);
            const ui::Color on_tint = tint.luminance() > 0.6f ? ink : white;
            const ui::Color glyph_c = t.text_dim.mix(on_tint, hot);
            if (!keep_disc || dock <= 0.01f) {
                Rect glyph = ui::center(disc, 14, 14);
                if (action_icon == "play-fill") {
                    glyph.x += 1;
                }
                p.icon(action_icon, glyph, glyph_c);
            } else if (dock >= 0.99f) {
                Rect glyph = ui::center(disc, 11, 11);
                if (action_icon == "play-fill") {
                    glyph.x += 0.5f;
                }
                p.icon(action_icon, glyph, glyph_c);
            } else {
                p.set_zoom(std::lerp(1.f, 11.f / 14.f, dock), disc.x + disc.w / 2, disc.y + disc.h / 2);
                Rect glyph = ui::center(disc, 14, 14);
                if (action_icon == "play-fill") {
                    glyph.x += 1;
                }
                p.icon(action_icon, glyph, glyph_c);
                p.set_zoom(1);
            }
        }
        return it.clicked;
    }

    float Pages::special_hue(Page page) {
        switch (page) {
            case Page::liked:
                return 0.97f;
            case Page::recent:
                return 0.72f;
            case Page::cloud:
                return 0.58f;
            default:
                return 0.98f;
        }
    }

    float Pages::hue(ui::Ui &ui) {
        const auto &route = browser_.route();
        const float size = 148 * ui.painter().scale();
        if (route.page == Page::playlist || route.page == Page::album || route.page == Page::similar) {
            return cover_hue(route.card.cover, size);
        }
        if (route.page == Page::artist && route.artist && route.artist->info) {
            return cover_hue(route.artist->info->avatar, size);
        }
        return -1;
    }

    void Pages::draw(ui::Ui &ui, Rect r) {
        auto &route = browser_.route();
        switch (route.page) {
            case Page::recommend:
                return recommend_page(ui, r);
            case Page::toplist:
                return toplist_page(ui, r);
            case Page::square:
                return square_page(ui, r);
            case Page::fm:
                return fm_page(ui, r);
            case Page::releases:
                return releases_page(ui, r);
            case Page::collection:
                return collection_page(ui, r);
            case Page::dislikes:
                return dislikes_page(ui, r);
            case Page::search:
                return search_page(ui, r);
            case Page::artist:
                if (route.artist) {
                    return artist_page(ui, r);
                }
                page_status(ui, r, true, "", false);
                return;
            default:
                break;
        }
        // What played on this device is at hand; cloud recent plays live on the platform shelf.
        if (route.page == Page::recent) {
            if (shelf().recent_tab == 0 || !signed_in()) {
                const auto &played = playback_.history(browser_.active());
                route.card.subtitle = lyren::trf("{} songs · On this device", played.size());
                return list_page(ui, r, played, false, "", std::max(browser_.entered, browser_.tab_since));
            }
            const auto &recent = shelf().recent_plays;
            route.card.subtitle = lyren::trf("{} songs · From all your devices", recent.tracks.size());
            return list_page(ui, r, recent.tracks, recent.loading, recent.error, std::max(recent.since, browser_.tab_since));
        }
        // No list yet: the account is still loading, or the page waits for a login.
        if (!route.list) {
            static const std::vector<Track> none;
            const std::string failed = route.page == Page::liked ? shelf().error : std::string();
            return list_page(ui, r, none, failed.empty(), failed, browser_.tab_since);
        }
        Browser::TrackList &list = *route.list;
        if (route.page == Page::liked) {
            route.card.subtitle = lyren::trf("{} songs", list.tracks.size());
        } else if (route.page == Page::cloud) {
            route.card.subtitle = lyren::trf("Cloud uploads · {} songs", list.tracks.size());
        }
        list_page(ui, r, list.tracks, list.loading, list.error, std::max(list.since, browser_.tab_since));
    }

    void Pages::recommend_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &lib = shelf();
        const auto &features = provider().features;
        struct Hero {
            Page page;
            std::string_view id, big, badge, title, subtitle, icon, action;
            float hue;
        };
        std::inplace_vector<Hero, 4> heroes;
        const std::string day(browser_.today().substr(3));
        const std::string date = date_label();
        if (features.daily.songs) {
            heroes.push_back({Page::daily, "hero-daily", day, date, "Daily mix", "Fresh picks, every day", {}, "play-fill", 0.98f});
        }
        if (browser_.supports(Page::fm, browser_.active())) {
            heroes.push_back({Page::fm, "hero-fm", {}, {}, provider().radio_title, "A station that learns your taste", "radio", "play-fill", 0.60f});
        }
        if (features.radar) {
            heroes.push_back({Page::radar, "hero-radar", {}, {}, "Radar", "Favorites and new discoveries", "sparkles", "play-fill", 0.77f});
        }
        if (browser_.supports(Page::releases, browser_.active())) {
            heroes.push_back({Page::releases, "hero-releases", {}, {}, "New releases", "Latest songs and albums", "disc-3", "chevron-right", 0.08f});
        }
        const int hero_count = static_cast<int>(heroes.size());
        const Rect body = page_frame(r);
        const CardGrid daily(ui, ui.id("daily-cards"), body.w);
        const CardGrid g(ui, ui.id("rec-cards"), body.w);
        constexpr float top_pad = 28, title_h = 76;
        constexpr float hero_h = 128, hero_gap = 16, after_heroes = 18;
        constexpr float dock_h = 56, pill_h = 36, pill_gap = 10, pill_pad_y = (dock_h - pill_h) / 2;
        const int hero_cols = std::min(hero_count, std::max(1, static_cast<int>((body.w + hero_gap) / 260)));
        const int hero_rows = hero_count ? (hero_count + hero_cols - 1) / hero_cols : 0;
        const float heroes_block_h = hero_rows ? static_cast<float>(hero_rows) * hero_h + static_cast<float>(hero_rows - 1) * hero_gap : 0.f;
        const float heroes_h = hero_rows ? heroes_block_h + hero_gap + after_heroes : 0.f;
        const float daily_h = lib.daily_lists.empty() ? 0 : 42 + daily.height(lib.daily_lists.size()) + 36;
        const float content = top_pad + title_h + heroes_h + daily_h + 42 + std::max(g.height(lib.recommended.cards.size()), 120.f) + 32;
        const ui::Id scroll = ui.id("recommend");
        const float off = ui.begin_scroll(scroll, r, content);
        const Rect clip = ui.clip();
        const float corner = t.radius_l();
        // Top accent wash dissolves as the page scrolls so the docked sticky bar meets a clean, unified surface.
        if (const float wash_a = 1 - smooth01(off / 180.f); wash_a > 0.01f) {
            p.gradient({clip.x, clip.y, clip.w, 240}, t.accent.with_alpha((t.dark ? 0.10f : 0.055f) * wash_a), t.accent.with_alpha(0), corner);
        }
        const auto &profile = lib.account;
        const std::string subtitle = std::format("{} · {}", date, profile ? lyren::trf("Picked for you, {}", profile->name) : lyren::tr("Picked for you"));

        // Compute the compact docked pill targets once on the stack so single-row hero cards morph smoothly into the
        // sticky bar, and multi-row layouts share the exact same docked proportions when pinned.
        std::array<std::string_view, 4> titles{};
        std::array<Rect, 4> dock_rects{};
        bool dock_action = true, dock_icon_only = false;
        if (hero_count) {
            const auto dock_title_style = t.style(type::label, t.text, text::medium);
            std::array<float, 4> title_ws{};
            float sum_full = 0, sum_compact = 0;
            for (int i = 0; i < hero_count; ++i) {
                const auto idx = static_cast<size_t>(i);
                titles[idx] = lyren::tr(heroes[idx].title);
                title_ws[idx] = std::ceil(p.measure(titles[idx], dock_title_style)) + 1;
                sum_full += title_ws[idx] + 78.f;
                sum_compact += title_ws[idx] + 52.f;
            }
            const float gaps_w = hero_count > 1 ? pill_gap * static_cast<float>(hero_count - 1) : 0.f;
            dock_action = sum_full + gaps_w <= body.w;
            const bool dock_natural = dock_action || (sum_compact + gaps_w <= body.w);
            const float each_w = (body.w - gaps_w) / static_cast<float>(hero_count);
            dock_icon_only = !dock_natural && each_w < 84.f;
            float dx = body.x;
            for (int i = 0; i < hero_count; ++i) {
                const auto idx = static_cast<size_t>(i);
                const float w = dock_action ? title_ws[idx] + 78.f : dock_natural ? title_ws[idx] + 52.f : each_w;
                dock_rects[idx] = {dx, r.y + pill_pad_y, w, pill_h};
                dx += w + pill_gap;
            }
        }

        auto draw_backing = [&](float alpha) {
            if (alpha <= 0.01f) {
                return;
            }
            // Keep the backing tall enough that top corners stay in the shader's upper half even for large theme radii.
            ui.push_clip({clip.x, clip.y, clip.w, dock_h});
            p.rect({clip.x, clip.y, clip.w, std::max(dock_h, corner * 2)}, t.surface.with_alpha(alpha), {corner, corner, 0, 0});
            ui.pop_clip();
            const float x0 = std::lerp(body.x, clip.x, alpha);
            const float x1 = std::lerp(body.right(), clip.right(), alpha);
            p.rect({x0, r.y + dock_h - 1, std::max(0.f, x1 - x0), 1}, t.line(alpha));
        };

        const float final_end_off = hero_count ? top_pad + title_h - pill_pad_y + heroes_block_h - pill_h : 0.f;
        const float under = hero_count ? smooth01((off - final_end_off) / 24.f) : 0.f;
        float cut_y = r.y;

        if (hero_count) {
            constexpr float latch_off = top_pad + title_h - pill_pad_y;
            constexpr float collapse0 = hero_h - pill_h;
            constexpr float row_step = hero_h + hero_gap;
            constexpr float row_wait_y = pill_pad_y + pill_h + hero_gap;
            if (const float title_a = 1 - smooth01((off - 12.f) / 56.f); title_a > 0.01f) {
                Fade f(p, title_a);
                page_title(ui, body, r.y + top_pad - off, lyren::tr("For you"), subtitle);
            }
            draw_backing(under);
            const float hw = (body.w - hero_gap * static_cast<float>(hero_cols - 1)) / static_cast<float>(hero_cols);
            float lowest_bottom = r.y + dock_h - pill_pad_y;
            for (int i = 0; i < hero_count; ++i) {
                const auto idx = static_cast<size_t>(i);
                const auto &entry = heroes[idx];
                const int row = i / hero_cols, col = i % hero_cols;
                const float in = ui.cascade(browser_.entered, i, pace::arrive);
                Fade fade(p, in);
                const float flow_x = body.x + static_cast<float>(col) * (hw + hero_gap);
                Rect rect{};
                float dock = 0;
                if (row == 0) {
                    // First row latches at the top dock line and compresses smoothly into compact pills.
                    dock = ease::standard(std::clamp((off - latch_off) / collapse0, 0.f, 1.f));
                    const float flow_y = r.y + top_pad + title_h - std::min(off, latch_off) + (1 - in) * 6 * (1 - dock);
                    const Rect flow{flow_x, flow_y, hw, hero_h};
                    rect = dock > 0 ? mix(flow, dock_rects[idx], dock) : flow;
                } else {
                    // Subsequent rows scroll up behind the docked strip, then shrink and slide rightward past the
                    // already-docked pills before curving up into their open slot on the sticky row.
                    const float start_off = latch_off + collapse0 + static_cast<float>(row - 1) * row_step;
                    const float u = std::clamp((off - start_off) / row_step, 0.f, 1.f);
                    if (u <= 0) {
                        const float flow_y = r.y + top_pad + title_h + static_cast<float>(row) * row_step - off + (1 - in) * 6;
                        rect = {flow_x, flow_y, hw, hero_h};
                    } else {
                        const float k_xh = ease::standard(std::clamp(u / 0.56f, 0.f, 1.f));
                        const float k_y = smooth01((u - 0.44f) / 0.56f);
                        dock = k_xh;
                        const float base_y = r.y + row_wait_y + (1 - in) * 6 * (1 - dock);
                        rect = {std::lerp(flow_x, dock_rects[idx].x, k_xh), std::lerp(base_y, dock_rects[idx].y, k_y), std::lerp(hw, dock_rects[idx].w, k_xh),
                                std::lerp(hero_h, pill_h, k_xh)};
                    }
                }
                lowest_bottom = std::max(lowest_bottom, rect.bottom());
                if (hero(ui, ui.id(entry.id), {rect, dock, dock_action, dock_icon_only}, entry.big, entry.badge, titles[idx], lyren::tr(entry.subtitle), entry.hue, entry.icon,
                         entry.action)) {
                    browser_.open(entry.page);
                }
            }
            if (off > latch_off) {
                cut_y = std::max(r.y + dock_h, lowest_bottom + pill_pad_y);
            }
        }

        // Everything below the sticky bar is clipped strictly beneath cut_y so hidden cards can neither bleed through
        // nor steal clicks from the pinned actions.
        ui.push_clip({clip.x, cut_y, clip.w, std::max(0.f, clip.bottom() - cut_y)});
        float y = r.y + top_pad - off;
        if (!hero_count) {
            page_title(ui, body, y, lyren::tr("For you"), subtitle);
        }
        y += title_h + heroes_h;
        if (!lib.daily_lists.empty()) {
            section_heading(ui, body.x, y, body.w, lyren::tr("Daily playlists"), std::to_string(lib.daily_lists.size()));
            if (int i = card_grid(ui, {body.x, y + 42, body.w, 0}, ui.id("daily-cards"), lib.daily_lists, false, std::max(browser_.entered + 0.06, lib.daily_since)); i >= 0) {
                browser_.open(lib.daily_lists[static_cast<size_t>(i)]);
            }
            y += daily_h;
        }
        const Listing &res = lib.recommended;
        section_heading(ui, body.x, y, body.w, lyren::tr("Recommended playlists"), res.cards.empty() ? std::string() : std::to_string(res.cards.size()));
        y += 42;
        if (!page_status(ui, {body.x, y, body.w, std::max(120.f, r.bottom() - y)}, res.loading && res.cards.empty(), res.error, false, Placeholder::cards)) {
            if (int i = card_grid(ui, {body.x, y, body.w, 0}, ui.id("rec-cards"), res.cards, false, std::max(browser_.entered + 0.06, res.since)); i >= 0) {
                browser_.open(res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.pop_clip();
        ui.end_scroll();
    }

    void Pages::toplist_page(ui::Ui &ui, Rect r) {
        const Listing &res = shelf().toplists;
        const Rect body = page_frame(r);
        const CardGrid g(ui, ui.id("top-cards"), body.w);
        const float off = ui.begin_scroll(ui.id("toplists"), r, 28 + 82 + g.height(res.cards.size()) + 32);
        const float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Charts"), lyren::tr("Official charts, updated daily or weekly"));
        if (!page_status(ui, {body.x, y + 82, body.w, r.bottom() - y - 82}, res.loading, res.error, false, Placeholder::cards)) {
            if (int i = card_grid(ui, {body.x, y + 82, body.w, 0}, ui.id("top-cards"), res.cards, false, std::max(browser_.entered + 0.04, res.since)); i >= 0) {
                browser_.open(res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.end_scroll();
    }

    void Pages::square_page(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        const auto &t = ui.theme();
        const Rect body = page_frame(r);
        const CardGrid g(ui, ui.id("square-cards"), body.w);
        constexpr float chips_y = 82, grid_y = chips_y + 52;
        const float content = 28 + grid_y + std::max(g.height(lib.square.cards.size()), 120.f) + (lib.square.more() ? 64 : 32);
        const float off = ui.begin_scroll(ui.id("square"), r, content);
        const float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Browse"), lyren::tr("Explore playlists by category"));

        // The popular categories as far as they fit, the picked one among them, the rest behind More categories.
        const Category all;
        std::vector<const Category *> categories{&all};
        if (!lib.category.id.empty() && std::ranges::find(lib.categories.hot, lib.category.id, &Category::id) == lib.categories.hot.end()) {
            categories.push_back(&lib.category);
        }
        for (const auto &category : lib.categories.hot) {
            categories.push_back(&category);
        }
        float x = body.x;
        const float more_w = lib.categories.groups.empty() ? 0 : ui.chip_width(lyren::tr("More categories"), true);
        const Category *picked = nullptr;
        ui.push_id(ui.id("square-chips"));
        for (const Category *category : categories) {
            const auto name = category->id.empty() ? lyren::tr("All") : std::string_view(category->name);
            const float w = ui.chip_width(name);
            if (x + w + 8 + more_w > body.right()) {
                break;
            }
            if (ui.chip(ui.id(category->id), {x, y + chips_y, w, 30}, name, category->id == lib.category.id)) {
                picked = category;
            }
            x += w + 8;
        }
        if (more_w > 0 && ui.chip(ui.id("more-categories"), {x, y + chips_y, more_w, 30}, lyren::tr("More categories"), false, "chevron-down")) {
            ui.open_modal(600, 480, [this](ui::Ui &u, Rect m) {
                categories_modal(u, m);
            });
        }
        ui.pop_id();
        if (picked) {
            library_.set_category(browser_.active(), *picked);
        }

        const float gy = y + grid_y;
        if (!page_status(ui, {body.x, gy, body.w, std::max(120.f, r.bottom() - gy)}, lib.square.loading && lib.square.cards.empty(),
                         lib.square.cards.empty() ? lib.square.error : "", false, Placeholder::cards)) {
            const double since = std::max({browser_.entered + 0.04, lib.square_since, lib.square.since});
            if (int i = card_grid(ui, {body.x, gy, body.w, 0}, ui.id("square-cards"), lib.square.cards, false, since); i >= 0) {
                browser_.open(lib.square.cards[static_cast<size_t>(i)]);
            }
            const float end = gy + g.height(lib.square.cards.size());
            if (lib.square.loading) {
                ui.spinner(ui::center({body.x, end, body.w, 64}, 20, 20), t.text_dim, 2);
            } else if (end < r.bottom() + 400) {
                library_.load_square(browser_.active());
            }
        }
        ui.end_scroll();
    }

    void Pages::fm_page(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        page_title(ui, page_frame(r), r.y + 28, lyren::tr(provider().radio_title), lyren::tr("Tuned to what you play"));
        const Rect below{r.x, r.y + 100, r.w, r.h - 100};
        if (page_status(ui, below, lib.radio_loading && lib.radio.empty(), lib.radio.empty() ? lib.radio_error : "", !signed_in())) {
            return;
        }
        if (lib.radio.empty()) {
            return;
        }
        lib.radio_pos = std::min(lib.radio_pos, static_cast<int>(lib.radio.size()) - 1);
        const Track &track = lib.radio[static_cast<size_t>(lib.radio_pos)];
        const bool on_air = playback_.is_current(track);
        const bool active = on_air && playback_.playing();
        const float size = std::clamp(std::min(r.w * 0.36f, below.h - 236), 160.f, 320.f);
        const bool has_next = static_cast<size_t>(lib.radio_pos + 1) < lib.radio.size() && below.h >= size + 230;
        const Rect col = ui::center(below, size, size + (has_next ? 176.f : 148.f));
        const float presence = t.motion > 0 ? ui.spring(ui.id("fm-presence"), active ? 1.f : 0.f, 480) : 1.f;
        const float resting = 0.97f + 0.03f * presence;
        const Rect cov = ui::center({col.x, col.y, size, size}, size * resting, size * resting);

        cover(ui, cov, track.cover, t.radius_l());
        {
            const auto art_it = ui.interact(ui.id("fm-cover"), cov);
            if (art_it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (art_it.clicked) {
                if (on_air) {
                    playback_.toggle();
                } else {
                    lib.radio_pending = false;
                    playback_.play_radio(browser_.active(), lib.radio, lib.radio_pos);
                }
            } else if (art_it.right_clicked) {
                shell_.tracks.menu(ui, art_it.px, art_it.py, lib.radio, lib.radio_pos);
            }
        }

        const float text_w = std::min(r.w - 64, size + 220);
        const float tx = col.x + (size - text_w) / 2;
        ui.label({tx, col.y + size + 22, text_w, 28}, track.title, t.style(type::title, t.text, text::semibold), Align::center);
        const std::string sub = track.album.empty() ? track.artist : track.artist + " · " + track.album;
        ui.label({tx, col.y + size + 52, text_w, 20}, sub, t.style(type::label, t.text_dim), Align::center);

        const float by = col.y + size + 88, cx = col.x + size / 2;
        const bool can_dislike = provider().capabilities.radio_dislike;
        const bool can_like = provider(track).capabilities.likes;
        // Centered transport row: optional dislike + like on the left of play, skip-forward on the right.
        auto next = [&](bool play) {
            const Source source = browser_.active();
            const bool playing = play || playback_.radio(source);
            if (library_.advance_radio(source)) {
                if (playing) {
                    playback_.play_radio(source, lib.radio, lib.radio_pos);
                }
            } else if (playing) {
                playback_.stop();
            }
        };
        const Rect play_r{cx - 24, by, 48, 48};
        if (can_like) {
            heart(ui, ui.id("fm-like"), {play_r.x - 52, by + 4, 40, 40}, track, 19);
        }
        if (can_dislike) {
            const float dx = can_like ? play_r.x - 104 : play_r.x - 52;
            if (ui.icon_button(ui.id("fm-trash"), {dx, by + 4, 40, 40}, "thumbs-down", 19, false, lyren::tr("Not interested"))) {
                library_.dislike_radio(track);
                next(false);
                return;
            }
        }
        {
            const ui::Id pid = ui.id("fm-play");
            const auto it = ui.interact(pid, play_r);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const auto [hot, down] = ui.feel(pid, it);
            const Rect pr = play_r.inset(-1.5f * hot + 2.f * down);
            p.rect(pr, t.accent.mix(white, 0.08f * hot), pr.w / 2);
            const float morph = ui.anim(ui.id("fm-morph"), active ? 1 : 0, 240, ease::emphasized);
            const ui::Color fg = t.on_accent();
            {
                Fade fade(p, 1 - morph);
                p.set_zoom((18 - 5 * morph) / 18.f, pr.x + pr.w / 2, pr.y + pr.h / 2);
                Rect glyph = ui::center(pr, 18, 18);
                glyph.x += 1;
                p.icon("play-fill", glyph, fg);
                p.set_zoom(1);
            }
            {
                Fade fade(p, morph);
                p.set_zoom((13 + 5 * morph) / 18.f, pr.x + pr.w / 2, pr.y + pr.h / 2);
                p.icon("pause-fill", ui::center(pr, 18, 18), fg);
                p.set_zoom(1);
            }
            if (it.clicked) {
                if (on_air) {
                    playback_.toggle();
                } else {
                    lib.radio_pending = false;
                    playback_.play_radio(browser_.active(), lib.radio, lib.radio_pos);
                }
            }
        }
        const Rect next_r{play_r.right() + 12, by + 4, 40, 40};
        if (lib.radio_pending && lib.radio_loading) {
            ui.spinner(ui::center(next_r, 18, 18), t.text_dim, 2);
        } else if (ui.icon_button(ui.id("fm-next"), next_r, "skip-forward", 19, false, lyren::tr("Next"))) {
            next(on_air);
        }
        if (has_next) {
            const Track &upcoming = lib.radio[static_cast<size_t>(lib.radio_pos + 1)];
            ui.label({tx, play_r.bottom() + 20, text_w, 18}, std::format("{} · {} — {}", lyren::tr("Up next"), upcoming.title, upcoming.artist), t.style(type::caption, t.faint()),
                     Align::center);
        }
    }

    void Pages::releases_page(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect body = page_frame(r);
        page_title(ui, body, r.y + 28, lyren::tr("New releases"), lyren::tr("Latest songs and albums"));

        const Rect tab_bar{body.x, r.y + 96, body.w, 38};
        p.rect({tab_bar.x, tab_bar.bottom() - 1, tab_bar.w, 1}, t.line());
        const std::array<std::string_view, 2> tabs = {lyren::tr("Songs"), lyren::tr("Albums")};
        const std::array enabled = {!provider().release_categories[0].empty(), !provider().release_categories[1].empty()};
        if (page_tabs(ui, ui.id("release-tabs"), {tab_bar.x, tab_bar.y, tab_bar.w - 44, tab_bar.h}, tabs, lib.release_tab, enabled)) {
            library_.load_releases(browser_.active());
        }
        const int tab = lib.release_tab;
        Listing &res = lib.releases[tab];
        const Rect refresh_r{tab_bar.right() - control::medium, tab_bar.y + (tab_bar.h - control::medium) / 2 - 2, control::medium, control::medium};
        if (res.loading) {
            ui.spinner(ui::center(refresh_r, 16, 16), t.text_dim, 2);
        } else if (ui.icon_button(ui.id("release-refresh"), refresh_r, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
            library_.set_release_category(browser_.active(), lib.release_category[tab]);
        }

        // One calm toolbar row: play controls on the left for Songs, and region filter chips (or a capsule menu when
        // narrow) cleanly aligned beside them.
        const float bar_y = tab_bar.bottom() + 14;
        float filter_x = body.x;
        if (tab == 0) {
            filter_x = play_buttons(ui, body.x, bar_y, res.tracks, control::medium, body.w < 540) + 6;
        }
        const auto &categories = provider().release_categories[tab];
        float chips_w = 0;
        for (size_t i = 0; i < categories.size(); ++i) {
            chips_w += ui.chip_width(lyren::tr(categories[i].name)) + (i ? 6.f : 0.f);
        }
        const float avail = body.right() - filter_x;
        if (chips_w <= avail) {
            float cx = tab == 0 ? body.right() - chips_w : body.x;
            ui.push_id(ui.id("release-cats"));
            for (size_t i = 0; i < categories.size(); ++i) {
                const auto name = lyren::tr(categories[i].name);
                const float w = ui.chip_width(name);
                if (ui.chip(ui.id(static_cast<uint64_t>(i)), {cx, bar_y + 2, w, 30}, name, static_cast<int>(i) == lib.release_category[tab])) {
                    library_.set_release_category(browser_.active(), static_cast<int>(i));
                }
                cx += w + 6;
            }
            ui.pop_id();
        } else {
            const std::string_view category = lyren::tr(categories[lib.release_category[tab]].name);
            const float cw = ui.chip_width(category, true);
            const Rect pill{tab == 0 ? body.right() - cw : body.x, bar_y + 2, cw, 30};
            if (ui.chip(ui.id("release-category"), pill, category, true, "chevron-down")) {
                std::vector<ui::MenuItem> items;
                for (size_t i = 0; i < categories.size(); ++i) {
                    items.push_back({i == static_cast<size_t>(lib.release_category[tab]) ? "check" : "", std::string(lyren::tr(categories[i].name)), [this, i] {
                                         library_.set_release_category(browser_.active(), static_cast<int>(i));
                                     }});
                }
                ui.open_menu(pill.x, pill.bottom() + 6, std::move(items));
            }
        }

        const float top = bar_y + control::medium + 14;
        Rect content{body.x - 12, top, body.w + 24, r.bottom() - top - 8};
        Fade fade(p, tab_enter(ui, content));
        std::string error = res.error;
        if (res.loaded && res.size() == 0 && error.empty()) {
            error = tab ? lyren::tr("No albums found") : lyren::tr("No songs here yet");
        }
        if (res.size() == 0 && page_status(ui, tab ? content.inset(12, 12) : content, res.loading, error, false, tab ? Placeholder::cards : Placeholder::rows)) {
            return;
        }
        if (tab == 0) {
            shell_.tracks.list(ui, content, ui.id("release-songs"), res.tracks, std::max({browser_.entered, browser_.tab_since, res.since}));
        } else {
            bool near_end;
            if (int i = grid_view(ui, content, ui.id("release-albums"), res.cards, false, std::max({browser_.entered, browser_.tab_since, res.since}), res.more(), near_end);
                i >= 0) {
                browser_.open(res.cards[static_cast<size_t>(i)]);
            }
            if (near_end) {
                library_.load_releases(browser_.active());
            }
        }
    }

    void Pages::collection_page(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        const Rect body = page_frame(r);
        page_title(ui, body, r.y + 28, lyren::tr("My collection"), lyren::tr("Saved albums and followed artists"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const Rect tab_bar{body.x, r.y + 96, body.w, 38};
        ui.painter().rect({tab_bar.x, tab_bar.bottom() - 1, tab_bar.w, 1}, ui.theme().line());
        const std::array<std::string_view, 2> tabs = {lyren::tr("Albums"), lyren::tr("Artists")};
        const std::array enabled = {provider().capabilities.collect_albums, provider().capabilities.follow_artists};
        page_tabs(ui, ui.id("collection-tabs"), {tab_bar.x, tab_bar.y, tab_bar.w - 44, tab_bar.h}, tabs, lib.collection_tab, enabled);
        const Listing &res = lib.collection[lib.collection_tab];
        const Rect refresh_r{tab_bar.right() - control::medium, tab_bar.y + (tab_bar.h - control::medium) / 2 - 2, control::medium, control::medium};
        if (res.loading) {
            ui.spinner(ui::center(refresh_r, 16, 16), ui.theme().text_dim, 2);
        } else if (ui.icon_button(ui.id("collection-refresh"), refresh_r, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
            spawn(library_.load_collection(browser_.active()));
        }
        Rect content{body.x - 12, tab_bar.bottom() + 10, body.w + 24, r.bottom() - tab_bar.bottom() - 18};
        Fade fade(ui.painter(), tab_enter(ui, content));
        std::string error = res.cards.empty() ? res.error : "";
        if (res.cards.empty() && res.loaded && error.empty()) {
            error = lib.collection_tab ? lyren::tr("No followed artists yet") : lyren::tr("No saved albums yet");
        }
        if (res.cards.empty() && page_status(ui, content.inset(12, 12), res.loading && !res.loaded, error, false, lib.collection_tab ? Placeholder::faces : Placeholder::cards)) {
            return;
        }
        bool near_end;
        const ui::Id id = ui.id(lib.collection_tab ? "collection-artists" : "collection-albums");
        if (int i = grid_view(ui, content, id, res.cards, lib.collection_tab == 1, std::max({browser_.entered, browser_.tab_since, res.since}), false, near_end); i >= 0) {
            browser_.open(res.cards[static_cast<size_t>(i)]);
        }
    }

    void Pages::dislikes_page(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect body = page_frame(r);
        page_title(ui, body, r.y + 28, lyren::tr("Disliked"), lyren::tr("Manage what you hear in recommendations"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const Rect tab_bar{body.x, r.y + 96, body.w, 38};
        p.rect({tab_bar.x, tab_bar.bottom() - 1, tab_bar.w, 1}, t.line());
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Artists"), lyren::tr("Styles")};
        const std::array icons = {"music", "mic-vocal", "sparkles"};
        if (page_tabs(ui, ui.id("dislike-tabs"), {tab_bar.x, tab_bar.y, tab_bar.w - 44, tab_bar.h}, tabs, lib.dislike_tab)) {
            if (!lib.dislikes[lib.dislike_tab]) {
                spawn(library_.load_dislikes(browser_.active(), static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        const Library::DislikeList none;
        const auto &cached = lib.dislikes[lib.dislike_tab];
        const Library::DislikeList &list = cached ? *cached : none;
        const bool loading = !cached || list.loading;
        const Rect refresh_r{tab_bar.right() - control::medium, tab_bar.y + (tab_bar.h - control::medium) / 2 - 2, control::medium, control::medium};
        if (loading) {
            ui.spinner(ui::center(refresh_r, 16, 16), t.text_dim, 2);
        } else if (ui.icon_button(ui.id("dislike-refresh"), refresh_r, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
            spawn(library_.load_dislikes(browser_.active(), static_cast<DislikeKind>(lib.dislike_tab)));
        }
        Rect content{body.x, tab_bar.bottom() + 12, body.w, r.bottom() - tab_bar.bottom() - 20};
        Fade fade(p, tab_enter(ui, content));
        if (!list.error.empty()) {
            page_status(ui, content, false, list.error, false);
            return;
        }
        if (list.items.empty()) {
            if (!loading) {
                empty(ui, content, "thumbs-down", lyren::tr("Nothing disliked here"));
            } else {
                page_status(ui, content, true, "", false, Placeholder::rows);
            }
            return;
        }
        constexpr float row_h = 68;
        const float off = ui.begin_scroll(ui.id("dislikes-" + std::to_string(lib.dislike_tab)), content, row_h * static_cast<float>(list.items.size()));
        const size_t first = static_cast<size_t>(off / row_h);
        const size_t last = std::min(list.items.size(), static_cast<size_t>((off + content.h) / row_h) + 1);
        const float restore_w = ui.button_width(lyren::tr("Restore"), true);
        for (size_t i = first; i < last; ++i) {
            const Dislike &item = list.items[i];
            ui.push_id(ui.id(item.id));
            Rect row{content.x, content.y - off + row_h * static_cast<float>(i), content.w, row_h};
            const float k = ui.cascade(std::max({browser_.entered, browser_.tab_since, list.since}), static_cast<int>(i - first), pace::arrive);
            Fade row_fade(p, k);
            row.y += (1 - k) * 8;
            if (const float hot = ui.anim(ui.id("row"), ui.hovering(row) ? 1 : 0, pace::quick); hot > 0) {
                p.rect(row.inset(0, 2), t.fill(hot), t.radius_s());
            }
            const Rect art{row.x + 10, row.y + 12, 44, 44};
            const float art_r = lib.dislike_tab == 1 ? 22.f : t.radius_s();
            cover(ui, art, item.cover, art_r, icons[lib.dislike_tab]);
            const float text_x = art.right() + 14, text_w = row.w - (text_x - row.x) - restore_w - 24;
            ui.label({text_x, row.y + 13, text_w, 22}, item.name.empty() ? item.id : item.name, t.style(type::body, t.text, text::medium));
            ui.label({text_x, row.y + 37, text_w, 18}, lyren::tr("Hidden from recommendations"), t.style(type::caption, t.text_dim));
            {
                ui::Enabled idle(ui, !loading && !list.pending.contains(item.id));
                if (ui.button(ui.id("restore"), {row.right() - restore_w - 10, row.y + (row.h - control::medium) / 2, restore_w, control::medium}, lyren::tr("Restore"),
                              ui::ButtonKind::secondary, "check")) {
                    library_.set_dislike(item, false);
                }
            }
            ui.pop_id();
        }
        ui.end_scroll();
    }

    void Pages::artist_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Browser::Artist &a = *browser_.route().artist;
        const Card &c = browser_.route().card;
        const bool compact = r.w < 680;
        const float art_d = compact ? 116.f : 148.f;
        const float btn_h = compact ? control::medium : control::large;
        const Rect frame = page_frame(r);
        const Rect head{frame.x, r.y + 28, frame.w, art_d};
        const Rect cov{head.x, head.y, art_d, art_d};
        const std::string &avatar = a.info ? a.info->avatar : c.cover;
        cover(ui, cov, avatar, art_d / 2, "user");
        if (const auto it = ui.interact(ui.id("artist-link"), cov); it.right_clicked) {
            shell_.dialogs.link_menu(ui, it.px, it.py, c);
        } else if (it.clicked && !avatar.empty()) {
            shell_.dialogs.show_picture(ui, avatar, 1);
        } else if (it.hovered && !avatar.empty()) {
            ui.cursor = wl::Cursor::pointer;
        }
        const float tx = cov.right() + (compact ? 20.f : 28.f), tw = head.right() - tx;
        ui.label({tx, head.y + 2, tw, 16}, lyren::tr("Artists"), t.style(type::micro, t.faint(), text::semibold));
        ui.label({tx, head.y + 20, tw, compact ? 30.f : 38.f}, a.info ? a.info->name : c.title, t.style(compact ? type::title : type::display, t.text, text::semibold));
        std::string about = a.error;
        if (a.info) {
            about = lyren::trf("{} songs · {} albums", a.info->songs, a.info->albums);
            if (!a.info->alias.empty()) {
                about = a.info->alias + " · " + about;
            }
        }
        ui.label({tx, head.y + (compact ? 52.f : 62.f), tw, 20}, about, t.style(type::label, t.text_dim));
        const std::vector<Track> &songs = a.songs.tracks;
        const float bx = play_buttons(ui, tx, head.bottom() - btn_h, songs, btn_h, compact);
        if (a.info && provider(c).capabilities.follow_artists) {
            const bool on = library_.kept(c.source, Library::Preference::artist, c.id, a.info->followed);
            const auto label = on ? lyren::tr("Following") : lyren::tr("Follow");
            const float follow_w = compact ? btn_h : std::max(ui.button_width(lyren::tr("Following"), true), ui.button_width(lyren::tr("Follow"), true));
            if (ui.button(ui.id("follow"), {bx, head.bottom() - btn_h, follow_w, btn_h}, compact ? std::string_view{} : label,
                          on ? ui::ButtonKind::secondary : ui::ButtonKind::tonal, on ? "user-check" : "user-plus", compact ? label : std::string_view{})) {
                library_.toggle_follow({.kind = CardKind::artist, .source = c.source, .id = c.id, .title = a.info->name, .cover = a.info->avatar}, a.info->followed);
            }
        }

        // Songs and albums load a page at a time as they scroll; each tab loads the first time it is picked.
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Albums"), lyren::tr("About")};
        const Rect bar{head.x, head.bottom() + 22, head.w, 38};
        p.rect({bar.x, bar.bottom() - 1, bar.w, 1}, t.line());
        const float retry_w = ui.button_width(lyren::tr("Retry"), true);
        const Rect retry{bar.right() - retry_w, bar.y + (bar.h - control::small) / 2 - 2, retry_w, control::small};
        if (page_tabs(ui, ui.id("artist-tabs"), {bar.x, bar.y, bar.w - retry_w - 16, bar.h}, std::span(tabs).first(provider(c).capabilities.artist_intro ? 3 : 2), a.tab)) {
            browser_.load_artist_tab(a.tab);
        }
        Rect content{head.x - 12, bar.bottom() + 10, head.w + 24, r.bottom() - bar.bottom() - 18};
        Fade fade(p, tab_enter(ui, content));
        Listing &res = a.tab == 0 ? a.songs : a.tab == 1 ? a.albums : a.intro;
        if (res.loading || a.loading) {
            ui.spinner({bar.right() - 22, bar.y + 9, 16, 16}, t.text_dim, 2);
        } else if ((!res.error.empty() || !a.error.empty()) && ui.button(ui.id("artist-retry"), retry, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
            if (!a.error.empty()) {
                browser_.reload_artist();
            }
            if (!res.error.empty()) {
                browser_.load_artist_tab(a.tab, true);
            }
        }
        const bool is_empty = a.tab == 2 ? a.sections.empty() : res.size() == 0;
        std::string error = is_empty ? res.error : "";
        if (is_empty && res.loaded && error.empty()) {
            error = a.tab == 2 ? lyren::tr("No biography available") : lyren::tr("Nothing here yet");
        }
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::spinner};
        if (is_empty &&
            page_status(ui, a.tab == 1 ? content.inset(12, 12) : content, res.loading || (!res.loaded && error.empty()), error, false, looks[static_cast<size_t>(a.tab)])) {
            return;
        }
        if (!res.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), res.error, t.style(type::caption, t.danger));
        }
        const double since = std::max({browser_.entered + 0.04, browser_.tab_since, res.since});
        bool near_end = false;
        int clicked = -1;
        switch (a.tab) {
            case 0:
                near_end = shell_.tracks.list(ui, content, scroll_id(ui, "artist-songs"), songs, since);
                break;
            case 1:
                clicked = grid_view(ui, content, scroll_id(ui, "artist-albums"), a.albums.cards, false, since, a.albums.loading, near_end);
                break;
            default:
                prose(ui, {content.x + 16, content.y + 8, content.w - 16, content.h - 8}, scroll_id(ui, "artist-intro"), a.sections);
        }
        if (near_end) {
            browser_.load_artist_tab(a.tab);
        }
        // Last: opening an album replaces the route this page draws from.
        if (clicked >= 0) {
            browser_.open(a.albums.cards[static_cast<size_t>(clicked)]);
        }
    }

    // Each search starts its lists from the top; a platform's tabs keep their own places.
    ui::Id Pages::search_scroll_id(ui::Ui &ui, int tab) const {
        const uint64_t platform = browser_.search_serial() * platforms.size() + static_cast<uint64_t>(browser_.active());
        return ui::sub_id(ui.id("search-results"), platform * 5 + static_cast<uint64_t>(tab));
    }

    void Pages::search_page(ui::Ui &ui, Rect r) {
        const auto &t = ui.theme();
        const Rect body = page_frame(r);
        const std::array<std::string_view, 5> tabs = {lyren::tr("Tracks"), lyren::tr("Albums"), lyren::tr("Artists"), lyren::tr("Playlists"), lyren::tr("Users")};
        const Rect tab_bar{body.x, r.y + 96, body.w, 38};
        ui.painter().rect({tab_bar.x, tab_bar.bottom() - 1, tab_bar.w, 1}, t.line());
        int &tab = browser_.search_tab();
        Listing &shown = browser_.results(tab);
        const float action_w = shown.error.empty() ? control::medium : ui.button_width(lyren::tr("Retry"), true);
        if (page_tabs(ui, ui.id("search-tabs"), {tab_bar.x, tab_bar.y, tab_bar.w - action_w - 12, tab_bar.h}, tabs, tab, provider().capabilities.search)) {
            browser_.ensure();
        }
        const ui::Id scroll = search_scroll_id(ui, tab);
        const Rect action_r{tab_bar.right() - action_w, tab_bar.y + (tab_bar.h - control::medium) / 2 - 2, action_w, control::medium};
        auto reload = [&] {
            if (shown.error.empty()) {
                shown = {};
                browser_.reset_selection();
                ui.scroll_to(scroll, 0);
            }
            browser_.load_results(tab, true);
        };
        if (shown.loading) {
            ui.spinner(ui::center({tab_bar.right() - control::medium, action_r.y, control::medium, control::medium}, 16, 16), t.text_dim, 2);
        } else if (shown.error.empty()) {
            if (ui.icon_button(ui.id("search-refresh"), action_r, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
                reload();
            }
        } else if (ui.button(ui.id("search-refresh"), action_r, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
            reload();
        }
        const std::array kinds = {lyren::tr("songs"), lyren::tr("albums"), lyren::tr("artists"), lyren::tr("playlists"), lyren::tr("users")};
        page_title(ui, body, r.y + 28, '"' + browser_.searched() + '"',
                   shown.loaded ? lyren::trf("{}{} {} found", shown.more() ? lyren::tr("About ") : "", shown.total, kinds[tab])
                                : lyren::trf("Searching {}", lyren::tr(platform(browser_.active()).short_name)));
        // Tab content slides toward the tab that was picked.
        Rect content{body.x - 12, tab_bar.bottom() + 10, body.w + 24, r.bottom() - tab_bar.bottom() - 18};
        Fade fade(ui.painter(), tab_enter(ui, content));
        const double since = std::max({browser_.entered, browser_.tab_since, shown.since});
        const bool is_empty = shown.size() == 0;
        std::string error = is_empty ? shown.error : "";
        if (is_empty && shown.more() && error.empty()) {
            browser_.load_results(tab);
        }
        if (is_empty && shown.loaded && !shown.more() && error.empty()) {
            error = lyren::tr("No results found");
        }
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::faces, Placeholder::cards, Placeholder::faces};
        if (is_empty && page_status(ui, tab ? content.inset(12, 12) : content, shown.loading, error, false, looks[static_cast<size_t>(tab)])) {
            return;
        }
        if (!shown.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), shown.error, t.style(type::caption, t.danger));
        }
        if (tab == 0) {
            if (shell_.tracks.list(ui, content, scroll, shown.tracks, since)) {
                browser_.load_results(0);
            }
            return;
        }
        bool near_end;
        const int i = grid_view(ui, content, scroll, shown.cards, tab == 2 || tab == 4, since, shown.loading, near_end);
        if (near_end) {
            browser_.load_results(tab);
        }
        if (i >= 0) {
            browser_.open(shown.cards[static_cast<size_t>(i)]);
        }
    }

    int Pages::grid_view(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since, bool loading, bool &near_end) {
        const Rect inner = r.inset(12, 0);
        const CardGrid g(ui, id, inner.w, round ? 130 : 150);
        const float h = 12 + g.height(cards.size()) + (loading ? 64 : 20);
        const float off = ui.begin_scroll(id, r, h);
        const int i = card_grid(ui, {inner.x, r.y + 12 - off, inner.w, 0}, id, cards, round, since);
        near_end = r.y + h - off < r.bottom() + 400;
        if (loading) {
            ui.spinner(ui::center({inner.x, r.y + h - off - 64, inner.w, 64}, 20, 20), ui.theme().text_dim, 2);
        }
        ui.end_scroll();
        return i;
    }

    void Pages::list_page(ui::Ui &ui, Rect r, const std::vector<Track> &tracks, bool loading, const std::string &list_error, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &c = browser_.route().card;
        const Page page = browser_.route().page;
        const bool compact = r.w < 680;
        const float art_d = compact ? 116.f : 148.f;
        const float btn_h = compact ? control::medium : control::large;
        const Rect frame = page_frame(r);
        const Rect head{frame.x, r.y + 28, frame.w, art_d};
        const Rect cov{head.x, head.y, art_d, art_d};
        bool custom_cover = false;
        switch (page) {
            case Page::liked:
                built_in_art(p, t, cov, special_hue(Page::liked), t.radius_l(), "heart-fill");
                break;
            case Page::recent:
                built_in_art(p, t, cov, special_hue(Page::recent), t.radius_l(), "history");
                break;
            case Page::cloud:
                built_in_art(p, t, cov, special_hue(Page::cloud), t.radius_l(), "cloud");
                break;
            case Page::radar:
                built_in_art(p, t, cov, 0.78f, t.radius_l(), "sparkles");
                break;
            case Page::daily: {
                built_in_art(p, t, cov, 0.98f, t.radius_l(), "");
                const std::string day(browser_.today().substr(3));
                ui.label(cov, day.substr(0, day.find(' ')), t.style(compact ? 42.f : type::numeral + 8, white, text::semibold | text::tabular_nums), Align::center);
                break;
            }
            default:
                custom_cover = true;
                cover(ui, cov, c.cover, t.radius_l(), page == Page::album ? "disc-album" : "list-music");
        }
        if (custom_cover) {
            const auto it = ui.interact(ui.id("list-link"), cov);
            if (it.hovered && !c.cover.empty()) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.right_clicked && (page == Page::playlist || page == Page::album)) {
                shell_.dialogs.link_menu(ui, it.px, it.py, c);
            } else if (it.clicked && !c.cover.empty()) {
                shell_.dialogs.show_picture(ui, c.cover, 1);
            }
        }

        const float tx = cov.right() + (compact ? 20.f : 28.f), tw = head.right() - tx;
        const TextStyle sub = t.style(type::label, t.text_dim);
        const bool has_eyebrow = page == Page::playlist || page == Page::album || page == Page::similar;
        if (has_eyebrow) {
            const std::string_view kind = page == Page::album ? lyren::tr("Albums") : page == Page::similar ? lyren::tr("Similar songs") : lyren::tr("Playlists");
            ui.label({tx, head.y + 2, tw, 16}, kind, t.style(type::micro, t.faint(), text::semibold));
        }
        const bool built_in = page == Page::daily || page == Page::radar || page == Page::cloud || page == Page::liked || page == Page::recent;
        const float title_y = head.y + (has_eyebrow ? 20.f : 12.f);
        const float title_h = compact ? 30.f : 38.f;
        ui.label({tx, title_y, tw, title_h}, built_in ? lyren::tr(c.title) : std::string_view(c.title), t.style(compact ? type::title : type::display, t.text, text::semibold));
        const float sub_y = title_y + title_h + 4;
        const AlbumInfo *album = page == Page::album && browser_.route().list && browser_.route().list->loaded ? &browser_.route().list->album : nullptr;
        if (album && !album->artist.empty()) {
            // Album: the artist links to their page, then the date and the label; the notes open in full.
            const float aw = std::min(std::ceil(p.measure(album->artist, sub)) + 1, tw);
            if (!album->artist_id.empty()) {
                if (ui.link(ui.id("album-artist"), {tx, sub_y, aw, 20}, album->artist, sub)) {
                    browser_.open(Page::artist,
                                  {.kind = CardKind::artist, .source = c.source, .id = album->artist_id, .title = album->artist.substr(0, album->artist.find(" / "))});
                }
            } else {
                ui.label({tx, sub_y, aw, 20}, album->artist, sub);
            }
            std::string rest;
            for (const std::string *part : {&album->date, &album->company}) {
                if (!part->empty()) {
                    rest += " · " + *part;
                }
            }
            ui.label({tx + aw, sub_y, std::max(0.f, tw - aw), 20}, rest, sub);
            if (!album->about.empty() && !compact) {
                const Rect ab{tx, sub_y + 22, tw, 20};
                const ui::Id bid = ui.id("album-about");
                const auto bt = ui.interact(bid, ab);
                if (bt.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const float bh = ui.anim(bid, bt.hovered ? 1 : 0, pace::quick);
                std::string_view first = album->about;
                first = first.substr(0, first.find('\n'));
                p.text(tx, ab.y + 15, lyren::trf("About: {}", first), t.style(type::caption, t.text_dim.mix(t.text, 0.6f * bh)), tw);
                if (bt.clicked) {
                    ui.open_modal(560, std::min(ui.height - 120, 600.f), [title = c.title, about = album->about](ui::Ui &u, Rect mr) {
                        dialog_title(u, mr, title);
                        prose(u, {mr.x, mr.y + 48, mr.w + 16, mr.h - 48}, u.id("album-notes"), {{"", about}});
                    });
                }
            }
        } else {
            const std::string subtitle = page == Page::daily ? lyren::trf("{} · Made for your taste", date_label()) : card_subtitle(c);
            ui.label({tx, sub_y, tw, 20}, built_in ? lyren::tr(subtitle) : std::string_view(subtitle), sub);
        }
        const float next_x = play_buttons(ui, tx, head.bottom() - btn_h, tracks, btn_h, compact);
        list_actions(ui, next_x, head.bottom() - btn_h, btn_h, compact);

        Rect body{head.x - 12, head.bottom() + 24, head.w + 24, r.bottom() - head.bottom() - 32};
        const bool login = Browser::needs_login(page) && !signed_in();
        std::string error = list_error;
        if (tracks.empty() && error.empty() && !loading && !login) {
            error = page == Page::similar ? lyren::tr("No similar songs found") : lyren::tr("No songs here yet");
        }
        if (tracks.empty() && page_status(ui, body, loading, error, login, Placeholder::rows)) {
            if (!loading && !login && !list_error.empty()) {
                const auto centre = ui::center(body, std::min(body.w, 360.f), 156);
                const float rw = ui.button_width(lyren::tr("Retry"), true);
                if (ui.button(ui.id("list-retry"), {centre.x + (centre.w - rw) / 2, centre.y + 112, rw, control::medium}, lyren::tr("Retry"), ui::ButtonKind::secondary,
                              "refresh-cw")) {
                    if (page == Page::recent) {
                        spawn(library_.load_recent_plays(c.source, true));
                    } else if (browser_.route().list) {
                        spawn(browser_.load_list(browser_.route().list, page, c));
                    } else {
                        spawn(library_.load(browser_.active()));
                    }
                }
            }
            return;
        }
        if (!list_error.empty()) {
            const Rect retry = ui::cut_top(body, 40).inset(12, 0);
            const float w = ui.button_width(lyren::tr("Retry"), true);
            ui.label({retry.x, retry.y, retry.w - w - 12, 32}, lyren::tr(list_error), t.style(type::caption, t.danger));
            if (ui.button(ui.id("list-retry"), {retry.right() - w, retry.y, w, control::small + 4}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                if (page == Page::recent) {
                    spawn(library_.load_recent_plays(c.source, true));
                } else {
                    spawn(browser_.load_list(browser_.route().list, page, c));
                    if (c.owned) {
                        spawn(library_.load(c.source));
                    }
                }
            }
        }
        shell_.tracks.list(ui, body, scroll_id(ui, "tracks"), tracks, std::max(browser_.entered + 0.04, since));
    }

    void Pages::list_actions(ui::Ui &ui, float x, float y, float height, bool compact) {
        const Card &c = browser_.route().card;
        const float gap = compact ? 8.f : 10.f;
        auto square = [&](std::string_view id, std::string_view icon, std::string_view tip) {
            const bool clicked = ui.button(ui.id(id), {x, y, height, height}, "", ui::ButtonKind::secondary, icon, tip);
            x += height + gap;
            return clicked;
        };
        auto labelled = [&](std::string_view id, std::string_view label, std::string_view icon, float w, ui::ButtonKind kind = ui::ButtonKind::secondary) {
            const float bw = compact ? height : w;
            const bool clicked = ui.button(ui.id(id), {x, y, bw, height}, compact ? std::string_view{} : label, kind, icon, compact ? label : std::string_view{});
            x += bw + gap;
            return clicked;
        };
        if (browser_.route().page == Page::album) {
            if (!browser_.route().list || !browser_.route().list->loaded) {
                return;
            }
            const bool on = library_.kept(c.source, Library::Preference::album, c.id, browser_.route().list->album.collected);
            const float w = std::max(ui.button_width(lyren::tr("Saved"), true), ui.button_width(lyren::tr("Save"), true));
            if (provider(c).capabilities.collect_albums && labelled("collect", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                library_.toggle_collect(
                    {.kind = CardKind::album, .source = c.source, .id = c.id, .title = c.title, .subtitle = browser_.route().list->album.artist, .cover = c.cover},
                    browser_.route().list->album.collected);
            }
            if (provider(c).capabilities.comments[1] && square("album-comments", "message-square", lyren::tr("Comments"))) {
                shell_.open_comments(c.source, Subject::album, browser_.route().list->album.thread, c.title, c.cover);
            }
        } else if (browser_.route().page == Page::radar) {
            if (browser_.route().list && !browser_.route().list->loading && square("radar-refresh", "refresh-cw", lyren::tr("Refresh"))) {
                spawn(browser_.load_list(browser_.route().list, Page::radar, c));
            }
        } else if (browser_.route().page == Page::liked) {
            if (provider().features.heartbeat && signed_in() && !shelf().liked.id.empty() && browser_.route().list && !browser_.route().list->tracks.empty()) {
                if (labelled("heartbeat", lyren::tr("Heartbeat"), "heart-pulse", ui.button_width(lyren::tr("Heartbeat"), true), ui::ButtonKind::tonal)) {
                    // From the picked song, otherwise from any of them.
                    const auto &v = browser_.route().list->tracks;
                    const size_t seed = browser_.selection().selected >= 0 && browser_.selection().selected < static_cast<int>(v.size())
                                            ? static_cast<size_t>(browser_.selection().selected)
                                            : playback_.random() % v.size();
                    spawn(playback_.heartbeat(v[seed], library_.shelf(v[seed].source).liked.id));
                }
            }
            if (signed_in() && browser_.route().list && !browser_.route().list->loading && square("liked-refresh", "refresh-cw", lyren::tr("Refresh"))) {
                spawn(browser_.load_list(browser_.route().list, Page::liked, c));
            }
        } else if (browser_.route().page == Page::cloud) {
            if (signed_in() && browser_.route().list && !browser_.route().list->loading && square("cloud-refresh", "refresh-cw", lyren::tr("Refresh"))) {
                spawn(browser_.load_list(browser_.route().list, Page::cloud, c));
            }
        } else if (browser_.route().page == Page::recent) {
            const std::array<std::string_view, 2> tabs = {lyren::tr("This device"), lyren::tr("Cloud")};
            const float tabs_w = std::max(160.f, ui.button_width(tabs[0], false) + ui.button_width(tabs[1], false) - 12);
            if (provider().features.recent_plays && signed_in()) {
                if (ui.segmented(ui.id("recent-tabs"), {x, y + (height - control::medium) / 2, tabs_w, control::medium}, tabs, shelf().recent_tab)) {
                    browser_.tab_since = ui.now;
                    browser_.reset_selection();
                    ui.scroll_to(scroll_id(ui, "tracks"), 0);
                    browser_.ensure();
                }
                x += tabs_w + gap;
                if (shelf().recent_tab == 1 && !shelf().recent_plays.loading && square("recent-refresh", "refresh-cw", lyren::tr("Refresh"))) {
                    spawn(library_.load_recent_plays(browser_.active(), true));
                }
            }
        } else if (browser_.route().page == Page::playlist) {
            if (provider(c).capabilities.collect_playlists && c.collectable && !c.owned && browser_.route().list) {
                const bool on = std::ranges::any_of(library_.shelf(c.source).playlists, [&](const Card &card) {
                    return same(card, c);
                });
                const float w = std::max(ui.button_width(lyren::tr("Saved"), true), ui.button_width(lyren::tr("Save"), true));
                if (labelled("collect-playlist", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                    library_.toggle_collect_playlist(c);
                }
            }
            if (provider(c).capabilities.comments[2] && c.commentable && square("playlist-comments", "message-square", lyren::tr("Comments"))) {
                shell_.open_comments(c.source, Subject::playlist, c.thread.empty() ? c.id : c.thread, c.title, c.cover);
            }
            if ((!c.owned || !provider(c).capabilities.playlist_edit) && browser_.route().list && !browser_.route().list->loading &&
                square("playlist-refresh", "refresh-cw", lyren::tr("Refresh"))) {
                spawn(browser_.load_list(browser_.route().list, Page::playlist, c));
            }
            if (c.owned && provider(c).capabilities.playlist_edit && square("playlist-more", "ellipsis", lyren::tr("More"))) {
                if (library_.shelf(c.source).playlist_writes.contains(c.id)) {
                    return env_.notify(std::string(lyren::tr("The playlist is still saving")), "list-music");
                }
                std::vector<ui::MenuItem> items;
                if (provider(c).capabilities.playlist_order && browser_.route().list && !browser_.route().list->loading) {
                    items.push_back(
                        {"list-ordered", std::string(lyren::tr(browser_.route().list->ordering ? "Done reordering" : "Reorder songs")), [this, c, list = browser_.route().list] {
                             if (!provider(c).logged_in()) {
                                 return env_.notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(c.source).name)), "user");
                             }
                             list->ordering = !list->ordering;
                             if (list->ordering && list->refreshing) {
                                 ++list->gen;
                                 list->refreshing = false;
                             }
                             browser_.reset_selection();
                             if (list->ordering) {
                                 env_.notify(std::string(lyren::tr("Use the arrows to move songs")), "list-ordered");
                             }
                         }});
                }
                if (browser_.route().list && !browser_.route().list->loading && !browser_.route().list->saving_order) {
                    items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this, c, list = browser_.route().list] {
                                         spawn(browser_.load_list(list, Page::playlist, c));
                                         spawn(library_.load(c.source));
                                     }});
                }
                if (auto link = share_url(c); !link.empty()) {
                    items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                         ui.copy = link;
                                         ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                     }});
                }
                items.push_back({"pencil", std::string(lyren::tr("Rename")), [this, &ui, c] {
                                     shell_.dialogs.edit_playlist(ui, c);
                                 }});
                items.push_back({"trash", std::string(lyren::tr("Delete playlist")),
                                 [this, &ui, c] {
                                     shell_.dialogs.confirm_delete(ui, c);
                                 },
                                 true});
                ui.open_menu(x - height - gap, y + height + 6, std::move(items));
            }
        }
    }

    void Pages::categories_modal(ui::Ui &ui, Rect r) {
        auto &lib = shelf();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.resize_modal(std::min(680.f, ui.width - 40), std::min(620.f, ui.height - 40));
        dialog_title(ui, r, lyren::tr("All categories"));
        const Rect viewport{r.x, r.y + 46, r.w, r.h - 46};
        const Category *picked = nullptr;
        auto layout = [&](float y, bool draw) {
            bool first_group = true;
            for (const auto &[heading, categories] : lib.categories.groups) {
                if (!first_group && draw) {
                    p.rect({r.x, y - 14, r.w - 8, 1}, t.line());
                }
                first_group = false;
                if (draw) {
                    ui.label({r.x, y, r.w, 22}, heading, t.style(type::label, t.text_dim, text::semibold));
                }
                y += 30;
                float x = r.x;
                for (const auto &category : categories) {
                    const float w = std::min(ui.chip_width(category.name), r.w - 8);
                    if (x + w > r.right() - 8) {
                        x = r.x;
                        y += 38;
                    }
                    if (draw) {
                        if (ui.chip(ui.id(category.id), {x, y, w, 30}, category.name, category.id == lib.category.id)) {
                            picked = &category;
                        }
                    }
                    x += w + 8;
                }
                y += 56;
            }
            return y;
        };
        ui.push_id(ui.id("all-categories"));
        const float offset = ui.begin_scroll(ui.id("scroll"), viewport, layout(0, false));
        layout(viewport.y - offset, true);
        ui.end_scroll();
        ui.pop_id();
        if (picked) {
            library_.set_category(browser_.active(), *picked);
            ui.close_modal();
        }
    }

} // namespace lyren
