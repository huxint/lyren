#include "app/shell.hpp"

#include <algorithm>
#include <ranges>

#include "core/i18n.hpp"

namespace lyren {

    // Keep the service's continuation independent of the number of visible rows.
    void Shell::arrived(Results &res, Result<Batch> r) {
        res.loading = false;
        if (!r) {
            res.error = r.error();
            return;
        }
        if (!res.loaded) {
            res.since = now_();
        }
        res.loaded = true;
        images_.prefetch(r->tracks);
        images_.prefetch(r->cards);
        res.next_offset = r->next_offset;
        res.tracks.append_range(std::views::as_rvalue(r->tracks));
        res.cards.append_range(std::views::as_rvalue(r->cards));
        res.total = res.more() ? std::max(r->total, static_cast<int>(res.size())) : static_cast<int>(res.size());
    }

    Task<void> Shell::load_cards(Source source, Results &res, Task<Result<std::vector<Card>>> request) {
        const auto gen = library(source).account_gen;
        res.loading = true;
        auto result = co_await std::move(request);
        if (gen != library(source).account_gen) {
            co_return;
        }
        Results fresh;
        arrived(fresh, std::move(result).transform([](std::vector<Card> cards) {
            return Batch{.cards = std::move(cards)};
        }));
        res = std::move(fresh);
        redraw_();
    }

    Task<void> Shell::load_daily_lists(Source source) {
        auto &lib = library(source);
        uint64_t gen = lib.gen;
        auto r = co_await provider(source).features.daily.playlists();
        if (gen != lib.gen || !r) {
            co_return;
        }
        lib.daily_lists = std::move(*r);
        images_.prefetch(lib.daily_lists);
        lib.daily_since = now_();
        redraw_();
    }

    Task<void> Shell::load_fm(Source source) {
        auto &lib = library(source);
        uint64_t gen = lib.gen;
        lib.fm_loading = true;
        lib.fm_error.clear();
        auto r = co_await provider(source).fm();
        if (gen != lib.gen) {
            co_return;
        }
        lib.fm_loading = false;
        if (r) {
            images_.prefetch(*r);
            lib.fm.append_range(std::views::as_rvalue(*r));
        } else {
            lib.fm_error = r.error();
        }
        redraw_();
    }

    void Shell::refresh_recent() {
        for (auto &lib : libraries_) {
            lib.recent.clear();
        }
        for (const auto &track : recent_) {
            library(track.source).recent.push_back(track);
        }
        for (const auto &lib : libraries_) {
            images_.prefetch(lib.recent);
        }
    }

    Task<void> Shell::load_dislikes(Source source, DislikeKind kind) {
        auto &cached = library(source).dislikes[static_cast<size_t>(kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        uint64_t gen = ++list->gen;
        list->loading = true;
        list->error.clear();
        redraw_();
        auto r = co_await provider(source).features.exclusions.list(kind);
        if (gen != list->gen || library(source).dislikes[static_cast<size_t>(kind)] != list) {
            co_return;
        }
        list->loading = false;
        list->loaded = true;
        if (r) {
            list->items = std::move(*r);
            list->since = now_();
        } else {
            list->error = r.error();
        }
        redraw_();
    }

    ui::Id Shell::search_scroll_id(ui::Ui &ui, Source source, int tab) {
        return ui::sub_id(ui.id("search-results"), static_cast<uint64_t>(source) * 4 + static_cast<uint64_t>(tab));
    }

    void Shell::search(ui::Ui &ui) {
        if (query_.empty()) {
            return;
        }
        reset_selection();
        searched_ = query_;
        ++search_gen_;
        results_ = {};
        search_tab_ = 0;
        for (const auto &entry : platforms) {
            for (int tab = 0; tab < 4; ++tab) {
                ui.scroll_to(search_scroll_id(ui, entry.source, tab), 0);
            }
        }
        load_results(0);
        open(ui, Page::search);
    }

    void Shell::load_results(int tab, bool retry) {
        Results &res = results_[static_cast<size_t>(active_)][tab];
        if (res.loading || (!retry && (!res.more() || !res.error.empty()))) {
            return;
        }
        res.loading = true;
        res.error.clear();
        spawn(fetch_results(search_gen_, tab, active_, searched_, res.next_offset));
    }

    Task<void> Shell::fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset) {
        const auto account_gen = library(source).account_gen;
        auto r = co_await provider(source).search(query, static_cast<SearchKind>(tab), offset, tab == 0 ? 30 : 40);
        if (gen != search_gen_ || account_gen != library(source).account_gen) {
            co_return;
        }
        Results &res = results_[static_cast<size_t>(source)][tab];
        arrived(res, std::move(r));
        if (!res.more()) {
            res.total = static_cast<int>(res.size());
        }
        redraw_();
    }

    Task<void> Shell::warm_pages(Source source) {
        // Visible work starts first; unvisited platforms must not fill the image budget at startup.
        co_await lyren::sleep{loop_, 200ms};
        if (source != active_) {
            co_return;
        }
        auto &lib = library(source);
        const auto &p = provider(source);
        if (!lib.recommended.loaded && !lib.recommended.loading) {
            co_await load_cards(source, library(source).recommended, provider(source).recommended_playlists());
        }
        if (source == active_ && p.capabilities.toplists && !lib.toplists.loaded && !lib.toplists.loading) {
            co_await load_cards(source, library(source).toplists, provider(source).toplists());
        }
        if (source == active_ && p.capabilities.square && !lib.square.loaded && !lib.square.loading) {
            lib.square.loading = true;
            co_await fetch_square(source, lib.square_gen, lib.category.id, 0);
        }
        if (source == active_ && p.capabilities.square && lib.categories.groups.empty() && !lib.categories_loading) {
            co_await load_categories(source);
        }
        for (int tab = 0; tab < 2; ++tab) {
            auto &res = lib.releases[tab];
            if (source == active_ && !p.release_categories[tab].empty() && !res.loaded && !res.loading) {
                res.loading = true;
                co_await fetch_releases(source, tab, lib.release_gen[tab], provider(source).release_categories[tab][lib.release_category[tab]].id, 0);
            }
        }
        if (source == active_ && supports(Page::collection, source) && p.logged_in() && !lib.collection[0].loaded && !lib.collection[0].loading) {
            co_await load_collection(source);
        }
    }

    void Shell::ensure_route_data() {
        Page page = route_.page;
        Library &lib = library();
        if (page == Page::search && !results_[static_cast<size_t>(active_)][search_tab_].loaded) {
            load_results(search_tab_);
        }
        bool listed = page == Page::daily || page == Page::radar || page == Page::cloud || page == Page::liked || page == Page::playlist || page == Page::album ||
                      page == Page::similar || (page == Page::recent && lib.recent_tab == 1 && signed_in());
        if (listed && !route_.list && !(needs_login(page) && !signed_in())) {
            if (page == Page::liked && !lib.playlists_loaded) {
                return;
            }
            route_.list = std::make_shared<List>();
            spawn(load_list(route_.list, page, route_.card));
        }
        if (page == Page::artist && !route_.artist) {
            route_.artist = std::make_shared<ArtistPage>();
            spawn(load_artist(route_.artist, route_.card));
        }
        if (page == Page::recommend && !lib.recommended.loaded && !lib.recommended.loading) {
            spawn(load_cards(active_, library(active_).recommended, provider(active_).recommended_playlists()));
        }
        if (page == Page::toplist && !lib.toplists.loaded && !lib.toplists.loading) {
            spawn(load_cards(active_, library(active_).toplists, provider(active_).toplists()));
        }
        if (page == Page::square) {
            if (lib.categories.groups.empty() && !lib.categories_loading) {
                spawn(load_categories(active_));
            }
            if (!lib.square.loaded) {
                load_square();
            }
        }
        if (page == Page::fm && lib.fm.empty() && !lib.fm_loading && signed_in()) {
            spawn(load_fm(active_));
        }
        if (page == Page::releases && !lib.releases[lib.release_tab].loaded) {
            load_releases();
        }
        if (page == Page::dislikes && signed_in()) {
            const auto &list = lib.dislikes[lib.dislike_tab];
            if (!list || (!list->loaded && !list->loading)) {
                spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        // Collections can change elsewhere, so they reload whenever the page is entered.
        if (page == Page::collection && signed_in() && !lib.collection[0].loading) {
            spawn(load_collection(active_));
        }
    }

    Task<void> Shell::load_list(std::shared_ptr<List> list, Page page, Card card) {
        auto &lib = library(card.source);
        uint64_t gen = ++list->gen, account_gen = lib.account_gen, revision = lib.preference_gen;
        list->loading = true;
        list->error.clear();
        // Not a ?: chain: with co_await in its branches GCC 16 also starts the tasks of branches not taken.
        Result<std::vector<Track>> r;
        if (page == Page::album) {
            auto a = co_await provider(card).album(card.id);
            if (gen != list->gen || account_gen != lib.account_gen) {
                co_return;
            }
            if (a) {
                r = std::move(a->tracks);
                list->album = std::move(*a);
                list->album.collected = reconcile_preference(card.source, Preference::album, card.id, list->album.collected, revision);
            } else {
                r = std::unexpected(a.error());
            }
        } else if (page == Page::liked) {
            if (!lib.liked.id.empty()) {
                r = co_await provider(card).tracks(lib.liked);
            }
        } else if (page == Page::daily) {
            r = co_await provider(card).features.daily.songs();
        } else if (page == Page::radar) {
            r = co_await provider(card).features.radar();
        } else if (page == Page::cloud) {
            r = co_await provider(card).features.cloud();
        } else if (page == Page::recent) {
            r = co_await provider(card).features.recent_plays();
        } else if (page == Page::similar) {
            r = co_await provider(card).similar(card.id);
        } else {
            r = co_await provider(card).tracks(card);
        }
        if (gen != list->gen || account_gen != lib.account_gen) {
            co_return;
        }
        list->loading = false;
        if (r) {
            if (selection_list_ == &list->tracks) {
                reset_selection();
            }
            list->tracks = std::move(*r);
            if (page == Page::liked) {
                lib.liked_ids.clear();
                for (const auto &track : list->tracks) {
                    lib.liked_ids.insert(track.id);
                }
                reconcile_preferences(card.source, Preference::song, revision);
            }
            images_.prefetch(list->tracks);
            list->since = now_();
        } else {
            list->error = r.error();
        }
        redraw_();
    }

    Task<void> Shell::load_artist(std::shared_ptr<ArtistPage> page, Card card) {
        auto &lib = library(card.source);
        uint64_t account_gen = lib.account_gen, revision = lib.preference_gen;
        page->loading = true;
        page->error.clear();
        if (!page->songs.loaded && !page->songs.loading) {
            page->songs.loading = true;
            page->songs.error.clear();
            spawn(fetch_artist(page, card, 0, 0));
        }
        auto r = co_await provider(card).artist(card.id);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        page->loading = false;
        if (r) {
            page->info = std::move(*r);
            page->info->followed = reconcile_preference(card.source, Preference::artist, card.id, page->info->followed, revision);
        } else {
            page->error = r.error();
        }
        redraw_();
    }

    void Shell::load_artist_tab(int tab, bool retry) {
        ArtistPage &a = *route_.artist;
        Results &res = tab == 0 ? a.songs : tab == 1 ? a.albums : a.intro;
        if (res.loading || (!retry && (!res.more() || !res.error.empty()))) {
            return;
        }
        res.loading = true;
        res.error.clear();
        spawn(fetch_artist(route_.artist, route_.card, tab, res.next_offset));
    }

    Task<void> Shell::fetch_artist(std::shared_ptr<ArtistPage> page, Card card, int tab, int offset) {
        if (tab == 2) {
            auto r = co_await provider(card).artist_intro(card.id);
            page->intro.loading = false;
            page->intro.loaded = true;
            page->intro.next_offset = -1;
            page->intro.since = now_();
            if (r) {
                page->sections = std::move(*r);
            } else {
                page->intro.error = r.error();
            }
            co_return redraw_();
        }
        Result<Batch> r;
        if (tab == 0) {
            r = co_await provider(card).artist_songs(card.id, offset);
        } else {
            r = co_await provider(card).artist_albums(card.id, offset);
        }
        arrived(tab == 0 ? page->songs : page->albums, std::move(r));
        redraw_();
    }

    Task<void> Shell::load_categories(Source source) {
        auto &lib = library(source);
        lib.categories_loading = true;
        auto r = co_await provider(source).categories();
        lib.categories_loading = false;
        if (r) {
            lib.categories = std::move(*r);
        }
        redraw_();
    }

    void Shell::set_category(ui::Ui &ui, Category category) {
        auto &lib = library();
        if (category.id == lib.category.id) {
            return;
        }
        lib.category = std::move(category);
        ++lib.square_gen;
        lib.square = {};
        lib.square_time = ui.now;
        ui.scroll_to(ui.id("square"), 0);
        load_square();
    }

    void Shell::load_square() {
        auto &lib = library();
        if (lib.square.loading || !lib.square.more() || (lib.square.loaded && !lib.square.error.empty())) {
            return;
        }
        lib.square.loading = true;
        lib.square.error.clear();
        spawn(fetch_square(active_, lib.square_gen, lib.category.id, lib.square.next_offset));
    }

    Task<void> Shell::fetch_square(Source source, uint64_t gen, std::string category, int offset) {
        auto r = co_await provider(source).square(std::move(category), offset);
        auto &lib = library(source);
        if (gen != lib.square_gen) {
            co_return;
        }
        arrived(lib.square, std::move(r));
        redraw_();
    }

    void Shell::load_releases() {
        auto &lib = library();
        int tab = lib.release_tab;
        auto &res = lib.releases[tab];
        if (res.loading || !res.more() || (res.loaded && !res.error.empty())) {
            return;
        }
        res.loading = true;
        res.error.clear();
        const auto &category = provider().release_categories[tab][lib.release_category[tab]];
        spawn(fetch_releases(active_, tab, lib.release_gen[tab], category.id, res.next_offset));
    }

    void Shell::set_release_category(ui::Ui &ui, int category) {
        auto &lib = library();
        int tab = lib.release_tab;
        lib.release_category[tab] = category;
        ++lib.release_gen[tab];
        lib.releases[tab] = {};
        selected_ = -1;
        ui.scroll_to(ui.id(tab ? "release-albums" : "release-songs"), 0);
        load_releases();
    }

    Task<void> Shell::fetch_releases(Source source, int tab, uint64_t gen, std::string category, int offset) {
        Result<Batch> r;
        if (tab == 0) {
            auto songs = co_await provider(source).new_songs(std::move(category));
            r = std::move(songs).transform([](std::vector<Track> tracks) {
                int total = static_cast<int>(tracks.size());
                return Batch{.tracks = std::move(tracks), .total = total};
            });
        } else {
            r = co_await provider(source).new_albums(std::move(category), offset);
        }
        auto &lib = library(source);
        if (gen != lib.release_gen[tab]) {
            co_return;
        }
        arrived(lib.releases[tab], std::move(r));
        redraw_();
    }

    Task<void> Shell::load_collection(Source source) {
        auto &lib = library(source);
        uint64_t gen = ++lib.collection_gen, revision = lib.preference_gen;
        for (auto &c : lib.collection) {
            c.loading = true;
        }
        auto &p = provider(source);
        auto collect = [](Provider &provider, bool artists) -> Task<Result<std::vector<Card>>> {
            if (artists && provider.capabilities.follow_artists) {
                co_return co_await provider.followed_artists();
            }
            if (!artists && provider.capabilities.collect_albums) {
                co_return co_await provider.collected_albums();
            }
            co_return std::vector<Card>{};
        };
        auto [albums, artists] = co_await both(collect(p, false), collect(p, true));
        if (gen != lib.collection_gen) {
            co_return;
        }
        auto batch = [](std::vector<Card> v) {
            return Batch{.cards = std::move(v)};
        };
        std::array<Results, 2> fresh;
        arrived(fresh[0], std::move(albums).transform(batch));
        arrived(fresh[1], std::move(artists).transform(batch));
        // A reload does not replay the cards' entrance.
        for (int i = 0; i < 2; ++i) {
            if (lib.collection[i].loaded) {
                fresh[i].since = lib.collection[i].since;
            }
            lib.collection[i] = std::move(fresh[i]);
            if (lib.collection[i].loaded) {
                reconcile_preferences(source, i == 0 ? Preference::album : Preference::artist, revision);
            }
        }
        redraw_();
    }

} // namespace lyren
