#include "app/view/chrome.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <inplace_vector>
#include <optional>

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
        const Rect nav = ui::cut_left(r, 64);
        {
            ui::Enabled possible(ui, browser_.can_go_back() || shell_.stage.open(), 0.28f);
            if (ui.icon_button(ui.id("back"), {nav.x + 2, bar.y + (bar.h - 28) / 2, 28, 28}, "chevron-left", 18, false, lyren::tr("Back"))) {
                shell_.back();
            }
        }
        {
            ui::Enabled possible(ui, browser_.can_go_forward() && !shell_.stage.open(), 0.28f);
            if (ui.icon_button(ui.id("forward"), {nav.x + 32, bar.y + (bar.h - 28) / 2, 28, 28}, "chevron-right", 18, false, lyren::tr("Forward"))) {
                shell_.forward();
            }
        }

        // A fixed-width platform capsule keeps the search field still when the name changes.
        const auto name_style = t.style(type::label, t.text, text::medium);
        const auto &entries = env_.providers.entries();
        float platform_w = 0;
        bool any_accounts = false;
        for (const auto &entry : entries) {
            platform_w = std::max(platform_w, p.measure(lyren::tr(entry.short_name), name_style));
            any_accounts |= entry.provider->capabilities.accounts;
        }
        platform_w = std::min(136.f, std::ceil(platform_w));
        const auto *active_entry = env_.providers.entry(browser_.active(), browser_.active_key().instance);
        const std::string_view active_short = active_entry ? lyren::tr(active_entry->short_name) : lyren::tr(platform(browser_.active()).short_name);
        const Rect selector = ui::center(ui::cut_left(r, platform_w + 52), platform_w + 52, 30);
        const ui::Id platform_id = ui.id("platform");
        const auto picked = ui.interact(platform_id, selector);
        if (picked.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = ui.feel(platform_id, picked);
        const Rect pill = selector.inset(down);
        p.rect(pill, t.fill(1 + 0.85f * hot), t.round(pill.h));
        p.rect({pill.x + 13, pill.y + pill.h / 2 - 3, 6, 6}, signed_in() ? t.accent : t.text.with_alpha(0.26f), 3);
        ui.label({pill.x + 26, pill.y, platform_w + 1, pill.h}, active_short, name_style);
        p.icon("chevron-down", {pill.right() - 20, pill.y + (pill.h - 10) / 2, 10, 10}, t.text_dim.mix(t.text, hot));
        if (picked.clicked) {
            std::vector<ui::MenuItem> items;
            for (const auto &entry : entries) {
                const bool current = entry.source == browser_.active() && entry.instance == browser_.active_key().instance;
                items.push_back({current ? "check" : "music", std::string(lyren::tr(entry.name)), [this, source = entry.source, inst = entry.instance] {
                                     browser_.switch_platform(source, inst);
                                 }});
            }
            if (any_accounts) {
                items.push_back({});
                items.push_back({"user", std::string(lyren::tr("Accounts")), [this, &ui] {
                                     shell_.account.open(ui);
                                 }});
            }
            ui.open_menu(selector.x, selector.bottom() + 6, std::move(items));
        }
        ui::cut_left(r, 8);

        const Rect settings = ui::cut_right(r, 38);
        const bool on_settings = browser_.route().page == Page::settings;
        if (ui.icon_button(ui.id("settings"), ui::center(settings, 30, 30), "settings", 17, on_settings, lyren::tr("Settings"))) {
            if (on_settings) {
                shell_.back();
            } else {
                browser_.open(Page::settings);
            }
        }
        if (any_accounts) {
            const Rect account = ui::cut_right(r, 38);
            if (const auto &mine = shelf().account; mine && env_.has_account(browser_.active(), browser_.active_key().instance)) {
                // Signed in: the avatar opens the listener's own page; from there, or with a right click, the accounts.
                const Rect face = ui::center(account, 26, 26);
                const ui::Id id = ui.id("account");
                const auto it = ui.interact(id, face.inset(-2));
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                const bool here = browser_.route().page == Page::profile && library_.own(browser_.route().card);
                const float ring = ui.anim(id, here ? 1 : it.hovered ? 0.5f : 0, pace::quick);
                if (ring > 0) {
                    p.rect(face.inset(-3), {0, 0, 0, 0}, 16, (here ? t.accent : t.text).with_alpha(ring * (here ? 0.9f : 0.5f)), 1.5f);
                }
                cover(ui, face, mine->avatar, 13, "user");
                if (it.hovered && !it.held) {
                    ui.tooltip(id, face, here ? lyren::tr("Accounts") : lyren::tr("My profile"));
                }
                if (it.right_clicked || (it.clicked && here)) {
                    shell_.account.open(ui);
                } else if (it.clicked) {
                    browser_.open_own_profile();
                }
            } else if (ui.icon_button(ui.id("account"), ui::center(account, 30, 30), "user", 17, false, lyren::tr("Accounts"))) {
                shell_.account.open(ui);
            }
        }
        const float w = std::min(400.f, r.w - 24);
        if (w > 120) {
            const float x = std::clamp(center_x - w / 2, r.x + 12, r.right() - 12 - w);
            const ui::Id search_id = ui.id("search");
            const Rect search_box{x, bar.y + (bar.h - control::medium) / 2, w, control::medium};
            const bool search_active = ui.focus == search_id && !ui.modal_open() && !ui.menu_open();
            if (search_active) {
                browser_.update_suggestions(browser_.query);
                if (browser_.query != last_search_query_) {
                    last_search_query_ = browser_.query;
                    search_sel_ = -1;
                }
                const auto &suggestions = browser_.suggestions();
                const int count = static_cast<int>(suggestions.size());
                if (search_sel_ >= count) {
                    search_sel_ = -1;
                }
                if (count > 0) {
                    auto &keys = ui.input().keys;
                    for (auto k = keys.begin(); k != keys.end();) {
                        if (k->keysym == XKB_KEY_Down || k->keysym == XKB_KEY_KP_Down) {
                            search_sel_ = search_sel_ + 1 >= count ? -1 : search_sel_ + 1;
                            k = keys.erase(k);
                            ui.redraw();
                        } else if (k->keysym == XKB_KEY_Up || k->keysym == XKB_KEY_KP_Up) {
                            search_sel_ = search_sel_ <= -1 ? count - 1 : search_sel_ - 1;
                            k = keys.erase(k);
                            ui.redraw();
                        } else {
                            ++k;
                        }
                    }
                }
            }
            if (ui.text_box(search_id, search_box, browser_.query, lyren::tr("Search music"), "search", "/")) {
                const auto &suggestions = browser_.suggestions();
                if (search_sel_ >= 0 && search_sel_ < static_cast<int>(suggestions.size())) {
                    browser_.query = suggestions[static_cast<size_t>(search_sel_)].text;
                }
                browser_.search();
                ui.focus = 0;
                search_sel_ = -1;
            }
            if (ui.focus == search_id && !ui.modal_open() && !ui.menu_open()) {
                browser_.update_suggestions(browser_.query);
                if (browser_.query != last_search_query_) {
                    last_search_query_ = browser_.query;
                    search_sel_ = -1;
                }
                const auto &suggestions = browser_.suggestions();
                const int count = static_cast<int>(suggestions.size());
                if (search_sel_ >= count) {
                    search_sel_ = -1;
                }
                if (count > 0) {
                    if (!search_was_open_) {
                        search_pop_since_ = ui.now;
                    }
                    search_was_open_ = true;
                    const bool history_header = trim(browser_.query).empty();
                    constexpr float pad = 6.f, row_h = 34.f, head_h = 28.f;
                    const float pop_h = pad * 2 + (history_header ? head_h : 0.f) + static_cast<float>(count) * row_h;
                    const Rect pop_r{search_box.x, search_box.bottom() + 6, search_box.w, std::min(pop_h, std::max(row_h + pad * 2, ui.height - search_box.bottom() - 16))};
                    ui.set_popover(pop_r, [this, search_id, history_header, since = search_pop_since_](ui::Ui &u, Rect box) {
                        auto &pp = u.painter();
                        const auto &th = u.theme();
                        const float in = u.progress(since, pace::steady);
                        Fade fade(pp, in);
                        box.y -= (1 - in) * 4;
                        pp.shadow({box.x, box.y + 6, box.w, box.h}, th.radius + 2, 24, th.shadow.with_alpha(th.shadow.a * 0.78f));
                        pp.rect(box, th.elevated, th.radius + 2, th.surface_edge(1.3f), 1);

                        u.push_clip(box);
                        constexpr float kpad = 6.f, krow_h = 34.f, khead_h = 28.f;
                        float y = box.y + kpad;
                        bool clear_all = false;
                        std::optional<std::string> remove_term, pick_term;

                        if (history_header) {
                            const Rect hdr{box.x + kpad, y, box.w - kpad * 2, khead_h};
                            const auto clear_lbl = lyren::tr("Clear");
                            const float cw = u.button_width(clear_lbl);
                            const Rect clear_r{hdr.right() - cw, hdr.y + (hdr.h - 22) / 2, cw, 22};
                            const ui::Id clear_id = u.id("search-clear-history");
                            if (u.button(clear_id, clear_r, clear_lbl, ui::ButtonKind::ghost)) {
                                clear_all = true;
                            }
                            if (u.is_active(clear_id)) {
                                u.keep_focus(search_id);
                            }
                            u.label({hdr.x + 8, hdr.y, std::max(0.f, clear_r.x - hdr.x - 12), hdr.h}, lyren::tr("Recent searches"),
                                    th.style(type::micro, th.faint(), text::semibold));
                            y += khead_h;
                        }

                        const auto &items = browser_.suggestions();
                        u.push_id(u.id("search-suggest"));
                        for (size_t i = 0; i < items.size(); ++i) {
                            const Rect row{box.x + kpad, y, box.w - kpad * 2, krow_h};
                            if (row.bottom() > box.bottom() - kpad + 1) {
                                break;
                            }
                            const auto &entry = items[i];
                            const ui::Id rid = u.id(static_cast<uint64_t>(i));
                            const bool kb_sel = static_cast<int>(i) == search_sel_;
                            const bool hovering_row = u.hovering(row);
                            const float hot = u.anim(rid, hovering_row ? 1.f : 0.f, pace::quick);
                            if (kb_sel || hot > 0) {
                                pp.rect(row, th.fill(kb_sel ? 1.35f : hot), th.radius_s());
                            }
                            float right_pad = 10.f;
                            if (entry.from_history) {
                                right_pad = 34.f;
                                if (hovering_row || kb_sel) {
                                    const Rect del_r{row.right() - 28, row.y + (row.h - 24) / 2, 24, 24};
                                    const ui::Id del_id = ui::sub_id(rid, 0xde);
                                    const auto dit = u.interact(del_id, del_r);
                                    if (dit.hovered) {
                                        u.cursor = wl::Cursor::pointer;
                                    }
                                    if (dit.pressed) {
                                        u.keep_focus(search_id);
                                    }
                                    const float dhot = u.anim(del_id, dit.hovered ? 1.f : 0.f, pace::quick);
                                    if (dhot > 0) {
                                        pp.rect(del_r, th.fill(1.2f * dhot), th.radius_s());
                                    }
                                    pp.icon("x", ui::center(del_r, 12, 12), th.text_dim.mix(th.text, dhot));
                                    if (dit.clicked) {
                                        remove_term = entry.text;
                                    }
                                }
                            }
                            const auto it = u.interact(rid, row);
                            if (it.hovered) {
                                u.cursor = wl::Cursor::pointer;
                            }
                            if (it.pressed) {
                                u.keep_focus(search_id);
                            }
                            if (it.clicked) {
                                pick_term = entry.text;
                            }
                            pp.icon(entry.from_history ? "history" : "search", {row.x + 10, row.y + (row.h - 14) / 2, 14, 14},
                                    kb_sel ? th.accent : th.text_dim.mix(th.text, hot * 0.5f));
                            u.label({row.x + 32, row.y, std::max(0.f, row.w - 32 - right_pad), row.h}, entry.text,
                                    th.style(type::label, kb_sel ? th.text : th.text.with_alpha(0.92f), kb_sel ? text::medium : 0));
                            y += krow_h;
                        }
                        u.pop_id();
                        u.pop_clip();

                        // Keep focus alive if the user clicks empty chrome inside the popover (e.g. its header).
                        if (const auto bg = u.interact(u.id("search-pop-bg"), box); bg.pressed) {
                            u.keep_focus(search_id);
                        }
                        if (clear_all) {
                            browser_.clear_search_history();
                            search_sel_ = -1;
                            u.keep_focus(search_id);
                        } else if (remove_term) {
                            browser_.remove_search_history(*remove_term);
                            search_sel_ = -1;
                            u.keep_focus(search_id);
                        } else if (pick_term) {
                            browser_.query = std::move(*pick_term);
                            browser_.search();
                            u.focus = 0;
                            search_sel_ = -1;
                        }
                    });
                } else {
                    search_was_open_ = false;
                }
            } else {
                search_was_open_ = false;
                search_sel_ = -1;
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
                                                 {Page::stats, "bar-chart", lyren::tr("Listening stats")},
                                                 {Page::pinned, "pin", lyren::tr("Pinned offline")},
                                                 {Page::cloud, "cloud", lyren::tr("My cloud")},
                                                 {Page::collection, "star", lyren::tr("My collection")},
                                                 {Page::dislikes, "thumbs-down", lyren::tr("Disliked")}});
        constexpr size_t discover = 4; // the entries before "My music"
        std::inplace_vector<Nav, entries.size()> nav;
        size_t mine = 0;
        for (size_t k = 0; k < entries.size(); ++k) {
            if (browser_.supports(entries[k].page, browser_.active(), browser_.active_key().instance)) {
                nav.push_back(entries[k]);
                mine += k < discover;
            }
        }
        const size_t lists = nav.size();
        const auto &playlists = shelf().playlists;
        const auto &route = browser_.route();
        constexpr float item_h = 35, step = 37, head_h = 36, rail = 64;
        // Labels fade as the sidebar folds into the rail.
        const float labels = std::clamp((r.w - rail) / 156, 0.f, 1.f);
        auto item_y = [&](size_t k) {
            return 4 + head_h * static_cast<float>(1 + (k >= mine) + (k >= lists)) + step * static_cast<float>(k);
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

        const float content = item_y(lists + playlists.size()) + 8 + (playlists.empty() ? 36 : 0);
        const float off = ui.begin_scroll(ui::sub_id(ui.id("sidebar"), std::hash<std::string_view>{}(browser_.active_id())), r, content);
        const Rect row0{r.x + 8, 0, r.w - 16, item_h};

        // One highlight glides to the selected entry.
        const float to = selected >= 0 ? item_y(static_cast<size_t>(selected)) : 0;
        const float lit = ui.anim(ui.id("side-lit"), selected >= 0 ? 1 : 0, pace::steady);
        if (lit > 0) {
            const float y = ui.spring(ui.id("side-sel"), to, 240);
            p.rect({row0.x, r.y + y - off, row0.w, item_h}, t.fill(1.45f * lit), t.radius_s());
        }

        auto heading = [&](size_t before, std::string_view text) {
            const float y = r.y + item_y(before) - head_h - off;
            if (labels > 0.01f) {
                ui.label({r.x + 18, y + 13, r.w - 64, 20}, text, t.style(type::micro, t.faint().with_alpha(labels), text::semibold));
            }
            if (labels < 0.99f && before > 0) {
                p.rect({r.x + 20, y + head_h / 2, r.w - 40, 1}, t.line(1 - labels));
            }
        };
        auto item = [&](ui::Id id, size_t k, std::string_view label, std::string_view icon, const Card *playlist) {
            Rect row = row0;
            row.y = r.y + item_y(k) - off;
            const Rect clip = ui.clip();
            if (row.bottom() < clip.y || row.y > clip.bottom()) {
                return false;
            }
            const auto it = ui.interact(id, row);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const auto [hot, down] = ui.feel(id, it, pace::quick);
            const bool on = selected == static_cast<int>(k);
            if (hot > 0 && !on) {
                p.rect(row.inset(down), t.fill(hot), t.radius_s());
            }
            const float ix = row.x + 11 + ((row.w - 22) / 2 - 8.5f) * (1 - labels);
            const Rect glyph{ix, row.y + (row.h - 17) / 2, 17, 17};
            if (playlist) {
                const Rect art_r = glyph.inset(-3.5f);
                cover(ui, art_r, playlist->cover, 5, "list-music");
            } else {
                p.icon(icon, glyph, on ? t.accent : t.text_dim.mix(t.text, hot * 0.7f));
            }
            if (labels > 0.01f) {
                const ui::Color color = on ? t.text : t.text_dim.mix(t.text, 0.45f + 0.55f * hot);
                ui.label({row.x + 39, row.y, row.w - 47, row.h}, label, t.style(type::body, color.with_alpha(labels), on ? text::medium : 0));
            } else if (it.hovered && !it.held) {
                ui.tooltip(id, row, label);
            }
            if (playlist && it.right_clicked) {
                std::vector<ui::MenuItem> menu;
                const Card c = *playlist;
                if (auto link = share_url(c); !link.empty()) {
                    menu.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                        ui.copy = link;
                                        ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                    }});
                }
                if (c.owned && provider(c).capabilities.playlist_edit) {
                    menu.push_back({"pencil", std::string(lyren::tr("Rename")), [this, &ui, c] {
                                        shell_.dialogs.edit_playlist(ui, c);
                                    }});
                    menu.push_back({"trash", std::string(lyren::tr("Delete playlist")),
                                    [this, &ui, c] {
                                        shell_.dialogs.confirm_delete(ui, c);
                                    },
                                    true});
                } else if (!c.owned && provider(c).capabilities.collect_playlists) {
                    menu.push_back({"trash", std::string(lyren::tr("Remove")),
                                    [this, c] {
                                        library_.toggle_collect_playlist(c);
                                    },
                                    true});
                }
                if (!menu.empty()) {
                    ui.open_menu(it.px, it.py, std::move(menu));
                }
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
            if (ui.icon_button(ui.id("new-playlist"), {r.right() - 38, y + 8, 26, 26}, "plus", 15, false, lyren::tr("New playlist"))) {
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
            if (item(item_id(ui, playlists[i]), lists + i, playlists[i].title, {}, &playlists[i])) {
                browser_.open(playlists[i]);
            }
        }
        ui.pop_id();
        if (playlists.empty() && labels > 0.01f) {
            ui.label({r.x + 18, r.y + item_y(lists) - off, r.w - 36, 20}, signed_in() ? lyren::tr("No playlists yet") : lyren::tr("Sign in to see your playlists"),
                     t.style(type::caption, t.faint().with_alpha(labels)));
        }
        ui.end_scroll();
    }

} // namespace lyren
