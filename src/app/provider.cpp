#include "app/provider.hpp"

#include <format>
#include <cmath>
#include <unordered_map>

namespace lyren {

namespace {
std::string normalized(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out += char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    return out;
}

std::string song_key(const Track& t) {
    std::vector<std::string> artists;
    std::string_view s = t.artist;
    while (true) {
        auto slash = s.find(" / ");
        artists.push_back(normalized(s.substr(0, slash)));
        if (slash == s.npos) break;
        s.remove_prefix(slash + 3);
    }
    std::ranges::sort(artists);
    std::string out = normalized(t.title);
    for (const auto& artist : artists) out += '\x1f' + artist;
    return out;
}

void alternative(Track& to, const Track& from) {
    auto add = [&](const Track& t) {
        if (key(to) == key(t) || std::ranges::any_of(to.alts, [&](const Track& a) { return key(a) == key(t); })) return;
        to.alts.push_back(t);
        to.alts.back().alts.clear();
    };
    add(from);
    for (const auto& t : from.alts) add(t);
}
}  // namespace

std::vector<Track> merge_tracks(const std::vector<Track>& a, const std::vector<Track>& b) {
    std::vector<Track> out;
    std::unordered_map<std::string, std::vector<size_t>> groups;
    for (const Track& t : interleave(a, b)) {
        auto& group = groups[song_key(t)];
        auto it = std::ranges::find_if(group, [&](size_t i) {
            const Track& x = out[i];
            return same(x, t) || (x.source != t.source && std::abs(x.duration - t.duration) <= 3);
        });
        if (it == group.end()) {
            group.push_back(out.size());
            out.push_back(t);
        } else {
            Track& x = out[*it];
            if (x.paid && !t.paid) {
                Track old = std::move(x);
                x = t;
                alternative(x, old);
            } else {
                alternative(x, t);
            }
        }
    }
    return out;
}

Task<Result<Playback>> resolve_stream(Track t, Quality quality, std::function<Task<Result<Stream>>(Track, Quality)> fetch) {
    std::vector<Track> candidates;
    candidates.push_back(t);
    candidates.insert(candidates.end(), t.alts.begin(), t.alts.end());
    for (auto& c : candidates) c.alts.clear();
    std::optional<Playback> preview;
    std::string error;
    for (const Track& candidate : candidates) {
        auto s = co_await fetch(candidate, quality);
        if (!s) {
            error = s.error();
            continue;
        }
        Track chosen = candidate;
        for (const auto& other : candidates) alternative(chosen, other);
        Playback p{.track = std::move(chosen), .stream = std::move(*s)};
        if (!p.stream.trial) co_return p;
        if (!preview) preview = std::move(p);
    }
    if (preview) co_return std::move(*preview);
    co_return std::unexpected(std::move(error));
}

std::string cover_url(std::string_view url, int px) {
    if (url.empty()) return {};
    std::string u(url);
    if (u.starts_with("http://")) u.insert(4, "s");
    // NetEase scales any image; some covers carry their own thumbnail and watermark parameters, the plain one is enough.
    if (u.contains(".music.126.net/")) return std::format("{}?param={}y{}", std::string_view(u).substr(0, u.find('?')), px, px);
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
        if (!size.empty() && std::ranges::all_of(size, [](char c) { return c >= '0' && c <= '9'; }))
            u.replace(slash + 1, size.size(), std::to_string(px <= 150 ? 150 : px <= 300 ? 300 : 600));
    }
    return u;
}

}  // namespace lyren
