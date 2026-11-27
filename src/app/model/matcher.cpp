#include "app/model/matcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/string_map.hpp"

namespace lyren {

    namespace {
        enum VersionFlag : uint8_t {
            ver_live = 1 << 0,
            ver_remix = 1 << 1,
            ver_instrumental = 1 << 2,
            ver_cover = 1 << 3,
            ver_speed = 1 << 4,
        };

        bool is_ascii_alnum(unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }

        std::string ascii_lower(std::string_view input) {
            std::string out(input);
            for (char &ch : out) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return out;
        }

        bool contains_word(std::string_view haystack, std::string_view word) {
            size_t pos = 0;
            while ((pos = haystack.find(word, pos)) != haystack.npos) {
                const bool left_ok = pos == 0 || !is_ascii_alnum(static_cast<unsigned char>(haystack[pos - 1]));
                const size_t end = pos + word.size();
                const bool right_ok = end == haystack.size() || !is_ascii_alnum(static_cast<unsigned char>(haystack[end]));
                if (left_ok && right_ok) {
                    return true;
                }
                pos += word.size();
            }
            return false;
        }

        uint8_t version_flags(std::string_view title, std::string_view alias) {
            std::string combined = ascii_lower(title);
            if (!alias.empty()) {
                combined += ' ';
                combined += ascii_lower(alias);
            }
            uint8_t flags = 0;
            if (contains_word(combined, "live") || contains_word(combined, "concert") || contains_word(combined, "unplugged") || combined.contains("现场") ||
                combined.contains("演唱会")) {
                flags |= ver_live;
            }
            if (contains_word(combined, "remix") || contains_word(combined, "dj") || contains_word(combined, "mix") || contains_word(combined, "mashup") ||
                contains_word(combined, "bootleg") || combined.contains("电音版")) {
                flags |= ver_remix;
            }
            if (contains_word(combined, "instrumental") || contains_word(combined, "karaoke") || contains_word(combined, "off vocal") || contains_word(combined, "inst") ||
                combined.contains("伴奏") || combined.contains("纯音乐") || combined.contains("消音")) {
                flags |= ver_instrumental;
            }
            if (contains_word(combined, "cover") || combined.contains("翻唱")) {
                flags |= ver_cover;
            }
            if (contains_word(combined, "sped up") || contains_word(combined, "slowed") || contains_word(combined, "nightcore") || combined.contains("变速") ||
                combined.contains("加速版") || combined.contains("降调")) {
                flags |= ver_speed;
            }
            return flags;
        }

        // Removes bracketed qualifiers such as "(feat. X)", "（Live）", "[Remaster]".
        std::string strip_brackets(std::string_view input) {
            std::string out;
            out.reserve(input.size());
            int depth = 0;
            for (size_t i = 0; i < input.size();) {
                const std::string_view tail = input.substr(i);
                if (tail[0] == '(' || tail[0] == '[') {
                    ++depth;
                    ++i;
                    continue;
                }
                if (tail[0] == ')' || tail[0] == ']') {
                    depth = std::max(0, depth - 1);
                    ++i;
                    continue;
                }
                if (tail.starts_with("（") || tail.starts_with("【")) {
                    ++depth;
                    i += 3;
                    continue;
                }
                if (tail.starts_with("）") || tail.starts_with("】")) {
                    depth = std::max(0, depth - 1);
                    i += 3;
                    continue;
                }
                if (depth == 0) {
                    out.push_back(input[i]);
                }
                ++i;
            }
            return out;
        }

        // Strips ASCII punctuation/whitespace and common CJK punctuation, lowercasing ASCII.
        std::string normalize_key(std::string_view input) {
            static constexpr std::array<std::string_view, 17> cjk_punct{
                "，", "。", "！", "？", "·", "’", "‘", "“", "”", "—", "～", "：", "；", "、", "　", "《", "》",
            };
            std::string out;
            out.reserve(input.size());
            for (size_t i = 0; i < input.size();) {
                const unsigned char c = static_cast<unsigned char>(input[i]);
                if (c < 0x80) {
                    if (c >= 'A' && c <= 'Z') {
                        out.push_back(static_cast<char>(c - 'A' + 'a'));
                    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                        out.push_back(static_cast<char>(c));
                    }
                    ++i;
                    continue;
                }
                const std::string_view tail = input.substr(i);
                bool skipped = false;
                for (std::string_view p : cjk_punct) {
                    if (tail.starts_with(p)) {
                        i += p.size();
                        skipped = true;
                        break;
                    }
                }
                if (skipped) {
                    continue;
                }
                out.push_back(input[i++]);
            }
            return out;
        }

        bool loose_contains(std::string_view a, std::string_view b) {
            if (a.empty() || b.empty()) {
                return false;
            }
            const auto [shorter, longer] = a.size() <= b.size() ? std::pair{a, b} : std::pair{b, a};
            if (!longer.contains(shorter)) {
                return false;
            }
            // Require meaningful overlap so single-letter titles do not match unrelated songs.
            const bool multi_byte = static_cast<unsigned char>(shorter[0]) >= 0x80;
            if (shorter.size() < (multi_byte ? 6uz : 3uz) && shorter.size() * 2 < longer.size()) {
                return false;
            }
            return shorter.size() * 5 >= longer.size() * 2;
        }

        std::vector<std::string> normalized_artists(const Track &t) {
            std::vector<std::string> out;
            for (const auto &a : track_artists(t)) {
                auto norm = normalize_key(strip_brackets(a.name));
                if (!norm.empty() && !std::ranges::contains(out, norm)) {
                    out.push_back(std::move(norm));
                }
            }
            if (out.empty()) {
                if (auto norm = normalize_key(strip_brackets(t.artist)); !norm.empty()) {
                    out.push_back(std::move(norm));
                }
            }
            return out;
        }
    } // namespace

    std::string match_query(const Track &target) {
        std::string q(trim(target.title));
        const auto artists = track_artists(target);
        if (!artists.empty() && !trim(artists.front().name).empty()) {
            if (!q.empty()) {
                q += ' ';
            }
            q += trim(artists.front().name);
        } else if (const auto a = trim(target.artist); !a.empty()) {
            if (!q.empty()) {
                q += ' ';
            }
            q += a;
        }
        return q;
    }

    MatchScore score_match(const Track &target, const Track &candidate) {
        // Never cross studio tracks with live, remix, instrumental, cover, or sped-up variants.
        if (version_flags(target.title, target.alias) != version_flags(candidate.title, candidate.alias)) {
            return {};
        }

        int duration_diff = 0;
        if (target.duration > 0 && candidate.duration > 0) {
            duration_diff = std::abs(static_cast<int>(target.duration) - static_cast<int>(candidate.duration));
            if (duration_diff > 6) {
                return {.acceptable = false, .score = 0, .duration_diff = duration_diff};
            }
        }

        const std::string t_full = normalize_key(target.title);
        const std::string c_full = normalize_key(candidate.title);
        const std::string t_base = normalize_key(strip_brackets(target.title));
        const std::string c_base = normalize_key(strip_brackets(candidate.title));
        const std::string t_key = t_base.empty() ? t_full : t_base;
        const std::string c_key = c_base.empty() ? c_full : c_base;
        if (t_key.empty() || c_key.empty()) {
            return {};
        }

        int score = 0;
        bool exact_title = false;
        if (!t_full.empty() && t_full == c_full) {
            score += 60;
            exact_title = true;
        } else if (t_key == c_key) {
            score += 52;
            exact_title = true;
        } else if (loose_contains(t_key, c_key)) {
            score += 34;
        } else {
            return {};
        }

        const auto t_artists = normalized_artists(target);
        const auto c_artists = normalized_artists(candidate);
        if (t_artists.empty()) {
            if (!exact_title) {
                return {};
            }
            score += 10;
        } else {
            if (c_artists.empty()) {
                return {};
            }
            const std::string c_joined = normalize_key(candidate.artist);
            const std::string t_joined = normalize_key(target.artist);
            size_t matched = 0;
            bool primary_matched = false;
            for (size_t i = 0; i < t_artists.size(); ++i) {
                const std::string &ta = t_artists[i];
                const bool hit = std::ranges::any_of(c_artists,
                                                     [&](const std::string &ca) {
                                                         return ta == ca || loose_contains(ta, ca);
                                                     }) ||
                                 (!c_joined.empty() && ta.size() >= 2 && c_joined.contains(ta));
                if (hit) {
                    ++matched;
                    if (i == 0) {
                        primary_matched = true;
                    }
                }
            }
            if (matched == 0 && !t_joined.empty()) {
                const bool reverse_hit = std::ranges::any_of(c_artists, [&](const std::string &ca) {
                    return ca.size() >= 2 && t_joined.contains(ca);
                });
                if (reverse_hit) {
                    matched = 1;
                }
            }
            if (matched == 0) {
                return {};
            }
            if (matched == t_artists.size() && t_artists.size() == c_artists.size()) {
                score += 40;
            } else if (primary_matched || matched * 2 >= t_artists.size()) {
                score += 30;
            } else {
                score += 20;
            }
        }

        if (!target.album.empty() && !candidate.album.empty()) {
            const std::string t_alb = normalize_key(strip_brackets(target.album));
            const std::string c_alb = normalize_key(strip_brackets(candidate.album));
            if (!t_alb.empty() && t_alb == c_alb) {
                score += 8;
            }
        }
        if (target.duration > 0 && candidate.duration > 0) {
            score += (6 - duration_diff) * 2;
        }
        if (!candidate.paid) {
            score += 4;
        }
        return {.acceptable = true, .score = score, .duration_diff = duration_diff};
    }

    Task<std::optional<FallbackMatch>> find_fallback_stream(Env &env, const Track &target, Quality quality, bool skip_trials) {
        const std::string query = match_query(target);
        if (query.empty()) {
            co_return std::nullopt;
        }
        std::vector<SourceKey> order;
        for (const auto &entry : env.providers.entries()) {
            if (entry.source == target.source && entry.instance == target.instance) {
                continue;
            }
            if (!entry.provider->capabilities.supports_search(SearchKind::tracks)) {
                continue;
            }
            order.push_back({entry.source, entry.instance});
        }
        std::ranges::stable_partition(order, [&](const SourceKey &k) {
            return env.signed_in(k.source, k.instance);
        });

        for (const auto &key : order) {
            Provider &provider = env.provider(key.source, key.instance);
            auto res = co_await provider.search(query, SearchKind::tracks, 0, 10);
            if (!res || res->tracks.empty()) {
                continue;
            }
            std::vector<std::pair<int, size_t>> ranked;
            ranked.reserve(res->tracks.size());
            for (size_t i = 0; i < res->tracks.size(); ++i) {
                if (const auto m = score_match(target, res->tracks[i]); m.acceptable) {
                    ranked.emplace_back(m.score, i);
                }
            }
            std::ranges::stable_sort(ranked, [](const auto &a, const auto &b) {
                return a.first > b.first;
            });
            const size_t tries = std::min<size_t>(ranked.size(), 2);
            for (size_t k = 0; k < tries; ++k) {
                Track cand = res->tracks[ranked[k].second];
                auto stream = co_await provider.stream(cand, quality);
                if (stream && !(stream->trial && skip_trials)) {
                    co_return FallbackMatch{std::move(cand), std::move(*stream)};
                }
            }
        }
        co_return std::nullopt;
    }

} // namespace lyren
