#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace lyren {

struct LyricLine {
    double time;  // seconds
    std::string text, translation;
};

// LRC with NetEase's JSON credit lines ({"t":ms,"c":[{"tx":...}]}); the translation is matched by timestamp.
// Lines are sorted by time; empty ones are dropped.
std::vector<LyricLine> parse_lyrics(std::string_view lrc, std::string_view translation = {});

// Index of the line playing at t, or -1 before the first.
int lyric_at(const std::vector<LyricLine>& lines, double t);

}  // namespace lyren
