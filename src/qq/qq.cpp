#include "qq/qq.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <charconv>
#include <ctime>
#include <format>
#include <iterator>
#include <random>
#include <source_location>
#include <curl/curl.h>

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/result_task.hpp"
#include "net/fetch.hpp"
#include "qq/crypto.hpp"
#include "qq/auth.hpp"

namespace lyren::qq {

    // Response models: only what is read; unknown keys are skipped.
    namespace {

        using Str = std::optional<std::string>;
        template <class T>
        using Vec = std::optional<std::vector<T>>;

        // Each part of a request answers with its own code.
        template <class T>
        struct Part {
            int code = 0;
            std::optional<T> data;
        };
        template <class A>
        struct One {
            std::optional<Part<A>> req_0;
        };
        template <class A, class B>
        struct Two {
            std::optional<Part<A>> req_0;
            std::optional<Part<B>> req_1;
        };
        template <class A, class B, class C>
        struct Three {
            std::optional<Part<A>> req_0;
            std::optional<Part<B>> req_1;
            std::optional<Part<C>> req_2;
        };

        struct QSinger {
            Str mid, name;
        };
        struct QAlbumRef {
            Str mid, name, title;
        };
        struct QFile {
            Str media_mid;
            uint32_t try_begin = 0, try_end = 0;
        };
        struct QPay {
            int pay_play = 0;
        };
        struct QVolume {
            float gain = 0, peak = 0;
        };
        struct QSong {
            uint64_t id = 0;
            Str mid, name, title, subtitle;
            Vec<QSinger> singer;
            std::optional<QAlbumRef> album;
            uint32_t interval = 0;
            int type = 0;
            std::optional<QFile> file;
            std::optional<QPay> pay;
            std::optional<QVolume> volume;
        };
        // Song lists wrap each song.
        struct QSongInfo {
            std::optional<QSong> songInfo;
        };

        template <class T>
        struct QList {
            Vec<T> list;
        };
        struct QSearchSinger {
            Str singerMID, singerName, singerPic;
            int songNum = 0, albumNum = 0;
        };
        struct QSearchAlbum {
            Str albumMID, albumName, albumPic, publicTime, singerName;
            int song_count = 0;
        };
        struct QCreator {
            Str name;
        };
        struct QSearchList {
            Str dissid, dissname, imgurl;
            int song_count = 0;
            std::optional<QCreator> creator;
        };
        struct QSearchBody {
            std::optional<QList<QSong>> song;
            std::optional<QList<QSearchAlbum>> album;
            std::optional<QList<QSearchSinger>> singer;
            std::optional<QList<QSearchList>> songlist;
        };
        struct QSearchMeta {
            int sum = 0, nextpage = -1;
        };
        struct QSearch {
            std::optional<QSearchBody> body;
            std::optional<QSearchMeta> meta;
        };

        struct QUrl {
            Str filename, purl;
            int result = 0;
        };
        struct QVkey {
            Vec<std::string> sip;
            Vec<QUrl> midurlinfo;
        };
        struct QDetail {
            std::optional<QSong> track_info;
        };

        struct QLyric {
            Str lyric, trans;
        };

        struct QDiss {
            Vec<QSong> songlist;
            int total_song_num = 0;
            struct Info {
                uint64_t id = 0;
                Str title, picurl, desc;
                Vec<int> vec_tagid;
            };
            std::optional<Info> dirinfo;
        };
        struct QSongList {
            Vec<QSongInfo> songList;
            int totalNum = 0;
        };
        struct QTopDetail {
            Vec<QSong> songInfoList;
        };

        struct QGridCreator {
            Str nick;
        };
        struct QGrid {
            uint64_t id = 0;
            Str title, picurl;
            std::optional<QGridCreator> creator;
        };
        struct QModule {
            Vec<QGrid> grids;
        };
        struct QRecommend {
            Vec<QModule> modules;
        };
        struct QCategory {
            uint32_t id = 0;
            Str name;
            uint8_t status = 0, source = 0;
        };
        struct QCategoryGroup {
            uint32_t group_id = 0;
            Str group_name;
            Vec<QCategory> v_item;
        };
        struct QCategories {
            Vec<QCategoryGroup> v_group;
        };
        struct QPlaylistCover {
            Str medium_url, default_url;
        };
        struct QPlaylistBasic {
            uint64_t tid = 0;
            Str title;
            std::optional<QGridCreator> creator;
            std::optional<QPlaylistCover> cover;
            int song_cnt = 0;
        };
        struct QCategoryItem {
            std::optional<QPlaylistBasic> basic;
        };
        struct QCategoryContent {
            int total_cnt = 0;
            Vec<QCategoryItem> v_item;
        };
        struct QCategoryPlaylists {
            std::optional<QCategoryContent> content;
        };
        struct QFeedItem {
            std::optional<QCategoryItem> Playlist;
        };
        struct QSquare {
            Vec<QFeedItem> List;
            int FromLimit = 0;
            bool HasMore = false;
        };
        struct QRelatedSong {
            std::optional<QSong> track;
        };
        struct QSimilar {
            Vec<QRelatedSong> vecSong;
        };
        struct QRadio {
            Vec<QSong> tracks;
        };
        struct QRadarSong {
            std::optional<QSong> Track;
        };
        struct QRadar {
            Vec<QRadarSong> VecSongs;
        };
        struct QNewSongs {
            Vec<QSong> songlist;
        };
        struct QNewAlbum {
            Str mid, name, release_time;
            Vec<QSinger> singers;
        };
        struct QNewAlbums {
            Vec<QNewAlbum> albums;
            int total = 0;
        };
        struct QTop {
            int topId = 0;
            Str title, frontPicUrl, updateTips;
        };
        struct QTopGroup {
            Vec<QTop> toplist;
        };
        struct QToplists {
            Vec<QTopGroup> group;
        };

        struct QAlbumBasic {
            Str albumName, publishDate, desc;
            uint64_t albumID = 0;
        };
        struct QCompany {
            Str name;
        };
        struct QAlbumSinger {
            Str mid, name;
        };
        struct QAlbumSingers {
            Vec<QAlbumSinger> singerList;
        };
        struct QAlbumDetail {
            std::optional<QAlbumBasic> basicInfo;
            std::optional<QCompany> company;
            std::optional<QAlbumSingers> singer;
        };

        struct QSingerBasic {
            Str name;
        };
        struct QSingerEx {
            Str desc, foreign_name;
        };
        struct QSingerPic {
            Str pic;
        };
        struct QSingerEntry {
            std::optional<QSingerBasic> basic_info;
            std::optional<QSingerEx> ex_info;
            std::optional<QSingerPic> pic;
            Str wiki;
        };
        struct QSingerDetail {
            Vec<QSingerEntry> singer_list;
        };
        struct QAlbumEntry {
            Str albumMid, albumName, publishDate;
            int totalNum = 0;
        };
        struct QAlbumList {
            Vec<QAlbumEntry> albumList;
            int total = 0;
        };

        struct QReplied {
            Str Nick, Content;
        };
        struct QComment {
            Str Nick, Avatar, Content, SeqNo, CmId, RankScore, Pic, PicSize, EmoPic;
            int64_t PubTime = 0;
            int PraiseNum = 0, ReplyCnt = 0, IsPraised = 0, IsSelf = 0;
            Vec<QReplied> RepliedComments;
        };
        struct QCommentList {
            Vec<QComment> Comments;
            int HasMore = 0, Total = 0;
        };
        struct QComments {
            std::optional<QCommentList> CommentList;
            int SubCode = 0;
        };
        struct QCommentPost {
            int SubCode = 0;
            Str Msg, AddedCmId, VerifyUrl;
        };

        struct QSession {
            uint64_t musicid = 0;
            Str musickey, openid, access_token, refresh_token, unionid, encryptUin;
            int64_t expired_at = 0;
        };
        struct QUser {
            Str headurl, nick;
        };
        struct QUsers {
            int code = 0;
            std::optional<std::map<std::string, QUser>> map_userinfo;
        };

        struct QDirectory {
            int dirId = 0;
            uint64_t tid = 0;
            Str dirName, dirPicUrl;
            int songNum = 0;
        };
        struct QDirectories {
            Vec<QDirectory> v_playlist;
        };
        struct QFavorite {
            uint64_t tid = 0;
            Str name, logo;
            int songnum = 0;
        };
        struct QFavorites {
            Vec<QFavorite> v_list;
            int total = 0, hasmore = 0;
        };
        struct QAssetSongs {
            Vec<glz::raw_json_view> list; // only used while parse_playlist's reply is alive
            bool hasmore = false;
        };
        struct QAlbumFavorite {
            Str albumMid, albumName, pic, singerName;
            int songNum = 0;
        };
        struct QAlbumFavorites {
            Vec<QAlbumFavorite> v_list;
        };
        struct QFollowSinger {
            Str SingerMid, SingerName, SingerPic;
        };
        struct QFollowed {
            Vec<QFollowSinger> List;
            int Total = 0;
        };
        struct QWriteResult {
            int dirId = 0, retCode = 0;
        };
        struct QWrite {
            int retCode = 0;
            std::optional<QWriteResult> result;
        };
        struct QPlaylistWrite {
            std::optional<int> result;
            Vec<uint64_t> v_failedPlaylistId;
        };
        struct QPlaylistOrderWrite {
            std::optional<int> retCode;
        };
        struct QDislikeItem {
            Str ID, Name, Img;
            std::optional<int> IdType;
        };
        struct QFeedback {
            std::optional<int> Retcode;
            Vec<QDislikeItem> Songs, Singers, Styles;
        };

        constexpr std::array dislike_fields = {"Songs", "Singers", "Styles"};
        constexpr std::array dislike_commands = {3, 2, 4};
        constexpr std::array dislike_cursors = {"SongLastid", "SingersLastid", "StyleLastid"};

        constexpr glz::opts lax{.error_on_unknown_keys = false};

        template <class T>
        Result<T> parse(const Reply &r, std::source_location origin = std::source_location::current()) {
            if (!r.error.empty()) {
                return std::unexpected(r.error);
            }
            T v{};
            if (auto e = glz::read<lax>(v, r.body)) {
                log::warn("qq {} at line {}: {} at byte {} ({} bytes)", origin.function_name(), origin.line(), glz::format_error(e.ec), e.count, r.body.size());
                return std::unexpected("Could not parse QQ Music response");
            }
            return v;
        }

        // Moves a part's data out of the reply, or returns why it has none.
        template <class T>
        Result<T> take_data(std::optional<Part<T>> &p, std::source_location origin = std::source_location::current()) {
            if (!p) {
                return std::unexpected("QQ Music returned no data");
            }
            if (p->code != 0) {
                log::warn("qq {} at line {}: code {}", origin.function_name(), origin.line(), p->code);
            }
            if (p->code == 2001) {
                return std::unexpected("Sign in to QQ Music and try again");
            }
            if (p->code != 0) {
                return std::unexpected(lyren::trf("QQ Music request failed ({})", p->code));
            }
            if (!p->data) {
                return std::unexpected("QQ Music returned no data");
            }
            return std::move(*p->data);
        }

        template <class T>
        Result<T> parse_data(const Reply &reply, std::source_location origin = std::source_location::current()) {
            return parse<One<T>>(reply, origin).and_then([origin](One<T> r) {
                return take_data(r.req_0, origin);
            });
        }

        // Covers by mid: T001 is an artist's picture, T002 an album's.
        std::string photo(int kind, std::string_view mid) {
            return mid.empty() ? "" : std::format("https://y.gtimg.cn/music/photo_new/T00{}R300x300M000{}.jpg", kind, mid);
        }

        Card playlist_card(const QPlaylistBasic &p) {
            return {.source = Source::qq,
                    .id = std::to_string(p.tid),
                    .title = p.title.value_or(""),
                    .subtitle = p.creator ? p.creator->nick.value_or("") : "",
                    .cover = p.cover ? p.cover->medium_url.value_or(p.cover->default_url.value_or("")) : "",
                    .count = p.song_cnt,
                    .count_kind = Card::Count::songs,
                    .collectable = true};
        }

        Track track(const QSong &s) {
            Track t{.id = std::to_string(s.id), .mid = s.mid.value_or(""), .source = Source::qq};
            t.title = s.title.value_or(s.name.value_or(""));
            t.alias = s.subtitle.value_or("");
            if (s.file) {
                t.media = s.file->media_mid.value_or("");
            }
            if (s.singer) {
                for (const auto &a : *s.singer) {
                    t.artist += (t.artist.empty() ? "" : " / ") + a.name.value_or("");
                }
                if (!s.singer->empty()) {
                    t.artist_id = s.singer->front().mid.value_or("");
                }
            }
            if (s.album) {
                t.album = s.album->title.value_or(s.album->name.value_or(""));
                t.album_id = s.album->mid.value_or("");
            }
            t.cover = t.album_id.empty() ? photo(1, t.artist_id) : photo(2, t.album_id);
            t.duration = s.interval;
            t.type = s.type;
            t.paid = s.pay && s.pay->pay_play != 0;
            return t;
        }

        std::vector<Track> tracks_of(const Vec<QSong> &songs) {
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

        std::vector<Track> tracks_of(const Vec<QSongInfo> &songs) {
            std::vector<Track> out;
            if (!songs) {
                return out;
            }
            out.reserve(songs->size());
            for (const auto &s : *songs) {
                if (s.songInfo) {
                    out.push_back(track(*s.songInfo));
                }
            }
            return out;
        }

        Result<std::vector<Track>> discovery_tracks(const Vec<QSong> &songs) {
            if (!songs) {
                return std::unexpected("QQ Music returned no song list");
            }
            for (const auto &song : *songs) {
                if (!song.id) {
                    return std::unexpected("QQ Music returned incomplete song details");
                }
            }
            return tracks_of(songs);
        }

        Result<std::vector<Track>> asset_tracks(const Vec<glz::raw_json_view> &songs) {
            std::vector<Track> out;
            if (!songs) {
                return out;
            }
            out.reserve(songs->size());
            for (size_t i = 0; i < songs->size(); ++i) {
                std::string_view json = (*songs)[i].str;
                std::string_view path = "direct";
                if (auto view = glz::get_view_json<"/songInfo">(json)) {
                    json = {view->data(), view->size()};
                    path = "songInfo";
                } else if (auto view = glz::get_view_json<"/track">(json)) {
                    json = {view->data(), view->size()};
                    path = "track";
                }
                QSong s;
                if (auto e = glz::read<lax>(s, json)) {
                    log::warn("qq playlist item {} ({}): {} at byte {} ({} bytes)", i, path, glz::format_error(e.ec), e.count, json.size());
                    return std::unexpected("Could not parse QQ Music song details");
                }
                if (s.id == 0) {
                    log::warn("qq playlist item {} ({}): missing song id", i, path);
                    return std::unexpected("QQ Music returned incomplete song details");
                }
                out.push_back(track(s));
            }
            return out;
        }

        Result<QWrite> written(const Reply &reply) {
            return parse_data<QWrite>(reply).and_then([&](auto d) -> Result<QWrite> {
                int code = d.retCode != 0 ? d.retCode : d.result ? d.result->retCode : 0;
                if (code != 0) {
                    return std::unexpected(lyren::trf("QQ Music did not complete the operation ({})", code));
                }
                return d;
            });
        }

        Result<QFeedback> feedback(const Reply &reply) {
            return parse_data<QFeedback>(reply).and_then([&](auto d) -> Result<QFeedback> {
                if (!d.Retcode) {
                    return std::unexpected("QQ Music returned no recommendation feedback result");
                }
                if (*d.Retcode != 0) {
                    return std::unexpected(lyren::trf("QQ Music recommendation feedback failed ({})", *d.Retcode));
                }
                return d;
            });
        }

        Card album_card(std::string mid, std::string title, std::string_view date, int count) {
            std::string year(date.substr(0, 4));
            std::string cover = photo(2, mid);
            return {.kind = CardKind::album,
                    .source = Source::qq,
                    .id = std::move(mid),
                    .title = std::move(title),
                    .subtitle = std::move(year),
                    .cover = std::move(cover),
                    .count = count,
                    .count_kind = Card::Count::songs};
        }

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

        // Comment times use the day and year relative to today.
        std::string when(int64_t seconds) {
            std::time_t t = static_cast<std::time_t>(seconds), now = std::time(nullptr);
            std::tm a, b;
            localtime_r(&t, &a);
            localtime_r(&now, &b);
            std::time_t yesterday = now - 86400;
            std::tm y;
            localtime_r(&yesterday, &y);
            if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) {
                return std::format("{:02}:{:02}", a.tm_hour, a.tm_min);
            }
            if (a.tm_year == y.tm_year && a.tm_yday == y.tm_yday) {
                return lyren::trf("Yesterday {:02}:{:02}", a.tm_hour, a.tm_min);
            }
            if (a.tm_year == b.tm_year) {
                return lyren::trf("{}-{}", a.tm_mon + 1, a.tm_mday);
            }
            return lyren::trf("{}-{}-{}", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
        }

        // The text of the next <tag><![CDATA[…]]></tag> in s after at, moving at past it; at becomes npos when there is none.
        std::string cdata(std::string_view s, std::string_view tag, size_t &at) {
            std::string open = '<' + std::string(tag) + "><![CDATA[";
            size_t b = s.find(open, at), e = b == s.npos ? b : s.find("]]>", b);
            if (e == s.npos) {
                at = s.npos;
                return {};
            }
            b += open.size();
            at = e + 3;
            return std::string(s.substr(b, e - b));
        }

        // Toplists share no ids with playlists; they are told apart by this prefix.
        constexpr std::string_view top_prefix = "top:";

        std::string millis() {
            return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string random_id() {
            std::random_device rng;
            return std::format("{:08x}{:08x}{:08x}{:08x}", rng(), rng(), rng(), rng());
        }

        Result<StringMap<std::string>> login_session(const Reply &reply) {
            return parse_data<QSession>(reply).and_then([&](auto s) -> Result<StringMap<std::string>> {
                if (!s.musicid || s.musickey.value_or("").empty() || s.openid.value_or("").empty() || s.refresh_token.value_or("").empty()) {
                    return std::unexpected("QQ Music returned incomplete credentials");
                }
                return StringMap<std::string>{{"musicid", std::to_string(s.musicid)},
                                              {"musickey", *s.musickey},
                                              {"openid", *s.openid},
                                              {"access_token", s.access_token.value_or("")},
                                              {"refresh_token", *s.refresh_token},
                                              {"unionid", s.unionid.value_or("")},
                                              {"expired_at", std::to_string(s.expired_at)},
                                              {"encrypt_uin", s.encryptUin.value_or("")}};
            });
        }

    } // namespace

    Result<PlaylistPage> parse_playlist(const Reply &reply) {
        return parse_data<QAssetSongs>(reply).and_then([&](auto d) -> Result<PlaylistPage> {
            return asset_tracks(d.list).transform([&](auto tracks) -> PlaylistPage {
                return PlaylistPage{std::move(tracks), d.hasmore};
            });
        });
    }

    Result<FavoritePlaylistsPage> parse_favorite_playlists(const Reply &reply) {
        return parse_data<QFavorites>(reply).and_then([&](auto d) -> Result<FavoritePlaylistsPage> {
            FavoritePlaylistsPage out{.total = d.total, .more = d.hasmore != 0};
            if (d.v_list) {
                for (const auto &p : *d.v_list) {
                    if (!p.tid) {
                        return std::unexpected("QQ Music returned an invalid saved playlist");
                    }
                    out.playlists.push_back({.source = Source::qq,
                                             .id = std::to_string(p.tid),
                                             .title = p.name.value_or(""),
                                             .cover = p.logo.value_or(""),
                                             .count = p.songnum,
                                             .count_kind = Card::Count::songs,
                                             .collectable = true});
                }
            }
            return out;
        });
    }

    Result<Categories> parse_categories(const Reply &reply) {
        return parse_data<QCategories>(reply).transform([&](auto d) -> Categories {
            Categories out;
            if (d.v_group) {
                for (const auto &group : *d.v_group) {
                    if (!group.v_item) {
                        continue;
                    }
                    std::vector<Category> entries;
                    for (const auto &item : *group.v_item) {
                        // AI categories have a separate content contract; only expose the installed reader.
                        if (item.status != 0 || item.source != 0 || !item.id || !item.name) {
                            continue;
                        }
                        entries.push_back({std::to_string(item.id), *item.name});
                        if (group.group_id == 1) {
                            out.hot.push_back(entries.back());
                        }
                    }
                    if (!entries.empty()) {
                        out.groups.emplace_back(group.group_name.value_or(""), std::move(entries));
                    }
                }
            }
            return out;
        });
    }

    Result<Batch> parse_square(const Reply &reply, int offset) {
        return parse_data<QSquare>(reply).and_then([&](auto d) -> Result<Batch> {
            Batch out;
            if (d.List) {
                out.cards.reserve(d.List->size());
                for (const auto &item : *d.List) {
                    if (!item.Playlist || !item.Playlist->basic || !item.Playlist->basic->tid) {
                        return std::unexpected("QQ Music returned an invalid recommended playlist");
                    }
                    out.cards.push_back(playlist_card(*item.Playlist->basic));
                }
            }
            const int end = offset + static_cast<int>(out.cards.size());
            out.total = d.HasMore && !out.cards.empty() ? std::max(d.FromLimit, end + 1) : end;
            return out;
        });
    }

    Result<Batch> parse_category_playlists(const Reply &reply) {
        return parse_data<QCategoryPlaylists>(reply).and_then([&](auto d) -> Result<Batch> {
            Batch out;
            if (d.content) {
                out.total = d.content->total_cnt;
                if (d.content->v_item) {
                    out.cards.reserve(d.content->v_item->size());
                    for (const auto &item : *d.content->v_item) {
                        if (!item.basic || !item.basic->tid) {
                            return std::unexpected("QQ Music returned an invalid category playlist");
                        }
                        out.cards.push_back(playlist_card(*item.basic));
                    }
                }
            }
            return out;
        });
    }

    Result<std::vector<Track>> parse_similar(const Reply &reply) {
        return parse_data<QSimilar>(reply).and_then([&](auto d) -> Result<std::vector<Track>> {
            std::vector<Track> out;
            if (d.vecSong) {
                out.reserve(d.vecSong->size());
                for (const auto &item : *d.vecSong) {
                    if (!item.track || !item.track->id || !item.track->mid) {
                        return std::unexpected("QQ Music returned incomplete related song details");
                    }
                    out.push_back(track(*item.track));
                }
            }
            return out;
        });
    }

    Result<std::vector<Track>> parse_radio(const Reply &reply) {
        return parse_data<QRadio>(reply).and_then([&](auto d) -> Result<std::vector<Track>> {
            return discovery_tracks(d.tracks);
        });
    }

    Result<std::vector<Track>> parse_radar(const Reply &reply) {
        return parse_data<QRadar>(reply).and_then([&](auto d) -> Result<std::vector<Track>> {
            if (!d.VecSongs) {
                return std::unexpected("QQ Music returned no radar songs");
            }
            std::vector<Track> out;
            out.reserve(d.VecSongs->size());
            for (const auto &song : *d.VecSongs) {
                if (!song.Track || !song.Track->id) {
                    return std::unexpected("QQ Music returned incomplete radar song details");
                }
                out.push_back(track(*song.Track));
            }
            return out;
        });
    }

    Result<std::vector<Track>> parse_new_songs(const Reply &reply) {
        return parse_data<QNewSongs>(reply).and_then([&](auto d) -> Result<std::vector<Track>> {
            return discovery_tracks(d.songlist);
        });
    }

    Result<Batch> parse_new_albums(const Reply &reply) {
        return parse_data<QNewAlbums>(reply).and_then([&](auto d) -> Result<Batch> {
            if (!d.albums) {
                return std::unexpected("QQ Music returned no album list");
            }
            Batch out{.total = d.total};
            for (const auto &album : *d.albums) {
                if (!album.mid || album.mid->empty()) {
                    return std::unexpected("QQ Music returned incomplete album details");
                }
                std::string artists;
                if (album.singers) {
                    for (const auto &singer : *album.singers) {
                        artists += (artists.empty() ? "" : " / ") + singer.name.value_or("");
                    }
                }
                std::string year = album.release_time.value_or("").substr(0, 4);
                if (!year.empty()) {
                    artists += (artists.empty() ? "" : " · ") + year;
                }
                out.cards.push_back({.kind = CardKind::album,
                                     .source = Source::qq,
                                     .id = *album.mid,
                                     .title = album.name.value_or(""),
                                     .subtitle = std::move(artists),
                                     .cover = photo(2, *album.mid)});
            }
            return out;
        });
    }

    Result<void> parse_playlist_write(const Reply &reply) {
        return parse_data<QPlaylistWrite>(reply).and_then([&](auto d) -> Result<void> {
            if (!d.result || *d.result != 0 || (d.v_failedPlaylistId && !d.v_failedPlaylistId->empty())) {
                return std::unexpected("QQ Music did not complete the playlist operation");
            }
            return {};
        });
    }

    Result<DislikesPage> parse_dislikes(const Reply &reply, DislikeKind kind) {
        return feedback(reply).and_then([&](auto d) -> Result<DislikesPage> {
            const auto &items = kind == DislikeKind::song ? d.Songs : kind == DislikeKind::artist ? d.Singers : d.Styles;
            if (!items) {
                return std::unexpected("QQ Music returned no dislike list");
            }
            DislikesPage out;
            for (const auto &item : *items) {
                if (!item.ID || item.ID->empty() || !item.IdType) {
                    return std::unexpected("QQ Music returned incomplete dislike details");
                }
                out.items.push_back({.id = *item.ID, .name = item.Name.value_or(""), .cover = item.Img.value_or(""), .id_type = *item.IdType, .source = Source::qq, .kind = kind});
            }
            if (!out.items.empty()) {
                out.cursor = out.items.back().id;
            }
            return out;
        });
    }

    Result<void> parse_playlist_order_write(const Reply &reply) {
        return parse_data<QPlaylistOrderWrite>(reply).and_then([&](auto d) -> Result<void> {
            if (!d.retCode) {
                return std::unexpected("QQ Music returned no playlist order result");
            }
            if (*d.retCode != 0) {
                return std::unexpected(lyren::trf("QQ Music did not save the playlist order ({})", *d.retCode));
            }
            return {};
        });
    }

    Result<void> parse_dislike_write(const Reply &reply) {
        return feedback(reply).transform([](const auto &) {});
    }

    Qq::Qq(Loop &loop, net::Http &http)
    : client(loop, http),
      loop_(loop),
      http_(http) {
        client.refresh_session = [this](bool force) {
            return renew(force);
        };
        reload_after_edit = true;
        radio_title = "Your radio";
        features.exclusions.list = [this](DislikeKind kind) {
            return dislikes(kind);
        };
        features.exclusions.set = [this](Dislike item, bool on) {
            return dislike(std::move(item), on);
        };
        release_categories[0] = {{"5", "Latest"}, {"1", "Mainland"}, {"6", "HK/TW"}, {"2", "Western"}, {"4", "Korea"}, {"3", "Japan"}};
        release_categories[1] = {{"1", "Mainland"}, {"2", "HK/TW"}, {"3", "Western"}, {"4", "Korea"}, {"5", "Japan"}};
        features.radar = [this] {
            return radar();
        };
        std::mt19937_64 rng{std::random_device{}()};
        guid_ = std::to_string(1000000000 + rng() % 9000000000);
    }

    Task<Result<void>> Qq::fm_trash(Track track) {
        return dislike({.id = std::move(track.id), .name = std::move(track.title), .source = Source::qq}, true);
    }

    Task<Result<void>> Qq::initialize() {
        if (logged_in()) {
            co_return co_await renew();
        }
        co_return Result<void>{};
    }

    Task<Result<std::vector<Track>>> Qq::fm() {
        co_return parse_radio(co_await client.call({{"music.radioProxy.MbTrackRadioSvr", "get_radio_track", R"({"id":99,"num":5,"from":0,"scene":0,"song_ids":[]})"}}));
    }

    Task<Result<std::vector<Track>>> Qq::radar() {
        co_return parse_radar(co_await client.call({{"music.recommend.TrackRelationServer", "GetRadarSong", R"({"Page":1,"ReqType":0,"FavSongs":[],"EntranceSongs":[]})"}}));
    }

    Task<Result<std::vector<Track>>> Qq::new_songs(std::string type) {
        co_return parse_new_songs(co_await client.call({{"newsong.NewSongServer", "get_new_song_info", std::format(R"({{"type":{}}})", type)}}));
    }

    Task<Result<Batch>> Qq::new_albums(std::string area, int offset) {
        co_return parse_new_albums(co_await client.call({{"newalbum.NewAlbumServer", "get_new_album_info", std::format(R"({{"area":{},"start":{},"num":20}})", area, offset)}}));
    }

    Task<Result<Categories>> Qq::categories() {
        co_return parse_categories(co_await client.call({{"playlist.PlaylistAllCategoriesServer", "get_all_categories", R"({"qq":""})"}}));
    }

    Task<Result<Batch>> Qq::square(std::string category, int offset) {
        constexpr int page_size = 50;
        if (category.empty()) {
            co_return parse_square(co_await client.call({{"music.playlist.PlaylistSquare", "GetRecommendFeed", std::format(R"({{"From":{},"Size":{}}})", offset, page_size)}}),
                                   offset);
        }
        co_return parse_category_playlists(
            co_await client.call({{"playlist.PlayListCategoryServer", "get_category_content",
                                   std::format(R"({{"caller":"0","category_id":{},"page":{},"use_page":1,"size":{}}})", category, offset / page_size, page_size)}}));
    }

    Task<Result<std::vector<Track>>> Qq::similar(std::string id) {
        co_return parse_similar(co_await client.call({{"music.recommend.TrackRelationServer", "GetSimilarSongs", std::format(R"({{"songid":{}}})", id)}}));
    }

    Task<Result<std::vector<Dislike>>> Qq::dislikes(DislikeKind kind) {
        const auto index = static_cast<size_t>(kind);
        Result<std::vector<Dislike>> out(std::in_place);
        std::string cursor;
        bool more = true;
        for (int page = 1; out && more; ++page) {
            std::string param = std::format(R"({{"Cmd":{},"Page":{}}})", dislike_commands[index], page);
            if (!cursor.empty()) {
                param.pop_back();
                std::format_to(std::back_inserter(param), R"(,"{}":{}}})", dislike_cursors[index], glz::write_json(glz::escape_bytes_t{cursor}).value());
            }
            out = parse_dislikes(co_await client.call({{"music.feedback.FeedbackBlack", "GetDislikeList", std::move(param)}}), kind).transform([&](DislikesPage r) {
                more = !r.items.empty();
                cursor = std::move(r.cursor);
                out->insert(out->end(), std::make_move_iterator(r.items.begin()), std::make_move_iterator(r.items.end()));
                return std::move(*out);
            });
        }
        co_return out;
    }

    Task<Result<void>> Qq::dislike(Dislike item, bool on) {
        const auto index = static_cast<size_t>(item.kind);
        std::string param = std::format(R"({{"{}":[{{"ID":{},"Name":{},"IdType":{}}}]}})", dislike_fields[index], glz::write_json(glz::escape_bytes_t{item.id}).value(),
                                        glz::write_json(glz::escape_bytes_t{item.name}).value(), on ? static_cast<int>(index) + 1 : item.id_type);
        co_return parse_dislike_write(
            co_await client.call({{"music.feedback.FeedbackBlack", on ? "AddDislike" : "CancelDislike", std::move(param)}}, Comm::app, net::Retry::never));
    }

    Task<Result<Batch>> Qq::search(std::string query, SearchKind kind, int offset, int limit) {
        static constexpr std::array types = {0, 2, 1, 3}; // songs, albums, artists, playlists
        std::mt19937_64 rng{std::random_device{}()};
        std::string param = std::format(R"({{"query":{},"search_type":{},"page_num":{},"num_per_page":{},"searchid":"{}"}})", glz::write_json(glz::escape_bytes_t{query}).value(),
                                        types[static_cast<int>(kind)], offset / limit + 1, limit, rng() % 100000000000000);
        co_return parse_search(co_await client.call({{"music.search.SearchCgiService", "DoSearchForQQMusicDesktop", param}}), kind, limit);
    }

    Result<Batch> parse_search(const Reply &reply, SearchKind kind, int limit) {
        return parse_data<QSearch>(reply).transform([&](auto d) -> Batch {
            Batch out{.total = d.meta ? d.meta->sum : 0, .next_offset = d.meta && d.meta->nextpage > 0 ? (d.meta->nextpage - 1) * limit : -1};
            if (!d.body) {
                return out;
            }
            const QSearchBody &b = *d.body;
            switch (kind) {
                case SearchKind::tracks:
                    if (b.song) {
                        out.tracks = tracks_of(b.song->list);
                    }
                    break;
                case SearchKind::albums:
                    if (b.album && b.album->list) {
                        for (const auto &a : *b.album->list) {
                            Card c = album_card(a.albumMID.value_or(""), a.albumName.value_or(""), a.publicTime.value_or(""), a.song_count);
                            std::string year = a.publicTime.value_or("").substr(0, 4);
                            c.subtitle = year.empty() ? a.singerName.value_or("") : a.singerName.value_or("") + " · " + year;
                            out.cards.push_back(std::move(c));
                        }
                    }
                    break;
                case SearchKind::artists:
                    if (b.singer && b.singer->list) {
                        for (const auto &a : *b.singer->list) {
                            out.cards.push_back({.kind = CardKind::artist,
                                                 .source = Source::qq,
                                                 .id = a.singerMID.value_or(""),
                                                 .title = a.singerName.value_or(""),
                                                 .cover = a.singerPic.value_or(photo(1, a.singerMID.value_or(""))),
                                                 .count = a.songNum,
                                                 .count_kind = Card::Count::songs});
                        }
                    }
                    break;
                case SearchKind::playlists:
                    if (b.songlist && b.songlist->list) {
                        for (const auto &p : *b.songlist->list) {
                            std::string sub;
                            if (p.creator && p.creator->name) {
                                sub = *p.creator->name;
                            }
                            out.cards.push_back({.kind = CardKind::playlist,
                                                 .source = Source::qq,
                                                 .id = p.dissid.value_or(""),
                                                 .title = p.dissname.value_or(""),
                                                 .subtitle = std::move(sub),
                                                 .cover = p.imgurl.value_or(""),
                                                 .count = p.song_count,
                                                 .count_kind = Card::Count::songs,
                                                 .collectable = true});
                        }
                    }
                    break;
            }
            return out;
        });
    }

    Task<Result<Stream>> Qq::stream(Track t, Quality quality) {
        if (t.mid.empty()) {
            co_return std::unexpected("QQ Music returned no playback details for this song");
        }
        // Down from the quality asked for; the preview clip comes last, for songs that need a membership.
        struct File {
            const char *prefix, *ext, *level;
        };
        static constexpr auto files =
            std::to_array<File>({{"F000", "flac", "lossless"}, {"M800", "mp3", "exhigh"}, {"M500", "mp3", "standard"}, {"C400", "m4a", "aac96"}, {"RS02", "mp3", "standard"}});
        // QQ's confirmed formats have no 192 kbps or Hi-Res tier; use the next supported tier below the request.
        size_t first = quality >= Quality::lossless ? 0 : quality == Quality::exhigh ? 1 : 2;
        std::string mids, types, names;
        for (size_t i = first; i < files.size(); ++i) {
            std::string_view sep = i == first ? "" : ",";
            std::format_to(std::back_inserter(mids), "{}\"{}\"", sep, t.mid);
            std::format_to(std::back_inserter(types), "{}{}", sep, t.type);
            std::format_to(std::back_inserter(names), "{}\"{}{}.{}\"", sep, files[i].prefix, t.media, files[i].ext);
        }
        std::string uin = client.logged_in() ? client.get("musicid") : "0";
        std::string vkey = std::format(R"({{"uin":"{}","guid":"{}","songmid":[{}],"songtype":[{}],"filename":[{}],"ctx":0}})", uin, guid_, mids, types, names);
        // The details carry the loudness.
        std::string detail = std::format(R"({{"song_mid":"{}"}})", t.mid);
        co_return parse<Two<QVkey, QDetail>>(co_await client.call({{"music.vkey.GetVkey", "UrlGetVkey", vkey}, {"music.pf_song_detail_svr", "get_song_detail", detail}}))
            .and_then([&](auto r) -> Result<Stream> {
                return take_data(r.req_0).and_then([&](auto v) -> Result<Stream> {
                    if (!v.sip || v.sip->empty() || !v.midurlinfo) {
                        return std::unexpected("This song is unavailable (membership or licensing restriction)");
                    }
                    for (size_t i = first; i < files.size(); ++i) {
                        std::string name = std::format("{}{}.{}", files[i].prefix, t.media, files[i].ext);
                        auto u = std::ranges::find_if(*v.midurlinfo, [&](const QUrl &x) {
                            return x.filename == name && x.result == 0 && x.purl && !x.purl->empty();
                        });
                        if (u == v.midurlinfo->end()) {
                            continue;
                        }
                        std::string host = v.sip->front();
                        if (host.starts_with("http://")) {
                            host.insert(4, "s");
                        }
                        // The RS02 preview file always holds the song's first 60 s, whatever the detail's try_begin/b_30s
                        // windows say (they mark the highlight segment QQ's own player offers), so its lyrics need no offset.
                        Stream s{.url = host + *u->purl, .level = files[i].level, .trial = std::string_view(files[i].prefix) == "RS02"};
                        auto d = take_data(r.req_1);
                        if (d && d->track_info) {
                            const QSong &song = *d->track_info;
                            if (song.file && song.file->try_end > song.file->try_begin) {
                                s.highlight_from = song.file->try_begin / 1000.0, s.highlight_to = song.file->try_end / 1000.0;
                            }
                            // A clip's loudness is not the song's.
                            if (song.volume && !s.trial) {
                                s.gain = song.volume->gain, s.peak = song.volume->peak;
                            }
                        }
                        return s;
                    }
                    return std::unexpected("This song is unavailable (membership or licensing restriction)");
                });
            });
    }

    Task<Result<Track>> Qq::song_detail(std::string mid) {
        co_return parse_data<QDetail>(co_await client.call({{"music.pf_song_detail_svr", "get_song_detail", std::format(R"({{"song_mid":"{}"}})", mid)}}))
            .and_then([&](auto d) -> Result<Track> {
                if (!d.track_info) {
                    return std::unexpected("QQ Music returned no details for this song");
                }
                return track(*d.track_info);
            });
    }

    Task<Result<std::vector<LyricLine>>> Qq::lyrics(Track t) {
        std::string param = std::format(R"({{"songId":{},"type":{},"crypt":1,"qrc":1,"trans":1,"roma":0,"lrc_t":0,"qrc_t":0,"trans_t":0,"roma_t":0}})", t.id, t.type);
        co_return parse<One<QLyric>>(co_await client.call({{"music.musichallSong.PlayLyricInfo", "GetPlayLyricInfo", param}}))
            .and_then([&](auto r) -> Result<std::vector<LyricLine>> {
                // The service reports songs without a lyric resource as 24001.
                if (r.req_0 && r.req_0->code == 24001) {
                    return std::vector<LyricLine>{};
                }
                return take_data(r.req_0).and_then([&](auto d) -> Result<std::vector<LyricLine>> {
                    std::string text = qrc_decode(d.lyric.value_or("")), trans = qrc_decode(d.trans.value_or(""));
                    return parse_qrc(text, trans);
                });
            });
    }

    Task<Result<std::vector<Track>>> Qq::tracks(Card card) {
        if (card.id.starts_with("dir:")) {
            Result<std::vector<Track>> out(std::in_place);
            bool more = true;
            for (int offset = 0; out && more;) {
                out = parse_playlist(co_await client.call(
                                         {{"music.musicasset.PlaylistDetailRead", "GetUniformSongDetailInfo",
                                           std::format(R"({{"uin":"{}","dirid":{},"bPaged":true,"offset":{},"size":100}})", client.get("musicid"), card.id.substr(4), offset)}}))
                          .transform([&](PlaylistPage page) {
                              more = !page.tracks.empty() && page.more;
                              offset += static_cast<int>(page.tracks.size());
                              out->insert(out->end(), std::make_move_iterator(page.tracks.begin()), std::make_move_iterator(page.tracks.end()));
                              return std::move(*out);
                          });
            }
            co_return out;
        }
        if (card.kind == CardKind::album) {
            co_return (co_await album(card.id)).transform([&](auto a) -> std::vector<Track> {
                return std::move(a.tracks);
            });
        }
        if (card.kind == CardKind::artist) {
            co_return (co_await artist_songs(card.id, 0)).transform([&](auto b) -> std::vector<Track> {
                return std::move(b.tracks);
            });
        }
        if (card.id.starts_with(top_prefix)) {
            co_return parse_data<QTopDetail>(
                co_await client.call({{"music.musicToplist.Toplist", "GetDetail", std::format(R"({{"topId":{},"num":300}})", card.id.substr(top_prefix.size()))}}))
                .transform([&](auto d) -> std::vector<Track> {
                    return tracks_of(d.songInfoList);
                });
        }
        Result<std::vector<Track>> out(std::in_place);
        bool more = true;
        for (int begin = 0; out && more;) {
            out = parse_data<QDiss>(co_await client.call({{"music.srfDissInfo.aiDissInfo", "uniform_get_Dissinfo",
                                                           std::format(R"({{"disstid":{},"userinfo":1,"tag":1,"song_begin":{},"song_num":1000}})", card.id, begin)}}))
                      .transform([&](const QDiss &d) {
                          auto part = tracks_of(d.songlist);
                          begin += static_cast<int>(part.size());
                          more = !part.empty() && begin < d.total_song_num;
                          out->insert(out->end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Qq::recommended_playlists() {
        co_return parse_data<QRecommend>(co_await client.call({{"playlist.HotRecommendServer", "get_new_hot_recommend", R"({"cmd":0,"page":0,"daily_page":0,"size":30})"}}))
            .transform([&](auto d) -> std::vector<Card> {
                std::vector<Card> out;
                if (d.modules) {
                    for (const auto &m : *d.modules) {
                        if (m.grids) {
                            for (const auto &g : *m.grids) {
                                out.push_back({.kind = CardKind::playlist,
                                               .source = Source::qq,
                                               .id = std::to_string(g.id),
                                               .title = g.title.value_or(""),
                                               .subtitle = g.creator ? g.creator->nick.value_or("") : "",
                                               .cover = g.picurl.value_or(""),
                                               .collectable = true});
                            }
                        }
                    }
                }
                return out;
            });
    }

    Task<Result<std::vector<Card>>> Qq::toplists() {
        co_return parse_data<QToplists>(co_await client.call({{"music.musicToplist.Toplist", "GetAll", "{}"}})).transform([&](auto d) -> std::vector<Card> {
            std::vector<Card> out;
            if (d.group) {
                for (const auto &g : *d.group) {
                    if (g.toplist) {
                        for (const auto &t : *g.toplist) {
                            out.push_back({.kind = CardKind::playlist,
                                           .source = Source::qq,
                                           .id = top_prefix + std::to_string(t.topId),
                                           .title = t.title.value_or(""),
                                           .subtitle = t.updateTips.value_or(""),
                                           .cover = t.frontPicUrl.value_or("")});
                        }
                    }
                }
            }
            return out;
        });
    }

    Task<Result<AlbumInfo>> Qq::album(std::string mid) {
        auto result =
            parse<Two<QAlbumDetail, QSongList>>(
                co_await client.call({{"music.musichallAlbum.AlbumInfoServer", "GetAlbumDetail", std::format(R"({{"albumMid":"{}"}})", mid)},
                                      {"music.musichallAlbum.AlbumSongList", "GetAlbumSongList", std::format(R"({{"albumMid":"{}","begin":0,"num":500,"order":2}})", mid)}}))
                .and_then([](Two<QAlbumDetail, QSongList> r) {
                    return take_data(r.req_0).and_then([&](const QAlbumDetail &a) {
                        return take_data(r.req_1).transform([&](const QSongList &s) {
                            AlbumInfo out{.tracks = tracks_of(s.songList)};
                            if (a.singer && a.singer->singerList) {
                                for (const auto &x : *a.singer->singerList) {
                                    out.artist += (out.artist.empty() ? "" : " / ") + x.name.value_or("");
                                }
                                if (!a.singer->singerList->empty()) {
                                    out.artist_id = a.singer->singerList->front().mid.value_or("");
                                }
                            }
                            if (a.basicInfo) {
                                out.date = a.basicInfo->publishDate.value_or("");
                                out.about = trimmed(a.basicInfo->desc);
                                out.thread = std::to_string(a.basicInfo->albumID);
                            }
                            if (a.company) {
                                out.company = a.company->name.value_or("");
                            }
                            return out;
                        });
                    });
                });
        if (client.logged_in()) {
            co_return co_await and_then(std::move(result), [this, &mid](AlbumInfo out) -> Task<Result<AlbumInfo>> {
                co_return (co_await collected_albums()).transform([&](const std::vector<Card> &favorites) {
                    out.collected = std::ranges::any_of(favorites, [&](const Card &c) {
                        return c.id == mid;
                    });
                    return std::move(out);
                });
            });
        }
        co_return result;
    }

    Task<Result<ArtistInfo>> Qq::artist(std::string mid) {
        // The details have no counts; the first entry of each list brings its total.
        auto result = parse<Three<QSingerDetail, QSongList, QAlbumList>>(
                          co_await client.call({{"music.musichallSinger.SingerInfoInter", "GetSingerDetail",
                                                 std::format(R"({{"singer_mids":["{}"],"pic":1,"group_singer":0,"wiki_singer":0,"ex_singer":1}})", mid)},
                                                {"music.musichallSong.SongListInter", "GetSingerSongList", std::format(R"({{"singerMid":"{}","begin":0,"num":1,"order":1}})", mid)},
                                                {"music.musichallAlbum.AlbumListServer", "GetAlbumList", std::format(R"({{"singerMid":"{}","order":0,"begin":0,"num":1}})", mid)}}))
                          .and_then([&](Three<QSingerDetail, QSongList, QAlbumList> r) {
                              return take_data(r.req_0).and_then([&](const QSingerDetail &d) -> Result<ArtistInfo> {
                                  if (!d.singer_list || d.singer_list->empty()) {
                                      return std::unexpected("Artist not found");
                                  }
                                  const QSingerEntry &e = d.singer_list->front();
                                  ArtistInfo out{.name = e.basic_info ? e.basic_info->name.value_or("") : ""};
                                  if (e.ex_info) {
                                      out.alias = e.ex_info->foreign_name.value_or("");
                                  }
                                  out.avatar = e.pic && e.pic->pic ? *e.pic->pic : photo(1, mid);
                                  if (auto s = take_data(r.req_1)) {
                                      out.songs = s->totalNum;
                                  }
                                  if (auto a = take_data(r.req_2)) {
                                      out.albums = a->total;
                                  }
                                  return out;
                              });
                          });
        if (client.logged_in()) {
            co_return co_await and_then(std::move(result), [this, &mid](ArtistInfo out) -> Task<Result<ArtistInfo>> {
                co_return (co_await followed_artists()).transform([&](const std::vector<Card> &followed) {
                    out.followed = std::ranges::any_of(followed, [&](const Card &c) {
                        return c.id == mid;
                    });
                    return std::move(out);
                });
            });
        }
        co_return result;
    }

    Task<Result<Batch>> Qq::artist_songs(std::string mid, int offset) {
        co_return parse_data<QSongList>(
            co_await client.call({{"music.musichallSong.SongListInter", "GetSingerSongList", std::format(R"({{"singerMid":"{}","begin":{},"num":100,"order":1}})", mid, offset)}}))
            .transform([&](auto d) -> Batch {
                return Batch{.tracks = tracks_of(d.songList), .total = d.totalNum};
            });
    }

    Task<Result<Batch>> Qq::artist_albums(std::string mid, int offset) {
        co_return parse_data<QAlbumList>(
            co_await client.call({{"music.musichallAlbum.AlbumListServer", "GetAlbumList", std::format(R"({{"singerMid":"{}","order":0,"begin":{},"num":30}})", mid, offset)}}))
            .transform([&](auto d) -> Batch {
                Batch out{.total = d.total};
                if (d.albumList) {
                    for (const auto &a : *d.albumList) {
                        out.cards.push_back(album_card(a.albumMid.value_or(""), a.albumName.value_or(""), a.publishDate.value_or(""), a.totalNum));
                    }
                }
                return out;
            });
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Qq::artist_intro(std::string mid) {
        co_return parse_data<QSingerDetail>(co_await client.call({{"music.musichallSinger.SingerInfoInter", "GetSingerDetail",
                                                                   std::format(R"({{"singer_mids":["{}"],"pic":0,"group_singer":0,"wiki_singer":1,"ex_singer":1}})", mid)}}))
            .transform([&](auto d) -> std::vector<std::pair<std::string, std::string>> {
                std::vector<std::pair<std::string, std::string>> out;
                if (!d.singer_list || d.singer_list->empty()) {
                    return out;
                }
                const QSingerEntry &e = d.singer_list->front();
                // The wiki: <desc> is the summary, <basic> short facts, <other> headed sections; all in CDATA.
                std::string wiki = e.wiki.value_or("");
                size_t at = 0;
                std::string summary = cdata(wiki, "desc", at);
                if (summary.empty() && e.ex_info) {
                    summary = e.ex_info->desc.value_or("");
                }
                if (!trimmed(summary).empty()) {
                    out.emplace_back("", trimmed(summary));
                }
                for (std::string_view block : {"basic", "other"}) {
                    size_t b = wiki.find('<' + std::string(block) + '>'), end = wiki.find("</" + std::string(block) + '>');
                    if (b == wiki.npos || end == wiki.npos) {
                        continue;
                    }
                    std::string_view items = std::string_view(wiki).substr(b, end - b);
                    std::string facts;
                    for (size_t i = 0; i != items.npos;) {
                        std::string key = cdata(items, "key", i);
                        if (i == items.npos) {
                            break;
                        }
                        std::string value = trimmed(cdata(items, "value", i));
                        if (block == "basic") {
                            std::format_to(std::back_inserter(facts), "{}{}：{}", facts.empty() ? "" : "\n", key, value);
                        } else if (!value.empty()) {
                            out.emplace_back(std::move(key), std::move(value));
                        }
                    }
                    if (!facts.empty()) {
                        out.emplace_back(lyren::tr("Basic information"), std::move(facts));
                    }
                }
                return out;
            });
    }

    Task<Result<CommentPage>> Qq::comments(Subject subject, std::string id, bool hot, int page, std::string cursor) {
        int biz = subject == Subject::song ? 1 : subject == Subject::album ? 2 : 3;
        if (id.starts_with(top_prefix)) {
            biz = 4, id = id.substr(top_prefix.size());
        }
        std::string common =
            std::format(R"("BizType":{},"BizId":"{}","LastCommentSeqNo":{},"PageSize":20,"PageNum":{})", biz, id, glz::write_json(glz::escape_bytes_t{cursor}).value(), page - 1);
        Rpc rpc{"music.globalComment.CommentReadServer", hot ? "GetHotCommentList" : "GetNewCommentList",
                hot ? std::format(R"({{{},"HotType":1,"WithAirborne":0}})", common) : std::format(R"({{{},"FromCommentId":"","WithHot":0}})", common)};
        co_return parse_comments(co_await client.call({rpc}));
    }

    Result<CommentPage> parse_comments(const Reply &reply, bool replies) {
        return parse_data<QComments>(reply).and_then([&](auto d) -> Result<CommentPage> {
            if (d.SubCode != 0) {
                return std::unexpected(lyren::trf("QQ Music comments failed ({})", d.SubCode));
            }
            CommentPage out;
            if (!d.CommentList) {
                return std::unexpected("QQ Music returned no comment list");
            }
            const QCommentList &l = *d.CommentList;
            out.total = l.Total;
            out.more = l.HasMore != 0;
            if (l.Comments) {
                for (const auto &c : *l.Comments) {
                    if (!c.CmId || c.CmId->empty()) {
                        return std::unexpected("QQ Music returned a comment without an ID");
                    }
                    Comment &m = out.comments.emplace_back();
                    m.user = c.Nick.value_or("");
                    m.avatar = c.Avatar.value_or("");
                    m.text = trimmed(c.Content);
                    // The text holds a marker where the sticker or picture goes; the picture is drawn instead.
                    for (auto [url, marker] : {std::pair{&c.EmoPic, std::string_view("[表情]")}, std::pair{&c.Pic, std::string_view("[图片]")}}) {
                        if (url->value_or("").empty()) {
                            continue;
                        }
                        m.image = **url;
                        m.sticker = url == &c.EmoPic;
                        if (size_t at = m.text.rfind(marker); at != std::string::npos) {
                            m.text = trimmed(m.text.erase(at, marker.size()));
                        }
                    }
                    if (std::string size = c.PicSize.value_or(""); !m.sticker && size.contains('x')) {
                        std::from_chars(size.data(), size.data() + size.find('x'), m.image_w);
                        std::from_chars(size.data() + size.find('x') + 1, size.data() + size.size(), m.image_h);
                    }
                    m.time = when(c.PubTime);
                    m.likes = c.PraiseNum;
                    m.id = *c.CmId;
                    m.replies = c.ReplyCnt;
                    m.liked = c.IsPraised != 0;
                    m.owned = c.IsSelf != 0;
                    if (c.RepliedComments && !c.RepliedComments->empty()) {
                        m.reply_user = c.RepliedComments->front().Nick.value_or("");
                        m.reply = trimmed(c.RepliedComments->front().Content);
                    }
                }
                if (!l.Comments->empty()) {
                    const auto &last = l.Comments->back();
                    if (out.more && (!last.SeqNo || last.SeqNo->empty() || (replies && !last.RankScore))) {
                        return std::unexpected("QQ Music returned no comment pagination cursor");
                    }
                    out.cursor = replies ? glz::write_json(std::array{last.SeqNo.value_or(""), last.RankScore.value_or("")}).value() : last.SeqNo.value_or("");
                }
            }
            return out;
        });
    }

    Task<Result<CommentPage>> Qq::comment_replies(Subject, std::string, std::string root, int page, std::string cursor) {
        std::array<std::string, 2> continuation;
        if (!cursor.empty()) {
            static_cast<void>(glz::read_json(continuation, cursor));
        }
        std::string param =
            std::format(R"({{"RootCmId":{},"LastCommentSeqNo":{},"LastRankScore":{},"PageSize":10,"RankType":0,"PageNum":{}}})", glz::write_json(glz::escape_bytes_t{root}).value(),
                        glz::write_json(glz::escape_bytes_t{continuation[0]}).value(), glz::write_json(glz::escape_bytes_t{continuation[1]}).value(), page - 1);
        co_return parse_comments(co_await client.call({{"music.globalComment.CommentReadServer", "GetReplyCommentList", std::move(param)}}), true);
    }

    Result<void> parse_comment_like(const Reply &reply) {
        return parse<One<glz::raw_json>>(reply).and_then([&](auto r) -> Result<void> {
            if (!r.req_0) {
                return std::unexpected("QQ Music returned no comment operation result");
            }
            if (r.req_0->code == 2001) {
                return std::unexpected("Sign in to QQ Music and try again");
            }
            if (r.req_0->code != 0) {
                return std::unexpected(lyren::trf("QQ Music comment operation failed ({})", r.req_0->code));
            }
            return {};
        });
    }

    Task<Result<void>> Qq::like_comment(Subject, std::string, std::string id, bool on) {
        std::string param =
            std::format(R"({{"comment_id":{},"type":{},"uin":{}}})", glz::write_json(glz::escape_bytes_t{id}).value(), on ? 3 : 4, glz::write_json(client.get("musicid")).value());
        co_return parse_comment_like(co_await client.call({{"GlobalComment.GlobalCommentWriteServer", "UpdateHotComment", std::move(param)}}, Comm::app, net::Retry::never));
    }

    static Result<QCommentPost> comment_written(const Reply &reply) {
        return parse_data<QCommentPost>(reply).and_then([&](auto d) -> Result<QCommentPost> {
            if (d.VerifyUrl && !d.VerifyUrl->empty()) {
                return std::unexpected("Complete verification in QQ Music before commenting");
            }
            if (d.SubCode != 0) {
                return std::unexpected(d.Msg && !d.Msg->empty() ? *d.Msg : lyren::trf("QQ Music comment operation failed ({})", d.SubCode));
            }
            return d;
        });
    }

    Result<std::string> parse_comment_post(const Reply &reply) {
        return comment_written(reply).and_then([&](auto d) -> Result<std::string> {
            if (!d.AddedCmId || d.AddedCmId->empty()) {
                return std::unexpected("QQ Music returned no posted comment ID");
            }
            return *d.AddedCmId;
        });
    }

    Task<Result<std::string>> Qq::post_comment(Subject subject, std::string id, std::string content, std::string reply) {
        int biz = subject == Subject::song ? 1 : subject == Subject::album ? 2 : 3;
        if (id.starts_with(top_prefix)) {
            biz = 4, id = id.substr(top_prefix.size());
        }
        std::string param = std::format(R"({{"BizType":{},"BizId":{},"Content":{})", biz, glz::write_json(id).value(), glz::write_json(glz::escape_bytes_t{content}).value());
        if (!reply.empty()) {
            param += std::format(R"(,"RepliedCmId":{})", glz::write_json(glz::escape_bytes_t{reply}).value());
        }
        param += '}';
        co_return parse_comment_post(co_await client.call({{"music.globalComment.CommentWriteServer", "AddComment", std::move(param)}}, Comm::app, net::Retry::never));
    }

    Result<void> parse_comment_delete(const Reply &reply) {
        return comment_written(reply).transform([](const auto &) {});
    }

    Task<Result<void>> Qq::delete_comment(Subject, std::string, std::string id) {
        auto param = std::format(R"({{"CommentId":{}}})", glz::write_json(glz::escape_bytes_t{id}).value());
        co_return parse_comment_delete(co_await client.call({{"music.globalComment.CommentWriteServer", "DelComment", std::move(param)}}, Comm::app, net::Retry::never));
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Qq::user_playlists(std::string uid) {
        co_return co_await and_then(
            parse_data<QDirectories>(co_await client.call({{"music.musicasset.PlaylistBaseRead", "GetPlaylistByUin", std::format(R"({{"uin":"{}"}})", uid)}})),
            [&](auto d) -> Task<Result<std::pair<Card, std::vector<Card>>>> {
                std::pair<Card, std::vector<Card>> out{{.source = Source::qq, .id = "dir:201", .title = "Liked songs", .owned = true}, {}};
                if (d.v_playlist) {
                    for (const auto &p : *d.v_playlist) {
                        if (p.dirId == 201) {
                            continue;
                        }
                        if (!p.dirId || p.dirName.value_or("").empty()) {
                            co_return std::unexpected("QQ Music returned an invalid playlist directory");
                        }
                        out.second.push_back({.source = Source::qq,
                                              .id = "dir:" + std::to_string(p.dirId),
                                              .title = *p.dirName,
                                              .cover = p.dirPicUrl.value_or(""),
                                              .thread = p.tid ? std::to_string(p.tid) : "",
                                              .count = p.songNum,
                                              .count_kind = Card::Count::songs,
                                              .owned = true,
                                              .commentable = p.tid != 0});
                    }
                }
                co_return (co_await collected_playlists()).transform([&](auto favorites) -> std::pair<Card, std::vector<Card>> {
                    out.second.insert(out.second.end(), std::make_move_iterator(favorites.begin()), std::make_move_iterator(favorites.end()));
                    return std::move(out);
                });
            });
    }

    Task<Result<std::vector<Card>>> Qq::collected_playlists() {
        Result<std::vector<Card>> out(std::in_place);
        bool more = true;
        for (int offset = 0; out && more;) {
            out = parse_favorite_playlists(
                      co_await client.call({{"music.musicasset.PlaylistFavRead", "GetPlaylistFavInfo_v2",
                                             std::format(R"({{"uin":{},"offset":{},"size":100}})", glz::write_json(client.get("encrypt_uin")).value(), offset)}}))
                      .transform([&](FavoritePlaylistsPage page) {
                          offset += static_cast<int>(page.playlists.size());
                          more = !page.playlists.empty() && page.more && offset < page.total;
                          out->insert(out->end(), std::make_move_iterator(page.playlists.begin()), std::make_move_iterator(page.playlists.end()));
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<void>> Qq::collect_playlist(std::string id, bool on) {
        co_return parse_playlist_write(co_await client.call({{"music.musicasset.PlaylistFavWrite", on ? "FavPlaylist" : "CancelFavPlaylist",
                                                              std::format(R"({{"uin":{},"v_playlistId":[{}]}})", glz::write_json(client.get("encrypt_uin")).value(), id)}},
                                                            Comm::app, net::Retry::never));
    }

    Task<Result<std::vector<std::string>>> Qq::liked_ids(std::string) {
        co_return (co_await tracks({.source = Source::qq, .id = "dir:201"})).transform([&](auto songs) -> std::vector<std::string> {
            std::vector<std::string> out;
            for (const auto &t : songs) {
                out.push_back(t.id);
            }
            return out;
        });
    }

    Task<Result<void>> Qq::like(Track track, bool on) {
        co_return co_await edit_playlist("dir:201", std::move(track), on);
    }

    Task<Result<void>> Qq::edit_playlist(std::string id, Track t, bool add) {
        // These write endpoints otherwise return Chinese messages in GBK, which is not valid UTF-8 JSON.
        co_return written(co_await client.call({{"music.musicasset.PlaylistDetailWrite", add ? "AddSonglist" : "DelSonglist",
                                                 std::format(R"({{"dirId":{},"bFmtUtf8":true,"v_songInfo":[{{"songId":{},"songType":{}}}]}})", id.substr(4), t.id, t.type)}},
                                               Comm::app, net::Retry::never))
            .transform([](const auto &) {});
    }

    Task<Result<void>> Qq::reorder_playlist(std::string id, std::vector<Track> tracks) {
        std::string param = std::format(R"({{"dirId":{},"v_songPair":[)", id.substr(4));
        for (size_t i = 0; i < tracks.size(); ++i) {
            std::format_to(std::back_inserter(param), R"({}{{"songId":{},"songType":{}}})", i ? "," : "", tracks[i].id, tracks[i].type);
        }
        param += "]}";
        co_return parse_playlist_order_write(co_await client.call({{"music.musicasset.PlaylistDetailWrite", "SeqSonglist", std::move(param)}}, Comm::app, net::Retry::never));
    }

    Task<Result<Card>> Qq::create_playlist(std::string name, bool hidden) {
        co_return written(co_await client.call({{"music.musicasset.PlaylistBaseWrite", "AddPlaylist",
                                                 std::format(R"({{"dirName":{},"dirShow":{},"dirDesc":"","dirPicUrl":"","taglist":""}})",
                                                             glz::write_json(glz::escape_bytes_t{name}).value(), hidden ? 0 : 1)}},
                                               Comm::app, net::Retry::never))
            .and_then([&](auto r) -> Result<Card> {
                if (!r.result || !r.result->dirId) {
                    return std::unexpected("QQ Music returned no directory ID for the new playlist");
                }
                return Card{.source = Source::qq,
                            .id = "dir:" + std::to_string(r.result->dirId),
                            .title = std::move(name),
                            .count_kind = Card::Count::songs,
                            .owned = true,
                            .commentable = false};
            });
    }

    Task<Result<void>> Qq::rename_playlist(std::string id, std::string name) {
        // EditPlaylist replaces all four fields; read and retain the cover, description and tags first.
        co_return co_await and_then(
            parse_data<QDiss>(co_await client.call(
                {{"music.srfDissInfo.aiDissInfo", "uniform_get_Dissinfo", std::format(R"({{"dirid":{},"optype":3,"tag":1,"userinfo":0,"pic_dpi":300}})", id.substr(4))}})),
            [&](auto d) -> Task<Result<void>> {
                if (!d.dirinfo) {
                    co_return std::unexpected("QQ Music returned no playlist details");
                }
                std::string tags;
                if (d.dirinfo->vec_tagid) {
                    for (int tag : *d.dirinfo->vec_tagid) {
                        tags += (tags.empty() ? "" : ",") + std::to_string(tag);
                    }
                }
                const std::string description = d.dirinfo->desc.value_or("");
                co_return written(
                    co_await client.call({{"music.musicasset.PlaylistBaseWrite", "EditPlaylist",
                                           std::format(R"({{"dirId":{},"mask":15,"dirNewName":{},"dirNewDesc":{},"dirNewPicUrl":{},"dirNewtaglist":"{}"}})", id.substr(4),
                                                       glz::write_json(glz::escape_bytes_t{name}).value(), glz::write_json(glz::escape_bytes_t{description}).value(),
                                                       glz::write_json(d.dirinfo->picurl.value_or("")).value(), tags)}},
                                         Comm::app, net::Retry::never))
                    .transform([](const auto &) {});
            });
    }

    Task<Result<void>> Qq::delete_playlist(std::string id) {
        co_return written(
            co_await client.call({{"music.musicasset.PlaylistBaseWrite", "DelPlaylist", std::format(R"({{"dirId":{}}})", id.substr(4))}}, Comm::app, net::Retry::never))
            .transform([](const QWrite &) {});
    }

    Task<Result<void>> Qq::collect(std::string mid, bool on) {
        co_return written(co_await client.call({{"music.musicasset.AlbumFavWrite", on ? "FavAlbum" : "CancelFavAlbum",
                                                 std::format(R"({{"uin":"{}","v_albumMid":["{}"]}})", client.get("musicid"), mid)}},
                                               Comm::app, net::Retry::never))
            .transform([](const auto &) {});
    }

    Task<Result<std::vector<Card>>> Qq::collected_albums() {
        co_return parse_data<QAlbumFavorites>(co_await client.call({{"music.musicasset.AlbumFavRead", "GetAlbumFavInfo", std::format(R"({{"uin":"{}"}})", client.get("musicid"))}}))
            .and_then([&](auto d) -> Result<std::vector<Card>> {
                std::vector<Card> out;
                if (d.v_list) {
                    for (const auto &a : *d.v_list) {
                        if (a.albumMid.value_or("").empty()) {
                            return std::unexpected("QQ Music returned an invalid saved album");
                        }
                        out.push_back({.kind = CardKind::album,
                                       .source = Source::qq,
                                       .id = *a.albumMid,
                                       .title = a.albumName.value_or(""),
                                       .subtitle = a.singerName.value_or(""),
                                       .cover = a.pic.value_or(photo(2, *a.albumMid)),
                                       .count = a.songNum});
                    }
                }
                return out;
            });
    }

    Task<Result<void>> Qq::follow(std::string mid, bool on) {
        co_return written(co_await client.call({{"Concern.ConcernSystemServer", "cgi_concern_user_v2",
                                                 std::format(R"({{"opertype":{},"source":0,"encrypt_singerid":1,"userinfo":{{"usertype":1,"userid":"{}"}}}})", on ? 0 : 1, mid)}},
                                               Comm::app, net::Retry::never))
            .transform([](const auto &) {});
    }

    Task<Result<std::vector<Card>>> Qq::followed_artists() {
        Result<std::vector<Card>> out(std::in_place);
        bool more = true;
        for (int from = 0; out && more;) {
            out = parse_data<QFollowed>(co_await client.call({{"music.concern.RelationList", "GetFollowSingerList", std::format(R"({{"From":{},"Size":100}})", from)}}))
                      .and_then([&](const QFollowed &d) -> Result<std::vector<Card>> {
                          more = d.List && !d.List->empty();
                          if (more) {
                              for (const auto &a : *d.List) {
                                  if (a.SingerMid.value_or("").empty()) {
                                      return std::unexpected("QQ Music returned an invalid followed artist");
                                  }
                                  out->push_back({.kind = CardKind::artist,
                                                  .source = Source::qq,
                                                  .id = *a.SingerMid,
                                                  .title = a.SingerName.value_or(""),
                                                  .cover = a.SingerPic.value_or(photo(1, *a.SingerMid))});
                              }
                              from += static_cast<int>(d.List->size());
                              more = from < d.Total;
                          }
                          return std::move(*out);
                      });
        }
        co_return out;
    }

    Task<Result<QrCode>> Qq::qr_start() {
        uint64_t gen = ++qr_gen_;
        net::CookieJar cookies;
        auto page = co_await net::fetch{
            loop_,
            http_,
            {.url = "https://xui.ptlogin2.qq.com/cgi-bin/xlogin?" +
                    form({{"appid", "716027609"}, {"daid", "383"}, {"pt_3rd_aid", "100497308"}, {"style", "40"}, {"s_url", "https://graph.qq.com/oauth2.0/login_jump"}}),
             .cookie_jar = cookies}};
        if (gen != qr_gen_) {
            co_return std::unexpected("QR sign-in was canceled");
        }
        if (!page.error.empty() || page.status != 200) {
            co_return std::unexpected("Could not connect to the QQ sign-in service");
        }
        cookies = std::move(page.cookie_jar);
        std::string js_version = qr_version(page.body);
        if (js_version.empty()) {
            co_return std::unexpected("QQ sign-in page returned no client version");
        }
        auto qr = co_await net::fetch{loop_,
                                      http_,
                                      {.url = "https://ssl.ptlogin2.qq.com/ptqrshow?" + form({{"appid", "716027609"},
                                                                                              {"e", "2"},
                                                                                              {"l", "M"},
                                                                                              {"s", "3"},
                                                                                              {"d", "72"},
                                                                                              {"v", "4"},
                                                                                              {"t", millis()},
                                                                                              {"daid", "383"},
                                                                                              {"pt_3rd_aid", "100497308"},
                                                                                              {"u1", "https://graph.qq.com/oauth2.0/login_jump"}}),
                                       .headers = {"Referer: https://xui.ptlogin2.qq.com/"},
                                       .cookie_jar = cookies}};
        if (gen != qr_gen_) {
            co_return std::unexpected("QR sign-in was canceled");
        }
        if (!qr.error.empty() || qr.status != 200 || !qr.body.starts_with("\x89PNG\r\n\x1a\n")) {
            co_return std::unexpected("QQ sign-in service returned no QR code");
        }
        cookies = std::move(qr.cookie_jar);
        if (cookies.get("qrsig", "ssl.ptlogin2.qq.com", "/ptqrlogin").empty()) {
            co_return std::unexpected("QQ sign-in service returned no QR token");
        }
        qr_cookies_ = std::move(cookies);
        js_version_ = std::move(js_version);
        oauth_state_ = random_id();
        co_return QrCode{.image = std::move(qr.body)};
    }

    Task<Result<QrStatus>> Qq::qr_poll() {
        uint64_t gen = qr_gen_;
        auto cookies = qr_cookies_;
        std::string state = oauth_state_;
        auto poll = co_await net::fetch{
            loop_,
            http_,
            {.url = "https://ssl.ptlogin2.qq.com/ptqrlogin?" + form({{"u1", "https://graph.qq.com/oauth2.0/login_jump"},
                                                                     {"ptqrtoken", std::to_string(hash33(cookies.get("qrsig", "ssl.ptlogin2.qq.com", "/ptqrlogin")))},
                                                                     {"ptredirect", "0"},
                                                                     {"h", "1"},
                                                                     {"t", "1"},
                                                                     {"g", "1"},
                                                                     {"from_ui", "1"},
                                                                     {"ptlang", "2052"},
                                                                     {"action", "0-0-" + millis()},
                                                                     {"js_ver", js_version_},
                                                                     {"js_type", "1"},
                                                                     {"pt_uistyle", "40"},
                                                                     {"aid", "716027609"},
                                                                     {"daid", "383"},
                                                                     {"pt_3rd_aid", "100497308"},
                                                                     {"login_sig", cookies.get("pt_login_sig", "xui.ptlogin2.qq.com", "/cgi-bin/xlogin")}}),
             .headers = {"Referer: https://xui.ptlogin2.qq.com/"},
             .cookie_jar = cookies}};
        if (gen != qr_gen_) {
            co_return QrStatus{};
        }
        if (!poll.error.empty() || poll.status != 200) {
            co_return std::unexpected("Could not check QQ QR code status");
        }
        cookies = std::move(poll.cookie_jar);
        qr_cookies_ = cookies;
        co_return co_await and_then(qr_callback(poll.body), [&](auto args) -> Task<Result<QrStatus>> {
            if (args[0] == "66") {
                co_return QrStatus{};
            }
            if (args[0] == "67") {
                co_return QrStatus{.state = QrState::scanned, .nickname = args[5]};
            }
            if (args[0] == "65") {
                co_return QrStatus{.state = QrState::expired};
            }
            if (args[0] != "0") {
                co_return std::unexpected(lyren::trf("QQ QR sign-in failed ({})", args[0]));
            }
            co_return co_await and_then(qr_confirmation(args[2]), [&](auto confirm) -> Task<Result<QrStatus>> {
                confirm.cookie_jar = std::move(cookies);
                auto checked = co_await net::fetch{loop_, http_, std::move(confirm)};
                if (gen != qr_gen_) {
                    co_return QrStatus{};
                }
                std::string p_skey = checked.cookie_jar.get("p_skey", "graph.qq.com", "/oauth2.0/authorize");
                if (!checked.error.empty() || checked.status >= 400 || p_skey.empty()) {
                    std::vector<std::string> cookie_names;
                    for (std::string_view cookie : checked.cookies) {
                        if (auto eq = cookie.find('='); eq != cookie.npos) {
                            cookie_names.emplace_back(cookie.substr(0, eq));
                        }
                    }
                    log::warn("qq login confirmation: HTTP {}, URL {}, {} bytes, cookie names {}", checked.status, checked.url.substr(0, checked.url.find_first_of("?#")),
                              checked.body.size(), cookie_names);
                    if (!checked.error.empty() || checked.status >= 400) {
                        co_return std::unexpected("QQ sign-in confirmation failed; scan again");
                    }
                    co_return std::unexpected("QQ sign-in returned no authorization credentials; scan again");
                }
                auto authorized =
                    co_await net::fetch{loop_,
                                        http_,
                                        {.url = "https://graph.qq.com/oauth2.0/authorize",
                                         .body = form({{"response_type", "code"},
                                                       {"client_id", "100497308"},
                                                       {"redirect_uri", "https://y.qq.com/wk_v17/common_login.html?type=QQ"},
                                                       {"scope", "get_user_info"},
                                                       {"state", state},
                                                       {"switch", ""},
                                                       {"from_ptlogin", "1"},
                                                       {"src", "1"},
                                                       {"update_auth", "1"},
                                                       {"openapi", "1010"},
                                                       {"g_tk", std::to_string(hash33(p_skey, 5381))},
                                                       {"auth_time", millis()},
                                                       {"ui", random_id()}}),
                                         .headers = {"Content-Type: application/x-www-form-urlencoded", "Referer: https://graph.qq.com/", "Origin: https://graph.qq.com"},
                                         .follow_redirects = false,
                                         .cookie_jar = std::move(checked.cookie_jar)}};
                if (gen != qr_gen_) {
                    co_return QrStatus{};
                }
                if (!authorized.error.empty() || authorized.status < 300 || authorized.status >= 400) {
                    co_return std::unexpected("QQ authorization was not completed; scan again");
                }
                std::string code = query_value(authorized.location, "code");
                if (code.empty() || query_value(authorized.location, "state") != state) {
                    co_return std::unexpected("QQ authorization state did not match; scan again");
                }
                auto session = login_session(
                    co_await client.call({{"QQConnectLogin.LoginServer", "QQLogin",
                                           std::format(R"({{"onlyNeedAccessToken":0,"forceRefreshToken":0,"appid":100497308,"code":{}}})", glz::write_json(code).value())}},
                                         Comm::linux, net::Retry::never));
                if (gen != qr_gen_) {
                    co_return QrStatus{};
                }
                if (!session) {
                    co_return std::unexpected(session.error());
                }
                client.session = std::move(*session);
                ++client.session_gen;
                renewal_.reset();
                renew_after_ = Clock::now() + 30min;
                qr_cookies_ = {};
                if (client.on_session) {
                    client.on_session();
                }
                co_return QrStatus{.state = QrState::confirmed, .nickname = args[5]};
            });
        });
    }

    Task<Result<void>> Qq::renew(bool force) {
        if (!renewal_ && !force && Clock::now() < renew_after_) {
            co_return Result<void>{};
        }
        if (renewal_) {
            auto renewal = renewal_;
            struct Wait {
                Renewal &state;
                bool await_ready() const {
                    return false;
                }
                void await_suspend(std::coroutine_handle<> h) {
                    state.waiters.push_back(h);
                }
                void await_resume() const {}
            };
            co_await Wait{*renewal};
            co_return renewal->result;
        }
        auto renewal = renewal_ = std::make_shared<Renewal>();
        uint64_t gen = client.session_gen;
        auto result = co_await renew_once();
        if (gen == client.session_gen) {
            renew_after_ = Clock::now() + (result ? 30min : 30s);
        }
        renewal->result = result;
        if (renewal_ == renewal) {
            renewal_.reset();
        }
        for (auto waiter : renewal->waiters) {
            waiter.resume();
        }
        co_return result;
    }

    Task<Result<void>> Qq::renew_once() {
        if (!logged_in()) {
            co_return std::unexpected("QQ Music session expired; scan again");
        }
        auto reply =
            co_await client.call({{"QQConnectLogin.LoginServer", "QQLogin",
                                   std::format(R"({{"openid":{},"access_token":{},"refresh_token":{},"musicid":{},"musickey":{},"onlyNeedAccessToken":0,"forceRefreshToken":0}})",
                                               glz::write_json(client.get("openid")).value(), glz::write_json(client.get("access_token")).value(),
                                               glz::write_json(client.get("refresh_token")).value(), client.get("musicid"), glz::write_json(client.get("musickey")).value())}},
                                 Comm::linux, net::Retry::never);
        if (reply.needs_login) {
            co_await logout();
            co_return std::unexpected("QQ Music session expired; scan again");
        }
        co_return login_session(reply).transform([&](auto session) -> void {
            client.session = std::move(session);
            if (client.on_session) {
                client.on_session();
            }
            return;
        });
    }

    Task<Result<Account>> Qq::account() {
        std::string uid = client.get("musicid");
        co_return parse_data<QUsers>(co_await client.call({{"userInfo.BaseUserInfoServer", "get_user_baseinfo_v2", std::format(R"({{"vec_uin":["{}"]}})", uid)}}))
            .and_then([&](auto d) -> Result<Account> {
                if (d.code != 0 || !d.map_userinfo || !d.map_userinfo->contains(uid)) {
                    return std::unexpected("QQ Music returned no account details");
                }
                const auto &u = d.map_userinfo->at(uid);
                return Account{.id = uid, .name = u.nick.value_or(""), .avatar = u.headurl.value_or("")};
            });
    }

    Task<void> Qq::logout() {
        cancel_qr();
        ++client.session_gen;
        renewal_.reset();
        renew_after_ = {};
        client.session.clear();
        if (client.on_session) {
            client.on_session();
        }
        co_return;
    }

} // namespace lyren::qq
