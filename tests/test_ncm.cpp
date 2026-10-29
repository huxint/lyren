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

TEST_CASE("rsa known vector") {
    // From YetEaseMusic crypto.rs, computed with Python's pow(m, 65537, n).
    CHECK(rsa_hex("aaaaaaaaaaaaaaaa") == "d473b9eca232f1b4090dd606b0df86de318748dd2eec307e4ed4345030fc4ee30"
                                         "331e598f41d5a6f5befaab94630ea1a1eda7cfade84fbec1a907913d2e4d2c87"
                                         "44bc572b99a050075e075b4537f645ecfa994f95906c32818e076aeda6bdb906b"
                                         "fa0bb96c4cf4bc3ed6d9ab76cf08441153d9d85e1ea3d78fa8d9210d581cee");
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
