#include <doctest/doctest.h>
#include "language_guard.hpp"
#include <glaze/glaze.hpp>

#include <atomic>
#include <optional>

#include "core/encoding.hpp"
#include "core/i18n.hpp"
#include "async.hpp"
#include "http_server.hpp"
#include "media/lyrics.hpp"
#include "media/provider.hpp"
#include "qq/auth.hpp"
#include "qq/crypto.hpp"
#include "qq/qq.hpp"

using namespace lyren;

namespace {
    using test::run;

    std::string_view http_body(std::string_view request) {
        return request.substr(request.find("\r\n\r\n") + 4);
    }
} // namespace

TEST_CASE("QQ search follows the server page when filtering returns fewer visible rows") {
    auto page = qq::parse_search({.body = R"({"req_0":{"code":0,"data":{
        "meta":{"sum":300,"nextpage":2,"perpage":40},
        "body":{"songlist":{"list":[{"dissid":"9007199254740993","dissname":"晚风","song_count":12}]}}
    }}})"},
                                 SearchKind::playlists, 40);
    REQUIRE(page);
    REQUIRE(page->cards.size() == 1);
    CHECK(page->cards[0].source == Source::qq);
    CHECK(page->cards[0].id == "9007199254740993");
    CHECK(page->cards[0].collectable);
    CHECK(page->total == 300);
    CHECK(page->next_offset == 40);
    auto end = qq::parse_search({.body = R"({"req_0":{"code":0,"data":{
        "meta":{"sum":300,"nextpage":-1},
        "body":{"songlist":{"list":[{"dissid":"last","dissname":"最后一个"}]}}
    }}})"},
                                SearchKind::playlists, 40);
    REQUIRE(end);
    CHECK(end->cards.size() == 1);
    CHECK(end->next_offset == -1);
    {
        auto artist = qq::parse_search({.body = R"({"req_0":{"code":0,"data":{
            "meta":{"sum":1,"nextpage":-1},
            "body":{"singer":{"list":[{"singerMID":"0025NhlN2yWrP4","singerName":"周杰伦","singerPic":"","songNum":10}]}}
        }}})"},
                                       SearchKind::artists, 20);
        REQUIRE(artist);
        REQUIRE(artist->cards.size() == 1);
        CHECK(artist->cards[0].cover == "https://y.gtimg.cn/music/photo_new/T001R300x300M0000025NhlN2yWrP4.jpg");
    }
    {
        auto album = qq::parse_search({.body = R"({"req_0":{"code":0,"data":{
            "meta":{"sum":1,"nextpage":-1},
            "body":{"album":{"list":[{"albumMID":"0041WVfh2vtlJE","albumName":"太阳之子","publicTime":"2026-03-25","singerName":"周杰伦","song_count":0}]}}
        }}})"},
                                      SearchKind::albums, 20);
        REQUIRE(album);
        REQUIRE(album->cards.size() == 1);
        CHECK(album->cards[0].count_kind == Card::Count::none);
    }
    {
        auto song = qq::parse_search({.body = R"({"req_0":{"code":0,"data":{
            "meta":{"sum":1,"nextpage":-1},
            "body":{"song":{"list":[{"id":1,"mid":"song-mid","title":"","name":"实际歌名","album":{"mid":"alb-mid","title":"","name":"实际专辑"}}]}}
        }}})"},
                                     SearchKind::tracks, 20);
        REQUIRE(song);
        REQUIRE(song->tracks.size() == 1);
        CHECK(song->tracks[0].title == "实际歌名");
        CHECK(song->tracks[0].album == "实际专辑");
    }
    CHECK_FALSE(qq::parse_search({.body = R"({"req_0":{"code":2001}})"}, SearchKind::playlists, 40));
}

TEST_CASE("QQ playlist pages keep songs in server order and stop at the final page") {
    auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"hasmore":true,"total":4,"list":[
        {"id":1,"mid":"one","title":"第一首","interval":42,"file":{"media_mid":"audio-one"},
         "album":{"mid":"album-one","title":"专辑"},"singer":[{"mid":"artist-one","name":"歌手"}]},
        {"unrelated":{"title":"忽略","values":[1,2,3]},"songInfo":{"id":2,"mid":"two","name":"第二首"}},
        {"track":{"id":3,"mid":"three","title":"第三首"}}
    ]}}})"});
    REQUIRE(page);
    REQUIRE(page->tracks.size() == 3);
    CHECK(page->more);
    CHECK(page->tracks[0].source == Source::qq);
    CHECK(page->tracks[0].id == "1");
    CHECK(page->tracks[0].title == "第一首");
    CHECK(page->tracks[0].artist == "歌手");
    CHECK(page->tracks[0].album == "专辑");
    CHECK(page->tracks[0].duration == 42);
    CHECK(page->tracks[0].media == "audio-one");
    CHECK(page->tracks[1].id == "2");
    CHECK(page->tracks[1].mid == "two");
    CHECK(page->tracks[1].title == "第二首");
    CHECK(page->tracks[2].id == "3");
    CHECK(page->tracks[2].title == "第三首");
    {
        // GetUniformSongDetailInfo uses a JSON boolean, unlike the numeric flag on favorite playlists.
        auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"total":1,"dirShow":0,
            "list":[{"id":1,"mid":"one","title":"最后一首"}],"hasmore":false,
            "filterNum":0,"diyNum":0,"deviceValid":true,"deviceBits":0}}})"});
        REQUIRE(page);
        REQUIRE(page->tracks.size() == 1);
        CHECK(page->tracks[0].title == "最后一首");
        CHECK_FALSE(page->more);
    }
    {
        auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"list":[
            {"id":1,"mid":"one"},{"songInfo":{"mid":"two"}}
        ]}}})"});
        REQUIRE_FALSE(page);
        CHECK(page.error() == "QQ Music returned incomplete song details");
    }
}

TEST_CASE("QQ favorite playlists parse pages and keep errors visible") {
    auto login = qq::parse_favorite_playlists({.body = R"({"req_0":{"code":2001}})"});
    REQUIRE_FALSE(login);
    CHECK(login.error() == "Sign in to QQ Music and try again");
    auto failure = qq::parse_favorite_playlists({.body = R"({"req_0":{"code":1000}})"});
    REQUIRE_FALSE(failure);
    CHECK(failure.error() == "QQ 音乐请求失败(1000)");
    auto wrong_identity = qq::parse_favorite_playlists({.body = R"({"req_0":{"code":80050,"data":{"v_list":[],"total":0,"hasmore":0}}})"});
    REQUIRE_FALSE(wrong_identity);
    CHECK(wrong_identity.error() == "QQ 音乐请求失败(80050)");
    auto other_endpoint = qq::parse_playlist({.body = R"({"req_0":{"code":80050}})"});
    REQUIRE_FALSE(other_endpoint);
    CHECK(other_endpoint.error() == "QQ 音乐请求失败(80050)");
    {
        auto page = qq::parse_favorite_playlists({.body = R"({"code":0,"req_0":{"code":0,"data":{
            "number":0,"hasmore":0,"v_list":[],"v_failTids":[],"v_delTids":[],"total":0,"hide":false
        }}})"});
        REQUIRE(page);
        CHECK(page->playlists.empty());
        CHECK(page->total == 0);
        CHECK_FALSE(page->more);
    }
    {
        auto result = qq::parse_favorite_playlists({.body = R"({"req_0":{"code":0,"data":{"total":1,"hasmore":0,"v_list":[
            {"tid":4714021623,"name":"Daily mix","songnum":30,"logo":"https://example.com/cover.jpg"}
        ]}}})"});
        REQUIRE(result);
        REQUIRE(result->playlists.size() == 1);
        const auto &card = result->playlists.front();
        CHECK(card.id == "4714021623");
        CHECK(card.title == "Daily mix");
        CHECK(card.cover == "https://example.com/cover.jpg");
        CHECK(card.count == 30);
        CHECK(card.count_kind == Card::Count::songs);
        CHECK(card.collectable);
        CHECK_FALSE(card.owned);
    }
}

TEST_CASE("qq request signature vectors") {
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
    CHECK(lines[0].words[4].time() == doctest::Approx(1.6));
    CHECK(lines[0].words[4].to == 11);
    CHECK(lines[1].time == doctest::Approx(29.264));
    CHECK(lines[1].text == "故事的 \"黄\"");
    CHECK(lines[1].words.back().duration() == doctest::Approx(0.5));
    CHECK(lines[1].translation == "The story's little yellow flower");
}

TEST_CASE("cover sizes per service") {
    CHECK(cover_url("http://p1.music.126.net/a/b.jpg?imageView=1", 128) == "https://p1.music.126.net/a/b.jpg?param=128y128");
    CHECK(cover_url("http://y.gtimg.cn/music/photo_new/T002R180x180M0000041WVfh2vtlJE_1.jpg", 128) == "https://y.gtimg.cn/music/photo_new/T002R150x150M0000041WVfh2vtlJE_1.jpg");
    CHECK(cover_url("https://y.gtimg.cn/music/photo_new/T001R300x300M0000025NhlN2yWrP4.jpg", 512) == "https://y.gtimg.cn/music/photo_new/T001R500x500M0000025NhlN2yWrP4.jpg");
    CHECK(cover_url("http://y.gtimg.cn/music/photo_new/T003R300x300M000002A28AA3D1uhW.jpg", 64) == "https://y.gtimg.cn/music/photo_new/T003R150x150M000002A28AA3D1uhW.jpg");
    CHECK(cover_url("http://y.gtimg.cn/music/photo_new/T003R300x300M000002A28AA3D1uhW.jpg", 800) == "https://y.gtimg.cn/music/photo_new/T003R500x500M000002A28AA3D1uhW.jpg");
    CHECK(cover_url("http://qpic.y.qq.com/music_cover/xiabfMZA/300?n=1", 512) == "https://qpic.y.qq.com/music_cover/xiabfMZA/600?n=1");
    CHECK(cover_url("http://p.qpic.cn/music_cover/xiabfMZA/600?n=1", 128) == "https://p.qpic.cn/music_cover/xiabfMZA/150?n=1");
    CHECK(cover_url("https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/600/h/600", 128) ==
          "https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/150/h/150");
    CHECK(cover_url("https://music-file6.y.qq.com/songcover/u/abc/cover.png?n=1", 256) == "https://music-file6.y.qq.com/songcover/u/abc/cover.png?imageView2/4/w/300/h/300");
    CHECK(cover_url("https://music-file.y.qq.com/songlist/u/abc/cover.jpg?sign=keep", 128) == "https://music-file.y.qq.com/songlist/u/abc/cover.jpg?sign=keep");
    CHECK(cover_url("https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/600/h/600&sign=keep", 128) ==
          "https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/600/h/600&sign=keep");
    CHECK(cover_url("https://pic6.y.qq.com/qqmusic/avatar/6f69-1792303394/140", 64) == "https://pic6.y.qq.com/qqmusic/avatar/6f69-1792303394/140");
}

TEST_CASE("QQ QR callbacks preserve status redirect and escaped nicknames") {
    auto waiting = qq::qr_callback("ptuiCB('66','0','','0','二维码未失效。','');");
    REQUIRE(waiting);
    CHECK((*waiting)[0] == "66");
    auto confirmed = qq::qr_callback(R"cb(ptuiCB('0','0','https://ssl.ptlogin2.graph.qq.com/check_sig?x=1','0','登录成功！','O\'Brien');)cb");
    REQUIRE(confirmed);
    CHECK((*confirmed)[2] == "https://ssl.ptlogin2.graph.qq.com/check_sig?x=1");
    CHECK((*confirmed)[5] == "O'Brien");
    CHECK_FALSE(qq::qr_callback("<html>login failed</html>"));
    CHECK_FALSE(qq::qr_callback("ptuiCB('0','0');"));
}

TEST_CASE("QQ OAuth fields use percent encoding and query decoding") {
    CHECK(encoding::query_string({{"code", "a+b"}, {"state", "中文\n&="}}) == "code=a%2Bb&state=%E4%B8%AD%E6%96%87%0A%26%3D");
    CHECK(qq::query_value("https://y.qq.com/callback?code=a%2Bb", "code") == "a+b");
    CHECK(qq::query_value("https://y.qq.com/callback?state=%E4%B8%AD%E6%96%87%0A%26%3D", "state") == "中文\n&=");
    CHECK(qq::query_value("https://y.qq.com/callback?code=a+b", "code") == "a b");
    CHECK(qq::query_value("https://y.qq.com/callback?state=one#code=two", "code").empty());
}

TEST_CASE("QQ login pages expose a quoted numeric client version") {
    CHECK(qq::qr_version("var config = { ptui_version: '22080914' };") == "22080914");
    CHECK(qq::qr_version("ptui_version :\n\t\"22080914\",") == "22080914");
    CHECK(qq::qr_version("ptui_version: encodeURIComponent('22080914'),") == "22080914");
    CHECK(qq::qr_version("<html>login failed</html>").empty());
    CHECK(qq::qr_version("ptui_version: '22080914\"").empty());
    CHECK(qq::qr_version("ptui_version: '22080914x'").empty());
}

TEST_CASE("QQ categories preserve service IDs independently of labels") {
    auto result = qq::parse_categories({.body = R"({"req_0":{"code":0,"data":{"v_group":[
        {"group_id":1,"group_name":"Featured","v_item":[
            {"id":3317,"name":"Official","status":0,"source":0},
            {"id":9527,"name":"Generated","status":0,"source":1},
            {"id":3417,"name":"Unavailable","status":2,"source":0}]},
        {"group_id":2,"group_name":"Mood","v_item":[
            {"id":167,"name":"Sweet","status":0,"source":0}]}
    ]}}})"});
    REQUIRE(result);
    REQUIRE(result->groups.size() == 2);
    REQUIRE(result->hot.size() == 1);
    CHECK(result->hot[0].id == "3317");
    CHECK(result->groups[1].second[0].id == "167");
    CHECK(result->groups[1].second[0].name == "Sweet");
}

TEST_CASE("QQ discovery responses keep identities, metadata and totals") {
    const std::string song = R"({"id":4835784001,"mid":"song-mid","title":"New song","type":1,"interval":237,
        "singer":[{"mid":"artist-mid","name":"Artist"}],"album":{"mid":"album-mid","title":"Album"},
        "file":{"media_mid":"media-mid"},"pay":{"pay_play":1}})";
    for (const auto &[field, parse] : std::array{std::pair{"tracks", qq::parse_radio}, std::pair{"songlist", qq::parse_new_songs}}) {
        CAPTURE(field);
        auto result = parse({.body = "{\"req_0\":{\"code\":0,\"data\":{\"" + std::string(field) + "\":[" + song + "]}}}"});
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        const auto &track = result->front();
        CHECK(track.source == Source::qq);
        CHECK(track.id == "4835784001");
        CHECK(track.mid == "song-mid");
        CHECK(track.media == "media-mid");
        CHECK(track.artist_id == "artist-mid");
        CHECK(track.album_id == "album-mid");
        CHECK(track.duration == 237);
        CHECK(track.type == 1);
        CHECK(track.paid);
    }
    {
        auto result = qq::parse_radar({.body = R"({"req_0":{"code":0,"data":{"VecSongs":[
            {"Track":{"id":10,"mid":"first","title":"First"}},
            {"Track":{"id":20,"mid":"second","title":"Second"}}
        ],"HasMore":true}}})"});
        REQUIRE(result);
        REQUIRE(result->size() == 2);
        CHECK(result->at(0).id == "10");
        CHECK(result->at(0).mid == "first");
        CHECK(result->at(1).id == "20");
        CHECK_FALSE(qq::parse_radar({.body = R"({"req_0":{"code":0,"data":{"VecSongs":[{}]}}})"}));
    }
    {
        for (auto parse : {qq::parse_radio, qq::parse_radar, qq::parse_new_songs}) {
            CHECK_FALSE(parse({.body = R"({"req_0":{"code":0,"data":{}}})"}));
        }
        auto radio = qq::parse_radio({.body = R"({"req_0":{"code":0,"data":{"tracks":[]}}})"});
        REQUIRE(radio);
        CHECK(radio->empty());
        CHECK_FALSE(qq::parse_radio({.body = R"({"req_0":{"code":0,"data":{"tracks":[{"title":"Missing ID"}]}}})"}));
    }
    {
        auto result = qq::parse_new_albums({.body = R"({"req_0":{"code":0,"data":{"total":4226,"albums":[
            {"id":105998828,"mid":"album-mid","name":"New album","release_time":"2026-10-29",
             "singers":[{"name":"First artist"},{"name":"Second artist"}]}
        ]}}})"});
        REQUIRE(result);
        CHECK(result->total == 4226);
        REQUIRE(result->cards.size() == 1);
        const auto &card = result->cards.front();
        CHECK(card.source == Source::qq);
        CHECK(card.kind == CardKind::album);
        CHECK(card.id == "album-mid");
        CHECK(card.title == "New album");
        CHECK(card.subtitle == "First artist / Second artist · 2026");
        CHECK(card.cover.find("album-mid") != std::string::npos);
        CHECK_FALSE(card.collectable);
        auto empty = qq::parse_new_albums({.body = R"({"req_0":{"code":0,"data":{"total":0,"albums":[]}}})"});
        REQUIRE(empty);
        CHECK(empty->cards.empty());
        CHECK_FALSE(qq::parse_new_albums({.body = R"({"req_0":{"code":0,"data":{"albums":[{"name":"Missing MID"}]}}})"}));
    }
    {
        auto category = qq::parse_category_playlists({.body = R"({"req_0":{"code":0,"data":{"content":{
            "total_cnt":986,"v_item":[{"basic":{"tid":7726401548,"title":"Classics","song_cnt":142}}]
        }}}})"});
        REQUIRE(category);
        REQUIRE(category->cards.size() == 1);
        CHECK(category->total == 986);
        CHECK(category->cards[0].id == "7726401548");
        auto similar = qq::parse_similar({.body = R"({"req_0":{"code":0,"data":{"vecSong":[{"track":{
            "id":213326626,"mid":"song-mid","title":"Related","type":0,"interval":237,
            "singer":[{"mid":"artist-mid","name":"Artist"}],"album":{"mid":"album-mid","title":"Album"},
            "file":{"media_mid":"media-mid"}
        }}]}}})"});
        REQUIRE(similar);
        REQUIRE(similar->size() == 1);
        CHECK(similar->front().id == "213326626");
        CHECK(similar->front().mid == "song-mid");
        CHECK(similar->front().media == "media-mid");
        CHECK(similar->front().duration == 237);
        CHECK_FALSE(qq::parse_similar({.body = R"({"req_0":{"code":0,"data":{"vecSong":[{}]}}})"}));
    }
    {
        const qq::Reply reply{.body = R"({"req_0":{"code":0,"data":{
            "List":[{"Playlist":{"basic":{"tid":8316781333,"title":"Road trip","song_cnt":1038,
                "cover":{"medium_url":"","default_url":"https://example.com/fallback.jpg"},"creator":{"nick":"DJ"}}}}],
            "HasMore":true,"FromLimit":400
        }}})"};
        auto page = qq::parse_square(reply, 50);
        REQUIRE(page);
        REQUIRE(page->cards.size() == 1);
        CHECK(page->cards[0].id == "8316781333");
        CHECK(page->cards[0].source == Source::qq);
        CHECK(page->cards[0].cover == "https://example.com/fallback.jpg");
        CHECK(page->cards[0].count == 1038);
        CHECK(page->cards[0].subtitle == "DJ");
        CHECK(page->total == 400);
        auto end = qq::parse_square({.body = R"({"req_0":{"code":0,"data":{"List":[],"HasMore":false,"FromLimit":400}}})"}, 400);
        REQUIRE(end);
        CHECK(end->cards.empty());
        CHECK(end->total == 400);
        CHECK_FALSE(qq::parse_square({.body = R"({"req_0":{"code":0,"data":{"List":[{}]}}})"}, 0));
    }
}

TEST_CASE("QQ writes require their business acknowledgement") {
    CHECK(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":0,"v_failedPlaylistId":[]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":0,"v_failedPlaylistId":[4714021623]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":1,"v_failedPlaylistId":[]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    {
        CHECK(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Msg":""}}})"}));
        CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"Retcode":10000}}})"}));
        CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"retcode":0}}})"}));
        CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    }
    {
        CHECK(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retCode":0}}})"}));
        auto refused = qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retCode":1000}}})"});
        REQUIRE_FALSE(refused);
        CHECK(refused.error() == "QQ 音乐没有保存歌单顺序(1000)");
        CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"result":{"retCode":0}}}})"}));
        CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retcode":0}}})"}));
        CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    }
    {
        CHECK(qq::parse_comment_like({.body = R"({"req_0":{"code":0,"data":{}}})"}));
        CHECK(qq::parse_comment_like({.body = R"({"req_0":{"code":0}})"}));
        CHECK_FALSE(qq::parse_comment_like({.body = R"({"req_0":{"code":2001}})"}));
        CHECK_FALSE(qq::parse_comment_like({.body = R"({"req_0":{"code":1000,"data":{}}})"}));
        CHECK_FALSE(qq::parse_comment_like({.body = "{}"}));
        CHECK_FALSE(qq::parse_comment_like({.error = "Network connection failed"}));
    }
    {
        auto posted = qq::parse_comment_post({.body = R"({"req_0":{"code":0,"data":{"SubCode":0,"AddedCmId":"1!new*comment","ParentCmId":"1!root","VerifyUrl":""}}})"});
        REQUIRE(posted);
        CHECK(*posted == "1!new*comment");
        auto refused = qq::parse_comment_post({.body = R"({"req_0":{"code":0,"data":{"SubCode":37,"Msg":"内容太长"}}})"});
        REQUIRE_FALSE(refused);
        CHECK(refused.error() == "内容太长");
        CHECK_FALSE(qq::parse_comment_post({.body = R"({"req_0":{"code":0,"data":{"SubCode":0,"VerifyUrl":"https://example.com/verify"}}})"}));
        CHECK_FALSE(qq::parse_comment_post({.body = R"({"req_0":{"code":0,"data":{}}})"}));
        CHECK_FALSE(qq::parse_comment_post({.body = R"({"req_0":{"code":0}})"}));
    }
    {
        CHECK(qq::parse_comment_delete({.body = R"({"req_0":{"code":0,"data":{"SubCode":0}}})"}));
        auto refused = qq::parse_comment_delete({.body = R"({"req_0":{"code":0,"data":{"SubCode":37,"Msg":"不能删除他人的评论"}}})"});
        REQUIRE_FALSE(refused);
        CHECK(refused.error() == "不能删除他人的评论");
    }
}

TEST_CASE("QQ dislike pages keep opaque IDs and fail visibly") {
    const qq::Reply reply{.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Page":1,"Token":"next",
        "Songs":[{"ID":"4835784","Name":"晴天","Img":"https://example.com/song.jpg","IdType":1,"Time":1793000000},
                 {"ID":"18446744073709551615","Name":"第二首","IdType":1}],
        "Singers":[{"ID":"artist-mid","Name":"歌手","Img":"https://example.com/artist.jpg","IdType":7}],
        "Styles":[{"ID":"12","Name":"摇滚","IdType":3}]}}})"};
    auto songs = qq::parse_dislikes(reply, DislikeKind::song);
    REQUIRE(songs);
    REQUIRE(songs->items.size() == 2);
    CHECK(songs->items[0].source == Source::qq);
    CHECK(songs->items[0].kind == DislikeKind::song);
    CHECK(songs->items[0].id == "4835784");
    CHECK(songs->items[0].name == "晴天");
    CHECK(songs->items[0].cover == "https://example.com/song.jpg");
    CHECK(songs->items[0].id_type == 1);
    CHECK(songs->cursor == "18446744073709551615");
    auto artists = qq::parse_dislikes(reply, DislikeKind::artist);
    REQUIRE(artists);
    REQUIRE(artists->items.size() == 1);
    CHECK(artists->items[0].kind == DislikeKind::artist);
    CHECK(artists->items[0].id == "artist-mid");
    CHECK(artists->items[0].id_type == 7);
    CHECK(artists->cursor == "artist-mid");
    auto styles = qq::parse_dislikes(reply, DislikeKind::style);
    REQUIRE(styles);
    REQUIRE(styles->items.size() == 1);
    CHECK(styles->items[0].kind == DislikeKind::style);
    CHECK(styles->items[0].name == "摇滚");
    CHECK(styles->items[0].id_type == 3);
    {
        const qq::Reply reply{.body = R"({"code":0,"req_0":{"code":0,"data":{
            "Retcode":0,"Msg":"","Songs":[],"Singers":[],"Styles":[],"Page":0,"Token":""}}})"};
        for (auto kind : {DislikeKind::song, DislikeKind::artist, DislikeKind::style}) {
            auto page = qq::parse_dislikes(reply, kind);
            REQUIRE(page);
            CHECK(page->items.empty());
            CHECK(page->cursor.empty());
        }
    }
    {
        auto business = qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":10000,"Songs":[]}}})"}, DislikeKind::song);
        REQUIRE_FALSE(business);
        CHECK(business.error() == "QQ 音乐推荐反馈失败(10000)");
        CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Songs":[]}}})"}, DislikeKind::song));
        CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Singers":[]}}})"}, DislikeKind::song));
        CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Songs":[{"ID":"1","IdType":1},{"Name":"no ID","IdType":1}]}}})"}, DislikeKind::song));
        CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Songs":[{"ID":"1"}]}}})"}, DislikeKind::song));
    }
}

TEST_CASE("QQ comment pages keep service identities, pictures and failures") {
    const qq::Reply reply{.body = R"({"req_0":{"code":0,"data":{"SubCode":0,"CommentList":{"HasMore":1,"Total":22,"Comments":[
        {"CmId":"1!opaque-id","SeqNo":"18446744073709551615","RankScore":"9007199254740993","EncryptUin":"owcANKcs7wCF7n**",
         "Nick":"作者","Content":"正文","PubTime":1700000000,"PraiseNum":12,"ReplyCnt":3,"IsPraised":1,"IsSelf":1,
         "RepliedComments":[{"Nick":"原作者","Content":"被回复的正文"}]}
    ]}}}})"};
    auto main = qq::parse_comments(reply);
    REQUIRE(main);
    REQUIRE(main->comments.size() == 1);
    CHECK(main->comments[0].id == "1!opaque-id");
    CHECK(main->comments[0].user_id == "owcANKcs7wCF7n**");
    CHECK(main->comments[0].liked);
    CHECK(main->comments[0].owned);
    CHECK(main->comments[0].likes == 12);
    CHECK(main->comments[0].replies == 3);
    CHECK(main->comments[0].reply_user == "原作者");
    CHECK(main->comments[0].reply == "被回复的正文");
    CHECK(main->cursor == "18446744073709551615");
    auto replies = qq::parse_comments(reply, true);
    REQUIRE(replies);
    CHECK(replies->cursor == R"(["18446744073709551615","9007199254740993"])");
    CHECK(replies->more);
    CHECK(replies->total == 22);
    {
        auto page = qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"SubCode":0,"CommentList":{"HasMore":0,"Total":3,"Comments":[
            {"CmId":"pic","Content":"晴天 [图片]","Pic":"https://music-file.y.qq.com/comment/a.jpg","PicSize":"1500x844","EmoPic":""},
            {"CmId":"emo","Content":" [表情]","Pic":"","PicSize":"","EmoPic":"https://y.qq.com/emo/1.png"},
            {"CmId":"text","Content":"[图片] 只是文字","Pic":"","PicSize":"bad"}
        ]}}}})"});
        REQUIRE(page);
        REQUIRE(page->comments.size() == 3);
        CHECK(page->comments[0].text == "晴天");
        CHECK(page->comments[0].image == "https://music-file.y.qq.com/comment/a.jpg");
        CHECK(page->comments[0].image_w == 1500);
        CHECK(page->comments[0].image_h == 844);
        CHECK_FALSE(page->comments[0].sticker);
        CHECK(page->comments[1].text.empty());
        CHECK(page->comments[1].image == "https://y.qq.com/emo/1.png");
        CHECK(page->comments[1].sticker);
        CHECK(page->comments[1].image_w == 0);
        CHECK(page->comments[2].text == "[图片] 只是文字");
        CHECK(page->comments[2].image.empty());
    }
    {
        auto empty = qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":0,"Total":0,"Comments":[]}}}})"}, true);
        REQUIRE(empty);
        CHECK(empty->comments.empty());
        CHECK_FALSE(empty->more);
        CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"SubCode":1000}}})"}));
        CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{}}})"}));
        CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":0,"Comments":[{"Nick":"no identity"}]}}}})"}));
        CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":1,"Comments":[{"CmId":"one"}]}}}})"}));
        CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":1,"Comments":[{"CmId":"one","SeqNo":"123"}]}}}})"}, true));
    }
}

TEST_CASE("QQ comment errors follow the selected language and retain the service code") {
    const lyren::test::LanguageGuard language_guard;
    const qq::Reply reply{.body = R"({"req_0":{"code":37}})"};
    set_language(Language::en);
    auto english = qq::parse_comment_like(reply);
    set_language(Language::zh);
    auto chinese = qq::parse_comment_like(reply);

    REQUIRE_FALSE(english);
    REQUIRE_FALSE(chinese);
    CHECK(english.error() == "QQ Music comment operation failed (37)");
    CHECK(chinese.error() == "QQ 音乐评论操作失败(37)");
}

TEST_CASE("QQ follow lists keep encrypted IDs and page by position") {
    auto page = qq::parse_people({.body = R"({"req_0":{"code":0,"data":{"Msg":"","Total":31,"FromLimit":2000,"HasMore":true,"LastPos":"opaque","List":[
        {"MID":"","EncUin":"owcANKcs7wCF7n**","Name":"听众","Desc":"签名","AvatarUrl":"http://thirdqq.qlogo.cn/g?b=sdk&s=140","FanNum":5,
         "IsFollow":true,"BeFollowed":false,"OtherInfo":{},"Time":1534514881}]}}})"},
                                 30);
    REQUIRE(page);
    CHECK(page->total == 31);
    CHECK(page->more);
    CHECK(page->cursor == "31");
    REQUIRE(page->people.size() == 1);
    const auto &person = page->people[0];
    CHECK(person.id == "owcANKcs7wCF7n**");
    CHECK(person.name == "听众");
    CHECK(person.signature == "签名");
    CHECK(person.avatar == "http://thirdqq.qlogo.cn/g?b=sdk&s=140");
    CHECK(person.fans == 5);
    CHECK(person.followed);
    CHECK_FALSE(person.follows_me);

    auto empty = qq::parse_people({.body = R"({"req_0":{"code":0,"data":{"Total":0,"HasMore":true,"List":null}}})"}, 0);
    REQUIRE(empty);
    CHECK(empty->people.empty());
    CHECK_FALSE(empty->more);
    CHECK_FALSE(qq::parse_people({.body = R"({"req_0":{"code":0,"data":{"List":[{"Name":"no ID"}]}}})"}, 0));
    CHECK_FALSE(qq::parse_people({.body = R"({"req_0":{"code":2001}})"}, 0));
    CHECK_FALSE(qq::parse_people({.error = "Network connection failed"}, 0));
}

TEST_CASE("QQ user playlists read picUrl covers and skip invalid directories") {
    auto result = qq::parse_user_playlists({.body = R"({"req_0":{"code":0,"data":{"total":5,"bFinish":true,"v_playlist":[
        {"dirId":201,"dirName":"我喜欢","tid":775578519,"songNum":99,"picUrl":"http://y.gtimg.cn/mediastyle/y/img/cover_love_300.jpg","bigpicUrl":"http://y.gtimg.cn/mediastyle/y/img/cover_love_300.jpg","albumPicUrl":"","invalid":false},
        {"dirId":54,"dirName":"通勤","tid":869224011,"songNum":163,"picUrl":"https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/600/h/600","invalid":false},
        {"dirId":55,"dirName":"纯专辑图","tid":0,"songNum":12,"picUrl":"","bigpicUrl":"","albumPicUrl":"http://y.gtimg.cn/music/photo_new/T002R500x500M000001ddAmt477PPK.jpg","invalid":false},
        {"dirId":56,"dirName":"空歌单","tid":9658677874,"songNum":0,"picUrl":"","bigpicUrl":"","albumPicUrl":"","invalid":false},
        {"dirId":0,"dirName":"无效ID","invalid":false},
        {"dirId":57,"dirName":"已失效","invalid":true}
    ]}}})"});
    REQUIRE(result);
    const auto &[liked, lists] = *result;
    CHECK(liked.id == "dir:201");
    CHECK(liked.count == 99);
    CHECK(liked.count_kind == Card::Count::songs);
    CHECK(liked.cover == "http://y.gtimg.cn/mediastyle/y/img/cover_love_300.jpg");
    REQUIRE(lists.size() == 3);
    CHECK(lists[0].id == "dir:54");
    CHECK(lists[0].title == "通勤");
    CHECK(lists[0].cover == "https://music-file.y.qq.com/songlist/u/abc/cover.jpg?imageView2/4/w/600/h/600");
    CHECK(lists[0].thread == "869224011");
    CHECK(lists[0].commentable);
    CHECK(lists[0].owned);
    CHECK(lists[1].id == "dir:55");
    CHECK(lists[1].cover == "http://y.gtimg.cn/music/photo_new/T002R500x500M000001ddAmt477PPK.jpg");
    CHECK_FALSE(lists[1].commentable);
    CHECK(lists[2].id == "dir:56");
    CHECK(lists[2].cover.empty());
    CHECK_FALSE(qq::parse_user_playlists({.body = R"({"req_0":{"code":2001}})"}));
}

TEST_CASE("QQ favorite albums parse live schema and pagination") {
    auto page = qq::parse_favorite_albums({.body = R"({"req_0":{"code":0,"data":{"number":23,"hasmore":1,"total":23,"v_list":[
        {"id":78837851,"mid":"000aiR9x2ok24A","name":"SO CLOSE TO WHAT","v_singer":[{"id":1,"mid":"s1","name":"Tate McRae"}],"logo":"http://y.gtimg.cn/music/photo_new/T002R150x150M000000aiR9x2ok24A_2.jpg","songnum":21},
        {"id":61263935,"mid":"003VlITY4TvDLJ","name":"合辑","v_singer":[{"name":"歌手A"},{"name":"歌手B"}],"logo":"","songnum":0}
    ]}}})"});
    REQUIRE(page);
    CHECK(page->total == 23);
    CHECK(page->more);
    REQUIRE(page->albums.size() == 2);
    CHECK(page->albums[0].kind == CardKind::album);
    CHECK(page->albums[0].id == "000aiR9x2ok24A");
    CHECK(page->albums[0].title == "SO CLOSE TO WHAT");
    CHECK(page->albums[0].subtitle == "Tate McRae");
    CHECK(page->albums[0].cover == "http://y.gtimg.cn/music/photo_new/T002R150x150M000000aiR9x2ok24A_2.jpg");
    CHECK(page->albums[0].count == 21);
    CHECK(page->albums[0].count_kind == Card::Count::songs);
    CHECK(page->albums[1].subtitle == "歌手A / 歌手B");
    CHECK(page->albums[1].cover == "https://y.gtimg.cn/music/photo_new/T002R300x300M000003VlITY4TvDLJ.jpg");
    CHECK(page->albums[1].count_kind == Card::Count::none);
    CHECK_FALSE(qq::parse_favorite_albums({.body = R"({"req_0":{"code":0,"data":{"v_list":[{"name":"no mid"}]}}})"}));
}

TEST_CASE("QQ followed artists support relation and legacy schemas") {
    auto relation = qq::parse_followed_artists({.body = R"({"req_0":{"code":0,"data":{"Total":2,"HasMore":true,"List":[
        {"MID":"0025NhlN2yWrP4","Name":"周杰伦","AvatarUrl":"https://y.gtimg.cn/music/photo_new/T001R300x300M0000025NhlN2yWrP4.jpg"}
    ]}}})"},
                                               0);
    REQUIRE(relation);
    CHECK(relation->total == 2);
    CHECK(relation->more);
    REQUIRE(relation->artists.size() == 1);
    CHECK(relation->artists[0].id == "0025NhlN2yWrP4");
    CHECK(relation->artists[0].title == "周杰伦");
    CHECK(relation->artists[0].cover == "https://y.gtimg.cn/music/photo_new/T001R300x300M0000025NhlN2yWrP4.jpg");

    auto legacy = qq::parse_followed_artists({.body = R"({"req_0":{"code":0,"data":{"Total":1,"List":[
        {"SingerMid":"001BLpXF2DyJe2","SingerName":"林俊杰","SingerPic":""}
    ]}}})"},
                                             0);
    REQUIRE(legacy);
    CHECK_FALSE(legacy->more);
    REQUIRE(legacy->artists.size() == 1);
    CHECK(legacy->artists[0].id == "001BLpXF2DyJe2");
    CHECK(legacy->artists[0].title == "林俊杰");
    CHECK(legacy->artists[0].cover == "https://y.gtimg.cn/music/photo_new/T001R300x300M000001BLpXF2DyJe2.jpg");
    CHECK_FALSE(qq::parse_followed_artists({.body = R"({"req_0":{"code":0,"data":{"Total":1,"List":[{"Name":"no mid"}]}}})"}, 0));
}

TEST_CASE("QQ other users playlists use public tids and never leak own favorites") {
    std::atomic<int> fav_calls = 0;
    test::Server server([&](std::string_view request) {
        const auto body = http_body(request);
        if (body.contains("GetPlaylistFavInfo_v2")) {
            ++fav_calls;
            return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"total":1,"hasmore":0,"v_list":[{"tid":999,"name":"OwnFav","songnum":5,"logo":""}]}}})");
        }
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"v_playlist":[
            {"dirId":201,"dirName":"我喜欢","tid":1001,"songNum":10,"picUrl":"https://example.com/like.jpg"},
            {"dirId":54,"dirName":"他人的歌单","tid":2002,"songNum":20,"picUrl":"https://example.com/other.jpg"},
            {"dirId":55,"dirName":"无公开ID","tid":0,"songNum":5,"picUrl":"https://example.com/private.jpg"}
        ]}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');
    provider.session() = {{"musicid", "12345"}, {"musickey", "key"}, {"encrypt_uin", "enc"}};
    provider.client.refresh_session = [](bool) -> Task<Result<void>> {
        co_return Result<void>{};
    };

    auto other = run(loop, provider.user_playlists("67890"));
    REQUIRE(other);
    CHECK(fav_calls == 0);
    CHECK(other->first.id == "1001");
    CHECK_FALSE(other->first.owned);
    CHECK(other->first.collectable);
    REQUIRE(other->second.size() == 1);
    CHECK(other->second[0].id == "2002");
    CHECK_FALSE(other->second[0].owned);
    CHECK(other->second[0].collectable);
}

TEST_CASE("QQ own user_playlists surfaces favorites failure instead of clearing saved playlists") {
    test::Server server([](std::string_view request) {
        const auto body = http_body(request);
        if (body.contains("GetPlaylistFavInfo_v2")) {
            return test::ok(R"({"code":0,"req_0":{"code":80050,"data":{"v_list":[],"total":0,"hasmore":0}}})");
        }
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"v_playlist":[
            {"dirId":201,"dirName":"我喜欢","tid":1001,"songNum":10,"picUrl":"https://example.com/like.jpg"}
        ]}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');
    provider.session() = {{"musicid", "12345"}, {"musickey", "key"}, {"encrypt_uin", "enc"}};
    provider.client.refresh_session = [](bool) -> Task<Result<void>> {
        co_return Result<void>{};
    };

    auto result = run(loop, provider.user_playlists("12345"));
    CHECK_FALSE(result);
}

TEST_CASE("QQ client retries single-part 2001 writes once, guards multipart/transport/session changes, and canonicalizes uin") {
    std::atomic<int> calls = 0;
    bool drop = false, multi_2001 = false, change_session = false;
    SUBCASE("single-part 2001 replays once after renewal") {}
    SUBCASE("multipart never-retry does not replay") {
        multi_2001 = true;
    }
    SUBCASE("transport failure never replays") {
        drop = true;
    }
    SUBCASE("session change during renewal aborts replay") {
        change_session = true;
    }

    test::Server server([&](std::string_view request) {
        const int n = ++calls;
        glz::generic parsed;
        const auto body = http_body(request);
        CHECK_FALSE(glz::read_json(parsed, body));
        CHECK(body.contains(R"("uin":1,)"));
        if (drop) {
            return std::string{};
        }
        if (n == 1) {
            return test::ok(R"({"code":0,"req_0":{"code":2001}})");
        }
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"retCode":0}}})");
    });
    Loop loop;
    net::Http http;
    qq::Client client(loop, http, server.url() + '/');
    client.session = {{"musicid", "001"}, {"musickey", "old-key"}};
    int renewals = 0;
    client.refresh_session = [&](bool force) -> Task<Result<void>> {
        if (force) {
            ++renewals;
            if (change_session) {
                ++client.session_gen;
            } else {
                client.session["musickey"] = "new-key";
            }
        }
        co_return Result<void>{};
    };

    std::vector<qq::Rpc> parts{{"mod", "write", "{}"}};
    if (multi_2001) {
        parts.push_back({"mod", "write2", "{}"});
    }
    auto reply = run(loop, client.call(std::move(parts), qq::Comm::app, net::Retry::never));

    if (drop) {
        CHECK_FALSE(reply.error.empty());
        CHECK(calls == 1);
        CHECK(renewals == 0);
    } else if (multi_2001) {
        CHECK(reply.needs_login);
        CHECK(calls == 1);
        CHECK(renewals == 1);
    } else if (change_session) {
        CHECK(reply.error == "QQ Music session changed; retry the action");
        CHECK(calls == 1);
        CHECK(renewals == 1);
    } else {
        CHECK(reply.error.empty());
        CHECK_FALSE(reply.needs_login);
        CHECK(calls == 2);
        CHECK(renewals == 1);
    }
}

TEST_CASE("QQ stream resolves missing media MID, skips empty sip entries, and maps anonymous login errors") {
    std::atomic<int> detail_calls = 0, vkey_calls = 0;
    test::Server server([&](std::string_view request) {
        const auto body = http_body(request);
        if (body.contains("UrlGetVkey")) {
            ++vkey_calls;
            CHECK(body.contains("M500real-media.mp3"));
            CHECK_FALSE(body.contains("M500song-mid.mp3"));
            if (body.contains(R"("uin":"0")")) {
                return test::ok(R"({"code":0,"req_0":{"code":104009,"data":{"sip":[],"midurlinfo":[]}}})");
            }
            return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"sip":["","http://cdn.example.com/"],
                "midurlinfo":[{"filename":"M500real-media.mp3","result":0,"purl":"track.mp3?vkey=1"}]}}})");
        }
        ++detail_calls;
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"track_info":{
            "id":213326626,"mid":"song-mid","file":{"media_mid":"real-media","try_begin":10000,"try_end":30000},
            "volume":{"gain":-6.5,"peak":0.9}
        }}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');

    auto anon = run(loop, provider.stream({.id = "213326626", .mid = "song-mid", .media = "real-media", .source = Source::qq}, Quality::standard));
    REQUIRE_FALSE(anon);
    CHECK(anon.error() == "Sign in to QQ Music and try again");

    provider.session() = {{"musicid", "123"}, {"musickey", "key"}};
    provider.client.refresh_session = [](bool) -> Task<Result<void>> {
        co_return Result<void>{};
    };
    auto resolved = run(loop, provider.stream({.id = "213326626", .mid = "song-mid", .source = Source::qq}, Quality::standard));
    REQUIRE(resolved);
    CHECK(detail_calls == 1);
    CHECK(vkey_calls == 2);
    CHECK(resolved->url == "https://cdn.example.com/track.mp3?vkey=1");
    CHECK(resolved->highlight_from == doctest::Approx(10.0));
    CHECK(resolved->highlight_to == doctest::Approx(30.0));
    CHECK(resolved->gain == doctest::Approx(-6.5));
}

TEST_CASE("QQ fm maps anonymous 1000 to sign-in while preserving authenticated 1000") {
    test::Server server([](std::string_view) {
        return test::ok(R"({"code":0,"req_0":{"code":1000,"data":{"tracks":[]}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');

    auto anon = run(loop, provider.fm());
    REQUIRE_FALSE(anon);
    CHECK(anon.error() == "Sign in to QQ Music and try again");

    provider.session() = {{"musicid", "123"}, {"musickey", "key"}};
    provider.client.refresh_session = [](bool) -> Task<Result<void>> {
        co_return Result<void>{};
    };
    auto authed = run(loop, provider.fm());
    REQUIRE_FALSE(authed);
    CHECK(authed.error() != "Sign in to QQ Music and try again");
}

TEST_CASE("QQ album pagination advances by raw server positions across filtered entries") {
    std::mutex mutex;
    std::vector<int> begins;
    test::Server server([&](std::string_view request) {
        std::lock_guard lock(mutex);
        const auto body = http_body(request);
        if (body.contains("GetAlbumDetail")) {
            begins.push_back(0);
            std::string list = R"({},{"songInfo":{"id":1,"mid":"m1","title":"s1"}})";
            for (int i = 2; i < 500; ++i) {
                list += std::format(R"(,{{"songInfo":{{"id":{0},"mid":"m{0}","title":"s{0}"}}}})", i);
            }
            return test::ok(std::format(
                R"({{"code":0,"req_0":{{"code":0,"data":{{"basicInfo":{{"albumID":9,"albumName":"Big"}}}}}},"req_1":{{"code":0,"data":{{"totalNum":503,"songList":[{}]}}}}}})",
                list));
        }
        if (body.contains(R"("begin":500)")) {
            begins.push_back(500);
            return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"totalNum":503,"songList":[{}]}}})");
        }
        if (body.contains(R"("begin":501)")) {
            begins.push_back(501);
            return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"totalNum":503,"songList":[
                {"songInfo":{"id":501,"mid":"m501","title":"s501"}},
                {"songInfo":{"id":502,"mid":"m502","title":"s502"}}
            ]}}})");
        }
        return test::ok(R"({"code":0,"req_0":{"code":1000}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');

    auto info = run(loop, provider.album("alb-mid"));
    REQUIRE(info);
    std::lock_guard lock(mutex);
    CHECK(begins == std::vector<int>{0, 500, 501});
    CHECK(info->tracks.size() == 501);
    CHECK(info->tracks.front().id == "1");
    CHECK(info->tracks.back().id == "502");
}

TEST_CASE("QQ dislikes rejects repeated cursors and excessive pages without silent truncation") {
    bool repeat = true;
    SUBCASE("repeated cursor fails without duplicating") {}
    SUBCASE("more than 100 advancing pages fails explicitly") {
        repeat = false;
    }
    std::atomic<int> pages = 0;
    test::Server server([&](std::string_view) {
        const int n = ++pages;
        const int id = repeat ? 1 : n;
        return test::ok(std::format(R"({{"code":0,"req_0":{{"code":0,"data":{{"Retcode":0,"Songs":[{{"ID":"{}","Name":"s","IdType":1}}]}}}}}})", id));
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');

    auto result = run(loop, provider.dislikes(DislikeKind::song));
    CHECK_FALSE(result);
    CHECK(pages == (repeat ? 2 : 100));
}

TEST_CASE("QQ people rejects invalid pagination cursors without issuing a request") {
    std::atomic<int> calls = 0;
    test::Server server([&](std::string_view) {
        ++calls;
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"Total":0,"List":[]}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');

    CHECK_FALSE(run(loop, provider.people("123", false, "not-a-number")));
    CHECK_FALSE(run(loop, provider.people("123", false, "-5")));
    CHECK(calls == 0);
}

TEST_CASE("QQ shares credential renewal across callers even when the initiator and another waiter cancel") {
    std::atomic<int> requests = 0;
    test::Server server([&](std::string_view request) {
        ++requests;
        CHECK(request.starts_with("POST /musicu.fcg "));
        CHECK(http_body(request).contains(R"("musicid":123)"));
        return test::ok(R"({"code":0,"req_0":{"code":0,"data":{"musicid":123,"musickey":"fresh","openid":"o","refresh_token":"r"}}})");
    });
    Loop loop;
    net::Http http;
    qq::Qq provider(loop, http, server.url() + '/');
    provider.session() = {{"musicid", "00123"}, {"musickey", "old"}, {"openid", "o"}, {"refresh_token", "r"}};
    auto first = provider.renew();
    first.await_suspend({}).resume();
    auto cancelled = provider.renew();
    cancelled.await_suspend({}).resume();
    first = {};
    cancelled = {};
    REQUIRE(run(loop, provider.renew()));
    CHECK(provider.client.get("musickey") == "fresh");
    CHECK(requests == 1);
    CHECK(run(loop, provider.renew(false)));
    CHECK(requests == 1);
    CHECK(run(loop, provider.renew()));
    CHECK(requests == 2);
}
