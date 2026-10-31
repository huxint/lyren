#include <doctest/doctest.h>

#include "ncm/crypto.hpp"
#include "ncm/netease.hpp"

using namespace lyren::ncm;

TEST_CASE("eapi known vector") {
    // From YetEaseMusic crypto.rs, computed independently with md5sum and openssl enc.
    CHECK(eapi("/api/song/lyric", R"({"id":1})") == "9B1595479B79266065903BA79E9883029338432CD7D98D1A3626E997A2C531219B13F34AFEC3DA8E69251F8EB21D1A1159D38E428357226CDD469CCFDC2CC9"
                                                    "54D27CF6A4C22E1023A03E8FAF9F5182C7BC2BCB0C4A7E53B249894EBF962CD6E6");
}

TEST_CASE("weapi matches independent AES and RSA vectors") {
    // AES-CBC computed with openssl enc; RSA with Python's pow(m, 65537, n).
    auto w = weapi(R"({"a":1})", "abcdefghijklmnop");
    CHECK(w.params == "Ho/4Pu1bYyrzwcnXPnTcXm1Gb04eSzwGSNb4NI3YqZY=");
    CHECK(w.enc_sec_key == "d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59"
                           "d53b32358903a121b5fca784c1f5ae44951fd02524df58ecc98e52cc7cf8689b"
                           "42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13e"
                           "f4cafbb0cf5a66731a3f1776c75ca35d0062dac70a3e33245afabcf47938487");
}

TEST_CASE("NetEase new songs preserve legacy metadata and exact resource IDs") {
    auto songs = parse_new_songs({.body = R"({"code":200,"data":[{
        "id":9007199254740993,"name":"新歌","duration":215900,"fee":1,"transNames":["译名"],
        "artists":[{"id":31,"name":"甲"},{"id":32,"name":"乙"}],
        "album":{"id":42,"name":"新碟","picUrl":"http://example.com/cover.jpg"}
    }]})"});
    REQUIRE(songs);
    REQUIRE(songs->size() == 1);
    const auto &song = songs->front();
    CHECK(song.source == lyren::Source::netease);
    CHECK(song.id == "9007199254740993");
    CHECK(song.title == "新歌");
    CHECK(song.alias == "译名");
    CHECK(song.duration == 215);
    CHECK(song.paid);
    CHECK(song.artist == "甲 / 乙");
    CHECK(song.artist_id == "31");
    CHECK(song.album_id == "42");
    CHECK(song.cover == "https://example.com/cover.jpg");
    CHECK_FALSE(parse_new_songs({.body = R"({"code":200,"data":null})"}));
    CHECK_FALSE(parse_new_songs({.error = "Connection failed"}));
}

TEST_CASE("NetEase new albums retain pagination totals and all credited artists") {
    auto page = parse_new_albums({.body = R"({"code":200,"total":500,"albums":[{
        "id":9007199254740993,"name":"联合创作","picUrl":"http://example.com/album.jpg","publishTime":0,"size":12,
        "artist":{"id":1,"name":"甲"},"artists":[{"id":1,"name":"甲"},{"id":2,"name":"乙"}]
    }]})"});
    REQUIRE(page);
    CHECK(page->total == 500);
    REQUIRE(page->cards.size() == 1);
    const auto &album = page->cards.front();
    CHECK(album.source == lyren::Source::netease);
    CHECK(album.kind == lyren::CardKind::album);
    CHECK(album.id == "9007199254740993");
    CHECK(album.title == "联合创作");
    CHECK(album.subtitle == "甲 / 乙");
    CHECK(album.cover == "https://example.com/album.jpg");
    CHECK(album.count == 12);
    CHECK(album.count_kind == lyren::Card::Count::songs);
    auto empty = parse_new_albums({.body = R"({"code":200,"total":500,"albums":[]})"});
    REQUIRE(empty);
    CHECK(empty->cards.empty());
    CHECK_FALSE(parse_new_albums({.body = R"({"code":502,"message":"请求失败"})"}));
}

TEST_CASE("NetEase playlist pages distinguish owned, liked, private and saved lists") {
    auto page = parse_user_playlists({.body = R"({"code":200,"more":true,"playlist":[
        {"id":1,"name":"我喜欢的音乐","specialType":5,"creator":{"userId":123}},
        {"id":2,"name":"自己的歌单","creator":{"userId":123}},
        {"id":9007199254740993,"name":"收藏歌单","creator":{"userId":456},"subscribed":true,"trackCount":37},
        {"id":4,"name":"私密歌单","creator":{"userId":456},"privacy":10}
    ]})"},
                                     "123");
    REQUIRE(page);
    CHECK(page->more);
    CHECK(page->liked.id == "1");
    CHECK(page->liked.owned);
    CHECK_FALSE(page->liked.collectable);
    REQUIRE(page->playlists.size() == 3);
    CHECK(page->playlists[0].owned);
    CHECK_FALSE(page->playlists[0].collectable);
    CHECK(page->playlists[1].id == "9007199254740993");
    CHECK_FALSE(page->playlists[1].owned);
    CHECK(page->playlists[1].collectable);
    CHECK(page->playlists[1].count == 37);
    CHECK_FALSE(page->playlists[2].collectable);
    auto end = parse_user_playlists({.body = R"({"code":200,"more":true,"playlist":[]})"}, "123");
    REQUIRE(end);
    CHECK_FALSE(end->more);
    CHECK_FALSE(parse_user_playlists({.body = R"({"code":200,"playlist":null})"}, "123"));
}

TEST_CASE("NetEase playlist writes preserve refusals instead of reporting success") {
    CHECK(parse_playlist_write({.body = R"({"code":200})"}));
    CHECK_FALSE(parse_playlist_write({.body = "{}"}));
    CHECK_FALSE(parse_playlist_write({.body = R"({"code":502})"}));
    auto refused = parse_playlist_write({.body = R"({"code":400,"message":"没有权限修改歌单"})"});
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "没有权限修改歌单");
    CHECK_FALSE(parse_playlist_write({.error = "Connection failed"}));
}

TEST_CASE("NetEase comments keep exact IDs, like state, replies and text") {
    auto page = parse_comments({.body = R"({"code":200,"data":{"totalCount":45,"hasMore":true,"cursor":"normalHot#20","comments":[
        {"commentId":9007199254740993,"user":{"nickname":"听众","avatarUrl":"http://example.com/avatar.jpg"},
         "content":" 第一行\n第二行 🎵 ","timeStr":"昨天23:18","likedCount":12,"liked":true,"owner":true,"replyCount":3,
         "beReplied":[{"user":{"nickname":"原作者"},"content":" 原文 "}]}
    ]}})"});
    REQUIRE(page);
    CHECK(page->total == 45);
    CHECK(page->more);
    CHECK(page->cursor == "normalHot#20");
    REQUIRE(page->comments.size() == 1);
    const auto &c = page->comments.front();
    CHECK(c.id == "9007199254740993");
    CHECK(c.user == "听众");
    CHECK(c.avatar == "https://example.com/avatar.jpg");
    CHECK(c.text == "第一行\n第二行 🎵");
    CHECK(c.time == "昨天23:18");
    CHECK(c.likes == 12);
    CHECK(c.liked);
    CHECK(c.owned);
    CHECK(c.replies == 3);
    CHECK(c.reply_user == "原作者");
    CHECK(c.reply == "原文");
    CHECK(c.image.empty());
}

TEST_CASE("NetEase comment pictures come over HTTPS without a known size") {
    auto page = parse_comments({.body = R"({"code":200,"data":{"totalCount":1,"hasMore":false,"comments":[
        {"commentId":7,"content":"看图","contentPicUrl":"http://p1.music.126.net/pic.jpg"},
        {"commentId":8,"content":"没有图","contentPicUrl":null}
    ]}})"});
    REQUIRE(page);
    REQUIRE(page->comments.size() == 2);
    CHECK(page->comments[0].image == "https://p1.music.126.net/pic.jpg");
    CHECK(page->comments[0].image_w == 0);
    CHECK_FALSE(page->comments[0].sticker);
    CHECK(page->comments[1].image.empty());
}

TEST_CASE("NetEase floor pagination uses the response timestamp and accepts nullable display fields") {
    auto page = parse_comments({.body = R"({"code":200,"data":{"totalCount":23,"hasMore":true,"time":1556817501007,
        "comments":[{"commentId":1436921389,"time":1554031500549,"user":null,"content":"回复","timeStr":null,
                     "liked":null,"replyCount":null,"beReplied":[{"user":null,"content":null}]}]}})"},
                               true);
    REQUIRE(page);
    CHECK(page->total == 23);
    CHECK(page->more);
    CHECK(page->cursor == "1556817501007");
    REQUIRE(page->comments.size() == 1);
    CHECK(page->comments.front().id == "1436921389");
    CHECK(page->comments.front().replies == 0);
    CHECK_FALSE(page->comments.front().liked);
    CHECK(page->comments.front().reply.empty());
    CHECK_FALSE(parse_comments({.body = R"({"code":200,"data":{"hasMore":true,"comments":[{"commentId":1}]}})"}, true));
}

TEST_CASE("NetEase comment failures do not become empty successful pages") {
    auto empty = parse_comments({.body = R"({"code":200,"data":{"comments":[],"totalCount":0,"hasMore":false}})"});
    REQUIRE(empty);
    CHECK(empty->comments.empty());
    CHECK_FALSE(empty->more);
    CHECK_FALSE(parse_comments({.body = R"({"code":400,"message":"无法加载"})"}));
    CHECK_FALSE(parse_comments({.body = R"({"code":200})"}));
    CHECK_FALSE(parse_comments({.body = R"({"code":200,"data":{"comments":[{"content":"missing ID"}]}})"}));
    CHECK_FALSE(parse_comments({.body = R"({"code":200,"data":{"hasMore":true,"comments":[{"commentId":1}]}})"}));
    CHECK_FALSE(parse_comments({.error = "Network connection failed"}));
}

TEST_CASE("NetEase comment writes require business success and posting returns its assigned ID") {
    CHECK(parse_comment_write({.body = R"({"code":200})"}));
    CHECK_FALSE(parse_comment_write({.body = R"({"code":502})"}));
    CHECK_FALSE(parse_comment_write({.body = "{}"}));
    auto posted = parse_comment_post({.body = R"({"code":200,"comment":{"commentId":9007199254740993,"content":"新评论"}})"});
    REQUIRE(posted);
    CHECK(*posted == "9007199254740993");
    auto refused = parse_comment_post({.body = R"({"code":400,"message":"评论内容过长"})"});
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "评论内容过长");
    CHECK_FALSE(parse_comment_post({.body = R"({"code":200,"comment":null})"}));
    CHECK_FALSE(parse_comment_post({.body = R"({"code":200,"comment":{}})"}));
    CHECK_FALSE(parse_comment_post({.error = "Network connection failed"}));
}
