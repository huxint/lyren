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

TEST_CASE("yrc word timing with a translation") {
    std::string yrc =
        "{\"t\":0,\"c\":[{\"tx\":\"作词: \"},{\"tx\":\"Taylor Swift\"}]}\n"
        "[16240,3600](16240,270,0)We (16510,210,0)were (16720,570,0)young\n"
        "[11430,2610](11430,450,0)Bad(11880,0,0), (11880,360,0)(oh) \n";
    auto lines = parse_lyrics("[00:16.24]ignored\n", "[00:16.240]我们都还年轻\n", yrc);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "作词: Taylor Swift");
    CHECK(lines[0].words.empty());

    const auto& bad = lines[1];
    CHECK(bad.time == doctest::Approx(11.43));
    CHECK(bad.text == "Bad, (oh)");
    REQUIRE(bad.words.size() == 3);
    CHECK(bad.words[1].duration == 0);
    CHECK(bad.text.substr(bad.words[1].from, bad.words[1].to - bad.words[1].from) == ", ");
    CHECK(bad.words[2].to == bad.text.size());

    const auto& young = lines[2];
    CHECK(young.text == "We were young");
    CHECK(young.translation == "我们都还年轻");
    REQUIRE(young.words.size() == 3);
    CHECK(young.words[1].time == doctest::Approx(16.51));
    CHECK(young.words[1].duration == doctest::Approx(0.21));
    CHECK(young.text.substr(young.words[1].from, young.words[1].to - young.words[1].from) == "were ");
    CHECK(word_at(young, 16.0) == -1);
    CHECK(word_at(young, 16.6) == 1);
    CHECK(word_at(young, 99) == 2);
}
