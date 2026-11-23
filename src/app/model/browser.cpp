#include "app/model/browser.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <unordered_map>

#include "core/i18n.hpp"

namespace lyren {

    std::vector<Track> Browser::Selection::of(const std::vector<Track> &tracks) const {
        std::vector<Track> picked;
        picked.reserve(chosen.size());
        for (const auto &track : tracks) {
            if (chosen.contains(track.id)) {
                picked.push_back(track);
            }
        }
        return picked;
    }

    Browser::Browser(Env &env, Library &library, Social &social, Source active)
    : env_(env),
      library_(library),
      social_(social),
      active_(active) {
        route_.card.source = active;
        route_.serial = ++serial_;
        const std::time_t now = std::time(nullptr);
        std::tm local;
        localtime_r(&now, &local);
        today_ = std::format("{:02}-{:02}", local.tm_mon + 1, local.tm_mday);
        library.subscribe([this](const Library::Event &event) {
            on_library(event);
        });
    }

    template <class F>
    void Browser::each_route(F &&visit) {
        visit(route_);
        for (auto &route : history_) {
            visit(route);
        }
        for (auto &route : forward_) {
            visit(route);
        }
    }

    bool Browser::supports(Page page, Source source) const {
        const auto &p = env_.provider(source);
        switch (page) {
            case Page::toplist:
                return p.capabilities.toplists;
            case Page::square:
                return p.capabilities.square;
            case Page::fm:
                return p.capabilities.radio;
            case Page::liked:
                return p.capabilities.likes;
            case Page::collection:
                return p.capabilities.collect_albums || p.capabilities.follow_artists;
            case Page::releases:
                return !p.release_categories[0].empty() || !p.release_categories[1].empty();
            case Page::daily:
                return bool(p.features.daily.songs);
            case Page::radar:
                return bool(p.features.radar);
            case Page::cloud:
                return bool(p.features.cloud);
            case Page::dislikes:
                return bool(p.features.exclusions.list);
            default:
                return true;
        }
    }

    bool Browser::needs_login(Page page) {
        return page == Page::daily || page == Page::fm || page == Page::liked || page == Page::cloud || page == Page::collection || page == Page::dislikes;
    }

    void Browser::open(Page page) {
        open(page, {.source = active_});
    }

    void Browser::open(Page page, const Card &card) {
        if (route_.page == page && same(route_.card, card) && page != Page::search) {
            // Already there, which still counts as going there: whatever covers the page steps aside.
            ++navigations_;
            return env_.redraw();
        }
        forward_.clear();
        history_.push_back(route_);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        // Older pages reload if they are visited again.
        if (history_.size() > 8) {
            Route &old = history_[history_.size() - 9];
            old.list.reset();
            old.artist.reset();
            old.profile.reset();
            if (old.search) {
                old.search->results = {};
            }
        }
        Card target = card;
        if (page == Page::playlist) {
            const auto &playlists = library_.shelf(card.source).playlists;
            if (std::ranges::any_of(playlists, [&](const Card &mine) {
                    return same(mine, card) && mine.owned;
                })) {
                target.owned = true;
            }
        }
        enter({.page = page, .card = std::move(target)}, 1);
    }

    void Browser::open(const Card &card) {
        if (card.kind == CardKind::user) {
            return open_profile(card.source, card.id, card.title, card.cover);
        }
        open(card.kind == CardKind::album ? Page::album : card.kind == CardKind::artist ? Page::artist : Page::playlist, card);
    }

    void Browser::open_profile(Source source, std::string id, std::string name, std::string avatar) {
        if (id.empty() || !env_.provider(source).capabilities.profiles) {
            return;
        }
        Card card{.kind = CardKind::user, .source = source, .id = std::move(id), .title = std::move(name), .cover = std::move(avatar)};
        card.owned = library_.own(card);
        open(Page::profile, card);
    }

    bool Browser::open_own_profile() {
        const auto &account = shelf().account;
        if (!account) {
            return false;
        }
        open(Page::profile, {.kind = CardKind::user, .source = active_, .id = account->id, .title = account->name, .cover = account->avatar, .owned = true});
        return true;
    }

    bool Browser::back() {
        if (history_.empty()) {
            return false;
        }
        forward_.push_back(route_);
        if (forward_.size() > 50) {
            forward_.erase(forward_.begin());
        }
        Route route = std::move(history_.back());
        history_.pop_back();
        enter(std::move(route), -1);
        return true;
    }

    bool Browser::forward() {
        if (forward_.empty()) {
            return false;
        }
        history_.push_back(route_);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        Route route = std::move(forward_.back());
        forward_.pop_back();
        enter(std::move(route), 1);
        return true;
    }

    void Browser::enter(Route route, int from) {
        route_ = std::move(route);
        if (!route_.serial) {
            route_.serial = ++serial_;
        }
        const bool platform_changed = active_ != route_.card.source;
        if (platform_changed) {
            active_ = route_.card.source;
            if (on_change) {
                on_change();
            }
        }
        if (route_.page == Page::search && route_.search) {
            query = route_.search->query;
            if (!env_.provider(active_).capabilities.search[static_cast<size_t>(route_.search->tab)]) {
                route_.search->tab = 0;
            }
        }
        entered = env_.now();
        direction = from;
        ++navigations_;
        reset_selection();
        switch (route_.page) {
            case Page::daily:
                route_.card = {.source = active_, .title = "Daily mix"};
                break;
            case Page::radar:
                route_.card = {.source = active_, .title = "Radar", .subtitle = "Favorites and new discoveries"};
                break;
            case Page::cloud:
                route_.card = {.source = active_, .title = "My cloud", .subtitle = "Songs uploaded to your cloud"};
                break;
            case Page::liked:
                route_.card = {.source = active_, .title = "Liked songs"};
                break;
            case Page::recent:
                route_.card = {.source = active_, .title = "Recently played"};
                break;
            default:
                break;
        }
        if (needs_login(route_.page) && !env_.signed_in(active_)) {
            route_.list.reset();
        }
        ensure();
        if (platform_changed) {
            spawn(warm(active_));
        }
        env_.redraw();
    }

    void Browser::switch_platform(Source source) {
        if (source == active_) {
            return;
        }
        // Search keeps each platform's cached results and scroll positions; the settings are everyone's.
        if (route_.page == Page::search || route_.page == Page::settings) {
            tab_direction = source > active_ ? 1 : -1;
            active_ = route_.card.source = source;
            if (route_.search && !env_.provider(active_).capabilities.search[static_cast<size_t>(route_.search->tab)]) {
                route_.search->tab = 0;
            }
            reset_selection();
            tab_since = env_.now();
            ++navigations_;
            ensure();
            spawn(warm(source));
            if (on_change) {
                on_change();
            }
            return env_.redraw();
        }
        Page page = route_.page;
        if (page == Page::playlist || page == Page::album || page == Page::artist || page == Page::similar || page == Page::profile || !supports(page, source)) {
            page = Page::recommend;
        }
        open(page, {.source = source});
    }

    void Browser::ensure() {
        const Page page = route_.page;
        auto &lib = shelf();
        const bool signed_in = env_.signed_in(active_);
        if (page == Page::search && route_.search && !results(route_.search->tab).loaded) {
            load_results(route_.search->tab);
        }
        const bool listed = page == Page::daily || page == Page::radar || page == Page::cloud || page == Page::liked || page == Page::playlist || page == Page::album ||
                            page == Page::similar || (page == Page::recent && lib.recent_tab == 1 && signed_in);
        // The liked songs wait for the account's playlists, which name the list behind them.
        if (listed && !route_.list && !(needs_login(page) && !signed_in) && !(page == Page::liked && !lib.playlists_loaded)) {
            route_.list = std::make_shared<TrackList>();
            spawn(load_list(route_.list, page, route_.card));
        }
        if (page == Page::artist && !route_.artist) {
            route_.artist = std::make_shared<Artist>();
            spawn(load_artist(route_.artist, route_.card));
        }
        if (page == Page::profile && !route_.profile && !(route_.card.owned && !signed_in)) {
            route_.profile = social_.open(route_.card);
        }
        if (page == Page::recommend && !lib.recommended.loaded && !lib.recommended.loading) {
            spawn(library_.load_recommended(active_));
        }
        if (page == Page::toplist && !lib.toplists.loaded && !lib.toplists.loading) {
            spawn(library_.load_toplists(active_));
        }
        if (page == Page::square) {
            if (lib.categories.groups.empty() && !lib.categories_loading) {
                spawn(library_.load_categories(active_));
            }
            if (!lib.square.loaded) {
                library_.load_square(active_);
            }
        }
        if (page == Page::fm && lib.radio.empty() && !lib.radio_loading && signed_in) {
            spawn(library_.load_radio(active_));
        }
        if (page == Page::releases && !lib.releases[static_cast<size_t>(lib.release_tab)].loaded) {
            library_.load_releases(active_);
        }
        if (page == Page::dislikes && signed_in) {
            const auto &list = lib.dislikes[static_cast<size_t>(lib.dislike_tab)];
            if (!list || (!list->loaded && !list->loading)) {
                spawn(library_.load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        // Collections can change elsewhere, so they reload whenever the page is entered.
        if (page == Page::collection && signed_in && !lib.collection[0].loading) {
            spawn(library_.load_collection(active_));
        }
    }

    Task<void> Browser::warm(Source source) {
        // Visible work starts first; unvisited platforms must not fill the image budget at startup.
        co_await lyren::sleep{env_.loop, 200ms};
        auto &lib = library_.shelf(source);
        const auto &p = env_.provider(source);
        if (source == active_ && !lib.recommended.loaded && !lib.recommended.loading) {
            co_await library_.load_recommended(source);
        }
        if (source == active_ && p.capabilities.toplists && !lib.toplists.loaded && !lib.toplists.loading) {
            co_await library_.load_toplists(source);
        }
        if (source == active_ && p.capabilities.square) {
            if (!lib.square.loaded) {
                library_.load_square(source);
            }
            if (lib.categories.groups.empty() && !lib.categories_loading) {
                co_await library_.load_categories(source);
            }
        }
        if (source == active_ && supports(Page::releases, source) && !lib.releases[static_cast<size_t>(lib.release_tab)].loaded) {
            library_.load_releases(source);
        }
        if (source == active_ && supports(Page::collection, source) && p.logged_in() && !lib.collection[0].loaded && !lib.collection[0].loading) {
            co_await library_.load_collection(source);
        }
    }

    void Browser::search() {
        if (query.empty()) {
            return;
        }
        reset_selection();
        if (route_.page == Page::search && route_.search && route_.search->query == query) {
            route_.search->results = {};
            route_.search->tab = 0;
            route_.search->serial = ++search_gen_;
            ++navigations_;
            load_results(0);
            return env_.redraw();
        }
        forward_.clear();
        history_.push_back(route_);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        if (history_.size() > 8) {
            Route &old = history_[history_.size() - 9];
            old.list.reset();
            old.artist.reset();
            old.profile.reset();
            if (old.search) {
                old.search->results = {};
            }
        }
        auto session = std::make_shared<Search>(Search{.query = query, .serial = ++search_gen_});
        enter({.page = Page::search, .card = {.source = active_}, .search = std::move(session)}, 1);
    }

    void Browser::load_results(int tab, bool retry) {
        if (!route_.search || route_.search->query.empty()) {
            return;
        }
        Listing &listing = results(tab);
        if (!listing.open(retry)) {
            return;
        }
        listing.loading = true;
        listing.error.clear();
        spawn(fetch_results(route_.search, route_.search->serial, tab, active_, listing.next_offset));
    }

    Task<void> Browser::fetch_results(std::shared_ptr<Search> session, uint64_t serial, int tab, Source source, int offset) {
        const auto &lib = library_.shelf(source);
        const uint64_t account_gen = lib.account_gen;
        auto r = co_await env_.provider(source).search(session->query, static_cast<SearchKind>(tab), offset, tab == 0 ? 30 : 40);
        if (serial != session->serial || account_gen != lib.account_gen) {
            co_return;
        }
        Listing &listing = session->results[static_cast<size_t>(source)][static_cast<size_t>(tab)];
        library_.arrived(listing, std::move(r));
        if (!listing.more()) {
            listing.total = static_cast<int>(listing.size());
        }
        env_.redraw();
    }

    Task<void> Browser::load_list(std::shared_ptr<TrackList> list, Page page, Card card) {
        auto &lib = library_.shelf(card.source);
        Provider &provider = env_.provider(card);
        const uint64_t gen = ++list->gen, account_gen = lib.account_gen, revision = lib.preference_gen;
        list->loading = true;
        list->error.clear();
        // Not a ?: chain: with co_await in its branches GCC 16 also starts the tasks of branches not taken.
        Result<std::vector<Track>> r;
        if (page == Page::album) {
            auto album = co_await provider.album(card.id);
            if (gen != list->gen || account_gen != lib.account_gen) {
                co_return;
            }
            if (album) {
                r = std::move(album->tracks);
                list->album = std::move(*album);
                list->album.collected = library_.reconcile(card.source, Library::Preference::album, card.id, list->album.collected, revision);
            } else {
                r = std::unexpected(album.error());
            }
        } else if (page == Page::liked) {
            if (!lib.liked.id.empty()) {
                r = co_await provider.tracks(lib.liked);
            }
        } else if (page == Page::daily) {
            r = co_await provider.features.daily.songs();
        } else if (page == Page::radar) {
            r = co_await provider.features.radar();
        } else if (page == Page::cloud) {
            r = co_await provider.features.cloud();
        } else if (page == Page::recent) {
            r = co_await provider.features.recent_plays();
        } else if (page == Page::similar) {
            r = co_await provider.similar(card.id);
        } else {
            r = co_await provider.tracks(card);
        }
        if (gen != list->gen || account_gen != lib.account_gen) {
            co_return;
        }
        list->loading = false;
        list->loaded = true;
        if (r) {
            if (selection_.list == &list->tracks) {
                reset_selection();
            }
            list->tracks = std::move(*r);
            if (page == Page::liked) {
                library_.set_liked(card.source, list->tracks, revision);
            }
            env_.images.prefetch(list->tracks);
            list->since = env_.now();
        } else {
            list->error = r.error();
        }
        env_.redraw();
    }

    void Browser::reload_list() {
        if (route_.list) {
            spawn(load_list(route_.list, route_.page, route_.card));
        } else {
            ensure();
        }
    }

    void Browser::reload_artist() {
        if (route_.artist) {
            spawn(load_artist(route_.artist, route_.card));
        } else {
            ensure();
        }
    }

    Task<void> Browser::load_artist(std::shared_ptr<Artist> page, Card card) {
        const auto &lib = library_.shelf(card.source);
        const uint64_t account_gen = lib.account_gen, revision = lib.preference_gen;
        page->loading = true;
        page->error.clear();
        if (!page->songs.loaded && !page->songs.loading) {
            page->songs.loading = true;
            page->songs.error.clear();
            spawn(fetch_artist(page, card, 0, 0));
        }
        auto r = co_await env_.provider(card).artist(card.id);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        page->loading = false;
        if (r) {
            page->info = std::move(*r);
            page->info->followed = library_.reconcile(card.source, Library::Preference::artist, card.id, page->info->followed, revision);
        } else {
            page->error = r.error();
        }
        env_.redraw();
    }

    void Browser::load_artist_tab(int tab, bool retry) {
        if (!route_.artist) {
            return;
        }
        Artist &artist = *route_.artist;
        if (retry && tab == 0 && !artist.info && !artist.loading) {
            spawn(load_artist(route_.artist, route_.card));
        }
        Listing &listing = tab == 0 ? artist.songs : tab == 1 ? artist.albums : artist.intro;
        if (!listing.open(retry)) {
            return;
        }
        listing.loading = true;
        listing.error.clear();
        spawn(fetch_artist(route_.artist, route_.card, tab, listing.next_offset));
    }

    Task<void> Browser::fetch_artist(std::shared_ptr<Artist> page, Card card, int tab, int offset) {
        Provider &provider = env_.provider(card);
        if (tab == 2) {
            auto r = co_await provider.artist_intro(card.id);
            page->intro.loading = false;
            page->intro.loaded = true;
            page->intro.next_offset = -1;
            page->intro.since = env_.now();
            if (r) {
                page->sections = std::move(*r);
            } else {
                page->intro.error = r.error();
            }
            co_return env_.redraw();
        }
        Result<Batch> r;
        if (tab == 0) {
            r = co_await provider.artist_songs(card.id, offset);
        } else {
            r = co_await provider.artist_albums(card.id, offset);
        }
        library_.arrived(tab == 0 ? page->songs : page->albums, std::move(r));
        env_.redraw();
    }

    void Browser::dislike_daily(std::shared_ptr<TrackList> list, Track track) {
        auto it = std::ranges::find_if(list->tracks, [&](const Track &x) {
            return same(x, track);
        });
        if (it == list->tracks.end()) {
            return;
        }
        const auto at = static_cast<size_t>(it - list->tracks.begin());
        list->tracks.erase(it);
        if (selection_.list == &list->tracks) {
            reset_selection();
        }
        spawn([](Browser *self, std::shared_ptr<TrackList> list, Track track, size_t at) -> Task<void> {
            auto r = co_await self->env_.provider(track).features.daily.dislike(track.id);
            auto pos = list->tracks.begin() + static_cast<std::ptrdiff_t>(std::min(at, list->tracks.size()));
            if (!r) {
                list->tracks.insert(pos, std::move(track));
                co_return self->env_.notify(lyren::trf("Could not mark as not interested: {}", r.error()), "triangle-alert", true);
            }
            if (*r) {
                list->tracks.insert(pos, std::move(**r));
            }
            self->env_.notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
        }(this, std::move(list), std::move(track), at));
    }

    void Browser::add_to_playlist(std::vector<Track> tracks, Card playlist) {
        // The page on show knows what the playlist already holds.
        if (route_.page == Page::playlist && same(route_.card, playlist) && route_.list) {
            std::unordered_set<std::string_view> existing;
            existing.reserve(route_.list->tracks.size());
            for (const auto &track : route_.list->tracks) {
                existing.insert(track.id);
            }
            std::erase_if(tracks, [&](const Track &track) {
                return existing.contains(track.id);
            });
        }
        if (tracks.empty()) {
            return env_.notify(std::string(lyren::tr("These songs are already in the playlist")), "folder-plus");
        }
        spawn(library_.edit_playlist(std::move(playlist), std::move(tracks), true));
    }

    void Browser::remove_from_playlist(Card playlist, std::vector<Track> tracks) {
        spawn(library_.edit_playlist(std::move(playlist), std::move(tracks), false));
    }

    void Browser::move_track(std::shared_ptr<TrackList> list, Card playlist, int from, int to) {
        if (!list->error.empty()) {
            return env_.notify(std::string(lyren::tr("Refresh this playlist before reordering")), "refresh-cw");
        }
        if (!env_.provider(playlist).logged_in()) {
            return env_.notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(playlist.source).name)), "user");
        }
        if (!library_.begin_write(playlist)) {
            return;
        }
        auto first = list->tracks.begin();
        if (from < to) {
            std::rotate(first + from, first + from + 1, first + to + 1);
        } else {
            std::rotate(first + to, first + from, first + from + 1);
        }
        selection_.selected = to;
        list->saving_order = true;
        env_.redraw();
        auto order = list->tracks;
        spawn(reorder(std::move(list), std::move(playlist), std::move(order), to, library_.shelf(playlist.source).account_gen));
    }

    Task<void> Browser::reorder(std::shared_ptr<TrackList> list, Card playlist, std::vector<Track> order, int selected, uint64_t account_gen) {
        Provider &provider = env_.provider(playlist);
        const auto &lib = library_.shelf(playlist.source);
        auto r = co_await provider.reorder_playlist(playlist.id, order);
        if (account_gen != lib.account_gen) {
            list->saving_order = false;
            co_return env_.redraw();
        }
        // A lost write response may still have committed; read back before reporting the result.
        auto fresh = co_await provider.tracks(playlist);
        list->saving_order = false;
        if (account_gen != lib.account_gen) {
            co_return env_.redraw();
        }
        library_.finish_write(playlist);
        if (!fresh) {
            list->error = fresh.error();
            co_return env_.notify(lyren::trf("Could not refresh playlist order: {}", fresh.error()), "triangle-alert", true);
        }
        const bool confirmed = std::ranges::equal(*fresh, order, [](const Track &a, const Track &b) {
            return same(a, b) && a.type == b.type;
        });
        list->tracks = std::move(*fresh);
        list->error.clear();
        if (route_.list == list) {
            auto it = std::ranges::find(list->tracks, order[static_cast<size_t>(selected)].id, &Track::id);
            selection_.selected = it == list->tracks.end() ? -1 : static_cast<int>(it - list->tracks.begin());
        }
        if (confirmed) {
            env_.notify(std::string(lyren::tr("Playlist order saved")), "list-ordered");
        } else {
            env_.notify(r ? std::string(lyren::tr("Playlist order was not saved")) : lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
        }
        env_.redraw();
    }

    void Browser::delete_playlist(Card playlist) {
        if (route_.page == Page::playlist && same(route_.card, playlist)) {
            back();
        }
        spawn(library_.delete_playlist(std::move(playlist)));
    }

    void Browser::choose(const std::vector<Track> &tracks, int i, bool range) {
        if (route_.page == Page::playlist && route_.list) {
            route_.list->ordering = false;
        }
        auto &s = selection_;
        if (!s.choosing && s.selected >= 0 && s.selected < static_cast<int>(tracks.size())) {
            s.chosen.insert(tracks[static_cast<size_t>(s.selected)].id);
            s.anchor = s.selected;
        }
        s.choosing = true;
        if (range && s.anchor >= 0) {
            for (int at = std::min(i, s.anchor); at <= std::max(i, s.anchor) && at < static_cast<int>(tracks.size()); ++at) {
                s.chosen.insert(tracks[static_cast<size_t>(at)].id);
            }
        } else {
            const auto &id = tracks[static_cast<size_t>(i)].id;
            if (!s.chosen.erase(id)) {
                s.chosen.insert(id);
            }
            s.anchor = i;
        }
        s.selected = i;
        env_.redraw();
    }

    void Browser::choose_all() {
        auto &s = selection_;
        if (!s.list) {
            return;
        }
        if (route_.page == Page::playlist && route_.list) {
            route_.list->ordering = false;
        }
        s.choosing = true;
        s.chosen.clear();
        for (const auto &track : *s.list) {
            s.chosen.insert(track.id);
        }
    }

    void Browser::on_library(const Library::Event &event) {
        using Kind = Library::Event::Kind;
        const Source source = event.source;
        switch (event.kind) {
            case Kind::liked:
                each_route([&](Route &route) {
                    if (route.card.source != source || route.page != Page::liked || !route.list) {
                        return;
                    }
                    auto &tracks = route.list->tracks;
                    auto it = std::ranges::find_if(tracks, [&](const Track &x) {
                        return same(x, *event.track);
                    });
                    if (event.on && it == tracks.end()) {
                        tracks.insert(tracks.begin(), *event.track);
                    } else if (!event.on && it != tracks.end()) {
                        tracks.erase(it);
                    }
                    if (selection_.list == &tracks) {
                        reset_selection();
                    }
                });
                break;
            case Kind::liked_stale:
                if (route_.page == Page::liked && route_.card.source == source && route_.list) {
                    spawn(load_list(route_.list, Page::liked, {.source = source}));
                }
                break;
            case Kind::playlists: {
                const auto &playlists = library_.shelf(source).playlists;
                each_route([&](Route &route) {
                    if (route.page != Page::playlist || route.card.source != source) {
                        return;
                    }
                    auto it = std::ranges::find_if(playlists, [&](const Card &card) {
                        return same(card, route.card);
                    });
                    if (it != playlists.end()) {
                        route.card = *it;
                    }
                });
                break;
            }
            case Kind::loaded:
                // A page that waited for the account loads now.
                if (source == active_) {
                    if (route_.page == Page::liked) {
                        route_.list.reset();
                    }
                    ensure();
                }
                break;
            case Kind::left:
                if (source == active_) {
                    reset_selection();
                }
                each_route([&](Route &route) {
                    if (route.search) {
                        route.search->results[static_cast<size_t>(source)] = {};
                    }
                    if (route.card.source != source) {
                        return;
                    }
                    route.list.reset();
                    route.artist.reset();
                    route.profile.reset();
                    // What was the account's own is nobody's now.
                    if (route.card.owned) {
                        route = {.page = Page::recommend, .card = {.source = source}, .serial = ++serial_};
                    }
                });
                break;
            case Kind::signed_out:
                if (source == active_) {
                    ensure();
                }
                break;
            case Kind::tracks_failed:
                each_route([&](Route &route) {
                    if (route.page == Page::playlist && same(route.card, *event.playlist) && route.list) {
                        route.list->error = event.error;
                    }
                });
                break;
            case Kind::tracks: {
                const Card &playlist = *event.playlist;
                const auto &fresh = *event.fresh;
                // Of a selection that was only partly carried out, what failed stays picked.
                auto &s = selection_;
                const auto &asked = *event.requested;
                if (s.choosing && s.chosen.size() == asked.size() && std::ranges::all_of(asked, [&](const Track &track) {
                        return s.chosen.contains(track.id);
                    })) {
                    std::unordered_map<std::string_view, int> actual;
                    actual.reserve(fresh.size());
                    for (const auto &track : fresh) {
                        actual.emplace(track.id, track.type);
                    }
                    std::vector<std::string_view> done;
                    for (const auto &track : asked) {
                        const auto it = actual.find(track.id);
                        if ((it != actual.end() && it->second == track.type) == event.on) {
                            done.push_back(track.id);
                        }
                    }
                    if (done.size() != asked.size()) {
                        for (auto id : done) {
                            s.chosen.erase(std::string(id));
                        }
                    }
                }
                // Every visit to the playlist shows the same songs.
                std::shared_ptr<TrackList> list;
                each_route([&](Route &route) {
                    if (!list && route.page == Page::playlist && same(route.card, playlist)) {
                        list = route.list;
                    }
                });
                if (!list) {
                    list = std::make_shared<TrackList>();
                    list->since = env_.now();
                }
                ++list->gen;
                list->tracks = fresh;
                list->loading = false;
                list->loaded = true;
                list->error.clear();
                if (s.list == &list->tracks) {
                    s.selected = s.anchor = -1;
                    std::unordered_set<std::string> remaining;
                    for (const auto &track : list->tracks) {
                        if (s.chosen.contains(track.id)) {
                            remaining.insert(track.id);
                        }
                    }
                    s.chosen = std::move(remaining);
                }
                each_route([&](Route &route) {
                    if (route.page == Page::playlist && same(route.card, playlist)) {
                        route.list = list;
                        route.card.count = static_cast<int>(list->tracks.size());
                    }
                });
                break;
            }
            case Kind::account:
            case Kind::radio_ready:
                break;
        }
        env_.redraw();
    }

} // namespace lyren
