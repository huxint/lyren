#include "app/lyrics.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
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
                    frac = v / std::pow(10.0, double(f.size()));
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

        std::vector<LyricLine> parse_lrc(std::string_view text) {
            std::vector<LyricLine> out;
            double offset = 0;
            while (!text.empty()) {
                std::string_view line = next_line(text);
                if (line.starts_with('{')) {
                    credit(line, out);
                    continue;
                }
                std::vector<double> times;
                while (line.starts_with('[')) {
                    auto close = line.find(']');
                    if (close == line.npos) {
                        break;
                    }
                    std::string_view tag = line.substr(1, close - 1);
                    if (auto t = timestamp(tag)) {
                        times.push_back(*t);
                    } else if (tag.starts_with("offset:")) {
                        int ms = 0;
                        std::string_view v = trim(tag.substr(7));
                        if (v.starts_with('+')) {
                            v.remove_prefix(1);
                        }
                        std::from_chars(v.data(), v.data() + v.size(), ms);
                        offset = ms / 1000.0;
                    }
                    line = line.substr(close + 1);
                }
                line = trim(line);
                if (line.empty()) {
                    continue;
                }
                for (double t : times) {
                    out.push_back({t, std::string(line), {}});
                }
            }
            // A positive offset shows lyrics earlier.
            for (auto &l : out) {
                l.time = std::max(0.0, l.time - offset);
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
            return out;
        }

        // The two leading numbers of "[start,duration]" or "(start,duration,0)" at the front of s, in ms; s moves past it.
        std::optional<std::pair<int, int>> timing(std::string_view &s, char open, char close) {
            auto end = s.find(close);
            if (!s.starts_with(open) || end == s.npos) {
                return std::nullopt;
            }
            const char *p = s.data() + 1, *e = s.data() + end;
            int a = 0, b = 0;
            auto r = std::from_chars(p, e, a);
            if (r.ec != std::errc{} || r.ptr == e || *r.ptr != ',' || std::from_chars(r.ptr + 1, e, b).ec != std::errc{}) {
                return std::nullopt;
            }
            s.remove_prefix(end + 1);
            return std::pair{a, b};
        }

        std::vector<LyricLine> parse_yrc(std::string_view text) {
            std::vector<LyricLine> out;
            while (!text.empty()) {
                std::string_view line = next_line(text);
                if (line.starts_with('{')) {
                    credit(line, out);
                    continue;
                }
                auto head = timing(line, '[', ']');
                if (!head) {
                    continue;
                }
                LyricLine l{.time = head->first / 1000.0};
                // Each word's text runs to the next timing; a '(' that doesn't start one is text.
                while (!line.empty()) {
                    if (auto w = timing(line, '(', ')')) {
                        auto at = uint32_t(l.text.size());
                        l.words.push_back({w->first / 1000.0, w->second / 1000.0, at, at});
                        continue;
                    }
                    size_t n = line.find('(', 1);
                    std::string_view chunk = line.substr(0, n);
                    while (l.text.empty() && chunk.starts_with(' ')) {
                        chunk.remove_prefix(1);
                    }
                    l.text += chunk;
                    if (!l.words.empty()) {
                        l.words.back().to = uint32_t(l.text.size());
                    }
                    line = n == line.npos ? std::string_view{} : line.substr(n);
                }
                while (l.text.ends_with(' ')) {
                    l.text.pop_back();
                }
                if (l.text.empty()) {
                    continue;
                }
                for (auto &w : l.words) {
                    w.to = std::min(w.to, uint32_t(l.text.size())), w.from = std::min(w.from, w.to);
                }
                out.push_back(std::move(l));
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
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
            while (!text.empty()) {
                std::string_view line = next_line(text);
                auto head = timing(line, '[', ']');
                if (!head) {
                    continue;
                }
                LyricLine l{.time = head->first / 1000.0};
                // A word runs up to the "(start,duration)" after it; a '(' that starts none is part of the word.
                while (!line.empty()) {
                    std::optional<std::pair<int, int>> w;
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
                    auto from = uint32_t(l.text.size());
                    l.text += word;
                    if (w) {
                        l.words.push_back({w->first / 1000.0, w->second / 1000.0, from, uint32_t(l.text.size())});
                    }
                }
                while (l.text.ends_with(' ')) {
                    l.text.pop_back();
                }
                if (l.text.empty()) {
                    continue;
                }
                for (auto &w : l.words) {
                    w.to = std::min(w.to, uint32_t(l.text.size())), w.from = std::min(w.from, w.to);
                }
                out.push_back(std::move(l));
            }
            std::ranges::stable_sort(out, {}, &LyricLine::time);
            return out;
        }

        void add_translation(std::vector<LyricLine> &lines, std::string_view translation) {
            auto trans = parse_lrc(translation);
            // QQ Music marks the lines it leaves untranslated with "//".
            std::erase_if(trans, [](const LyricLine &l) {
                return l.text == "//";
            });
            size_t j = 0;
            for (auto &l : lines) {
                while (j < trans.size() && trans[j].time < l.time - 0.05) {
                    ++j;
                }
                if (j < trans.size() && std::abs(trans[j].time - l.time) <= 0.05 && trans[j].text != l.text) {
                    l.translation = trans[j].text;
                }
            }
        }

    } // namespace

    std::vector<LyricLine> parse_lyrics(std::string_view lrc, std::string_view translation, std::string_view yrc) {
        auto lines = parse_yrc(yrc);
        if (lines.empty()) {
            lines = parse_lrc(lrc);
        }
        add_translation(lines, translation);
        return lines;
    }

    std::vector<LyricLine> parse_qrc(std::string_view qrc, std::string_view translation) {
        auto lines = parse_qrc_lines(xml_attribute(qrc, "LyricContent"));
        add_translation(lines, translation);
        return lines;
    }

    int lyric_at(const std::vector<LyricLine> &lines, double t) {
        return int(std::ranges::upper_bound(lines, t, {}, &LyricLine::time) - lines.begin()) - 1;
    }

    int word_at(const LyricLine &line, double t) {
        return int(std::ranges::upper_bound(line.words, t, {}, &LyricWord::time) - line.words.begin()) - 1;
    }

} // namespace lyren
