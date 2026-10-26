#pragma once
#include <cstdint>
#include <string>
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
    std::string id;
    std::string title, alias;  // alias: translated or alternative title, shown dimmed
    std::string artist;        // all artists, joined with " / "
    std::string album;
    std::string artist_id, album_id;  // the first artist
    std::string cover;  // base URL; Provider::cover_url sizes it
    int duration = 0;   // seconds
    bool cloud = false;  // uploaded to the user's NetEase cloud drive
};

inline bool same(const Track& a, const Track& b) { return a.source == b.source && a.id == b.id; }

enum class CardKind : uint8_t { playlist, album, artist };

// An album, artist or playlist tile.
struct Card {
    CardKind kind = CardKind::playlist;
    Source source = Source::netease;
    std::string id, title, subtitle, cover;
    int count = 0;  // tracks behind it, if known
    bool owned = false;  // a playlist the signed-in user created, so it can be edited
};

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

struct Account {
    std::string id, name, avatar;
};

enum class QrState : uint8_t { waiting, scanned, confirmed, expired };

struct QrStatus {
    QrState state = QrState::waiting;
    std::string nickname, avatar;  // once scanned
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
    virtual std::string cover_url(const std::string& base, int px) const = 0;

    virtual bool logged_in() const = 0;
    // Returns the text to encode in the QR code.
    virtual Task<Result<std::string>> qr_start() = 0;
    virtual Task<Result<QrStatus>> qr_poll() = 0;
    virtual Task<Result<Account>> account() = 0;
    virtual Task<void> logout() = 0;
};

}  // namespace lyren
