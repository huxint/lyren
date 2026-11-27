#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "core/string_map.hpp"
#include "core/task.hpp"
#include "media/lyrics.hpp"
#include "media/quality.hpp"
#include "media/types.hpp"

namespace lyren {

    // Only expose actions supported by the platform's protocol.
    struct Capabilities {
        std::array<bool, 5> search{true, true, true, true, false}; // tracks, albums, artists, playlists, users
        bool supports_search(SearchKind kind) const {
            const auto i = std::to_underlying(kind);
            return i < search.size() && search[i];
        }
        bool toplists = false, square = false, radio = false, radio_dislike = false;
        bool similar = false, artist_intro = false;
        bool library = false, likes = false;
        bool playlist_edit = false, playlist_order = false, private_playlists = false;
        bool collect_playlists = false, collect_albums = false, follow_artists = false;
        std::array<bool, 4> comments{}; // song, album, playlist, post
        bool comment_sort = false, comment_replies = false, comment_likes = false, comment_post = false, comment_delete = false;
        bool accounts = true; // false for accountless sources such as a local music library
        bool qr_login = false, session_import = false;
        // Listener pages: other people's (the signed-in account's own page needs none of these), follow lists,
        // following people, posts and writing them, the listening ranking, and other people's playlists.
        bool profiles = false, people = false, follow_users = false, posts = false, post_write = false, ranking = false, profile_playlists = false;
    };

    // Platform-specific features. Shared capabilities belong to Provider's virtual interface.
    struct Features {
        // Daily recommendations: songs, playlists and "not interested".
        struct Daily {
            std::move_only_function<Task<Result<std::vector<Track>>>()> songs;
            std::move_only_function<Task<Result<std::vector<Card>>>()> playlists;
            std::move_only_function<Task<Result<std::optional<Track>>>(std::string)> dislike;
        };
        // Recommendation exclusions: the stored list and its add / restore write.
        struct Exclusions {
            std::move_only_function<Task<Result<std::vector<Dislike>>>(DislikeKind)> list;
            std::move_only_function<Task<Result<void>>(Dislike, bool)> set;
        };
        // Listening reports: start (recent plays) and play (listening time).
        struct Reporting {
            std::move_only_function<Task<void>(std::string, std::string)> start;
            std::move_only_function<Task<void>(std::string, std::string, int)> play;
        };
        Daily daily;
        Exclusions exclusions;
        Reporting reporting;
        // Single sources.
        std::move_only_function<Task<Result<std::vector<Track>>>()> cloud, recent_plays, radar;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string, std::string)> heartbeat;
    };

    // Step-by-step diagnostics for imported platform connections (signing service, device identity, account session).
    struct SessionCheck {
        enum class State : uint8_t {
            pending,
            ok,
            skipped,
            failed
        };
        struct Step {
            State state = State::pending;
            std::string detail;
        };
        Step signer;
        Step identity;
        Step account;
        bool ok() const {
            return signer.state == State::ok && identity.state == State::ok && (account.state == State::ok || account.state == State::skipped);
        }
    };

    // The shared platform contract; adapters supply their optional features without changing the shell.
    class Provider {
    public:
        virtual ~Provider() = default;

        Features features;
        Capabilities capabilities;
        std::string_view radio_title = "Personal FM";
        std::array<std::vector<Category>, 2> release_categories; // songs, albums
        bool reload_after_edit = false;                          // server-assigned playlist metadata and order need a readback
        virtual StringMap<std::string> &session();
        virtual void on_session_changed(std::move_only_function<void()> changed);
        virtual Task<Result<void>> initialize();
        virtual void cancel_login() {}

        virtual Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) = 0;
        virtual Task<Result<std::vector<std::string>>> search_suggest(std::string query);
        virtual Task<Result<Stream>> stream(Track track, Quality quality) = 0;
        virtual Task<Result<std::vector<LyricLine>>> lyrics(Track track) = 0;
        // A local timed LRC for this platform and track takes precedence over the service's lyrics.
        Task<Result<std::vector<LyricLine>>> playback_lyrics(Track track, std::filesystem::path lyric_dir);
        // The tracks of a playlist, an album, or an artist's top songs.
        virtual Task<Result<std::vector<Track>>> tracks(Card card) = 0;

        virtual Task<Result<std::vector<Card>>> recommended_playlists();
        virtual Task<Result<std::vector<Card>>> toplists();
        virtual Task<Result<std::vector<Track>>> fm();
        virtual Task<Result<void>> fm_trash(Track track);
        virtual Task<Result<std::vector<Track>>> new_songs(std::string area);
        virtual Task<Result<Batch>> new_albums(std::string area, int offset);
        virtual Task<Result<Categories>> categories();
        virtual Task<Result<Batch>> square(std::string category, int offset);
        virtual Task<Result<std::vector<Track>>> similar(std::string id);
        virtual Task<Result<AlbumInfo>> album(std::string id);
        virtual Task<Result<ArtistInfo>> artist(std::string id);
        virtual Task<Result<Batch>> artist_songs(std::string id, int offset);
        virtual Task<Result<Batch>> artist_albums(std::string id, int offset);
        // Headings and their text; the summary comes first, without a heading.
        virtual Task<Result<std::vector<std::pair<std::string, std::string>>>> artist_intro(std::string id);
        // page counts from 1; cursor is what the page before returned, empty for the first.
        virtual Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor);
        virtual Task<Result<CommentPage>> comment_replies(Subject subject, std::string id, std::string root, int page, std::string cursor);
        virtual Task<Result<void>> like_comment(Subject subject, std::string id, std::string comment, bool on);
        // Empty reply ID posts a new comment; returns the service-assigned comment ID.
        virtual Task<Result<std::string>> post_comment(Subject subject, std::string id, std::string content, std::string reply = {}, std::string root = {});
        virtual Task<Result<void>> delete_comment(Subject subject, std::string id, std::string comment);

        virtual Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid);
        virtual Task<Result<std::vector<std::string>>> liked_ids(std::string uid);
        virtual Task<Result<void>> like(Track track, bool on);
        virtual Task<Result<void>> follow(std::string artist, bool on);
        virtual Task<Result<void>> collect(std::string album, bool on);
        virtual Task<Result<std::vector<Card>>> collected_playlists();
        virtual Task<Result<void>> collect_playlist(std::string id, bool on);
        virtual Task<Result<std::vector<Card>>> collected_albums();
        virtual Task<Result<std::vector<Card>>> followed_artists();
        virtual Task<Result<Card>> create_playlist(std::string name, bool hidden);
        virtual Task<Result<void>> rename_playlist(std::string id, std::string name);
        virtual Task<Result<void>> delete_playlist(std::string id);
        Task<Result<void>> edit_playlist(std::string id, Track track, bool add);
        // One request batch: up to 100 tracks from this platform.
        virtual Task<Result<void>> edit_playlist(std::string id, std::vector<Track> tracks, bool add);
        virtual Task<Result<void>> reorder_playlist(std::string id, std::vector<Track> tracks);

        // Listener pages. uid is the service's user ID; the signed-in account's own is Account::id.
        virtual Task<Result<Profile>> profile(std::string uid);
        // Whom they follow, or their fans; cursor is what the page before returned, empty for the first.
        virtual Task<Result<PeoplePage>> people(std::string uid, bool fans, std::string cursor);
        virtual Task<Result<void>> follow_user(std::string uid, bool on);
        virtual Task<Result<PostPage>> posts(std::string uid, std::string cursor);
        // Posts by the people the signed-in account follows.
        virtual Task<Result<PostPage>> feed(std::string cursor);
        virtual Task<Result<void>> like_post(Post post, bool on);
        virtual Task<Result<void>> delete_post(std::string id);
        // A new post with words and, optionally, a song it shares.
        virtual Task<Result<void>> share(std::string text, std::optional<Track> track);
        virtual Task<Result<void>> forward(Post post, std::string text);
        // all_time: since the account began, otherwise the last week.
        virtual Task<Result<Ranking>> ranking(std::string uid, bool all_time);

        virtual bool logged_in() const;
        // A URL to encode or the login service's PNG.
        virtual Task<Result<QrCode>> qr_start();
        virtual Task<Result<QrStatus>> qr_poll();
        virtual Task<Result<void>> import_session(std::string json);
        virtual Task<SessionCheck> check_session(std::string json);
        virtual Task<Result<Account>> account();
        virtual Task<void> logout();

    private:
        StringMap<std::string> empty_session_;
    };

    // One registered music source instance (built-in online platform, accountless local library, or self-hosted server).
    struct SourceEntry {
        Source source = Source::netease;
        std::string instance;   // empty for singleton sources; unique slug for multi-instance sources
        std::string id;         // canonical source_id(source, instance)
        std::string name;       // display name (or i18n key for built-in platforms)
        std::string short_name; // compact selector label
        std::string scan_hint;
        Provider *provider = nullptr;
        std::shared_ptr<Provider> owned; // set for dynamically added runtime instances
    };

    // Dynamic registry of active music sources. Supports the built-in 5 online platforms as well as accountless
    // local libraries and multiple instances of the same Source kind (e.g. multiple OpenSubsonic servers).
    class ProviderRegistry {
    public:
        ProviderRegistry() = default;
        // Registers the five built-in online platforms in platforms[] order.
        ProviderRegistry(std::initializer_list<std::reference_wrapper<Provider>> builtins);

        // Adds or replaces a source instance; returns its canonical id.
        std::string add(SourceEntry entry);
        std::string add(Source source, Provider &provider, std::string instance = {}, std::string name = {}, std::string short_name = {}) {
            return add(SourceEntry{.source = source, .instance = std::move(instance), .name = std::move(name), .short_name = std::move(short_name), .provider = &provider});
        }
        bool remove(std::string_view id);

        std::span<const SourceEntry> entries() const {
            return entries_;
        }
        bool empty() const {
            return entries_.empty();
        }
        size_t size() const {
            return entries_.size();
        }
        bool contains(Source source, std::string_view instance = {}) const {
            return find(source, instance) != nullptr;
        }
        bool contains(std::string_view id) const {
            return find(id) != nullptr;
        }
        const SourceEntry *entry(std::string_view id) const;
        const SourceEntry *entry(Source source, std::string_view instance = {}) const;
        Provider *find(std::string_view id) const;
        Provider *find(Source source, std::string_view instance = {}) const;
        Provider &get(Source source, std::string_view instance = {}) const;
        Provider &get(std::string_view id) const;

    private:
        std::vector<SourceEntry> entries_;
    };

    using Providers = ProviderRegistry;

} // namespace lyren
