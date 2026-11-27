#include "media/lyrics.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <format>

#include "core/encoding.hpp"

namespace lyren {

    namespace {

        struct Credit {
            std::optional<std::string> tx;
        };
        struct CreditLine {
            double t = 0;
            std::optional<std::vector<Credit>> c;
        };
        struct KrcTranslation {
            int type = 0;
            std::vector<std::vector<std::string>> lyricContent;
        };
        struct KrcLanguage {
            std::vector<KrcTranslation> content;
        };

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
                s.remove_suffix(1);
            }
            return s;
        }

        // "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx".
        std::optional<double> timestamp(std::string_view s) {
            auto colon = s.find(':');
            if (colon == s.npos || colon == 0) {
                return std::nullopt;
            }
            int min = 0;
            if (std::from_chars(s.data(), s.data() + colon, min).ec != std::errc{}) {
                return std::nullopt;
            }
            std::string_view rest = s.substr(colon + 1);
            auto sep = rest.find_first_of(".:");
            int sec = 0;
            std::string_view whole = rest.substr(0, sep);
            if (whole.empty() || std::from_chars(whole.data(), whole.data() + whole.size(), sec).ec != std::errc{}) {
                return std::nullopt;
            }
            double frac = 0;
            if (sep != rest.npos) {
                std::string_view f = rest.substr(sep + 1);
                int v = 0;
                if (!f.empty() && std::from_chars(f.data(), f.data() + f.size(), v).ec == std::errc{}) {
                    frac = v / std::pow(10.0, static_cast<double>(f.size()));
                }
            }
            return min * 60.0 + sec + frac;
        }

        // Pops the next line off text, trimmed.
        std::string_view next_line(std::string_view &text) {
            auto nl = text.find('\n');
            std::string_view line = trim(text.substr(0, nl));
            text = nl == text.npos ? std::string_view{} : text.substr(nl + 1);
            return line;
        }

        void credit(std::string_view line, std::vector<LyricLine> &out) {
            CreditLine c;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(c, line) || !c.c) {
                return;
            }
            std::string s;
            for (const auto &part : *c.c) {
                s += part.tx.value_or("");
            }
            if (!trim(s).empty()) {
                out.push_back({c.t / 1000, std::string(trim(s)), {}});
            }
        }

        std::optional<int32_t> lyric_offset(std::string_view line) {
            if (!line.starts_with("[offset:") || !line.ends_with(']')) {
                return std::nullopt;
            }
            auto value = trim(line.substr(8, line.size() - 9));
            if (value.starts_with('+')) {
                value.remove_prefix(1);
            }
            int32_t milliseconds = 0;
            auto result = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                return std::nullopt;
            }
            return milliseconds;
        }

        // A positive offset shows lyrics earlier. Clip both ends of a word crossing the start of the song.
        void apply_offset(std::span<LyricLine> lines, int32_t offset) {
            for (auto &line : lines) {
                line.time = std::max(0.0, line.time - offset / 1000.0);
                for (auto &word : line.words) {
                    const int64_t start = static_cast<int64_t>(word.start_ms) - offset;
                    const int64_t end = std::clamp<int64_t>(start + word.duration_ms, 0, UINT32_MAX);
                    word.start_ms = static_cast<uint32_t>(std::clamp<int64_t>(start, 0, UINT32_MAX));
                    word.duration_ms = static_cast<uint32_t>(end - word.start_ms);
                }
            }
        }

        // The two leading numbers of "[start,duration]" or "(start,duration,0)" at the front of s, in ms; s moves past it.
        std::optional<std::pair<uint32_t, uint32_t>> timing(std::string_view &s, char open, char close) {
            auto end = s.find(close);
            if (!s.starts_with(open) || end == s.npos) {
                return std::nullopt;
            }
            const char *p = s.data() + 1, *e = s.data() + end;
            uint32_t a = 0, b = 0;
            auto r = std::from_chars(p, e, a);
            if (r.ec != std::errc{} || r.ptr == e || *r.ptr != ',' || std::from_chars(r.ptr + 1, e, b).ec != std::errc{}) {
                return std::nullopt;
            }
            s.remove_prefix(end + 1);
            return std::pair{a, b};
        }

        void sanitize_words(LyricLine &line) {
            for (auto &w : line.words) {
                w.to = std::min(w.to, static_cast<uint32_t>(line.text.size()));
                w.from = std::min(w.from, w.to);
            }
            if (!std::ranges::is_sorted(line.words, {}, &LyricWord::start_ms)) {
                std::ranges::stable_sort(line.words, {}, &LyricWord::start_ms);
            }
        }

        std::optional<LyricLine> parse_prefix_timed_line(std::string_view line, uint32_t time_ms, uint32_t word_base_ms, char open, char close) {
            LyricLine l{.time = time_ms / 1000.0};
            while (!line.empty()) {
                if (const auto w = timing(line, open, close)) {
                    if (w->first <= UINT32_MAX - word_base_ms) {
                        const auto at = static_cast<uint32_t>(l.text.size());
                        l.words.push_back({word_base_ms + w->first, w->second, at, at});
                    }
                    continue;
                }
                const size_t n = line.find(open, 1);
                std::string_view chunk = line.substr(0, n);
                while (l.text.empty() && chunk.starts_with(' ')) {
                    chunk.remove_prefix(1);
                }
                l.text += chunk;
                if (!l.words.empty()) {
                    l.words.back().to = static_cast<uint32_t>(l.text.size());
                }
                line = n == line.npos ? std::string_view{} : line.substr(n);
            }
            while (l.text.ends_with(' ')) {
                l.text.pop_back();
            }
            if (trim(l.text).empty()) {
                return std::nullopt;
            }
            sanitize_words(l);
            return l;
        }

        std::vector<LyricLine> read_lrc(std::string_view text, int32_t &offset, bool word_timed = false) {
            std::vector<LyricLine> out;
            if (text.starts_with("\xEF\xBB\xBF")) {
                text.remove_prefix(3);
            }
            while (!text.empty()) {
                std::string_view line = next_line(text);
                if (line.starts_with('{')) {
                    credit(line, out);
                    continue;
                }
                if (word_timed) {
                    if (auto head = timing(line, '[', ']')) {
                        if (auto parsed = parse_prefix_timed_line(line, head->first, 0, '<', '>')) {
                            out.push_back(std::move(*parsed));
                        }
                        continue;
                    }
                }
                std::vector<double> times;
                while (line.starts_with('[')) {
                    const auto close = line.find(']');
                    if (close == line.npos) {
                        break;
                    }
                    if (auto t = timestamp(line.substr(1, close - 1))) {
                        times.push_back(*t);
                    } else if (auto value = lyric_offset(line.substr(0, close + 1))) {
                        offset = *value;
                    }
                    line.remove_prefix(close + 1);
                }
                line = trim(line);
                if (!line.empty()) {
                    for (double t : times) {
                        out.push_back({t, std::string(line), {}});
                    }
                }
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
            return out;
        }

        std::vector<LyricLine> parse_lrc(std::string_view text, bool word_timed = false) {
            int32_t offset = 0;
            auto lines = read_lrc(text, offset, word_timed);
            apply_offset(lines, offset);
            return lines;
        }

        std::vector<LyricLine> parse_yrc(std::string_view text) {
            std::vector<LyricLine> out;
            int32_t offset = 0;
            while (!text.empty()) {
                std::string_view line = next_line(text);
                if (line.starts_with('{')) {
                    credit(line, out);
                    continue;
                }
                if (auto value = lyric_offset(line)) {
                    offset = *value;
                    continue;
                }
                const auto head = timing(line, '[', ']');
                if (!head) {
                    continue;
                }
                if (auto parsed = parse_prefix_timed_line(line, head->first, 0, '(', ')')) {
                    out.push_back(std::move(*parsed));
                }
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
            apply_offset(out, offset);
            return out;
        }

        // The attribute's value with the five XML entities undone.
        std::string xml_attribute(std::string_view xml, std::string_view name) {
            std::string key = std::string(name) + "=\"";
            auto at = xml.find(key);
            if (at == xml.npos) {
                return {};
            }
            std::string_view v = xml.substr(at + key.size());
            v = v.substr(0, v.find('"'));
            static constexpr auto entities = std::to_array<std::pair<std::string_view, char>>({{"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}});
            std::string out;
            while (!v.empty()) {
                auto e = std::ranges::find_if(entities, [&](const auto &x) {
                    return v.starts_with(x.first);
                });
                out += e == entities.end() ? v.front() : e->second;
                v.remove_prefix(e == entities.end() ? 1 : e->first.size());
            }
            return out;
        }

        std::vector<LyricLine> parse_qrc_lines(std::string_view text) {
            std::vector<LyricLine> out;
            int32_t offset = 0;
            while (!text.empty()) {
                std::string_view line = next_line(text);
                if (auto value = lyric_offset(line)) {
                    offset = *value;
                    continue;
                }
                auto head = timing(line, '[', ']');
                if (!head) {
                    continue;
                }
                LyricLine l{.time = head->first / 1000.0};
                // A word runs up to the "(start,duration)" after it; a '(' that starts none is part of the word.
                while (!line.empty()) {
                    std::optional<std::pair<uint32_t, uint32_t>> w;
                    std::string_view rest;
                    size_t p = line.find('(');
                    for (; p != line.npos; p = line.find('(', p + 1)) {
                        rest = line.substr(p);
                        if ((w = timing(rest, '(', ')'))) {
                            break;
                        }
                    }
                    std::string_view word = line.substr(0, p);
                    line = w ? rest : std::string_view{};
                    if (l.text.empty() && trim(word).empty()) {
                        continue;
                    }
                    auto from = static_cast<uint32_t>(l.text.size());
                    l.text += word;
                    if (w) {
                        l.words.push_back({w->first, w->second, from, static_cast<uint32_t>(l.text.size())});
                    }
                }
                while (l.text.ends_with(' ')) {
                    l.text.pop_back();
                }
                if (trim(l.text).empty()) {
                    continue;
                }
                sanitize_words(l);
                out.push_back(std::move(l));
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
            apply_offset(out, offset);
            return out;
        }

        std::vector<LyricLine> parse_qq_text(std::string_view text) {
            std::string xml;
            if (text.find("LyricContent=") != text.npos) {
                xml = xml_attribute(text, "LyricContent");
                text = xml;
            }
            auto lines = parse_qrc_lines(text);
            return lines.empty() ? parse_lrc(text) : std::move(lines);
        }

        void add_translation(std::vector<LyricLine> &lines, std::vector<LyricLine> trans) {
            // QQ Music marks the lines it leaves untranslated with "//".
            std::erase_if(trans, [](const LyricLine &l) {
                return l.text == "//";
            });
            size_t next = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                auto &line = lines[i];
                double best = 0.05;
                while (next < trans.size() && trans[next].time <= line.time + 0.05) {
                    auto &candidate = trans[next];
                    const double distance = std::abs(candidate.time - line.time);
                    if (i + 1 < lines.size() && std::abs(candidate.time - lines[i + 1].time) < distance) {
                        break;
                    }
                    if (distance <= best) {
                        best = distance;
                        line.translation = candidate.text == line.text ? std::string{} : std::move(candidate.text);
                    }
                    ++next;
                }
            }
        }

    } // namespace

    std::vector<LyricLine> parse_lyrics(std::string_view lrc, std::string_view translation, std::string_view yrc) {
        auto lines = parse_yrc(yrc);
        if (lines.empty()) {
            lines = parse_lrc(lrc);
        }
        add_translation(lines, parse_lrc(translation));
        return lines;
    }

    std::vector<LyricLine> parse_krc(std::string_view text) {
        std::vector<LyricLine> lines;
        KrcLanguage language;
        std::vector<uint32_t> starts;
        int32_t offset = 0;
        while (!text.empty()) {
            auto line = next_line(text);
            if (auto value = lyric_offset(line)) {
                offset = *value;
                continue;
            }
            if (line.starts_with("[language:") && line.ends_with(']')) {
                const auto decoded = encoding::unbase64(line.substr(10, line.size() - 11));
                static_cast<void>(glz::read<glz::opts{.error_on_unknown_keys = false}>(language, decoded));
                continue;
            }
            const auto head = timing(line, '[', ']');
            if (!head) {
                continue;
            }
            starts.push_back(head->first);
            if (auto parsed = parse_prefix_timed_line(line, head->first, head->first, '<', '>')) {
                lines.push_back(std::move(*parsed));
            }
        }
        std::ranges::stable_sort(lines, {}, &LyricLine::time);
        std::vector<LyricLine> trans;
        for (const auto &entry : language.content) {
            if (entry.type != 1) {
                continue;
            }
            const size_t n = std::min(starts.size(), entry.lyricContent.size());
            trans.reserve(trans.size() + n);
            for (size_t i = 0; i < n; ++i) {
                std::string joined;
                for (const auto &word : entry.lyricContent[i]) {
                    joined += word;
                }
                const auto cleaned = trim(joined);
                if (!cleaned.empty()) {
                    trans.push_back({starts[i] / 1000.0, std::string(cleaned), {}});
                }
            }
        }
        std::ranges::stable_sort(trans, {}, &LyricLine::time);
        add_translation(lines, std::move(trans));
        apply_offset(lines, offset);
        return lines;
    }

    std::vector<LyricLine> parse_timed_lrc(std::string_view text) {
        return parse_lrc(text, true);
    }

    std::vector<LyricLine> parse_kuwo(std::string_view text) {
        int start_factor = 0, duration_factor = 0;
        if (const auto at = text.find("[kuwo:"); at != text.npos) {
            if (const auto end = text.find(']', at); end != text.npos) {
                int value = 0;
                const auto parsed = std::from_chars(text.data() + at + 6, text.data() + end, value, 8);
                if (parsed.ec == std::errc{} && parsed.ptr == text.data() + end && value > 0) {
                    start_factor = value / 10;
                    duration_factor = value % 10;
                }
            }
        }
        int32_t offset_ms = 0;
        auto lines = read_lrc(text, offset_ms);
        std::vector<LyricLine> out;
        out.reserve(lines.size());
        for (auto &line : lines) {
            std::string_view remaining = line.text;
            std::string plain;
            std::vector<LyricWord> words;
            bool zero_tags = false, timed_tags = false;
            while (!remaining.empty()) {
                const auto at = remaining.find('<');
                plain += remaining.substr(0, at);
                if (at == remaining.npos) {
                    break;
                }
                remaining.remove_prefix(at);
                const auto end = remaining.find('>');
                int64_t a = 0, b = 0;
                bool timed = false;
                if (end != remaining.npos) {
                    const char *last = remaining.data() + end;
                    const auto first = std::from_chars(remaining.data() + 1, last, a);
                    if (first.ec == std::errc{} && first.ptr != last && *first.ptr == ',') {
                        const auto second = std::from_chars(first.ptr + 1, last, b);
                        timed = second.ec == std::errc{} && second.ptr == last;
                    }
                }
                if (!timed) {
                    plain += remaining.front();
                    remaining.remove_prefix(1);
                    continue;
                }
                zero_tags |= a == 0 && b == 0;
                timed_tags |= a != 0 || b != 0;
                remaining.remove_prefix(end + 1);
                const auto next = remaining.find('<');
                const auto word = remaining.substr(0, next);
                const auto from = static_cast<uint32_t>(plain.size());
                plain += word;
                if (start_factor && duration_factor && (a || b)) {
                    const long double offset = std::floor(std::abs(static_cast<long double>(a) + b) / (2 * start_factor));
                    const long double duration = std::floor(std::abs(static_cast<long double>(a) - b) / (2 * duration_factor));
                    const long double start = std::round(line.time * 1000) + offset;
                    if (start >= 0 && start <= UINT32_MAX && duration > 0 && duration <= UINT32_MAX - start) {
                        words.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(duration), from, static_cast<uint32_t>(plain.size())});
                    }
                }
                remaining = next == remaining.npos ? std::string_view{} : remaining.substr(next);
            }
            // All-zero word tags mark the translation after its original. Its timestamp can be the
            // next original's start, so a timestamp join would attach it to the wrong sentence.
            if (zero_tags && !timed_tags) {
                const auto cleaned = trim(plain);
                if (!out.empty() && !cleaned.empty() && cleaned != out.back().text) {
                    out.back().translation = std::string(cleaned);
                }
                continue;
            }
            line.text = std::string(trim(plain));
            if (line.text.empty()) {
                continue;
            }
            if (plain.size() != line.text.size()) {
                const auto lead = static_cast<uint32_t>(plain.find_first_not_of(" \t\r"));
                for (auto &w : words) {
                    w.from = w.from > lead ? w.from - lead : 0;
                    w.to = w.to > lead ? w.to - lead : 0;
                }
            }
            line.words = std::move(words);
            sanitize_words(line);
            out.push_back(std::move(line));
        }
        apply_offset(out, offset_ms);
        return out;
    }

    std::vector<LyricLine> parse_qrc(std::string_view qrc, std::string_view translation) {
        auto lines = parse_qq_text(qrc);
        add_translation(lines, parse_qq_text(translation));
        return lines;
    }

    int lyric_at(const std::vector<LyricLine> &lines, double t) {
        return static_cast<int>(std::ranges::upper_bound(lines, t, {}, &LyricLine::time) - lines.begin()) - 1;
    }

    int word_at(const LyricLine &line, double t) {
        return static_cast<int>(std::ranges::upper_bound(line.words, t, {}, &LyricWord::time) - line.words.begin()) - 1;
    }

} // namespace lyren
