#include "media/types.hpp"
#include "core/i18n.hpp"

#include <format>

namespace lyren {

    std::string share_url(const Track &track) {
        return track.source == Source::qq ? "https://i.y.qq.com/v8/playsong.html?songid=" + track.id : "https://music.163.com/song?id=" + track.id;
    }

    std::string share_url(const Card &card) {
        if (card.source == Source::netease) {
            constexpr std::array paths = {"playlist", "album", "artist"};
            return std::format("https://music.163.com/{}?id={}", paths[static_cast<size_t>(card.kind)], card.id);
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
        }
        return id.empty() ? "" : std::format("https://y.qq.com/n/ryqq/{}/{}", path, id);
    }

    std::string card_subtitle(const Card &card) {
        if (card.count_kind == Card::Count::none) {
            return card.subtitle;
        }
        std::string count = trf(card.count_kind == Card::Count::songs ? "{} songs" : "{} albums", card.count);
        return card.subtitle.empty() ? count : count + " · " + card.subtitle;
    }

    std::string cover_url(std::string_view url, int px) {
        if (url.empty()) {
            return {};
        }
        std::string u(url);
        if (u.starts_with("http://")) {
            u.insert(4, "s");
        }
        // NetEase scales any image; some covers carry their own thumbnail and watermark parameters, the plain one is enough.
        if (u.contains(".music.126.net/")) {
            return std::format("{}?param={}y{}", std::string_view(u).substr(0, u.find('?')), px, px);
        }
        // QQ Music has a few sizes of each, named in the path: …/photo_new/T002R300x300M000<mid>.jpg, or …/music_cover/<id>/300.
        if (auto at = u.find("/photo_new/T"); at != u.npos) {
            size_t r = at + 15, m = u.find('M', r);
            if (r < u.size() && u[r] == 'R' && m != u.npos) {
                int s = px <= 90 ? 90 : px <= 150 ? 150 : px <= 300 ? 300 : px <= 600 ? 500 : 800;
                u.replace(r + 1, m - r - 1, std::format("{}x{}", s, s));
            }
        } else if (u.contains("qpic.y.qq.com/music_cover/")) {
            size_t q = std::min(u.find('?'), u.size()), slash = u.rfind('/', q);
            std::string_view size = std::string_view(u).substr(slash + 1, q - slash - 1);
            if (!size.empty() && std::ranges::all_of(size, [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                u.replace(slash + 1, size.size(), std::to_string(px <= 150 ? 150 : px <= 300 ? 300 : 600));
            }
        }
        return u;
    }

    std::string picture_url(std::string_view url, int px) {
        std::string u(url);
        if (u.starts_with("http://")) {
            u.insert(4, "s");
        }
        // A zero height keeps NetEase's aspect; QQ Music serves comment pictures only at their own size.
        if (u.contains(".music.126.net/")) {
            return std::format("{}?param={}x0", std::string_view(u).substr(0, u.find('?')), px);
        }
        return u;
    }

} // namespace lyren
