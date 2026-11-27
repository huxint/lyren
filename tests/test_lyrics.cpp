#include <doctest/doctest.h>

#include "media/lyrics.hpp"

using namespace lyren;

TEST_CASE("lrc with credits, repeats and a translation") {
    std::string lrc = "{\"t\":0,\"c\":[{\"tx\":\"作词: \"},{\"tx\":\"周杰伦\"}]}\n"
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
    std::string yrc = "{\"t\":0,\"c\":[{\"tx\":\"作词: \"},{\"tx\":\"Taylor Swift\"}]}\n"
                      "[16240,3600](16240,270,0)We (16510,210,0)were (16720,570,0)young\n"
                      "[11430,2610](11430,450,0)Bad(11880,0,0), (11880,360,0)(oh) \n";
    auto lines = parse_lyrics("[00:16.24]ignored\n", "[00:16.240]我们都还年轻\n", yrc);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "作词: Taylor Swift");
    CHECK(lines[0].words.empty());

    const auto &bad = lines[1];
    CHECK(bad.time == doctest::Approx(11.43));
    CHECK(bad.text == "Bad, (oh)");
    REQUIRE(bad.words.size() == 3);
    CHECK(bad.words[1].duration() == 0);
    CHECK(bad.text.substr(bad.words[1].from, bad.words[1].to - bad.words[1].from) == ", ");
    CHECK(bad.words[2].to == bad.text.size());

    const auto &young = lines[2];
    CHECK(young.text == "We were young");
    CHECK(young.translation == "我们都还年轻");
    REQUIRE(young.words.size() == 3);
    CHECK(young.words[1].time() == doctest::Approx(16.51));
    CHECK(young.words[1].duration() == doctest::Approx(0.21));
    CHECK(young.text.substr(young.words[1].from, young.words[1].to - young.words[1].from) == "were ");
    CHECK(word_at(young, 16.0) == -1);
    CHECK(word_at(young, 16.6) == 1);
    CHECK(word_at(young, 99) == 2);
}

TEST_CASE("QRC offsets keep lines words and translated LRC on the same timeline") {
    const auto lines = parse_qrc(R"qrc(<Lyric_1 LyricContent="[offset:+500]
[10000,2000]First (10000,750)line(10750,1250)
[14000,1000]Next(14000,1000)
"/>)qrc",
                                 "[offset:500]\n[00:10.00]Translated line\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].time == doctest::Approx(9.5));
    CHECK(lines[0].translation == "Translated line");
    REQUIRE(lines[0].words.size() == 2);
    CHECK(lines[0].words[0].time() == doctest::Approx(9.5));
    CHECK(lines[0].words[1].time() == doctest::Approx(10.25));
    CHECK(lines[0].words[1].duration() == doctest::Approx(1.25));
    CHECK(lyric_at(lines, 9.49) == -1);
    CHECK(lyric_at(lines, 9.5) == 0);
    CHECK(word_at(lines[0], 10.24) == 0);
    CHECK(word_at(lines[0], 10.25) == 1);
    {
        const auto lines = parse_qrc("[1000,1000]One(1000,1000)\n[offset:-250]\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].time == doctest::Approx(1.25));
        REQUIRE(lines[0].words.size() == 1);
        CHECK(lines[0].words[0].time() == doctest::Approx(1.25));
        CHECK(lines[0].words[0].duration() == doctest::Approx(1));
    }
    {
        const auto lines = parse_qrc("[offset:750]\n[0,2000]One(0,500) two(500,1500)\n");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].time == 0);
        REQUIRE(lines[0].words.size() == 2);
        CHECK(lines[0].words[0].time() == 0);
        CHECK(lines[0].words[0].duration() == 0);
        CHECK(lines[0].words[1].time() == 0);
        CHECK(lines[0].words[1].duration() == doctest::Approx(1.25));
    }
}

TEST_CASE("YRC offsets also shift word cues and preserve the match to translated LRC") {
    const auto lines = parse_lyrics("", "[offset:-250]\n[00:01.00]Translated", "[1000,500](1100,400,0)Word\n[offset:-250]");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].time == doctest::Approx(1.25));
    CHECK(lines[0].translation == "Translated");
    REQUIRE(lines[0].words.size() == 1);
    CHECK(lines[0].words[0].start_ms == 1350);
    CHECK(lines[0].words[0].duration_ms == 400);
}

TEST_CASE("KRC applies the offset after resolving relative word times") {
    const auto lines = parse_krc("[offset:+1200]\n[1000,1000]<100,200,0>One<300,700,0> two");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].time == 0);
    REQUIRE(lines[0].words.size() == 2);
    CHECK(lines[0].words[0].start_ms == 0);
    CHECK(lines[0].words[0].duration_ms == 100);
    CHECK(lines[0].words[1].start_ms == 100);
    CHECK(lines[0].words[1].duration_ms == 700);
}

TEST_CASE("mixed timed LRC applies a trailing offset once to both kinds of line") {
    const auto lines = parse_timed_lrc("[2000,500]<2100,400>Word\n[00:01.00]Line\n[offset:+500]");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].time == doctest::Approx(0.5));
    CHECK(lines[1].time == doctest::Approx(1.5));
    REQUIRE(lines[1].words.size() == 1);
    CHECK(lines[1].words[0].start_ms == 1600);
}

TEST_CASE("QQ plain and XML lyrics can use either word timing or LRC") {
    auto lrc = parse_qrc("[00:10.12]First line\n[00:20.50]Second line\n");
    REQUIRE(lrc.size() == 2);
    CHECK(lrc[0].time == doctest::Approx(10.12));
    CHECK(lrc[0].words.empty());
    auto xml = parse_qrc(R"qrc(<Lyric_1 LyricContent="[00:10.12]First line
"/>)qrc",
                         "[10120,1000]Translation(10120,1000)\n");
    REQUIRE(xml.size() == 1);
    CHECK(xml[0].translation == "Translation");
}

TEST_CASE("adjacent lyric lines reserve a translation for its exact timestamp") {
    const auto lines = lyren::parse_lyrics("[00:01.000]first\n[00:01.030]second", "[00:01.030]translation");
    REQUIRE(lines.size() == 2);
    CHECK(lines.front().translation.empty());
    CHECK(lines.back().translation == "translation");
}

TEST_CASE("format_lyrics_text and format_lrc preserve text, translations, and rounded timestamps") {
    const std::vector<LyricLine> lines{
        {.time = 0.0, .text = "Intro"},
        {.time = 59.996, .text = "Bridge line", .translation = "副歌之前"},
        {.time = 125.4, .text = "Chorus", .translation = "副歌"},
    };
    CHECK(format_lyrics_text(lines, false) == "Intro\nBridge line\nChorus");
    CHECK(format_lyrics_text(lines, true) == "Intro\nBridge line\n副歌之前\nChorus\n副歌");

    const std::string lrc = format_lrc(lines, false);
    CHECK(lrc == "[00:00.00]Intro\n[01:00.00]Bridge line\n[02:05.40]Chorus");
    const auto roundtrip = parse_lyrics(lrc);
    REQUIRE(roundtrip.size() == 3);
    CHECK(roundtrip[1].time == doctest::Approx(60.0));
    CHECK(roundtrip[2].text == "Chorus");

    const std::string lrc_trans = format_lrc(lines, true);
    CHECK(lrc_trans == "[00:00.00]Intro\n[01:00.00]Bridge line\n[01:00.00]副歌之前\n[02:05.40]Chorus\n[02:05.40]副歌");
}
