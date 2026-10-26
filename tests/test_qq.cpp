#include <doctest/doctest.h>

#include "app/lyrics.hpp"
#include "app/provider.hpp"
#include "qq/crypto.hpp"
#include "qq/auth.hpp"

using namespace lyren;

TEST_CASE("qq request signature vectors") {
    // docs/qqmusic-api.md 15.2.
    CHECK(qq::sign("") == "zzcf0e03e5gx4qeiq5cfgdyqwu7sdqfsb5fro3aa45059");
    CHECK(qq::sign("{}") == "zzcf8e26805gyafigxmxjehoe02mvsjjgtwzw6f1a05f1");
    CHECK(qq::sign(R"({"query":"周杰伦"})") == "zzcd01dd27o7bhc94y5rowjwmo133r17pred2357df9");
}

TEST_CASE("hash33 wraps at 32 bits and keeps 31") {
    CHECK(qq::hash33("abc") == 108966);
    CHECK(qq::hash33("abc", 5381) == 193485963);
    CHECK(qq::hash33("Q_H_L_63k3NaGnGqa1W9jtA0XHjQM6eQhFlv3fOI0") == 221379291);
}

TEST_CASE("qrc block cipher") {
    // docs/qqmusic-api.md 15.6: "QQMUSIC!" encrypts to 923dd8137d15fefa.
    CHECK(qq::qrc_block(0x923dd8137d15fefaull) == 0x51514d5553494321ull);
    // That block never reaches the two S-box entries that differ from standard DES; this one reaches both. From a
    // separate implementation that decodes real lyrics.
    CHECK(qq::qrc_block(0x6d132cded6237b2eull) == 0x72ac0da675f74dbaull);
}

TEST_CASE("qrc lyrics") {
    std::string qrc = R"(<?xml version="1.0" encoding="utf-8"?>
<QrcInfos>
<LyricInfo LyricCount="1">
<Lyric_1 LyricType="1" LyricContent="[ti:晴天]
[offset:0]
[0,2250]晴(0,160)天(160,160) (320,160)((1440,160)Jay(1600,160))(2080,160)
[29264,3447]故(29264,350)事(29614,330)的(29944,200) (30144,100)&quot;黄&quot;(30244,500)
"/>
</LyricInfo>
</QrcInfos>)";
    auto lines = parse_qrc(qrc, "[00:29.26]The story's little yellow flower\n");
    REQUIRE(lines.size() == 2);
    // A word comes before its timing, and a '(' that starts no timing is a word of its own.
    CHECK(lines[0].text == "晴天 (Jay)");
    REQUIRE(lines[0].words.size() == 6);
    CHECK(lines[0].words[3].from == 7);
    CHECK(lines[0].words[3].to == 8);
    CHECK(lines[0].words[4].time == doctest::Approx(1.6));
    CHECK(lines[0].words[4].to == 11);
    CHECK(lines[1].time == doctest::Approx(29.264));
    CHECK(lines[1].text == "故事的 \"黄\"");
    CHECK(lines[1].words.back().duration == doctest::Approx(0.5));
    CHECK(lines[1].translation == "The story's little yellow flower");
}

TEST_CASE("cover sizes per service") {
    CHECK(cover_url("http://p1.music.126.net/a/b.jpg?imageView=1", 128) == "https://p1.music.126.net/a/b.jpg?param=128y128");
    CHECK(cover_url("http://y.gtimg.cn/music/photo_new/T002R180x180M0000041WVfh2vtlJE_1.jpg", 128) ==
          "https://y.gtimg.cn/music/photo_new/T002R150x150M0000041WVfh2vtlJE_1.jpg");
    CHECK(cover_url("https://y.gtimg.cn/music/photo_new/T001R300x300M0000025NhlN2yWrP4.jpg", 512) ==
          "https://y.gtimg.cn/music/photo_new/T001R500x500M0000025NhlN2yWrP4.jpg");
    CHECK(cover_url("http://qpic.y.qq.com/music_cover/xiabfMZA/300?n=1", 512) == "https://qpic.y.qq.com/music_cover/xiabfMZA/600?n=1");
    CHECK(cover_url("https://pic6.y.qq.com/qqmusic/avatar/6f69-1792303394/140", 64) == "https://pic6.y.qq.com/qqmusic/avatar/6f69-1792303394/140");
}

TEST_CASE("QQ QR callbacks and OAuth query parameters") {
    auto waiting = qq::qr_callback("ptuiCB('66','0','','0','二维码未失效。','');");
    REQUIRE(waiting);
    CHECK((*waiting)[0] == "66");
    auto confirmed = qq::qr_callback(R"cb(ptuiCB('0','0','https://ssl.ptlogin2.graph.qq.com/check_sig?x=1','0','登录成功！','O\'Brien');)cb");
    REQUIRE(confirmed);
    CHECK((*confirmed)[2] == "https://ssl.ptlogin2.graph.qq.com/check_sig?x=1");
    CHECK((*confirmed)[5] == "O'Brien");
    CHECK_FALSE(qq::qr_callback("<html>login failed</html>"));
    CHECK_FALSE(qq::qr_callback("ptuiCB('0','0');"));
    auto query = qq::form({{"code", "a+b"}, {"state", "中文\n&="}});
    CHECK(qq::query_value("https://y.qq.com/callback?" + query, "code") == "a+b");
    CHECK(qq::query_value("https://y.qq.com/callback?" + query, "state") == "中文\n&=");
    CHECK(qq::query_value("https://y.qq.com/callback?state=one#code=two", "code").empty());
}

TEST_CASE("QQ untranslated markers stay out of lyrics") {
    auto lines = parse_lyrics("[00:01]One\n[00:02]Two\n", "[00:01]//\n[00:02]二\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].translation.empty());
    CHECK(lines[1].translation == "二");
}
