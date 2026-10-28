#include "ncm/netease.hpp"
#include "core/i18n.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <format>
#include <map>

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
            uint32_t dt = 0, duration = 0;
            int fee = 0;
            Strs alia, tns, alias, transNames;
        };
        struct NCreator {
            Str nickname;
            uint64_t userId = 0;
        };
        struct NPlaylist {
            uint64_t id = 0;
            Str name, coverImgUrl, picUrl, updateFrequency;
            int32_t trackCount = 0;
            std::optional<NCreator> creator;
            int specialType = 0;
            std::optional<bool> subscribed;
        };
        struct NAlbumInfo {
            uint64_t id = 0;
            Str name, picUrl;
            std::optional<NArtist> artist;
            int64_t publishTime = 0;
            int32_t size = 0;
        };
        struct NArtistInfo {
            uint64_t id = 0;
            Str name, picUrl, img1v1Url;
            int32_t musicSize = 0, albumSize = 0;
        };

        struct SearchResult {
            std::optional<std::vector<NSong>> songs;
            std::optional<std::vector<NAlbumInfo>> albums;
            std::optional<std::vector<NArtistInfo>> artists;
            std::optional<std::vector<NPlaylist>> playlists;
            int32_t songCount = 0, albumCount = 0, artistCount = 0, playlistCount = 0;
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
            std::optional<float> gain, peak;
        };
        struct UrlReply {
            std::optional<std::vector<NUrl>> data;
        };

        struct NLyric {
            Str lyric;
        };
        struct LyricReply {
            std::optional<NLyric> lrc, tlyric, yrc, ytlrc;
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
        struct NAlbumFull {
            Str name, picUrl, description, company;
            int64_t publishTime = 0;
            std::optional<std::vector<NArtist>> artists;
        };
        struct AlbumReply {
            std::optional<std::vector<NSong>> songs;
            std::optional<NAlbumFull> album;
        };
        struct ArtistReply {
            std::optional<std::vector<NSong>> hotSongs;
        };
        struct PlaylistsReply {
            std::optional<std::vector<NPlaylist>> playlist, result, list, recommend;
        };
        struct DailyData {
            std::optional<std::vector<NSong>> dailySongs;
        };
        struct DailyReply {
            std::optional<DailyData> data;
        };
        struct DislikeReply {
            std::optional<NSong> data;
        };
        struct NHeartbeat {
            std::optional<NSong> songInfo;
        };
        struct HeartbeatReply {
            std::optional<std::vector<NHeartbeat>> data;
        };
        struct NRecord {
            std::optional<NSong> data;
        };
        struct RecordsData {
            std::optional<std::vector<NRecord>> list;
        };
        struct RecordsReply {
            std::optional<RecordsData> data;
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
        struct SquareReply {
            std::optional<std::vector<NPlaylist>> playlists;
            int32_t total = 0;
        };
        struct NCategory {
            Str name;
            int category = 0;
            bool hot = false;
        };
        struct CatalogueReply {
            std::optional<std::vector<NCategory>> sub;
            std::optional<std::map<std::string, std::string>> categories;
        };
        struct NArtistHead {
            Str name, avatar;
            Strs alias, transNames;
            int32_t musicSize = 0, albumSize = 0;
        };
        struct ArtistHeadData {
            std::optional<NArtistHead> artist;
        };
        struct ArtistHeadReply {
            std::optional<ArtistHeadData> data;
        };
        struct FollowedReply {
            std::optional<bool> followed;
        };
        struct ArtistSongsReply {
            std::optional<std::vector<NSong>> songs;
            int32_t total = 0;
        };
        struct ArtistAlbumsReply {
            std::optional<std::vector<NAlbumInfo>> hotAlbums;
            std::optional<NArtistInfo> artist;
        };
        struct NSection {
            Str ti, txt;
        };
        struct IntroReply {
            Str briefDesc;
            std::optional<std::vector<NSection>> introduction;
        };
        struct CollectedReply {
            std::optional<bool> isSub;
        };
        struct NSubAlbum {
            uint64_t id = 0;
            Str name, picUrl;
            std::optional<std::vector<NArtist>> artists;
        };
        struct SubAlbumsReply {
            std::optional<std::vector<NSubAlbum>> data;
            std::optional<bool> hasMore;
        };
        struct SubArtistsReply {
            std::optional<std::vector<NArtistInfo>> data;
            std::optional<bool> hasMore;
        };
        struct CreateReply {
            std::optional<NPlaylist> playlist;
        };
        struct NCommentUser {
            Str nickname, avatarUrl;
        };
        struct NReplied {
            std::optional<NCommentUser> user;
            Str content;
        };
        struct NComment {
            std::optional<NCommentUser> user;
            Str content, timeStr;
            int32_t likedCount = 0;
            std::optional<std::vector<NReplied>> beReplied;
        };
        struct CommentsData {
            std::optional<std::vector<NComment>> comments;
            int32_t totalCount = 0;
            std::optional<bool> hasMore;
            Str cursor;
        };
        struct CommentsReply {
            std::optional<CommentsData> data;
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
        Result<T> parse(const Reply &r) {
            if (!r.error.empty()) {
                return std::unexpected(r.error);
            }
            T v{};
            if (auto e = glz::read<lax>(v, r.body)) {
                log::warn("ncm: cannot parse reply: {}", glz::format_error(e, r.body).substr(0, 200));
                return std::unexpected("Could not parse server response");
            }
            return v;
        }

        std::string https(std::string url) {
            if (url.starts_with("http://")) {
                url.insert(4, "s");
            }
            return url;
        }

        Track track(const NSong &s) {
            Track t{.id = std::to_string(s.id), .title = s.name.value_or("")};
            const auto &ar = s.ar ? s.ar : s.artists;
            if (ar) {
                for (const auto &a : *ar) {
                    if (!t.artist.empty()) {
                        t.artist += " / ";
                    }
                    t.artist += a.name.value_or("");
                }
                if (!ar->empty()) {
                    t.artist_id = std::to_string(ar->front().id);
                }
            }
            if (const auto &al = s.al ? s.al : s.album) {
                t.album = al->name.value_or("");
                t.album_id = std::to_string(al->id);
                t.cover = https(al->picUrl.value_or(""));
            }
            for (const Strs *names : {&s.tns, &s.transNames, &s.alia, &s.alias}) {
                if (*names && !(*names)->empty()) {
                    t.alias = (*names)->front();
                    break;
                }
            }
            t.duration = static_cast<int>((s.dt > 0 ? s.dt : s.duration) / 1000);
            t.paid = s.fee == 1 || s.fee == 4;
            return t;
        }

        std::vector<Track> tracks_of(const std::optional<std::vector<NSong>> &songs) {
            std::vector<Track> out;
            if (!songs) {
                return out;
            }
            out.reserve(songs->size());
            for (const auto &s : *songs) {
                out.push_back(track(s));
            }
            return out;
        }

        Card playlist_card(const NPlaylist &p) {
            Card c{.kind = CardKind::playlist, .id = std::to_string(p.id), .title = p.name.value_or("")};
            c.cover = https(p.coverImgUrl.value_or(p.picUrl.value_or("")));
            c.count = static_cast<int>(p.trackCount);
            c.subtitle = p.updateFrequency.value_or("");
            c.count_kind = p.updateFrequency ? Card::Count::none : Card::Count::songs;
            return c;
        }

        // Search results and the square name the creator too.
        Card listed_playlist(const NPlaylist &p) {
            Card c = playlist_card(p);
            if (p.creator && p.creator->nickname) {
                c.subtitle = *p.creator->nickname;
                c.count_kind = Card::Count::songs;
            }
            return c;
        }

        std::tm local(int64_t ms) {
            std::time_t t = static_cast<std::time_t>(ms / 1000);
            std::tm tm;
            localtime_r(&t, &tm);
            return tm;
        }

        std::string year_of(int64_t ms) {
            return ms > 0 ? std::to_string(local(ms).tm_year + 1900) : "";
        }

        std::string names(const std::optional<std::vector<NArtist>> &artists) {
            std::string out;
            if (artists) {
                for (const auto &a : *artists) {
                    out += (out.empty() ? "" : " / ") + a.name.value_or("");
                }
            }
            return out;
        }

        // Descriptions and comments often start or end with blank lines.
        std::string trimmed(const Str &s) {
            if (!s) {
                return {};
            }
            std::string_view v = *s;
            constexpr std::string_view blank = " \t\r\n";
            v.remove_prefix(std::min(v.size(), v.find_first_not_of(blank)));
            v = v.substr(0, v.find_last_not_of(blank) + 1);
            return std::string(v);
        }

        Result<void> done(const Reply &r) {
            if (!r.error.empty()) {
                return std::unexpected(r.error);
            }
            return {};
        }

    } // namespace

    Netease::Netease(Loop &loop, net::Http &http)
    : client(loop, http) {
        features.daily = [this] {
            return daily();
        };
        features.daily_playlists = [this] {
            return daily_playlists();
        };
        features.dislike_daily = [this](std::string id) {
            return dislike_daily(std::move(id));
        };
        features.heartbeat = [this](std::string seed, std::string playlist) {
            return heartbeat(std::move(seed), std::move(playlist));
        };
        features.similar = [this](std::string id) {
            return similar(std::move(id));
        };
        features.recent_plays = [this] {
            return recent_plays();
        };
        features.fm = [this] {
            return fm();
        };
        features.fm_trash = [this](std::string id) {
            return fm_trash(std::move(id));
        };
        features.cloud = [this] {
            return cloud();
        };
        features.categories = [this] {
            return categories();
        };
        features.square = [this](std::string category, int offset) {
            return square(std::move(category), offset);
        };
        features.report_start = [this](std::string id, std::string source) {
            return report_start(std::move(id), std::move(source));
        };
        features.report_play = [this](std::string id, std::string source, int seconds) {
            return report_play(std::move(id), std::move(source), seconds);
        };
    }

    Task<Result<void>> Netease::initialize() {
        client.ensure_fingerprint();
        co_return Result<void>{};
    }

    Task<Result<Batch>> Netease::search(std::string query, SearchKind kind, int offset, int limit) {
        static constexpr std::array types = {1, 10, 100, 1000};
        auto r = parse<SearchReply>(
            co_await client.call("/api/cloudsearch/pc", std::format(R"("s":{},"type":{},"limit":{},"offset":{},"total":true)", glz::write_json(glz::escape_bytes_t{query}).value(),
                                                                    types[static_cast<int>(kind)], limit, offset)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        Batch page;
        if (!r->result) {
            co_return page;
        }
        const SearchResult &res = *r->result;
        switch (kind) {
            case SearchKind::tracks:
                page.tracks = tracks_of(res.songs);
                page.total = static_cast<int>(res.songCount);
                break;
            case SearchKind::albums:
                if (res.albums) {
                    for (const auto &a : *res.albums) {
                        std::string artist = a.artist ? a.artist->name.value_or("") : "", year = year_of(a.publishTime);
                        page.cards.push_back({.kind = CardKind::album,
                                              .id = std::to_string(a.id),
                                              .title = a.name.value_or(""),
                                              .subtitle = year.empty() ? artist : std::format("{} · {}", artist, year),
                                              .cover = https(a.picUrl.value_or("")),
                                              .count = static_cast<int>(a.size)});
                    }
                }
                page.total = static_cast<int>(res.albumCount);
                break;
            case SearchKind::artists:
                if (res.artists) {
                    for (const auto &a : *res.artists) {
                        page.cards.push_back({.kind = CardKind::artist,
                                              .id = std::to_string(a.id),
                                              .title = a.name.value_or(""),
                                              .cover = https(a.picUrl.value_or(a.img1v1Url.value_or(""))),
                                              .count = static_cast<int>(a.musicSize),
                                              .count_kind = Card::Count::songs});
                    }
                }
                page.total = static_cast<int>(res.artistCount);
                break;
            case SearchKind::playlists:
                if (res.playlists) {
                    for (const auto &p : *res.playlists) {
                        page.cards.push_back(listed_playlist(p));
                    }
                }
                page.total = static_cast<int>(res.playlistCount);
                break;
        }
        co_return page;
    }

    Task<Result<Stream>> Netease::stream(Track t, Quality quality) {
        static constexpr std::array levels = {"standard", "higher", "exhigh", "lossless", "hires"};
        // Cloud-drive uploads only resolve through the older endpoint.
        Reply reply;
        if (t.cloud) {
            reply = co_await client.call("/api/song/enhance/player/url", std::format(R"("ids":"[{}]","br":999000)", t.id));
        } else {
            reply =
                co_await client.call("/api/song/enhance/player/url/v1", std::format(R"("ids":"[{}]","level":"{}","encodeType":"flac")", t.id, levels[static_cast<int>(quality)]));
        }
        auto r = parse<UrlReply>(reply);
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->data || r->data->empty() || !r->data->front().url || r->data->front().url->empty()) {
            co_return std::unexpected("This song is unavailable (membership or licensing restriction)");
        }
        const NUrl &u = r->data->front();
        co_return Stream{.url = https(*u.url),
                         .trial = u.freeTrialInfo.has_value(),
                         .trial_start = u.freeTrialInfo ? u.freeTrialInfo->start : 0,
                         .level = u.level.value_or(""),
                         .gain = u.gain.value_or(0.f),
                         .peak = u.peak.value_or(0.f)};
    }

    Task<Result<std::vector<LyricLine>>> Netease::lyrics(Track t) {
        auto r = parse<LyricReply>(co_await client.call("/api/song/lyric/v1", std::format(R"("id":{},"cp":false,"tv":0,"lv":0,"rv":0,"kv":0,"yv":0,"ytv":0,"yrv":0)", t.id)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto text = [](const std::optional<NLyric> &l) {
            return l ? l->lyric.value_or("") : "";
        };
        std::string yrc = text(r->yrc), ytlrc = text(r->ytlrc);
        // The word-timed translation follows the yrc line starts.
        co_return parse_lyrics(text(r->lrc), yrc.empty() || ytlrc.empty() ? text(r->tlyric) : ytlrc, yrc);
    }

    Task<Result<std::vector<Track>>> Netease::tracks(Card card) {
        if (card.kind == CardKind::album) {
            auto r = co_await album(card.id);
            if (!r) {
                co_return std::unexpected(r.error());
            }
            co_return std::move(r->tracks);
        }
        if (card.kind == CardKind::artist) {
            auto r = parse<ArtistReply>(co_await client.call("/api/v1/artist/" + card.id, "", Api::weapi));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            co_return tracks_of(r->hotSongs);
        }
        auto r = parse<PlaylistReply>(co_await client.call("/api/v6/playlist/detail", std::format(R"("id":{},"n":100000,"s":8)", card.id)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->playlist) {
            co_return std::vector<Track>{};
        }
        auto out = tracks_of(r->playlist->tracks);
        // Long playlists list every id but only the first tracks in full.
        const auto &ids = r->playlist->trackIds;
        if (ids && ids->size() > out.size()) {
            std::vector<std::string> rest;
            for (size_t i = out.size(); i < ids->size(); ++i) {
                rest.push_back(std::to_string((*ids)[i].id));
            }
            auto more = co_await song_detail(std::move(rest));
            if (!more) {
                co_return std::unexpected(more.error());
            }
            out.insert(out.end(), std::make_move_iterator(more->begin()), std::make_move_iterator(more->end()));
        }
        co_return out;
    }

    Task<Result<std::vector<Track>>> Netease::song_detail(std::vector<std::string> ids) {
        std::vector<Track> out;
        for (size_t i = 0; i < ids.size(); i += 500) {
            std::string c = "[";
            for (size_t k = i; k < std::min(ids.size(), i + 500); ++k) {
                c += std::format("{}{{\"id\":{}}}", k > i ? "," : "", ids[k]);
            }
            c += "]";
            auto r = parse<SongsReply>(co_await client.call("/api/v3/song/detail", std::format(R"("c":{})", glz::write_json(c).value()), Api::weapi));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            auto part = tracks_of(r->songs);
            out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }
        co_return out;
    }

    Task<Result<QrCode>> Netease::qr_start() {
        auto r = parse<QrKeyReply>(co_await client.call("/api/login/qrcode/unikey", R"("type":3)"));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->unikey) {
            co_return std::unexpected("Could not get a QR code");
        }
        qr_key_ = *r->unikey;
        co_return QrCode{.text = "https://music.163.com/login?codekey=" + qr_key_};
    }

    Task<Result<QrStatus>> Netease::qr_poll() {
        Reply reply = co_await client.call("/api/login/qrcode/client/login", std::format(R"("key":{},"type":3)", glz::write_json(qr_key_).value()));
        auto r = parse<QrCheckReply>(reply);
        if (!r) {
            co_return std::unexpected(r.error());
        }
        QrStatus s;
        switch (reply.code) {
            case 800:
                s.state = QrState::expired;
                break;
            case 801:
                s.state = QrState::waiting;
                break;
            case 802:
                s.state = QrState::scanned;
                s.nickname = r->nickname.value_or("");
                s.avatar = https(r->avatarUrl.value_or(""));
                break;
            case 803:
                s.state = QrState::confirmed;
                break;
            default:
                co_return std::unexpected(std::format("Sign-in failed ({})", reply.code));
        }
        co_return s;
    }

    Task<Result<Account>> Netease::account() {
        Reply reply = co_await client.call("/api/w/nuser/account/get", "", Api::weapi);
        auto r = parse<AccountReply>(reply);
        // QR logins cannot be refreshed; once the session is gone the user has to scan again.
        if (reply.code == 301 || (r && !r->profile)) {
            client.clear_login();
            co_return std::unexpected("NetEase Music session expired; scan again to sign in");
        }
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Account{.id = std::to_string(r->profile->userId), .name = r->profile->nickname.value_or(""), .avatar = https(r->profile->avatarUrl.value_or(""))};
    }

    Task<void> Netease::logout() {
        co_await client.call("/api/logout", "");
        client.clear_login();
    }

    Task<Result<std::vector<Card>>> Netease::recommended_playlists() {
        auto r = parse<PlaylistsReply>(co_await client.call("/api/personalized/playlist", R"("limit":30,"total":true,"n":1000)", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<Card> out;
        if (r->result) {
            for (const auto &p : *r->result) {
                out.push_back(playlist_card(p));
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Netease::toplists() {
        auto r = parse<PlaylistsReply>(co_await client.call("/api/toplist", ""));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<Card> out;
        if (r->list) {
            for (const auto &p : *r->list) {
                out.push_back(playlist_card(p));
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Track>>> Netease::daily() {
        auto r = parse<DailyReply>(co_await client.call("/api/v3/discovery/recommend/songs", "", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return r->data ? tracks_of(r->data->dailySongs) : std::vector<Track>{};
    }

    Task<Result<std::vector<Card>>> Netease::daily_playlists() {
        auto r = parse<PlaylistsReply>(co_await client.call("/api/v1/discovery/recommend/resource", "", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<Card> out;
        if (r->recommend) {
            for (const auto &p : *r->recommend) {
                out.push_back(playlist_card(p));
            }
        }
        co_return out;
    }

    Task<Result<std::optional<Track>>> Netease::dislike_daily(std::string id) {
        auto r = parse<DislikeReply>(co_await client.call("/api/v2/discovery/recommend/dislike", std::format(R"("resId":{},"resType":4,"sceneType":1)", id), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->data) {
            co_return std::nullopt;
        }
        co_return track(*r->data);
    }

    Task<Result<std::vector<Track>>> Netease::heartbeat(std::string seed, std::string playlist) {
        auto r = parse<HeartbeatReply>(co_await client.call("/api/playmode/intelligence/list",
                                                            std::format(R"("songId":{},"type":"fromPlayOne","playlistId":{},"startMusicId":{},"count":1)", seed, playlist, seed)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<Track> out;
        if (r->data) {
            for (const auto &d : *r->data) {
                if (d.songInfo) {
                    out.push_back(track(*d.songInfo));
                }
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Track>>> Netease::similar(std::string id) {
        auto r = parse<SongsReply>(co_await client.call("/api/v1/discovery/simiSong", std::format(R"("songid":{},"limit":50,"offset":0)", id), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return tracks_of(r->songs);
    }

    Task<Result<std::vector<Track>>> Netease::recent_plays() {
        auto r = parse<RecordsReply>(co_await client.call("/api/play-record/song/list", R"("limit":100)", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<Track> out;
        if (r->data && r->data->list) {
            for (const auto &rec : *r->data->list) {
                if (rec.data) {
                    out.push_back(track(*rec.data));
                }
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Track>>> Netease::fm() {
        auto r = parse<FmReply>(co_await client.call("/api/v1/radio/get", "", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return tracks_of(r->data);
    }

    Task<Result<void>> Netease::fm_trash(std::string id) {
        co_return done(co_await client.call("/api/radio/trash/add", std::format(R"("songId":{},"alg":"RT","time":25)", id), Api::weapi));
    }

    Task<Result<std::vector<Track>>> Netease::cloud() {
        std::vector<Track> out;
        for (int offset = 0;; offset += 500) {
            auto r = parse<CloudReply>(co_await client.call("/api/v1/cloud/get", std::format(R"("limit":500,"offset":{})", offset), Api::weapi));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            if (r->data) {
                for (const auto &c : *r->data) {
                    if (c.simpleSong) {
                        out.push_back(track(*c.simpleSong));
                        out.back().cloud = true;
                    }
                }
            }
            if (!r->hasMore.value_or(false) || !r->data || r->data->empty()) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Netease::user_playlists(std::string uid) {
        auto r = parse<PlaylistsReply>(co_await client.call("/api/user/playlist", std::format(R"("uid":{},"limit":1000,"offset":0,"includeVideo":true)", uid), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::pair<Card, std::vector<Card>> out;
        if (r->playlist) {
            for (const auto &p : *r->playlist) {
                Card c = playlist_card(p);
                c.owned = p.creator && std::to_string(p.creator->userId) == uid;
                if (p.specialType == 5) {
                    out.first = std::move(c);
                } else {
                    out.second.push_back(std::move(c));
                }
            }
        }
        co_return out;
    }

    Task<Result<std::vector<std::string>>> Netease::liked_ids(std::string uid) {
        auto r = parse<IdsReply>(co_await client.call("/api/song/like/get", std::format(R"("uid":{})", uid)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<std::string> out;
        if (r->ids) {
            for (uint64_t id : *r->ids) {
                out.push_back(std::to_string(id));
            }
        }
        co_return out;
    }

    Task<Result<void>> Netease::like(Track track, bool on) {
        co_return done(co_await client.call("/api/radio/like", std::format(R"("alg":"itembased","trackId":{},"like":{},"time":"3")", track.id, on), Api::weapi));
    }

    Task<Result<Categories>> Netease::categories() {
        auto r = parse<CatalogueReply>(co_await client.call("/api/playlist/catalogue", ""));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        Categories out;
        if (!r->sub || !r->categories) {
            co_return out;
        }
        for (const auto &[key, heading] : *r->categories) {
            auto &g = out.groups.emplace_back(heading, std::vector<Category>{});
            for (const auto &c : *r->sub) {
                if (std::to_string(c.category) == key) {
                    g.second.push_back({c.name.value_or(""), c.name.value_or("")});
                }
            }
        }
        for (const auto &c : *r->sub) {
            if (c.hot) {
                out.hot.push_back({c.name.value_or(""), c.name.value_or("")});
            }
        }
        co_return out;
    }

    Task<Result<Batch>> Netease::square(std::string category, int offset) {
        if (category.empty()) {
            category = "\u5168\u90e8";
        }
        auto r = parse<SquareReply>(co_await client.call(
            "/api/playlist/list", std::format(R"("cat":{},"order":"hot","limit":50,"offset":{},"total":true)", glz::write_json(glz::escape_bytes_t{category}).value(), offset),
            Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        Batch out{.total = static_cast<int>(r->total)};
        if (r->playlists) {
            for (const auto &p : *r->playlists) {
                out.cards.push_back(listed_playlist(p));
            }
        }
        co_return out;
    }

    Task<Result<ArtistInfo>> Netease::artist(std::string id) {
        auto r = parse<ArtistHeadReply>(co_await client.call("/api/artist/head/info/get", std::format(R"("id":{})", id)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->data || !r->data->artist) {
            co_return std::unexpected("Artist not found");
        }
        const NArtistHead &a = *r->data->artist;
        ArtistInfo out{.name = a.name.value_or(""), .avatar = https(a.avatar.value_or("")), .songs = static_cast<int>(a.musicSize), .albums = static_cast<int>(a.albumSize)};
        for (const Strs *list : {&a.transNames, &a.alias}) {
            if (*list) {
                for (const auto &n : **list) {
                    out.alias += (out.alias.empty() ? "" : " / ") + n;
                }
            }
        }
        if (logged_in()) {
            auto f = parse<FollowedReply>(co_await client.call("/api/artist/detail/dynamic", std::format(R"("id":{})", id)));
            out.followed = f && f->followed.value_or(false);
        }
        co_return out;
    }

    Task<Result<Batch>> Netease::artist_songs(std::string id, int offset) {
        auto r = parse<ArtistSongsReply>(
            co_await client.call("/api/v1/artist/songs", std::format(R"("id":{},"private_cloud":"true","work_type":1,"order":"hot","offset":{},"limit":100)", id, offset)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Batch{.tracks = tracks_of(r->songs), .total = static_cast<int>(r->total)};
    }

    Task<Result<Batch>> Netease::artist_albums(std::string id, int offset) {
        auto r = parse<ArtistAlbumsReply>(co_await client.call("/api/artist/albums/" + id, std::format(R"("limit":30,"offset":{},"total":true)", offset), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        Batch out{.total = r->artist ? static_cast<int>(r->artist->albumSize) : 0};
        if (r->hotAlbums) {
            for (const auto &a : *r->hotAlbums) {
                std::string year = year_of(a.publishTime);
                out.cards.push_back({.kind = CardKind::album,
                                     .id = std::to_string(a.id),
                                     .title = a.name.value_or(""),
                                     .subtitle = std::move(year),
                                     .cover = https(a.picUrl.value_or("")),
                                     .count = static_cast<int>(a.size),
                                     .count_kind = Card::Count::songs});
            }
        }
        co_return out;
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Netease::artist_intro(std::string id) {
        auto r = parse<IntroReply>(co_await client.call("/api/artist/introduction", std::format(R"("id":{})", id), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        std::vector<std::pair<std::string, std::string>> out;
        if (std::string brief = trimmed(r->briefDesc); !brief.empty()) {
            out.emplace_back("", std::move(brief));
        }
        if (r->introduction) {
            for (const auto &s : *r->introduction) {
                out.emplace_back(trimmed(s.ti), trimmed(s.txt));
            }
        }
        co_return out;
    }

    Task<Result<AlbumInfo>> Netease::album(std::string id) {
        auto r = parse<AlbumReply>(co_await client.call("/api/v1/album/" + id, "", Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        AlbumInfo out{.thread = id, .tracks = tracks_of(r->songs)};
        if (const auto &a = r->album) {
            out.artist = names(a->artists);
            if (a->artists && !a->artists->empty()) {
                out.artist_id = std::to_string(a->artists->front().id);
            }
            if (a->publishTime > 0) {
                std::tm tm = local(a->publishTime);
                out.date = std::format("{}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
            out.company = a->company.value_or("");
            out.about = trimmed(a->description);
            // Album tracks often come without a cover of their own.
            std::string cover = https(a->picUrl.value_or(""));
            for (auto &t : out.tracks) {
                if (t.cover.empty()) {
                    t.cover = cover;
                }
            }
        }
        if (logged_in()) {
            auto c = parse<CollectedReply>(co_await client.call("/api/album/detail/dynamic", std::format(R"("id":{})", id), Api::weapi));
            out.collected = c && c->isSub.value_or(false);
        }
        co_return out;
    }

    Task<Result<void>> Netease::follow(std::string artist, bool on) {
        co_return done(co_await client.call(std::format("/api/artist/{}", on ? "sub" : "unsub"), std::format(R"("artistId":"{}","artistIds":"[{}]")", artist, artist), Api::weapi));
    }

    Task<Result<void>> Netease::collect(std::string album, bool on) {
        co_return done(co_await client.call(std::format("/api/album/{}", on ? "sub" : "unsub"), std::format(R"("id":"{}")", album), Api::weapi));
    }

    Task<Result<std::vector<Card>>> Netease::collected_albums() {
        std::vector<Card> out;
        for (int offset = 0;; offset += 100) {
            auto r = parse<SubAlbumsReply>(co_await client.call("/api/album/sublist", std::format(R"("limit":100,"offset":{},"total":true)", offset), Api::weapi));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            if (r->data) {
                for (const auto &a : *r->data) {
                    out.push_back(
                        {.kind = CardKind::album, .id = std::to_string(a.id), .title = a.name.value_or(""), .subtitle = names(a.artists), .cover = https(a.picUrl.value_or(""))});
                }
            }
            if (!r->hasMore.value_or(false) || !r->data || r->data->empty()) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Netease::followed_artists() {
        std::vector<Card> out;
        for (int offset = 0;; offset += 100) {
            auto r = parse<SubArtistsReply>(co_await client.call("/api/artist/sublist", std::format(R"("limit":100,"offset":{},"total":true)", offset), Api::weapi));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            if (r->data) {
                for (const auto &a : *r->data) {
                    out.push_back({.kind = CardKind::artist,
                                   .id = std::to_string(a.id),
                                   .title = a.name.value_or(""),
                                   .cover = https(a.picUrl.value_or(a.img1v1Url.value_or(""))),
                                   .count = static_cast<int>(a.albumSize),
                                   .count_kind = Card::Count::albums});
                }
            }
            if (!r->hasMore.value_or(false) || !r->data || r->data->empty()) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<Card>> Netease::create_playlist(std::string name, bool hidden) {
        auto r = parse<CreateReply>(co_await client.call(
            "/api/playlist/create", std::format(R"("name":{},"privacy":"{}","type":"NORMAL")", glz::write_json(glz::escape_bytes_t{name}).value(), hidden ? 10 : 0), Api::weapi));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->playlist) {
            co_return std::unexpected("No playlist returned");
        }
        Card c = playlist_card(*r->playlist);
        c.owned = true;
        co_return c;
    }

    Task<Result<void>> Netease::rename_playlist(std::string id, std::string name) {
        co_return done(co_await client.call("/api/playlist/update/name", std::format(R"("id":"{}","name":{})", id, glz::write_json(glz::escape_bytes_t{name}).value())));
    }

    Task<Result<void>> Netease::delete_playlist(std::string id) {
        co_return done(co_await client.call("/api/playlist/remove", std::format(R"("ids":"[{}]")", id), Api::weapi));
    }

    Task<Result<void>> Netease::edit_playlist(std::string id, Track t, bool add) {
        std::string track = std::move(t.id);
        auto fields = [&](std::string_view ids) {
            return std::format(R"("op":"{}","pid":"{}","trackIds":{},"imme":"true")", add ? "add" : "del", id, glz::write_json(ids).value());
        };
        Reply r = co_await client.call("/api/playlist/manipulate/tracks", fields(std::format(R"(["{}"])", track)));
        // The server sometimes answers 512 for a single id; listing it twice goes through.
        if (r.code == 512) {
            r = co_await client.call("/api/playlist/manipulate/tracks", fields(std::format(R"(["{}","{}"])", track, track)));
        }
        if (r.code == 502 && add) {
            co_return std::unexpected("This song is already in the playlist");
        }
        co_return done(r);
    }

    Task<Result<CommentPage>> Netease::comments(Subject subject, std::string id, bool hot, int page, std::string cursor) {
        static constexpr std::array threads = {"R_SO_4_", "R_AL_3_", "A_PL_0_"};
        // Hot comments page by an offset, the newest by the time of the last one; the server hands back either.
        if (cursor.empty()) {
            cursor = hot ? "normalHot#0" : "0";
        }
        auto r = parse<CommentsReply>(co_await client.call(
            "/api/v2/resource/comments", std::format(R"("threadId":"{}{}","pageNo":{},"showInner":true,"pageSize":20,"cursor":{},"sortType":{})",
                                                     threads[static_cast<int>(subject)], id, page, glz::write_json(glz::escape_bytes_t{cursor}).value(), hot ? 2 : 3)));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        CommentPage out;
        if (!r->data) {
            co_return out;
        }
        const CommentsData &d = *r->data;
        out.total = d.totalCount;
        out.more = d.hasMore.value_or(false);
        out.cursor = d.cursor.value_or("");
        if (d.comments) {
            for (const auto &c : *d.comments) {
                Comment &m = out.comments.emplace_back();
                if (c.user) {
                    m.user = c.user->nickname.value_or(""), m.avatar = https(c.user->avatarUrl.value_or(""));
                }
                m.text = trimmed(c.content);
                m.time = c.timeStr.value_or("");
                m.likes = c.likedCount;
                if (c.beReplied && !c.beReplied->empty()) {
                    const NReplied &b = c.beReplied->front();
                    m.reply_user = b.user ? b.user->nickname.value_or("") : "";
                    m.reply = trimmed(b.content);
                }
            }
        }
        co_return out;
    }

    // As api-enhanced's scrobble: every value is a string, and the play record always ends with "playend" (the server
    // does not count others).
    Task<void> Netease::report_start(std::string id, std::string source) {
        std::string logs = std::format(R"([{{"action":"startplay","json":{{"id":"{}","type":"song","mainsite":"1","mainsiteWeb":"1","content":"id={}"}}}}])", id, source);
        co_await client.call("/api/feedback/weblog", std::format(R"("logs":{})", glz::write_json(logs).value()), Api::clientlog);
    }

    Task<void> Netease::report_play(std::string id, std::string source, int seconds) {
        std::string logs = std::format(R"([{{"action":"play","json":{{"download":0,"end":"playend","id":"{}","sourceId":"{}","time":"{}","type":"song","wifi":0,)"
                                       R"("source":"list","mainsite":"1","mainsiteWeb":"1","content":"id={}"}}}}])",
                                       id, source, seconds, source);
        co_await client.call("/api/feedback/weblog", std::format(R"("logs":{})", glz::write_json(logs).value()), Api::clientlog);
    }

} // namespace lyren::ncm
