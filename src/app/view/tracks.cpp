#include "app/view/tracks.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <bit>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    bool Tracks::list(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Track> &tracks, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &picked = browser_.selection();
        const auto &route = browser_.route();
        if (picked.list != &tracks) {
            browser_.reset_selection();
            picked.list = &tracks;
        }
        // The page's own playlist: the listener's to take songs out of and to put in order.
        const bool editable = route.page == Page::playlist && route.list && &tracks == &route.list->tracks;
        const bool ordering = editable && route.list->ordering;
        constexpr float row_h = 56;
        const Rect head = ui::cut_top(r, 34);
        const TrackColumns c = track_columns(head);
        const auto caption = t.style(type::caption, t.faint());
        if (picked.choosing) {
            const bool all = picked.chosen.size() == tracks.size() && !tracks.empty();
            if (ui.icon_button(ui.id("select-all"), ui::center(c.index, 30, 30),
                               all                     ? "square-check"
                               : picked.chosen.empty() ? "square"
                                                       : "square-minus",
                               18, !picked.chosen.empty())) {
                picked.chosen.clear();
                if (!all) {
                    for (const auto &track : tracks) {
                        picked.chosen.insert(track.id);
                    }
                }
            }
        } else {
            ui.label(c.index, "#", caption, Align::center);
        }
        ui.label(c.title, lyren::tr("Title"), caption);
        if (c.album.w > 0) {
            ui.label(c.album, lyren::tr("Album"), caption);
        }
        if (provider().capabilities.playlist_edit && !ordering && !tracks.empty()) {
            const auto label = lyren::tr(picked.choosing ? "Done selecting" : "Select songs");
            const float w = ui.button_width(label);
            if (ui.button(ui.id("select-songs"), {head.right() - w - 12, head.y + (head.h - control::small) / 2, w, control::small}, label, ui::ButtonKind::ghost)) {
                if (picked.choosing) {
                    browser_.reset_selection();
                    picked.list = &tracks;
                } else if (picked.selected >= 0 && picked.selected < static_cast<int>(tracks.size())) {
                    browser_.choose(tracks, picked.selected, false);
                } else {
                    picked.choosing = true;
                }
            }
        } else {
            ui.label(c.duration, lyren::tr(ordering ? "Order" : "Time"), caption, Align::right);
        }
        p.rect({head.x + 12, head.bottom() - 1, head.w - 24, 1}, t.line());
        ui::cut_top(r, 4);

        // What to do with the picked songs comes in above the rows.
        if (const float bar_h = ui.spring(ui::sub_id(id, 0xba), picked.choosing ? 44.f : 0.f, pace::glide); bar_h > 0.5f) {
            const Rect bar = ui::cut_top(r, bar_h).inset(12, 0);
            Fade fade(p, std::clamp(bar_h / 44, 0.f, 1.f));
            ui::Enabled live(ui, picked.choosing, 1);
            ui.push_clip({bar.x, bar.y, bar.w, bar_h});
            float right = bar.right();
            if (editable && route.card.owned) {
                const bool busy = library_.writing(route.card.source, route.card.id);
                const auto label = lyren::tr(busy ? "Saving..." : "Remove");
                const float w = ui.button_width(label, true);
                ui::Enabled possible(ui, !picked.chosen.empty() && !busy);
                if (ui.button(ui.id("remove-selected"), {right - w, bar.y + 5, w, control::medium}, label, ui::ButtonKind::secondary, "list-minus")) {
                    browser_.remove_from_playlist(route.card, picked.of(tracks));
                }
                right -= w + 8;
            }
            const auto label = lyren::tr("Add to playlist");
            const float w = ui.button_width(label, true);
            {
                ui::Enabled possible(ui, !picked.chosen.empty());
                if (ui.button(ui.id("add-selected"), {right - w, bar.y + 5, w, control::medium}, label, ui::ButtonKind::secondary, "folder-plus")) {
                    shell_.dialogs.pick_playlist(ui, picked.of(tracks));
                }
            }
            ui.label({bar.x + 4, bar.y, right - w - bar.x - 16, 44}, lyren::trf("{} selected", picked.chosen.size()), t.style(type::label, t.text_dim));
            ui.pop_clip();
        }

        const float off = ui.begin_scroll(id, r, static_cast<float>(tracks.size()) * row_h + 8);
        // One row past the bottom keeps its place, so it slides in with the rest when a row above it goes.
        const int count = static_cast<int>(tracks.size());
        const int first = static_cast<int>(off / row_h), last = static_cast<int>((off + r.h) / row_h) + 2;
        for (int i = std::max(0, first - 8); i < std::min(last + 16, count); ++i) {
            if (i < first || i > last) {
                images_.prefetch(tracks[static_cast<size_t>(i)].cover, static_cast<uint32_t>(Images::tier(40 * p.scale())));
            }
        }
        // Rows glide to a new place when the list changes under them; a list that arrives anew starts in place.
        std::optional<Track> toggled_like;
        ui.push_id(ui::sub_id(id, std::bit_cast<uint64_t>(since)));
        for (int i = first; i <= last && i < count && i < static_cast<int>(tracks.size()); ++i) {
            const float in = ui.cascade(since, i - first);
            Fade fade(p, in);
            const float y = ui.spring(ui::sub_id(item_id(ui, tracks[static_cast<size_t>(i)]), 0x70), static_cast<float>(i) * row_h, pace::glide);
            row(ui, {r.x, r.y + y - off + (1 - in) * 12, r.w, row_h}, tracks, i, toggled_like);
        }
        ui.pop_id();
        if (off > 0.5f && t.surface.a > 0.9f) {
            p.gradient({r.x, r.y, r.w, 12}, t.surface.with_alpha(0.9f), t.surface.with_alpha(0));
        }
        ui.end_scroll();
        if (toggled_like) {
            library_.toggle_like(std::move(*toggled_like));
        }
        return last + 10 >= count;
    }

    void Tracks::row(ui::Ui &ui, Rect r, const std::vector<Track> &tracks, int i, std::optional<Track> &toggled_like) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &picked = browser_.selection();
        const auto &route = browser_.route();
        const Track &track = tracks[static_cast<size_t>(i)];
        ui.push_id(item_id(ui, track));
        const ui::Id row_id = ui.id("row");
        const bool pointed = ui.hovering(r) || ui.is_active(row_id), on = playback_.is_current(track);
        const bool chosen = picked.chosen.contains(track.id), selected = picked.choosing ? chosen : picked.selected == i;
        const float hot = ui.anim(row_id, pointed ? 1 : 0, pace::quick), lit = ui.anim(ui::sub_id(row_id, 0x5e), selected ? 1 : 0, pace::steady);
        if (hot > 0 || lit > 0 || (on && !picked.choosing)) {
            const Rect bg = r.inset(4, 2);
            if (picked.choosing) {
                p.rect(bg, t.accent.with_alpha(0.05f * hot + 0.09f * lit), t.radius_s());
            } else if (on) {
                p.rect(bg, t.accent.with_alpha(0.065f + 0.045f * hot), t.radius_s());
            } else {
                p.rect(bg, t.fill(hot + 0.9f * lit), t.radius_s());
            }
        }

        const TrackColumns c = track_columns(r);
        const auto number = t.style(type::label, t.faint(), text::tabular_nums);
        // Ctrl and Shift pick rows where playlists can be edited, as the checkboxes do.
        constexpr uint32_t pick_mods = wl::mod_ctrl | wl::mod_shift;
        const bool picking = picked.choosing || (provider(track).capabilities.playlist_edit &&
                                                 ((ui.input().mods & pick_mods) || std::ranges::any_of(ui.input().actions, [&](const ui::PointerAction &action) {
                                                      return action.press && action.button == BTN_LEFT && r.contains(action.x, action.y) && (action.mods & pick_mods);
                                                  })));
        if (picking) {
            p.icon(chosen ? "square-check" : "square", ui::center(c.index, 18, 18), chosen ? t.accent : t.text_dim);
        } else if (pointed) {
            if (ui.icon_button(ui.id("play"), ui::center(c.index, 30, 30), on && playback_.playing() ? "pause-fill" : "play-fill", 14)) {
                if (on) {
                    playback_.toggle();
                } else {
                    playback_.play(tracks, i);
                }
            }
        } else if (on) {
            ui.equalizer(ui.id("playing"), ui::center(c.index, 14, 14), t.accent, playback_.playing());
        } else {
            ui.label(c.index, int_text(i + 1), number, Align::center);
        }
        const Rect art{c.title.x, r.y + (r.h - 40) / 2, 40, 40};
        cover(ui, art, track.cover, t.radius_s());
        const float tx = art.right() + 14, tw = c.title.right() - tx - 8;
        const auto title = t.style(type::body, on ? t.accent : t.text, on ? text::medium : 0);
        const float w = p.text(tx, r.y + 25, track.title, title, tw);
        if (!track.alias.empty() && w + 24 < tw) {
            const auto alias_style = t.style(type::body, t.faint());
            const float rem = tw - w - 6;
            const float open_w = p.measure("（", alias_style);
            const float alias_w = p.measure(track.alias, alias_style);
            const float close_w = p.measure("）", alias_style);
            float ax = tx + w + 6;
            ax += p.text(ax, r.y + 25, "（", alias_style, rem);
            if (open_w + alias_w + close_w <= rem) {
                ax += p.text(ax, r.y + 25, track.alias, alias_style);
                p.text(ax, r.y + 25, "）", alias_style);
            } else if (rem > open_w) {
                p.text(ax, r.y + 25, track.alias, alias_style, rem - open_w);
            }
        }
        p.text(tx, r.y + 43, track.artist, t.style(type::caption, t.text_dim), tw);
        if (c.album.w > 0) {
            ui.label(c.album, track.album, t.style(type::label, t.text_dim));
        }
        const bool ordering = route.page == Page::playlist && route.list && &tracks == &route.list->tracks && route.list->ordering;
        if (ordering) {
            const auto &list = *route.list;
            const bool busy = list.saving_order || list.loading || !list.error.empty() || library_.writing(route.card.source, route.card.id);
            auto move = [&](std::string_view name, Rect cell, std::string_view icon, int to) {
                ui::Enabled possible(ui, !busy && to >= 0 && to < static_cast<int>(tracks.size()), 0.3f);
                if (!ui.icon_button(ui.id(name), ui::center(cell, 32, 32), icon, 18)) {
                    return false;
                }
                browser_.move_track(route.list, route.card, i, to);
                return true;
            };
            if (move("move-up", c.like, "chevron-up", i - 1) || move("move-down", c.duration, "chevron-down", i + 1)) {
                ui.pop_id();
                return;
            }
        } else {
            if (pointed || library_.liked(track)) {
                heart(ui, ui.id("like"), ui::center(c.like, 32, 32), track, 16, &toggled_like);
            }
            ui.label(c.duration, mmss(track.duration), number, Align::right);
        }

        // Disabled ordering buttons still own their cells; a repeated click must not reach the row's play action.
        Rect hit = r;
        if (ordering) {
            hit.w = c.like.x - hit.x;
        }
        const auto it = ui.interact(row_id, hit);
        if (it.pressed) {
            if (picking) {
                browser_.choose(tracks, i, it.mods & wl::mod_shift);
            } else {
                picked.selected = i;
            }
        }
        if (it.double_clicked && !picking) {
            playback_.play(tracks, i);
        }
        if (it.right_clicked) {
            if (picked.choosing && !chosen) {
                picked.chosen = {track.id};
                picked.anchor = i;
            }
            picked.selected = i;
            menu(ui, it.px, it.py, tracks, i);
        }
        ui.pop_id();
    }

    void Tracks::menu(ui::Ui &ui, float x, float y, const std::vector<Track> &tracks, int i) {
        const Track track = tracks[static_cast<size_t>(i)];
        const auto &picked = browser_.selection();
        const auto &route = browser_.route();
        const auto &caps = provider(track).capabilities;
        // One's own playlist can take songs out.
        const bool removable = route.page == Page::playlist && route.card.owned && caps.playlist_edit && route.list && &tracks == &route.list->tracks;
        std::vector<ui::MenuItem> items;
        auto add = [&](std::string_view icon, std::string_view label, std::move_only_function<void()> action, bool danger = false) {
            items.push_back({std::string(icon), std::string(lyren::tr(label)), std::move(action), danger});
        };
        if (picked.choosing && picked.list == &tracks && picked.chosen.size() > 1 && picked.chosen.contains(track.id)) {
            auto songs = std::make_shared<std::vector<Track>>(picked.of(tracks));
            add("play", "Play selected", [this, songs] {
                playback_.play(*songs, 0);
            });
            add("folder-plus", "Add to playlist", [this, &ui, songs] {
                shell_.dialogs.pick_playlist(ui, *songs);
            });
            if (removable) {
                add(
                    "list-minus", "Remove from playlist",
                    [this, playlist = route.card, songs] {
                        browser_.remove_from_playlist(playlist, *songs);
                    },
                    true);
            }
            return ui.open_menu(x, y, std::move(items));
        }
        add("play", "Play", [this, songs = tracks, i] {
            playback_.play(songs, i);
        });
        add("list-plus", "Play next", [this, &ui, track] {
            playback_.play_next(track);
            ui.toast(std::string(lyren::tr("Added to play next")), "list-plus");
        });
        if (!track.album_id.empty()) {
            add("disc-album", "View album", [this, track] {
                browser_.open(Page::album,
                              {.kind = CardKind::album, .source = track.source, .id = track.album_id, .title = track.album, .subtitle = track.artist, .cover = track.cover});
            });
        }
        if (!track.artist_id.empty()) {
            add("mic-vocal", "View artist", [this, track] {
                browser_.open(Page::artist, {.kind = CardKind::artist, .source = track.source, .id = track.artist_id, .title = track.artist.substr(0, track.artist.find(" / "))});
            });
        }
        if (caps.similar) {
            add("sparkles", "Similar songs", [this, track] {
                browser_.open(Page::similar, {.source = track.source, .id = track.id, .title = track.title, .subtitle = track.artist, .cover = track.cover});
            });
        }
        if (caps.comments[0]) {
            add("message-square", "Comments", [this, track] {
                shell_.open_comments(track.source, Subject::song, track.id, track.title + " · " + track.artist, track.cover);
            });
        }
        if (caps.post_write) {
            add("square-pen", "Share in a post", [this, &ui, track] {
                shell_.feed.compose(ui, track.source, track);
            });
        }
        items.push_back({});
        if (caps.likes) {
            const bool liked = library_.liked(track);
            add(liked ? "heart-off" : "heart", liked ? "Unlike" : "Like", [this, track] {
                library_.toggle_like(track);
            });
        }
        if (caps.playlist_edit) {
            add("folder-plus", "Add to playlist", [this, &ui, track] {
                shell_.dialogs.pick_playlist(ui, {track});
            });
        }
        add("copy", "Copy title and artist", [&ui, track] {
            ui.copy = track.title + " - " + track.artist;
            ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
        });
        if (auto link = share_url(track); !link.empty()) {
            add("link", "Copy link", [&ui, link = std::move(link)] {
                ui.copy = link;
                ui.toast(std::string(lyren::tr("Link copied")), "copy");
            });
        }
        if (route.page == Page::daily && route.list && &tracks == &route.list->tracks && provider(track).features.daily.dislike) {
            add("thumbs-down", "Not interested", [this, list = route.list, track] {
                browser_.dislike_daily(list, track);
            });
        }
        if (provider(track).features.exclusions.set) {
            add("thumbs-down", "Dislike this song", [this, track] {
                library_.set_dislike({.id = track.id, .name = track.title, .cover = track.cover, .source = track.source}, true);
            });
        }
        if (removable) {
            add(
                "list-minus", "Remove from playlist",
                [this, playlist = route.card, track] {
                    browser_.remove_from_playlist(playlist, {track});
                },
                true);
        }
        ui.open_menu(x, y, std::move(items));
    }

} // namespace lyren
