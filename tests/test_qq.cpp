#include <doctest/doctest.h>

#include "core/i18n.hpp"
#include "media/lyrics.hpp"
#include "media/provider.hpp"
#include "qq/crypto.hpp"
#include "qq/auth.hpp"
#include "qq/qq.hpp"

using namespace lyren;

TEST_CASE("QQ playlist pages preserve direct and wrapped songs in server order") {
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
}

TEST_CASE("QQ playlist pages stop at the final page even when it contains songs") {
    // GetUniformSongDetailInfo uses a JSON boolean, unlike the numeric flag on favorite playlists.
    auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"total":1,"dirShow":0,
        "list":[{"id":1,"mid":"one","title":"最后一首"}],"hasmore":false,
        "filterNum":0,"diyNum":0,"deviceValid":true,"deviceBits":0}}})"});
    REQUIRE(page);
    REQUIRE(page->tracks.size() == 1);
    CHECK(page->tracks[0].title == "最后一首");
    CHECK_FALSE(page->more);
}

TEST_CASE("QQ playlist pages accept an empty directory") {
    auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"total":0,"dirShow":0,
        "list":[],"hasmore":false,"filterNum":0,"diyNum":0,"deviceValid":true,"deviceBits":0}}})"});
    REQUIRE(page);
    CHECK(page->tracks.empty());
    CHECK_FALSE(page->more);
}

TEST_CASE("QQ playlist pages preserve songs without playback mids in server order") {
    auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"hasmore":true,"list":[
        {"id":1,"mid":"one","title":"前一首"},
        {"track":{"id":2995157998,"mid":"","type":0,"title":"没有播放标识","interval":42}},
        {"songInfo":{"id":3,"title":"未返回 MID"}},
        {"id":4,"mid":null,"title":"MID 为 null"},
        {"id":5,"mid":"five","title":"后一首"}
    ]}}})"});
    REQUIRE(page);
    REQUIRE(page->tracks.size() == 5);
    CHECK(page->more);
    CHECK(page->tracks[0].id == "1");
    CHECK(page->tracks[1].id == "2995157998");
    CHECK(page->tracks[1].title == "没有播放标识");
    CHECK(page->tracks[1].duration == 42);
    CHECK(page->tracks[1].mid.empty());
    CHECK(page->tracks[2].id == "3");
    CHECK(page->tracks[2].mid.empty());
    CHECK(page->tracks[3].id == "4");
    CHECK(page->tracks[3].mid.empty());
    CHECK(page->tracks[4].id == "5");
    CHECK(page->tracks[4].mid == "five");
}

TEST_CASE("QQ playlist pages reject songs without an id instead of returning a partial list") {
    auto page = qq::parse_playlist({.body = R"({"req_0":{"code":0,"data":{"list":[
        {"id":1,"mid":"one"},{"songInfo":{"mid":"two"}}
    ]}}})"});
    REQUIRE_FALSE(page);
    CHECK(page.error() == "QQ Music returned incomplete song details");
}

TEST_CASE("QQ favorite playlists treat the empty collection response as a final page") {
    auto page = qq::parse_favorite_playlists({.body = R"({"code":0,"req_0":{"code":0,"data":{
        "number":0,"hasmore":0,"v_list":[],"v_failTids":[],"v_delTids":[],"total":0,"hide":false
    }}})"});
    REQUIRE(page);
    CHECK(page->playlists.empty());
    CHECK(page->total == 0);
    CHECK_FALSE(page->more);
}

TEST_CASE("QQ favorite playlists keep authentication and other errors visible") {
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
}

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
    CHECK(cover_url("http://qpic.y.qq.com/music_cover/xiabfMZA/300?n=1", 512) == "https://qpic.y.qq.com/music_cover/xiabfMZA/600?n=1");
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
    CHECK(qq::form({{"code", "a+b"}, {"state", "中文\n&="}}) == "code=a%2Bb&state=%E4%B8%AD%E6%96%87%0A%26%3D");
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

TEST_CASE("QQ untranslated markers stay out of lyrics") {
    auto lines = parse_lyrics("[00:01]One\n[00:02]Two\n", "[00:01]//\n[00:02]二\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].translation.empty());
    CHECK(lines[1].translation == "二");
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
    CHECK_FALSE(qq::parse_categories({.body = R"({"req_0":{"code":2001}})"}));
}

TEST_CASE("QQ discovery pagination preserves 64-bit playlist IDs and final pages") {
    const qq::Reply reply{.body = R"({"req_0":{"code":0,"data":{
        "List":[{"Playlist":{"basic":{"tid":8316781333,"title":"Road trip","song_cnt":1038,
            "cover":{"medium_url":"https://example.com/cover.jpg"},"creator":{"nick":"DJ"}}}}],
        "HasMore":true,"FromLimit":400
    }}})"};
    auto page = qq::parse_square(reply, 50);
    REQUIRE(page);
    REQUIRE(page->cards.size() == 1);
    CHECK(page->cards[0].id == "8316781333");
    CHECK(page->cards[0].source == Source::qq);
    CHECK(page->cards[0].count == 1038);
    CHECK(page->cards[0].subtitle == "DJ");
    CHECK(page->total == 400);
    auto end = qq::parse_square({.body = R"({"req_0":{"code":0,"data":{"List":[],"HasMore":false,"FromLimit":400}}})"}, 400);
    REQUIRE(end);
    CHECK(end->cards.empty());
    CHECK(end->total == 400);
    CHECK_FALSE(qq::parse_square({.body = R"({"req_0":{"code":0,"data":{"List":[{}]}}})"}, 0));
}

TEST_CASE("QQ category pages and related songs decode their distinct response envelopes") {
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

TEST_CASE("QQ radio and new releases retain song identity and playback metadata") {
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
}

TEST_CASE("QQ radar unwraps capitalized Track records and keeps their order") {
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

TEST_CASE("QQ discovery keeps service errors distinct from empty results") {
    for (auto parse : {qq::parse_radio, qq::parse_radar, qq::parse_new_songs}) {
        auto failed = parse({.body = R"({"req_0":{"code":2001,"data":{}}})"});
        REQUIRE_FALSE(failed);
        CHECK(failed.error() == "Sign in to QQ Music and try again");
        CHECK_FALSE(parse({.error = "Network connection failed"}));
        CHECK_FALSE(parse({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    }
    auto radio = qq::parse_radio({.body = R"({"req_0":{"code":0,"data":{"tracks":[]}}})"});
    REQUIRE(radio);
    CHECK(radio->empty());
    CHECK_FALSE(qq::parse_radio({.body = R"({"req_0":{"code":0,"data":{"tracks":[{"title":"Missing ID"}]}}})"}));
}

TEST_CASE("QQ new album pages use album mids and retain the service total") {
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
    CHECK_FALSE(qq::parse_new_albums({.body = R"({"req_0":{"code":2001}})"}));
}

TEST_CASE("QQ playlist favorites reject per-item failures even when the RPC succeeded") {
    CHECK(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":0,"v_failedPlaylistId":[]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":0,"v_failedPlaylistId":[4714021623]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{"result":1,"v_failedPlaylistId":[]}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    CHECK_FALSE(qq::parse_playlist_write({.body = R"({"req_0":{"code":2001}})"}));
}

TEST_CASE("QQ saved playlists preserve public IDs covers and lower-case song counts") {
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

TEST_CASE("QQ dislike pages keep categories opaque IDs and the ID type for restoration") {
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
}

TEST_CASE("QQ dislike pagination ends on the service's empty page") {
    const qq::Reply reply{.body = R"({"code":0,"req_0":{"code":0,"data":{
        "Retcode":0,"Msg":"","Songs":[],"Singers":[],"Styles":[],"Page":0,"Token":""}}})"};
    for (auto kind : {DislikeKind::song, DislikeKind::artist, DislikeKind::style}) {
        auto page = qq::parse_dislikes(reply, kind);
        REQUIRE(page);
        CHECK(page->items.empty());
        CHECK(page->cursor.empty());
    }
}

TEST_CASE("QQ dislike failures never appear as an empty successful list") {
    auto auth = qq::parse_dislikes({.body = R"({"req_0":{"code":2001}})"}, DislikeKind::song);
    REQUIRE_FALSE(auth);
    CHECK(auth.error() == "Sign in to QQ Music and try again");
    auto business = qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":10000,"Songs":[]}}})"}, DislikeKind::song);
    REQUIRE_FALSE(business);
    CHECK(business.error() == "QQ 音乐推荐反馈失败(10000)");
    CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Songs":[]}}})"}, DislikeKind::song));
    CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Singers":[]}}})"}, DislikeKind::song));
    CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Songs":[{"ID":"1","IdType":1},{"Name":"no ID","IdType":1}]}}})"}, DislikeKind::song));
    CHECK_FALSE(qq::parse_dislikes({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Songs":[{"ID":"1"}]}}})"}, DislikeKind::song));
}

TEST_CASE("QQ recommendation writes require the case-sensitive business result") {
    CHECK(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"Retcode":0,"Msg":""}}})"}));
    CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"Retcode":10000}}})"}));
    CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":2001}})"}));
    CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{"retcode":0}}})"}));
    CHECK_FALSE(qq::parse_dislike_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    CHECK_FALSE(qq::parse_dislike_write({.error = "Network connection failed"}));
}

TEST_CASE("QQ playlist ordering requires the explicit SeqSonglist result") {
    CHECK(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retCode":0}}})"}));
    auto refused = qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retCode":1000}}})"});
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "QQ 音乐没有保存歌单顺序(1000)");
    CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":2001}})"}));
    CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"result":{"retCode":0}}}})"}));
    CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{"retcode":0}}})"}));
    CHECK_FALSE(qq::parse_playlist_order_write({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    CHECK_FALSE(qq::parse_playlist_order_write({.error = "Network connection failed"}));
}

TEST_CASE("QQ comment identities like state and reply cursors retain their service values") {
    const qq::Reply reply{.body = R"({"req_0":{"code":0,"data":{"SubCode":0,"CommentList":{"HasMore":1,"Total":22,"Comments":[
        {"CmId":"1!opaque-id","SeqNo":"18446744073709551615","RankScore":"9007199254740993",
         "Nick":"作者","Content":"正文","PubTime":1700000000,"PraiseNum":12,"ReplyCnt":3,"IsPraised":1,
         "RepliedComments":[{"Nick":"原作者","Content":"被回复的正文"}]}
    ]}}}})"};
    auto main = qq::parse_comments(reply);
    REQUIRE(main);
    REQUIRE(main->comments.size() == 1);
    CHECK(main->comments[0].id == "1!opaque-id");
    CHECK(main->comments[0].liked);
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
}

TEST_CASE("QQ comment business failures are distinct from final empty pages") {
    auto empty = qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":0,"Total":0,"Comments":[]}}}})"}, true);
    REQUIRE(empty);
    CHECK(empty->comments.empty());
    CHECK_FALSE(empty->more);
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"SubCode":1000}}})"}));
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":2001}})"}));
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":0,"Comments":[{"Nick":"no identity"}]}}}})"}));
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":1,"Comments":[{"CmId":"one"}]}}}})"}));
    CHECK_FALSE(qq::parse_comments({.body = R"({"req_0":{"code":0,"data":{"CommentList":{"HasMore":1,"Comments":[{"CmId":"one","SeqNo":"123"}]}}}})"}, true));
}

TEST_CASE("QQ comment likes use the RPC acknowledgement for a void operation") {
    CHECK(qq::parse_comment_like({.body = R"({"req_0":{"code":0,"data":{}}})"}));
    CHECK(qq::parse_comment_like({.body = R"({"req_0":{"code":0}})"}));
    CHECK_FALSE(qq::parse_comment_like({.body = R"({"req_0":{"code":2001}})"}));
    CHECK_FALSE(qq::parse_comment_like({.body = R"({"req_0":{"code":1000,"data":{}}})"}));
    CHECK_FALSE(qq::parse_comment_like({.body = "{}"}));
    CHECK_FALSE(qq::parse_comment_like({.error = "Network connection failed"}));
}

TEST_CASE("QQ comment errors follow the selected language and retain the service code") {
    const Language before = language();
    const qq::Reply reply{.body = R"({"req_0":{"code":37}})"};
    set_language(Language::en);
    auto english = qq::parse_comment_like(reply);
    set_language(Language::zh);
    auto chinese = qq::parse_comment_like(reply);
    set_language(before);

    REQUIRE_FALSE(english);
    REQUIRE_FALSE(chinese);
    CHECK(english.error() == "QQ Music comment operation failed (37)");
    CHECK(chinese.error() == "QQ 音乐评论操作失败(37)");
}
