#pragma once
#include <array>
#include <functional>
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
        std::array<bool, 4> search{true, true, true, true};
        bool toplists = false, square = false, radio = false, radio_dislike = false;
        bool similar = false, artist_intro = false;
        bool library = false, likes = false;
        bool playlist_edit = false, playlist_order = false, private_playlists = false;
        bool collect_playlists = false, collect_albums = false, follow_artists = false;
        std::array<bool, 3> comments{}; // song, album, playlist
        bool comment_sort = false, comment_replies = false, comment_likes = false, comment_post = false, comment_delete = false;
        bool qr_login = false, session_import = false;
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

    // The shared platform contract; adapters supply their optional features without changing the shell.
    class Provider {
    public:
        virtual ~Provider() = default;

        Features features;
        Capabilities capabilities;
        std::string_view radio_title = "Personal FM";
        std::array<std::vector<Category>, 2> release_categories; // songs, albums
        bool reload_after_edit = false;                          // server-assigned playlist metadata and order need a readback
        virtual StringMap<std::string> &session() = 0;
        virtual void on_session_changed(std::move_only_function<void()> changed) = 0;
        virtual Task<Result<void>> initialize() = 0;
        virtual void cancel_login() {}

        virtual Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) = 0;
        virtual Task<Result<Stream>> stream(Track track, Quality quality) = 0;
        virtual Task<Result<std::vector<LyricLine>>> lyrics(Track track) = 0;
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
        virtual Task<Result<void>> edit_playlist(std::string id, Track track, bool add);
        virtual Task<Result<void>> reorder_playlist(std::string id, std::vector<Track> tracks);

        virtual bool logged_in() const = 0;
        // A URL to encode or the login service's PNG.
        virtual Task<Result<QrCode>> qr_start();
        virtual Task<Result<QrStatus>> qr_poll();
        virtual Task<Result<void>> import_session(std::string json);
        virtual Task<Result<Account>> account() = 0;
        virtual Task<void> logout() = 0;
    };

    // Every registered platform must have an adapter; missing entries fail at compile time.
    using Providers = std::array<std::reference_wrapper<Provider>, platforms.size()>;

} // namespace lyren
