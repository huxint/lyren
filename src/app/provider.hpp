#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app/lyrics.hpp"
#include "core/result.hpp"
#include "core/task.hpp"
#include "sys/config.hpp"

namespace lyren {

// Only the data layer looks at the source, to pick the provider that plays a track.
enum class Source : uint8_t { netease, qq };

struct Track {
    Source source = Source::netease;
    std::string id;          // QQ Music: the numeric song id
    std::string mid, media;  // QQ Music: the song's mid and its audio file's
    std::string title, alias;  // alias: translated or alternative title, shown dimmed
    std::string artist;        // all artists, joined with " / "
    std::string album;
    std::string artist_id, album_id;  // the first artist; QQ Music: mids
    std::string cover;  // cover_url() sizes it
    int duration = 0;   // seconds
    int type = 0;      // QQ Music's song type, also used when editing a playlist
    bool cloud = false;  // uploaded to the user's NetEase cloud drive
    bool paid = false;   // the listing says more than a preview takes a membership or a purchase
    std::vector<Track> alts;  // the same song on the other service, to fall back on
};

// The same song, also when one of them is the other's stand-in from the other service.
inline bool same(const Track& a, const Track& b) {
    auto one = [](const Track& x, const Track& y) { return x.source == y.source && x.id == y.id; };
    return one(a, b) || std::ranges::any_of(a.alts, [&](const Track& x) { return one(x, b); }) ||
           std::ranges::any_of(b.alts, [&](const Track& x) {
               return one(a, x) || std::ranges::any_of(a.alts, [&](const Track& y) { return one(x, y); });
           });
}

inline std::string key(Source source, std::string_view id) { return (source == Source::netease ? "ncm:" : "qq:") + std::string(id); }
inline std::string key(const Track& t) { return key(t.source, t.id); }

// Interleave the services' rankings, keeping each service's order.
template <class T>
std::vector<T> interleave(const std::vector<T>& a, const std::vector<T>& b) {
    std::vector<T> out;
    out.reserve(a.size() + b.size());
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        if (i < a.size()) out.push_back(a[i]);
        if (i < b.size()) out.push_back(b[i]);
    }
    return out;
}

// Matching title, artists and duration (within 3 s); the other recording remains available for playback.
std::vector<Track> merge_tracks(const std::vector<Track>& a, const std::vector<Track>& b);

// A cover, avatar or playlist picture at the service's size closest to px.
std::string cover_url(std::string_view url, int px);

enum class CardKind : uint8_t { playlist, album, artist };

// An album, artist or playlist tile.
struct Card {
    CardKind kind = CardKind::playlist;
    Source source = Source::netease;
    std::string id, title, subtitle, cover;
    std::string thread;  // an owned QQ directory's public playlist id, for comments
    int count = 0;  // tracks behind it, if known
    bool owned = false;  // a playlist the signed-in user created, so it can be edited
};

inline bool same(const Card& a, const Card& b) { return a.source == b.source && a.kind == b.kind && a.id == b.id; }
inline std::string key(const Card& c) { return key(c.source, c.id) + ":" + std::to_string(int(c.kind)); }

enum class SearchKind : uint8_t { tracks, albums, artists, playlists };

// One page of a longer listing: search results, an artist's songs or albums, the playlist square.
struct Batch {
    std::vector<Track> tracks;
    std::vector<Card> cards;
    int total = 0;
};

// The head of an artist's page.
struct ArtistInfo {
    std::string name, alias, avatar;
    int songs = 0, albums = 0;
    bool followed = false;
};

struct AlbumInfo {
    std::string artist, artist_id, date, company, about;
    std::string thread;  // what its comments are filed under (QQ Music: the numeric id)
    bool collected = false;
    std::vector<Track> tracks;
};

// What a comment thread is about.
enum class Subject : uint8_t { song, album, playlist };

struct Comment {
    std::string user, avatar, text, time;  // time as the service words it
    int likes = 0;
    std::string reply_user, reply;  // the comment this one answers
};

struct CommentPage {
    std::vector<Comment> comments;
    int total = 0;
    bool more = false;
    std::string cursor;  // where the next page starts
};

struct Stream {
    std::string url;
    bool trial = false;  // only a preview clip is playable
    double trial_start = 0;  // where the clip begins in the song, s
    std::string level;   // the quality the server granted
    float gain = 0;      // loudness correction, dB
    float peak = 0;      // sample peak, 0 when unknown
};

struct Playback {
    Track track;  // the recording that actually plays, with its alternatives retained
    Stream stream;
};

// Prefer a full stream; use a preview only after trying every available recording.
Task<Result<Playback>> resolve_stream(Track track, Quality quality, std::function<Task<Result<Stream>>(Track, Quality)> fetch);

struct Account {
    std::string id, name, avatar;
};

enum class QrState : uint8_t { waiting, scanned, confirmed, expired };

struct QrStatus {
    QrState state = QrState::waiting;
    std::string nickname, avatar;  // once scanned
};

struct QrCode {
    std::string text;   // NetEase: encode this URL
    std::string image;  // QQ: the PNG returned by the login service
};

// What both services can do. Service-specific features are called on the concrete class.
class Provider {
public:
    virtual ~Provider() = default;

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

}  // namespace lyren
