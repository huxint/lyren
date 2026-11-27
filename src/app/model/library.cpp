#include "app/model/library.hpp"

#include <algorithm>
#include <ranges>
#include <span>
#include <unordered_map>

#include "core/i18n.hpp"

namespace lyren {

    namespace {
        // Adds a missing item or takes it out; server metadata and ordering win for existing items.
        void place(std::vector<Card> &items, const Card &item, bool on) {
            auto it = std::ranges::find_if(items, [&](const Card &x) {
                return same(x, item);
            });
            if (on && it == items.end()) {
                items.insert(items.begin(), item);
            } else if (!on && it != items.end()) {
                items.erase(it);
            }
        }
    } // namespace

    Library::Library(Env &env)
    : env_(env) {}

    void Library::emit(const Event &event) {
        for (auto &listener : listeners_) {
            listener(event);
        }
    }

    Task<void> Library::load(Source source) {
        Provider &p = env_.provider(source);
        if (!env_.signed_in(source)) {
            co_return;
        }
        Shelf &lib = shelf(source);
        uint64_t gen = ++lib.gen;
        lib.error.clear();
        std::string uid;
        if (p.capabilities.accounts) {
            auto a = co_await p.account();
            if (gen != lib.gen) {
                co_return;
            }
            if (!a) {
                lib.error = a.error();
                env_.notify(a.error(), "user", true);
                if (!p.logged_in()) {
                    // The session turned out to be dead; whoever keeps the credentials hears of it.
                    emit({.kind = Event::Kind::signed_out, .source = source});
                }
                co_return;
            }
            const bool account_changed = !lib.account || lib.account->id != a->id;
            if (account_changed && lib.account) {
                reset(source);
                gen = lib.gen;
            }
            lib.account = *a;
            uid = a->id;
            env_.prefetch(a->avatar, 64);
            if (account_changed) {
                emit({.kind = Event::Kind::account, .source = source});
            }
            env_.redraw();
        }
        if (p.features.daily.playlists) {
            spawn(load_daily_lists(source));
        }
        if (p.capabilities.library) {
            const uint64_t revision = lib.preference_gen, playlist_gen = ++lib.playlist_gen;
            auto playlists = co_await p.user_playlists(uid);
            if (gen != lib.gen) {
                co_return;
            }
            if (playlist_gen == lib.playlist_gen) {
                if (playlists) {
                    apply_playlists(source, std::move(*playlists), revision);
                } else {
                    lib.error = playlists.error();
                    env_.notify(playlists.error(), "list-music", true);
                }
            }
        }
        if (p.capabilities.likes) {
            const uint64_t revision = lib.preference_gen;
            auto ids = co_await p.liked_ids(uid);
            if (gen != lib.gen) {
                co_return;
            }
            if (ids) {
                lib.liked_ids = {ids->begin(), ids->end()};
                reconcile_all(source, Preference::song, revision);
            } else {
                env_.notify(ids.error(), "heart", true);
            }
        }
        // Pages that waited for the account load now.
        emit({.kind = Event::Kind::loaded, .source = source});
        env_.redraw();
    }

    void Library::reset(Source source) {
        Shelf &lib = shelf(source);
        Shelf fresh;
        // Requests under way belong to the account that left.
        fresh.gen = lib.gen + 1;
        fresh.account_gen = lib.account_gen + 1;
        fresh.playlist_gen = lib.playlist_gen + 1;
        fresh.collection_gen = lib.collection_gen + 1;
        fresh.square_gen = lib.square_gen + 1;
        fresh.radio_gen = lib.radio_gen + 1;
        for (size_t tab = 0; tab < fresh.release_gen.size(); ++tab) {
            fresh.release_gen[tab] = lib.release_gen[tab] + 1;
        }
        // What does not depend on who is signed in stays.
        fresh.categories = std::move(lib.categories);
        fresh.category = std::move(lib.category);
        fresh.release_category = lib.release_category;
        fresh.release_tab = lib.release_tab, fresh.recent_tab = lib.recent_tab;
        fresh.collection_tab = lib.collection_tab, fresh.dislike_tab = lib.dislike_tab;
        lib = std::move(fresh);
        emit({.kind = Event::Kind::left, .source = source});
        env_.redraw();
    }

    Task<void> Library::sign_out(Source source) {
        reset(source);
        co_await env_.provider(source).logout();
        emit({.kind = Event::Kind::signed_out, .source = source});
        env_.notify(lyren::trf("Signed out of {}", lyren::tr(platform(source).name)), "log-out");
    }

    std::optional<bool> Library::kept(Source source, Preference kind, std::string_view id, std::optional<bool> reported) const {
        const Shelf &lib = shelf(source);
        const auto &edits = lib.preferences[static_cast<size_t>(kind)];
        if (auto it = edits.find(id); it != edits.end()) {
            return it->second.value;
        }
        if (kind == Preference::album || kind == Preference::artist) {
            const auto &caps = env_.provider(source).capabilities;
            const bool supported = kind == Preference::artist ? caps.follow_artists : caps.collect_albums;
            const auto &collection = lib.collection[kind == Preference::artist ? 1 : 0];
            if (supported && collection.loaded) {
                return std::ranges::any_of(collection.cards, [&](const Card &card) {
                    return card.id == id;
                });
            }
            if (!env_.signed_in(source)) {
                return false;
            }
        }
        return reported;
    }

    void Library::apply(Source source, Preference kind, const Edit &edit) {
        Shelf &lib = shelf(source);
        const bool on = edit.value;
        if (kind == Preference::song) {
            if (on) {
                lib.liked_ids.insert(edit.track.id);
            } else {
                lib.liked_ids.erase(edit.track.id);
            }
            emit({.kind = Event::Kind::liked, .source = source, .track = &edit.track, .on = on});
        } else if (kind == Preference::playlist) {
            place(lib.playlists, edit.card, on);
        } else if (kind == Preference::user) {
            // Follow buttons and counts read the edit itself.
        } else {
            auto &collection = lib.collection[kind == Preference::artist ? 1 : 0];
            if (collection.loaded) {
                place(collection.cards, edit.card, on);
                collection.total = static_cast<int>(collection.cards.size());
            }
        }
    }

    void Library::set(Source source, Preference kind, Track track, Card card, bool on) {
        Shelf &lib = shelf(source);
        auto &[id, edit] = *lib.preferences[static_cast<size_t>(kind)].try_emplace(kind == Preference::song ? track.id : card.id, Edit{.confirmed = !on}).first;
        edit.track = std::move(track);
        edit.card = std::move(card);
        edit.value = on;
        edit.revision = ++lib.preference_gen;
        apply(source, kind, edit);
        if (!edit.pending) {
            edit.pending = true;
            spawn(write(source, kind, id, lib.account_gen));
        }
        env_.redraw();
    }

    Task<void> Library::write(Source source, Preference kind, std::string id, uint64_t account_gen) {
        Shelf &lib = shelf(source);
        Provider &provider = env_.provider(source);
        for (;;) {
            // The edit is looked up after every wait: a reset in between takes the map with it.
            auto &edit = lib.preferences[static_cast<size_t>(kind)].at(id);
            const bool on = edit.value;
            const uint64_t revision = edit.revision;
            Result<void> result;
            switch (kind) {
                case Preference::song:
                    result = co_await provider.like(edit.track, on);
                    break;
                case Preference::artist:
                    result = co_await provider.follow(edit.card.id, on);
                    break;
                case Preference::album:
                    result = co_await provider.collect(edit.card.id, on);
                    break;
                case Preference::playlist:
                    result = co_await provider.collect_playlist(edit.card.id, on);
                    break;
                case Preference::user:
                    result = co_await provider.follow_user(edit.card.id, on);
                    break;
            }
            if (account_gen != lib.account_gen) {
                co_return;
            }
            auto &now = lib.preferences[static_cast<size_t>(kind)].at(id);
            const bool changed = now.value != on || (!result && now.revision != revision);
            now.revision = ++lib.preference_gen;
            if (result) {
                now.confirmed = on;
            } else if (!changed) {
                now.value = now.confirmed;
                apply(source, kind, now);
                env_.notify(lyren::trf("Action failed: {}", result.error()), "triangle-alert", true);
            }
            if (!changed) {
                now.pending = false;
                break;
            }
            // A newer click still needs its own write, even if the previous response was lost.
        }
        env_.redraw();
        if (kind == Preference::playlist) {
            const uint64_t revision = lib.preference_gen;
            auto favorites = co_await provider.collected_playlists();
            if (account_gen != lib.account_gen) {
                co_return;
            }
            if (favorites) {
                std::erase_if(lib.playlists, [](const Card &card) {
                    return !card.owned;
                });
                lib.playlists.append_range(std::views::as_rvalue(*favorites));
                reconcile_all(source, kind, revision);
                emit({.kind = Event::Kind::playlists, .source = source});
            } else {
                env_.notify(favorites.error(), "list-music", true);
            }
            env_.redraw();
        } else if (kind == Preference::song && provider.reload_after_edit) {
            emit({.kind = Event::Kind::liked_stale, .source = source});
        }
    }

    bool Library::reconcile(Source source, Preference kind, std::string_view id, bool value, uint64_t revision) {
        auto &edits = shelf(source).preferences[static_cast<size_t>(kind)];
        auto it = edits.find(id);
        if (it == edits.end()) {
            return value;
        }
        auto &edit = it->second;
        if (!edit.pending && edit.revision <= revision) {
            edit.value = edit.confirmed = value;
        }
        // Keep the revision so a read begun before the write cannot overwrite a later readback.
        apply(source, kind, edit);
        return edit.value;
    }

    void Library::reconcile_all(Source source, Preference kind, uint64_t revision) {
        Shelf &lib = shelf(source);
        for (const auto &[id, edit] : lib.preferences[static_cast<size_t>(kind)]) {
            bool value;
            if (kind == Preference::song) {
                value = lib.liked_ids.contains(id);
            } else {
                const auto &cards = kind == Preference::playlist ? lib.playlists : lib.collection[kind == Preference::artist ? 1 : 0].cards;
                value = std::ranges::any_of(cards, [&](const Card &card) {
                    return card.id == id;
                });
            }
            reconcile(source, kind, id, value, revision);
        }
    }

    void Library::set_liked(Source source, const std::vector<Track> &tracks, uint64_t revision) {
        Shelf &lib = shelf(source);
        lib.liked_ids.clear();
        for (const auto &track : tracks) {
            lib.liked_ids.insert(track.id);
        }
        reconcile_all(source, Preference::song, revision);
    }

    void Library::toggle_like(Track track) {
        if (!env_.provider(track).logged_in()) {
            return env_.notify(lyren::trf("Sign in to {} to like this song", lyren::tr(platform(track.source).name)), "heart");
        }
        const bool on = !liked(track);
        if (on) {
            pop_source_ = track.source;
            pop_id_ = track.id;
            pop_time_ = env_.now();
        }
        env_.notify(std::string(lyren::tr(on ? "Added to liked songs" : "Removed from liked songs")), on ? "heart-fill" : "heart-off");
        const Source source = track.source;
        set(source, Preference::song, std::move(track), {}, on);
    }

    void Library::set_followed(Card artist, bool on) {
        if (!env_.provider(artist).logged_in()) {
            return env_.notify(std::string(lyren::tr("Sign in to this artist's platform to follow them")), "user-plus");
        }
        env_.notify(on ? lyren::trf("Following {}", artist.title) : std::string(lyren::tr("Unfollowed artist")), on ? "user-check" : "user");
        const Source source = artist.source;
        set(source, Preference::artist, {}, std::move(artist), on);
    }

    void Library::set_collected(Card album, bool on) {
        if (!env_.provider(album).logged_in()) {
            return env_.notify(std::string(lyren::tr("Sign in to this album's platform to save it")), "star");
        }
        env_.notify(std::string(lyren::tr(on ? "Album saved" : "Album removed")), on ? "star-fill" : "star");
        const Source source = album.source;
        set(source, Preference::album, {}, std::move(album), on);
    }

    void Library::toggle_collect_playlist(Card playlist) {
        if (!env_.provider(playlist).logged_in()) {
            return env_.notify(std::string(lyren::tr("Sign in to this playlist's platform to save it")), "star");
        }
        const bool on = !std::ranges::any_of(shelf(playlist.source).playlists, [&](const Card &card) {
            return same(card, playlist);
        });
        env_.notify(std::string(lyren::tr(on ? "Playlist saved" : "Playlist removed")), on ? "star-fill" : "star");
        const Source source = playlist.source;
        set(source, Preference::playlist, {}, std::move(playlist), on);
    }

    void Library::toggle_follow_user(Source source, const Person &person) {
        if (!env_.provider(source).logged_in()) {
            return env_.notify(lyren::trf("Sign in to {} to follow people", lyren::tr(platform(source).name)), "user-plus");
        }
        const bool on = !following(source, person);
        env_.notify(lyren::trf(on ? "Following {}" : "Unfollowed {}", person.name), on ? "user-check" : "user-minus");
        set(source, Preference::user, {}, {.kind = CardKind::user, .source = source, .id = person.id, .title = person.name, .cover = person.avatar}, on);
    }

    void Library::apply_playlists(Source source, std::pair<Card, std::vector<Card>> playlists, uint64_t revision) {
        Shelf &lib = shelf(source);
        lib.liked = std::move(playlists.first);
        lib.playlists = std::move(playlists.second);
        lib.playlists_loaded = true;
        lib.error.clear();
        reconcile_all(source, Preference::playlist, revision);
        env_.prefetch(lib.playlists);
        emit({.kind = Event::Kind::playlists, .source = source});
    }

    Task<bool> Library::refresh_playlists(Source source, uint64_t account_gen) {
        Shelf &lib = shelf(source);
        if (!lib.account) {
            co_return false;
        }
        const uint64_t generation = ++lib.playlist_gen, revision = lib.preference_gen;
        auto playlists = co_await env_.provider(source).user_playlists(lib.account->id);
        if (account_gen != lib.account_gen || generation != lib.playlist_gen) {
            co_return false;
        }
        if (!playlists) {
            lib.error = playlists.error();
            env_.notify(lyren::trf("Could not confirm playlist changes: {}", lyren::tr(playlists.error())), "triangle-alert", true);
            co_return false;
        }
        apply_playlists(source, std::move(*playlists), revision);
        env_.redraw();
        co_return true;
    }

    bool Library::begin_write(const Card &playlist) {
        Shelf &lib = shelf(playlist.source);
        if (!env_.provider(playlist).logged_in() || !lib.account) {
            env_.notify(lyren::trf("Sign in to {} to edit playlists", lyren::tr(platform(playlist.source).name)), "user");
            return false;
        }
        if (!lib.playlist_writes.insert(playlist.id).second) {
            env_.notify(std::string(lyren::tr("The playlist is still saving")), "list-music");
            return false;
        }
        env_.redraw();
        return true;
    }

    void Library::finish_write(const Card &playlist) {
        shelf(playlist.source).playlist_writes.erase(playlist.id);
        env_.redraw();
    }

    Task<void> Library::create_playlist(Source source, std::string name, bool hidden, std::vector<Track> tracks) {
        Card pending{.source = source};
        if (!begin_write(pending)) {
            co_return;
        }
        Shelf &lib = shelf(source);
        const uint64_t account_gen = lib.account_gen;
        auto created = co_await env_.provider(source).create_playlist(name, hidden);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        co_await refresh_playlists(source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_write(pending);
        if (!created) {
            // Without the returned ID, a same-name playlist is not proof of this creation.
            co_return env_.notify(lyren::trf("Could not confirm playlist creation: {}", lyren::tr(created.error())), "triangle-alert", true);
        }
        place(lib.playlists, *created, true);
        emit({.kind = Event::Kind::playlists, .source = source});
        env_.notify(lyren::trf("Created playlist {}", created->title), "circle-check");
        if (!tracks.empty()) {
            spawn(edit_playlist(std::move(*created), std::move(tracks), true));
        }
    }

    Task<void> Library::rename_playlist(Card playlist, std::string name) {
        if (!begin_write(playlist)) {
            co_return;
        }
        Shelf &lib = shelf(playlist.source);
        const uint64_t account_gen = lib.account_gen;
        auto result = co_await env_.provider(playlist).rename_playlist(playlist.id, name);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        const bool refreshed = co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_write(playlist);
        if (!refreshed) {
            co_return;
        }
        auto it = std::ranges::find_if(lib.playlists, [&](const Card &card) {
            return same(card, playlist);
        });
        if (it != lib.playlists.end() && it->title == name) {
            env_.notify(std::string(lyren::tr("Playlist renamed")), "pencil");
        } else {
            env_.notify(result ? std::string(lyren::tr("Playlist name was not saved")) : lyren::trf("Could not rename: {}", lyren::tr(result.error())), "triangle-alert", true);
        }
    }

    Task<void> Library::delete_playlist(Card playlist) {
        if (!begin_write(playlist)) {
            co_return;
        }
        Shelf &lib = shelf(playlist.source);
        const uint64_t account_gen = lib.account_gen;
        auto result = co_await env_.provider(playlist).delete_playlist(playlist.id);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        const bool refreshed = co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_write(playlist);
        if (!refreshed) {
            co_return;
        }
        if (std::ranges::none_of(lib.playlists, [&](const Card &card) {
                return same(card, playlist);
            })) {
            emit({.kind = Event::Kind::playlist_deleted, .source = playlist.source, .playlist = &playlist});
            env_.notify(lyren::trf("Deleted playlist {}", playlist.title), "trash");
        } else {
            env_.notify(result ? std::string(lyren::tr("Playlist was not deleted")) : lyren::trf("Could not delete playlist: {}", lyren::tr(result.error())), "triangle-alert",
                        true);
        }
    }

    Task<void> Library::edit_playlist(Card playlist, std::vector<Track> tracks, bool add, uint64_t tag) {
        if (!begin_write(playlist)) {
            co_return;
        }
        Shelf &lib = shelf(playlist.source);
        Provider &provider = env_.provider(playlist);
        const uint64_t account_gen = lib.account_gen;
        Result<void> result;
        for (auto batch : std::span(tracks) | std::views::chunk(100)) {
            result = co_await provider.edit_playlist(playlist.id, std::vector<Track>(batch.begin(), batch.end()), add);
            if (account_gen != lib.account_gen) {
                co_return;
            }
            if (!result) {
                break;
            }
        }
        // Even a lost or rejected batch can have applied some songs; the service's list decides the outcome.
        auto fresh = co_await provider.tracks(playlist);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        if (!fresh) {
            finish_write(playlist);
            emit({.kind = Event::Kind::tracks_failed, .source = playlist.source, .playlist = &playlist, .error = fresh.error()});
            co_return env_.notify(lyren::trf("Could not confirm playlist changes: {}", lyren::tr(fresh.error())), "triangle-alert", true);
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
                confirmed += (it != actual.end() && it->second == track.type) == add;
            }
        }
        for (auto &card : lib.playlists) {
            if (same(card, playlist)) {
                card.count = static_cast<int>(fresh->size());
            }
        }
        emit({.kind = Event::Kind::tracks, .source = playlist.source, .playlist = &playlist, .fresh = &*fresh, .requested = &tracks, .on = add, .tag = tag});
        co_await refresh_playlists(playlist.source, account_gen);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        finish_write(playlist);
        const bool complete = confirmed == tracks.size();
        std::string message = lyren::trf(add ? "Added {} of {} songs to {}" : "Removed {} of {} songs from {}", confirmed, tracks.size(), playlist.title);
        if (!complete && !result) {
            message += ": " + std::string(lyren::tr(result.error()));
        }
        env_.notify(std::move(message), add ? "folder-plus" : "list-minus", !complete);
    }

    Task<void> Library::load_dislikes(Source source, DislikeKind kind) {
        auto &cached = shelf(source).dislikes[static_cast<size_t>(kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        const uint64_t gen = ++list->gen;
        list->begin();
        env_.redraw();
        auto r = co_await env_.provider(source).features.exclusions.list(kind);
        if (gen != list->gen || shelf(source).dislikes[static_cast<size_t>(kind)] != list) {
            co_return;
        }
        if (r) {
            list->items = std::move(*r);
            list->succeed(env_.now(), true);
        } else {
            list->fail(r.error(), env_.now());
        }
        env_.redraw();
    }

    void Library::set_dislike(Dislike item, bool on) {
        if (!env_.provider(item).logged_in()) {
            return env_.notify(lyren::trf("Sign in to {} to change recommendations", lyren::tr(platform(item.source).name)), "user");
        }
        auto &cached = shelf(item.source).dislikes[static_cast<size_t>(item.kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        if (!list->pending.insert(item.id).second) {
            return;
        }
        env_.redraw();
        spawn([](Library *self, Dislike item, bool on, std::shared_ptr<DislikeList> list) -> Task<void> {
            auto r = co_await self->env_.provider(item).features.exclusions.set(item, on);
            list->pending.erase(item.id);
            if (self->shelf(item.source).dislikes[static_cast<size_t>(item.kind)] != list) {
                co_return;
            }
            if (!r) {
                co_return self->env_.notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->env_.notify(std::string(lyren::tr(on ? "Hidden from recommendations" : "Restored to recommendations")), on ? "thumbs-down" : "check");
            co_await self->load_dislikes(item.source, item.kind);
        }(this, std::move(item), on, std::move(list)));
    }

} // namespace lyren
