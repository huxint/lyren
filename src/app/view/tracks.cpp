#include "app/view/tracks.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>

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
        constexpr float track_row_h = 56;
        constexpr size_t auto_filter_threshold = 15;

        unsigned char ascii_lower(unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c;
        }

        int fold_compare(std::string_view a, std::string_view b) {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                const unsigned char ca = ascii_lower(static_cast<unsigned char>(a[i]));
                const unsigned char cb = ascii_lower(static_cast<unsigned char>(b[i]));
                if (ca != cb) {
                    return ca < cb ? -1 : 1;
                }
            }
            return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        }

        bool contains_ci(std::string_view haystack, std::string_view needle) {
            if (needle.empty()) {
                return true;
            }
            if (needle.size() > haystack.size()) {
                return false;
            }
            const auto it = std::ranges::search(haystack, needle, [](char a, char b) {
                return ascii_lower(static_cast<unsigned char>(a)) == ascii_lower(static_cast<unsigned char>(b));
            });
            return !it.empty();
        }

        bool matches_filter(const Track &track, std::string_view query) {
            size_t pos = 0;
            while (pos < query.size()) {
                while (pos < query.size() && (query[pos] == ' ' || query[pos] == '\t')) {
                    ++pos;
                }
                const size_t start = pos;
                while (pos < query.size() && query[pos] != ' ' && query[pos] != '\t') {
                    ++pos;
                }
                if (pos == start) {
                    break;
                }
                const std::string_view token = query.substr(start, pos - start);
                if (!contains_ci(track.title, token) && !contains_ci(track.alias, token) && !contains_ci(track.artist, token) && !contains_ci(track.album, token)) {
                    return false;
                }
            }
            return true;
        }

        uint64_t tracks_fingerprint(const std::vector<Track> &tracks) {
            uint64_t h = 0xcbf29ce484222325ull ^ tracks.size();
            auto mix_u64 = [&](uint64_t v) {
                h = (h ^ v) * 0x100000001b3ull;
            };
            for (const auto &t : tracks) {
                mix_u64(std::hash<std::string_view>{}(t.id));
                mix_u64(t.duration);
                mix_u64(static_cast<uint64_t>(t.source));
            }
            return h;
        }
    } // namespace

    bool Tracks::is_picked_list(const std::vector<Track> &tracks) const {
        const auto &picked = browser_.selection();
        return picked.list == &tracks || (&tracks == &view_tracks_ && picked.list == view_source_);
    }

    const std::vector<Track> &Tracks::displayed_tracks(const std::vector<Track> &tracks) {
        const std::string_view q = trim(filter_);
        if (q.empty() && sort_key_ == SortKey::none) {
            return tracks;
        }
        const uint64_t fp = tracks_fingerprint(tracks);
        if (cached_data_ == tracks.data() && cached_count_ == tracks.size() && cached_fingerprint_ == fp && cached_filter_ == q && cached_sort_ == sort_key_ &&
            cached_desc_ == sort_desc_) {
            return view_tracks_;
        }
        view_tracks_.clear();
        view_tracks_.reserve(tracks.size());
        for (const auto &track : tracks) {
            if (q.empty() || matches_filter(track, q)) {
                view_tracks_.push_back(track);
            }
        }
        if (sort_key_ != SortKey::none) {
            const SortKey key = sort_key_;
            const bool desc = sort_desc_;
            std::ranges::stable_sort(view_tracks_, [key, desc](const Track &a, const Track &b) {
                int cmp = 0;
                switch (key) {
                    case SortKey::title:
                        cmp = fold_compare(a.title, b.title);
                        break;
                    case SortKey::album:
                        if (a.album.empty() != b.album.empty()) {
                            return !a.album.empty();
                        }
                        cmp = fold_compare(a.album, b.album);
                        break;
                    case SortKey::duration:
                        cmp = a.duration < b.duration ? -1 : a.duration > b.duration ? 1 : 0;
                        break;
                    case SortKey::none:
                        break;
                }
                return desc ? cmp > 0 : cmp < 0;
            });
        }
        cached_data_ = tracks.data();
        cached_count_ = tracks.size();
        cached_fingerprint_ = fp;
        cached_filter_.assign(q);
        cached_sort_ = sort_key_;
        cached_desc_ = sort_desc_;
        return view_tracks_;
    }

    bool Tracks::has_selected() const {
        if (!has_list()) {
            return false;
        }
        const int sel = browser_.selection().selected;
        return sel >= 0 && sel < static_cast<int>(active_tracks_->size());
    }

    void Tracks::move_selection(ui::Ui &ui, int delta) {
        if (!has_list()) {
            return;
        }
        auto &picked = browser_.selection();
        const int count = static_cast<int>(active_tracks_->size());
        const int next = picked.selected < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(picked.selected + delta, 0, count - 1);
        picked.selected = next;
        const float top = static_cast<float>(next) * track_row_h;
        const float bottom = top + track_row_h;
        const float off = ui.scroll_offset(active_scroll_id_);
        if (top < off) {
            ui.scroll_to(active_scroll_id_, top, true);
        } else if (active_view_h_ > track_row_h && bottom > off + active_view_h_) {
            ui.scroll_to(active_scroll_id_, bottom - active_view_h_, true);
        }
        env_.redraw();
    }

    bool Tracks::play_selected() {
        if (!has_selected()) {
            return false;
        }
        playback_.play(*active_tracks_, browser_.selection().selected);
        return true;
    }

    void Tracks::select_all() {
        if (!has_list()) {
            return;
        }
        auto &picked = browser_.selection();
        auto &route = browser_.route();
        if (route.page == Page::playlist && route.list) {
            route.list->ordering = false;
        }
        picked.choosing = true;
        ++picked.revision;
        picked.chosen.clear();
        for (const auto &track : *active_tracks_) {
            picked.chosen.insert(track.id);
        }
        env_.redraw();
    }

    bool Tracks::list(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Track> &tracks, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &picked = browser_.selection();
        const auto &route = browser_.route();
        if (picked.list != &tracks) {
            browser_.reset_selection();
            picked.list = &tracks;
        }
        if (view_id_ != id || view_source_ != &tracks || view_route_serial_ != route.serial || view_tab_since_ != browser_.tab_since) {
            view_id_ = id;
            view_source_ = &tracks;
            view_route_serial_ = route.serial;
            view_tab_since_ = browser_.tab_since;
            sort_key_ = SortKey::none;
            sort_desc_ = false;
            filter_open_ = false;
            filter_focus_ = false;
            filter_.clear();
            cached_data_ = nullptr;
        }
        // The page's own playlist: the listener's to take songs out of and to put in order.
        const bool editable = route.page == Page::playlist && route.list && &tracks == &route.list->tracks;
        const bool ordering = editable && route.list->ordering;
        if (ordering && (sort_key_ != SortKey::none || !filter_.empty() || filter_open_)) {
            sort_key_ = SortKey::none;
            sort_desc_ = false;
            filter_.clear();
            filter_open_ = false;
        }

        const std::vector<Track> &shown = displayed_tracks(tracks);
        if (picked.selected >= static_cast<int>(shown.size())) {
            picked.selected = shown.empty() ? -1 : static_cast<int>(shown.size()) - 1;
        }
        const bool can_edit_playlist = !tracks.empty() && provider(tracks.front()).capabilities.playlist_edit;
        const bool show_toolbar = !ordering && !tracks.empty() && (tracks.size() >= auto_filter_threshold || filter_open_ || !filter_.empty());

        auto toggle_selecting = [&] {
            if (picked.choosing) {
                browser_.reset_selection();
                picked.list = &tracks;
            } else if (picked.selected >= 0 && picked.selected < static_cast<int>(shown.size())) {
                browser_.choose(shown, picked.selected, false);
            } else {
                picked.choosing = true;
            }
        };

        if (show_toolbar) {
            ui::cut_top(r, 6);
            const Rect bar = ui::cut_top(r, 36).inset(12, 0);
            ui::cut_top(r, 4);
            const auto select_label = lyren::tr(picked.choosing ? "Done selecting" : "Select songs");
            const float select_w = ui.button_width(select_label);
            if (ui.button(ui.id("select-songs"), {bar.right() - select_w, bar.y + (bar.h - control::small) / 2, select_w, control::small}, select_label, ui::ButtonKind::ghost)) {
                toggle_selecting();
            }
            const float avail = std::max(0.f, bar.w - select_w - 12);
            const float fw = std::min(260.f, avail);
            if (fw >= 96) {
                const ui::Id filter_id = ui.id("track-filter");
                if (std::exchange(filter_focus_, false)) {
                    ui.focus = filter_id;
                }
                const std::string before = filter_;
                ui.text_box(filter_id, {bar.x, bar.y + (bar.h - control::small) / 2, fw, control::small}, filter_, lyren::tr("Filter songs"), "search");
                if (filter_ != before) {
                    ui.scroll_to(id, 0);
                }
                if (!trim(filter_).empty() && avail - fw > 48) {
                    const auto count_text = small_format<24>("{} / {}", shown.size(), tracks.size());
                    ui.label({bar.x + fw + 12, bar.y, avail - fw - 14, bar.h}, count_text, t.style(type::caption, t.text_dim, text::tabular_nums));
                }
            }
        }

        const Rect head = ui::cut_top(r, 34);
        const TrackColumns c = track_columns(head);
        const auto caption = t.style(type::caption, t.faint());
        if (picked.choosing) {
            const bool all = !shown.empty() && std::ranges::all_of(shown, [&](const Track &track) {
                return picked.chosen.contains(track.id);
            });
            if (ui.icon_button(ui.id("select-all"), ui::center(c.index, 30, 30),
                               all                     ? "square-check"
                               : picked.chosen.empty() ? "square"
                                                       : "square-minus",
                               18, !picked.chosen.empty())) {
                picked.chosen.clear();
                if (!all) {
                    for (const auto &track : shown) {
                        picked.chosen.insert(track.id);
                    }
                }
            }
        } else {
            ui.label(c.index, "#", caption, Align::center);
        }

        auto sort_header = [&](std::string_view wid, Rect cell, std::string_view label, SortKey key, Align align) {
            if (ordering || tracks.empty()) {
                return ui.label(cell, label, caption, align);
            }
            const bool active = sort_key_ == key;
            const auto st = t.style(type::caption, active ? t.text : t.faint(), active ? text::medium : 0);
            const float lw = std::ceil(p.measure(label, st));
            constexpr float icon_sz = 12.f, icon_gap = 3.f;
            const float icon_w = active ? icon_sz + icon_gap : 0.f;
            const float hit_w = std::min(cell.w, lw + icon_w + 10);
            const Rect hit = align == Align::right ? Rect{cell.right() - hit_w, cell.y, hit_w, cell.h} : Rect{cell.x, cell.y, hit_w, cell.h};
            const ui::Id hid = ui.id(wid);
            const auto it = ui.interact(hid, hit);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const float hot = ui.anim(hid, it.hovered ? 1.f : 0.f, pace::quick);
            const ui::Color ink = active ? t.text : t.faint().mix(t.text, hot * 0.85f);
            const ui::TextStyle draw_st = t.style(type::caption, ink, active ? text::medium : 0);
            const float icon_y = std::round(hit.y + (hit.h - icon_sz) / 2 + 0.5f);
            if (align == Align::right) {
                ui.label({hit.right() - lw, hit.y, lw + 1, hit.h}, label, draw_st, Align::right);
                if (active && hit.right() - lw - icon_gap - icon_sz >= cell.x) {
                    p.icon(sort_desc_ ? "chevron-down" : "chevron-up", {hit.right() - lw - icon_gap - icon_sz, icon_y, icon_sz, icon_sz}, t.accent);
                }
            } else {
                ui.label({hit.x, hit.y, std::min(lw + 1, hit.w), hit.h}, label, draw_st);
                if (active && hit.x + lw + icon_gap + icon_sz <= cell.right()) {
                    p.icon(sort_desc_ ? "chevron-down" : "chevron-up", {hit.x + lw + icon_gap, icon_y, icon_sz, icon_sz}, t.accent);
                }
            }
            if (it.clicked) {
                if (sort_key_ != key) {
                    sort_key_ = key;
                    sort_desc_ = false;
                } else if (!sort_desc_) {
                    sort_desc_ = true;
                } else {
                    sort_key_ = SortKey::none;
                    sort_desc_ = false;
                }
                ui.scroll_to(id, 0);
            }
        };

        sort_header("sort-title", c.title, lyren::tr("Title"), SortKey::title, Align::left);
        if (c.album.w > 0) {
            sort_header("sort-album", c.album, lyren::tr("Album"), SortKey::album, Align::left);
        }
        if (!show_toolbar && !ordering && !tracks.empty()) {
            const float bx = c.duration.x - 6;
            if (ui.icon_button(ui.id("select-songs"), {bx - 26, head.y + (head.h - 26) / 2, 26, 26}, picked.choosing ? "square-check" : "square", 15, picked.choosing,
                               lyren::tr(picked.choosing ? "Done selecting" : "Select songs"))) {
                toggle_selecting();
            }
            if (ui.icon_button(ui.id("open-filter"), {bx - 54, head.y + (head.h - 26) / 2, 26, 26}, "search", 15, false, lyren::tr("Filter songs"))) {
                filter_open_ = filter_focus_ = true;
            }
        }
        if (ordering) {
            ui.label(c.duration, lyren::tr("Order"), caption, Align::right);
        } else {
            sort_header("sort-duration", c.duration, lyren::tr("Time"), SortKey::duration, Align::right);
        }
        p.rect({head.x + 12, head.bottom() - 1, head.w - 24, 1}, t.line());
        ui::cut_top(r, 4);

        // What to do with the picked songs comes in above the rows.
        if (const float bar_h = ui.spring(ui::sub_id(id, 0xba), picked.choosing ? 48.f : 0.f, pace::glide); bar_h > 0.5f) {
            const Rect bar = ui::cut_top(r, bar_h).inset(4, 0);
            Fade fade(p, std::clamp(bar_h / 48, 0.f, 1.f));
            ui::Enabled live(ui, picked.choosing, 1);
            ui.push_clip({bar.x, bar.y, bar.w, bar_h});
            const Rect tray{bar.x, bar.y + 2, bar.w, 40};
            p.rect(tray, t.fill(0.75f), t.radius, t.surface_edge(1.1f), 1);
            const bool compact_bar = tray.w < 680;
            float right = tray.right() - 8;
            auto bar_button = [&](std::string_view bid, std::string_view label_key, std::string_view icon, bool enabled, ui::ButtonKind kind = ui::ButtonKind::secondary) {
                const auto text = lyren::tr(label_key);
                const float w = compact_bar ? control::small : ui.button_width(text, true);
                ui::Enabled possible(ui, enabled);
                const bool clicked = ui.button(ui.id(bid), {right - w, tray.y + (tray.h - control::small) / 2, w, control::small}, compact_bar ? std::string_view{} : text, kind,
                                               icon, compact_bar ? text : std::string_view{});
                right -= w + 6;
                return clicked;
            };
            const bool has_chosen = !picked.chosen.empty();
            if (editable && route.card.owned && can_edit_playlist) {
                const bool busy = library_.writing(route.card.source, route.card.id);
                if (bar_button("remove-selected", busy ? "Saving..." : "Remove", "list-minus", has_chosen && !busy)) {
                    spawn(library_.edit_playlist(route.card, picked.of(shown), false, picked.revision));
                }
            }
            if (can_edit_playlist) {
                if (bar_button("add-selected", "Add to playlist", "folder-plus", has_chosen)) {
                    shell_.dialogs.pick_playlist(ui, picked.of(shown), picked.revision);
                }
            }
            if (bar_button("enqueue-selected", "Add to queue", "list-plus", has_chosen)) {
                playback_.enqueue(picked.of(shown));
                ui.toast(std::string(lyren::tr("Added to queue")), "list-plus");
            }
            if (bar_button("play-next-selected", "Play next", "list-start", has_chosen)) {
                playback_.play_next(picked.of(shown));
                ui.toast(std::string(lyren::tr("Added to play next")), "list-start");
            }
            if (bar_button("play-selected", "Play selected", "play", has_chosen, ui::ButtonKind::tonal)) {
                playback_.play(picked.of(shown), 0);
            }
            ui.label({tray.x + 14, tray.y, std::max(0.f, right - tray.x - 22), tray.h}, lyren::trf("{} selected", picked.chosen.size()),
                     t.style(type::label, has_chosen ? t.text : t.text_dim, (has_chosen ? text::medium : 0) | text::tabular_nums));
            ui.pop_clip();
        }

        active_tracks_ = &shown;
        active_scroll_id_ = id;
        active_view_h_ = r.h;

        if (shown.empty() && !tracks.empty()) {
            empty(ui, r, "search", lyren::tr("No matching songs"));
            return false;
        }

        const float off = ui.begin_scroll(id, r, static_cast<float>(shown.size()) * track_row_h + 8);
        // One row past the bottom keeps its place, so it slides in with the rest when a row above it goes.
        const int count = static_cast<int>(shown.size());
        const int first = static_cast<int>(off / track_row_h), last = static_cast<int>((off + r.h) / track_row_h) + 2;
        for (int i = std::max(0, first - 8); i < std::min(last + 16, count); ++i) {
            if (i < first || i > last) {
                images_.prefetch(shown[static_cast<size_t>(i)].cover, static_cast<uint32_t>(Images::tier(40 * p.scale())));
            }
        }
        // Rows glide to a new place when the list changes under them; a list that arrives anew starts in place.
        std::optional<Track> toggled_like;
        ui.push_id(ui::sub_id(id, std::bit_cast<uint64_t>(since)));
        for (int i = first; i <= last && i < count; ++i) {
            const float in = ui.cascade(since, i - first);
            Fade fade(p, in);
            const float y = ui.spring(ui::sub_id(item_id(ui, shown[static_cast<size_t>(i)]), 0x70), static_cast<float>(i) * track_row_h, pace::glide);
            row(ui, {r.x, r.y + y - off + (1 - in) * 12, r.w, track_row_h}, shown, i, toggled_like);
        }
        ui.pop_id();
        if (off > 0.5f && t.surface.a > 0.9f) {
            p.gradient({r.x, r.y, r.w, 12}, t.surface.with_alpha(0.9f), t.surface.with_alpha(0));
        }
        ui.end_scroll();
        if (toggled_like) {
            library_.toggle_like(std::move(*toggled_like));
        }
        return trim(filter_).empty() && sort_key_ == SortKey::none && last + 10 >= count;
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
        const bool ordering = route.page == Page::playlist && route.list && &tracks == &route.list->tracks && route.list->ordering;
        // Ctrl and Shift pick rows on every platform, as the checkboxes do.
        constexpr uint32_t pick_mods = wl::mod_ctrl | wl::mod_shift;
        const bool picking = !ordering && (picked.choosing || (ui.input().mods & pick_mods) || std::ranges::any_of(ui.input().actions, [&](const ui::PointerAction &action) {
                                               return action.press && action.button == BTN_LEFT && r.contains(action.x, action.y) && (action.mods & pick_mods);
                                           }));
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
        const auto vip_style = t.style(type::micro, t.text_dim, text::medium);
        const float vip_h = std::round(vip_style.size * 1.36f);
        const float vip_w = track.paid ? std::ceil(p.measure("VIP", vip_style)) + 10 : 0.f;
        const float title_max = track.paid && tw > vip_w + 32 ? tw - vip_w - 7 : tw;
        const float w = p.text(tx, r.y + 25, track.title, title, title_max);
        float after_x = tx + w;
        if (track.paid && after_x + 7 + vip_w <= tx + tw) {
            const float mid = r.y + 25 - p.ascent(title.size) * 0.5f;
            const Rect badge{after_x + 7, std::round(mid - vip_h / 2), vip_w, vip_h};
            p.rect(badge, t.fill(0.95f), std::min(4.f, t.radius_s() * 0.7f), t.surface_edge(1.5f), 1);
            ui.label(badge, "VIP", vip_style, Align::center);
            after_x = badge.right();
        }
        if (!track.alias.empty() && after_x + 24 < tx + tw) {
            const auto alias_style = t.style(type::body, t.faint());
            const float rem = tx + tw - after_x - 6;
            const float open_w = p.measure("（", alias_style);
            const float alias_w = p.measure(track.alias, alias_style);
            const float close_w = p.measure("）", alias_style);
            float ax = after_x + 6;
            ax += p.text(ax, r.y + 25, "（", alias_style, rem);
            if (open_w + alias_w + close_w <= rem) {
                ax += p.text(ax, r.y + 25, track.alias, alias_style);
                p.text(ax, r.y + 25, "）", alias_style);
            } else if (rem > open_w) {
                p.text(ax, r.y + 25, track.alias, alias_style, rem - open_w);
            }
        }
        if (pins_.is_pinned(track)) {
            p.icon("pin", {tx, r.y + 33, 11, 11}, pins_.is_cached(track) ? t.accent.with_alpha(0.85f) : t.faint());
            p.text(tx + 15, r.y + 43, track.artist, t.style(type::caption, t.text_dim), std::max(0.f, tw - 15));
        } else {
            p.text(tx, r.y + 43, track.artist, t.style(type::caption, t.text_dim), tw);
        }
        if (c.album.w > 0) {
            ui.label(c.album, track.album, t.style(type::label, t.text_dim));
        }
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

    void Tracks::menu(ui::Ui &ui, float x, float y, const std::vector<Track> &tracks, int i, bool in_queue) {
        const Track track = tracks[static_cast<size_t>(i)];
        const auto &picked = browser_.selection();
        const auto &route = browser_.route();
        const auto &caps = provider(track).capabilities;
        // One's own playlist can take songs out.
        const bool removable = !in_queue && route.page == Page::playlist && route.card.owned && caps.playlist_edit && route.list &&
                               (&tracks == &route.list->tracks || (&tracks == &view_tracks_ && view_source_ == &route.list->tracks));
        std::vector<ui::MenuItem> items;
        auto add = [&](std::string_view icon, std::string_view label, std::move_only_function<void()> action, bool danger = false) {
            items.push_back({std::string(icon), std::string(lyren::tr(label)), std::move(action), danger});
        };
        if (!in_queue && picked.choosing && is_picked_list(tracks) && picked.chosen.size() > 1 && picked.chosen.contains(track.id)) {
            auto songs = std::make_shared<std::vector<Track>>(picked.of(tracks));
            add("play", "Play selected", [this, songs] {
                playback_.play(*songs, 0);
            });
            add("list-start", "Play next", [this, &ui, songs] {
                playback_.play_next(*songs);
                ui.toast(std::string(lyren::tr("Added to play next")), "list-start");
            });
            add("list-plus", "Add to queue", [this, &ui, songs] {
                playback_.enqueue(*songs);
                ui.toast(std::string(lyren::tr("Added to queue")), "list-plus");
            });
            const bool all_pinned = std::ranges::all_of(*songs, [&](const Track &tr) {
                return pins_.is_pinned(tr);
            });
            add(all_pinned ? "pin-off" : "pin", all_pinned ? "Unpin offline" : "Pin offline", [this, songs, all_pinned] {
                if (all_pinned) {
                    pins_.unpin_tracks(*songs);
                } else {
                    pins_.pin_tracks(*songs);
                }
            });
            if (caps.playlist_edit) {
                items.push_back({});
                add("folder-plus", "Add to playlist", [this, &ui, songs, tag = picked.revision] {
                    shell_.dialogs.pick_playlist(ui, *songs, tag);
                });
            }
            if (removable) {
                add(
                    "list-minus", "Remove from playlist",
                    [this, playlist = route.card, songs, tag = picked.revision] {
                        spawn(library_.edit_playlist(playlist, *songs, false, tag));
                    },
                    true);
            }
            return ui.open_menu(x, y, std::move(items));
        }
        if (in_queue) {
            add("play", "Play", [this, i] {
                playback_.jump(i);
            });
            if (i > 0) {
                add("arrow-up-to-line", "Move to top", [this, i] {
                    playback_.move(i, 0);
                });
            }
            if (i + 1 < static_cast<int>(tracks.size())) {
                add("arrow-down-to-line", "Move to bottom", [this, i, last = static_cast<int>(tracks.size()) - 1] {
                    playback_.move(i, last);
                });
            }
        } else {
            add("play", "Play", [this, songs = tracks, i] {
                playback_.play(songs, i);
            });
            add("list-start", "Play next", [this, &ui, track] {
                playback_.play_next(track);
                ui.toast(std::string(lyren::tr("Added to play next")), "list-start");
            });
            add("list-plus", "Add to queue", [this, &ui, track] {
                playback_.enqueue(track);
                ui.toast(std::string(lyren::tr("Added to queue")), "list-plus");
            });
        }
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
            add("audio-lines", "Similar songs", [this, track] {
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
        {
            const bool pinned = pins_.is_pinned(track);
            add(pinned ? "pin-off" : "pin", pinned ? "Unpin offline" : "Pin offline", [this, track] {
                pins_.toggle_pin(track);
            });
        }
        add("copy", "Copy title and artist", [&ui, track] {
            ui.copy = track.title + " - " + track.artist;
            ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
        });
        if (auto link = share_url(track); !link.empty()) {
            add("link", "Copy link", [&ui, link = std::move(link)] {
                ui.copy = link;
                ui.toast(std::string(lyren::tr("Link copied")), "link");
            });
        }
        if (!in_queue && route.page == Page::daily && route.list && (&tracks == &route.list->tracks || (&tracks == &view_tracks_ && view_source_ == &route.list->tracks)) &&
            provider(track).features.daily.dislike) {
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
                    spawn(library_.edit_playlist(playlist, {track}, false));
                },
                true);
        }
        ui.open_menu(x, y, std::move(items));
    }

} // namespace lyren
