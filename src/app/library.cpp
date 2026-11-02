#include "app/shell.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <span>
#include <unordered_map>

#include "app/draw.hpp"

namespace lyren {

    using ui::Rect;

    // Adds a missing item or takes it out; server metadata and ordering win for existing items.
    template <class T>
    static void place(std::vector<T> &items, const T &item, bool on) {
        auto it = std::ranges::find_if(items, [&](const T &x) {
            return same(x, item);
        });
        if (on && it == items.end()) {
            items.insert(items.begin(), item);
        } else if (!on && it != items.end()) {
            items.erase(it);
        }
    }

    bool Shell::liked(const Track &t) const {
        return library(t.source).liked_ids.contains(t.id);
    }

    bool Shell::preference_value(Source source, Preference kind, std::string_view id, bool value) const {
        const auto &edits = library(source).preferences[static_cast<size_t>(kind)];
        auto it = edits.find(id);
        return it == edits.end() ? value : it->second.value;
    }

    void Shell::apply_preference(Source source, Preference kind, const PreferenceEdit &edit) {
        auto &lib = library(source);
        const bool on = edit.value;
        if (kind == Preference::song) {
            if (on) {
                lib.liked_ids.insert(edit.track.id);
            } else {
                lib.liked_ids.erase(edit.track.id);
            }
            auto update = [&](Route &route) {
                if (route.card.source == source && route.page == Page::liked && route.list) {
                    place(route.list->tracks, edit.track, on);
                }
            };
            update(route_);
            for (auto &route : history_) {
                update(route);
            }
        } else if (kind == Preference::playlist) {
            place(lib.playlists, edit.card, on);
        } else {
            auto &collection = lib.collection[kind == Preference::artist ? 1 : 0];
            if (collection.loaded) {
                place(collection.cards, edit.card, on);
                collection.total = static_cast<int>(collection.cards.size());
            }
        }
    }

    void Shell::set_preference(Source source, Preference kind, Track track, Card card, bool on) {
        auto &lib = library(source);
        auto &[id, edit] = *lib.preferences[static_cast<size_t>(kind)].try_emplace(kind == Preference::song ? track.id : card.id, PreferenceEdit{.confirmed = !on}).first;
        edit.track = std::move(track);
        edit.card = std::move(card);
        edit.value = on;
        edit.revision = ++lib.preference_gen;
        apply_preference(source, kind, edit);
        if (!edit.pending) {
            edit.pending = true;
            spawn(write_preference(source, kind, id, lib.account_gen));
        }
        redraw_();
    }

    Task<void> Shell::write_preference(Source source, Preference kind, std::string id, uint64_t account_gen) {
        auto &lib = library(source);
        auto &edit = lib.preferences[static_cast<size_t>(kind)].at(id);
        for (;;) {
            bool on = edit.value;
            uint64_t revision = edit.revision;
            Result<void> result;
            switch (kind) {
                case Preference::song:
                    result = co_await provider(source).like(edit.track, on);
                    break;
                case Preference::artist:
                    result = co_await provider(source).follow(edit.card.id, on);
                    break;
                case Preference::album:
                    result = co_await provider(source).collect(edit.card.id, on);
                    break;
                case Preference::playlist:
                    result = co_await provider(source).collect_playlist(edit.card.id, on);
                    break;
            }
            if (account_gen != lib.account_gen) {
                co_return;
            }
            bool changed = edit.value != on || (!result && edit.revision != revision);
            edit.revision = ++lib.preference_gen;
            if (result) {
                edit.confirmed = on;
            } else if (!changed) {
                edit.value = edit.confirmed;
                apply_preference(source, kind, edit);
                notify(lyren::trf("Action failed: {}", result.error()), "triangle-alert", true);
            }
            if (!changed) {
                break;
            }
            // A newer click still needs its own write, even if the previous response was lost.
        }
        edit.pending = false;
        redraw_();
        if (kind == Preference::playlist) {
            uint64_t revision = lib.preference_gen;
            auto favorites = co_await provider(source).collected_playlists();
            if (account_gen != lib.account_gen) {
                co_return;
            }
            if (favorites) {
                std::erase_if(lib.playlists, [](const Card &card) {
                    return !card.owned;
                });
                lib.playlists.append_range(std::views::as_rvalue(*favorites));
                reconcile_preferences(source, kind, revision);
            } else {
                notify(favorites.error(), "list-music", true);
            }
            redraw_();
        } else if (kind == Preference::song && provider(source).reload_after_edit && route_.page == Page::liked && route_.card.source == source && route_.list) {
            co_await load_list(route_.list, Page::liked, {.source = source});
        }
    }

    bool Shell::reconcile_preference(Source source, Preference kind, std::string_view id, bool value, uint64_t revision) {
        auto &edits = library(source).preferences[static_cast<size_t>(kind)];
        auto it = edits.find(id);
        if (it == edits.end()) {
            return value;
        }
        auto &edit = it->second;
        if (!edit.pending && edit.revision <= revision) {
            edit.value = edit.confirmed = value;
        }
        // Keep the revision so a read begun before the write cannot overwrite a later readback.
        apply_preference(source, kind, edit);
        return edit.value;
    }

    void Shell::reconcile_preferences(Source source, Preference kind, uint64_t revision) {
        auto &lib = library(source);
        auto &edits = lib.preferences[static_cast<size_t>(kind)];
        for (const auto &[id, edit] : edits) {
            bool value;
            if (kind == Preference::song) {
                value = lib.liked_ids.contains(id);
            } else {
                const auto &cards = kind == Preference::playlist ? lib.playlists : lib.collection[kind == Preference::artist ? 1 : 0].cards;
                value = std::ranges::any_of(cards, [&](const Card &card) {
                    return card.id == id;
                });
            }
            reconcile_preference(source, kind, id, value, revision);
        }
    }

    void Shell::toggle_like(Track t) {
        if (!provider(t).logged_in()) {
            return notify(lyren::trf("Sign in to {} to like this song", lyren::tr(platform(t.source).name)), "heart");
        }
        bool on = !liked(t);
        if (on) {
            pop_key_ = key(t), pop_time_ = now_();
        }
        notify(std::string(lyren::tr(on ? "Added to liked songs" : "Removed from liked songs")), on ? "heart-fill" : "heart-off");
        Source source = t.source;
        set_preference(source, Preference::song, std::move(t), {}, on);
    }

    void Shell::toggle_follow(std::shared_ptr<ArtistPage> page, Card artist) {
        if (!provider(artist).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this artist's platform to follow them")), "user-plus");
        }
        bool on = !preference_value(artist.source, Preference::artist, artist.id, page->info->followed);
        notify(on ? lyren::trf("Following {}", artist.title) : std::string(lyren::tr("Unfollowed artist")), on ? "user-check" : "user");
        Source source = artist.source;
        set_preference(source, Preference::artist, {}, std::move(artist), on);
    }

    void Shell::toggle_collect(std::shared_ptr<List> list, Card album) {
        if (!provider(album).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this album's platform to save it")), "star");
        }
        bool on = !preference_value(album.source, Preference::album, album.id, list->album.collected);
        notify(std::string(lyren::tr(on ? "Album saved" : "Album removed")), on ? "star-fill" : "star");
        Source source = album.source;
        set_preference(source, Preference::album, {}, std::move(album), on);
    }

    void Shell::apply_playlists(Source source, std::pair<Card, std::vector<Card>> playlists, uint64_t revision) {
        auto &lib = library(source);
        lib.liked = std::move(playlists.first);
        lib.playlists = std::move(playlists.second);
        lib.playlists_loaded = true;
        lib.library_error.clear();
        reconcile_preferences(source, Preference::playlist, revision);
        images_.prefetch(lib.playlists);
        auto update = [&](Route &route) {
            if (route.page == Page::playlist && route.card.source == source) {
                auto it = std::ranges::find_if(lib.playlists, [&](const Card &card) {
                    return same(card, route.card);
                });
                if (it != lib.playlists.end()) {
                    route.card = *it;
                }
            }
        };
        update(route_);
        for (auto &route : history_) {
            update(route);
        }
    }

    Task<bool> Shell::refresh_playlists(Source source, uint64_t account_gen) {
        auto &lib = library(source);
        const uint64_t generation = ++lib.playlist_gen, revision = lib.preference_gen;
        auto playlists = co_await provider(source).user_playlists(lib.account->id);
        if (account_gen != lib.account_gen || generation != lib.playlist_gen) {
            co_return false;
        }
        if (!playlists) {
            lib.library_error = playlists.error();
            notify(lyren::trf("Could not confirm playlist changes: {}", lyren::tr(playlists.error())), "triangle-alert", true);
            co_return false;
        }
        apply_playlists(source, std::move(*playlists), revision);
        redraw_();
        co_return true;
    }

    bool Shell::begin_playlist_write(const Card &playlist) {
        auto &lib = library(playlist.source);
        if (!provider(playlist).logged_in() || !lib.account) {
            notify(lyren::trf("Sign in to {} to edit playlists", lyren::tr(platform(playlist.source).name)), "user");
            return false;
        }
        if (!lib.playlist_writes.insert(playlist.id).second) {
            notify(std::string(lyren::tr("The playlist is still saving")), "list-music");
            return false;
        }
        redraw_();
        return true;
    }

    void Shell::finish_playlist_write(const Card &playlist) {
        library(playlist.source).playlist_writes.erase(playlist.id);
        redraw_();
    }

    Task<void> Shell::create_playlist(Source source, std::string name, bool hidden, std::vector<Track> tracks) {
        Card pending{.source = source};
        if (!begin_playlist_write(pending)) {
            co_return;
        }
        const uint64_t account_gen = library(source).account_gen;
        auto created = co_await provider(source).create_playlist(name, hidden);
        if (account_gen != library(source).account_gen) {
            co_return;
        }
        co_await refresh_playlists(source, account_gen);
        if (account_gen != library(source).account_gen) {
            co_return;
        }
        finish_playlist_write(pending);
        if (!created) {
            // Without the returned ID, a same-name playlist is not proof of this creation.
            co_return notify(lyren::trf("Could not confirm playlist creation: {}", lyren::tr(created.error())), "triangle-alert", true);
        }
        place(library(source).playlists, *created, true);
        notify(lyren::trf("Created playlist {}", created->title), "circle-check");
        if (!tracks.empty()) {
            add_to_playlist(std::move(tracks), std::move(*created));
        }
    }

    Task<void> Shell::rename_playlist(Card playlist, std::string name) {
        if (!begin_playlist_write(playlist)) {
            co_return;
        }
        auto &lib = library(playlist.source);
        const uint64_t account_gen = lib.account_gen;
        auto result = co_await provider(playlist).rename_playlist(playlist.id, name);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        const bool refreshed = co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_playlist_write(playlist);
        if (!refreshed) {
            co_return;
        }
        auto it = std::ranges::find_if(lib.playlists, [&](const Card &card) {
            return same(card, playlist);
        });
        if (it != lib.playlists.end() && it->title == name) {
            notify(std::string(lyren::tr("Playlist renamed")), "pencil");
        } else {
            notify(result ? std::string(lyren::tr("Playlist name was not saved")) : lyren::trf("Could not rename: {}", lyren::tr(result.error())), "triangle-alert", true);
        }
    }

    Task<void> Shell::delete_playlist(Card playlist) {
        if (!begin_playlist_write(playlist)) {
            co_return;
        }
        auto &lib = library(playlist.source);
        const uint64_t account_gen = lib.account_gen;
        auto result = co_await provider(playlist).delete_playlist(playlist.id);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        const bool refreshed = co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_playlist_write(playlist);
        if (!refreshed) {
            co_return;
        }
        if (std::ranges::none_of(lib.playlists, [&](const Card &card) {
                return same(card, playlist);
            })) {
            notify(lyren::trf("Deleted playlist {}", playlist.title), "trash");
        } else {
            notify(result ? std::string(lyren::tr("Playlist was not deleted")) : lyren::trf("Could not delete playlist: {}", lyren::tr(result.error())), "triangle-alert", true);
        }
    }

    void Shell::toggle_collect_playlist(Card playlist) {
        if (!provider(playlist).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this playlist's platform to save it")), "star");
        }
        const auto &playlists = library(playlist.source).playlists;
        bool on = !std::ranges::any_of(playlists, [&](const Card &card) {
            return same(card, playlist);
        });
        notify(std::string(lyren::tr(on ? "Playlist saved" : "Playlist removed")), on ? "star-fill" : "star");
        Source source = playlist.source;
        set_preference(source, Preference::playlist, {}, std::move(playlist), on);
    }

    void Shell::add_to_playlist(std::vector<Track> tracks, Card playlist) {
        auto list = route_.page == Page::playlist && same(route_.card, playlist) ? route_.list : nullptr;
        if (list) {
            std::unordered_set<std::string_view> existing;
            existing.reserve(list->tracks.size());
            for (const auto &track : list->tracks) {
                existing.insert(track.id);
            }
            std::erase_if(tracks, [&](const Track &track) {
                return existing.contains(track.id);
            });
        }
        if (tracks.empty()) {
            return notify(std::string(lyren::tr("These songs are already in the playlist")), "folder-plus");
        }
        spawn(write_playlist_tracks(std::move(list), std::move(playlist), std::move(tracks), true));
    }

    void Shell::remove_from_playlist(std::shared_ptr<List> list, Card playlist, std::vector<Track> tracks) {
        spawn(write_playlist_tracks(std::move(list), std::move(playlist), std::move(tracks), false));
    }

    void Shell::apply_playlist_tracks(std::shared_ptr<List> list, const Card &playlist, std::vector<Track> tracks) {
        if (!list && route_.page == Page::playlist && same(route_.card, playlist)) {
            list = route_.list;
        }
        if (!list) {
            list = std::make_shared<List>();
            list->since = now_();
        }
        ++list->gen;
        list->tracks = std::move(tracks);
        list->loading = false;
        list->error.clear();
        if (selection_list_ == &list->tracks) {
            selected_ = selection_anchor_ = -1;
            std::unordered_set<std::string> remaining;
            for (const auto &track : list->tracks) {
                if (chosen_.contains(track.id)) {
                    remaining.insert(track.id);
                }
            }
            chosen_ = std::move(remaining);
        }
        auto update = [&](Route &route) {
            if (route.page == Page::playlist && same(route.card, playlist)) {
                route.list = list;
                route.card.count = static_cast<int>(list->tracks.size());
            }
        };
        update(route_);
        for (auto &route : history_) {
            update(route);
        }
        for (auto &card : library(playlist.source).playlists) {
            if (same(card, playlist)) {
                card.count = static_cast<int>(list->tracks.size());
            }
        }
        redraw_();
    }

    Task<void> Shell::write_playlist_tracks(std::shared_ptr<List> list, Card playlist, std::vector<Track> tracks, bool add) {
        if (!begin_playlist_write(playlist)) {
            co_return;
        }
        auto &lib = library(playlist.source);
        const uint64_t account_gen = lib.account_gen;
        const auto *origin_selection = selection_list_;
        Result<void> result;
        for (auto batch : std::span(tracks) | std::views::chunk(100)) {
            result = co_await provider(playlist).edit_playlist(playlist.id, std::vector<Track>(batch.begin(), batch.end()), add);
            if (account_gen != lib.account_gen) {
                co_return;
            }
            if (!result) {
                break;
            }
        }
        // Even a lost or rejected batch can have applied some songs; the service's list decides the outcome.
        auto fresh = co_await provider(playlist).tracks(playlist);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        if (!fresh) {
            finish_playlist_write(playlist);
            if (list) {
                list->error = fresh.error();
            }
            co_return notify(lyren::trf("Could not confirm playlist changes: {}", lyren::tr(fresh.error())), "triangle-alert", true);
        }
        size_t confirmed = 0;
        {
            std::unordered_map<std::string_view, int> actual;
            actual.reserve(fresh->size());
            for (const auto &track : *fresh) {
                actual.emplace(track.id, track.type);
            }
            for (const auto &track : tracks) {
                const auto it = actual.find(track.id);
                const bool present = it != actual.end() && it->second == track.type;
                confirmed += present == add;
            }
            if (confirmed != tracks.size() && choosing_ && selection_list_ == origin_selection && chosen_.size() == tracks.size() &&
                std::ranges::all_of(tracks, [&](const Track &track) {
                    return chosen_.contains(track.id);
                })) {
                for (const auto &track : tracks) {
                    const auto it = actual.find(track.id);
                    const bool present = it != actual.end() && it->second == track.type;
                    if (present == add) {
                        chosen_.erase(track.id);
                    }
                }
            }
        }
        apply_playlist_tracks(std::move(list), playlist, std::move(*fresh));
        co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_playlist_write(playlist);
        const bool complete = confirmed == tracks.size();
        std::string message = lyren::trf(add ? "Added {} of {} songs to {}" : "Removed {} of {} songs from {}", confirmed, tracks.size(), playlist.title);
        if (!complete && !result) {
            message += ": " + std::string(lyren::tr(result.error()));
        }
        notify(std::move(message), add ? "folder-plus" : "list-minus", !complete);
    }

    void Shell::move_playlist_track(std::shared_ptr<List> list, Card playlist, int from, int to) {
        if (!list->error.empty()) {
            return notify(std::string(lyren::tr("Refresh this playlist before reordering")), "refresh-cw");
        }
        if (!provider(playlist).logged_in()) {
            return notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(playlist.source).name)), "user");
        }
        if (!begin_playlist_write(playlist)) {
            return;
        }
        auto first = list->tracks.begin();
        if (from < to) {
            std::rotate(first + from, first + from + 1, first + to + 1);
        } else {
            std::rotate(first + to, first + from, first + from + 1);
        }
        selected_ = to;
        list->saving_order = true;
        uint64_t gen = library(playlist.source).account_gen;
        redraw_();
        spawn([](Shell *self, std::shared_ptr<List> list, Card playlist, std::vector<Track> order, int selected, uint64_t gen) -> Task<void> {
            auto r = co_await self->provider(playlist).reorder_playlist(playlist.id, order);
            auto &lib = self->library(playlist.source);
            if (gen != lib.account_gen) {
                list->saving_order = false;
                co_return self->redraw_();
            }
            // A lost write response may still have committed; read back before reporting the result.
            auto fresh = co_await self->provider(playlist).tracks(playlist);
            list->saving_order = false;
            if (gen != lib.account_gen) {
                co_return self->redraw_();
            }
            self->finish_playlist_write(playlist);
            if (!fresh) {
                list->error = fresh.error();
                co_return self->notify(lyren::trf("Could not refresh playlist order: {}", fresh.error()), "triangle-alert", true);
            }
            bool confirmed = std::ranges::equal(*fresh, order, [](const Track &a, const Track &b) {
                return same(a, b) && a.type == b.type;
            });
            list->tracks = std::move(*fresh);
            list->error.clear();
            if (self->route_.list == list) {
                auto it = std::ranges::find(list->tracks, order[static_cast<size_t>(selected)].id, &Track::id);
                self->selected_ = it == list->tracks.end() ? -1 : static_cast<int>(it - list->tracks.begin());
            }
            if (confirmed) {
                self->notify(std::string(lyren::tr("Playlist order saved")), "list-ordered");
            } else {
                self->notify(r ? std::string(lyren::tr("Playlist order was not saved")) : lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->redraw_();
        }(this, list, std::move(playlist), list->tracks, to, gen));
    }

    void Shell::link_menu(ui::Ui &ui, float x, float y, const Card &card) {
        auto link = share_url(card);
        if (link.empty()) {
            return;
        }
        ui.open_menu(x, y, {{"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                 ui.copy = link;
                                 ui.toast(std::string(lyren::tr("Link copied")), "copy");
                             }}});
    }

    // The song leaves the daily list at once; the one the server recommends instead takes its place.
    void Shell::dislike_daily(std::shared_ptr<List> list, Track t) {
        auto it = std::ranges::find_if(list->tracks, [&](const Track &x) {
            return same(x, t);
        });
        if (it == list->tracks.end()) {
            return;
        }
        size_t at = static_cast<size_t>(it - list->tracks.begin());
        list->tracks.erase(it);
        spawn([](Shell *self, std::shared_ptr<List> list, Track t, size_t at) -> Task<void> {
            auto r = co_await self->provider(t).features.daily.dislike(t.id);
            auto pos = list->tracks.begin() + std::min(at, list->tracks.size());
            if (!r) {
                list->tracks.insert(pos, std::move(t));
                co_return self->notify(lyren::trf("Could not mark as not interested: {}", r.error()), "triangle-alert", true);
            }
            if (*r) {
                list->tracks.insert(pos, std::move(**r));
            }
            self->notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
        }(this, std::move(list), std::move(t), at));
    }

    Task<void> Shell::heartbeat(Track seed) {
        auto r = co_await provider(seed).features.heartbeat(seed.id, library(seed.source).liked.id);
        if (!r) {
            co_return notify(lyren::trf("Could not start Heartbeat mode: {}", r.error()), "triangle-alert", true);
        }
        std::vector<Track> list = std::move(*r);
        std::erase_if(list, [&](const Track &x) {
            return same(x, seed);
        });
        list.insert(list.begin(), std::move(seed));
        if (mode_ == Mode::shuffle) {
            set_mode(Mode::list);
        }
        play(list, 0);
        notify(std::string(lyren::tr("Heartbeat mode mixes your favorites with similar songs")), "heart-pulse");
    }

    std::vector<Track> Shell::chosen_tracks(const std::vector<Track> &tracks) const {
        std::vector<Track> selected;
        selected.reserve(chosen_.size());
        for (const auto &track : tracks) {
            if (chosen_.contains(track.id)) {
                selected.push_back(track);
            }
        }
        return selected;
    }

    void Shell::choose_track(const std::vector<Track> &tracks, int i, bool range) {
        if (route_.page == Page::playlist && route_.list) {
            route_.list->ordering = false;
        }
        if (!choosing_ && selected_ >= 0) {
            chosen_.insert(tracks[static_cast<size_t>(selected_)].id);
            selection_anchor_ = selected_;
        }
        choosing_ = true;
        if (range && selection_anchor_ >= 0) {
            for (int at = std::min(i, selection_anchor_); at <= std::max(i, selection_anchor_); ++at) {
                chosen_.insert(tracks[static_cast<size_t>(at)].id);
            }
        } else {
            const auto &id = tracks[static_cast<size_t>(i)].id;
            if (!chosen_.erase(id)) {
                chosen_.insert(id);
            }
            selection_anchor_ = i;
        }
        selected_ = i;
        redraw_();
    }

    void Shell::reset_selection() {
        selected_ = selection_anchor_ = -1;
        selection_list_ = nullptr;
        choosing_ = false;
        chosen_.clear();
    }

    void Shell::set_dislike(Dislike item, bool on) {
        if (!provider(item).logged_in()) {
            return notify(lyren::trf("Sign in to {} to change recommendations", lyren::tr(platform(item.source).name)), "user");
        }
        auto &cached = library(item.source).dislikes[static_cast<size_t>(item.kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        if (!list->pending.insert(item.id).second) {
            return;
        }
        redraw_();
        spawn([](Shell *self, Dislike item, bool on, std::shared_ptr<DislikeList> list) -> Task<void> {
            auto r = co_await self->provider(item).features.exclusions.set(item, on);
            list->pending.erase(item.id);
            if (self->library(item.source).dislikes[static_cast<size_t>(item.kind)] != list) {
                co_return;
            }
            if (!r) {
                co_return self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->notify(std::string(lyren::tr(on ? "Hidden from recommendations" : "Restored to recommendations")), on ? "thumbs-down" : "check");
            co_await self->load_dislikes(item.source, item.kind);
        }(this, std::move(item), on, std::move(list)));
    }

    void Shell::open_editor(ui::Ui &ui, Card card, std::vector<Track> tracks) {
        edit_name_ = card.title;
        edit_hidden_ = false;
        edit_focus_ = true;
        edit_tracks_ = std::move(tracks);
        edit_source_ = card.id.empty() ? edit_tracks_.empty() ? active_ : edit_tracks_.front().source : card.source;
        ui.open_modal(400, card.id.empty() ? 286 : 216, [this, card](ui::Ui &u, Rect r) {
            editor_modal(u, r, card);
        });
    }

    void Shell::editor_modal(ui::Ui &ui, Rect r, Card card) {
        const auto &t = ui.theme();
        bool create = card.id.empty();
        ui.label({r.x, r.y, r.w - 40, 30}, create ? lyren::tr("New playlist") : lyren::tr("Rename playlist"), {.size = t.fs(20), .color = t.text});
        if (create) {
            ui.label({r.x, r.y + 30, r.w - 40, 18}, lyren::tr(platform(edit_source_).name), {.size = t.fs(12), .color = t.text_dim});
        }
        ui::Id field = ui.id("playlist-name");
        if (std::exchange(edit_focus_, false)) {
            ui.focus = field;
        }
        bool submit = ui.text_box(field, {r.x, r.y + 52, r.w, 38}, edit_name_, lyren::tr("Playlist name"));
        if (create && provider(edit_source_).capabilities.private_playlists) {
            float y = r.y + 110;
            ui.label({r.x, y, r.w - 60, 20}, lyren::tr("Private playlist"), {.size = t.fs(14), .color = t.text});
            ui.label({r.x, y + 22, r.w - 60, 18}, lyren::tr("Only you can see it"), {.size = t.fs(12), .color = t.text_dim});
            ui.toggle(ui.id("playlist-hidden"), {r.right() - 40, y + 9, 40, 22}, edit_hidden_);
        }
        float ok_w = std::max(96.f, button_width(ui, create ? lyren::tr("Create") : lyren::tr("Save changes"), false));
        float cancel_w = std::max(84.f, button_width(ui, lyren::tr("Cancel"), false));
        Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
        if (ui.button(ui.id("playlist-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
            ui.close_modal();
        }
        const bool busy = library(edit_source_).playlist_writes.contains(card.id);
        ui.disabled = busy;
        {
            Fade dim(ui.painter(), busy ? 0.4f : 1.f);
            submit |= ui.button(ui.id("playlist-save"), ok, create ? lyren::tr("Create") : lyren::tr("Save changes"), ui::ButtonKind::primary);
        }
        ui.disabled = false;
        std::string name = edit_name_;
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        if (!submit || name.empty() || busy) {
            return;
        }
        ui.close_modal();
        if (create) {
            spawn(create_playlist(edit_source_, std::move(name), edit_hidden_, edit_tracks_));
            return;
        }
        if (name == card.title) {
            return;
        }
        spawn(rename_playlist(std::move(card), std::move(name)));
    }

    void Shell::confirm_delete(ui::Ui &ui, Card card) {
        ui.open_modal(400, 196, [this, card](ui::Ui &u, Rect r) {
            const auto &t = u.theme();
            u.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Delete playlist"), {.size = t.fs(20), .color = t.text});
            u.label({r.x, r.y + 48, r.w, 20}, lyren::trf("{} will be permanently deleted.", card.title), {.size = t.fs(13), .color = t.text_dim});
            float ok_w = std::max(96.f, button_width(u, lyren::tr("Delete"), true)), cancel_w = std::max(84.f, button_width(u, lyren::tr("Cancel"), false));
            Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
            if (u.button(u.id("delete-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
                u.close_modal();
            }
            const bool busy = library(card.source).playlist_writes.contains(card.id);
            u.disabled = busy;
            bool submit;
            {
                Fade dim(u.painter(), busy ? 0.4f : 1.f);
                submit = u.button(u.id("delete-ok"), ok, lyren::tr("Delete"), ui::ButtonKind::primary, "trash");
            }
            u.disabled = false;
            if (!submit) {
                return;
            }
            u.close_modal();
            if (route_.page == Page::playlist && same(route_.card, card)) {
                back(u);
            }
            spawn(delete_playlist(card));
        });
    }

    void Shell::open_picker(ui::Ui &ui, std::vector<Track> tracks) {
        const Source source = tracks.front().source;
        if (!provider(source).logged_in()) {
            return ui.toast(lyren::trf("Sign in to {} to add songs to a playlist", lyren::tr(platform(source).name)), "folder-plus");
        }
        ui.open_modal(420, 480, [this, tracks = std::move(tracks)](ui::Ui &u, Rect r) {
            picker_modal(u, r, tracks);
        });
    }

    void Shell::picker_modal(ui::Ui &ui, Rect r, const std::vector<Track> &tracks) {
        auto &p = ui.painter();
        const auto &th = ui.theme();
        const Source source = tracks.front().source;
        ui.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Add to playlist"), {.size = th.fs(20), .color = th.text});
        const std::string subtitle =
            tracks.size() == 1 ? tracks.front().title + " · " + tracks.front().artist : lyren::trf("{} songs · {}", tracks.size(), lyren::tr(platform(source).name));
        ui.label({r.x, r.y + 32, r.w - 40, 20}, subtitle, {.size = th.fs(13), .color = th.text_dim});
        std::vector<const Card *> own;
        for (const Card &c : library(source).playlists) {
            if (c.owned && provider(c).logged_in()) {
                own.push_back(&c);
            }
        }
        constexpr float row_h = 56, top = 68;
        float list_h = static_cast<float>(own.size() + 1) * row_h;
        ui.resize_modal(420, std::min(ui.height - 120, top + list_h + 48));
        Rect view{r.x - 8, r.y + top, r.w + 24, r.h - top};
        float off = ui.begin_scroll(ui.id("picker"), view, list_h);
        auto entry = [&](size_t i, std::string_view title, std::string_view sub, const std::string *art, bool busy) {
            Rect e{view.x, view.y + static_cast<float>(i) * row_h - off, r.w + 16, row_h};
            ui::Id id = ui.id(static_cast<uint64_t>(i));
            ui.disabled = busy;
            auto it = ui.interact(id, e);
            ui.disabled = false;
            Fade dim(p, busy ? 0.45f : 1.f);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            if (hot > 0) {
                p.rect(e.inset(2), th.hover.with_alpha(hot), th.radius_s());
            }
            Rect cov{e.x + 10, e.y + 8, 40, 40};
            if (art) {
                cover(ui, cov, *art, th.radius_s(), "list-music");
            } else {
                p.rect(cov, th.text.with_alpha(0.07f), th.radius_s());
                p.icon("plus", ui::center(cov, 18, 18), th.text_dim);
            }
            p.text(cov.right() + 14, e.y + (sub.empty() ? 34 : 25), title, {.size = th.fs(14), .color = th.text}, e.w - 80);
            if (!sub.empty()) {
                p.text(cov.right() + 14, e.y + 43, sub, {.size = th.fs(12), .color = th.text_dim}, e.w - 80);
            }
            return it.clicked;
        };
        std::optional<Card> picked;
        ui.push_id(ui.id("picker-rows"));
        bool create = entry(0, lyren::tr("New playlist"), "", nullptr, library(source).playlist_writes.contains(""));
        for (size_t i = 0; i < own.size(); ++i) {
            if (entry(i + 1, own[i]->title, card_subtitle(*own[i]), &own[i]->cover, library(source).playlist_writes.contains(own[i]->id))) {
                picked = *own[i];
            }
        }
        ui.pop_id();
        ui.end_scroll();
        if (create) {
            open_editor(ui, {}, tracks);
        }
        if (picked) {
            ui.close_modal();
            add_to_playlist(tracks, std::move(*picked));
        }
    }

} // namespace lyren
