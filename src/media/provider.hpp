#pragma once
#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "core/task.hpp"
#include "media/lyrics.hpp"
#include "media/quality.hpp"
#include "media/types.hpp"

namespace lyren {

    // Optional features expose both availability and the operation. Shared pages only offer installed features.
    struct Features {
        std::move_only_function<Task<Result<std::vector<Track>>>()> daily, fm, cloud, recent_plays;
        std::string_view fm_title = "Personal FM";
        std::move_only_function<Task<Result<std::vector<Track>>>()> radar;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string)> new_songs;
        std::move_only_function<Task<Result<Batch>>(std::string, int)> new_albums;
        std::array<std::vector<Category>, 2> release_categories;
        std::move_only_function<Task<Result<std::vector<Card>>>()> daily_playlists;
        std::move_only_function<Task<Result<std::vector<Card>>>()> collected_playlists;
        std::move_only_function<Task<Result<void>>(std::string, bool)> collect_playlist;
        std::move_only_function<Task<Result<std::optional<Track>>>(std::string)> dislike_daily;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string, std::string)> heartbeat;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string)> similar;
        std::move_only_function<Task<Result<void>>(Track)> fm_trash;
        std::move_only_function<Task<Result<std::vector<Dislike>>>(DislikeKind)> dislikes;
        std::move_only_function<Task<Result<void>>(Dislike, bool)> dislike;
        std::move_only_function<Task<Result<void>>(std::string, std::vector<Track>)> reorder_playlist;
        std::move_only_function<Task<Result<Categories>>()> categories;
        std::move_only_function<Task<Result<Batch>>(std::string, int)> square;
        std::move_only_function<Task<void>(std::string, std::string)> report_start;
        std::move_only_function<Task<void>(std::string, std::string, int)> report_play;
    };

    // The shared platform contract; adapters supply their optional features without changing the shell.
    class Provider {
    public:
        virtual ~Provider() = default;

        Features features;
        bool reload_after_edit = false; // server-assigned playlist metadata and order need a readback
        virtual std::map<std::string, std::string> &session() = 0;
        virtual void on_session_changed(std::move_only_function<void()> changed) = 0;
        virtual Task<Result<void>> initialize() = 0;
        virtual void cancel_qr() = 0;

        virtual Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) = 0;
        virtual Task<Result<Stream>> stream(Track track, Quality quality) = 0;
        virtual Task<Result<std::vector<LyricLine>>> lyrics(Track track) = 0;
        // The tracks of a playlist, an album, or an artist's top songs.
        virtual Task<Result<std::vector<Track>>> tracks(Card card) = 0;

        virtual Task<Result<std::vector<Card>>> recommended_playlists() = 0;
        virtual Task<Result<std::vector<Card>>> toplists() = 0;
        virtual Task<Result<AlbumInfo>> album(std::string id) = 0;
        virtual Task<Result<ArtistInfo>> artist(std::string id) = 0;
        virtual Task<Result<Batch>> artist_songs(std::string id, int offset) = 0;
        virtual Task<Result<Batch>> artist_albums(std::string id, int offset) = 0;
        // Headings and their text; the summary comes first, without a heading.
        virtual Task<Result<std::vector<std::pair<std::string, std::string>>>> artist_intro(std::string id) = 0;
        // page counts from 1; cursor is what the page before returned, empty for the first.
        virtual Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) = 0;

        virtual Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) = 0;
        virtual Task<Result<std::vector<std::string>>> liked_ids(std::string uid) = 0;
        virtual Task<Result<void>> like(Track track, bool on) = 0;
        virtual Task<Result<void>> follow(std::string artist, bool on) = 0;
        virtual Task<Result<void>> collect(std::string album, bool on) = 0;
        virtual Task<Result<std::vector<Card>>> collected_albums() = 0;
        virtual Task<Result<std::vector<Card>>> followed_artists() = 0;
        virtual Task<Result<Card>> create_playlist(std::string name, bool hidden) = 0;
        virtual Task<Result<void>> rename_playlist(std::string id, std::string name) = 0;
        virtual Task<Result<void>> delete_playlist(std::string id) = 0;
        virtual Task<Result<void>> edit_playlist(std::string id, Track track, bool add) = 0;

        virtual bool logged_in() const = 0;
        // A URL to encode or the login service's PNG.
        virtual Task<Result<QrCode>> qr_start() = 0;
        virtual Task<Result<QrStatus>> qr_poll() = 0;
        virtual Task<Result<Account>> account() = 0;
        virtual Task<void> logout() = 0;
    };

    using Providers = std::array<Provider *, platforms.size()>;

} // namespace lyren
