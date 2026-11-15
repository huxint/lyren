#include "app/view/chrome.hpp"

#include <algorithm>
#include <cmath>
#include <inplace_vector>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    void Chrome::top_bar(ui::Ui &ui, Rect bar, float center_x) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // The stage keeps only the window buttons in its title strip.
        const float covered = shell_.stage.raised(ui);
        if (covered >= 1) {
            return;
        }
        Fade under_stage(p, 1 - covered);
        ui::Enabled reachable(ui, covered <= 0.5f, 1);
        Rect r = bar;
        const Rect nav = ui::cut_left(r, 40);
        {
            ui::Enabled possible(ui, browser_.can_go_back() || shell_.stage.open(), 0.3f);
            if (ui.icon_button(ui.id("back"), ui::center(nav, 32, 32), "chevron-left", 20, false, lyren::tr("Back"))) {
                shell_.back();
            }
        }
        // A fixed-width platform capsule keeps the search field still when the name changes.
        const auto name_style = t.style(type::label, t.text, text::medium);
        float platform_w = 0;
        for (const auto &entry : platforms) {
            platform_w = std::max(platform_w, p.measure(lyren::tr(entry.short_name), name_style));
        }
        platform_w = std::ceil(platform_w);
        const Rect selector = ui::center(ui::cut_left(r, platform_w + 52), platform_w + 52, 32);
        const ui::Id platform_id = ui.id("platform");
        const auto picked = ui.interact(platform_id, selector);
        const auto [hot, down] = ui.feel(platform_id, picked);
        const Rect pill = selector.inset(down);
        p.rect(pill, t.fill(1 + hot), t.round(pill.h));
        p.rect({pill.x + 13, pill.y + pill.h / 2 - 3, 6, 6}, t.accent, 3);
        ui.label({pill.x + 27, pill.y, platform_w + 1, pill.h}, lyren::tr(platform(browser_.active()).short_name), name_style);
        p.icon("chevron-down", {pill.right() - 20, pill.y + 11, 10, 10}, t.text_dim.mix(t.text, hot));
        if (picked.clicked) {
            std::vector<ui::MenuItem> items;
            for (const auto &entry : platforms) {
                items.push_back({entry.source == browser_.active() ? "check" : "music", std::string(lyren::tr(entry.name)), [this, source = entry.source] {
                                     browser_.switch_platform(source);
                                 }});
            }
            ui.open_menu(selector.x, selector.bottom() + 6, std::move(items));
        }
        ui::cut_left(r, 8);

        const Rect settings = ui::cut_right(r, 40);
        const bool on_settings = browser_.route().page == Page::settings;
        if (ui.icon_button(ui.id("settings"), ui::center(settings, 32, 32), "settings", 18, on_settings, lyren::tr("Settings"))) {
            if (on_settings) {
                shell_.back();
            } else {
                browser_.open(Page::settings);
            }
        }
        const Rect account = ui::cut_right(r, 40);
        if (const auto &mine = shelf().account) {
            // Signed in: the avatar opens the listener's own page; from there, or with a right click, the accounts.
            const Rect face = ui::center(account, 28, 28);
            const ui::Id id = ui.id("account");
            const auto it = ui.interact(id, face.inset(-2));
            const bool here = browser_.route().page == Page::profile && library_.own(browser_.route().card);
            const float ring = ui.anim(id, here ? 1 : it.hovered ? 0.5f : 0, pace::quick);
            if (ring > 0) {
                p.rect(face.inset(-3), {0, 0, 0, 0}, 17, (here ? t.accent : t.text).with_alpha(ring * (here ? 0.9f : 0.5f)), 1.5f);
            }
            cover(ui, face, mine->avatar, 14, "user");
            if (it.hovered && !it.held) {
                ui.tooltip(id, face, here ? lyren::tr("Accounts") : lyren::tr("My profile"));
            }
            if (it.right_clicked || (it.clicked && here)) {
                shell_.account.open(ui);
            } else if (it.clicked) {
                browser_.open_own_profile();
            }
        } else if (ui.icon_button(ui.id("account"), ui::center(account, 32, 32), "user", 18, false, lyren::tr("Accounts"))) {
            shell_.account.open(ui);
        }
        const float w = std::min(420.f, r.w - 32);
        if (w > 120) {
            const float x = std::clamp(center_x - w / 2, r.x + 16, r.right() - 16 - w);
            if (ui.text_box(ui.id("search"), {x, bar.y + (bar.h - control::medium) / 2, w, control::medium}, browser_.query, lyren::tr("Search music"), "search")) {
                browser_.search();
            }
        }
    }

    void Chrome::sidebar(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        struct Nav {
            Page page;
            std::string_view icon, label;
        };
        const auto entries = std::to_array<Nav>({{Page::recommend, "compass", lyren::tr("For you")},
                                                 {Page::toplist, "list-ordered", lyren::tr("Charts")},
                                                 {Page::square, "layout-grid", lyren::tr("Browse")},
                                                 {Page::fm, "radio", lyren::tr(provider().radio_title)},
                                                 {Page::profile, "user", lyren::tr("My profile")},
                                                 {Page::liked, "heart", lyren::tr("Liked songs")},
                                                 {Page::recent, "history", lyren::tr("Recently played")},
                                                 {Page::cloud, "cloud", lyren::tr("My cloud")},
                                                 {Page::collection, "star", lyren::tr("My collection")},
                                                 {Page::dislikes, "thumbs-down", lyren::tr("Disliked")}});
        constexpr size_t discover = 4; // the entries before "My music"
        std::inplace_vector<Nav, entries.size()> nav;
        size_t mine = 0;
        for (size_t k = 0; k < entries.size(); ++k) {
            if (browser_.supports(entries[k].page, browser_.active())) {
                nav.push_back(entries[k]);
                mine += k < discover;
            }
        }
        const size_t lists = nav.size();
        const auto &playlists = shelf().playlists;
        const auto &route = browser_.route();
        constexpr float item_h = 36, step = 38, head_h = 38, rail = 72;
        // Labels fade as the sidebar folds into the rail.
        const float labels = std::clamp((r.w - rail) / 148, 0.f, 1.f);
        auto item_y = [&](size_t k) {
            return 6 + head_h * static_cast<float>(1 + (k >= mine) + (k >= lists)) + step * static_cast<float>(k);
        };

        int selected = -1;
        for (size_t k = 0; k < nav.size(); ++k) {
            // Today's mix, the radar and the new releases are reached from "For you".
            const bool under_first = k == 0 && (route.page == Page::daily || route.page == Page::radar || route.page == Page::releases);
            const bool here = nav[k].page == route.page && (route.page != Page::profile || library_.own(route.card));
            if (here || under_first) {
                selected = static_cast<int>(k);
            }
        }
        if (route.page == Page::playlist) {
            for (size_t i = 0; i < playlists.size(); ++i) {
                if (same(playlists[i], route.card)) {
                    selected = static_cast<int>(lists + i);
                }
            }
        }

        const float content = item_y(lists + playlists.size()) + 8 + (playlists.empty() ? 40 : 0);
        const float off = ui.begin_scroll(ui::sub_id(ui.id("sidebar"), std::to_underlying(browser_.active())), r, content);
        const Rect row0{r.x + 12, 0, r.w - 24, item_h};

        // One highlight glides to the selected entry, an accent mark at its edge stretching ahead of it.
        const float to = selected >= 0 ? item_y(static_cast<size_t>(selected)) : 0;
        const float lit = ui.anim(ui.id("side-lit"), selected >= 0 ? 1 : 0, pace::steady);
        if (lit > 0) {
            const float y = ui.spring(ui.id("side-sel"), to, 260);
            p.rect({row0.x, r.y + y - off, row0.w, item_h}, t.fill(1.5f * lit), t.radius_s());
            const ui::Id top_id = ui.id("side-mark-top"), bottom_id = ui.id("side-mark-bottom");
            const bool down = to + 10 > ui.spring_value(top_id, to + 10);
            const float top = ui.spring(top_id, to + 10, down ? 380 : 200), bottom = ui.spring(bottom_id, to + item_h - 10, down ? 200 : 380);
            p.rect({row0.x + 3, r.y + top - off, 3, bottom - top}, t.accent.with_alpha(lit), 1.5f);
        }

        auto heading = [&](size_t before, std::string_view text) {
            const float y = r.y + item_y(before) - head_h - off;
            if (labels > 0.01f) {
                ui.label({r.x + 22, y + 14, r.w - 76, 20}, text, t.style(type::micro, t.faint().with_alpha(labels), text::semibold));
            }
            if (labels < 0.99f && before > 0) {
                p.rect({r.x + 24, y + head_h / 2, r.w - 48, 1}, t.line(1 - labels));
            }
        };
        auto item = [&](ui::Id id, size_t k, std::string_view label, std::string_view icon, const std::string *thumb) {
            Rect row = row0;
            row.y = r.y + item_y(k) - off;
            const Rect clip = ui.clip();
            if (row.bottom() < clip.y || row.y > clip.bottom()) {
                return false;
            }
            const auto it = ui.interact(id, row);
            const auto [hot, down] = ui.feel(id, it, pace::quick);
            const bool on = selected == static_cast<int>(k);
            if (hot > 0 && !on) {
                p.rect(row.inset(down), t.fill(hot), t.radius_s());
            }
            const float ix = row.x + 12 + ((row.w - 24) / 2 - 9) * (1 - labels);
            const Rect glyph{ix, row.y + (row.h - 18) / 2, 18, 18};
            if (thumb) {
                cover(ui, glyph.inset(-2), *thumb, 5, "");
            } else {
                p.icon(icon, glyph, on ? t.accent : t.text_dim.mix(t.text, hot * 0.7f));
            }
            if (labels > 0.01f) {
                const ui::Color color = on ? t.text : t.text_dim.mix(t.text, 0.45f + 0.55f * hot);
                ui.label({row.x + 42, row.y, row.w - 50, row.h}, label, t.style(type::body, color.with_alpha(labels), on ? text::medium : 0));
            } else if (it.hovered && !it.held) {
                ui.tooltip(id, row, label);
            }
            return it.clicked;
        };

        heading(0, lyren::tr("Discover"));
        heading(mine, lyren::tr("My music"));
        heading(lists, lyren::tr("Playlists"));
        if (signed_in() && labels > 0.5f && provider().capabilities.playlist_edit) {
            Fade fade(p, labels);
            const float y = r.y + item_y(lists) - head_h - off;
            ui::Enabled idle(ui, !library_.writing(browser_.active(), "") && shelf().account.has_value());
            if (ui.icon_button(ui.id("new-playlist"), {r.right() - 44, y + 9, 26, 26}, "plus", 15, false, lyren::tr("New playlist"))) {
                shell_.dialogs.edit_playlist(ui);
            }
        }
        for (size_t k = 0; k < nav.size(); ++k) {
            if (item(ui.id(nav[k].icon), k, nav[k].label, nav[k].icon, nullptr)) {
                if (nav[k].page != Page::profile) {
                    browser_.open(nav[k].page);
                } else if (!browser_.open_own_profile()) {
                    shell_.account.open(ui);
                }
            }
        }
        ui.push_id(ui.id("playlists"));
        for (size_t i = 0; i < playlists.size(); ++i) {
            if (item(item_id(ui, playlists[i]), lists + i, playlists[i].title, {}, &playlists[i].cover)) {
                browser_.open(playlists[i]);
            }
        }
        ui.pop_id();
        if (playlists.empty() && labels > 0.01f) {
            ui.label({r.x + 22, r.y + item_y(lists) - off, r.w - 44, 20}, signed_in() ? lyren::tr("No playlists yet") : lyren::tr("Sign in to see your playlists"),
                     t.style(type::caption, t.faint().with_alpha(labels)));
        }
        ui.end_scroll();
    }

} // namespace lyren
