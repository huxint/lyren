#include "ncm/netease.hpp"

#include <glaze/glaze.hpp>

#include <format>

#include "core/log.hpp"

namespace lyren::ncm {

// Response models. NetEase sends null for many fields, so most are optional; unknown keys are skipped.
namespace {

using Str = std::optional<std::string>;
using Strs = std::optional<std::vector<std::string>>;

struct NArtist {
    uint64_t id = 0;
    Str name;
};
struct NAlbum {
    uint64_t id = 0;
    Str name, picUrl;
};
// Newer endpoints use ar/al/dt, older ones artists/album/duration.
struct NSong {
    uint64_t id = 0;
    Str name;
    std::optional<std::vector<NArtist>> ar, artists;
    std::optional<NAlbum> al, album;
    double dt = 0, duration = 0;
    Strs alia, tns, alias, transNames;
};
struct NCreator {
    Str nickname;
    uint64_t userId = 0;
};
struct NPlaylist {
    uint64_t id = 0;
    Str name, coverImgUrl, picUrl, updateFrequency;
    double trackCount = 0;
    std::optional<NCreator> creator;
    int specialType = 0;
    std::optional<bool> subscribed;
};
struct NAlbumInfo {
    uint64_t id = 0;
    Str name, picUrl;
    std::optional<NArtist> artist;
    double publishTime = 0, size = 0;
};
struct NArtistInfo {
    uint64_t id = 0;
    Str name, picUrl, img1v1Url;
    double musicSize = 0, albumSize = 0;
};

struct SearchResult {
    std::optional<std::vector<NSong>> songs;
    std::optional<std::vector<NAlbumInfo>> albums;
    std::optional<std::vector<NArtistInfo>> artists;
    std::optional<std::vector<NPlaylist>> playlists;
    double songCount = 0, albumCount = 0, artistCount = 0, playlistCount = 0;
};
struct SearchReply {
    std::optional<SearchResult> result;
};

struct NTrial {
    double start = 0, end = 0;
};
struct NUrl {
    Str url, level;
    std::optional<NTrial> freeTrialInfo;
};
struct UrlReply {
    std::optional<std::vector<NUrl>> data;
};

struct NLyric {
    Str lyric;
};
struct LyricReply {
    std::optional<NLyric> lrc, tlyric;
};

struct NTrackId {
    uint64_t id = 0;
};
struct NPlaylistDetail {
    std::optional<std::vector<NSong>> tracks;
    std::optional<std::vector<NTrackId>> trackIds;
};
struct PlaylistReply {
    std::optional<NPlaylistDetail> playlist;
};
struct SongsReply {
    std::optional<std::vector<NSong>> songs;
};
struct AlbumReply {
    std::optional<std::vector<NSong>> songs;
    std::optional<NAlbum> album;
};
struct ArtistReply {
    std::optional<std::vector<NSong>> hotSongs;
};
struct PlaylistsReply {
    std::optional<std::vector<NPlaylist>> playlist, result, list;
};
struct DailyData {
    std::optional<std::vector<NSong>> dailySongs;
};
struct DailyReply {
    std::optional<DailyData> data;
};
struct FmReply {
    std::optional<std::vector<NSong>> data;
};
struct NCloud {
    std::optional<NSong> simpleSong;
};
struct CloudReply {
    std::optional<std::vector<NCloud>> data;
    std::optional<bool> hasMore;
};
struct IdsReply {
    std::optional<std::vector<uint64_t>> ids;
};
struct QrKeyReply {
    Str unikey;
};
struct QrCheckReply {
    Str nickname, avatarUrl;
};
struct NProfile {
    uint64_t userId = 0;
    Str nickname, avatarUrl;
};
struct AccountReply {
    std::optional<NProfile> profile;
};

constexpr glz::opts lax{.error_on_unknown_keys = false};

template <class T>
Result<T> parse(const Reply& r) {
    if (!r.error.empty()) return std::unexpected(r.error);
    T v{};
    if (auto e = glz::read<lax>(v, r.body)) {
        log::warn("ncm: cannot parse reply: {}", glz::format_error(e, r.body).substr(0, 200));
        return std::unexpected("服务器返回的数据无法解析");
    }
    return v;
}

std::string https(std::string url) {
    if (url.starts_with("http://")) url.insert(4, "s");
    return url;
}

Track track(const NSong& s) {
    Track t{.id = std::to_string(s.id), .title = s.name.value_or("")};
    const auto& ar = s.ar ? s.ar : s.artists;
    if (ar) {
        for (const auto& a : *ar) {
            if (!t.artist.empty()) t.artist += " / ";
            t.artist += a.name.value_or("");
        }
        if (!ar->empty()) t.artist_id = std::to_string(ar->front().id);
    }
    if (const auto& al = s.al ? s.al : s.album) {
        t.album = al->name.value_or("");
        t.album_id = std::to_string(al->id);
        t.cover = https(al->picUrl.value_or(""));
    }
    for (const Strs* names : {&s.tns, &s.transNames, &s.alia, &s.alias})
        if (*names && !(*names)->empty()) {
            t.alias = (*names)->front();
            break;
        }
    t.duration = int((s.dt > 0 ? s.dt : s.duration) / 1000);
    return t;
}

std::vector<Track> tracks_of(const std::optional<std::vector<NSong>>& songs) {
    std::vector<Track> out;
    if (songs)
        for (const auto& s : *songs) out.push_back(track(s));
    return out;
}

Card playlist_card(const NPlaylist& p) {
    Card c{.kind = CardKind::playlist, .id = std::to_string(p.id), .title = p.name.value_or("")};
    c.cover = https(p.coverImgUrl.value_or(p.picUrl.value_or("")));
    c.count = int(p.trackCount);
    c.subtitle = p.updateFrequency ? *p.updateFrequency : std::format("{} 首", c.count);
    return c;
}

std::string year_of(double ms) {
    if (ms <= 0) return {};
    std::time_t t = std::time_t(ms / 1000);
    std::tm tm;
    localtime_r(&t, &tm);
    return std::to_string(tm.tm_year + 1900);
}

}  // namespace

Task<Result<SearchPage>> Netease::search(std::string query, SearchKind kind, int offset, int limit) {
    static constexpr int types[] = {1, 10, 100, 1000};
    auto r = parse<SearchReply>(co_await client.call(
        "/api/cloudsearch/pc", std::format(R"("s":{},"type":{},"limit":{},"offset":{},"total":true)", json_string(query), types[int(kind)], limit, offset)));
    if (!r) co_return std::unexpected(r.error());
    SearchPage page;
    if (!r->result) co_return page;
    const SearchResult& res = *r->result;
    switch (kind) {
        case SearchKind::tracks:
            page.tracks = tracks_of(res.songs);
            page.total = int(res.songCount);
            break;
        case SearchKind::albums:
            if (res.albums)
                for (const auto& a : *res.albums) {
                    std::string artist = a.artist ? a.artist->name.value_or("") : "", year = year_of(a.publishTime);
                    page.cards.push_back({.kind = CardKind::album,
                                          .id = std::to_string(a.id),
                                          .title = a.name.value_or(""),
                                          .subtitle = year.empty() ? artist : std::format("{} · {}", artist, year),
                                          .cover = https(a.picUrl.value_or("")),
                                          .count = int(a.size)});
                }
            page.total = int(res.albumCount);
            break;
        case SearchKind::artists:
            if (res.artists)
                for (const auto& a : *res.artists)
                    page.cards.push_back({.kind = CardKind::artist,
                                          .id = std::to_string(a.id),
                                          .title = a.name.value_or(""),
                                          .subtitle = a.musicSize > 0 ? std::format("单曲 {}", int(a.musicSize)) : "",
                                          .cover = https(a.picUrl.value_or(a.img1v1Url.value_or("")))});
            page.total = int(res.artistCount);
            break;
        case SearchKind::playlists:
            if (res.playlists)
                for (const auto& p : *res.playlists) {
                    Card c = playlist_card(p);
                    if (p.creator && p.creator->nickname) c.subtitle = std::format("{} 首 · {}", c.count, *p.creator->nickname);
                    page.cards.push_back(std::move(c));
                }
            page.total = int(res.playlistCount);
            break;
    }
    co_return page;
}

Task<Result<Stream>> Netease::stream(Track t, Quality quality) {
    static constexpr const char* levels[] = {"standard", "higher", "exhigh", "lossless", "hires"};
    // Cloud-drive uploads only resolve through the older endpoint.
    Reply reply;
    if (t.cloud) reply = co_await client.call("/api/song/enhance/player/url", std::format(R"("ids":"[{}]","br":999000)", t.id));
    else
        reply = co_await client.call("/api/song/enhance/player/url/v1",
                                     std::format(R"("ids":"[{}]","level":"{}","encodeType":"flac")", t.id, levels[int(quality)]));
    auto r = parse<UrlReply>(reply);
    if (!r) co_return std::unexpected(r.error());
    if (!r->data || r->data->empty() || !r->data->front().url || r->data->front().url->empty())
        co_return std::unexpected("这首歌暂时不能播放（需要会员或没有版权）");
    const NUrl& u = r->data->front();
    co_return Stream{.url = https(*u.url),
                     .trial = u.freeTrialInfo.has_value(),
                     .trial_start = u.freeTrialInfo ? u.freeTrialInfo->start : 0,
                     .level = u.level.value_or("")};
}

Task<Result<Lyrics>> Netease::lyrics(Track t) {
    auto r = parse<LyricReply>(
        co_await client.call("/api/song/lyric/v1", std::format(R"("id":{},"cp":false,"tv":0,"lv":0,"rv":0,"kv":0,"yv":0,"ytv":0,"yrv":0)", t.id)));
    if (!r) co_return std::unexpected(r.error());
    Lyrics l;
    if (r->lrc) l.lrc = r->lrc->lyric.value_or("");
    if (r->tlyric) l.translation = r->tlyric->lyric.value_or("");
    co_return l;
}

Task<Result<std::vector<Track>>> Netease::tracks(Card card) {
    if (card.kind == CardKind::album) {
        auto r = parse<AlbumReply>(co_await client.call("/api/v1/album/" + card.id, "", Api::weapi));
        if (!r) co_return std::unexpected(r.error());
        auto out = tracks_of(r->songs);
        // Album tracks often come without a cover of their own.
        std::string cover = r->album ? https(r->album->picUrl.value_or("")) : "";
        for (auto& t : out)
            if (t.cover.empty()) t.cover = cover;
        co_return out;
    }
    if (card.kind == CardKind::artist) {
        auto r = parse<ArtistReply>(co_await client.call("/api/v1/artist/" + card.id, "", Api::weapi));
        if (!r) co_return std::unexpected(r.error());
        co_return tracks_of(r->hotSongs);
    }
    auto r = parse<PlaylistReply>(co_await client.call("/api/v6/playlist/detail", std::format(R"("id":{},"n":100000,"s":8)", card.id)));
    if (!r) co_return std::unexpected(r.error());
    if (!r->playlist) co_return std::vector<Track>{};
    auto out = tracks_of(r->playlist->tracks);
    // Long playlists list every id but only the first tracks in full.
    const auto& ids = r->playlist->trackIds;
    if (ids && ids->size() > out.size()) {
        std::vector<std::string> rest;
        for (size_t i = out.size(); i < ids->size(); ++i) rest.push_back(std::to_string((*ids)[i].id));
        auto more = co_await song_detail(std::move(rest));
        if (!more) co_return std::unexpected(more.error());
        out.insert(out.end(), std::make_move_iterator(more->begin()), std::make_move_iterator(more->end()));
    }
    co_return out;
}

Task<Result<std::vector<Track>>> Netease::song_detail(std::vector<std::string> ids) {
    std::vector<Track> out;
    for (size_t i = 0; i < ids.size(); i += 500) {
        std::string c = "[";
        for (size_t k = i; k < std::min(ids.size(), i + 500); ++k) c += std::format("{}{{\"id\":{}}}", k > i ? "," : "", ids[k]);
        c += "]";
        auto r = parse<SongsReply>(co_await client.call("/api/v3/song/detail", std::format(R"("c":{})", json_string(c)), Api::weapi));
        if (!r) co_return std::unexpected(r.error());
        auto part = tracks_of(r->songs);
        out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    co_return out;
}

std::string Netease::cover_url(const std::string& base, int px) const {
    if (base.empty()) return {};
    // Some covers carry their own thumbnail and watermark parameters; the plain image is enough.
    return std::format("{}?param={}y{}", std::string_view(base).substr(0, base.find('?')), px, px);
}

Task<Result<std::string>> Netease::qr_start() {
    auto r = parse<QrKeyReply>(co_await client.call("/api/login/qrcode/unikey", R"("type":3)"));
    if (!r) co_return std::unexpected(r.error());
    if (!r->unikey) co_return std::unexpected("没有拿到二维码");
    qr_key_ = *r->unikey;
    co_return "https://music.163.com/login?codekey=" + qr_key_;
}

Task<Result<QrStatus>> Netease::qr_poll() {
    Reply reply = co_await client.call("/api/login/qrcode/client/login", std::format(R"("key":{},"type":3)", json_string(qr_key_)));
    auto r = parse<QrCheckReply>(reply);
    if (!r) co_return std::unexpected(r.error());
    QrStatus s;
    switch (reply.code) {
        case 800: s.state = QrState::expired; break;
        case 801: s.state = QrState::waiting; break;
        case 802:
            s.state = QrState::scanned;
            s.nickname = r->nickname.value_or("");
            s.avatar = https(r->avatarUrl.value_or(""));
            break;
        case 803: s.state = QrState::confirmed; break;
        default: co_return std::unexpected(std::format("登录失败（{}）", reply.code));
    }
    co_return s;
}

Task<Result<Account>> Netease::account() {
    Reply reply = co_await client.call("/api/w/nuser/account/get", "", Api::weapi);
    auto r = parse<AccountReply>(reply);
    // QR logins cannot be refreshed; once the session is gone the user has to scan again.
    if (reply.code == 301 || (r && !r->profile)) {
        client.clear_login();
        co_return std::unexpected("网易云音乐的登录已失效，请重新扫码登录");
    }
    if (!r) co_return std::unexpected(r.error());
    co_return Account{.id = std::to_string(r->profile->userId), .name = r->profile->nickname.value_or(""),
                      .avatar = https(r->profile->avatarUrl.value_or(""))};
}

Task<void> Netease::logout() {
    co_await client.call("/api/logout", "");
    client.clear_login();
}

Task<Result<std::vector<Card>>> Netease::recommended_playlists() {
    auto r = parse<PlaylistsReply>(co_await client.call("/api/personalized/playlist", R"("limit":30,"total":true,"n":1000)", Api::weapi));
    if (!r) co_return std::unexpected(r.error());
    std::vector<Card> out;
    if (r->result)
        for (const auto& p : *r->result) out.push_back(playlist_card(p));
    co_return out;
}

Task<Result<std::vector<Card>>> Netease::toplists() {
    auto r = parse<PlaylistsReply>(co_await client.call("/api/toplist", ""));
    if (!r) co_return std::unexpected(r.error());
    std::vector<Card> out;
    if (r->list)
        for (const auto& p : *r->list) out.push_back(playlist_card(p));
    co_return out;
}

Task<Result<std::vector<Track>>> Netease::daily() {
    auto r = parse<DailyReply>(co_await client.call("/api/v3/discovery/recommend/songs", "", Api::weapi));
    if (!r) co_return std::unexpected(r.error());
    co_return r->data ? tracks_of(r->data->dailySongs) : std::vector<Track>{};
}

Task<Result<std::vector<Track>>> Netease::fm() {
    auto r = parse<FmReply>(co_await client.call("/api/v1/radio/get", "", Api::weapi));
    if (!r) co_return std::unexpected(r.error());
    co_return tracks_of(r->data);
}

Task<Result<void>> Netease::fm_trash(std::string id) {
    Reply r = co_await client.call("/api/radio/trash/add", std::format(R"("songId":{},"alg":"RT","time":25)", id), Api::weapi);
    if (!r.error.empty()) co_return std::unexpected(r.error);
    co_return Result<void>{};
}

Task<Result<std::vector<Track>>> Netease::cloud() {
    std::vector<Track> out;
    for (int offset = 0;; offset += 500) {
        auto r = parse<CloudReply>(co_await client.call("/api/v1/cloud/get", std::format(R"("limit":500,"offset":{})", offset), Api::weapi));
        if (!r) co_return std::unexpected(r.error());
        if (r->data)
            for (const auto& c : *r->data)
                if (c.simpleSong) {
                    out.push_back(track(*c.simpleSong));
                    out.back().cloud = true;
                }
        if (!r->hasMore.value_or(false) || !r->data || r->data->empty()) break;
    }
    co_return out;
}

Task<Result<std::pair<Card, std::vector<Card>>>> Netease::user_playlists(std::string uid) {
    auto r = parse<PlaylistsReply>(
        co_await client.call("/api/user/playlist", std::format(R"("uid":{},"limit":1000,"offset":0,"includeVideo":true)", uid), Api::weapi));
    if (!r) co_return std::unexpected(r.error());
    std::pair<Card, std::vector<Card>> out;
    if (r->playlist)
        for (const auto& p : *r->playlist) {
            if (p.specialType == 5) out.first = playlist_card(p);
            else out.second.push_back(playlist_card(p));
        }
    co_return out;
}

Task<Result<std::vector<std::string>>> Netease::liked_ids(std::string uid) {
    auto r = parse<IdsReply>(co_await client.call("/api/song/like/get", std::format(R"("uid":{})", uid)));
    if (!r) co_return std::unexpected(r.error());
    std::vector<std::string> out;
    if (r->ids)
        for (uint64_t id : *r->ids) out.push_back(std::to_string(id));
    co_return out;
}

Task<Result<void>> Netease::like(std::string id, bool on) {
    Reply r = co_await client.call("/api/radio/like", std::format(R"("alg":"itembased","trackId":{},"like":{},"time":"3")", id, on), Api::weapi);
    if (!r.error.empty()) co_return std::unexpected(r.error);
    co_return Result<void>{};
}

}  // namespace lyren::ncm
