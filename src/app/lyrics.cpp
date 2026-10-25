#include "app/lyrics.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

namespace lyren {

namespace {

struct Credit {
    std::optional<std::string> tx;
};
struct CreditLine {
    double t = 0;
    std::optional<std::vector<Credit>> c;
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx".
std::optional<double> timestamp(std::string_view s) {
    auto colon = s.find(':');
    if (colon == s.npos || colon == 0) return std::nullopt;
    int min = 0;
    if (std::from_chars(s.data(), s.data() + colon, min).ec != std::errc{}) return std::nullopt;
    std::string_view rest = s.substr(colon + 1);
    auto sep = rest.find_first_of(".:");
    int sec = 0;
    std::string_view whole = rest.substr(0, sep);
    if (whole.empty() || std::from_chars(whole.data(), whole.data() + whole.size(), sec).ec != std::errc{}) return std::nullopt;
    double frac = 0;
    if (sep != rest.npos) {
        std::string_view f = rest.substr(sep + 1);
        int v = 0;
        if (!f.empty() && std::from_chars(f.data(), f.data() + f.size(), v).ec == std::errc{}) frac = v / std::pow(10.0, double(f.size()));
    }
    return min * 60.0 + sec + frac;
}

std::vector<LyricLine> parse_lrc(std::string_view text) {
    std::vector<LyricLine> out;
    double offset = 0;
    while (!text.empty()) {
        auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == text.npos ? std::string_view{} : text.substr(nl + 1);
        if (line.starts_with('{')) {
            CreditLine c;
            if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(c, line) && c.c) {
                std::string s;
                for (const auto& part : *c.c) s += part.tx.value_or("");
                if (!trim(s).empty()) out.push_back({c.t / 1000, std::string(trim(s)), {}});
            }
            continue;
        }
        std::vector<double> times;
        while (line.starts_with('[')) {
            auto close = line.find(']');
            if (close == line.npos) break;
            std::string_view tag = line.substr(1, close - 1);
            if (auto t = timestamp(tag)) times.push_back(*t);
            else if (tag.starts_with("offset:")) {
                int ms = 0;
                std::string_view v = trim(tag.substr(7));
                if (v.starts_with('+')) v.remove_prefix(1);
                std::from_chars(v.data(), v.data() + v.size(), ms);
                offset = ms / 1000.0;
            }
            line = line.substr(close + 1);
        }
        line = trim(line);
        if (line.empty()) continue;
        for (double t : times) out.push_back({t, std::string(line), {}});
    }
    // A positive offset shows lyrics earlier.
    for (auto& l : out) l.time = std::max(0.0, l.time - offset);
    std::ranges::stable_sort(out, {}, &LyricLine::time);
    return out;
}

}  // namespace

std::vector<LyricLine> parse_lyrics(std::string_view lrc, std::string_view translation) {
    auto lines = parse_lrc(lrc);
    auto trans = parse_lrc(translation);
    size_t j = 0;
    for (auto& l : lines) {
        while (j < trans.size() && trans[j].time < l.time - 0.05) ++j;
        if (j < trans.size() && std::abs(trans[j].time - l.time) <= 0.05 && trans[j].text != l.text) l.translation = trans[j].text;
    }
    return lines;
}

int lyric_at(const std::vector<LyricLine>& lines, double t) {
    auto it = std::upper_bound(lines.begin(), lines.end(), t, [](double v, const LyricLine& l) { return v < l.time; });
    return int(it - lines.begin()) - 1;
}

}  // namespace lyren
