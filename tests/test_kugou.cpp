#include <doctest/doctest.h>

#include "core/encoding.hpp"
#include "kugou/kugou.hpp"

using namespace lyren;

TEST_CASE("KuGou signatures cover raw parameter values and the exact JSON bytes") {
    kugou::Params query{{"dfid", "-"},          {"mid", "00000000000000000000000000000001"},
                        {"uuid", "-"},          {"appid", "1005"},
                        {"clientver", "20489"}, {"clienttime", "1700000000"},
                        {"type", "1"},          {"plat", "4"},
                        {"srcappid", "2919"},   {"qrcode_txt", "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&"}};
    CHECK(kugou::sign(query, {}, kugou::login_salt) == "2d59cbf72eb87de841f349e5c327d337");
    auto request = kugou::request("https://login-user.kugou.com/v2/qrcode", query, {}, kugou::login_salt);
    CHECK(request.url.contains("qrcode_txt=https%3A%2F%2Fh5.kugou.com%2Fapps%2FloginQRCode%2Fhtml%2Findex.html%3Fappid%3D1005%26"));
    CHECK(request.url.contains("signature=2d59cbf72eb87de841f349e5c327d337"));
    CHECK_FALSE(request.body);

    query.erase("type");
    query.erase("srcappid");
    query.erase("qrcode_txt");
    query["plat"] = "1";
    query["userid"] = "42";
    query["token"] = "synthetic-token";
    std::string body = R"({"userid":"42","token":"synthetic-token","total_ver":979,"type":2,"page":1,"pagesize":100})";
    CHECK(kugou::sign(query, body, kugou::android_salt) == "c1dffdcfa3b2b244f5016b344337d1fd");
    request = kugou::request("https://gateway.kugou.com/v7/get_all_list", query, body, kugou::android_salt);
    CHECK(request.body == body);
    CHECK(kugou::sign(query, body + '\n', kugou::android_salt) != "c1dffdcfa3b2b244f5016b344337d1fd");

    kugou::Params cdn{{"appid", "1058"},   {"clientver", "20000"}, {"module", "playlist"}, {"page", "2"},
                      {"pagesize", "100"}, {"specialid", "42"},    {"srcappid", "2919"},   {"type", "0"}};
    CHECK(kugou::sign(cdn, {}, kugou::login_salt) == "625e27b2f8e2170abd8acefdad6d8d76");
}

TEST_CASE("KuGou search preserves large IDs and advances a short service page") {
    auto page = kugou::parse_search(R"({"error_code":0,"data":{"total":70,"lists":[{
        "FileHash":"abcdefabcdefabcdefabcdefabcdefab","Audioid":9007199254740993,"MixSongID":"9007199254740995",
        "SongName":"测试曲","Singers":[{"id":3520,"name":"甲"},{"id":2,"name":"乙"}],
        "AlbumID":"42","AlbumName":"专辑","Duration":269,"PayType":3,
        "Image":"http://imge.kugou.com/stdmusic/{size}/cover.jpg"
    }]}})",
                                    SearchKind::tracks, 0, 30);
    REQUIRE(page);
    REQUIRE(page->tracks.size() == 1);
    CHECK(page->next_offset == 30);
    CHECK(page->tracks[0].source == Source::kugou);
    CHECK(page->tracks[0].id == "ABCDEFABCDEFABCDEFABCDEFABCDEFAB");
    CHECK(page->tracks[0].mid == "9007199254740993");
    CHECK(page->tracks[0].media == "9007199254740995");
    CHECK(page->tracks[0].artist == "甲 / 乙");
    CHECK(page->tracks[0].artist_id == "3520");
    CHECK(page->tracks[0].duration == 269);
    CHECK(page->tracks[0].paid);
    CHECK_FALSE(kugou::parse_search(R"({"error_code":20028,"data":{"total":0,"lists":[]}})", SearchKind::tracks, 0, 30));
    CHECK_FALSE(kugou::parse_search(R"({"error_code":0,"data":{"total":70,"lists":[]}})", SearchKind::tracks, 30, 30));
}

TEST_CASE("KuGou metadata preserves order and resolves retired hashes only in a unique single lookup") {
    const std::string body = R"({"error_code":0,"data":[[
        {"songname":"Second","author_name":"Artist","album_info":{"album_id":9007199254740993},
         "audio_info":{"hash":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB","audio_id":"11","timelength":"205500"}}
    ],[
        {"songname":"First","audio_info":{"hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","hash_320":"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC","audio_id":9007199254740995,"timelength":180000}}
    ]]})";
    std::vector<std::string> hashes{"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
    auto songs = kugou::parse_metadata(body, hashes);
    REQUIRE(songs);
    REQUIRE(songs->size() == 3);
    CHECK((*songs)[0].title == "First");
    CHECK((*songs)[0].mid == "9007199254740995");
    CHECK((*songs)[1].album_id == "9007199254740993");
    CHECK((*songs)[1].duration == 205);
    CHECK((*songs)[2].title == "First");
    hashes.push_back("DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD");
    CHECK_FALSE(kugou::parse_metadata(body, hashes));
    std::vector<std::string> retired{"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"};
    CHECK_FALSE(kugou::parse_metadata(body, retired));
    const auto resolved = kugou::parse_metadata(R"({"error_code":0,"data":[[
        {"songname":"New file","audio_info":{"hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","audio_id":"11"}}
    ]]})",
                                                retired);
    REQUIRE(resolved);
    CHECK(resolved->front().id == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(resolved->front().mid == "11");
    CHECK_FALSE(kugou::parse_metadata(R"({"error_code":0,"data":[[
        {"songname":"Unidentified file","audio_info":{"hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}
    ]]})",
                                      retired));
}

TEST_CASE("KuGou stream responses preserve preview offsets and reject unavailable audio") {
    const auto body = R"({"status":1,"url":["https://audio.example/preview.mp3"],"extName":"mp3","timeLength":269,
        "volume_gain":-2.5,"volume_peak":0.8,"hash_offset":{"start_ms":69600,"end_ms":129600}})";
    auto preview = kugou::parse_stream(body, true, "128");
    REQUIRE(preview);
    CHECK(preview->trial);
    CHECK(preview->trial_start == doctest::Approx(69.6));
    CHECK(preview->gain == doctest::Approx(-2.5));
    CHECK(preview->peak == doctest::Approx(0.8));
    auto full = kugou::parse_stream(body, false, "320");
    REQUIRE(full);
    CHECK_FALSE(full->trial);
    CHECK(full->trial_start == 0);
    CHECK(full->level == "exhigh");
    CHECK_FALSE(kugou::parse_stream(R"({"status":2,"priv_status":0})", false, "128"));
    CHECK_FALSE(kugou::parse_stream(R"({"status":1,"url":["https://audio.example/encrypted.kgg"],"extName":"kgg"})", false, "128"));
}

TEST_CASE("KuGou KRC decodes relative word timing and keeps translations with sorted lines") {
    // Independent zlib + XOR fixture: two out-of-order Chinese lines and a language payload.
    auto bytes = encoding::unbase64("a3JjMTjb6rkX/j9oHHp+mIR/mqMKdVNC1jhDcqH6G2XMIGM7Y9sTS+3AXzvNi7t+7W5kS027q+WssAbVozQC0L2Y/FUzFavlLRnGeiI6u+7lb1LKgmzr5HSPJmVzOmL/"
                                    "zL8DCvnl7HtSPgBL0HKdoE9cZ/ktJ5YTmQZvV5o6/VbwZ+lCWuMhsEqQvE/zXt4v1cKlcYw82wNxJXQ1zQFY");
    auto lyrics = kugou::parse_krc(bytes);
    REQUIRE(lyrics);
    REQUIRE(lyrics->size() == 2);
    CHECK((*lyrics)[0].time == 1.0);
    CHECK((*lyrics)[0].text == "你好");
    CHECK((*lyrics)[0].translation == "Hello");
    REQUIRE((*lyrics)[0].words.size() == 2);
    CHECK((*lyrics)[0].words[1].start_ms == 1200);
    CHECK((*lyrics)[0].words[1].from == 3);
    CHECK((*lyrics)[0].words[1].to == 6);
    CHECK((*lyrics)[1].translation == "World");
    CHECK((*lyrics)[1].words[1].start_ms == 2400);
    CHECK_FALSE(kugou::parse_krc("krc1invalid"));
    auto lrc = kugou::parse_lyric_download(R"({"fmt":"lrc","content":"WzAwOjAwLjAwXea1i+ivleatjOivjQo="})");
    REQUIRE(lrc);
    REQUIRE(lrc->size() == 1);
    CHECK(lrc->front().text == "测试歌词");
}
