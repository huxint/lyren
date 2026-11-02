#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <fstream>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"
#include "http_server.hpp"
#include "qishui/media.hpp"
#include "qishui/qishui.hpp"

using namespace lyren;
using qishui::Json;

namespace {
    template <class T>
    Result<T> run(Loop &loop, Task<Result<T>> task) {
        std::optional<Result<T>> result;
        const auto timeout = loop.after(5s, [&] {
            loop.quit();
        });
        spawn([](Loop &loop, Task<Result<T>> task, std::optional<Result<T>> &result) -> Task<void> {
            result = co_await std::move(task);
            loop.quit();
        }(loop, std::move(task), result));
        if (!result) {
            loop.run();
        }
        loop.cancel(timeout);
        REQUIRE(result);
        return std::move(*result);
    }

    std::string_view body(std::string_view request) {
        return request.substr(request.find("\r\n\r\n") + 4);
    }

    Json json(std::string_view text) {
        Json value;
        REQUIRE_FALSE(glz::read_json(value, text));
        return value;
    }

    struct Context {
        Loop loop;
        net::Http http;
        std::move_only_function<void(const Json &)> check_signature;
        test::Server signer{[this](std::string_view request) {
            CHECK(request.contains("Authorization: Bearer test-signer-token\r\n"));
            if (request.starts_with("GET /config ")) {
                return test::ok(R"({"ok":true,"identity":{"device_id":"test-device","iid":"test-install","fp":"test-fingerprint"}})");
            }
            CHECK(request.starts_with("POST /sign "));
            if (check_signature) {
                check_signature(json(body(request)));
            }
            return test::ok(R"({"ok":true,"headers":{"X-Helios":"test-helios","X-MEDUSA":"test-medusa"}})");
        }};
        qishui::Qishui service{loop, http};

        explicit Context(const std::string &base) {
            service.client.api_base = base;
            service.session() = {
                {"signer_url", signer.url()}, {"signer_token", "test-signer-token"}, {"user_id", "7"}, {"cookie_jar", "127.0.0.1\tFALSE\t/\tFALSE\t0\tsessionid\told\n"}};
            REQUIRE(run(loop, service.initialize()));
        }

        std::string connection() const {
            return glz::write_json(glz::obj{"signer_url", signer.url(), "signer_token", "test-signer-token", "cookie", "sessionid=new; sid_tt=x=y"}).value();
        }
    };
} // namespace

TEST_CASE("Qishui signs the exact URL, Cookie and body that reach the gateway") {
    const std::string payload = "{\"text\":\"音乐\", \"ids\":[\"9007199254740993\"]}\n";
    std::promise<Json> signed_request;
    auto signature = signed_request.get_future();
    test::Server api([&](std::string_view request) {
        auto signed_value = signature.get();
        const auto url = qishui::string(qishui::field(signed_value, "url"));
        CHECK(request.starts_with("POST " + url.substr(url.find('/', 8)) + " HTTP/1.1\r\n"));
        CHECK(body(request) == payload);
        CHECK(request.contains("cookie: sessionid=old\r\n"));
        CHECK(request.contains("x-helios: test-helios\r\n"));
        CHECK(request.contains("x-medusa: test-medusa\r\n"));
        CHECK_FALSE(request.contains("test-signer-token"));
        auto response = test::ok(R"({"status_code":0})");
        response.insert(response.find("\r\n") + 2, "Set-Cookie: sessionid=renewed; Path=/; HttpOnly\r\n");
        return response;
    });
    Context c(api.url());
    c.check_signature = [&](const Json &value) {
        CHECK(qishui::string(qishui::field(value, "method")) == "POST");
        CHECK(qishui::string(qishui::field(value, "body")) == payload);
        const auto &headers = qishui::field(value, "headers");
        CHECK(qishui::string(qishui::field(headers, "cookie")) == "sessionid=old");
        CHECK(qishui::string(qishui::field(headers, "x-ss-stub")) == "C7E0576C535E599BCDBFE3D546C43041");
        const auto url = qishui::string(qishui::field(value, "url"));
        CHECK(url.contains("item_types=album&item_types=playlist&q=%E9%9F%B3%E4%B9%90%20%26"));
        CHECK(url.contains("device_id=test-device"));
        signed_request.set_value(value);
    };
    REQUIRE(run(c.loop, c.service.client.call("/luna/pc/test", {{"item_types", "album"}, {"item_types", "playlist"}, {"q", "音乐 &"}}, payload, qishui::Operation::write)));
    CHECK(net::CookieJar::from_netscape(c.service.session().at("cookie_jar")).get("sessionid", "127.0.0.1") == "renewed");
}

TEST_CASE("Qishui connection import validates before replacing a session and can be cancelled") {
    std::atomic<bool> delay = false;
    test::Server api([&](std::string_view request) {
        CHECK(request.starts_with("GET /luna/pc/me?"));
        if (delay) {
            std::this_thread::sleep_for(30ms);
        }
        return test::ok(R"({"my_info":{"id":"9007199254740993","nickname":"测试账号"}})");
    });
    Context c(api.url());
    const auto original = c.service.session();
    CHECK_FALSE(run(c.loop, c.service.import_session(R"({"cookie":"incomplete"})")));
    CHECK(c.service.session() == original);
    delay = true;
    c.loop.after(10ms, [&] {
        c.service.cancel_login();
    });
    CHECK_FALSE(run(c.loop, c.service.import_session(c.connection())));
    CHECK(c.service.session() == original);
    delay = false;
    REQUIRE(run(c.loop, c.service.import_session(c.connection())));
    CHECK(c.service.logged_in());
    CHECK(c.service.session().at("user_id") == "9007199254740993");
    const auto cookies = net::CookieJar::from_netscape(c.service.session().at("cookie_jar"));
    CHECK(cookies.get("sessionid", "api.qishui.com") == "new");
    CHECK(cookies.get("sid_tt", "api.qishui.com") == "x=y");
    CHECK(cookies.header("other.example").empty());
}

TEST_CASE("Qishui search uses group cursors and playlist writes retain string IDs") {
    std::atomic<int> searches = 0, writes = 0;
    test::Server api([&](std::string_view request) {
        if (request.starts_with("GET /luna/pc/search/track?")) {
            CHECK_FALSE(request.contains("&count="));
            if (++searches == 1) {
                return test::ok(
                    R"({"result_groups":[{"id":"albums","next_cursor":"wrong","has_more":true},{"id":"tracks","has_more":true,"next_cursor":"next/页","data":[{"entity":{"track":{"id":9007199254740993,"name":"歌曲","duration":240000,"artists":[{"id":"a","name":"歌手"}],"album":{"id":"b","name":"专辑"}}}}]}]})");
            }
            CHECK(request.contains("cursor=next%2F%E9%A1%B5"));
            return test::ok(R"({"result_groups":[{"id":"tracks","has_more":false,"next_cursor":"","data":[]}]})");
        }
        CHECK(request.starts_with("POST /luna/me/playlist/media/sort?"));
        const auto value = json(body(request));
        CHECK(qishui::string(qishui::field(value, "playlist_id")) == "9007199254740995");
        const auto rows = qishui::array(qishui::field(value, "media"));
        REQUIRE(rows.size() == 2);
        CHECK(qishui::field(rows[0], "id").is_string());
        CHECK(qishui::string(qishui::field(rows[0], "id")) == "18446744073709551615");
        CHECK(qishui::string(qishui::field(rows[1], "id")) == "9007199254740993");
        ++writes;
        return test::ok(R"({"status_info":{"status_msg":""}})");
    });
    Context c(api.url());
    auto first = run(c.loop, c.service.search("歌", SearchKind::tracks, 0, 30));
    REQUIRE(first);
    REQUIRE(first->tracks.size() == 1);
    CHECK(first->tracks[0].id == "9007199254740993");
    CHECK(first->tracks[0].duration == 240);
    CHECK(first->next_offset == 1);
    auto second = run(c.loop, c.service.search("歌", SearchKind::tracks, first->next_offset, 30));
    REQUIRE(second);
    CHECK(second->next_offset == -1);
    CHECK(searches == 2);
    REQUIRE(run(c.loop, c.service.reorder_playlist("9007199254740995", {{.id = "18446744073709551615"}, {.id = "9007199254740993"}})));
    CHECK(writes == 1);
}

TEST_CASE("Qishui playlist batches sign all media and never replay a lost response") {
    bool add = true, lost = false;
    SUBCASE("append") {}
    SUBCASE("remove") {
        add = false;
    }
    SUBCASE("lost response") {
        lost = true;
    }
    std::atomic<int> writes = 0;
    test::Server api([&](std::string_view request) {
        CHECK(request.starts_with(add ? "POST /luna/pc/me/playlist/media/append?" : "POST /luna/pc/me/playlist/media/delete?"));
        auto value = json(body(request));
        CHECK(qishui::string(qishui::field(value, "playlist_id")) == "9007199254740995");
        auto media = qishui::array(qishui::field(value, "media"));
        REQUIRE(media.size() == 2);
        CHECK(qishui::field(media[0], "id").is_string());
        CHECK(qishui::string(qishui::field(media[0], "id")) == "9007199254740993");
        CHECK(qishui::string(qishui::field(media[1], "id")) == "18446744073709551615");
        CHECK(qishui::string(qishui::field(media[0], "type")) == "track");
        CHECK(qishui::string(qishui::field(media[1], "type")) == "track");
        ++writes;
        return lost ? std::string{} : test::ok(R"({"status_info":{"status_msg":""}})");
    });
    Context c(api.url());
    c.check_signature = [&](const Json &value) {
        auto payload = json(qishui::string(qishui::field(value, "body")));
        CHECK(qishui::array(qishui::field(payload, "media")).size() == 2);
    };
    std::vector<Track> tracks{{.id = "9007199254740993", .source = Source::qishui}, {.id = "18446744073709551615", .source = Source::qishui}};
    auto result = run(c.loop, c.service.edit_playlist("9007199254740995", std::move(tracks), add));
    CHECK(result.has_value() == !lost);
    CHECK(writes == 1);
}

TEST_CASE("Qishui comment replies keep their root and uncertain writes are not repeated") {
    std::atomic<int> actions = 0;
    test::Server api([&](std::string_view request) {
        const auto value = json(body(request));
        CHECK(qishui::string(qishui::field(value, "comment_id")) == "root");
        CHECK(qishui::string(qishui::field(value, "reply_id")) == "reply");
        CHECK(qishui::number(qishui::field(value, "type")) == 3);
        ++actions;
        return std::string{};
    });
    Context c(api.url());
    const auto response = json(
        R"({"count":5,"has_more":true,"cursor":"reply-page","reply_infos":[{"id":"reply","content":"回复","user":{"id":"7","nickname":"我"},"count_digged":2,"user_digged":true}]})");
    auto replies = qishui::parse_comments(response, "7", "root");
    REQUIRE(replies);
    REQUIRE(replies->comments.size() == 1);
    CHECK(replies->comments[0].id == "root:reply");
    CHECK(replies->comments[0].owned);
    CHECK(replies->comments[0].liked);
    CHECK(replies->more);
    CHECK(replies->cursor == "reply-page");
    CHECK_FALSE(run(c.loop, c.service.like_comment(Subject::song, "song", replies->comments[0].id, false)));
    CHECK(actions == 1);
}

TEST_CASE("Qishui library merges private and saved playlists and reuses the likes identity") {
    std::atomic<int> catalogues = 0;
    test::Server api([&](std::string_view request) {
        if (request.starts_with("GET /luna/pc/me/playlist?")) {
            ++catalogues;
            return test::ok(R"({"playlists":[{"id":"likes","title":"喜欢","type":1},{"id":"mine","title":"私密歌单","is_private":true}],"has_more":false})");
        }
        if (request.starts_with("GET /luna/me/collection/mixed?")) {
            CHECK(request.contains("item_types=playlist"));
            ++catalogues;
            return test::ok(R"({"mixed_collections":[{"playlist":{"id":"saved","title":"收藏歌单"}},{"playlist":{"id":"mine","title":"重复项"}}],"has_more":false})");
        }
        CHECK(request.starts_with("GET /luna/pc/playlist/detail?"));
        CHECK(request.contains("playlist_id=likes"));
        return test::ok(R"({"media_resources":[{"type":"track","entity":{"track_wrapper":{"track":{"id":"9007199254740993","name":"喜欢的歌"}}}}],"has_more":false})");
    });
    Context c(api.url());
    const auto library = run(c.loop, c.service.user_playlists("7"));
    REQUIRE(library);
    CHECK(library->first.id == "likes");
    CHECK_FALSE(library->first.owned);
    REQUIRE(library->second.size() == 2);
    CHECK(library->second[0].id == "mine");
    CHECK(library->second[0].owned);
    CHECK_FALSE(library->second[0].commentable);
    CHECK(library->second[1].id == "saved");
    CHECK_FALSE(library->second[1].owned);
    const auto ids = run(c.loop, c.service.liked_ids("7"));
    REQUIRE(ids);
    CHECK(*ids == std::vector<std::string>{"9007199254740993"});
    CHECK(catalogues == 2);
}

TEST_CASE("Qishui session expiry and verification decisions remain visible") {
    test::Server api([](std::string_view request) {
        if (request.starts_with("GET /verify?")) {
            auto response = test::ok(R"({"status_code":0})");
            response.insert(response.find("\r\n") + 2, "BdTuring-Verify: synthetic-decision\r\n");
            return response;
        }
        return test::ok(R"({"status_code":1000016,"status_info":{"status_msg":"expired"}})");
    });
    Context c(api.url());
    auto verification = run(c.loop, c.service.client.call("/verify"));
    REQUIRE_FALSE(verification);
    CHECK(verification.error().contains("verification"));
    CHECK(c.service.logged_in());
    CHECK_FALSE(run(c.loop, c.service.account()));
    CHECK_FALSE(c.service.logged_in());
    CHECK_FALSE(c.service.session().contains("cookie_jar"));
    CHECK(c.service.session().contains("signer_url"));
}

TEST_CASE("Qishui media selects full audio, decodes stream keys and retains word timing") {
    constexpr std::string_view auth = "nb0T+1u8EvVZvBL3X7sR9UG4DvdfpybeXacm3FugJd6loioycGFk";
    const crypto::Aes128Key expected{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    auto key = qishui::stream_key(auth);
    REQUIRE(key);
    CHECK(*key == expected);
    CHECK_FALSE(qishui::stream_key("bad"));
    auto response = json(
        R"({"track_player":{"video_model":{"video_list":[{"main_url":"https://audio.example/preview","quality":"lossless","duration":30},{"main_url":"https://audio.example/full?auth=keep","quality":"highest","duration":240}]}}})");
    response["track_player"]["video_model"]["encrypt_info"]["spade_a"] = auth;
    auto stream = qishui::parse_stream(response, Quality::lossless, 240);
    REQUIRE(stream);
    CHECK(stream->url == "https://audio.example/full?auth=keep");
    CHECK_FALSE(stream->trial);
    REQUIRE(stream->decrypt_key);
    CHECK(*stream->decrypt_key == expected);
    auto model = qishui::parse_stream(
        json(
            R"({"track_player":{"video_model":{"video_duration":240,"video_list":[{"main_url":"https://audio.example/high","video_meta":{"quality":"highest","bitrate":257475}},{"main_url":"https://audio.example/low","video_meta":{"quality":"medium","bitrate":65437}}]}}})"),
        Quality::standard, 240);
    REQUIRE(model);
    CHECK(model->url == "https://audio.example/low");
    CHECK(model->level == "medium");
    auto preview = qishui::parse_stream(
        json(R"({"Result":{"Data":{"PlayInfoList":[{"MainPlayUrl":"","BackupPlayUrl":"https://audio.example/clip","Quality":"medium","Duration":30}]}}})"), Quality::hires, 240);
    REQUIRE(preview);
    CHECK(preview->trial);
    CHECK(preview->url == "https://audio.example/clip");
    CHECK_FALSE(preview->decrypt_key);
    auto lyrics = qishui::parse_lyrics("[12500,3000]示例<12500,500>歌<13000,500>词\n[00:20.00]下一行");
    REQUIRE(lyrics.size() == 2);
    CHECK(lyrics[0].time == 12.5);
    CHECK(lyrics[0].text == "示例歌词");
    REQUIRE(lyrics[0].words.size() == 2);
    CHECK(lyrics[0].words[0].start_ms == 12500);
    CHECK(lyrics[0].words[0].from == 6);
    CHECK(lyrics[0].words[1].to == 12);
    auto krc = qishui::parse_lyrics("[14690,6530]<0,210,0>故<210,200,0>事", "krc");
    REQUIRE(krc.size() == 1);
    CHECK(krc[0].text == "故事");
    REQUIRE(krc[0].words.size() == 2);
    CHECK(krc[0].words[0].start_ms == 14690);
    CHECK(krc[0].words[1].start_ms == 14900);
}

TEST_CASE("Qishui anonymous search keeps Android parameters separate from signed PC requests") {
    std::atomic<int> requests = 0;
    test::Server api([&](std::string_view request) {
        CHECK(request.starts_with("GET /luna/search/track?"));
        CHECK(request.contains("device_platform=android"));
        CHECK(request.contains("aid=386088"));
        CHECK(request.contains("count=30"));
        CHECK(request.contains("User-Agent: com.luna.music/100198030"));
        CHECK_FALSE(request.contains("x-helios:"));
        CHECK_FALSE(request.contains("app_name=luna_pc"));
        CHECK(request.contains(++requests == 1 ? "cursor=0&" : "cursor=30&"));
        return test::ok(requests == 1 ? R"({"result_groups":[{"id":"tracks","has_more":true,"next_cursor":"30","data":[{"entity":{"track":{"id":"1","name":"歌曲"}}}]}]})"
                                      : R"({"result_groups":[{"id":"tracks","has_more":false,"next_cursor":"","data":[]}]})");
    });
    Loop loop;
    net::Http http;
    qishui::Qishui service(loop, http);
    service.client.api_base = api.url();
    REQUIRE(run(loop, service.initialize()));
    const auto device = service.session().at("search_device_id");
    CHECK(device != service.session().at("search_iid"));
    auto first = run(loop, service.search("歌", SearchKind::tracks, 0, 30));
    REQUIRE(first);
    CHECK(first->next_offset == 1);
    auto second = run(loop, service.search("歌", SearchKind::tracks, first->next_offset, 30));
    REQUIRE(second);
    CHECK(second->next_offset == -1);
    CHECK(service.session().at("search_device_id") == device);
    CHECK(requests == 2);
}

TEST_CASE("Qishui public discovery reads nested playlists and deduplicates the recommendation feed") {
    std::atomic<int> batches = 0;
    test::Server api([&](std::string_view request) {
        CHECK_FALSE(request.contains("x-helios:"));
        if (request.starts_with("POST /luna/pc/discover?")) {
            return test::ok(R"({"blocks":[{"inner_block":[{"resources":[{"entity":{"playlist":{"id":"nested","title":"推荐歌单"}}}]}]}]})");
        }
        CHECK(request.starts_with("POST /luna/feed/playlist-square?"));
        const auto value = json(body(request));
        CHECK(value.get_object().size() == 1);
        CHECK(qishui::number(qishui::field(value, "category_id")) == 0);
        if (++batches == 1) {
            return test::ok(R"({"has_more":true,"items":[{"entity":{"playlist":{"id":"a","title":"歌单甲"}}},{"entity":{"playlist":{"id":"b","title":"歌单乙"}}}]})");
        }
        return test::ok(R"({"has_more":true,"items":[{"entity":{"playlist":{"id":"b","title":"歌单乙"}}},{"entity":{"playlist":{"id":"c","title":"歌单丙"}}}]})");
    });
    Loop loop;
    net::Http http;
    qishui::Qishui service(loop, http);
    service.client.api_base = api.url();
    REQUIRE(run(loop, service.initialize()));
    const auto recommended = run(loop, service.recommended_playlists());
    REQUIRE(recommended);
    REQUIRE(recommended->size() == 1);
    CHECK(recommended->front().id == "nested");
    auto page = run(loop, service.square("", 0));
    REQUIRE(page);
    CHECK(page->cards.size() == 2);
    CHECK(page->next_offset == 1);
    page = run(loop, service.square("", page->next_offset));
    REQUIRE(page);
    REQUIRE(page->cards.size() == 1);
    CHECK(page->cards.front().id == "c");
    page = run(loop, service.square("", page->next_offset));
    REQUIRE(page);
    CHECK(page->cards.empty());
    CHECK(page->next_offset == -1);
    CHECK(page->total == 3);
}

TEST_CASE("Qishui SEO playback follows player info and passes its key to the streaming decoder") {
    std::ifstream file(LYREN_TEST_DATA "/cenc.m4a", std::ios::binary);
    const std::string media_bytes{std::istreambuf_iterator<char>(file), {}};
    REQUIRE_FALSE(media_bytes.empty());
    std::string base;
    std::atomic<int> metadata_reads = 0;
    test::Server api([&](std::string_view request) {
        if (request.starts_with("GET /luna/h5/seo_track?")) {
            ++metadata_reads;
            CHECK(request.contains("track_id=123"));
            CHECK(request.contains("device_platform=web"));
            Json response = json(R"({"seo_track":{"track":{"id":"123","duration":1000}},"lyric":{"content":"[00:00.00]音乐"},"track_player":{}})");
            response["track_player"]["url_player_info"] = base + "/player";
            return test::ok(glz::write_json(response).value());
        }
        if (request.starts_with("GET /player ")) {
            Json response = json(
                R"({"Result":{"Data":{"PlayInfoList":[{"MainPlayUrl":"","Duration":1,"Quality":"highest","PlayAuth":"nb0T+1u8EvVZvBL3X7sR9UG4DvdfpybeXacm3FugJd6loioycGFk"}]}}})");
            response["Result"]["Data"]["PlayInfoList"].get_array()[0]["MainPlayUrl"] = base + "/audio";
            return test::ok(glz::write_json(response).value());
        }
        CHECK(request.starts_with("GET /audio "));
        return test::ok(media_bytes);
    });
    base = api.url();
    Loop loop;
    net::Http http;
    qishui::Qishui service(loop, http);
    service.client.seo_base = base;
    REQUIRE(run(loop, service.initialize()));
    const Track track{.id = "123", .source = Source::qishui};
    auto stream = run(loop, service.stream(track, Quality::exhigh));
    REQUIRE(stream);
    REQUIRE(stream->decrypt_key);
    CHECK(stream->url == base + "/audio");
    auto spool = audio::Spool::url(http, stream->url);
    std::string error;
    auto decoder = audio::open_decoder(*spool, error, stream->decrypt_key);
    REQUIRE_MESSAGE(decoder, error);
    CHECK(decoder->format == audio::Format{22050, 2});
    std::array<float, 2048> samples;
    CHECK(decoder->read(samples.data(), 1024) == 1024);
    const auto lyrics = run(loop, service.lyrics(track));
    REQUIRE(lyrics);
    REQUIRE(lyrics->size() == 1);
    CHECK(lyrics->front().text == "音乐");
    CHECK(metadata_reads == 1);
}
