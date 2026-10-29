#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lyren {

    struct LyricWord {
        uint32_t start_ms, duration_ms; // exact service timestamps; up to 49 days
        uint32_t from, to;              // UTF-8 boundaries: 0 <= from <= to <= line.text.size()

        double time() const {
            return start_ms / 1000.0;
        }
        double duration() const {
            return duration_ms / 1000.0;
        }
    };
    static_assert(sizeof(LyricWord) == 16);

    struct LyricLine {
        double time; // finite, nonnegative seconds; lines are sorted in increasing time
        std::string text, translation;
        std::vector<LyricWord> words; // empty without word timing
    };

    // LRC with NetEase's JSON credit lines ({"t":ms,"c":[{"tx":...}]}); the translation is matched by timestamp.
    // Word-timed lyrics (NetEase yrc: "[start,duration](start,duration,0)word…" in ms) replace the LRC lines when present.
    // Lines are sorted by time; empty ones are dropped.
    std::vector<LyricLine> parse_lyrics(std::string_view lrc, std::string_view translation = {}, std::string_view yrc = {});
    // QQ Music QRC once decoded: plain text or an XML wrapper whose LyricContent holds "[start,duration]word(start,duration)word…" lines
    // (ms, the word before its timing) among LRC tags. The translation is LRC, matched by timestamp like above.
    std::vector<LyricLine> parse_qrc(std::string_view qrc, std::string_view translation = {});

    // Index of the line playing at t, or -1 before the first.
    int lyric_at(const std::vector<LyricLine> &lines, double t);
    // Index of the last word of line started by t, or -1 before the first.
    int word_at(const LyricLine &line, double t);

} // namespace lyren
