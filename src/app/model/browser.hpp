#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/env.hpp"
#include "app/model/fetch.hpp"
#include "app/model/library.hpp"
#include "app/model/social.hpp"
#include "core/string_map.hpp"

namespace lyren {

    enum class Page : uint8_t {
        recommend,
        daily,
        toplist,
        square,
        fm,
        radar,
        releases,
        settings,
        liked,
        recent,
        stats,
        pinned,
        cloud,
        collection,
        dislikes,
        playlist,
        album,
        artist,
        similar,
        search,
        profile
    };

    // Where the listener is in the app: the platform, the page and the way back, the search, and what the page on
    // show needs loaded: its songs, its artist, its listener.
    class Browser {
    public:
        // Songs behind a page, loaded when it is entered.
        struct TrackList : Fetch {
            std::vector<Track> tracks;
            AlbumInfo album;                             // album pages: the details; the tracks are above
            int daily_slot = -1;                         // day-cycle stamp (00:00 / 06:00) for daily mix rollover
            bool ordering = false, saving_order = false; // a playlist being put in order by hand
        };
        // An artist's page: the head, songs and albums a page at a time, the introduction (intro holds only its status).
        struct Artist {
            std::optional<ArtistInfo> info;
            bool loading = false;
            std::string error;
            int tab = 0;
            Listing songs, albums, intro;
            std::vector<std::pair<std::string, std::string>> sections;
        };
        // One search query and its results on each platform.
        struct Search {
            std::string query;
            int tab = 0;
            uint64_t serial = 0;
            uint64_t version = 0;
            bool aggregate = false;
            StringMap<std::array<Listing, 5>> results;
        };
        struct Suggestion {
            std::string text;
            bool from_history = false;
        };
        struct Route {
            Page page = Page::recommend;
            Card card; // what a playlist-like page shows
            std::shared_ptr<TrackList> list;
            std::shared_ptr<Artist> artist;
            std::shared_ptr<Social::Page> profile;
            std::shared_ptr<Search> search;
            uint64_t serial = 0; // names this visit: what it scrolls starts from the top
        };
        // Rows picked in the track list on show, to act on together.
        struct Selection {
            int selected = -1, anchor = -1;
            bool choosing = false;
            const std::vector<Track> *list = nullptr;
            StringHashSet chosen;
            uint64_t revision = 1;

            // The picked songs of tracks, in its order.
            std::vector<Track> of(const std::vector<Track> &tracks) const;
        };

        Browser(Env &env, Library &library, Social &social, Source active, std::string instance = {});
        ~Browser();
        Browser(const Browser &) = delete;
        Browser &operator=(const Browser &) = delete;

        std::move_only_function<void()> on_change; // the platform on show changed

        Route &route() {
            return route_;
        }
        Source active() const {
            return active_.source;
        }
        const SourceKey &active_key() const {
            return active_;
        }
        std::string active_id() const {
            return active_.id();
        }
        Library::Shelf &shelf() {
            return library_.shelf(active_.source, active_.instance);
        }
        bool can_go_back() const {
            return !history_.empty();
        }
        bool can_go_forward() const {
            return !forward_.empty();
        }
        // Counts the times the listener went somewhere.
        uint64_t navigations() const {
            return navigations_;
        }
        // "05-17", for today's mix.
        std::string_view today() const {
            return today_;
        }

        // How the page on show came in: when, and from which side (1 forward, -1 back); the same for its tabs.
        double entered = -10, tab_since = -10;
        int direction = 1, tab_direction = 1;

        bool supports(Page page, Source source, std::string_view instance = {}) const;
        static bool needs_login(Page page);
        void open(Page page);
        void open(Page page, const Card &card);
        void open(const Card &card); // the page for a card
        void open_profile(Source source, std::string id, std::string name, std::string avatar);
        // False when the platform has no account signed in.
        bool open_own_profile();
        bool open_own_profile(Source source);
        bool back();
        bool forward();
        void switch_platform(Source source, std::string instance = {});
        // Loads what the page on show still lacks.
        void ensure();
        // Fills a platform's browse pages ahead of their first visit.
        Task<void> warm(Source source);

        // Search.
        std::string query; // as typed
        int &search_tab() {
            return route_.search ? route_.search->tab : empty_tab_;
        }
        const std::string &searched() const {
            return route_.search ? route_.search->query : empty_query_;
        }
        uint64_t search_serial() const {
            return route_.search ? route_.search->serial : 0;
        }
        uint64_t search_version() const {
            return route_.search ? route_.search->version : 0;
        }
        bool search_aggregated() const {
            return route_.search && route_.search->aggregate;
        }
        void set_search_aggregated(bool on);
        Listing &results(int tab) {
            return route_.search ? route_.search->results[active_.id()][static_cast<size_t>(tab)] : empty_listing_;
        }
        Listing &results(Source source, std::string_view instance, int tab) {
            return route_.search ? route_.search->results[source_id(source, instance)][static_cast<size_t>(tab)] : empty_listing_;
        }
        void search();
        void search_on(Source source, std::string term, int tab = 0, bool aggregate = false);
        // Search history and debounced live suggestions for the top search box.
        void update_suggestions(std::string_view input);
        const std::vector<Suggestion> &suggestions() const {
            return suggestions_;
        }
        void remove_search_history(std::string_view term);
        void clear_search_history();
        // Resolves an @mention on source: opens the matching listener profile if found, otherwise opens search.
        void open_mention(Source source, std::string name);
        void load_results(int tab, bool retry = false);
        void load_all_results(int tab, bool retry = false);
        void reset_all_results(int tab);

        // The songs, artist and listener of the page on show.
        Task<void> load_list(std::shared_ptr<TrackList> list, Page page, Card card, bool background = false);
        void reload_list();
        // Asks again for an artist whose page did not load.
        void reload_artist();
        void load_artist_tab(int tab, bool retry = false);
        // Replaces the cached profile on show with a fresh load.
        void reload_profile();
        // The song leaves the daily list at once; the one the service recommends instead takes its place.
        void dislike_daily(std::shared_ptr<TrackList> list, Track track);
        void add_to_playlist(std::vector<Track> tracks, Card playlist, uint64_t selection_tag = 0);
        void move_track(std::shared_ptr<TrackList> list, Card playlist, int from, int to);
        void delete_playlist(Card playlist);

        Selection &selection() {
            return selection_;
        }
        void choose(const std::vector<Track> &tracks, int i, bool range);
        void choose_all();
        void reset_selection() {
            const uint64_t next = selection_.revision + 1;
            selection_ = {.revision = next};
        }

    private:
        struct CachedList {
            Source source = Source::netease;
            Page page = Page::playlist;
            std::string id;
            std::shared_ptr<TrackList> list;
        };
        struct CachedArtist {
            Source source = Source::netease;
            std::string id;
            std::shared_ptr<Artist> artist;
        };
        struct PageMeta {
            bool needs_login = false;
            bool listed = false;
            int built_in_slot = -1;
            std::string_view title;
            std::string_view subtitle;
        };
        static constexpr PageMeta page_meta(Page page) {
            switch (page) {
                case Page::liked:
                    return {.needs_login = true, .listed = true, .built_in_slot = 0, .title = "Liked songs"};
                case Page::daily:
                    return {.needs_login = true, .listed = true, .built_in_slot = 1, .title = "Daily mix"};
                case Page::radar:
                    return {.listed = true, .built_in_slot = 2, .title = "Radar", .subtitle = "Favorites and new discoveries"};
                case Page::cloud:
                    return {.needs_login = true, .listed = true, .built_in_slot = 3, .title = "My cloud", .subtitle = "Songs uploaded to your cloud"};
                case Page::recent:
                    return {.title = "Recently played"};
                case Page::stats:
                    return {.title = "Listening stats", .subtitle = "Local listening across all platforms"};
                case Page::pinned:
                    return {.title = "Pinned offline", .subtitle = "Kept in local audio cache"};
                case Page::fm:
                case Page::collection:
                case Page::dislikes:
                    return {.needs_login = true};
                case Page::playlist:
                case Page::album:
                case Page::similar:
                    return {.listed = true};
                default:
                    return {};
            }
        }
        static void trim_history(std::vector<Route> &stack);
        void refresh_clock();
        std::shared_ptr<TrackList> cached_list(Source source, Page page, std::string_view id, bool create);
        std::shared_ptr<Artist> cached_artist(Source source, std::string_view id);
        void enter(Route route, int direction);
        void on_library(const Library::Event &event);
        template <class F>
        void each_route(F &&visit);
        Task<void> fetch_results(std::shared_ptr<Search> session, uint64_t serial, int tab, SourceKey source, int offset);
        Task<void> fetch_suggestions(uint64_t gen, SourceKey source, std::string query);
        void rebuild_suggestions(std::string_view input);
        Task<void> resolve_mention(uint64_t gen, Source source, std::string name);
        Task<void> load_artist(std::shared_ptr<Artist> page, Card card);
        Task<void> fetch_artist(std::shared_ptr<Artist> page, Card card, int tab, int offset);
        Task<void> reorder(std::shared_ptr<TrackList> list, Card playlist, std::vector<Track> order, int selected, uint64_t account_gen);

        Env &env_;
        Library &library_;
        Social &social_;
        SourceKey active_;
        Route route_;
        std::vector<Route> history_, forward_;
        StringMap<std::array<std::shared_ptr<TrackList>, 4>> built_in_lists_;
        LruCache<CachedList, 16> list_cache_;
        LruCache<CachedArtist, 12> artist_cache_;
        uint64_t serial_ = 0, navigations_ = 0;
        std::string today_;
        int daily_slot_ = -1;
        uint64_t search_gen_ = 0, mention_gen_ = 0;
        std::string suggest_input_;
        uint64_t suggest_gen_ = 0, suggest_history_serial_ = 0;
        SourceKey suggest_source_;
        Loop::TimerId suggest_timer_ = 0;
        std::vector<std::string> remote_suggestions_;
        std::vector<Suggestion> suggestions_;
        std::string empty_query_;
        int empty_tab_ = 0;
        Listing empty_listing_;
        Selection selection_;
    };

} // namespace lyren
