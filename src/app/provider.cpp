#include "app/provider.hpp"

#include <format>

namespace lyren {

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

} // namespace lyren
