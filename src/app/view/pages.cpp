#include "app/view/pages.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <inplace_vector>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "audio/spool.hpp"
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
            playback_.play_all(tracks, false);
        }
        x += play_w + gap;
        if (ui.button(ui.id("shuffle-all"), {x, y, shuffle_w, height}, shuffle_label, ui::ButtonKind::secondary, "shuffle", compact ? lyren::tr("Shuffle") : std::string_view{})) {
            playback_.play_all(tracks, true);
        }
        return x + shuffle_w + gap;
    }

    float Pages::collection_button(ui::Ui &ui, float x, float y, float height, bool compact) {
        const auto &route = browser_.route();
        const Card &c = route.card;
        const bool artist = route.page == Page::artist;
        const auto reported = artist ? route.artist->info->followed : route.list->album.collected;
        const auto kept = library_.kept(c.source, artist ? Library::Preference::artist : Library::Preference::album, c.id, reported);
        const auto yes = lyren::tr(artist ? "Following" : "Saved"), no = lyren::tr(artist ? "Follow" : "Save");
        const Rect r{x, y, compact ? height : std::max(ui.button_width(yes, true), ui.button_width(no, true)), height};
        if (!kept) {
            if (library_.shelf(c.source).collection[artist].loading) {
                ui.spinner(ui::center(r, 16, 16), ui.theme().text_dim, 2);
            } else {
                const auto retry = lyren::tr("Retry");
                if (ui.button(ui.id("collection-retry"), r, compact ? std::string_view{} : retry, ui::ButtonKind::secondary, "refresh-cw", compact ? retry : std::string_view{})) {
                    spawn(library_.load_collection(c.source));
                }
            }
            return r.w;
        }
        const bool on = *kept;
        const auto label = on ? yes : no;
        const auto icon = artist ? (on ? "user-check" : "user-plus") : (on ? "star-fill" : "star");
        const auto kind = artist && !on ? ui::ButtonKind::tonal : ui::ButtonKind::secondary;
        if (ui.button(ui.id(artist ? "follow" : "collect"), r, compact ? std::string_view{} : label, kind, icon, compact ? label : std::string_view{})) {
            if (artist) {
                const auto &info = *route.artist->info;
                library_.set_followed({.kind = CardKind::artist, .source = c.source, .id = c.id, .title = info.name, .cover = info.avatar}, !on);
            } else {
                library_.set_collected({.kind = CardKind::album, .source = c.source, .id = c.id, .title = c.title, .subtitle = route.list->album.artist, .cover = c.cover}, !on);
            }
        }
        return r.w;
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

    bool Pages::hero(ui::Ui &ui, ui::Id id, HeroSlot slot, std::string_view big, std::string_view badge, std::string_view title, std::string_view subtitle, std::string_view icon,
                     std::string_view action_icon) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect r = slot.rect;
        const float dock = std::clamp(slot.dock, 0.f, 1.f);
        const auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = ui.feel(id, it, pace::steady);
        const Rect card = Rect{r.x, r.y - std::lerp(1.f, 0.5f, dock) * hot, r.w, r.h}.inset(down * std::lerp(1.f, 0.6f, dock));
        const float radius = std::lerp(t.radius_l() * 0.85f, t.round(36.f), dock);
        // Calm monochrome architectural surface across all cards: dark mode uses layered neutral fills, while light
        // mode pairs a deeper neutral tint with a whisper hairline so card silhouettes read crisply on white paper.
        const ui::Color full_paper = t.dark ? t.fill(0.90f + 0.50f * hot) : t.fill(1.45f + 0.70f * hot);
        const ui::Color dock_paper = t.dark ? t.fill(0.84f + 0.56f * hot) : t.fill(1.40f + 0.65f * hot);
        const ui::Color card_edge = t.dark ? ui::Color{} : t.border.with_alpha(0.70f + 0.30f * hot);
        p.rect(card, full_paper.mix(dock_paper, dock), radius, card_edge, t.dark ? 0.f : 1.f);
        if (slot.icon_only && dock > 0.75f && it.hovered && !it.held) {
            ui.tooltip(id, card, title);
        }

        const Rect emblem_full{card.x + 14, card.y + (card.h - 44) / 2, 44, 44};
        const Rect emblem_dock = slot.icon_only ? ui::center(card, 26, 26) : Rect{card.x + 5, card.y + (card.h - 26) / 2, 26, 26};
        const Rect emblem = mix(emblem_full, emblem_dock, dock);
        const float emblem_r = std::lerp(t.radius, emblem.h / 2, dock);
        const ui::Color emblem_rest = t.dark ? t.fill(1.25f) : t.surface;
        const ui::Color emblem_hot = t.dark ? t.accent.with_alpha(0.20f) : t.surface.mix(t.accent, 0.14f);
        const ui::Color emblem_edge = t.dark ? ui::Color{} : t.border.with_alpha(0.85f).mix(t.accent.with_alpha(0.35f), hot);
        p.rect(emblem, emblem_rest.mix(emblem_hot, hot), emblem_r, emblem_edge, t.dark ? 0.f : 1.f);

        if (icon.empty()) {
            // Calendar leaf at rest (quiet accent month above the crisp day), morphing directly into the docked badge.
            if (!badge.empty()) {
                if (const float month_a = 1 - smooth01(dock / 0.35f); month_a > 0.01f) {
                    Fade mf(p, month_a);
                    const float mfs = std::min(t.fs(10.f), 11.f);
                    ui.label({emblem.x, emblem.y + 4, emblem.w, 12}, badge, {.size = mfs, .color = t.accent.with_alpha(0.92f), .features = text::semibold}, Align::center);
                }
            }
            const Rect day_full{emblem.x, emblem.y + (badge.empty() ? 0.f : 16.f), emblem.w, badge.empty() ? emblem.h : 24.f};
            const Rect day_r = mix(day_full, emblem, dock);
            const float full_fs = std::min(t.fs(19.f), 20.f);
            const float dock_fs = std::min(t.fs(type::caption), emblem.h * 0.54f);
            const ui::Color day_c = t.text.mix(t.accent, std::max(hot * 0.85f, dock * 0.85f));
            if (dock <= 0.01f) {
                ui.label(day_full, big, {.size = full_fs, .color = day_c, .features = text::semibold | text::tabular_nums}, Align::center);
            } else if (dock >= 0.99f) {
                ui.label(emblem, big, {.size = dock_fs, .color = day_c, .features = text::semibold | text::tabular_nums}, Align::center);
            } else {
                const float s = std::lerp(1.f, dock_fs / full_fs, dock);
                p.set_zoom(s, day_r.x + day_r.w / 2, day_r.y + day_r.h / 2);
                ui.label({day_r.x + (day_r.w - day_r.w / s) / 2, day_r.y + (day_r.h - day_r.h / s) / 2, day_r.w / s, day_r.h / s}, big,
                         {.size = full_fs, .color = day_c, .features = text::semibold | text::tabular_nums}, Align::center);
                p.set_zoom(1);
            }
        } else {
            const float rest_a = t.dark ? std::lerp(0.78f, 0.72f, dock) : std::lerp(0.86f, 0.80f, dock);
            const ui::Color glyph_c = t.text.with_alpha(rest_a).mix(t.accent, hot);
            if (dock >= 0.99f) {
                p.icon(icon, ui::center(emblem, 16, 16), glyph_c);
            } else if (dock <= 0.01f) {
                p.icon(icon, ui::center(emblem, 22, 22), glyph_c);
            } else {
                p.set_zoom(std::lerp(1.f, 16.f / 22.f, dock), emblem.x + emblem.w / 2, emblem.y + emblem.h / 2);
                p.icon(icon, ui::center(emblem, 22, 22), glyph_c);
                p.set_zoom(1);
            }
        }

        const bool play = action_icon == "play-fill";
        const float tx_full = card.x + 68;
        const float tw_full = std::max(0.f, card.w - 68 - (play ? 48.f : 36.f));
        const float title_h = std::max(22.f, std::ceil(t.fs(type::lead) * 1.45f));
        const float sub_h = std::max(18.f, std::ceil(t.fs(type::caption) * 1.45f));
        const float title_y = card.y + std::floor((card.h - title_h - 2.f - sub_h) / 2);
        if (const float sub_a = 1 - smooth01(dock / 0.30f); sub_a > 0.01f) {
            const float sub_y = std::min(title_y + title_h + 2.f - 12 * dock, card.bottom() - sub_h - 2.f);
            if (sub_y >= card.y) {
                Fade f(p, sub_a);
                ui.label({tx_full, sub_y, tw_full, sub_h}, subtitle, t.style(type::caption, t.text_dim));
            }
        }

        const Rect title_full{tx_full, title_y, tw_full, title_h};
        if (slot.icon_only) {
            if (const float title_a = 1 - smooth01(dock / 0.45f); title_a > 0.01f) {
                Fade f(p, title_a);
                ui.label(title_full, title, t.style(type::lead, t.text, text::semibold));
            }
        } else {
            const float tx = emblem_dock.right() + 8;
            const Rect title_dock{tx, card.y + (card.h - 20) / 2, std::max(0.f, card.right() - tx - (slot.show_action ? 36.f : 12.f)), 20};
            const Rect tr = mix(title_full, title_dock, dock);
            if (dock >= 0.99f) {
                ui.label(title_dock, title, t.style(type::label, t.text, text::medium));
            } else if (dock <= 0.01f) {
                ui.label(title_full, title, t.style(type::lead, t.text, text::semibold));
            } else {
                const float s = std::lerp(1.f, type::label / type::lead, dock);
                p.set_zoom(s, tr.x, tr.y + tr.h / 2);
                ui.label({tr.x, tr.y, tr.w / s, tr.h}, title, t.style(type::lead, t.text, text::semibold));
                p.set_zoom(1);
            }
        }

        const bool keep_disc = slot.show_action && !slot.icon_only;
        const float disc_a = keep_disc ? 1.f : 1 - smooth01(dock / 0.45f);
        if (disc_a > 0.01f) {
            Fade f(p, disc_a);
            const Rect disc_full =
                play ? Rect{card.right() - 46, card.y + (card.h - 32) / 2, 32, 32}.inset(-1.f * hot) : Rect{card.right() - 34, card.y + (card.h - 24) / 2, 24, 24};
            const Rect disc_dock = Rect{card.right() - 30, card.y + (card.h - 24) / 2, 24, 24}.inset(play ? -0.5f * hot : 0.f);
            Rect disc = keep_disc ? mix(disc_full, disc_dock, dock) : disc_full;
            ui::Color glyph_c;
            if (play) {
                const ui::Color disc_rest = t.dark ? t.fill(1.25f) : t.surface;
                const ui::Color disc_edge = t.dark ? ui::Color{} : t.border.with_alpha(0.85f * (1 - hot));
                p.rect(disc, disc_rest.mix(t.accent, hot), disc.w / 2, disc_edge, t.dark ? 0.f : 1.f);
                glyph_c = (t.dark ? t.text_dim : t.text.with_alpha(0.75f)).mix(t.on_accent(), hot);
            } else {
                disc.x += std::lerp(2.f, 1.5f, dock) * hot;
                glyph_c = (t.dark ? t.faint() : t.text_dim.with_alpha(0.78f)).mix(t.text, hot);
            }
            const float full_ic = play ? 15.f : 17.f, dock_ic = play ? 13.f : 14.f;
            if (!keep_disc || dock <= 0.01f) {
                Rect glyph = ui::center(disc, full_ic, full_ic);
                if (play) {
                    glyph.x += 1;
                }
                p.icon(action_icon, glyph, glyph_c);
            } else if (dock >= 0.99f) {
                Rect glyph = ui::center(disc, dock_ic, dock_ic);
                if (play) {
                    glyph.x += 0.5f;
                }
                p.icon(action_icon, glyph, glyph_c);
            } else {
                p.set_zoom(std::lerp(1.f, dock_ic / full_ic, dock), disc.x + disc.w / 2, disc.y + disc.h / 2);
                Rect glyph = ui::center(disc, full_ic, full_ic);
                if (play) {
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
            case Page::pinned:
                return 0.52f;
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
            case Page::stats:
                return stats_page(ui, r);
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
        if (route.page == Page::pinned) {
            return list_page(ui, r, pins_.tracks(), false, "", browser_.entered);
        }
        // What played on this device is at hand; cloud recent plays live on the platform shelf.
        if (route.page == Page::recent) {
            if (shelf().recent_tab == 0 || !signed_in()) {
                const auto &played = playback_.history(browser_.active());
                return list_page(ui, r, played, false, "", std::max(browser_.entered, browser_.tab_since));
            }
            const auto &recent = shelf().recent_plays;
            return list_page(ui, r, recent.tracks, recent.loading, recent.error, std::max(recent.since, browser_.tab_since));
        }
        // No list yet: the account is still loading, or the page waits for a login.
        if (!route.list) {
            static const std::vector<Track> none;
            const std::string_view failed = route.page == Page::liked ? shelf().error : std::string_view{};
            return list_page(ui, r, none, failed.empty(), failed, browser_.tab_since);
        }
        Browser::TrackList &list = *route.list;
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
        };
        std::inplace_vector<Hero, 4> heroes;
        const std::string_view day = browser_.today().substr(3);
        const int month = (browser_.today()[0] - '0') * 10 + browser_.today()[1] - '0' - 1;
        const std::string_view month_lbl = month_short(month);
        const std::string date = date_label();
        if (features.daily.songs) {
            heroes.push_back({Page::daily, "hero-daily", day, month_lbl, "Daily mix", "Fresh picks, every day", {}, "play-fill"});
        }
        if (browser_.supports(Page::fm, browser_.active())) {
            heroes.push_back({Page::fm, "hero-fm", {}, {}, provider().radio_title, "A station that learns your taste", "radio", "play-fill"});
        }
        if (features.radar) {
            heroes.push_back({Page::radar, "hero-radar", {}, {}, "Radar", "Favorites and new discoveries", "radio", "play-fill"});
        }
        if (browser_.supports(Page::releases, browser_.active())) {
            heroes.push_back({Page::releases, "hero-releases", {}, {}, "New releases", "Latest songs and albums", "disc-3", "chevron-right"});
        }
        const int hero_count = static_cast<int>(heroes.size());
        const Rect body = page_frame(r);
        const CardGrid daily(ui, ui.id("daily-cards"), body.w);
        const CardGrid g(ui, ui.id("rec-cards"), body.w);
        constexpr float top_pad = 28, title_h = 76;
        const float hero_title_h = std::max(22.f, std::ceil(t.fs(type::lead) * 1.45f));
        const float hero_sub_h = std::max(18.f, std::ceil(t.fs(type::caption) * 1.45f));
        const float hero_h = std::max(80.f, 19.f + hero_title_h + 2.f + hero_sub_h + 19.f);
        constexpr float hero_gap = 14, after_heroes = 18;
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
                sum_full += title_ws[idx] + 81.f;
                sum_compact += title_ws[idx] + 55.f;
            }
            const float gaps_w = hero_count > 1 ? pill_gap * static_cast<float>(hero_count - 1) : 0.f;
            dock_action = sum_full + gaps_w <= body.w;
            const bool dock_natural = dock_action || (sum_compact + gaps_w <= body.w);
            const float each_w = (body.w - gaps_w) / static_cast<float>(hero_count);
            dock_icon_only = !dock_natural && each_w < 84.f;
            float dx = body.x;
            for (int i = 0; i < hero_count; ++i) {
                const auto idx = static_cast<size_t>(i);
                const float w = dock_action ? title_ws[idx] + 81.f : dock_natural ? title_ws[idx] + 55.f : each_w;
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
            const float collapse0 = hero_h - pill_h;
            const float row_step = hero_h + hero_gap;
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
                if (hero(ui, ui.id(entry.id), {rect, dock, dock_action, dock_icon_only}, entry.big, entry.badge, titles[idx], lyren::tr(entry.subtitle), entry.icon,
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
            section_heading(ui, body.x, y, body.w, lyren::tr("Daily playlists"), int_text(lib.daily_lists.size()));
            if (int i = card_grid(ui, {body.x, y + 42, body.w, 0}, ui.id("daily-cards"), lib.daily_lists, false, std::max(browser_.entered + 0.06, lib.daily_since)); i >= 0) {
                browser_.open(lib.daily_lists[static_cast<size_t>(i)]);
            }
            y += daily_h;
        }
        const Listing &res = lib.recommended;
        section_heading(ui, body.x, y, body.w, lyren::tr("Recommended playlists"), res.cards.empty() ? SmallText<24>{} : int_text(res.cards.size()));
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
                         lib.square.cards.empty() ? lib.square.error : std::string_view{}, false, Placeholder::cards)) {
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
        const auto &t = ui.theme();
        page_title(ui, page_frame(r), r.y + 28, lyren::tr(provider().radio_title), lyren::tr("Tuned to what you play"));
        const Rect below{r.x, r.y + 100, r.w, r.h - 100};
        if (page_status(ui, below, lib.radio_loading && lib.radio.empty(), lib.radio.empty() ? lib.radio_error : std::string_view{}, !signed_in())) {
            return;
        }
        if (lib.radio.empty()) {
            return;
        }
        lib.radio_pos = std::min(lib.radio_pos, static_cast<int>(lib.radio.size()) - 1);
        const Track &track = lib.radio[static_cast<size_t>(lib.radio_pos)];
        const bool on_air = playback_.radio(browser_.active()) && playback_.is_current(track);
        const bool active = on_air && playback_.playing();
        const float size = std::clamp(std::min(r.w * 0.36f, below.h - 236), 160.f, 320.f);
        const bool has_next = static_cast<size_t>(lib.radio_pos + 1) < lib.radio.size() && below.h >= size + 230;
        const Rect col = ui::center(below, size, size + (has_next ? 176.f : 148.f));
        const float presence = t.motion > 0 ? ui.spring(ui.id("fm-presence"), active ? 1.f : 0.f, 480) : 1.f;
        const float resting = 0.97f + 0.03f * presence;
        const Rect cov = ui::center({col.x, col.y, size, size}, size * resting, size * resting);

        cover(ui, cov, track.cover, t.radius_l());
        const auto art_it = ui.interact(ui.id("fm-cover"), cov);
        if (art_it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (art_it.right_clicked) {
            shell_.tracks.menu(ui, art_it.px, art_it.py, lib.radio, lib.radio_pos);
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
        auto next = [&] {
            const Source source = browser_.active();
            if (playback_.radio(source)) {
                playback_.skip(1);
            } else {
                library_.advance_radio(source);
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
                next();
                return;
            }
        }
        const bool play_clicked = play_morph_button(ui, ui.id("fm-play"), play_r, active, 18, true);
        const Rect next_r{play_r.right() + 12, by + 4, 40, 40};
        if (lib.radio_pending && lib.radio_loading) {
            ui.spinner(ui::center(next_r, 18, 18), t.text_dim, 2);
        } else if (ui.icon_button(ui.id("fm-next"), next_r, "skip-forward", 19, false, lyren::tr("Next"))) {
            next();
            return;
        }
        if (has_next) {
            const Track &upcoming = lib.radio[static_cast<size_t>(lib.radio_pos + 1)];
            ui.label({tx, play_r.bottom() + 20, text_w, 18}, std::format("{} · {} — {}", lyren::tr("Up next"), upcoming.title, upcoming.artist), t.style(type::caption, t.faint()),
                     Align::center);
        }
        if (play_clicked || art_it.clicked) {
            if (on_air) {
                playback_.toggle();
            } else {
                lib.radio_pending = false;
                playback_.play_radio(browser_.active(), lib.radio, lib.radio_pos);
            }
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
        if (res.size() == 0 && page_status(ui, tab ? content.inset(12, 12) : content, res.loading, res.error, false, tab ? Placeholder::cards : Placeholder::rows)) {
            return;
        }
        if (res.loaded && res.size() == 0) {
            return empty(ui, content, tab ? "disc-album" : "music", lyren::tr(tab ? "No albums found" : "No songs here yet"));
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
        if (res.cards.empty() &&
            page_status(ui, content.inset(12, 12), res.loading && !res.loaded, res.error, false, lib.collection_tab ? Placeholder::faces : Placeholder::cards)) {
            return;
        }
        if (res.loaded && res.cards.empty()) {
            return empty(ui, content, lib.collection_tab ? "users" : "disc-album", lyren::tr(lib.collection_tab ? "No followed artists yet" : "No saved albums yet"));
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
        const std::array icons = {"music", "mic-vocal", "audio-lines"};
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
        const float off = ui.begin_scroll(ui::sub_id(ui.id("dislikes"), static_cast<uint64_t>(lib.dislike_tab)), content, row_h * static_cast<float>(list.items.size()));
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

    void Pages::stats_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect body = page_frame(r);

        if (env_.store) {
            const uint64_t serial = env_.store->stats_serial();
            if (stats_cached_range_ != stats_range_ || stats_serial_ != serial) {
                const auto range = stats_range_ == 0 ? Store::StatsRange::week : stats_range_ == 1 ? Store::StatsRange::month : Store::StatsRange::all;
                stats_cache_ = env_.store->listening_stats(range);
                stats_tracks_.clear();
                stats_tracks_.reserve(stats_cache_.top_tracks.size());
                for (const auto &ts : stats_cache_.top_tracks) {
                    stats_tracks_.push_back(ts.track);
                }
                stats_cached_range_ = stats_range_;
                stats_serial_ = serial;
            }
        }
        const auto &stats = stats_cache_;

        auto format_listen_time = [](int64_t seconds) -> SmallText<24> {
            if (seconds <= 0) {
                return small_format<24>("0m");
            }
            if (seconds < 60) {
                return small_format<24>("{}s", seconds);
            }
            const int64_t mins = seconds / 60;
            if (mins < 60) {
                return small_format<24>("{}m", mins);
            }
            const int64_t hours = mins / 60, rem = mins % 60;
            return rem > 0 ? small_format<24>("{}h {}m", hours, rem) : small_format<24>("{}h", hours);
        };

        auto source_label = [&](std::string_view sid) -> std::string_view {
            if (const auto sk = parse_source_key(sid)) {
                if (const auto *entry = env_.providers.entry(sk->source, sk->instance)) {
                    return lyren::tr(entry->name);
                }
                return lyren::tr(platform(sk->source).name);
            }
            return sid;
        };

        const std::array<std::string_view, 3> range_labels{lyren::tr("7 days"), lyren::tr("30 days"), lyren::tr("All time")};
        float widest = 0;
        for (auto l : range_labels) {
            widest = std::max(widest, p.measure(l, t.style(type::label, {}, text::medium)));
        }
        const float seg_w = std::min(std::max(204.f, (std::ceil(widest) + 26) * 3), body.w);
        const bool stacked_hdr = body.w < seg_w + 260;
        const float hdr_h = stacked_hdr ? 114.f : 74.f;

        const size_t n_sources = stats.by_source.size();
        const size_t n_artists = std::min<size_t>(stats.top_artists.size(), 8);
        const size_t n_tracks = std::min<size_t>(stats.top_tracks.size(), 15);
        const int artist_cols = body.w >= 620 ? 2 : 1;
        const size_t artist_rows = n_artists ? (n_artists + static_cast<size_t>(artist_cols) - 1) / static_cast<size_t>(artist_cols) : 0;

        float content_h = 28.f + hdr_h;
        if (stats.total_plays == 0) {
            content_h += 280.f;
        } else {
            content_h += 86.f + 24.f; // 3 summary metric cards
            if (n_sources > 0) {
                content_h += 34.f + 20.f + static_cast<float>(n_sources) * 34.f + 26.f;
            }
            if (n_artists > 0) {
                content_h += 34.f + static_cast<float>(artist_rows) * 52.f - 6.f + 26.f;
            }
            if (n_tracks > 0) {
                content_h += 34.f + static_cast<float>(n_tracks) * 54.f + 28.f;
            }
        }

        const float off = ui.begin_scroll(ui.id("stats-scroll"), r, content_h);
        float y = r.y + 28 - off;
        const Rect title_box{body.x, y, stacked_hdr ? body.w : std::max(140.f, body.w - seg_w - 16), 66};
        page_title(ui, title_box, y, lyren::tr("Listening stats"), lyren::tr("Local listening across all platforms"));
        const Rect seg_r = stacked_hdr ? Rect{body.x, y + 72, std::min(seg_w, body.w), control::medium} : Rect{body.right() - seg_w, y + 6, seg_w, control::medium};
        if (ui.segmented(ui.id("stats-range"), seg_r, range_labels, stats_range_)) {
            browser_.tab_since = ui.now;
            ui.redraw();
        }
        y += hdr_h;

        if (stats.total_plays == 0) {
            empty(ui, {body.x, y, body.w, std::max(240.f, r.bottom() - y)}, "bar-chart", lyren::tr("No listening stats yet"),
                  lyren::tr("Play songs on any platform to build your listening overview"));
            ui.end_scroll();
            return;
        }

        const double since = std::max(browser_.entered, browser_.tab_since);
        const ui::Color card_bg = t.dark ? t.fill(0.90f) : t.bg.mix(t.surface, 0.35f);
        const ui::Color card_edge = t.dark ? t.surface_edge(0.9f) : t.border.with_alpha(0.75f);

        // Summary metric cards.
        {
            constexpr float gap = 12.f, card_h = 86.f;
            const float cw = (body.w - gap * 2) / 3.f;
            const auto time_text = format_listen_time(stats.total_seconds);
            const auto plays_text = int_text(stats.total_plays);
            const auto songs_text = int_text(stats.unique_tracks);
            const std::string artists_sub = lyren::trf("{} artists", stats.unique_artists);
            struct Metric {
                std::string_view label, value, sub;
            };
            const std::array<Metric, 3> metrics{{
                {lyren::tr("Listening time"), time_text, {}},
                {lyren::tr("Plays"), plays_text, {}},
                {lyren::tr("Songs played"), songs_text, artists_sub},
            }};
            for (size_t i = 0; i < metrics.size(); ++i) {
                const float in = ui.cascade(since, static_cast<int>(i), pace::arrive);
                Fade fade(p, in);
                const Rect card{body.x + static_cast<float>(i) * (cw + gap), y + (1 - in) * 6, cw, card_h};
                p.rect(card, card_bg, t.radius, card_edge, 1);
                ui.label({card.x + 16, card.y + 13, card.w - 32, 18}, metrics[i].label, t.style(type::caption, t.text_dim));
                if (metrics[i].sub.empty()) {
                    ui.label({card.x + 16, card.y + 38, card.w - 32, 32}, metrics[i].value, t.style(type::display, t.text, text::semibold | text::tabular_nums));
                } else {
                    const auto val_st = t.style(type::display, t.text, text::semibold | text::tabular_nums);
                    const float vw = std::min(std::ceil(p.measure(metrics[i].value, val_st)) + 1, card.w - 32);
                    ui.label({card.x + 16, card.y + 38, vw, 32}, metrics[i].value, val_st);
                    if (card.w - 32 - vw > 54) {
                        ui.label({card.x + 16 + vw + 10, card.y + 46, card.w - 42 - vw, 20}, metrics[i].sub, t.style(type::caption, t.faint(), text::tabular_nums));
                    }
                }
            }
            y += card_h + 24.f;
        }

        // Platform distribution.
        if (n_sources > 0) {
            section_heading(ui, body.x, y, body.w, lyren::tr("By platform"));
            y += 34.f;
            const float box_h = 20.f + static_cast<float>(n_sources) * 34.f;
            const Rect box{body.x, y, body.w, box_h};
            p.rect(box, card_bg, t.radius, card_edge, 1);
            const int64_t total_sec = std::max<int64_t>(1, stats.total_seconds);
            float sy = box.y + 10.f;
            for (const auto &src : stats.by_source) {
                const Rect row{box.x + 16, sy, box.w - 32, 34.f};
                const float ratio = std::clamp(static_cast<float>(src.seconds) / static_cast<float>(total_sec), 0.f, 1.f);
                const int pct = static_cast<int>(std::lround(ratio * 100.f));
                const auto dur = format_listen_time(src.seconds);
                const auto detail = small_format<40>("{} · {}%", std::string_view(dur), pct);
                const float name_w = std::min(150.f, row.w * 0.34f);
                const float stat_w = 112.f;
                ui.label({row.x, row.y, name_w, row.h}, source_label(src.source_id), t.style(type::label, t.text, text::medium));
                const float bar_x = row.x + name_w + 12;
                const float bar_w = std::max(0.f, row.right() - stat_w - 12 - bar_x);
                if (bar_w >= 40) {
                    const Rect track{bar_x, row.y + (row.h - 5) / 2, bar_w, 5};
                    p.rect(track, t.fill(1.5f), 2.5f);
                    if (const float fw = std::max(ratio > 0 ? 5.f : 0.f, bar_w * ratio); fw > 0) {
                        p.rect({track.x, track.y, fw, track.h}, t.accent.with_alpha(0.75f), 2.5f);
                    }
                }
                ui.label({row.right() - stat_w, row.y, stat_w, row.h}, detail, t.style(type::caption, t.text_dim, text::tabular_nums), Align::right);
                sy += 34.f;
            }
            y += box_h + 26.f;
        }

        // Top artists.
        std::optional<Card> open_artist;
        if (n_artists > 0) {
            section_heading(ui, body.x, y, body.w, lyren::tr("Top artists"), int_text(n_artists));
            y += 34.f;
            constexpr float col_gap = 12.f, row_h = 46.f, row_gap = 6.f;
            const float col_w = (body.w - col_gap * static_cast<float>(artist_cols - 1)) / static_cast<float>(artist_cols);
            const int64_t peak = std::max<int64_t>(1, stats.top_artists.front().seconds);
            ui.push_id(ui.id("stats-artists"));
            for (size_t i = 0; i < n_artists; ++i) {
                const auto &art = stats.top_artists[i];
                const size_t r_idx = i / static_cast<size_t>(artist_cols), c_idx = i % static_cast<size_t>(artist_cols);
                const Rect row{body.x + static_cast<float>(c_idx) * (col_w + col_gap), y + static_cast<float>(r_idx) * (row_h + row_gap), col_w, row_h};
                const ui::Id aid = ui.id(static_cast<uint64_t>(i));
                const bool clickable = !art.artist_id.empty();
                const auto it = clickable ? ui.interact(aid, row) : ui::Interaction{};
                if (clickable && it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const float hot = clickable ? ui.anim(aid, it.hovered ? 1.f : 0.f, pace::quick) : 0.f;
                p.rect(row, card_bg.mix(t.fill(1.5f), hot * 0.55f), t.radius_s(), card_edge, 1);

                ui.label({row.x + 12, row.y, 24, row.h}, int_text(i + 1), t.style(type::caption, i < 3 ? t.accent : t.faint(), text::semibold | text::tabular_nums), Align::center);
                const auto dur = format_listen_time(art.seconds);
                const std::string right_txt = std::format("{} · {}", lyren::trf("{} plays", art.plays), std::string_view(dur));
                const float rw = std::min(132.f, std::ceil(p.measure(right_txt, t.style(type::caption, {}, text::tabular_nums))) + 6);
                const float nx = row.x + 40;
                const float nw = std::max(0.f, row.right() - 14 - rw - nx - 8);
                ui.label({nx, row.y + 8, nw, 20}, art.name, t.style(type::body, t.text, text::medium));
                ui.label({row.right() - 14 - rw, row.y + 8, rw, 20}, right_txt, t.style(type::caption, t.text_dim, text::tabular_nums), Align::right);
                const float bw = std::max(0.f, row.right() - 14 - nx);
                const float ratio = std::clamp(static_cast<float>(art.seconds) / static_cast<float>(peak), 0.f, 1.f);
                p.rect({nx, row.bottom() - 11, bw, 3}, t.fill(1.4f), 1.5f);
                if (const float fw = std::max(ratio > 0 ? 4.f : 0.f, bw * ratio); fw > 0) {
                    p.rect({nx, row.bottom() - 11, fw, 3}, t.accent.with_alpha(0.62f), 1.5f);
                }
                if (clickable && it.clicked) {
                    open_artist = Card{.kind = CardKind::artist, .source = art.source, .instance = art.instance, .id = art.artist_id, .title = art.name};
                }
            }
            ui.pop_id();
            y += static_cast<float>(artist_rows) * (row_h + row_gap) - row_gap + 26.f;
        }

        // Most played songs.
        if (n_tracks > 0) {
            section_heading(ui, body.x, y, body.w, lyren::tr("Most played"), int_text(n_tracks));
            y += 34.f;
            constexpr float row_h = 54.f;
            const Rect clip = ui.clip();
            ui.push_id(ui.id("stats-tracks"));
            for (size_t i = 0; i < n_tracks; ++i) {
                const Rect row{body.x - 4, y + static_cast<float>(i) * row_h, body.w + 8, row_h};
                if (row.bottom() < clip.y || row.y > clip.bottom()) {
                    continue;
                }
                const auto &item = stats.top_tracks[i];
                const Track &track = item.track;
                const ui::Id tid = ui.id(static_cast<uint64_t>(i));
                const bool on = playback_.is_current(track);
                const bool pointed = ui.hovering(row);
                const float hot = ui.anim(tid, pointed ? 1.f : 0.f, pace::quick);
                if (hot > 0 || on) {
                    p.rect(row.inset(0, 2), on ? t.accent.with_alpha(0.065f + 0.045f * hot) : t.fill(hot), t.radius_s());
                }
                const Rect idx_r{row.x + 6, row.y, 34, row.h};
                if (pointed) {
                    if (ui.icon_button(ui::sub_id(tid, 1), ui::center(idx_r, 30, 30), on && playback_.playing() ? "pause-fill" : "play-fill", 14)) {
                        if (on) {
                            playback_.toggle();
                        } else {
                            playback_.play(stats_tracks_, static_cast<int>(i));
                        }
                    }
                } else if (on) {
                    ui.equalizer(ui::sub_id(tid, 2), ui::center(idx_r, 14, 14), t.accent, playback_.playing());
                } else {
                    ui.label(idx_r, int_text(i + 1), t.style(type::label, t.faint(), text::tabular_nums), Align::center);
                }

                const Rect art{idx_r.right() + 4, row.y + (row.h - 38) / 2, 38, 38};
                cover(ui, art, track.cover, t.radius_s());

                const auto dur = format_listen_time(item.seconds);
                const std::string right_txt = std::format("{} · {}", lyren::trf("{} plays", item.plays), std::string_view(dur));
                const float right_w = std::min(152.f, std::ceil(p.measure(right_txt, t.style(type::caption, {}, text::tabular_nums))) + 6);
                ui.label({row.right() - 12 - right_w, row.y, right_w, row.h}, right_txt, t.style(type::caption, t.text_dim, text::tabular_nums), Align::right);

                float right_edge = row.right() - 12 - right_w - 10;
                if (n_sources > 1 && body.w >= 540) {
                    const auto *entry = env_.providers.entry(track.source, track.instance);
                    const std::string_view src_short = entry ? lyren::tr(entry->short_name) : lyren::tr(platform(track.source).short_name);
                    const auto pill_st = t.style(type::micro, t.text_dim);
                    const float pw = std::ceil(p.measure(src_short, pill_st)) + 14;
                    const Rect pill{right_edge - pw, row.y + (row.h - 20) / 2, pw, 20};
                    p.rect(pill, t.fill(1.0f), 10);
                    ui.label(pill, src_short, pill_st, Align::center);
                    right_edge = pill.x - 10;
                }

                const float tx = art.right() + 12;
                const float tw = std::max(0.f, right_edge - tx);
                ui.label({tx, row.y + 8, tw, 20}, track.title, t.style(type::body, on ? t.accent : t.text, on ? text::medium : 0));
                ui.label({tx, row.y + 29, tw, 18}, track.artist, t.style(type::caption, t.text_dim));

                const auto it = ui.interact(tid, row);
                if (it.double_clicked) {
                    playback_.play(stats_tracks_, static_cast<int>(i));
                } else if (it.right_clicked) {
                    shell_.tracks.menu(ui, it.px, it.py, stats_tracks_, static_cast<int>(i));
                }
            }
            ui.pop_id();
        }
        ui.end_scroll();

        if (open_artist) {
            browser_.open(Page::artist, *open_artist);
        }
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
        std::string about(lyren::tr(a.error));
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
            collection_button(ui, bx, head.bottom() - btn_h, btn_h, compact);
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
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::spinner};
        if (is_empty &&
            page_status(ui, a.tab == 1 ? content.inset(12, 12) : content, res.loading || (!res.loaded && res.error.empty()), res.error, false, looks[static_cast<size_t>(a.tab)])) {
            return;
        }
        if (is_empty && res.loaded) {
            return empty(ui, content, a.tab == 2 ? "user" : "music", lyren::tr(a.tab == 2 ? "No biography available" : "Nothing here yet"));
        }
        if (!res.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), lyren::tr(res.error), t.style(type::caption, t.danger));
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
        const uint64_t source_hash = std::hash<std::string_view>{}(browser_.active_id());
        return ui::sub_id(ui::sub_id(ui.id("search-results"), browser_.search_serial()), source_hash * 5 + static_cast<uint64_t>(tab));
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
        const auto *active_entry = env_.providers.entry(browser_.active(), browser_.active_key().instance);
        const std::string_view source_short = active_entry ? lyren::tr(active_entry->short_name) : lyren::tr(platform(browser_.active()).short_name);
        const std::array kinds = {lyren::tr("songs"), lyren::tr("albums"), lyren::tr("artists"), lyren::tr("playlists"), lyren::tr("users")};
        page_title(ui, body, r.y + 28, '"' + browser_.searched() + '"',
                   shown.loaded ? lyren::trf("{}{} {} found", shown.more() ? lyren::tr("About ") : "", shown.total, kinds[tab]) : lyren::trf("Searching {}", source_short));
        // Tab content slides toward the tab that was picked.
        Rect content{body.x - 12, tab_bar.bottom() + 10, body.w + 24, r.bottom() - tab_bar.bottom() - 18};
        Fade fade(ui.painter(), tab_enter(ui, content));
        const double since = std::max({browser_.entered, browser_.tab_since, shown.since});
        const bool is_empty = shown.size() == 0;
        if (is_empty && shown.more() && shown.error.empty()) {
            browser_.load_results(tab);
        }
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::faces, Placeholder::cards, Placeholder::faces};
        if (is_empty && page_status(ui, tab ? content.inset(12, 12) : content, shown.loading, shown.error, false, looks[static_cast<size_t>(tab)])) {
            return;
        }
        if (is_empty && shown.loaded && !shown.more()) {
            return empty(ui, content, "search", lyren::tr("No results found"));
        }
        if (!shown.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), lyren::tr(shown.error), t.style(type::caption, t.danger));
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

    void Pages::list_page(ui::Ui &ui, Rect r, const std::vector<Track> &tracks, bool loading, std::string_view list_error, double since) {
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
            case Page::pinned:
                built_in_art(p, t, cov, special_hue(Page::pinned), t.radius_l(), "pin");
                break;
            case Page::cloud:
                built_in_art(p, t, cov, special_hue(Page::cloud), t.radius_l(), "cloud");
                break;
            case Page::radar:
                built_in_art(p, t, cov, 0.78f, t.radius_l(), "radio");
                break;
            case Page::daily: {
                built_in_art(p, t, cov, 0.98f, t.radius_l(), "");
                ui.label(cov, std::string_view(browser_.today()).substr(3), t.style(compact ? 42.f : type::numeral + 8, white, text::semibold | text::tabular_nums), Align::center);
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
        const bool built_in = page == Page::daily || page == Page::radar || page == Page::cloud || page == Page::liked || page == Page::recent || page == Page::pinned;
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
            std::string subtitle;
            if (page == Page::daily) {
                subtitle = lyren::trf("{} · Made for your taste", date_label());
            } else if (page == Page::recent) {
                subtitle = shelf().recent_tab == 0 || !signed_in() ? lyren::trf("{} songs · On this device", tracks.size())
                                                                   : lyren::trf("{} songs · From all your devices", tracks.size());
            } else if (page == Page::liked && browser_.route().list) {
                subtitle = lyren::trf("{} songs", tracks.size());
            } else if (page == Page::pinned) {
                const bool caching = pins_.caching();
                if (pinned_cache_checked_ < 0 || ui.now - pinned_cache_checked_ >= 5.0 || pinned_cache_count_ != tracks.size() || pinned_was_caching_ != caching) {
                    pinned_cache_bytes_ = audio::Spool::pinned_cache_usage();
                    pinned_cache_checked_ = ui.now;
                    pinned_cache_count_ = tracks.size();
                    pinned_was_caching_ = caching;
                }
                subtitle = lyren::trf("{} songs · {}", tracks.size(), std::string_view(format_bytes(pinned_cache_bytes_)));
                if (caching) {
                    subtitle += " · ";
                    subtitle += lyren::tr("Caching...");
                }
            } else if (page == Page::cloud && browser_.route().list) {
                subtitle = lyren::trf("Cloud uploads · {} songs", tracks.size());
            } else if (built_in) {
                subtitle = std::string(lyren::tr(c.subtitle));
            } else {
                subtitle = card_subtitle(c);
            }
            ui.label({tx, sub_y, tw, 20}, subtitle, sub);
        }
        const float next_x = play_buttons(ui, tx, head.bottom() - btn_h, tracks, btn_h, compact);
        list_actions(ui, next_x, head.bottom() - btn_h, btn_h, compact);

        Rect body{head.x - 12, head.bottom() + 24, head.w + 24, r.bottom() - head.bottom() - 32};
        const bool login = Browser::needs_login(page) && !signed_in();
        if (tracks.empty() && page_status(ui, body, loading, list_error, login, Placeholder::rows)) {
            if (!loading && !login && !list_error.empty()) {
                const auto centre = ui::center(body, std::min(body.w, 360.f), 156);
                const float rw = ui.button_width(lyren::tr("Retry"), true);
                if (ui.button(ui.id("list-retry"), {centre.x + (centre.w - rw) / 2, centre.y + 112, rw, control::medium}, lyren::tr("Retry"), ui::ButtonKind::secondary,
                              "refresh-cw")) {
                    browser_.reload_list();
                }
            }
            return;
        }
        if (tracks.empty()) {
            if (page == Page::pinned) {
                return empty(ui, body, "pin", lyren::tr("No pinned songs yet"), lyren::tr("Pin songs or playlists to keep them available offline"));
            }
            return empty(ui, body, "music", lyren::tr(page == Page::similar ? "No similar songs found" : "No songs here yet"));
        }
        if (!list_error.empty()) {
            const Rect retry = ui::cut_top(body, 40).inset(12, 0);
            const float w = ui.button_width(lyren::tr("Retry"), true);
            ui.label({retry.x, retry.y, retry.w - w - 12, 32}, lyren::tr(list_error), t.style(type::caption, t.danger));
            if (ui.button(ui.id("list-retry"), {retry.right() - w, retry.y, w, control::small + 4}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                browser_.reload_list();
                if (c.owned) {
                    spawn(library_.load(c.source));
                }
            }
        }
        shell_.tracks.list(ui, body, scroll_id(ui, "tracks"), tracks, std::max(browser_.entered + 0.04, since));
    }

    void Pages::list_actions(ui::Ui &ui, float x, float y, float height, bool compact) {
        const Card &c = browser_.route().card;
        const float gap = compact ? 8.f : 10.f;
        auto square = [&](std::string_view id, std::string_view icon, std::string_view tip, bool active = false) {
            const bool clicked = ui.button(ui.id(id), {x, y, height, height}, "", active ? ui::ButtonKind::tonal : ui::ButtonKind::secondary, icon, tip);
            x += height + gap;
            return clicked;
        };
        auto comments = [&](std::string_view id, Subject subject, const std::string &thread) {
            if (square(id, "message-square", lyren::tr("Comments"), shell_.comments_open(c.source, subject, thread))) {
                shell_.toggle_comments(c.source, subject, thread, c.title, c.cover);
            }
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
            if (provider(c).capabilities.collect_albums) {
                x += collection_button(ui, x, y, height, compact) + gap;
            }
            if (!browser_.route().list->tracks.empty()) {
                const bool pinned = pins_.is_list_pinned(c);
                if (square("album-pin", pinned ? "pin-off" : "pin", lyren::tr(pinned ? "Unpin offline" : "Pin offline"), pinned)) {
                    pins_.toggle_pin_list(c, browser_.route().list->tracks);
                }
            }
            if (provider(c).capabilities.comments[1]) {
                comments("album-comments", Subject::album, browser_.route().list->album.thread);
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
            if (provider(c).capabilities.comments[2] && c.commentable) {
                comments("playlist-comments", Subject::playlist, c.thread.empty() ? c.id : c.thread);
            }
            if (browser_.route().list && !browser_.route().list->tracks.empty()) {
                const bool pinned = pins_.is_list_pinned(c);
                if (square("playlist-pin", pinned ? "pin-off" : "pin", lyren::tr(pinned ? "Unpin offline" : "Pin offline"), pinned)) {
                    pins_.toggle_pin_list(c, browser_.route().list->tracks);
                }
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
