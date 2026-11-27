#include "media/types.hpp"
#include "core/i18n.hpp"
#include "core/string_map.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace lyren {

    namespace {
        std::string image_url(std::string_view url, int px) {
            std::string result(url);
            if (result.starts_with("http://")) {
                result.insert(4, "s");
            }
            // The same image template is served by several CDN domains (kugou.com, kgimg.com, ...).
            const std::string_view size = px <= 150 ? "150" : px <= 240 ? "240" : "480";
            for (auto at = result.find("{size}"); at != result.npos; at = result.find("{size}", at + size.size())) {
                result.replace(at, 6, size);
            }
            for (auto at = result.find(' '); at != result.npos; at = result.find(' ', at + 3)) {
                result.replace(at, 1, "%20");
            }
            return result;
        }
    } // namespace

    std::optional<Source> parse_source(std::string_view id) {
        for (const auto &kind : source_kinds) {
            if (kind.id == id) {
                return kind.source;
            }
        }
        return std::nullopt;
    }

    std::optional<SourceKey> parse_source_key(std::string_view id) {
        const size_t colon = id.find(':');
        const std::string_view head = colon == id.npos ? id : id.substr(0, colon);
        const auto source = parse_source(head);
        if (!source) {
            return std::nullopt;
        }
        if (colon == id.npos) {
            return SourceKey{*source, {}};
        }
        const std::string_view inst = id.substr(colon + 1);
        if (inst.empty()) {
            return std::nullopt;
        }
        return SourceKey{*source, std::string(inst)};
    }

    std::string share_url(const Track &track) {
        switch (track.source) {
            case Source::netease:
                return "https://music.163.com/song?id=" + track.id;
            case Source::qq:
                return "https://i.y.qq.com/v8/playsong.html?songid=" + track.id;
            case Source::qishui:
                return "https://www.qishui.com/track/" + track.id;
            case Source::bodian:
                return "https://h5app.kuwo.cn/m/bodian/playMusic.html?uid=-1&musicId=" + track.id + "&opusId=";
            case Source::kugou:
            case Source::local:
            case Source::subsonic:
                return {};
        }
        std::unreachable();
    }

    std::string share_url(const Card &card) {
        if (card.source == Source::local || card.source == Source::subsonic) {
            return {};
        }
        if (card.kind == CardKind::user) {
            return card.source == Source::netease ? "https://music.163.com/user/home?id=" + card.id : "";
        }
        if (card.source == Source::netease) {
            constexpr std::array paths = {"playlist", "album", "artist"};
            return std::format("https://music.163.com/{}?id={}", paths[static_cast<size_t>(card.kind)], card.id);
        }
        if (card.source == Source::qishui) {
            if (card.kind == CardKind::album) {
                return "https://www.qishui.com/share/album?album_id=" + card.id;
            }
            return card.kind == CardKind::playlist && !card.id.starts_with("top:") ? "https://www.qishui.com/playlist/" + card.id : "";
        }
        if (card.source == Source::kugou) {
            return card.kind == CardKind::playlist && !card.id.contains(':') ? "https://www.kugou.com/yy/special/single/" + card.id + ".html" : "";
        }
        if (card.source == Source::bodian) {
            constexpr std::string_view base = "https://h5app.kuwo.cn/m/bodian/";
            if (card.kind == CardKind::album) {
                return std::format("{}album.html?uid=-1&albumid={}", base, card.id);
            }
            if (card.kind == CardKind::artist) {
                return std::format("{}singer.html?uid=-1&singerId={}", base, card.id);
            }
            std::string_view id = card.id, source = "4";
            if (auto colon = id.find(':'); colon != id.npos) {
                source = id.substr(0, colon);
                id.remove_prefix(colon + 1);
                if (source != "4" && source != "5" && source != "13") {
                    return {};
                }
            }
            return std::format("{}collection.html?uid=-1&playlistId={}&source={}", base, id, source);
        }
        std::string_view id = card.id;
        std::string_view path;
        switch (card.kind) {
            case CardKind::playlist:
                path = "playlist";
                if (id.starts_with("top:")) {
                    path = "toplist";
                    id.remove_prefix(4);
                } else if (id.starts_with("dir:")) {
                    id = card.thread;
                }
                break;
            case CardKind::album:
                path = "albumDetail";
                break;
            case CardKind::artist:
                path = "singer";
                break;
            case CardKind::user:
                break;
        }
        return id.empty() || path.empty() ? "" : std::format("https://y.qq.com/n/ryqq/{}/{}", path, id);
    }

    std::vector<ArtistRef> track_artists(const Track &track) {
        if (!track.artists.empty()) {
            return track.artists;
        }
        std::vector<ArtistRef> out;
        std::string_view raw = trim(track.artist);
        size_t pos = 0;
        while (pos < raw.size()) {
            const size_t sep = raw.find(" / ", pos);
            const std::string_view part = trim(sep == raw.npos ? raw.substr(pos) : raw.substr(pos, sep - pos));
            if (!part.empty()) {
                out.push_back({out.empty() ? track.artist_id : std::string{}, std::string(part)});
            }
            if (sep == raw.npos) {
                break;
            }
            pos = sep + 3;
        }
        return out;
    }

    namespace {
        std::string ascii_lower_str(std::string_view s) {
            std::string out(s);
            for (char &ch : out) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return out;
        }

        std::string_view extract_url(std::string_view input) {
            size_t pos = input.find("https://");
            if (pos == input.npos) {
                pos = input.find("http://");
            }
            if (pos == input.npos) {
                return {};
            }
            size_t end = pos;
            while (end < input.size()) {
                const unsigned char c = static_cast<unsigned char>(input[end]);
                if (c <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')' || c == ']') {
                    break;
                }
                ++end;
            }
            std::string_view url = input.substr(pos, end - pos);
            while (!url.empty() && (url.back() == '.' || url.back() == ',' || url.back() == ';')) {
                url.remove_suffix(1);
            }
            return url;
        }

        std::string_view query_param(std::string_view query, std::string_view key) {
            size_t pos = 0;
            while (pos < query.size()) {
                const size_t amp = query.find('&', pos);
                const std::string_view pair = amp == query.npos ? query.substr(pos) : query.substr(pos, amp - pos);
                if (const size_t eq = pair.find('='); eq != pair.npos) {
                    const std::string_view k = pair.substr(0, eq);
                    if (k.size() == key.size() && std::ranges::equal(k, key, [](char a, char b) {
                            auto lo = [](char c) {
                                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                            };
                            return lo(a) == lo(b);
                        })) {
                        return pair.substr(eq + 1);
                    }
                }
                if (amp == query.npos) {
                    break;
                }
                pos = amp + 1;
            }
            return {};
        }

        bool valid_token(std::string_view s) {
            return !s.empty() && std::ranges::all_of(s, [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == ':';
            });
        }

        std::string_view strip_html(std::string_view seg) {
            if (seg.ends_with(".html")) {
                seg.remove_suffix(5);
            }
            return seg;
        }

        std::string_view segment_after(std::string_view path, std::string_view prefix) {
            const size_t at = path.find(prefix);
            if (at == path.npos) {
                return {};
            }
            std::string_view rest = path.substr(at + prefix.size());
            const size_t slash = rest.find('/');
            if (slash != rest.npos) {
                rest = rest.substr(0, slash);
            }
            return strip_html(rest);
        }
    } // namespace

    std::optional<ShareLink> parse_share_url(std::string_view input) {
        const std::string_view url = extract_url(input);
        const size_t scheme = url.find("://");
        if (scheme == url.npos) {
            return std::nullopt;
        }
        std::string_view rest = url.substr(scheme + 3);
        const size_t path_pos = rest.find_first_of("/?#");
        std::string host = ascii_lower_str(path_pos == rest.npos ? rest : rest.substr(0, path_pos));
        if (const size_t colon = host.find(':'); colon != host.npos) {
            host.resize(colon);
        }
        std::string_view tail = path_pos == rest.npos ? std::string_view{} : rest.substr(path_pos);

        // NetEase frequently puts the actual route inside a hash fragment: music.163.com/#/song?id=123
        std::string_view path, query, fragment;
        if (const size_t hash = tail.find('#'); hash != tail.npos) {
            const std::string_view before_hash = tail.substr(0, hash);
            const std::string_view frag = tail.substr(hash + 1);
            fragment = frag;
            if (frag.starts_with('/')) {
                const size_t q = frag.find('?');
                path = q == frag.npos ? frag : frag.substr(0, q);
                query = q == frag.npos ? std::string_view{} : frag.substr(q + 1);
            } else {
                const size_t q = before_hash.find('?');
                path = q == before_hash.npos ? before_hash : before_hash.substr(0, q);
                query = q == before_hash.npos ? std::string_view{} : before_hash.substr(q + 1);
            }
        } else {
            const size_t q = tail.find('?');
            path = q == tail.npos ? tail : tail.substr(0, q);
            query = q == tail.npos ? std::string_view{} : tail.substr(q + 1);
        }
        const std::string lpath = ascii_lower_str(path);

        using Kind = ShareLink::Kind;
        auto make = [](Source src, Kind kind, std::string_view id) -> std::optional<ShareLink> {
            if (!valid_token(id)) {
                return std::nullopt;
            }
            return ShareLink{src, kind, std::string(id)};
        };

        // NetEase Music
        if (host == "music.163.com" || host == "y.music.163.com") {
            const std::string_view id = query_param(query, "id");
            if (lpath == "/song" || lpath == "/m/song") {
                return make(Source::netease, Kind::song, id);
            }
            if (lpath == "/playlist" || lpath == "/m/playlist") {
                return make(Source::netease, Kind::playlist, id);
            }
            if (lpath == "/album" || lpath == "/m/album") {
                return make(Source::netease, Kind::album, id);
            }
            if (lpath == "/artist" || lpath == "/m/artist") {
                return make(Source::netease, Kind::artist, id);
            }
            if (lpath == "/user/home" || lpath == "/m/user/home") {
                return make(Source::netease, Kind::user, id);
            }
            return std::nullopt;
        }

        // QQ Music
        if (host == "y.qq.com" || host == "i.y.qq.com" || host == "c.y.qq.com" || host == "c6.y.qq.com") {
            if (lpath.contains("playsong")) {
                if (auto mid = query_param(query, "songmid"); valid_token(mid)) {
                    return ShareLink{Source::qq, Kind::song, std::string(mid)};
                }
                return make(Source::qq, Kind::song, query_param(query, "songid"));
            }
            if (auto seg = segment_after(path, "/songDetail/"); !seg.empty()) {
                return make(Source::qq, Kind::song, seg);
            }
            if (auto seg = segment_after(path, "/songdetail/"); !seg.empty()) {
                return make(Source::qq, Kind::song, seg);
            }
            if (auto seg = segment_after(path, "/playlist/"); !seg.empty()) {
                return make(Source::qq, Kind::playlist, seg);
            }
            if (auto seg = segment_after(path, "/toplist/"); valid_token(seg)) {
                return ShareLink{Source::qq, Kind::playlist, "top:" + std::string(seg)};
            }
            if (auto seg = segment_after(path, "/albumDetail/"); !seg.empty()) {
                return make(Source::qq, Kind::album, seg);
            }
            if (auto seg = segment_after(path, "/albumdetail/"); !seg.empty()) {
                return make(Source::qq, Kind::album, seg);
            }
            if (auto seg = segment_after(path, "/singer/"); !seg.empty()) {
                return make(Source::qq, Kind::artist, seg);
            }
            if (lpath.contains("taoge")) {
                return make(Source::qq, Kind::playlist, query_param(query, "id"));
            }
            if (lpath.contains("/details/album")) {
                if (auto mid = query_param(query, "albummid"); valid_token(mid)) {
                    return ShareLink{Source::qq, Kind::album, std::string(mid)};
                }
                return make(Source::qq, Kind::album, query_param(query, "id"));
            }
            if (lpath.contains("/details/singer")) {
                if (auto mid = query_param(query, "singermid"); valid_token(mid)) {
                    return ShareLink{Source::qq, Kind::artist, std::string(mid)};
                }
                return make(Source::qq, Kind::artist, query_param(query, "id"));
            }
            return std::nullopt;
        }

        // Qishui Music
        if (host == "qishui.com" || host == "www.qishui.com" || host == "music.douyin.com" || host == "qishui.douyin.com") {
            if (auto seg = segment_after(path, "/track/"); !seg.empty()) {
                return make(Source::qishui, Kind::song, seg);
            }
            if (auto id = query_param(query, "track_id"); !id.empty()) {
                return make(Source::qishui, Kind::song, id);
            }
            if (auto seg = segment_after(path, "/playlist/"); !seg.empty()) {
                return make(Source::qishui, Kind::playlist, seg);
            }
            if (auto id = query_param(query, "playlist_id"); !id.empty()) {
                return make(Source::qishui, Kind::playlist, id);
            }
            if (auto seg = segment_after(path, "/album/"); !seg.empty()) {
                return make(Source::qishui, Kind::album, seg);
            }
            if (auto id = query_param(query, "album_id"); !id.empty()) {
                return make(Source::qishui, Kind::album, id);
            }
            if (auto seg = segment_after(path, "/artist/"); !seg.empty()) {
                return make(Source::qishui, Kind::artist, seg);
            }
            if (auto id = query_param(query, "artist_id"); !id.empty()) {
                return make(Source::qishui, Kind::artist, id);
            }
            return std::nullopt;
        }

        // Kugou Music
        if (host == "kugou.com" || host == "www.kugou.com" || host == "m.kugou.com") {
            if (auto seg = segment_after(path, "/yy/special/single/"); !seg.empty()) {
                return make(Source::kugou, Kind::playlist, seg);
            }
            if (auto seg = segment_after(path, "/yy/album/single/"); !seg.empty()) {
                return make(Source::kugou, Kind::album, seg);
            }
            if (auto seg = segment_after(path, "/yy/singer/home/"); !seg.empty()) {
                return make(Source::kugou, Kind::artist, seg);
            }
            if (auto hash = query_param(query, "hash"); hash.size() == 32 && valid_token(hash)) {
                return ShareLink{Source::kugou, Kind::song, std::string(hash)};
            }
            if (auto hash = query_param(fragment, "hash"); hash.size() == 32 && valid_token(hash)) {
                return ShareLink{Source::kugou, Kind::song, std::string(hash)};
            }
            return std::nullopt;
        }

        // Bodian Music
        if ((host == "h5app.kuwo.cn" && lpath.starts_with("/m/bodian/")) || host == "bodian.kuwo.cn") {
            if (lpath.contains("playmusic")) {
                return make(Source::bodian, Kind::song, query_param(query, "musicId"));
            }
            if (lpath.contains("album")) {
                return make(Source::bodian, Kind::album, query_param(query, "albumid"));
            }
            if (lpath.contains("singer")) {
                return make(Source::bodian, Kind::artist, query_param(query, "singerId"));
            }
            if (lpath.contains("collection")) {
                const std::string_view pid = query_param(query, "playlistId");
                std::string_view src = query_param(query, "source");
                if (src.empty()) {
                    src = "4";
                }
                if (valid_token(pid) && valid_token(src)) {
                    return ShareLink{Source::bodian, Kind::playlist, std::format("{}:{}", src, pid)};
                }
            }
            return std::nullopt;
        }

        return std::nullopt;
    }

    std::string card_subtitle(const Card &card) {
        if (card.count_kind == Card::Count::none) {
            return card.subtitle;
        }
        static constexpr std::array formats = {"", "{} songs", "{} albums", "{} fans"};
        std::string count = trf(formats[static_cast<size_t>(card.count_kind)], card.count_kind == Card::Count::fans ? short_number(card.count) : std::to_string(card.count));
        return card.subtitle.empty() ? count : count + " · " + card.subtitle;
    }

    std::string short_number(int n) {
        if (language() == Language::zh) {
            if (n >= 100000000) {
                return trf("{:.1f}yi", n / 1e8);
            }
            return n >= 10000 ? trf("{:.1f}w", n / 1e4) : std::to_string(n);
        }
        if (n >= 1000000) {
            return std::format("{:.1f}M", n / 1e6);
        }
        return n >= 1000 ? std::format("{:.1f}K", n / 1e3) : std::to_string(n);
    }

    std::string cover_url(std::string_view url, int px) {
        if (url.empty()) {
            return {};
        }
        std::string u = image_url(url, px);
        // NetEase scales any image; some covers carry their own thumbnail and watermark parameters, the plain one is enough.
        if (u.contains(".music.126.net/")) {
            return std::format("{}?param={}y{}", std::string_view(u).substr(0, u.find('?')), px, px);
        }
        // QQ Music has fixed sizes per CDN: …/photo_new/T00{1,2,3}R300x300M000<mid>.jpg, …/music_cover/<id>/300,
        // or music-file*.y.qq.com/...?imageView2/4/w/600/h/600. T003 (toplist) covers do not exist at 90 or 800.
        if (auto at = u.find("/photo_new/T"); at != u.npos) {
            size_t r = at + 15, m = u.find('M', r);
            if (r < u.size() && u[r] == 'R' && m != u.npos) {
                const std::string_view kind = std::string_view(u).substr(at + 11, 4);
                if (kind == "T001" || kind == "T002") {
                    int s = px <= 90 ? 90 : px <= 150 ? 150 : px <= 300 ? 300 : px <= 600 ? 500 : 800;
                    u.replace(r + 1, m - r - 1, std::format("{}x{}", s, s));
                } else if (kind == "T003") {
                    int s = px <= 150 ? 150 : px <= 300 ? 300 : 500;
                    u.replace(r + 1, m - r - 1, std::format("{}x{}", s, s));
                }
            }
        } else if (u.contains("qpic.y.qq.com/music_cover/") || u.contains("p.qpic.cn/music_cover/")) {
            size_t q = std::min(u.find('?'), u.size()), slash = u.rfind('/', q);
            std::string_view size = std::string_view(u).substr(slash + 1, q - slash - 1);
            if (!size.empty() && std::ranges::all_of(size, [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                u.replace(slash + 1, size.size(), std::to_string(px <= 150 ? 150 : px <= 300 ? 300 : 600));
            }
        } else if (auto scheme = u.find("://"); scheme != u.npos) {
            std::string_view host = std::string_view(u).substr(scheme + 3);
            host = host.substr(0, host.find('/'));
            if (host.starts_with("music-file") && host.ends_with(".y.qq.com")) {
                const std::string_view base = std::string_view(u).substr(0, u.find('?'));
                const std::string_view query = base.size() < u.size() ? std::string_view(u).substr(base.size() + 1) : std::string_view{};
                const bool pure_image_view = query.starts_with("imageView2/") && std::ranges::all_of(query, [](unsigned char c) {
                                                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' || c == '_' || c == '-';
                                             });
                if (query.empty() || query == "n=1" || pure_image_view) {
                    const int s = px <= 150 ? 150 : px <= 300 ? 300 : 600;
                    return std::format("{}?imageView2/4/w/{}/h/{}", base, s, s);
                }
            }
        }
        return u;
    }

    std::string picture_url(std::string_view url, int px) {
        std::string u = image_url(url, px);
        // A zero height keeps NetEase's aspect; QQ Music serves comment pictures only at their own size.
        if (u.contains(".music.126.net/")) {
            return std::format("{}?param={}x0", std::string_view(u).substr(0, u.find('?')), px);
        }
        return u;
    }

} // namespace lyren
