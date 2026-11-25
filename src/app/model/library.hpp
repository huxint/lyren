#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "app/env.hpp"
#include "app/model/fetch.hpp"
#include "core/string_map.hpp"

namespace lyren {

    // What the listener keeps on each platform and what the platform offers them: the account, its playlists, likes,
    // follows and collections, and the pages the platform fills (recommendations, charts, the square, releases, radio).
    // Whatever the listener changes shows at once and is written behind; the service's answer settles it.
    class Library {
    public:
        // What can be kept or let go with one switch.
        enum class Preference : uint8_t {
            song,
            artist,
            album,
            playlist,
            user
        };
        struct DislikeList : Fetch {
            std::vector<Dislike> items;
            std::unordered_set<std::string> pending; // being written
        };
        struct RecentPlays : Fetch {
            std::vector<Track> tracks;
        };
        // One writer per resource. Further clicks change the desired value while the current request finishes.
        struct Edit {
            bool value = false, confirmed = false, pending = false;
            uint64_t revision = 0;
            Track track;
            Card card;
        };
        // One platform's account and cached pages.
        struct Shelf {
            std::optional<Account> account;
            // gen disowns loads of the account's data, account_gen everything asked on behalf of the account that was
            // signed in; preference_gen orders reads against the writes they may have missed.
            uint64_t gen = 0, account_gen = 0, preference_gen = 0, playlist_gen = 0;
            std::array<StringMap<Edit>, 5> preferences;
            std::unordered_set<std::string> playlist_writes; // playlists being changed; "" while one is created
            std::vector<Card> playlists, daily_lists;
            double daily_since = -10;
            Card liked; // the playlist behind the liked songs
            std::unordered_set<std::string> liked_ids;
            bool playlists_loaded = false;
            std::string error; // why the account's data did not load

            Listing recommended, toplists;
            std::array<Listing, 2> releases; // songs, albums
            std::array<int, 2> release_category{};
            std::array<uint64_t, 2> release_gen{};
            Categories categories;
            bool categories_loading = false;
            Category category;
            Listing square;
            uint64_t square_gen = 0;
            double square_since = -10;
            std::array<Listing, 2> collection; // albums, artists
            uint64_t collection_gen = 0;
            double collection_at = -1e9, collection_retry_at = -1e9;
            std::array<std::shared_ptr<DislikeList>, 3> dislikes;
            RecentPlays recent_plays;

            std::vector<Track> radio;
            int radio_pos = 0;
            bool radio_loading = false;
            bool radio_pending = false; // waiting for the next batch after reaching the end
            std::string radio_error;
            double radio_since = -10; // when the radio moved on

            // Where the listener left this platform's tabbed pages.
            int release_tab = 0, recent_tab = 0, collection_tab = 0, dislike_tab = 0;
        };
        // Told to the parts that keep copies of what changed.
        struct Event {
            enum class Kind : uint8_t {
                account,       // another account is signed in on the platform
                loaded,        // the account's playlists and likes arrived
                left,          // the account is gone and its data with it
                signed_out,    // the service has been told
                liked,         // a song joined or left the liked songs
                liked_stale,   // the liked songs changed on the service in ways only a reload shows
                playlists,     // the account's playlists changed
                tracks,        // a playlist's songs changed: fresh is what it holds now, requested what was asked
                tracks_failed, // a playlist's songs may have changed, and reading them back failed
                radio_ready    // the next radio songs arrived while waiting at the end of a batch
            };
            Kind kind;
            Source source;
            const Track *track = nullptr;
            const Card *playlist = nullptr;
            const std::vector<Track> *fresh = nullptr, *requested = nullptr;
            bool on = false; // liked: joined; tracks: added
            std::string_view error;
        };

        explicit Library(Env &env);
        Library(const Library &) = delete;
        Library &operator=(const Library &) = delete;

        void subscribe(std::move_only_function<void(const Event &)> listener) {
            listeners_.push_back(std::move(listener));
        }

        Shelf &shelf(Source source) {
            return shelves_[static_cast<size_t>(source)];
        }
        const Shelf &shelf(Source source) const {
            return shelves_[static_cast<size_t>(source)];
        }
        // The listener card is the signed-in account's own.
        bool own(const Card &listener) const {
            const auto &account = shelf(listener.source).account;
            return account && account->id == listener.id;
        }

        // The account.
        Task<void> load(Source source);
        void reset(Source source);
        Task<void> sign_out(Source source);

        // Likes, follows and collections.
        bool liked(const Track &t) const {
            return shelf(t.source).liked_ids.contains(t.id);
        }
        // When the song was last liked, for the heart's pop; long ago otherwise.
        double liked_at(const Track &t) const {
            return pop_key_ == key(t) ? pop_time_ : -10;
        }
        // What shows for something the service reported as value: the listener's own change, if there is one.
        bool kept(Source source, Preference kind, std::string_view id, bool value) const;
        bool following(Source source, const Person &person) const {
            return kept(source, Preference::user, person.id, person.followed);
        }
        void toggle_like(Track track);
        void toggle_follow(Card artist, bool followed);
        void toggle_collect(Card album, bool collected);
        void toggle_collect_playlist(Card playlist);
        void toggle_follow_user(Source source, const Person &person);
        // A read that began at revision came back with value: it stands unless a write has happened since. Returns
        // what to show.
        bool reconcile(Source source, Preference kind, std::string_view id, bool value, uint64_t revision);
        void reconcile_all(Source source, Preference kind, uint64_t revision);
        // The liked songs as just read from the service.
        void set_liked(Source source, const std::vector<Track> &tracks, uint64_t revision);

        // Playlists.
        bool writing(Source source, std::string_view playlist) const {
            return shelf(source).playlist_writes.contains(std::string(playlist));
        }
        // One write per playlist at a time; false, with a note, when it cannot start.
        bool begin_write(const Card &playlist);
        void finish_write(const Card &playlist);
        Task<bool> refresh_playlists(Source source, uint64_t account_gen);
        Task<void> create_playlist(Source source, std::string name, bool hidden, std::vector<Track> tracks);
        Task<void> rename_playlist(Card playlist, std::string name);
        Task<void> delete_playlist(Card playlist);
        // Adds the songs to the playlist or takes them out, then reads it back: the service's list decides the outcome.
        Task<void> edit_playlist(Card playlist, std::vector<Track> tracks, bool add);

        // Recommendations the listener turned down.
        Task<void> load_dislikes(Source source, DislikeKind kind);
        void set_dislike(Dislike item, bool on);

        // What the platform offers.
        Task<void> load_recommended(Source source);
        Task<void> load_toplists(Source source);
        Task<void> load_daily_lists(Source source);
        Task<void> load_categories(Source source);
        void set_category(Source source, Category category);
        void load_square(Source source);
        void set_release_category(Source source, int category);
        void load_releases(Source source);
        void ensure_collection(Source source);
        Task<void> load_collection(Source source, bool force = true);
        void ensure_recent_plays(Source source);
        Task<void> load_recent_plays(Source source, bool force = false);
        void note_cloud_play(const Track &track, bool settled);
        Task<void> load_radio(Source source);
        // The radio moves on to its next song, fetching more as the end nears. False while waiting for the next batch.
        bool advance_radio(Source source);
        // Fewer songs like this one on the radio.
        void dislike_radio(Track track);

        // Appends a page to a listing.
        void arrived(Listing &listing, Result<Batch> batch);

    private:
        void emit(const Event &event);
        void apply(Source source, Preference kind, const Edit &edit);
        void set(Source source, Preference kind, Track track, Card card, bool on);
        Task<void> write(Source source, Preference kind, std::string id, uint64_t account_gen);
        void apply_playlists(Source source, std::pair<Card, std::vector<Card>> playlists, uint64_t revision);
        Task<void> load_cards(Source source, Listing Shelf::*listing, Task<Result<std::vector<Card>>> request);
        Task<void> fetch_square(Source source, uint64_t gen, std::string category, int offset);
        Task<void> fetch_releases(Source source, int tab, uint64_t gen, std::string category, int offset);

        Env &env_;
        std::array<Shelf, platforms.size()> shelves_;
        std::vector<std::move_only_function<void(const Event &)>> listeners_;
        std::string pop_key_; // the song liked last, and when
        double pop_time_ = -10;
    };

} // namespace lyren
