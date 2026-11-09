#include <doctest/doctest.h>

#include "core/i18n.hpp"
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

TEST_CASE("NetEase comment pages keep exact IDs, pictures, cursors and failures") {
    auto page = parse_comments({.body = R"({"code":200,"data":{"totalCount":45,"hasMore":true,"cursor":"normalHot#20","comments":[
        {"commentId":9007199254740993,"user":{"userId":9007199254740995,"nickname":"听众","avatarUrl":"http://example.com/avatar.jpg"},
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
    CHECK(c.user_id == "9007199254740995");
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
    {
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
    {
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
    {
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

TEST_CASE("NetEase listener pages keep counts, relations and the province in either language") {
    const Reply reply{.body = R"({"code":200,"level":9,"listenSongs":12345,"createDays":3001,"peopleCanSeeMyPlayRecord":true,"profile":{
        "userId":9007199254740993,"nickname":"听众","avatarUrl":"http://p1.music.126.net/a.jpg","backgroundUrl":"https://p1.music.126.net/b.jpg",
        "signature":"\n 签名 \n","follows":12,"followeds":34567,"eventCount":8,"playlistCount":5,"vipType":11,"province":440000,
        "followed":true,"followMe":false}})"};
    const auto before = lyren::language();
    lyren::set_language(lyren::Language::en);
    auto profile = parse_profile(reply);
    REQUIRE(profile);
    CHECK(profile->id == "9007199254740993");
    CHECK(profile->name == "听众");
    CHECK(profile->avatar == "https://p1.music.126.net/a.jpg");
    CHECK(profile->background == "https://p1.music.126.net/b.jpg");
    CHECK(profile->signature == "签名");
    CHECK(profile->location == "Guangdong");
    CHECK(profile->level == 9);
    CHECK(profile->follows == 12);
    CHECK(profile->fans == 34567);
    CHECK(profile->posts == 8);
    CHECK(profile->playlists == 5);
    CHECK(profile->listened == 12345);
    CHECK(profile->days == 3001);
    CHECK(profile->vip);
    CHECK(profile->followed);
    CHECK_FALSE(profile->follows_me);
    lyren::set_language(lyren::Language::zh);
    CHECK(parse_profile(reply)->location == "广东");
    lyren::set_language(before);

    auto plain = parse_profile({.body = R"({"code":200,"level":0,"profile":{"userId":1,"nickname":"新人","province":999999}})"});
    REQUIRE(plain);
    CHECK(plain->location.empty());
    CHECK(plain->signature.empty());
    CHECK(plain->days == -1);
    CHECK_FALSE(plain->vip);
    CHECK_FALSE(plain->followed);
    CHECK_FALSE(parse_profile({.body = R"({"code":404,"profile":null})"}));
    CHECK_FALSE(parse_profile({.body = R"({"code":200,"profile":{"userId":0}})"}));
    CHECK_FALSE(parse_profile({.error = "Network connection failed"}));
}

TEST_CASE("NetEase follow lists page by offset and only fans carry a total") {
    auto follows = parse_people({.body = R"({"code":200,"more":true,"follow":[
        {"userId":9007199254740993,"nickname":"甲","avatarUrl":"http://p1.music.126.net/1.jpg","signature":" 你好 ","followeds":10,"followed":true,"mutual":true},
        {"userId":2,"nickname":"乙"}]})"},
                                false, 20);
    REQUIRE(follows);
    CHECK(follows->total == -1);
    CHECK(follows->more);
    CHECK(follows->cursor == "22");
    REQUIRE(follows->people.size() == 2);
    const auto &first = follows->people[0];
    CHECK(first.id == "9007199254740993");
    CHECK(first.name == "甲");
    CHECK(first.avatar == "https://p1.music.126.net/1.jpg");
    CHECK(first.signature == "你好");
    CHECK(first.fans == 10);
    CHECK(first.followed);
    CHECK(first.follows_me);
    CHECK(follows->people[1].fans == -1);
    CHECK_FALSE(follows->people[1].followed);
    CHECK_FALSE(follows->people[1].follows_me);

    auto fans = parse_people({.body = R"({"code":200,"more":false,"size":3,"followeds":[{"userId":3,"nickname":"丙"}]})"}, true, 0);
    REQUIRE(fans);
    CHECK(fans->total == 3);
    CHECK_FALSE(fans->more);
    CHECK(fans->cursor.empty());
    REQUIRE(fans->people.size() == 1);
    CHECK(fans->people[0].id == "3");

    // An empty page ends the list whatever the service says.
    auto end = parse_people({.body = R"({"code":200,"more":true,"follow":[]})"}, false, 40);
    REQUIRE(end);
    CHECK_FALSE(end->more);
    CHECK(end->cursor.empty());
    CHECK_FALSE(parse_people({.body = R"({"code":200,"followeds":[]})"}, false, 0));
    CHECK_FALSE(parse_people({.body = R"({"code":200,"follow":[]})"}, true, 0));
    CHECK_FALSE(parse_people({.error = "Network connection failed"}, true, 0));
}

TEST_CASE("NetEase posts carry their song, playlist, pictures and forwarded post") {
    // Each post's body is JSON in a string; a forwarded post's body is a string inside that one.
    auto page = parse_posts(
        {.body =
             R"({"code": 200, "more": true, "lasttime": 1700000000000, "events": [{"id": 9007199254740993, "type": 18, "eventTime": 1700000000123, "user": {"userId": 7, "nickname": "作者", "avatarUrl": "http://p1.music.126.net/u.jpg"}, "json": "{\"msg\": \"\\n#每日一听# 好听 @朋友 \", \"song\": {\"id\": 186016, \"name\": \"晴天\", \"artists\": [{\"id\": 6452, \"name\": \"周杰伦\"}], \"album\": {\"id\": 18905, \"name\": \"叶惠美\", \"picUrl\": \"http://p1.music.126.net/c.jpg\"}, \"duration\": 269000}}", "threadId": "A_EV_2_9007199254740993_7", "owner": true, "pics": [], "info": {"liked": true, "likedCount": 5, "commentCount": 2, "shareCount": 1, "commentThread": {"resourceInfo": {"name": "晴天"}}}},
        {"id": 2, "type": 13, "eventTime": 1, "user": {"userId": 8, "nickname": "乙"}, "json": "{\"msg\": \"\", \"playlist\": {\"id\": 3778678, \"name\": \"热歌榜\", \"coverImgUrl\": \"http://p1.music.126.net/p.jpg\", \"trackCount\": 200, \"creator\": {\"nickname\": \"网易云音乐\", \"userId\": 1}}}", "info": {"likedCount": 0}},
        {"id": 3, "type": 35, "eventTime": 2, "user": {"userId": 9, "nickname": "丙"}, "json": "{\"msg\": \"看图\"}", "pics": [{"originUrl": "http://p1.music.126.net/o.jpg", "width": 1080, "height": 1440}, {"originUrl": null, "width": 0, "height": 0}], "info": {"commentThread": {"resourceInfo": {"name": "看图"}}}},
        {"id": 4, "type": 22, "eventTime": 3, "user": {"userId": 10, "nickname": "丁"}, "json": "{\"msg\": \"转发\", \"event\": {\"id\": 5, \"type\": 18, \"eventTime\": 0, \"user\": {\"userId\": 11, \"nickname\": \"原作者\"}, \"json\": \"{\\\"msg\\\": \\\"原文\\\", \\\"song\\\": {\\\"id\\\": 1, \\\"name\\\": \\\"歌\\\"}}\"}}"},
        {"id": 6, "type": 21, "eventTime": 4, "user": {"userId": 12, "nickname": "戊"}, "json": "{\"msg\": \"\"}", "info": {"commentThread": {"resourceInfo": {"name": " 一段视频 "}}}},
        {"id": 7, "type": 35, "eventTime": 5, "user": {"userId": 13, "nickname": "己"}, "json": "not json"},
        {"id": 0, "type": 18}]})"});
    REQUIRE(page);
    CHECK(page->more);
    CHECK(page->cursor == "1700000000000");
    REQUIRE(page->posts.size() == 6);

    const auto &song = page->posts[0];
    CHECK(song.id == "9007199254740993");
    CHECK(song.thread == "9007199254740993_7");
    CHECK(song.author.id == "7");
    CHECK(song.author.name == "作者");
    CHECK(song.author.avatar == "https://p1.music.126.net/u.jpg");
    CHECK(song.text == "#每日一听# 好听 @朋友");
    CHECK(song.time == 1700000000123);
    REQUIRE(song.track);
    CHECK(song.track->id == "186016");
    CHECK(song.track->title == "晴天");
    CHECK(song.track->artist == "周杰伦");
    CHECK(song.track->cover == "https://p1.music.126.net/c.jpg");
    CHECK_FALSE(song.card);
    CHECK(song.resource.empty());
    CHECK(song.liked);
    CHECK(song.likes == 5);
    CHECK(song.comments == 2);
    CHECK(song.forwards == 1);
    CHECK(song.owned);

    const auto &playlist = page->posts[1];
    CHECK(playlist.thread == "2_8");
    CHECK(playlist.text.empty());
    REQUIRE(playlist.card);
    CHECK(playlist.card->kind == lyren::CardKind::playlist);
    CHECK(playlist.card->id == "3778678");
    CHECK(playlist.card->title == "热歌榜");
    CHECK(playlist.card->subtitle == "网易云音乐");
    CHECK(playlist.card->cover == "https://p1.music.126.net/p.jpg");
    CHECK(playlist.card->count == 200);
    CHECK_FALSE(playlist.liked);
    CHECK_FALSE(playlist.owned);

    const auto &pictures = page->posts[2];
    CHECK(pictures.text == "看图");
    REQUIRE(pictures.pictures.size() == 1);
    CHECK(pictures.pictures[0].url == "https://p1.music.126.net/o.jpg");
    CHECK(pictures.pictures[0].w == 1080);
    CHECK(pictures.pictures[0].h == 1440);
    CHECK(pictures.resource.empty());

    const auto &forward = page->posts[3];
    CHECK(forward.text == "转发");
    CHECK(forward.resource.empty());
    REQUIRE(forward.forwarded.size() == 1);
    const auto &original = forward.forwarded[0];
    CHECK(original.id == "5");
    CHECK(original.thread == "5_11");
    CHECK(original.author.name == "原作者");
    CHECK(original.text == "原文");
    REQUIRE(original.track);
    CHECK(original.track->title == "歌");
    CHECK(original.forwarded.empty());

    CHECK(page->posts[4].resource == "一段视频");
    CHECK(page->posts[5].text.empty());
    CHECK_FALSE(page->posts[5].track);

    // The feed lists its posts under another key.
    auto feed = parse_posts({.body = R"({"code":200,"more":false,"lasttime":-1,"event":[{"id":1,"type":35,"json":"{\"msg\":\"hi\"}"}]})"});
    REQUIRE(feed);
    CHECK_FALSE(feed->more);
    CHECK(feed->cursor.empty());
    REQUIRE(feed->posts.size() == 1);
    CHECK(feed->posts[0].text == "hi");
    auto end = parse_posts({.body = R"({"code":200,"more":true,"lasttime":5,"events":[]})"});
    REQUIRE(end);
    CHECK_FALSE(end->more);
    CHECK_FALSE(parse_posts({.body = R"({"code":200,"more":true,"events":[{"id":1}]})"}));
    CHECK_FALSE(parse_posts({.body = R"({"code":200})"}));
    CHECK_FALSE(parse_posts({.error = "Network connection failed"}));
}

TEST_CASE("NetEase listening rankings keep plays and scores, and report privacy") {
    const Reply reply{.body = R"({"code":200,"weekData":[
        {"playCount":12,"score":100,"song":{"id":1,"name":"甲","ar":[{"id":2,"name":"歌手"}],"al":{"id":3,"name":"专辑","picUrl":"http://p1.music.126.net/a.jpg"},"dt":200000}},
        {"playCount":0,"score":150,"song":{"id":4,"name":"乙"}},
        {"playCount":3,"score":-5,"song":{"id":5,"name":"丙"}},
        {"playCount":9,"score":50}]})"};
    auto week = parse_ranking(reply, false);
    REQUIRE(week);
    REQUIRE(week->tracks.size() == 3);
    CHECK(week->tracks[0].id == "1");
    CHECK(week->tracks[0].artist == "歌手");
    CHECK(week->tracks[0].cover == "https://p1.music.126.net/a.jpg");
    CHECK(week->tracks[0].duration == 200);
    CHECK(week->plays == std::vector<int>{12, 0, 3});
    CHECK(week->scores == std::vector<int>{100, 100, 0});
    CHECK_FALSE(parse_ranking(reply, true));
    auto all = parse_ranking({.body = R"({"code":200,"allData":[]})"}, true);
    REQUIRE(all);
    CHECK(all->tracks.empty());
    auto hidden = parse_ranking({.code = -2, .body = R"({"code":-2})", .error = "无权限访问"}, false);
    REQUIRE_FALSE(hidden);
    CHECK(hidden.error() == "This listening ranking is private");
    CHECK_FALSE(parse_ranking({.error = "Network connection failed"}, true));
}

TEST_CASE("NetEase social writes succeed only with code 200 and keep the service's words") {
    CHECK(parse_social_write({.body = R"({"code":200})"}));
    auto busy = parse_social_write({.body = R"({"code":400,"message":"操作太频繁"})"});
    REQUIRE_FALSE(busy);
    CHECK(busy.error() == "操作太频繁");
    auto self = parse_social_write({.body = R"({"code":502,"msg":"不能关注自己"})"});
    REQUIRE_FALSE(self);
    CHECK(self.error() == "不能关注自己");
    const auto before = lyren::language();
    lyren::set_language(lyren::Language::en);
    auto bare = parse_social_write({.body = R"({"code":201})"});
    REQUIRE_FALSE(bare);
    CHECK(bare.error() == "NetEase Music refused the request (201)");
    lyren::set_language(before);
    CHECK_FALSE(parse_social_write({.body = "{}"}));
    CHECK_FALSE(parse_social_write({.error = "Network connection failed"}));
}
