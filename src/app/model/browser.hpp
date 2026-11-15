#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/env.hpp"
#include "app/model/fetch.hpp"
#include "app/model/library.hpp"
#include "app/model/social.hpp"

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
        struct Route {
            Page page = Page::recommend;
            Card card; // what a playlist-like page shows
            std::shared_ptr<TrackList> list;
            std::shared_ptr<Artist> artist;
            std::shared_ptr<Social::Page> profile;
            uint64_t serial = 0; // names this visit: what it scrolls starts from the top
        };
        // Rows picked in the track list on show, to act on together.
        struct Selection {
            int selected = -1, anchor = -1;
            bool choosing = false;
            const std::vector<Track> *list = nullptr;
            std::unordered_set<std::string> chosen;

            // The picked songs of tracks, in its order.
            std::vector<Track> of(const std::vector<Track> &tracks) const;
        };

        Browser(Env &env, Library &library, Social &social, Source active);
        Browser(const Browser &) = delete;
        Browser &operator=(const Browser &) = delete;

        std::move_only_function<void()> on_change; // the platform on show changed

        Route &route() {
            return route_;
        }
        Source active() const {
            return active_;
        }
        Library::Shelf &shelf() {
            return library_.shelf(active_);
        }
        bool can_go_back() const {
            return !history_.empty();
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

        bool supports(Page page, Source source) const;
        static bool needs_login(Page page);
        void open(Page page);
        void open(Page page, const Card &card);
        void open(const Card &card); // the page for a card
        void open_profile(Source source, std::string id, std::string name, std::string avatar);
        // False when the platform on show has no account signed in.
        bool open_own_profile();
        bool back();
        void switch_platform(Source source);
        // Loads what the page on show still lacks.
        void ensure();
        // Fills a platform's browse pages ahead of their first visit.
        Task<void> warm(Source source);

        // Search.
        std::string query; // as typed
        int search_tab = 0;
        const std::string &searched() const {
            return searched_;
        }
        uint64_t search_serial() const {
            return search_gen_;
        }
        Listing &results(int tab) {
            return results_[static_cast<size_t>(active_)][static_cast<size_t>(tab)];
        }
        void search();
        void load_results(int tab, bool retry = false);

        // The songs and the artist of the page on show.
        Task<void> load_list(std::shared_ptr<TrackList> list, Page page, Card card);
        void reload_list();
        // Asks again for an artist whose page did not load.
        void reload_artist();
        void load_artist_tab(int tab, bool retry = false);
        // The song leaves the daily list at once; the one the service recommends instead takes its place.
        void dislike_daily(std::shared_ptr<TrackList> list, Track track);
        void add_to_playlist(std::vector<Track> tracks, Card playlist);
        void remove_from_playlist(Card playlist, std::vector<Track> tracks);
        void move_track(std::shared_ptr<TrackList> list, Card playlist, int from, int to);
        void delete_playlist(Card playlist);

        Selection &selection() {
            return selection_;
        }
        void choose(const std::vector<Track> &tracks, int i, bool range);
        void choose_all();
        void reset_selection() {
            selection_ = {};
        }

    private:
        void enter(Route route, int direction);
        void on_library(const Library::Event &event);
        template <class F>
        void each_route(F &&visit);
        Task<void> fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset);
        Task<void> load_artist(std::shared_ptr<Artist> page, Card card);
        Task<void> fetch_artist(std::shared_ptr<Artist> page, Card card, int tab, int offset);
        Task<void> reorder(std::shared_ptr<TrackList> list, Card playlist, std::vector<Track> order, int selected, uint64_t account_gen);

        Env &env_;
        Library &library_;
        Social &social_;
        Source active_;
        Route route_;
        std::vector<Route> history_;
        uint64_t serial_ = 0, navigations_ = 0;
        std::string today_;
        std::string searched_;
        std::array<std::array<Listing, 5>, platforms.size()> results_;
        uint64_t search_gen_ = 0;
        Selection selection_;
    };

} // namespace lyren
