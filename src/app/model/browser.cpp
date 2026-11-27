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

    Browser::Browser(Env &env, Library &library, Social &social, Source active, std::string instance)
    : env_(env),
      library_(library),
      social_(social),
      active_{active, std::move(instance)} {
        if (!env_.providers.contains(active_.source, active_.instance) && !env_.providers.empty()) {
            const auto &first = env_.providers.entries().front();
            active_ = {first.source, first.instance};
        }
        route_.card.source = active_.source;
        route_.card.instance = active_.instance;
        route_.serial = ++serial_;
        refresh_clock();
        library.subscribe([this](const Library::Event &event) {
            on_library(event);
        });
    }

    Browser::~Browser() {
        env_.loop.cancel(suggest_timer_);
    }

    void Browser::refresh_clock() {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        today_ = std::format("{:02}-{:02}", local.tm_mon + 1, local.tm_mday);
        // Daily mixes roll over at 00:00 (QQ) and 06:00 (NetEase) local time.
        const int slot = (local.tm_year * 1000 + local.tm_yday) * 2 + (local.tm_hour >= 6);
        if (daily_slot_ >= 0 && slot != daily_slot_) {
            for (const auto &entry : env_.providers.entries()) {
                if (!entry.provider) {
                    continue;
                }
                if (auto it = built_in_lists_.find(entry.id); it != built_in_lists_.end()) {
                    if (auto &daily = it->second[static_cast<size_t>(page_meta(Page::daily).built_in_slot)]) {
                        daily->stale = true;
                    }
                }
                if (env_.signed_in(entry.source, entry.instance) && entry.provider->features.daily.playlists) {
                    spawn(library_.load_daily_lists(entry.source));
                }
            }
        }
        daily_slot_ = slot;
    }

    void Browser::trim_history(std::vector<Route> &stack) {
        if (stack.size() > 50) {
            stack.erase(stack.begin());
        }
        // Older visits release strong references so history never pins evicted caches; revisiting re-binds from cache or reloads.
        if (stack.size() > 8) {
            Route &old = stack[stack.size() - 9];
            old.list.reset();
            old.artist.reset();
            old.profile.reset();
            if (old.search) {
                if (old.search.use_count() == 1) {
                    old.search->results = {};
                } else {
                    old.search = std::make_shared<Search>(Search{.query = old.search->query, .tab = old.search->tab, .serial = old.search->serial});
                }
            }
        }
    }

    std::shared_ptr<Browser::TrackList> Browser::cached_list(Source source, Page page, std::string_view id, bool create) {
        if (const int slot = page_meta(page).built_in_slot; slot >= 0) {
            auto &entry = built_in_lists_[source_id(source)][static_cast<size_t>(slot)];
            if (!entry && create) {
                entry = std::make_shared<TrackList>();
            }
            return entry;
        }
        if (page != Page::playlist && page != Page::album && page != Page::similar) {
            return {};
        }
        if (auto *found = list_cache_.find([&](const CachedList &entry) {
                return entry.source == source && entry.page == page && entry.id == id;
            })) {
            return found->list;
        }
        if (!create) {
            return {};
        }
        auto list = std::make_shared<TrackList>();
        list_cache_.push({.source = source, .page = page, .id = std::string(id), .list = list});
        return list;
    }

    std::shared_ptr<Browser::Artist> Browser::cached_artist(Source source, std::string_view id) {
        if (auto *found = artist_cache_.find([&](const CachedArtist &entry) {
                return entry.source == source && entry.id == id;
            })) {
            return found->artist;
        }
        auto artist = std::make_shared<Artist>();
        artist_cache_.push({.source = source, .id = std::string(id), .artist = artist});
        return artist;
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

    bool Browser::supports(Page page, Source source, std::string_view instance) const {
        const auto &p = env_.provider(source, instance);
        switch (page) {
            case Page::toplist:
                return p.capabilities.toplists;
            case Page::square:
                return p.capabilities.square;
            case Page::fm:
                return p.capabilities.radio;
            case Page::profile:
                return p.capabilities.accounts;
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
        return page_meta(page).needs_login;
    }

    void Browser::open(Page page) {
        open(page, {.source = active_.source, .instance = active_.instance});
    }

    void Browser::open(Page page, const Card &card) {
        // Opening the account's own liked playlist card goes straight to the dedicated Liked songs page.
        if (page == Page::playlist && !card.id.empty() && card.id == library_.shelf(card.source).liked.id) {
            return open(Page::liked, {.source = card.source});
        }
        if (route_.page == page && same(route_.card, card) && page != Page::search) {
            // Already there, which still counts as going there: whatever covers the page steps aside.
            ++navigations_;
            return env_.redraw();
        }
        forward_.clear();
        history_.push_back(route_);
        trim_history(history_);
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
        return open_own_profile(active_.source);
    }

    bool Browser::open_own_profile(Source source) {
        const auto &account = library_.shelf(source).account;
        if (!account) {
            return false;
        }
        open(Page::profile, {.kind = CardKind::user, .source = source, .id = account->id, .title = account->name, .cover = account->avatar, .owned = true});
        return true;
    }

    bool Browser::back() {
        if (history_.empty()) {
            return false;
        }
        forward_.push_back(route_);
        trim_history(forward_);
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
        trim_history(history_);
        Route route = std::move(forward_.back());
        forward_.pop_back();
        enter(std::move(route), 1);
        return true;
    }

    void Browser::enter(Route route, int from) {
        refresh_clock();
        if (route_.list && !route_.list->saving_order) {
            route_.list->ordering = false;
        }
        route_ = std::move(route);
        if (!route_.serial) {
            route_.serial = ++serial_;
        }
        const SourceKey target{route_.card.source, route_.card.instance};
        const bool platform_changed = active_ != target;
        if (platform_changed) {
            active_ = target;
            if (on_change) {
                on_change();
            }
        }
        if (route_.page == Page::search && route_.search) {
            query = route_.search->query;
            if (!env_.provider(active_.source, active_.instance).capabilities.search[static_cast<size_t>(route_.search->tab)]) {
                route_.search->tab = 0;
            }
        }
        entered = env_.now();
        direction = from;
        ++navigations_;
        reset_selection();
        if (const auto meta = page_meta(route_.page); !meta.title.empty()) {
            route_.card = {.source = active_.source, .instance = active_.instance, .title = std::string(meta.title), .subtitle = std::string(meta.subtitle)};
        }
        if (needs_login(route_.page) && !env_.signed_in(active_.source, active_.instance)) {
            route_.list.reset();
        }
        ensure();
        if (platform_changed) {
            spawn(warm(active_.source));
        }
        env_.redraw();
    }

    void Browser::switch_platform(Source source, std::string instance) {
        const SourceKey target{source, std::move(instance)};
        if (target == active_ || !env_.providers.contains(target.source, target.instance)) {
            return;
        }
        // Search keeps each platform's cached results and scroll positions; settings, stats and pinned are cross-platform.
        if (route_.page == Page::search || route_.page == Page::settings || route_.page == Page::stats || route_.page == Page::pinned) {
            tab_direction = target.source > active_.source ? 1 : -1;
            active_ = target;
            route_.card.source = active_.source;
            route_.card.instance = active_.instance;
            if (route_.search && !env_.provider(active_.source, active_.instance).capabilities.search[static_cast<size_t>(route_.search->tab)]) {
                route_.search->tab = 0;
            }
            reset_selection();
            tab_since = env_.now();
            ++navigations_;
            ensure();
            spawn(warm(active_.source));
            if (on_change) {
                on_change();
            }
            return env_.redraw();
        }
        Page page = route_.page;
        if (page == Page::profile && route_.card.owned && open_own_profile(target.source)) {
            return;
        }
        if (page == Page::playlist || page == Page::album || page == Page::artist || page == Page::similar || page == Page::profile ||
            !supports(page, target.source, target.instance)) {
            page = Page::recommend;
        }
        open(page, {.source = target.source, .instance = target.instance});
    }

    void Browser::ensure() {
        const Page page = route_.page;
        auto &lib = shelf();
        const bool signed_in = env_.signed_in(active_.source, active_.instance);
        if (page == Page::search && route_.search && !results(route_.search->tab).loaded) {
            load_results(route_.search->tab);
        }
        if (page == Page::recent && lib.recent_tab == 1 && signed_in) {
            library_.ensure_recent_plays(active_.source);
        }
        const auto meta = page_meta(page);
        // The liked songs wait for the account's playlists, which name the list behind them.
        if (meta.listed && !(meta.needs_login && !signed_in) && !(page == Page::liked && !lib.playlists_loaded)) {
            if (!route_.list) {
                route_.list = cached_list(route_.card.source, page, route_.card.id, true);
            }
            if (auto &list = route_.list; list && !list->busy()) {
                const bool empty_error = !list->error.empty() && list->tracks.empty();
                const bool swr_page = page == Page::liked || page == Page::cloud || page == Page::playlist;
                const double now = env_.now();
                const bool expired = swr_page && list->loaded && now - list->fetched_at >= 300.0;
                const bool daily_rolled = page == Page::daily && list->loaded && list->daily_slot >= 0 && list->daily_slot != daily_slot_;
                const bool busy_write = page == Page::playlist && (list->ordering || list->saving_order || library_.writing(route_.card.source, route_.card.id));
                if (!list->loaded || empty_error) {
                    spawn(load_list(list, page, route_.card, false));
                } else if (!busy_write && (list->stale || expired || daily_rolled) && now >= list->retry_at) {
                    spawn(load_list(list, page, route_.card, !list->tracks.empty()));
                }
            }
        }
        if (page == Page::album && env_.signed_in(route_.card.source) && env_.provider(route_.card).capabilities.collect_albums) {
            library_.ensure_collection(route_.card.source);
        }
        if (page == Page::artist) {
            if (env_.signed_in(route_.card.source) && env_.provider(route_.card).capabilities.follow_artists) {
                library_.ensure_collection(route_.card.source);
            }
            if (!route_.artist) {
                route_.artist = cached_artist(route_.card.source, route_.card.id);
            }
            if (auto &artist = route_.artist; !artist->loading) {
                if (!artist->info && (!artist->error.empty() || (!artist->songs.loaded && !artist->songs.loading))) {
                    spawn(load_artist(artist, route_.card));
                }
                const Listing &tab = artist->tab == 0 ? artist->songs : artist->tab == 1 ? artist->albums : artist->intro;
                const bool tab_empty = artist->tab == 2 ? artist->sections.empty() : tab.size() == 0;
                if (!tab.loading && tab_empty && !tab.error.empty()) {
                    load_artist_tab(artist->tab, true);
                }
            }
        }
        if (page == Page::profile && !(route_.card.owned && !signed_in)) {
            if (!route_.profile || (!route_.profile->loading && !route_.profile->info && !route_.profile->error.empty())) {
                route_.profile = social_.open(route_.card);
            } else if (route_.card.owned || library_.own(route_.card)) {
                library_.ensure_recent_plays(route_.card.source);
                if (route_.profile->section == Social::Section::collection) {
                    library_.ensure_collection(route_.card.source);
                }
            }
        }
        if (page == Page::recommend && !lib.recommended.loaded && !lib.recommended.loading) {
            spawn(library_.load_recommended(active_.source));
        }
        if (page == Page::toplist && !lib.toplists.loaded && !lib.toplists.loading) {
            spawn(library_.load_toplists(active_.source));
        }
        if (page == Page::square) {
            if (lib.categories.groups.empty() && !lib.categories_loading) {
                spawn(library_.load_categories(active_.source));
            }
            if (!lib.square.loaded) {
                library_.load_square(active_.source);
            }
        }
        if (page == Page::fm && lib.radio.empty() && !lib.radio_loading && signed_in) {
            spawn(library_.load_radio(active_.source));
        }
        if (page == Page::releases && !lib.releases[static_cast<size_t>(lib.release_tab)].loaded) {
            library_.load_releases(active_.source);
        }
        if (page == Page::dislikes && signed_in) {
            const auto &list = lib.dislikes[static_cast<size_t>(lib.dislike_tab)];
            if (!list || (!list->loaded && !list->loading)) {
                spawn(library_.load_dislikes(active_.source, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        if (page == Page::collection && signed_in) {
            library_.ensure_collection(active_.source);
        }
    }

    Task<void> Browser::warm(Source source) {
        // Visible work starts first; unvisited platforms must not fill the image budget at startup.
        co_await lyren::sleep{env_.loop, 200ms};
        auto &lib = library_.shelf(source);
        const auto &p = env_.provider(source);
        if (source == active_.source && !lib.recommended.loaded && !lib.recommended.loading) {
            co_await library_.load_recommended(source);
        }
        if (source == active_.source && p.capabilities.toplists && !lib.toplists.loaded && !lib.toplists.loading) {
            co_await library_.load_toplists(source);
        }
        if (source == active_.source && p.capabilities.square) {
            if (!lib.square.loaded) {
                library_.load_square(source);
            }
            if (lib.categories.groups.empty() && !lib.categories_loading) {
                co_await library_.load_categories(source);
            }
        }
        if (source == active_.source && supports(Page::releases, source) && !lib.releases[static_cast<size_t>(lib.release_tab)].loaded) {
            library_.load_releases(source);
        }
        if (source == active_.source) {
            library_.ensure_collection(source);
        }
    }

    void Browser::search() {
        search_on(active_.source, std::string(query), 0);
    }

    void Browser::search_on(Source source, std::string term, int tab) {
        const std::string_view clean = trim(term);
        if (clean.empty()) {
            return;
        }
        query = std::string(clean);
        if (env_.store) {
            env_.store->record_search(query);
        }
        reset_selection();
        const std::string inst = source == active_.source ? active_.instance : std::string{};
        const int safe_tab = tab >= 0 && tab < 5 && env_.provider(source, inst).capabilities.search[static_cast<size_t>(tab)] ? tab : 0;
        if (route_.page == Page::search && route_.search && route_.search->query == query && active_.source == source && active_.instance == inst) {
            route_.search->results.clear();
            route_.search->tab = safe_tab;
            route_.search->serial = ++search_gen_;
            ++navigations_;
            load_results(safe_tab);
            return env_.redraw();
        }
        forward_.clear();
        history_.push_back(route_);
        trim_history(history_);
        auto session = std::make_shared<Search>(Search{.query = query, .tab = safe_tab, .serial = ++search_gen_});
        enter({.page = Page::search, .card = {.source = source, .instance = inst}, .search = std::move(session)}, 1);
    }

    void Browser::rebuild_suggestions(std::string_view input) {
        suggestions_.clear();
        StringHashSet seen;
        auto fold = [](std::string_view s) {
            std::string out(s);
            for (char &ch : out) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return out;
        };
        auto push = [&](std::string text, bool history) {
            const std::string_view trimmed = trim(text);
            if (trimmed.empty() || suggestions_.size() >= 8) {
                return;
            }
            if (!seen.insert(fold(trimmed)).second) {
                return;
            }
            suggestions_.push_back({std::string(trimmed), history});
        };
        if (env_.store) {
            for (auto &h : env_.store->search_history(input, 8)) {
                push(std::move(h.query), true);
            }
        }
        if (!trim(input).empty()) {
            for (const auto &s : remote_suggestions_) {
                push(s, false);
            }
        }
    }

    void Browser::update_suggestions(std::string_view input) {
        const uint64_t hist_serial = env_.store ? env_.store->search_serial() : 0;
        const bool text_changed = input != suggest_input_ || active_ != suggest_source_;
        const bool history_changed = hist_serial != suggest_history_serial_;
        if (!text_changed && !history_changed) {
            return;
        }
        suggest_input_.assign(input);
        suggest_source_ = active_;
        suggest_history_serial_ = hist_serial;
        if (text_changed) {
            remote_suggestions_.clear();
            env_.loop.cancel(suggest_timer_);
            suggest_timer_ = 0;
            const uint64_t gen = ++suggest_gen_;
            if (const std::string_view trimmed = trim(input); !trimmed.empty()) {
                suggest_timer_ = env_.loop.after(180ms, [this, gen, src = active_, q = std::string(trimmed)]() mutable {
                    suggest_timer_ = 0;
                    spawn(fetch_suggestions(gen, std::move(src), std::move(q)));
                });
            }
        }
        rebuild_suggestions(input);
    }

    Task<void> Browser::fetch_suggestions(uint64_t gen, SourceKey source, std::string q) {
        auto r = co_await env_.provider(source.source, source.instance).search_suggest(std::move(q));
        if (gen != suggest_gen_ || source != active_) {
            co_return;
        }
        if (r) {
            remote_suggestions_ = std::move(*r);
            rebuild_suggestions(suggest_input_);
            env_.redraw();
        }
    }

    void Browser::remove_search_history(std::string_view term) {
        if (!env_.store) {
            return;
        }
        env_.store->remove_search(term);
        suggest_history_serial_ = env_.store->search_serial();
        rebuild_suggestions(suggest_input_);
        env_.redraw();
    }

    void Browser::clear_search_history() {
        if (!env_.store) {
            return;
        }
        env_.store->clear_search_history();
        suggest_history_serial_ = env_.store->search_serial();
        rebuild_suggestions(suggest_input_);
        env_.redraw();
    }

    void Browser::open_mention(Source source, std::string name) {
        std::string_view sv = trim(name);
        if (sv.starts_with('@')) {
            sv.remove_prefix(1);
        }
        sv = trim(sv);
        if (sv.empty()) {
            return;
        }
        std::string clean(sv);
        const auto &caps = env_.provider(source).capabilities;
        if (caps.profiles && caps.supports_search(SearchKind::users)) {
            spawn(resolve_mention(++mention_gen_, source, std::move(clean)));
            return;
        }
        search_on(source, std::move(clean), 0);
    }

    Task<void> Browser::resolve_mention(uint64_t gen, Source source, std::string name) {
        const uint64_t nav = navigations_;
        auto r = co_await env_.provider(source).search(name, SearchKind::users, 0, 10);
        if (gen != mention_gen_ || nav != navigations_) {
            co_return;
        }
        auto fold_eq = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                auto ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[i]);
                if (ca >= 'A' && ca <= 'Z') {
                    ca = static_cast<unsigned char>(ca - 'A' + 'a');
                }
                if (cb >= 'A' && cb <= 'Z') {
                    cb = static_cast<unsigned char>(cb - 'A' + 'a');
                }
                if (ca != cb) {
                    return false;
                }
            }
            return true;
        };
        if (r && !r->cards.empty()) {
            auto exact = std::ranges::find_if(r->cards, [&](const Card &c) {
                return c.kind == CardKind::user && fold_eq(c.title, name);
            });
            if (exact != r->cards.end()) {
                open_profile(source, exact->id, exact->title, exact->cover);
                co_return;
            }
            if (r->cards.size() == 1 && r->cards.front().kind == CardKind::user) {
                const auto &c = r->cards.front();
                open_profile(source, c.id, c.title, c.cover);
                co_return;
            }
        }
        search_on(source, std::move(name), static_cast<int>(SearchKind::users));
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

    Task<void> Browser::fetch_results(std::shared_ptr<Search> session, uint64_t serial, int tab, SourceKey source, int offset) {
        const auto &lib = library_.shelf(source.source, source.instance);
        const uint64_t account_gen = lib.account_gen;
        auto r = co_await env_.provider(source.source, source.instance).search(session->query, static_cast<SearchKind>(tab), offset, tab == 0 ? 30 : 40);
        if (serial != session->serial || account_gen != lib.account_gen) {
            co_return;
        }
        Listing &listing = session->results[source.id()][static_cast<size_t>(tab)];
        library_.arrived(listing, std::move(r));
        if (!listing.more()) {
            listing.total = static_cast<int>(listing.size());
        }
        env_.redraw();
    }

    Task<void> Browser::load_list(std::shared_ptr<TrackList> list, Page page, Card card, bool background) {
        auto &lib = library_.shelf(card.source);
        Provider &provider = env_.provider(card);
        const bool quiet = background && list->loaded && !list->tracks.empty();
        const uint64_t gen = ++list->gen, account_gen = lib.account_gen, revision = lib.preference_gen;
        list->begin(quiet);
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
                if (list->album.collected) {
                    list->album.collected = library_.reconcile(card.source, Library::Preference::album, card.id, *list->album.collected, revision);
                }
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
        } else if (page == Page::similar) {
            r = co_await provider.similar(card.id);
        } else {
            r = co_await provider.tracks(card);
        }
        if (gen != list->gen || account_gen != lib.account_gen) {
            co_return;
        }
        if (r) {
            const bool had_tracks = !list->tracks.empty();
            const bool same_tracks = list->loaded && std::ranges::equal(list->tracks, *r, [](const Track &a, const Track &b) {
                                         return same(a, b) && a.type == b.type;
                                     });
            std::string selected_id;
            if (selection_.list == &list->tracks && !same_tracks && quiet && selection_.selected >= 0 && selection_.selected < static_cast<int>(list->tracks.size())) {
                selected_id = list->tracks[static_cast<size_t>(selection_.selected)].id;
            }
            list->succeed(env_.now(), !had_tracks || (!quiet && !same_tracks));
            if (page == Page::daily) {
                list->daily_slot = daily_slot_;
            }
            list->tracks = std::move(*r);
            if (selection_.list == &list->tracks && !same_tracks) {
                if (quiet) {
                    auto it = selected_id.empty() ? list->tracks.end() : std::ranges::find(list->tracks, selected_id, &Track::id);
                    selection_.selected = selection_.anchor = it == list->tracks.end() ? -1 : static_cast<int>(it - list->tracks.begin());
                    if (selection_.choosing) {
                        StringHashSet remaining;
                        for (const auto &track : list->tracks) {
                            if (selection_.chosen.contains(track.id)) {
                                remaining.insert(track.id);
                            }
                        }
                        selection_.chosen = std::move(remaining);
                    }
                } else {
                    reset_selection();
                }
            }
            if (page == Page::liked) {
                library_.set_liked(card.source, list->tracks, revision);
            }
            env_.prefetch(list->tracks);
        } else {
            list->fail(r.error(), env_.now(), quiet);
        }
        env_.redraw();
    }

    void Browser::reload_list() {
        if (route_.page == Page::recent) {
            spawn(library_.load_recent_plays(route_.card.source, true));
            return;
        }
        if (route_.list) {
            spawn(load_list(route_.list, route_.page, route_.card, false));
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

    void Browser::reload_profile() {
        if (route_.page != Page::profile) {
            return;
        }
        route_.profile = social_.reload(route_.card);
        env_.redraw();
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
            if (page->info->followed) {
                page->info->followed = library_.reconcile(card.source, Library::Preference::artist, card.id, *page->info->followed, revision);
            }
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

    void Browser::add_to_playlist(std::vector<Track> tracks, Card playlist, uint64_t selection_tag) {
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
        spawn(library_.edit_playlist(std::move(playlist), std::move(tracks), true, selection_tag));
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
        list->succeed(env_.now(), false);
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
        spawn(library_.delete_playlist(std::move(playlist)));
    }

    void Browser::choose(const std::vector<Track> &tracks, int i, bool range) {
        if (route_.page == Page::playlist && route_.list) {
            route_.list->ordering = false;
        }
        auto &s = selection_;
        ++s.revision;
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
        ++s.revision;
        s.chosen.clear();
        for (const auto &track : *s.list) {
            s.chosen.insert(track.id);
        }
    }

    void Browser::on_library(const Library::Event &event) {
        using Kind = Library::Event::Kind;
        const Source source = event.source;
        const std::string sid = source_id(source);
        switch (event.kind) {
            case Kind::liked: {
                auto &list = built_in_lists_[sid][static_cast<size_t>(page_meta(Page::liked).built_in_slot)];
                if (!list || !list->loaded) {
                    break;
                }
                auto &tracks = list->tracks;
                auto it = std::ranges::find_if(tracks, [&](const Track &x) {
                    return same(x, *event.track);
                });
                bool changed = false;
                if (event.on && it == tracks.end()) {
                    tracks.insert(tracks.begin(), *event.track);
                    changed = true;
                } else if (!event.on && it != tracks.end()) {
                    tracks.erase(it);
                    changed = true;
                }
                if (changed && selection_.list == &tracks) {
                    reset_selection();
                }
                break;
            }
            case Kind::liked_stale: {
                auto &list = built_in_lists_[sid][static_cast<size_t>(page_meta(Page::liked).built_in_slot)];
                if (list) {
                    list->stale = true;
                }
                if (route_.page == Page::liked && route_.card.source == source && route_.list) {
                    spawn(load_list(route_.list, Page::liked, {.source = source}, !route_.list->tracks.empty()));
                }
                break;
            }
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
            case Kind::playlist_deleted: {
                const Card &playlist = *event.playlist;
                list_cache_.erase_if([&](const CachedList &entry) {
                    return entry.page == Page::playlist && entry.source == playlist.source && entry.id == playlist.id;
                });
                auto purge = [&](std::vector<Route> &stack) {
                    std::erase_if(stack, [&](const Route &route) {
                        return route.page == Page::playlist && same(route.card, playlist);
                    });
                };
                purge(history_);
                if (route_.page == Page::playlist && same(route_.card, playlist)) {
                    if (!back()) {
                        enter({.page = Page::recommend, .card = {.source = playlist.source}}, -1);
                    }
                }
                purge(forward_);
                break;
            }
            case Kind::loaded: {
                auto &liked = built_in_lists_[sid][static_cast<size_t>(page_meta(Page::liked).built_in_slot)];
                if (liked) {
                    liked->stale = true;
                }
                // A page that waited for the account loads now; an already-cached liked list refreshes in place.
                if (source == active_.source) {
                    if (route_.page == Page::liked && route_.list && !route_.list->busy()) {
                        spawn(load_list(route_.list, Page::liked, {.source = source}, !route_.list->tracks.empty()));
                    } else {
                        ensure();
                    }
                }
                break;
            }
            case Kind::left:
                if (source == active_.source) {
                    reset_selection();
                }
                built_in_lists_.erase(sid);
                list_cache_.erase_if([&](const CachedList &entry) {
                    return entry.source == source;
                });
                artist_cache_.erase_if([&](const CachedArtist &entry) {
                    return entry.source == source;
                });
                each_route([&](Route &route) {
                    if (route.search) {
                        route.search->results.erase(sid);
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
                if (source == active_.source) {
                    ensure();
                }
                break;
            case Kind::tracks_failed: {
                auto cached = cached_list(event.playlist->source, Page::playlist, event.playlist->id, false);
                const bool visible = route_.page == Page::playlist && same(route_.card, *event.playlist);
                if (cached) {
                    if (visible || cached->tracks.empty()) {
                        cached->error = event.error;
                    } else {
                        cached->stale = true;
                    }
                }
                each_route([&](Route &route) {
                    if (route.page == Page::playlist && same(route.card, *event.playlist) && route.list && route.list != cached) {
                        route.list->error = event.error;
                    }
                });
                break;
            }
            case Kind::tracks: {
                const Card &playlist = *event.playlist;
                const auto &fresh = *event.fresh;
                // Of a selection that was only partly carried out, what failed stays picked.
                auto &s = selection_;
                const auto &asked = *event.requested;
                if (event.tag != 0 && s.choosing && s.revision == event.tag) {
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
                            if (auto it = s.chosen.find(id); it != s.chosen.end()) {
                                s.chosen.erase(it);
                            }
                        }
                        if (event.on) {
                            std::erase_if(s.chosen, [&](const std::string &id) {
                                return actual.contains(id);
                            });
                        }
                        ++s.revision;
                    }
                }
                // Every visit to the playlist shows the same songs.
                std::shared_ptr<TrackList> list = cached_list(playlist.source, Page::playlist, playlist.id, false);
                if (!list) {
                    each_route([&](Route &route) {
                        if (!list && route.page == Page::playlist && same(route.card, playlist)) {
                            list = route.list;
                        }
                    });
                }
                if (!list) {
                    list = cached_list(playlist.source, Page::playlist, playlist.id, true);
                    list->since = env_.now();
                } else if (!list_cache_.find(
                               [&](const CachedList &entry) {
                                   return entry.list == list;
                               },
                               false)) {
                    list_cache_.push({.source = playlist.source, .page = Page::playlist, .id = playlist.id, .list = list});
                }
                ++list->gen;
                list->tracks = fresh;
                list->succeed(env_.now(), false);
                if (s.list == &list->tracks) {
                    s.selected = s.anchor = -1;
                    StringHashSet remaining;
                    for (const auto &track : list->tracks) {
                        if (s.chosen.contains(track.id)) {
                            remaining.insert(track.id);
                        }
                    }
                    s.chosen = std::move(remaining);
                    ++s.revision;
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
            case Kind::radio_exhausted:
                break;
        }
        env_.redraw();
    }

} // namespace lyren
