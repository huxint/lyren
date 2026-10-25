#include <doctest/doctest.h>

#include "app/lyrics.hpp"

using namespace lyren;

TEST_CASE("lrc with credits, repeats and a translation") {
    std::string lrc =
        "{\"t\":0,\"c\":[{\"tx\":\"作词: \"},{\"tx\":\"周杰伦\"}]}\n"
        "[ti:晴天]\n"
        "[00:30.54]故事的小黄花\r\n"
        "[00:34.165][01:40.5]从出生那年就飘着\n"
        "[00:37:77]\n"
        "[00:41]童年的荡秋千\n";
    std::string trans = "[00:30.540]The little yellow flower\n[00:34.165]Has been floating since birth\n";
    auto lines = parse_lyrics(lrc, trans);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].time == 0);
    CHECK(lines[0].text == "作词: 周杰伦");
    CHECK(lines[1].time == doctest::Approx(30.54));
    CHECK(lines[1].translation == "The little yellow flower");
    CHECK(lines[2].time == doctest::Approx(34.165));
    CHECK(lines[2].translation == "Has been floating since birth");
    CHECK(lines[3].text == "童年的荡秋千");
    CHECK(lines[3].time == doctest::Approx(41));
    CHECK(lines[4].time == doctest::Approx(100.5));
    CHECK(lines[4].translation.empty());

    CHECK(lyric_at(lines, -1) == -1);
    CHECK(lyric_at(lines, 0) == 0);
    CHECK(lyric_at(lines, 34.2) == 2);
    CHECK(lyric_at(lines, 500) == 4);
}

TEST_CASE("lrc offset") {
    auto lines = parse_lyrics("[offset:500]\n[00:10.00]a\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].time == doctest::Approx(9.5));
}
