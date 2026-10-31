#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lyren {

    // Stable persisted identifiers: [netease, qq], also the indices into platforms.
    enum class Source : uint8_t {
        netease,
        qq
    };

    // Stable state-file values: list = 0, one = 1, shuffle = 2.
    enum class PlayMode : uint8_t {
        list,
        one,
        shuffle
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
        std::string id;           // QQ Music: the numeric song id
        std::string mid, media;   // QQ Music: the song's mid and its audio file's
        std::string title, alias; // alias: translated or alternative title, shown dimmed
        std::string artist;       // all artists, joined with " / "
        std::string album;
        std::string artist_id, album_id; // the first artist; QQ Music: mids
        std::string cover;               // cover_url() sizes it
        uint32_t duration = 0;           // whole seconds; zero when the service supplies no duration
        int type = 0;                    // QQ Music's song type, also used when editing a playlist
        Source source = Source::netease;
        bool cloud = false; // uploaded to the user's NetEase cloud drive
        bool paid = false;  // the listing says more than a preview takes a membership or a purchase
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
    // A picture whose whole frame matters, such as one attached to a comment, about px on its longer side when the
    // service can scale it.
    std::string picture_url(std::string_view url, int px);
    std::string share_url(const Track &track);

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
        bool owned = false;       // a playlist the signed-in user created, so it can be edited
        bool commentable = true;  // private directories may not have a public comment thread
        bool collectable = false; // a public playlist that can be saved to the account
    };

    std::string card_subtitle(const Card &card);
    std::string share_url(const Card &card);

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
        int total = 0;        // nonnegative service count; may be an estimate while more pages remain
        int next_offset = -1; // search continuation; -1 ends the results, independent of the visible row count
    };

    struct Category {
        std::string id; // empty for all categories; independent of the displayed name
        std::string name = "All";
    };

    enum class DislikeKind : uint8_t {
        song,
        artist,
        style
    };

    struct Dislike {
        std::string id, name, cover;
        int id_type = 0; // the service's ID type, retained when restoring an item
        Source source = Source::netease;
        DislikeKind kind = DislikeKind::song;
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
        std::string id;
        std::string user, avatar, text, time; // time as the service words it
        std::string reply_user, reply;        // the comment this one answers
        std::string image;                    // an attached picture, or a sticker
        int likes = 0, replies = 0;
        int image_w = 0, image_h = 0; // the picture's size when the service gives it
        bool liked = false;
        bool owned = false; // the signed-in account can delete it
        bool sticker = false;
    };

    struct CommentPage {
        std::vector<Comment> comments;
        int total = 0;
        bool more = false;
        std::string cursor; // where the next page starts
    };

    struct Stream {
        std::string url;
        std::string level;                           // the quality the server granted
        double trial_start = 0;                      // nonnegative finite song seconds (QQ's RS02 file: always 0)
        double highlight_from = 0, highlight_to = 0; // the service's chorus window in the song, s; empty when the end is not after the start
        float gain = 0;                              // finite loudness correction, dB
        float peak = 0;                              // nonnegative finite sample peak, 0 when unknown
        bool trial = false;                          // only a preview clip is playable
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

} // namespace lyren
