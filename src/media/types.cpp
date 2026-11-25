#include "media/types.hpp"
#include "core/i18n.hpp"

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
                return {};
        }
        std::unreachable();
    }

    std::string share_url(const Card &card) {
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
