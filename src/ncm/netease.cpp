#include "ncm/netease.hpp"
#include "core/i18n.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <map>
#include <tuple>

#include "core/log.hpp"
#include "core/result_task.hpp"

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
            int privacy = 0;
            std::optional<bool> subscribed;
        };
        struct NAlbumInfo {
            uint64_t id = 0;
            Str name, picUrl;
            std::optional<NArtist> artist;
            std::optional<std::vector<NArtist>> artists;
            int64_t publishTime = 0;
            int32_t size = 0;
        };
        struct NArtistInfo {
            uint64_t id = 0;
            Str name, picUrl, img1v1Url;
            int32_t musicSize = 0, albumSize = 0;
        };

        // A listener as lists, searches and posts describe them.
        struct NUser {
            uint64_t userId = 0;
            Str nickname, avatarUrl, signature;
            std::optional<int32_t> followeds;
            std::optional<bool> followed, mutual;
        };

        struct SearchResult {
            std::optional<std::vector<NSong>> songs;
            std::optional<std::vector<NAlbumInfo>> albums;
            std::optional<std::vector<NArtistInfo>> artists;
            std::optional<std::vector<NPlaylist>> playlists;
            std::optional<std::vector<NUser>> userprofiles;
            int32_t songCount = 0, albumCount = 0, artistCount = 0, playlistCount = 0, userprofileCount = 0;
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
            bool more = false;
        };
        struct NewAlbumsReply {
            std::optional<std::vector<NAlbumInfo>> albums;
            int total = 0;
        };
        struct WriteReply {
            int code = 0;
            Str message, msg;
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
            uint64_t userId = 0;
            Str nickname, avatarUrl;
        };
        struct NReplied {
            std::optional<NCommentUser> user;
            Str content;
        };
        struct NComment {
            std::optional<NCommentUser> user;
            Str content, timeStr, contentPicUrl;
            uint64_t commentId = 0;
            int32_t likedCount = 0;
            std::optional<int32_t> replyCount;
            std::optional<bool> liked, owner;
            std::optional<std::vector<NReplied>> beReplied;
        };
        struct CommentsData {
            std::optional<std::vector<NComment>> comments;
            int32_t totalCount = 0;
            std::optional<bool> hasMore;
            Str cursor;
            int64_t time = -1; // floor replies continue with this timestamp
        };
        struct CommentsReply {
            int code = 0;
            Str message, msg;
            std::optional<CommentsData> data;
            std::optional<NComment> comment;
        };
        struct NProfile {
            uint64_t userId = 0;
            Str nickname, avatarUrl;
        };
        struct AccountReply {
            std::optional<NProfile> profile;
        };

        struct NUserProfile {
            uint64_t userId = 0;
            Str nickname, avatarUrl, backgroundUrl, signature;
            int32_t follows = 0, followeds = 0, eventCount = 0, playlistCount = 0, vipType = 0, province = 0;
            std::optional<bool> followed, followMe;
        };
        struct UserDetailReply {
            int32_t level = 0, listenSongs = 0, createDays = -1;
            std::optional<bool> peopleCanSeeMyPlayRecord;
            std::optional<NUserProfile> profile;
        };
        struct FollowsReply {
            int code = 0;
            Str message, msg;
            std::optional<std::vector<NUser>> follow, followeds;
            std::optional<bool> more;
            std::optional<int32_t> size;
        };

        struct NPic {
            Str originUrl, pcRectangleUrl, rectangleUrl;
            int32_t width = 0, height = 0;
        };
        struct NResourceInfo {
            Str name;
        };
        struct NCommentThread {
            std::optional<NResourceInfo> resourceInfo;
        };
        struct NEventInfo {
            std::optional<bool> liked;
            int32_t likedCount = 0, commentCount = 0, shareCount = 0;
            std::optional<NCommentThread> commentThread;
        };
        // A post. Its body is JSON in a string; a forwarded one is an object inside that body.
        struct NEvent {
            uint64_t id = 0;
            int32_t type = 0;
            int64_t eventTime = 0;
            std::optional<NUser> user;
            Str json, threadId;
            std::optional<std::vector<NPic>> pics;
            std::optional<NEventInfo> info;
            std::optional<bool> owner;
        };
        struct NEventBody {
            Str msg;
            std::optional<NSong> song;
            std::optional<NPlaylist> playlist;
            std::optional<NAlbumInfo> album;
            std::optional<NEvent> event;
        };
        struct EventsReply {
            std::optional<std::vector<NEvent>> events, event;
            std::optional<bool> more;
            int64_t lasttime = -1;
        };

        struct NRanked {
            int32_t playCount = 0, score = 0;
            std::optional<NSong> song;
        };
        struct RankingReply {
            std::optional<std::vector<NRanked>> weekData, allData;
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
            c.collectable = p.specialType != 5 && p.privacy != 10;
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

        Card album_card(const NAlbumInfo &a) {
            std::string artist = a.artists ? names(a.artists) : a.artist ? a.artist->name.value_or("") : "";
            std::string year = year_of(a.publishTime);
            return {.kind = CardKind::album,
                    .id = std::to_string(a.id),
                    .title = a.name.value_or(""),
                    .subtitle = artist.empty() ? year
                                : year.empty() ? artist
                                               : artist + " · " + year,
                    .cover = https(a.picUrl.value_or("")),
                    .count = static_cast<int>(a.size),
                    .count_kind = Card::Count::songs};
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

        std::string comment_thread(Subject subject, std::string_view id) {
            static constexpr std::array prefixes = {"R_SO_4_", "R_AL_3_", "A_PL_0_", "A_EV_2_"};
            return std::string(prefixes[static_cast<size_t>(subject)]) + std::string(id);
        }

        Person person(const NUser &u) {
            return {.id = std::to_string(u.userId),
                    .name = u.nickname.value_or(""),
                    .avatar = https(u.avatarUrl.value_or("")),
                    .signature = trimmed(u.signature),
                    .fans = u.followeds.value_or(-1),
                    .followed = u.followed.value_or(false),
                    .follows_me = u.mutual.value_or(false)};
        }

        // Province codes of user profiles (GB/T 2260), in both languages.
        std::string province(int code) {
            static constexpr std::array<std::tuple<int, std::string_view, std::string_view>, 34> names{{
                {110000, "Beijing", "北京"},
                {120000, "Tianjin", "天津"},
                {130000, "Hebei", "河北"},
                {140000, "Shanxi", "山西"},
                {150000, "Inner Mongolia", "内蒙古"},
                {210000, "Liaoning", "辽宁"},
                {220000, "Jilin", "吉林"},
                {230000, "Heilongjiang", "黑龙江"},
                {310000, "Shanghai", "上海"},
                {320000, "Jiangsu", "江苏"},
                {330000, "Zhejiang", "浙江"},
                {340000, "Anhui", "安徽"},
                {350000, "Fujian", "福建"},
                {360000, "Jiangxi", "江西"},
                {370000, "Shandong", "山东"},
                {410000, "Henan", "河南"},
                {420000, "Hubei", "湖北"},
                {430000, "Hunan", "湖南"},
                {440000, "Guangdong", "广东"},
                {450000, "Guangxi", "广西"},
                {460000, "Hainan", "海南"},
                {500000, "Chongqing", "重庆"},
                {510000, "Sichuan", "四川"},
                {520000, "Guizhou", "贵州"},
                {530000, "Yunnan", "云南"},
                {540000, "Tibet", "西藏"},
                {610000, "Shaanxi", "陕西"},
                {620000, "Gansu", "甘肃"},
                {630000, "Qinghai", "青海"},
                {640000, "Ningxia", "宁夏"},
                {650000, "Xinjiang", "新疆"},
                {710000, "Taiwan", "台湾"},
                {810000, "Hong Kong", "香港"},
                {820000, "Macao", "澳门"},
            }};
            auto it = std::ranges::find(names, code, [](const auto &n) {
                return std::get<0>(n);
            });
            return it == names.end() ? "" : std::string(language() == Language::zh ? std::get<2>(*it) : std::get<1>(*it));
        }

        // Writes succeed only with code 200; the client lets a few other codes through for the callers that read them.
        Result<void> written(const Reply &reply) {
            return parse<WriteReply>(reply).and_then([&](auto r) -> Result<void> {
                if (r.code != 200) {
                    std::string message = r.message.value_or(r.msg.value_or(""));
                    return std::unexpected(message.empty() ? lyren::trf("NetEase Music refused the request ({})", r.code) : std::move(message));
                }
                return {};
            });
        }

        Post post(const NEvent &e, bool nested = false) {
            Post out{.id = std::to_string(e.id), .time = e.eventTime, .owned = e.owner.value_or(false)};
            if (e.user) {
                out.author = person(*e.user);
            }
            constexpr std::string_view prefix = "A_EV_2_";
            out.thread = e.threadId && e.threadId->starts_with(prefix) ? e.threadId->substr(prefix.size()) : out.id + "_" + out.author.id;
            if (e.info) {
                out.liked = e.info->liked.value_or(false);
                out.likes = e.info->likedCount, out.comments = e.info->commentCount, out.forwards = e.info->shareCount;
            }
            if (e.pics) {
                for (const auto &pic : *e.pics) {
                    if (auto url = https(pic.originUrl.value_or("")); !url.empty()) {
                        out.pictures.push_back({std::move(url), pic.width, pic.height});
                    }
                }
            }
            NEventBody body;
            if (e.json && !glz::read<lax>(body, *e.json)) {
                out.text = trimmed(body.msg);
                if (body.song) {
                    out.track = track(*body.song);
                } else if (body.playlist) {
                    out.card = listed_playlist(*body.playlist);
                } else if (body.album) {
                    out.card = album_card(*body.album);
                }
                if (body.event && !nested) {
                    out.forwarded.push_back(post(*body.event, true));
                }
            }
            // Videos, programmes and the like: the service's own words for what is shared.
            if (!out.track && !out.card && out.forwarded.empty() && e.type != 35 && e.info && e.info->commentThread && e.info->commentThread->resourceInfo) {
                out.resource = trimmed(e.info->commentThread->resourceInfo->name);
            }
            return out;
        }

        Result<CommentsReply> comment_response(const Reply &reply) {
            return parse<CommentsReply>(reply).and_then([&](auto r) -> Result<CommentsReply> {
                if (r.code != 200) {
                    std::string message = r.message.value_or(r.msg.value_or(""));
                    return std::unexpected(message.empty() ? lyren::trf("NetEase Music comment operation failed ({})", r.code) : std::move(message));
                }
                return r;
            });
        }

    } // namespace

    Netease::Netease(Loop &loop, net::Http &http)
    : client(loop, http) {
        capabilities = {.toplists = true,
                        .square = true,
                        .radio = true,
                        .radio_dislike = true,
                        .similar = true,
                        .artist_intro = true,
                        .library = true,
                        .likes = true,
                        .playlist_edit = true,
                        .playlist_order = true,
                        .private_playlists = true,
                        .collect_playlists = true,
                        .collect_albums = true,
                        .follow_artists = true,
                        .comments = {true, true, true, true},
                        .comment_sort = true,
                        .comment_replies = true,
                        .comment_likes = true,
                        .comment_post = true,
                        .comment_delete = true,
                        .qr_login = true,
                        .profiles = true,
                        .people = true,
                        .follow_users = true,
                        .posts = true,
                        .post_write = true,
                        .ranking = true,
                        .profile_playlists = true};
        capabilities.search[4] = true;
        features.daily.songs = [this] {
            return daily();
        };
        features.daily.playlists = [this] {
            return daily_playlists();
        };
        features.daily.dislike = [this](std::string id) {
            return dislike_daily(std::move(id));
        };
        release_categories[0] = {{"0", "All"}, {"7", "Chinese"}, {"96", "Western"}, {"8", "Japan"}, {"16", "Korea"}};
        release_categories[1] = {{"ALL", "All"}, {"ZH", "Chinese"}, {"EA", "Western"}, {"JP", "Japan"}, {"KR", "Korea"}};
        features.reporting.start = [this](std::string id, std::string source) {
            return report_start(std::move(id), std::move(source));
        };
        features.reporting.play = [this](std::string id, std::string source, int seconds) {
            return report_play(std::move(id), std::move(source), seconds);
        };
        features.cloud = [this] {
            return cloud();
        };
        features.recent_plays = [this] {
            return recent_plays();
        };
        features.heartbeat = [this](std::string seed, std::string playlist) {
            return heartbeat(std::move(seed), std::move(playlist));
        };
    }

    Task<Result<void>> Netease::initialize() {
        client.ensure_fingerprint();
        co_return Result<void>{};
    }

    Task<Result<Batch>> Netease::search(std::string query, SearchKind kind, int offset, int limit) {
        static constexpr std::array types = {1, 10, 100, 1000, 1002};
        co_return parse<SearchReply>(
            co_await client.call("/api/cloudsearch/pc", std::format(R"("s":{},"type":{},"limit":{},"offset":{},"total":true)", glz::write_json(glz::escape_bytes_t{query}).value(),
                                                                    types[static_cast<int>(kind)], limit, offset)))
            .transform([&](auto r) -> Batch {
                Batch page;
                if (!r.result) {
                    return page;
                }
                const SearchResult &res = *r.result;
                switch (kind) {
                    case SearchKind::tracks:
                        page.tracks = tracks_of(res.songs);
                        page.total = static_cast<int>(res.songCount);
                        break;
                    case SearchKind::albums:
                        if (res.albums) {
                            for (const auto &a : *res.albums) {
                                page.cards.push_back(album_card(a));
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
                    case SearchKind::users:
                        if (res.userprofiles) {
                            for (const auto &u : *res.userprofiles) {
                                page.cards.push_back(user_card(person(u)));
                            }
                        }
                        page.total = static_cast<int>(res.userprofileCount);
                        break;
                }
                if (offset + limit < page.total) {
                    page.next_offset = offset + limit;
                }
                return page;
            });
    }

    Task<Result<Stream>> Netease::stream(Track t, Quality quality) {
        static constexpr std::array levels = {"standard", "higher", "exhigh", "lossless", "hires"};
        // Cloud-drive uploads only resolve through the older endpoint.
        Reply reply;
        if (t.cloud) {
            reply = co_await client.call("/api/song/enhance/player/url", std::format(R"("ids":"[{}]","br":999000)", t.id));
        } else {
            reply =
                co_await client.call_xeapi("/api/song/enhance/player/url/v1", {{"ids", "[" + t.id + "]"}, {"level", levels[static_cast<int>(quality)]}, {"encodeType", "flac"}});
        }
        co_return parse<UrlReply>(reply).and_then([&](auto r) -> Result<Stream> {
            if (!r.data || r.data->empty() || !r.data->front().url || r.data->front().url->empty()) {
                return std::unexpected("This song is unavailable (membership or licensing restriction)");
            }
            const NUrl &u = r.data->front();
            return Stream{.url = https(*u.url),
                          .level = u.level.value_or(""),
                          .trial_start = u.freeTrialInfo ? u.freeTrialInfo->start : 0,
                          .gain = u.gain.value_or(0.f),
                          .peak = u.peak.value_or(0.f),
                          .trial = u.freeTrialInfo.has_value()};
        });
    }

    Task<Result<std::vector<LyricLine>>> Netease::lyrics(Track t) {
        co_return parse<LyricReply>(co_await client.call("/api/song/lyric/v1", std::format(R"("id":{},"cp":false,"tv":0,"lv":0,"rv":0,"kv":0,"yv":0,"ytv":0,"yrv":0)", t.id)))
            .transform([](const LyricReply &r) {
                auto text = [](const std::optional<NLyric> &l) {
                    return l ? l->lyric.value_or("") : "";
                };
                std::string yrc = text(r.yrc), ytlrc = text(r.ytlrc);
                // The word-timed translation follows the yrc line starts.
                return parse_lyrics(text(r.lrc), yrc.empty() || ytlrc.empty() ? text(r.tlyric) : ytlrc, yrc);
            });
    }

    Task<Result<std::vector<Track>>> Netease::tracks(Card card) {
        if (card.kind == CardKind::album) {
            co_return (co_await album(card.id)).transform([&](auto r) -> std::vector<Track> {
                return std::move(r.tracks);
            });
        }
        if (card.kind == CardKind::artist) {
            co_return parse<ArtistReply>(co_await client.call("/api/v1/artist/" + card.id, "", Api::weapi)).transform([&](auto r) -> std::vector<Track> {
                return tracks_of(r.hotSongs);
            });
        }
        co_return co_await and_then(parse<PlaylistReply>(co_await client.call("/api/v6/playlist/detail", std::format(R"("id":{},"n":100000,"s":8)", card.id))),
                                    [this](PlaylistReply r) -> Task<Result<std::vector<Track>>> {
                                        if (!r.playlist) {
                                            co_return std::vector<Track>{};
                                        }
                                        auto out = tracks_of(r.playlist->tracks);
                                        // Long playlists list every id but only the first tracks in full.
                                        const auto &ids = r.playlist->trackIds;
                                        if (ids && ids->size() > out.size()) {
                                            std::vector<std::string> rest;
                                            for (size_t i = out.size(); i < ids->size(); ++i) {
                                                rest.push_back(std::to_string((*ids)[i].id));
                                            }
                                            co_return (co_await song_detail(std::move(rest))).transform([&](std::vector<Track> more) {
                                                out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
                                                return std::move(out);
                                            });
                                        }
                                        co_return out;
                                    });
    }

    Task<Result<std::vector<Track>>> Netease::song_detail(std::vector<std::string> ids) {
        Result<std::vector<Track>> out(std::in_place);
        for (size_t i = 0; out && i < ids.size(); i += 500) {
            std::string c = "[";
            for (size_t k = i; k < std::min(ids.size(), i + 500); ++k) {
                std::format_to(std::back_inserter(c), "{}{{\"id\":{}}}", k > i ? "," : "", ids[k]);
            }
            c += "]";
            out = parse<SongsReply>(co_await client.call("/api/v3/song/detail", std::format(R"("c":{})", glz::write_json(c).value()), Api::weapi))
                      .transform([&](const SongsReply &r) {
                          auto part = tracks_of(r.songs);
                          out->insert(out->end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<QrCode>> Netease::qr_start() {
        co_return parse<QrKeyReply>(co_await client.call("/api/login/qrcode/unikey", R"("type":3)")).and_then([&](auto r) -> Result<QrCode> {
            if (!r.unikey) {
                return std::unexpected("Could not get a QR code");
            }
            qr_key_ = *r.unikey;
            return QrCode{.text = "https://music.163.com/login?codekey=" + qr_key_};
        });
    }

    Task<Result<QrStatus>> Netease::qr_poll() {
        Reply reply = co_await client.call("/api/login/qrcode/client/login", std::format(R"("key":{},"type":3)", glz::write_json(qr_key_).value()));
        co_return parse<QrCheckReply>(reply).and_then([&](auto r) -> Result<QrStatus> {
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
                    s.nickname = r.nickname.value_or("");
                    s.avatar = https(r.avatarUrl.value_or(""));
                    break;
                case 803:
                    s.state = QrState::confirmed;
                    break;
                default:
                    return std::unexpected("Sign-in failed (" + std::to_string(reply.code) + ')');
            }
            return s;
        });
    }

    Task<Result<Account>> Netease::account() {
        const std::string login = client.get("MUSIC_U");
        Reply reply = co_await client.call("/api/w/nuser/account/get", "", Api::weapi);
        auto r = parse<AccountReply>(reply);
        // QR logins cannot be refreshed; once the session is gone the user has to scan again.
        if (reply.code == 301 || (r && !r->profile)) {
            if (login == client.get("MUSIC_U")) {
                client.clear_login();
            }
            co_return std::unexpected("NetEase Music session expired; scan again to sign in");
        }
        co_return std::move(r).transform([](const AccountReply &r) {
            return Account{.id = std::to_string(r.profile->userId), .name = r.profile->nickname.value_or(""), .avatar = https(r.profile->avatarUrl.value_or(""))};
        });
    }

    Task<void> Netease::logout() {
        const std::string login = client.get("MUSIC_U");
        co_await client.call("/api/logout", "", Api::eapi, net::Retry::never);
        if (login == client.get("MUSIC_U")) {
            client.clear_login();
        }
    }

    Task<Result<std::vector<Card>>> Netease::recommended_playlists() {
        co_return parse<PlaylistsReply>(co_await client.call("/api/personalized/playlist", R"("limit":30,"total":true,"n":1000)", Api::weapi))
            .transform([&](auto r) -> std::vector<Card> {
                std::vector<Card> out;
                if (r.result) {
                    for (const auto &p : *r.result) {
                        out.push_back(playlist_card(p));
                    }
                }
                return out;
            });
    }

    Task<Result<std::vector<Card>>> Netease::toplists() {
        co_return parse<PlaylistsReply>(co_await client.call("/api/toplist", "")).transform([&](auto r) -> std::vector<Card> {
            std::vector<Card> out;
            if (r.list) {
                for (const auto &p : *r.list) {
                    out.push_back(playlist_card(p));
                }
            }
            return out;
        });
    }

    Result<std::vector<Track>> parse_new_songs(const Reply &reply) {
        return parse<FmReply>(reply).and_then([&](auto r) -> Result<std::vector<Track>> {
            if (!r.data) {
                return std::unexpected("NetEase Music returned no new songs");
            }
            return tracks_of(r.data);
        });
    }

    Result<Batch> parse_new_albums(const Reply &reply, int offset) {
        return parse<NewAlbumsReply>(reply).and_then([&](auto r) -> Result<Batch> {
            if (!r.albums) {
                return std::unexpected("NetEase Music returned no new albums");
            }
            Batch out{.total = r.total, .next_offset = !r.albums->empty() && offset + 20 < r.total ? offset + 20 : -1};
            for (const auto &a : *r.albums) {
                out.cards.push_back(album_card(a));
            }
            return out;
        });
    }

    Task<Result<std::vector<Track>>> Netease::new_songs(std::string area) {
        co_return parse_new_songs(co_await client.call("/api/v1/discovery/new/songs", std::format(R"("areaId":{},"total":true)", area), Api::weapi));
    }

    Task<Result<Batch>> Netease::new_albums(std::string area, int offset) {
        co_return parse_new_albums(
            co_await client.call("/api/album/new", std::format(R"("area":{},"limit":20,"offset":{},"total":true)", glz::write_json(area).value(), offset), Api::weapi), offset);
    }

    Task<Result<std::vector<Track>>> Netease::daily() {
        co_return parse<DailyReply>(co_await client.call("/api/v3/discovery/recommend/songs", "", Api::weapi)).transform([&](auto r) -> std::vector<Track> {
            return r.data ? tracks_of(r.data->dailySongs) : std::vector<Track>{};
        });
    }

    Task<Result<std::vector<Card>>> Netease::daily_playlists() {
        co_return parse<PlaylistsReply>(co_await client.call("/api/v1/discovery/recommend/resource", "", Api::weapi)).transform([&](auto r) -> std::vector<Card> {
            std::vector<Card> out;
            if (r.recommend) {
                for (const auto &p : *r.recommend) {
                    out.push_back(playlist_card(p));
                }
            }
            return out;
        });
    }

    Task<Result<std::optional<Track>>> Netease::dislike_daily(std::string id) {
        co_return parse<DislikeReply>(
            co_await client.call("/api/v2/discovery/recommend/dislike", std::format(R"("resId":{},"resType":4,"sceneType":1)", id), Api::weapi, net::Retry::never))
            .transform([&](auto r) -> std::optional<Track> {
                if (!r.data) {
                    return std::nullopt;
                }
                return track(*r.data);
            });
    }

    Task<Result<std::vector<Track>>> Netease::heartbeat(std::string seed, std::string playlist) {
        co_return parse<HeartbeatReply>(co_await client.call("/api/playmode/intelligence/list",
                                                             std::format(R"("songId":{},"type":"fromPlayOne","playlistId":{},"startMusicId":{},"count":1)", seed, playlist, seed)))
            .transform([&](auto r) -> std::vector<Track> {
                std::vector<Track> out;
                if (r.data) {
                    for (const auto &d : *r.data) {
                        if (d.songInfo) {
                            out.push_back(track(*d.songInfo));
                        }
                    }
                }
                return out;
            });
    }

    Task<Result<std::vector<Track>>> Netease::similar(std::string id) {
        co_return parse<SongsReply>(co_await client.call("/api/v1/discovery/simiSong", std::format(R"("songid":{},"limit":50,"offset":0)", id), Api::weapi))
            .transform([&](auto r) -> std::vector<Track> {
                return tracks_of(r.songs);
            });
    }

    Task<Result<std::vector<Track>>> Netease::recent_plays() {
        co_return parse<RecordsReply>(co_await client.call("/api/play-record/song/list", R"("limit":100)", Api::weapi)).transform([&](auto r) -> std::vector<Track> {
            std::vector<Track> out;
            if (r.data && r.data->list) {
                for (const auto &rec : *r.data->list) {
                    if (rec.data) {
                        out.push_back(track(*rec.data));
                    }
                }
            }
            return out;
        });
    }

    Task<Result<std::vector<Track>>> Netease::fm() {
        co_return parse<FmReply>(co_await client.call("/api/v1/radio/get", "", Api::weapi)).transform([&](auto r) -> std::vector<Track> {
            return tracks_of(r.data);
        });
    }

    Task<Result<void>> Netease::fm_trash(Track track) {
        co_return done(co_await client.call("/api/radio/trash/add", std::format(R"("songId":{},"alg":"RT","time":25)", track.id), Api::weapi, net::Retry::never));
    }

    Task<Result<std::vector<Track>>> Netease::cloud() {
        Result<std::vector<Track>> out(std::in_place);
        bool more = true;
        for (int offset = 0; out && more; offset += 500) {
            out = parse<CloudReply>(co_await client.call("/api/v1/cloud/get", std::format(R"("limit":500,"offset":{})", offset), Api::weapi)).transform([&](const CloudReply &r) {
                if (r.data) {
                    for (const auto &c : *r.data) {
                        if (c.simpleSong) {
                            out->push_back(track(*c.simpleSong));
                            out->back().cloud = true;
                        }
                    }
                }
                more = r.hasMore.value_or(false) && r.data && !r.data->empty();
                return std::move(*out);
            });
        }
        co_return out;
    }

    Result<PlaylistPage> parse_user_playlists(const Reply &reply, std::string_view uid) {
        return parse<PlaylistsReply>(reply).and_then([&](auto r) -> Result<PlaylistPage> {
            if (!r.playlist) {
                return std::unexpected("NetEase Music returned no playlists");
            }
            PlaylistPage out{.more = r.more && !r.playlist->empty()};
            for (const auto &p : *r.playlist) {
                Card c = playlist_card(p);
                c.owned = p.creator && std::to_string(p.creator->userId) == uid;
                c.collectable = c.collectable && !c.owned;
                if (p.specialType == 5 && c.owned) {
                    out.liked = std::move(c);
                } else {
                    out.playlists.push_back(std::move(c));
                }
            }
            return out;
        });
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Netease::user_playlists(std::string uid) {
        Result<std::pair<Card, std::vector<Card>>> out(std::in_place);
        bool more = true;
        for (int offset = 0; out && more; offset += 100) {
            out = parse_user_playlists(co_await client.call("/api/user/playlist", std::format(R"("uid":{},"limit":100,"offset":{},"includeVideo":true)", uid, offset), Api::weapi),
                                       uid)
                      .transform([&](PlaylistPage r) {
                          if (!r.liked.id.empty()) {
                              out->first = std::move(r.liked);
                          }
                          out->second.insert(out->second.end(), std::make_move_iterator(r.playlists.begin()), std::make_move_iterator(r.playlists.end()));
                          more = r.more;
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<std::vector<std::string>>> Netease::liked_ids(std::string uid) {
        co_return parse<IdsReply>(co_await client.call("/api/song/like/get", std::format(R"("uid":{})", uid))).transform([&](auto r) -> std::vector<std::string> {
            std::vector<std::string> out;
            if (r.ids) {
                for (uint64_t id : *r.ids) {
                    out.push_back(std::to_string(id));
                }
            }
            return out;
        });
    }

    Task<Result<void>> Netease::like(Track track, bool on) {
        co_return done(co_await client.call("/api/radio/like", std::format(R"("alg":"itembased","trackId":{},"like":{},"time":"3")", track.id, on), Api::weapi, net::Retry::never));
    }

    Task<Result<Categories>> Netease::categories() {
        co_return parse<CatalogueReply>(co_await client.call("/api/playlist/catalogue", "")).transform([&](auto r) -> Categories {
            Categories out;
            if (!r.sub || !r.categories) {
                return out;
            }
            for (const auto &[key, heading] : *r.categories) {
                auto &g = out.groups.emplace_back(heading, std::vector<Category>{});
                for (const auto &c : *r.sub) {
                    if (std::to_string(c.category) == key) {
                        g.second.push_back({c.name.value_or(""), c.name.value_or("")});
                    }
                }
            }
            for (const auto &c : *r.sub) {
                if (c.hot) {
                    out.hot.push_back({c.name.value_or(""), c.name.value_or("")});
                }
            }
            return out;
        });
    }

    Task<Result<Batch>> Netease::square(std::string category, int offset) {
        if (category.empty()) {
            category = "\u5168\u90e8";
        }
        co_return parse<SquareReply>(
            co_await client.call("/api/playlist/list",
                                 std::format(R"("cat":{},"order":"hot","limit":50,"offset":{},"total":true)", glz::write_json(glz::escape_bytes_t{category}).value(), offset),
                                 Api::weapi))
            .transform([&](auto r) -> Batch {
                Batch out{.total = static_cast<int>(r.total), .next_offset = offset + 50 < r.total ? offset + 50 : -1};
                if (r.playlists) {
                    for (const auto &p : *r.playlists) {
                        out.cards.push_back(listed_playlist(p));
                    }
                }
                return out;
            });
    }

    Task<Result<ArtistInfo>> Netease::artist(std::string id) {
        co_return co_await and_then(
            parse<ArtistHeadReply>(co_await client.call("/api/artist/head/info/get", std::format(R"("id":{})", id))), [&](auto r) -> Task<Result<ArtistInfo>> {
                if (!r.data || !r.data->artist) {
                    co_return std::unexpected("Artist not found");
                }
                const NArtistHead &a = *r.data->artist;
                ArtistInfo out{
                    .name = a.name.value_or(""), .avatar = https(a.avatar.value_or("")), .songs = static_cast<int>(a.musicSize), .albums = static_cast<int>(a.albumSize)};
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
            });
    }

    Task<Result<Batch>> Netease::artist_songs(std::string id, int offset) {
        co_return parse<ArtistSongsReply>(
            co_await client.call("/api/v1/artist/songs", std::format(R"("id":{},"private_cloud":"true","work_type":1,"order":"hot","offset":{},"limit":100)", id, offset)))
            .transform([&](auto r) -> Batch {
                return Batch{.tracks = tracks_of(r.songs), .total = static_cast<int>(r.total), .next_offset = offset + 100 < r.total ? offset + 100 : -1};
            });
    }

    Task<Result<Batch>> Netease::artist_albums(std::string id, int offset) {
        co_return parse<ArtistAlbumsReply>(co_await client.call("/api/artist/albums/" + id, std::format(R"("limit":30,"offset":{},"total":true)", offset), Api::weapi))
            .transform([&](auto r) -> Batch {
                Batch out{.total = r.artist ? static_cast<int>(r.artist->albumSize) : 0};
                out.next_offset = offset + 30 < out.total ? offset + 30 : -1;
                if (r.hotAlbums) {
                    for (const auto &a : *r.hotAlbums) {
                        out.cards.push_back(album_card(a));
                    }
                }
                return out;
            });
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Netease::artist_intro(std::string id) {
        co_return parse<IntroReply>(co_await client.call("/api/artist/introduction", std::format(R"("id":{})", id), Api::weapi))
            .transform([&](auto r) -> std::vector<std::pair<std::string, std::string>> {
                std::vector<std::pair<std::string, std::string>> out;
                if (std::string brief = trimmed(r.briefDesc); !brief.empty()) {
                    out.emplace_back("", std::move(brief));
                }
                if (r.introduction) {
                    for (const auto &s : *r.introduction) {
                        out.emplace_back(trimmed(s.ti), trimmed(s.txt));
                    }
                }
                return out;
            });
    }

    Task<Result<AlbumInfo>> Netease::album(std::string id) {
        co_return co_await and_then(parse<AlbumReply>(co_await client.call("/api/v1/album/" + id, "", Api::weapi)), [&](auto r) -> Task<Result<AlbumInfo>> {
            AlbumInfo out{.thread = id, .tracks = tracks_of(r.songs)};
            if (const auto &a = r.album) {
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
        });
    }

    Task<Result<void>> Netease::follow(std::string artist, bool on) {
        co_return done(co_await client.call(on ? "/api/artist/sub" : "/api/artist/unsub", std::format(R"("artistId":"{}","artistIds":"[{}]")", artist, artist), Api::weapi,
                                            net::Retry::never));
    }

    Task<Result<void>> Netease::collect(std::string album, bool on) {
        co_return done(co_await client.call(on ? "/api/album/sub" : "/api/album/unsub", std::format(R"("id":"{}")", album), Api::weapi, net::Retry::never));
    }

    Task<Result<std::vector<Card>>> Netease::collected_albums() {
        Result<std::vector<Card>> out(std::in_place);
        bool more = true;
        for (int offset = 0; out && more; offset += 100) {
            out = parse<SubAlbumsReply>(co_await client.call("/api/album/sublist", std::format(R"("limit":100,"offset":{},"total":true)", offset), Api::weapi))
                      .transform([&](const SubAlbumsReply &r) {
                          if (r.data) {
                              for (const auto &a : *r.data) {
                                  out->push_back({.kind = CardKind::album,
                                                  .id = std::to_string(a.id),
                                                  .title = a.name.value_or(""),
                                                  .subtitle = names(a.artists),
                                                  .cover = https(a.picUrl.value_or(""))});
                              }
                          }
                          more = r.hasMore.value_or(false) && r.data && !r.data->empty();
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Netease::followed_artists() {
        Result<std::vector<Card>> out(std::in_place);
        bool more = true;
        for (int offset = 0; out && more; offset += 100) {
            out = parse<SubArtistsReply>(co_await client.call("/api/artist/sublist", std::format(R"("limit":100,"offset":{},"total":true)", offset), Api::weapi))
                      .transform([&](const SubArtistsReply &r) {
                          if (r.data) {
                              for (const auto &a : *r.data) {
                                  out->push_back({.kind = CardKind::artist,
                                                  .id = std::to_string(a.id),
                                                  .title = a.name.value_or(""),
                                                  .cover = https(a.picUrl.value_or(a.img1v1Url.value_or(""))),
                                                  .count = static_cast<int>(a.albumSize),
                                                  .count_kind = Card::Count::albums});
                              }
                          }
                          more = r.hasMore.value_or(false) && r.data && !r.data->empty();
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<Card>> Netease::create_playlist(std::string name, bool hidden) {
        co_return parse<CreateReply>(
            co_await client.call("/api/playlist/create",
                                 std::format(R"("name":{},"privacy":"{}","type":"NORMAL")", glz::write_json(glz::escape_bytes_t{name}).value(), hidden ? 10 : 0), Api::weapi,
                                 net::Retry::never))
            .and_then([&](auto r) -> Result<Card> {
                if (!r.playlist) {
                    return std::unexpected("No playlist returned");
                }
                Card c = playlist_card(*r.playlist);
                c.owned = true;
                return c;
            });
    }

    Task<Result<void>> Netease::rename_playlist(std::string id, std::string name) {
        co_return done(co_await client.call("/api/playlist/update/name", std::format(R"("id":"{}","name":{})", id, glz::write_json(glz::escape_bytes_t{name}).value()), Api::eapi,
                                            net::Retry::never));
    }

    Task<Result<void>> Netease::delete_playlist(std::string id) {
        co_return done(co_await client.call("/api/playlist/remove", std::format(R"("ids":"[{}]")", id), Api::weapi, net::Retry::never));
    }

    Task<Result<void>> Netease::edit_playlist(std::string id, std::vector<Track> tracks, bool add) {
        std::vector<std::string> ids;
        ids.reserve(tracks.size());
        for (auto &track : tracks) {
            ids.push_back(std::move(track.id));
        }
        auto fields = [&](std::string_view ids) {
            return std::format(R"("op":"{}","pid":"{}","trackIds":{},"imme":"true")", add ? "add" : "del", id, glz::write_json(ids).value());
        };
        Reply r = co_await client.call("/api/playlist/manipulate/tracks", fields(glz::write_json(ids).value()), Api::eapi, net::Retry::never);
        // The server sometimes answers 512 for a single id; listing it twice goes through.
        if (r.code == 512 && ids.size() == 1) {
            ids.push_back(ids.front());
            r = co_await client.call("/api/playlist/manipulate/tracks", fields(glz::write_json(ids).value()), Api::eapi, net::Retry::never);
        }
        if (r.code == 502 && add) {
            co_return std::unexpected("This song is already in the playlist");
        }
        co_return done(r);
    }

    Result<void> parse_playlist_write(const Reply &reply) {
        return parse<WriteReply>(reply).and_then([&](auto r) -> Result<void> {
            if (r.code != 200) {
                std::string message = r.message.value_or(r.msg.value_or(""));
                return std::unexpected(message.empty() ? lyren::trf("NetEase Music playlist operation failed ({})", r.code) : std::move(message));
            }
            return {};
        });
    }

    Task<Result<std::vector<Card>>> Netease::collected_playlists() {
        co_return co_await and_then(account(), [this](Account user) -> Task<Result<std::vector<Card>>> {
            co_return (co_await user_playlists(user.id)).transform([&](auto playlists) -> std::vector<Card> {
                std::erase_if(playlists.second, [](const Card &card) {
                    return card.owned;
                });
                return std::move(playlists.second);
            });
        });
    }

    Task<Result<void>> Netease::collect_playlist(std::string id, bool on) {
        co_return parse_playlist_write(
            co_await client.call(on ? "/api/playlist/subscribe" : "/api/playlist/unsubscribe", std::format(R"("id":{})", id), Api::weapi, net::Retry::never));
    }

    Task<Result<void>> Netease::reorder_playlist(std::string id, std::vector<Track> tracks) {
        std::vector<std::string> ids;
        ids.reserve(tracks.size());
        for (const auto &track : tracks) {
            ids.push_back(track.id);
        }
        co_return parse_playlist_write(
            co_await client.call("/api/playlist/manipulate/tracks",
                                 std::format(R"("pid":{},"trackIds":{},"op":"update")", glz::write_json(id).value(), glz::write_json(glz::write_json(ids).value()).value()),
                                 Api::eapi, net::Retry::never));
    }

    Task<Result<CommentPage>> Netease::comments(Subject subject, std::string id, bool hot, int page, std::string cursor) {
        // Hot comments page by an offset, the newest by the time of the last one; the server hands back either.
        if (cursor.empty()) {
            cursor = hot ? "normalHot#0" : "0";
        }
        co_return parse_comments(
            co_await client.call("/api/v2/resource/comments", std::format(R"("threadId":"{}","pageNo":{},"showInner":true,"pageSize":20,"cursor":{},"sortType":{})",
                                                                          comment_thread(subject, id), page, glz::write_json(glz::escape_bytes_t{cursor}).value(), hot ? 2 : 3)));
    }

    Result<CommentPage> parse_comments(const Reply &reply, bool replies) {
        return comment_response(reply).and_then([&](auto r) -> Result<CommentPage> {
            if (!r.data) {
                return std::unexpected("NetEase Music returned no comment list");
            }
            CommentPage out;
            const CommentsData &d = *r.data;
            out.total = d.totalCount;
            out.more = d.hasMore.value_or(false);
            out.cursor = replies ? std::to_string(d.time) : d.cursor.value_or("");
            if (d.comments) {
                for (const auto &c : *d.comments) {
                    if (c.commentId == 0) {
                        return std::unexpected("NetEase Music returned a comment without an ID");
                    }
                    Comment &m = out.comments.emplace_back();
                    m.id = std::to_string(c.commentId);
                    if (c.user) {
                        m.user = c.user->nickname.value_or(""), m.avatar = https(c.user->avatarUrl.value_or(""));
                        m.user_id = c.user->userId ? std::to_string(c.user->userId) : "";
                    }
                    m.text = trimmed(c.content);
                    m.time = c.timeStr.value_or("");
                    m.likes = c.likedCount;
                    m.liked = c.liked.value_or(false);
                    m.owned = c.owner.value_or(false);
                    m.replies = c.replyCount.value_or(0);
                    m.image = https(c.contentPicUrl.value_or(""));
                    if (c.beReplied && !c.beReplied->empty()) {
                        const NReplied &b = c.beReplied->front();
                        m.reply_user = b.user ? b.user->nickname.value_or("") : "";
                        m.reply = trimmed(b.content);
                    }
                }
            }
            if (out.more && !out.comments.empty() && (replies ? d.time < 0 : out.cursor.empty())) {
                return std::unexpected("NetEase Music returned no comment pagination cursor");
            }
            return out;
        });
    }

    Task<Result<CommentPage>> Netease::comment_replies(Subject subject, std::string id, std::string root, int, std::string cursor) {
        co_return parse_comments(
            co_await client.call("/api/resource/comment/floor/get",
                                 std::format(R"("threadId":"{}","parentCommentId":"{}","time":{},"limit":20)", comment_thread(subject, id), root, cursor.empty() ? "-1" : cursor),
                                 Api::weapi),
            true);
    }

    Result<void> parse_comment_write(const Reply &reply) {
        return comment_response(reply).transform([&](auto) -> void {
            return;
        });
    }

    Task<Result<void>> Netease::like_comment(Subject subject, std::string id, std::string comment, bool on) {
        co_return parse_comment_write(co_await client.call(on ? "/api/v1/comment/like" : "/api/v1/comment/unlike",
                                                           std::format(R"("threadId":"{}","commentId":"{}")", comment_thread(subject, id), comment), Api::weapi,
                                                           net::Retry::never));
    }

    Result<std::string> parse_comment_post(const Reply &reply) {
        return comment_response(reply).and_then([&](auto r) -> Result<std::string> {
            if (!r.comment || r.comment->commentId == 0) {
                return std::unexpected("NetEase Music returned no posted comment ID");
            }
            return std::to_string(r.comment->commentId);
        });
    }

    Task<Result<std::string>> Netease::post_comment(Subject subject, std::string id, std::string content, std::string reply, std::string) {
        std::string fields = std::format(R"("threadId":"{}","content":{})", comment_thread(subject, id), glz::write_json(glz::escape_bytes_t{content}).value());
        if (!reply.empty()) {
            fields += std::format(R"(,"commentId":"{}")", reply);
        }
        co_return parse_comment_post(
            co_await client.call(reply.empty() ? "/api/resource/comments/add" : "/api/resource/comments/reply", std::move(fields), Api::eapi, net::Retry::never));
    }

    Task<Result<void>> Netease::delete_comment(Subject subject, std::string id, std::string comment) {
        co_return parse_comment_write(co_await client.call("/api/resource/comments/delete",
                                                           std::format(R"("threadId":"{}","commentId":"{}")", comment_thread(subject, id), comment), Api::eapi, net::Retry::never));
    }

    Result<Profile> parse_profile(const Reply &reply) {
        return parse<UserDetailReply>(reply).and_then([&](auto r) -> Result<Profile> {
            if (!r.profile || !r.profile->userId) {
                return std::unexpected("NetEase Music returned no such listener");
            }
            const NUserProfile &u = *r.profile;
            return Profile{.id = std::to_string(u.userId),
                           .name = u.nickname.value_or(""),
                           .avatar = https(u.avatarUrl.value_or("")),
                           .background = https(u.backgroundUrl.value_or("")),
                           .signature = trimmed(u.signature),
                           .location = province(u.province),
                           .level = r.level,
                           .follows = u.follows,
                           .fans = u.followeds,
                           .posts = u.eventCount,
                           .playlists = u.playlistCount,
                           .listened = r.listenSongs,
                           .days = r.createDays,
                           .vip = u.vipType > 0,
                           .followed = u.followed.value_or(false),
                           .follows_me = u.followMe.value_or(false)};
        });
    }

    Result<PeoplePage> parse_people(const Reply &reply, bool fans, int offset) {
        return parse<FollowsReply>(reply).and_then([&](auto r) -> Result<PeoplePage> {
            const auto &list = fans ? r.followeds : r.follow;
            if (!list) {
                // A hidden list comes back with the service's reason.
                std::string message = r.message.value_or(r.msg.value_or(""));
                return std::unexpected(message.empty() ? "NetEase Music returned no follow list" : std::move(message));
            }
            PeoplePage out{.total = fans && r.size ? *r.size : -1, .more = r.more.value_or(false) && !list->empty()};
            for (const auto &u : *list) {
                out.people.push_back(person(u));
            }
            if (out.more) {
                out.cursor = std::to_string(offset + static_cast<int>(list->size()));
            }
            return out;
        });
    }

    Result<PostPage> parse_posts(const Reply &reply) {
        return parse<EventsReply>(reply).and_then([&](auto r) -> Result<PostPage> {
            const auto &list = r.events ? r.events : r.event;
            if (!list) {
                return std::unexpected("NetEase Music returned no posts");
            }
            PostPage out{.more = r.more.value_or(false) && !list->empty()};
            for (const auto &e : *list) {
                if (e.id) {
                    out.posts.push_back(post(e));
                }
            }
            if (out.more) {
                if (r.lasttime < 0) {
                    return std::unexpected("NetEase Music returned no post pagination cursor");
                }
                out.cursor = std::to_string(r.lasttime);
            }
            return out;
        });
    }

    Result<Ranking> parse_ranking(const Reply &reply, bool all_time) {
        if (reply.code == -2) {
            return std::unexpected("This listening ranking is private");
        }
        return parse<RankingReply>(reply).and_then([&](auto r) -> Result<Ranking> {
            const auto &list = all_time ? r.allData : r.weekData;
            if (!list) {
                return std::unexpected("NetEase Music returned no listening ranking");
            }
            Ranking out;
            for (const auto &entry : *list) {
                if (entry.song) {
                    out.tracks.push_back(track(*entry.song));
                    out.plays.push_back(entry.playCount);
                    out.scores.push_back(std::clamp(entry.score, 0, 100));
                }
            }
            return out;
        });
    }

    Result<void> parse_social_write(const Reply &reply) {
        return written(reply);
    }

    Task<Result<Profile>> Netease::profile(std::string uid) {
        co_return parse_profile(co_await client.call("/api/v1/user/detail/" + uid, "", Api::weapi));
    }

    Task<Result<PeoplePage>> Netease::people(std::string uid, bool fans, std::string cursor) {
        const int offset = cursor.empty() ? 0 : std::stoi(cursor);
        if (fans) {
            co_return parse_people(
                co_await client.call("/api/user/getfolloweds/" + uid, std::format(R"("userId":{},"time":"0","limit":30,"offset":{},"getcounts":"true")", uid, offset)), true,
                offset);
        }
        co_return parse_people(co_await client.call("/api/user/getfollows/" + uid, std::format(R"("offset":{},"limit":30,"order":true)", offset), Api::weapi), false, offset);
    }

    Task<Result<void>> Netease::follow_user(std::string uid, bool on) {
        co_return written(co_await client.call(std::format("/api/user/{}/{}", on ? "follow" : "delfollow", uid), "", Api::weapi, net::Retry::never));
    }

    Task<Result<PostPage>> Netease::posts(std::string uid, std::string cursor) {
        co_return parse_posts(co_await client.call("/api/event/get/" + uid, std::format(R"("getcounts":true,"time":{},"limit":20,"total":false)", cursor.empty() ? "-1" : cursor)));
    }

    Task<Result<PostPage>> Netease::feed(std::string cursor) {
        co_return parse_posts(co_await client.call("/api/v1/event/get", std::format(R"("pagesize":20,"lasttime":{})", cursor.empty() ? "-1" : cursor), Api::weapi));
    }

    Task<Result<void>> Netease::like_post(Post post, bool on) {
        co_return written(co_await client.call(on ? "/api/resource/like" : "/api/resource/unlike", std::format(R"("threadId":"{}")", comment_thread(Subject::post, post.thread)),
                                               Api::weapi, net::Retry::never));
    }

    Task<Result<void>> Netease::delete_post(std::string id) {
        co_return written(co_await client.call("/api/event/delete", std::format(R"("id":{})", glz::write_json(id).value()), Api::weapi, net::Retry::never));
    }

    Task<Result<void>> Netease::share(std::string text, std::optional<Track> track) {
        co_return written(co_await client.call(
            "/api/share/friends/resource",
            std::format(R"("type":"{}","msg":{},"id":"{}")", track ? "song" : "noresource", glz::write_json(glz::escape_bytes_t{text}).value(), track ? track->id : ""), Api::weapi,
            net::Retry::never));
    }

    Task<Result<void>> Netease::forward(Post post, std::string text) {
        co_return written(co_await client.call("/api/event/forward",
                                               std::format(R"("forwards":{},"id":{},"eventUserId":{})", glz::write_json(glz::escape_bytes_t{text}).value(),
                                                           glz::write_json(post.id).value(), glz::write_json(post.author.id).value()),
                                               Api::weapi, net::Retry::never));
    }

    Task<Result<Ranking>> Netease::ranking(std::string uid, bool all_time) {
        co_return parse_ranking(co_await client.call("/api/v1/play/record", std::format(R"("uid":{},"type":{})", uid, all_time ? 0 : 1), Api::weapi), all_time);
    }

    // Listening reports encode every value as a string; a completed play record ends with "playend".
    Task<void> Netease::report_start(std::string id, std::string source) {
        std::string logs = std::format(R"([{{"action":"startplay","json":{{"id":"{}","type":"song","mainsite":"1","mainsiteWeb":"1","content":"id={}"}}}}])", id, source);
        co_await client.call("/api/feedback/weblog", std::format(R"("logs":{})", glz::write_json(logs).value()), Api::clientlog, net::Retry::never);
    }

    Task<void> Netease::report_play(std::string id, std::string source, int seconds) {
        std::string logs = std::format(R"([{{"action":"play","json":{{"download":0,"end":"playend","id":"{}","sourceId":"{}","time":"{}","type":"song","wifi":0,)"
                                       R"("source":"list","mainsite":"1","mainsiteWeb":"1","content":"id={}"}}}}])",
                                       id, source, seconds, source);
        co_await client.call("/api/feedback/weblog", std::format(R"("logs":{})", glz::write_json(logs).value()), Api::clientlog, net::Retry::never);
    }

} // namespace lyren::ncm
