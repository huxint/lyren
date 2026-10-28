#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/lyrics.hpp"
#include "core/result.hpp"
#include "core/task.hpp"
#include "sys/config.hpp"

namespace lyren {

    // Stable identities for persisted tracks and the platform registry below.
    enum class Source : uint8_t {
        netease,
        qq
    };

    struct Platform {
        Source source;
        std::string_view id, name, short_name, scan_hint;
    };

    inline constexpr std::array platforms{
        Platform{Source::netease, "netease", "NetEase Music", "NetEase", "Scan with the NetEase Music app"},
        Platform{Source::qq, "qq", "QQ Music", "QQ Music", "Scan with QQ, then confirm QQ Music sign-in"},
    };

    inline const Platform &platform(Source source) {
        return platforms[static_cast<size_t>(source)];
    }

    struct Track {
        Source source = Source::netease;
        std::string id;           // QQ Music: the numeric song id
        std::string mid, media;   // QQ Music: the song's mid and its audio file's
        std::string title, alias; // alias: translated or alternative title, shown dimmed
        std::string artist;       // all artists, joined with " / "
        std::string album;
        std::string artist_id, album_id; // the first artist; QQ Music: mids
        std::string cover;               // cover_url() sizes it
        int duration = 0;                // seconds
        int type = 0;                    // QQ Music's song type, also used when editing a playlist
        bool cloud = false;              // uploaded to the user's NetEase cloud drive
        bool paid = false;               // the listing says more than a preview takes a membership or a purchase
    };

    inline bool same(const Track &a, const Track &b) {
        return a.source == b.source && a.id == b.id;
    }

    inline std::string key(Source source, std::string_view id) {
        return std::string(platform(source).id) + ":" + std::string(id);
    }
    inline std::string key(const Track &t) {
        return key(t.source, t.id);
    }

    // A cover, avatar or playlist picture at the service's size closest to px.
    std::string cover_url(std::string_view url, int px);

    enum class CardKind : uint8_t {
        playlist,
        album,
        artist
    };

    // An album, artist or playlist tile.
    struct Card {
        enum class Count : uint8_t {
            none,
            songs,
            albums
        };
        CardKind kind = CardKind::playlist;
        Source source = Source::netease;
        std::string id, title, subtitle, cover;
        std::string thread; // an owned QQ directory's public playlist id, for comments
        int count = 0;      // tracks behind it, if known
        Count count_kind = Count::none;
        bool owned = false;      // a playlist the signed-in user created, so it can be edited
        bool commentable = true; // private directories may not have a public comment thread
    };

    std::string card_subtitle(const Card &card);

    inline bool same(const Card &a, const Card &b) {
        return a.source == b.source && a.kind == b.kind && a.id == b.id;
    }
    inline std::string key(const Card &c) {
        return key(c.source, c.id) + ":" + std::to_string(static_cast<int>(c.kind));
    }

    enum class SearchKind : uint8_t {
        tracks,
        albums,
        artists,
        playlists
    };

    // One page of a longer listing: search results, an artist's songs or albums, the playlist square.
    struct Batch {
        std::vector<Track> tracks;
        std::vector<Card> cards;
        int total = 0;
    };

    // Round-robin rankings from every registered platform, retaining each listing and its identity.
    template <class T, size_t N>
    std::vector<T> interleave(const std::array<Batch, N> &sources, std::vector<T> Batch::*rows) {
        size_t count = 0;
        for (const auto &source : sources) {
            count += (source.*rows).size();
        }
        std::vector<T> out;
        out.reserve(count);
        for (size_t rank = 0; out.size() < count; ++rank) {
            for (const auto &source : sources) {
                if (rank < (source.*rows).size()) {
                    out.push_back((source.*rows)[rank]);
                }
            }
        }
        return out;
    }

    struct Category {
        std::string id; // empty for all categories; independent of the displayed name
        std::string name = "All";
    };

    // Playlist square categories, under their headings.
    struct Categories {
        std::vector<Category> hot;
        std::vector<std::pair<std::string, std::vector<Category>>> groups;
    };

    // The head of an artist's page.
    struct ArtistInfo {
        std::string name, alias, avatar;
        int songs = 0, albums = 0;
        bool followed = false;
    };

    struct AlbumInfo {
        std::string artist, artist_id, date, company, about;
        std::string thread; // what its comments are filed under (QQ Music: the numeric id)
        bool collected = false;
        std::vector<Track> tracks;
    };

    // What a comment thread is about.
    enum class Subject : uint8_t {
        song,
        album,
        playlist
    };

    struct Comment {
        std::string user, avatar, text, time; // time as the service words it
        int likes = 0;
        std::string reply_user, reply; // the comment this one answers
    };

    struct CommentPage {
        std::vector<Comment> comments;
        int total = 0;
        bool more = false;
        std::string cursor; // where the next page starts
    };

    struct Stream {
        std::string url;
        bool trial = false;     // only a preview clip is playable
        double trial_start = 0; // where the clip begins in the song, s
        std::string level;      // the quality the server granted
        float gain = 0;         // loudness correction, dB
        float peak = 0;         // sample peak, 0 when unknown
    };

    struct Account {
        std::string id, name, avatar;
    };

    enum class QrState : uint8_t {
        waiting,
        scanned,
        confirmed,
        expired
    };

    struct QrStatus {
        QrState state = QrState::waiting;
        std::string nickname, avatar; // once scanned
    };

    struct QrCode {
        std::string text;  // NetEase: encode this URL
        std::string image; // QQ: the PNG returned by the login service
    };

    // Optional features expose both availability and the operation. Shared pages only offer installed features.
    struct Features {
        std::move_only_function<Task<Result<std::vector<Track>>>()> daily, fm, cloud, recent_plays;
        std::move_only_function<Task<Result<std::vector<Card>>>()> daily_playlists;
        std::move_only_function<Task<Result<std::optional<Track>>>(std::string)> dislike_daily;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string, std::string)> heartbeat;
        std::move_only_function<Task<Result<std::vector<Track>>>(std::string)> similar;
        std::move_only_function<Task<Result<void>>(std::string)> fm_trash;
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
