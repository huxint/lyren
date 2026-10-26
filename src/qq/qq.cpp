#include "qq/qq.hpp"

#include <glaze/glaze.hpp>

#include <ctime>
#include <format>
#include <random>
#include <regex>
#include <curl/curl.h>

#include "core/json.hpp"
#include "core/log.hpp"
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
            double try_begin = 0;
        };
        struct QPay {
            int pay_play = 0;
        };
        struct QVolume {
            double gain = 0, peak = 0;
        };
        struct QSong {
            uint64_t id = 0;
            Str mid, name, title, subtitle;
            Vec<QSinger> singer;
            std::optional<QAlbumRef> album;
            int interval = 0;
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
            int sum = 0;
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
            int qrc = 0;
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
            Str Nick, Avatar, Content, SeqNo;
            int64_t PubTime = 0;
            int PraiseNum = 0;
            Vec<QReplied> RepliedComments;
        };
        struct QCommentList {
            Vec<QComment> Comments;
            int HasMore = 0, Total = 0;
        };
        struct QComments {
            std::optional<QCommentList> CommentList;
        };

        struct QSession {
            uint64_t musicid = 0;
            Str musickey, openid, access_token, refresh_token, unionid;
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
            Str name, pic;
            int songNum = 0;
        };
        struct QFavorites {
            Vec<QFavorite> v_list;
            int total = 0, hasmore = 0;
        };
        struct QAssetSongs {
            Vec<glz::generic_u64> list;
            int total = 0, hasmore = 0;
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

        constexpr glz::opts lax{.error_on_unknown_keys = false};

        template <class T>
        Result<T> parse(const Reply &r) {
            if (!r.error.empty()) {
                return std::unexpected(r.error);
            }
            T v{};
            if (auto e = glz::read<lax>(v, r.body)) {
                log::warn("qq: cannot parse reply ({} bytes)", r.body.size());
                return std::unexpected("QQ 音乐返回的数据无法解析");
            }
            return v;
        }

        // A part's data, or why it has none.
        template <class T>
        Result<T> data(const std::optional<Part<T>> &p) {
            if (!p) {
                return std::unexpected("QQ 音乐没有返回数据");
            }
            if (p->code == 2001) {
                return std::unexpected("QQ 音乐要求登录后再试");
            }
            if (p->code != 0) {
                return std::unexpected(std::format("QQ 音乐请求失败（{}）", p->code));
            }
            if (!p->data) {
                return std::unexpected("QQ 音乐没有返回数据");
            }
            return *p->data;
        }

        // Covers by mid: T001 is an artist's picture, T002 an album's.
        std::string photo(int kind, std::string_view mid) {
            return mid.empty() ? "" : std::format("https://y.gtimg.cn/music/photo_new/T00{}R300x300M000{}.jpg", kind, mid);
        }

        Track track(const QSong &s) {
            Track t{.source = Source::qq, .id = std::to_string(s.id), .mid = s.mid.value_or("")};
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
            if (songs) {
                for (const auto &s : *songs) {
                    out.push_back(track(s));
                }
            }
            return out;
        }

        std::vector<Track> tracks_of(const Vec<QSongInfo> &songs) {
            std::vector<Track> out;
            if (songs) {
                for (const auto &s : *songs) {
                    if (s.songInfo) {
                        out.push_back(track(*s.songInfo));
                    }
                }
            }
            return out;
        }

        Result<std::vector<Track>> asset_tracks(const Vec<glz::generic_u64> &songs) {
            std::vector<Track> out;
            if (!songs) {
                return out;
            }
            for (const auto &item : *songs) {
                const auto *song = &item;
                if (!item.is_object()) {
                    return std::unexpected("QQ 音乐返回了无法识别的歌曲资料");
                }
                const auto &object = item.get_object();
                if (auto it = object.find("songInfo"); it != object.end()) {
                    song = &it->second;
                } else if (auto it = object.find("track"); it != object.end()) {
                    song = &it->second;
                }
                std::string json;
                (void)glz::write_json(*song, json);
                QSong s;
                if (glz::read<lax>(s, json) || s.id == 0 || s.mid.value_or("").empty()) {
                    return std::unexpected("QQ 音乐返回了不完整的歌曲资料");
                }
                out.push_back(track(s));
            }
            return out;
        }

        Result<QWrite> written(const Reply &reply) {
            auto r = parse<One<QWrite>>(reply);
            if (!r) {
                return std::unexpected(r.error());
            }
            auto d = data(r->req_0);
            if (!d) {
                return std::unexpected(d.error());
            }
            int code = d->retCode != 0 ? d->retCode : d->result ? d->result->retCode : 0;
            if (code != 0) {
                return std::unexpected(std::format("QQ 音乐没有完成操作（{}）", code));
            }
            return *d;
        }

        Card album_card(std::string mid, std::string title, std::string_view date, int count) {
            std::string year(date.substr(0, 4));
            std::string sub = count > 0 ? std::format("{} 首", count) : "";
            if (!year.empty()) {
                sub = sub.empty() ? year : std::format("{} · {}", year, sub);
            }
            std::string cover = photo(2, mid);
            return {.kind = CardKind::album,
                    .source = Source::qq,
                    .id = std::move(mid),
                    .title = std::move(title),
                    .subtitle = std::move(sub),
                    .cover = std::move(cover),
                    .count = count};
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

        // Comment times as the apps put them: 14:05, 昨天 14:05, 3月2日, 2024年3月2日.
        std::string when(int64_t seconds) {
            std::time_t t = std::time_t(seconds), now = std::time(nullptr);
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
                return std::format("昨天 {:02}:{:02}", a.tm_hour, a.tm_min);
            }
            if (a.tm_year == b.tm_year) {
                return std::format("{}月{}日", a.tm_mon + 1, a.tm_mday);
            }
            return std::format("{}年{}月{}日", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
        }

        // The text of the next <tag><![CDATA[…]]></tag> in s after at, moving at past it; at becomes npos when there is none.
        std::string cdata(std::string_view s, std::string_view tag, size_t &at) {
            std::string open = std::format("<{}><![CDATA[", tag);
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

        Result<std::map<std::string, std::string>> login_session(const Reply &reply) {
            auto r = parse<One<QSession>>(reply);
            if (!r) {
                return std::unexpected(r.error());
            }
            auto s = data(r->req_0);
            if (!s) {
                return std::unexpected(s.error());
            }
            if (!s->musicid || s->musickey.value_or("").empty() || s->openid.value_or("").empty() || s->refresh_token.value_or("").empty()) {
                return std::unexpected("QQ 音乐没有返回完整的登录凭据");
            }
            return std::map<std::string, std::string>{{"musicid", std::to_string(s->musicid)},        {"musickey", *s->musickey},           {"openid", *s->openid},
                                                      {"access_token", s->access_token.value_or("")}, {"refresh_token", *s->refresh_token}, {"unionid", s->unionid.value_or("")},
                                                      {"expired_at", std::to_string(s->expired_at)}};
        }

    } // namespace

    Qq::Qq(Loop &loop, net::Http &http)
    : client(loop, http),
      loop_(loop),
      http_(http) {
        std::mt19937_64 rng{std::random_device{}()};
        guid_ = std::to_string(1000000000 + rng() % 9000000000);
    }

    Task<Result<Batch>> Qq::search(std::string query, SearchKind kind, int offset, int limit) {
        static constexpr int types[] = {0, 2, 1, 3}; // songs, albums, artists, playlists
        std::mt19937_64 rng{std::random_device{}()};
        std::string param = std::format(R"({{"query":{},"search_type":{},"page_num":{},"num_per_page":{},"searchid":"{}"}})", json_string(query), types[int(kind)],
                                        offset / limit + 1, limit, rng() % 100000000000000);
        auto r = parse<One<QSearch>>(co_await client.call({{"music.search.SearchCgiService", "DoSearchForQQMusicDesktop", param}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        Batch out{.total = d->meta ? d->meta->sum : 0};
        if (!d->body) {
            co_return out;
        }
        const QSearchBody &b = *d->body;
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
                        c.subtitle = year.empty() ? a.singerName.value_or("") : std::format("{} · {}", a.singerName.value_or(""), year);
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
                                             .subtitle = a.songNum > 0 ? std::format("单曲 {}", a.songNum) : "",
                                             .cover = a.singerPic.value_or(photo(1, a.singerMID.value_or("")))});
                    }
                }
                break;
            case SearchKind::playlists:
                if (b.songlist && b.songlist->list) {
                    for (const auto &p : *b.songlist->list) {
                        std::string sub = std::format("{} 首", p.song_count);
                        if (p.creator && p.creator->name) {
                            sub += " · " + *p.creator->name;
                        }
                        out.cards.push_back({.kind = CardKind::playlist,
                                             .source = Source::qq,
                                             .id = p.dissid.value_or(""),
                                             .title = p.dissname.value_or(""),
                                             .subtitle = std::move(sub),
                                             .cover = p.imgurl.value_or(""),
                                             .count = p.song_count});
                    }
                }
                break;
        }
        co_return out;
    }

    Task<Result<Stream>> Qq::stream(Track t, Quality quality) {
        // Down from the quality asked for; the preview clip comes last, for songs that need a membership.
        struct File {
            const char *prefix, *ext, *level;
        };
        static constexpr File files[] = {
            {"F000", "flac", "lossless"}, {"M800", "mp3", "exhigh"}, {"M500", "mp3", "standard"}, {"C400", "m4a", "standard"}, {"RS02", "mp3", "standard"}};
        size_t first = quality >= Quality::lossless ? 0 : quality == Quality::exhigh ? 1 : 2;
        std::string mids, types, names;
        for (size_t i = first; i < std::size(files); ++i) {
            std::string_view sep = i == first ? "" : ",";
            mids += std::format("{}\"{}\"", sep, t.mid);
            types += std::format("{}0", sep);
            names += std::format("{}\"{}{}.{}\"", sep, files[i].prefix, t.media, files[i].ext);
        }
        std::string uin = client.logged_in() ? client.get("musicid") : "0";
        std::string vkey = std::format(R"({{"uin":"{}","guid":"{}","songmid":[{}],"songtype":[{}],"filename":[{}],"ctx":0}})", uin, guid_, mids, types, names);
        // The details carry the loudness and where the preview clip starts.
        std::string detail = std::format(R"({{"song_mid":"{}"}})", t.mid);
        auto r = parse<Two<QVkey, QDetail>>(co_await client.call({{"music.vkey.GetVkey", "UrlGetVkey", vkey}, {"music.pf_song_detail_svr", "get_song_detail", detail}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto v = data(r->req_0);
        if (!v) {
            co_return std::unexpected(v.error());
        }
        if (!v->sip || v->sip->empty() || !v->midurlinfo) {
            co_return std::unexpected("这首歌暂时不能播放（需要会员或没有版权）");
        }
        for (size_t i = first; i < std::size(files); ++i) {
            std::string name = std::format("{}{}.{}", files[i].prefix, t.media, files[i].ext);
            auto u = std::ranges::find_if(*v->midurlinfo, [&](const QUrl &x) {
                return x.filename == name && x.result == 0 && x.purl && !x.purl->empty();
            });
            if (u == v->midurlinfo->end()) {
                continue;
            }
            std::string host = v->sip->front();
            if (host.starts_with("http://")) {
                host.insert(4, "s");
            }
            Stream s{.url = host + *u->purl, .trial = std::string_view(files[i].prefix) == "RS02", .level = files[i].level};
            auto d = data(r->req_1);
            if (d && d->track_info) {
                const QSong &song = *d->track_info;
                if (song.file) {
                    s.trial_start = s.trial ? song.file->try_begin / 1000 : 0;
                }
                // A clip's loudness is not the song's.
                if (song.volume && !s.trial) {
                    s.gain = float(song.volume->gain), s.peak = float(song.volume->peak);
                }
            }
            co_return s;
        }
        co_return std::unexpected("这首歌暂时不能播放（需要会员或没有版权）");
    }

    Task<Result<Track>> Qq::song_detail(std::string mid) {
        auto r = parse<One<QDetail>>(co_await client.call({{"music.pf_song_detail_svr", "get_song_detail", std::format(R"({{"song_mid":{}}})", json_string(mid))}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        if (!d->track_info) {
            co_return std::unexpected("QQ 音乐没有返回这首歌的资料");
        }
        co_return track(*d->track_info);
    }

    Task<Result<std::vector<LyricLine>>> Qq::lyrics(Track t) {
        std::string param = std::format(R"({{"songId":{},"type":0,"crypt":1,"qrc":1,"trans":1,"roma":0,"lrc_t":0,"qrc_t":0,"trans_t":0,"roma_t":0}})", t.id);
        auto r = parse<One<QLyric>>(co_await client.call({{"music.musichallSong.PlayLyricInfo", "GetPlayLyricInfo", param}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::string text = qrc_decode(d->lyric.value_or("")), trans = qrc_decode(d->trans.value_or(""));
        // Songs without word timing send LRC in the same wrapping.
        if (d->qrc == 1 && text.starts_with("<?xml")) {
            co_return parse_qrc(text, trans);
        }
        co_return parse_lyrics(text, trans);
    }

    Task<Result<std::vector<Track>>> Qq::tracks(Card card) {
        if (card.id.starts_with("dir:")) {
            std::vector<Track> out;
            for (int offset = 0;;) {
                auto r = parse<One<QAssetSongs>>(co_await client.call(
                    {{"music.musicasset.PlaylistDetailRead", "GetUniformSongDetailInfo",
                      std::format(R"({{"uin":{},"dirid":{},"bPaged":true,"offset":{},"size":100}})", json_string(client.get("musicid")), card.id.substr(4), offset)}}));
                if (!r) {
                    co_return std::unexpected(r.error());
                }
                auto d = data(r->req_0);
                if (!d) {
                    co_return std::unexpected(d.error());
                }
                auto part = asset_tracks(d->list);
                if (!part) {
                    co_return std::unexpected(part.error());
                }
                bool end = part->empty() || !d->hasmore;
                offset += int(part->size());
                out.insert(out.end(), std::make_move_iterator(part->begin()), std::make_move_iterator(part->end()));
                if (end) {
                    break;
                }
            }
            co_return out;
        }
        if (card.kind == CardKind::album) {
            auto a = co_await album(card.id);
            if (!a) {
                co_return std::unexpected(a.error());
            }
            co_return std::move(a->tracks);
        }
        if (card.kind == CardKind::artist) {
            auto b = co_await artist_songs(card.id, 0);
            if (!b) {
                co_return std::unexpected(b.error());
            }
            co_return std::move(b->tracks);
        }
        if (card.id.starts_with(top_prefix)) {
            auto r = parse<One<QTopDetail>>(
                co_await client.call({{"music.musicToplist.Toplist", "GetDetail", std::format(R"({{"topId":{},"num":300}})", card.id.substr(top_prefix.size()))}}));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            auto d = data(r->req_0);
            if (!d) {
                co_return std::unexpected(d.error());
            }
            co_return tracks_of(d->songInfoList);
        }
        std::vector<Track> out;
        for (int begin = 0;;) {
            auto r = parse<One<QDiss>>(co_await client.call({{"music.srfDissInfo.aiDissInfo", "uniform_get_Dissinfo",
                                                              std::format(R"({{"disstid":{},"userinfo":1,"tag":1,"song_begin":{},"song_num":1000}})", card.id, begin)}}));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            auto d = data(r->req_0);
            if (!d) {
                co_return std::unexpected(d.error());
            }
            auto part = tracks_of(d->songlist);
            if (part.empty()) {
                break;
            }
            begin += int(part.size());
            out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
            if (begin >= d->total_song_num) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Qq::recommended_playlists() {
        auto r = parse<One<QRecommend>>(co_await client.call({{"playlist.HotRecommendServer", "get_new_hot_recommend", R"({"cmd":0,"page":0,"daily_page":0,"size":30})"}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::vector<Card> out;
        if (d->modules) {
            for (const auto &m : *d->modules) {
                if (m.grids) {
                    for (const auto &g : *m.grids) {
                        out.push_back({.kind = CardKind::playlist,
                                       .source = Source::qq,
                                       .id = std::to_string(g.id),
                                       .title = g.title.value_or(""),
                                       .subtitle = g.creator ? g.creator->nick.value_or("") : "",
                                       .cover = g.picurl.value_or("")});
                    }
                }
            }
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Qq::toplists() {
        auto r = parse<One<QToplists>>(co_await client.call({{"music.musicToplist.Toplist", "GetAll", "{}"}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::vector<Card> out;
        if (d->group) {
            for (const auto &g : *d->group) {
                if (g.toplist) {
                    for (const auto &t : *g.toplist) {
                        out.push_back({.kind = CardKind::playlist,
                                       .source = Source::qq,
                                       .id = std::format("{}{}", top_prefix, t.topId),
                                       .title = t.title.value_or(""),
                                       .subtitle = t.updateTips.value_or(""),
                                       .cover = t.frontPicUrl.value_or("")});
                    }
                }
            }
        }
        co_return out;
    }

    Task<Result<AlbumInfo>> Qq::album(std::string mid) {
        auto r = parse<Two<QAlbumDetail, QSongList>>(
            co_await client.call({{"music.musichallAlbum.AlbumInfoServer", "GetAlbumDetail", std::format(R"({{"albumMid":"{}"}})", mid)},
                                  {"music.musichallAlbum.AlbumSongList", "GetAlbumSongList", std::format(R"({{"albumMid":"{}","begin":0,"num":500,"order":2}})", mid)}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto a = data(r->req_0);
        auto s = data(r->req_1);
        if (!a) {
            co_return std::unexpected(a.error());
        }
        if (!s) {
            co_return std::unexpected(s.error());
        }
        AlbumInfo out{.tracks = tracks_of(s->songList)};
        if (a->singer && a->singer->singerList) {
            for (const auto &x : *a->singer->singerList) {
                out.artist += (out.artist.empty() ? "" : " / ") + x.name.value_or("");
            }
            if (!a->singer->singerList->empty()) {
                out.artist_id = a->singer->singerList->front().mid.value_or("");
            }
        }
        if (a->basicInfo) {
            out.date = a->basicInfo->publishDate.value_or("");
            out.about = trimmed(a->basicInfo->desc);
            out.thread = std::to_string(a->basicInfo->albumID);
        }
        if (a->company) {
            out.company = a->company->name.value_or("");
        }
        if (client.logged_in()) {
            auto favorites = co_await collected_albums();
            if (!favorites) {
                co_return std::unexpected(favorites.error());
            }
            out.collected = std::ranges::any_of(*favorites, [&](const Card &c) {
                return c.id == mid;
            });
        }
        co_return out;
    }

    Task<Result<ArtistInfo>> Qq::artist(std::string mid) {
        // The details have no counts; the first entry of each list brings its total.
        auto r = parse<Three<QSingerDetail, QSongList, QAlbumList>>(co_await client.call(
            {{"music.musichallSinger.SingerInfoInter", "GetSingerDetail", std::format(R"({{"singer_mids":["{}"],"pic":1,"group_singer":0,"wiki_singer":0,"ex_singer":1}})", mid)},
             {"music.musichallSong.SongListInter", "GetSingerSongList", std::format(R"({{"singerMid":"{}","begin":0,"num":1,"order":1}})", mid)},
             {"music.musichallAlbum.AlbumListServer", "GetAlbumList", std::format(R"({{"singerMid":"{}","order":0,"begin":0,"num":1}})", mid)}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        if (!d->singer_list || d->singer_list->empty()) {
            co_return std::unexpected("没有找到这位歌手");
        }
        const QSingerEntry &e = d->singer_list->front();
        ArtistInfo out{.name = e.basic_info ? e.basic_info->name.value_or("") : ""};
        if (e.ex_info) {
            out.alias = e.ex_info->foreign_name.value_or("");
        }
        out.avatar = e.pic && e.pic->pic ? *e.pic->pic : photo(1, mid);
        if (auto s = data(r->req_1)) {
            out.songs = s->totalNum;
        }
        if (auto a = data(r->req_2)) {
            out.albums = a->total;
        }
        if (client.logged_in()) {
            auto followed = co_await followed_artists();
            if (!followed) {
                co_return std::unexpected(followed.error());
            }
            out.followed = std::ranges::any_of(*followed, [&](const Card &c) {
                return c.id == mid;
            });
        }
        co_return out;
    }

    Task<Result<Batch>> Qq::artist_songs(std::string mid, int offset) {
        auto r = parse<One<QSongList>>(
            co_await client.call({{"music.musichallSong.SongListInter", "GetSingerSongList", std::format(R"({{"singerMid":"{}","begin":{},"num":100,"order":1}})", mid, offset)}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        co_return Batch{.tracks = tracks_of(d->songList), .total = d->totalNum};
    }

    Task<Result<Batch>> Qq::artist_albums(std::string mid, int offset) {
        auto r = parse<One<QAlbumList>>(
            co_await client.call({{"music.musichallAlbum.AlbumListServer", "GetAlbumList", std::format(R"({{"singerMid":"{}","order":0,"begin":{},"num":30}})", mid, offset)}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        Batch out{.total = d->total};
        if (d->albumList) {
            for (const auto &a : *d->albumList) {
                out.cards.push_back(album_card(a.albumMid.value_or(""), a.albumName.value_or(""), a.publishDate.value_or(""), a.totalNum));
            }
        }
        co_return out;
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Qq::artist_intro(std::string mid) {
        auto r = parse<One<QSingerDetail>>(co_await client.call({{"music.musichallSinger.SingerInfoInter", "GetSingerDetail",
                                                                  std::format(R"({{"singer_mids":["{}"],"pic":0,"group_singer":0,"wiki_singer":1,"ex_singer":1}})", mid)}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::vector<std::pair<std::string, std::string>> out;
        if (!d->singer_list || d->singer_list->empty()) {
            co_return out;
        }
        const QSingerEntry &e = d->singer_list->front();
        // The wiki: <desc> is the summary, <basic> short facts (外文名, 生日…), <other> headed sections; all in CDATA.
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
            size_t b = wiki.find(std::format("<{}>", block)), end = wiki.find(std::format("</{}>", block));
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
                    facts += std::format("{}{}：{}", facts.empty() ? "" : "\n", key, value);
                } else if (!value.empty()) {
                    out.emplace_back(std::move(key), std::move(value));
                }
            }
            if (!facts.empty()) {
                out.emplace_back("基本资料", std::move(facts));
            }
        }
        co_return out;
    }

    Task<Result<CommentPage>> Qq::comments(Subject subject, std::string id, bool hot, int page, std::string cursor) {
        int biz = subject == Subject::song ? 1 : subject == Subject::album ? 2 : 3;
        if (id.starts_with(top_prefix)) {
            biz = 4, id = id.substr(top_prefix.size());
        }
        std::string common = std::format(R"("BizType":{},"BizId":"{}","LastCommentSeqNo":{},"PageSize":20,"PageNum":{})", biz, id, json_string(cursor), page - 1);
        Rpc rpc{"music.globalComment.CommentReadServer", hot ? "GetHotCommentList" : "GetNewCommentList",
                hot ? std::format(R"({{{},"HotType":1,"WithAirborne":0}})", common) : std::format(R"({{{},"FromCommentId":"","WithHot":0}})", common)};
        auto r = parse<One<QComments>>(co_await client.call({rpc}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        CommentPage out;
        if (!d->CommentList) {
            co_return out;
        }
        const QCommentList &l = *d->CommentList;
        out.total = l.Total;
        out.more = l.HasMore != 0;
        if (l.Comments) {
            for (const auto &c : *l.Comments) {
                Comment &m = out.comments.emplace_back();
                m.user = c.Nick.value_or("");
                m.avatar = c.Avatar.value_or("");
                m.text = trimmed(c.Content);
                m.time = when(c.PubTime);
                m.likes = c.PraiseNum;
                if (c.RepliedComments && !c.RepliedComments->empty()) {
                    m.reply_user = c.RepliedComments->front().Nick.value_or("");
                    m.reply = trimmed(c.RepliedComments->front().Content);
                }
            }
            if (!l.Comments->empty()) {
                out.cursor = l.Comments->back().SeqNo.value_or("");
            }
        }
        co_return out;
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Qq::user_playlists(std::string uid) {
        auto r = parse<One<QDirectories>>(co_await client.call({{"music.musicasset.PlaylistBaseRead", "GetPlaylistByUin", std::format(R"({{"uin":{}}})", json_string(uid))}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::pair<Card, std::vector<Card>> out{{.source = Source::qq, .id = "dir:201", .title = "我喜欢的音乐", .owned = true}, {}};
        if (d->v_playlist) {
            for (const auto &p : *d->v_playlist) {
                if (p.dirId == 201) {
                    continue;
                }
                if (!p.dirId || p.dirName.value_or("").empty()) {
                    co_return std::unexpected("QQ 音乐返回了无法识别的歌单目录");
                }
                out.second.push_back({.source = Source::qq,
                                      .id = "dir:" + std::to_string(p.dirId),
                                      .title = *p.dirName,
                                      .subtitle = std::format("{} 首", p.songNum),
                                      .cover = p.dirPicUrl.value_or(""),
                                      .thread = p.tid ? std::to_string(p.tid) : "",
                                      .count = p.songNum,
                                      .owned = true});
            }
        }
        for (int offset = 0;;) {
            auto f = parse<One<QFavorites>>(co_await client.call(
                {{"music.musicasset.PlaylistFavRead", "GetPlaylistFavInfo_v2", std::format(R"({{"uin":{},"offset":{},"size":100}})", json_string(uid), offset)}}));
            if (!f) {
                co_return std::unexpected(f.error());
            }
            auto page = data(f->req_0);
            if (!page) {
                co_return std::unexpected(page.error());
            }
            if (!page->v_list || page->v_list->empty()) {
                break;
            }
            for (const auto &p : *page->v_list) {
                if (!p.tid) {
                    co_return std::unexpected("QQ 音乐返回了无法识别的收藏歌单");
                }
                out.second.push_back({.source = Source::qq,
                                      .id = std::to_string(p.tid),
                                      .title = p.name.value_or(""),
                                      .subtitle = std::format("{} 首", p.songNum),
                                      .cover = p.pic.value_or(""),
                                      .count = p.songNum});
            }
            offset += int(page->v_list->size());
            if (!page->hasmore || offset >= page->total) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<std::vector<std::string>>> Qq::liked_ids(std::string) {
        auto songs = co_await tracks({.source = Source::qq, .id = "dir:201"});
        if (!songs) {
            co_return std::unexpected(songs.error());
        }
        std::vector<std::string> out;
        for (const auto &t : *songs) {
            out.push_back(t.id);
        }
        co_return out;
    }

    Task<Result<void>> Qq::like(Track track, bool on) {
        co_return co_await edit_playlist("dir:201", std::move(track), on);
    }

    Task<Result<void>> Qq::edit_playlist(std::string id, Track t, bool add) {
        auto r = written(co_await client.call({{"music.musicasset.PlaylistDetailWrite", add ? "AddSonglist" : "DelSonglist",
                                                std::format(R"({{"dirId":{},"v_songInfo":[{{"songId":{},"songType":{}}}]}})", id.substr(4), t.id, t.type)}},
                                              Comm::app, false));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Result<void>{};
    }

    Task<Result<Card>> Qq::create_playlist(std::string name, bool hidden) {
        auto r = written(co_await client.call({{"music.musicasset.PlaylistBaseWrite", "AddPlaylist",
                                                std::format(R"({{"dirName":{},"dirShow":{},"dirDesc":"","dirPicUrl":"","taglist":""}})", json_string(name), hidden ? 0 : 1)}},
                                              Comm::app, false));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        if (!r->result || !r->result->dirId) {
            co_return std::unexpected("QQ 音乐没有返回新歌单的目录 ID");
        }
        co_return Card{.source = Source::qq, .id = "dir:" + std::to_string(r->result->dirId), .title = std::move(name), .subtitle = "0 首", .owned = true};
    }

    Task<Result<void>> Qq::rename_playlist(std::string id, std::string name) {
        // EditPlaylist replaces all four fields; read and retain the cover, description and tags first.
        auto r = parse<One<QDiss>>(co_await client.call(
            {{"music.srfDissInfo.aiDissInfo", "uniform_get_Dissinfo", std::format(R"({{"dirid":{},"optype":3,"tag":1,"userinfo":0,"pic_dpi":300}})", id.substr(4))}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        if (!d->dirinfo) {
            co_return std::unexpected("QQ 音乐没有返回歌单资料");
        }
        std::string tags;
        if (d->dirinfo->vec_tagid) {
            for (int tag : *d->dirinfo->vec_tagid) {
                tags += (tags.empty() ? "" : ",") + std::to_string(tag);
            }
        }
        auto w = written(
            co_await client.call({{"music.musicasset.PlaylistBaseWrite", "EditPlaylist",
                                   std::format(R"({{"dirId":{},"mask":15,"dirNewName":{},"dirNewDesc":{},"dirNewPicUrl":{},"dirNewtaglist":{}}})", id.substr(4), json_string(name),
                                               json_string(d->dirinfo->desc.value_or("")), json_string(d->dirinfo->picurl.value_or("")), json_string(tags))}},
                                 Comm::app, false));
        if (!w) {
            co_return std::unexpected(w.error());
        }
        co_return Result<void>{};
    }

    Task<Result<void>> Qq::delete_playlist(std::string id) {
        auto r = written(co_await client.call({{"music.musicasset.PlaylistBaseWrite", "DelPlaylist", std::format(R"({{"dirId":{}}})", id.substr(4))}}, Comm::app, false));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Result<void>{};
    }

    Task<Result<void>> Qq::collect(std::string mid, bool on) {
        auto r = written(co_await client.call({{"music.musicasset.AlbumFavWrite", on ? "FavAlbum" : "CancelFavAlbum",
                                                std::format(R"({{"uin":{},"v_albumMid":[{}]}})", json_string(client.get("musicid")), json_string(mid))}},
                                              Comm::app, false));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Result<void>{};
    }

    Task<Result<std::vector<Card>>> Qq::collected_albums() {
        auto r = parse<One<QAlbumFavorites>>(
            co_await client.call({{"music.musicasset.AlbumFavRead", "GetAlbumFavInfo", std::format(R"({{"uin":{}}})", json_string(client.get("musicid")))}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        std::vector<Card> out;
        if (d->v_list) {
            for (const auto &a : *d->v_list) {
                if (a.albumMid.value_or("").empty()) {
                    co_return std::unexpected("QQ 音乐返回了无法识别的收藏专辑");
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
        co_return out;
    }

    Task<Result<void>> Qq::follow(std::string mid, bool on) {
        auto r = written(
            co_await client.call({{"Concern.ConcernSystemServer", "cgi_concern_user_v2",
                                   std::format(R"({{"opertype":{},"source":0,"encrypt_singerid":1,"userinfo":{{"usertype":1,"userid":{}}}}})", on ? 0 : 1, json_string(mid))}},
                                 Comm::app, false));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        co_return Result<void>{};
    }

    Task<Result<std::vector<Card>>> Qq::followed_artists() {
        std::vector<Card> out;
        for (int from = 0;;) {
            auto r = parse<One<QFollowed>>(co_await client.call({{"music.concern.RelationList", "GetFollowSingerList", std::format(R"({{"From":{},"Size":100}})", from)}}));
            if (!r) {
                co_return std::unexpected(r.error());
            }
            auto d = data(r->req_0);
            if (!d) {
                co_return std::unexpected(d.error());
            }
            if (!d->List || d->List->empty()) {
                break;
            }
            for (const auto &a : *d->List) {
                if (a.SingerMid.value_or("").empty()) {
                    co_return std::unexpected("QQ 音乐返回了无法识别的关注歌手");
                }
                out.push_back({.kind = CardKind::artist,
                               .source = Source::qq,
                               .id = *a.SingerMid,
                               .title = a.SingerName.value_or(""),
                               .cover = a.SingerPic.value_or(photo(1, *a.SingerMid))});
            }
            from += int(d->List->size());
            if (from >= d->Total) {
                break;
            }
        }
        co_return out;
    }

    Task<Result<QrCode>> Qq::qr_start() {
        uint64_t gen = ++qr_gen_;
        std::map<std::string, std::string> cookies;
        auto page = co_await net::fetch{
            loop_,
            http_,
            {.url = "https://xui.ptlogin2.qq.com/cgi-bin/xlogin?" +
                    form({{"appid", "716027609"}, {"daid", "383"}, {"pt_3rd_aid", "100497308"}, {"style", "40"}, {"s_url", "https://graph.qq.com/oauth2.0/login_jump"}})}};
        if (gen != qr_gen_) {
            co_return std::unexpected("已取消扫码登录");
        }
        if (!page.error.empty() || page.status != 200) {
            co_return std::unexpected("无法连接 QQ 登录服务");
        }
        receive_cookies(cookies, page.cookies);
        std::smatch version;
        if (!std::regex_search(page.body, version, std::regex(R"re(ptui_version\s*:\s*(?:encodeURIComponent\()?['"]([0-9]+)['"])re"))) {
            co_return std::unexpected("QQ 登录页没有返回客户端版本");
        }
        std::string js_version = version[1];
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
                                       .headers = {"Referer: https://xui.ptlogin2.qq.com/", cookie_header(cookies)}}};
        if (gen != qr_gen_) {
            co_return std::unexpected("已取消扫码登录");
        }
        if (!qr.error.empty() || qr.status != 200 || !qr.body.starts_with("\x89PNG\r\n\x1a\n")) {
            co_return std::unexpected("QQ 登录服务没有返回二维码");
        }
        receive_cookies(cookies, qr.cookies);
        if (cookies["qrsig"].empty()) {
            co_return std::unexpected("QQ 登录服务没有返回二维码令牌");
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
        auto poll = co_await net::fetch{loop_,
                                        http_,
                                        {.url = "https://ssl.ptlogin2.qq.com/ptqrlogin?" + form({{"u1", "https://graph.qq.com/oauth2.0/login_jump"},
                                                                                                 {"ptqrtoken", std::to_string(hash33(cookies["qrsig"]))},
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
                                                                                                 {"login_sig", cookies["pt_login_sig"]}}),
                                         .headers = {"Referer: https://xui.ptlogin2.qq.com/", cookie_header(cookies)}}};
        if (gen != qr_gen_) {
            co_return QrStatus{};
        }
        if (!poll.error.empty() || poll.status != 200) {
            co_return std::unexpected("无法查询 QQ 二维码状态");
        }
        auto args = qr_callback(poll.body);
        if (!args) {
            co_return std::unexpected(args.error());
        }
        if ((*args)[0] == "66") {
            co_return QrStatus{};
        }
        if ((*args)[0] == "67") {
            co_return QrStatus{.state = QrState::scanned, .nickname = (*args)[5]};
        }
        if ((*args)[0] == "65") {
            co_return QrStatus{.state = QrState::expired};
        }
        if ((*args)[0] != "0") {
            co_return std::unexpected(std::format("QQ 扫码登录失败（{}）", (*args)[0]));
        }
        std::string confirm = (*args)[2];
        if (!confirm.starts_with("https://ssl.ptlogin2.graph.qq.com/check_sig?")) {
            co_return std::unexpected("QQ 登录返回了无法识别的确认地址");
        }
        receive_cookies(cookies, poll.cookies);
        auto checked = co_await net::fetch{loop_, http_, {.url = confirm, .headers = {cookie_header(cookies)}}};
        if (gen != qr_gen_) {
            co_return QrStatus{};
        }
        if (!checked.error.empty() || checked.status >= 400) {
            co_return std::unexpected("QQ 登录确认失败，请重新扫码");
        }
        receive_cookies(cookies, checked.cookies);
        if (cookies["p_skey"].empty()) {
            co_return std::unexpected("QQ 登录没有返回授权凭据，请重新扫码");
        }
        auto authorized = co_await net::fetch{loop_,
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
                                                             {"g_tk", std::to_string(hash33(cookies["p_skey"], 5381))},
                                                             {"auth_time", millis()},
                                                             {"ui", random_id()}}),
                                               .headers = {"Content-Type: application/x-www-form-urlencoded", "Referer: https://graph.qq.com/", cookie_header(cookies)},
                                               .follow_redirects = false}};
        if (gen != qr_gen_) {
            co_return QrStatus{};
        }
        if (!authorized.error.empty() || authorized.status < 300 || authorized.status >= 400) {
            co_return std::unexpected("QQ 授权没有完成，请重新扫码");
        }
        std::string code = query_value(authorized.location, "code");
        if (code.empty() || query_value(authorized.location, "state") != state) {
            co_return std::unexpected("QQ 授权返回的数据不匹配，请重新扫码");
        }
        auto session = login_session(co_await client.call(
            {{"QQConnectLogin.LoginServer", "QQLogin", std::format(R"({{"onlyNeedAccessToken":0,"forceRefreshToken":0,"appid":100497308,"code":{}}})", json_string(code))}},
            Comm::linux, false));
        if (gen != qr_gen_) {
            co_return QrStatus{};
        }
        if (!session) {
            co_return std::unexpected(session.error());
        }
        client.session = std::move(*session);
        qr_cookies_.clear();
        if (client.on_session) {
            client.on_session();
        }
        co_return QrStatus{.state = QrState::confirmed, .nickname = (*args)[5]};
    }

    Task<Result<void>> Qq::renew() {
        uint64_t gen = qr_gen_;
        auto reply =
            co_await client.call({{"QQConnectLogin.LoginServer", "QQLogin",
                                   std::format(R"({{"openid":{},"access_token":{},"refresh_token":{},"musicid":{},"musickey":{},"onlyNeedAccessToken":0,"forceRefreshToken":0}})",
                                               json_string(client.get("openid")), json_string(client.get("access_token")), json_string(client.get("refresh_token")),
                                               client.get("musicid"), json_string(client.get("musickey")))}},
                                 Comm::linux, false);
        if (gen != qr_gen_) {
            co_return Result<void>{};
        }
        if (!reply.error.empty()) {
            co_return std::unexpected(reply.error);
        }
        auto session = login_session(reply);
        if (!session) {
            client.session.clear();
            if (client.on_session) {
                client.on_session();
            }
            co_return std::unexpected("QQ 音乐登录已失效，请重新扫码");
        }
        client.session = std::move(*session);
        if (client.on_session) {
            client.on_session();
        }
        co_return Result<void>{};
    }

    Task<Result<Account>> Qq::account() {
        std::string uid = client.get("musicid");
        auto r = parse<One<QUsers>>(co_await client.call({{"userInfo.BaseUserInfoServer", "get_user_baseinfo_v2", std::format(R"({{"vec_uin":[{}]}})", json_string(uid))}}));
        if (!r) {
            co_return std::unexpected(r.error());
        }
        auto d = data(r->req_0);
        if (!d) {
            co_return std::unexpected(d.error());
        }
        if (d->code != 0 || !d->map_userinfo || !d->map_userinfo->contains(uid)) {
            co_return std::unexpected("QQ 音乐没有返回账号资料");
        }
        const auto &u = d->map_userinfo->at(uid);
        co_return Account{.id = uid, .name = u.nick.value_or(""), .avatar = u.headurl.value_or("")};
    }

    Task<void> Qq::logout() {
        cancel_qr();
        client.session.clear();
        if (client.on_session) {
            client.on_session();
        }
        co_return;
    }

} // namespace lyren::qq
