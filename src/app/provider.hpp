#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
};

enum class SearchKind : uint8_t { tracks, albums, artists, playlists };

struct SearchPage {
    std::vector<Track> tracks;
    std::vector<Card> cards;
    int total = 0;
};

struct Stream {
    std::string url;
    bool trial = false;  // only a preview clip is playable
    double trial_start = 0;  // where the clip begins in the song, s
    std::string level;   // the quality the server granted
};

struct Lyrics {
    std::string lrc, translation;  // LRC text
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

    virtual Task<Result<SearchPage>> search(std::string query, SearchKind kind, int offset, int limit) = 0;
    virtual Task<Result<Stream>> stream(Track track, Quality quality) = 0;
    virtual Task<Result<Lyrics>> lyrics(Track track) = 0;
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
