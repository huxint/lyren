#include <doctest/doctest.h>
#include "language_guard.hpp"

#include <atomic>
#include <future>
#include <fstream>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"
#include "core/i18n.hpp"
#include "async.hpp"
#include "http_server.hpp"
#include "qishui/media.hpp"
#include "qishui/qishui.hpp"

using namespace lyren;
using qishui::Json;

namespace {
    using test::run;

    std::string_view body(std::string_view request) {
        return request.substr(request.find("\r\n\r\n") + 4);
    }

    Json parse_json(std::string_view text) {
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
                check_signature(parse_json(body(request)));
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
        const auto url = lyren::json::text(signed_value, "url");
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
        CHECK(lyren::json::text(value, "method") == "POST");
        CHECK(lyren::json::text(value, "body") == payload);
        const auto &headers = lyren::json::field(value, "headers");
        CHECK(lyren::json::text(headers, "cookie") == "sessionid=old");
        CHECK(lyren::json::text(headers, "x-ss-stub") == "C7E0576C535E599BCDBFE3D546C43041");
        const auto url = lyren::json::text(value, "url");
        CHECK(url.contains("item_types=album&item_types=playlist&q=%E9%9F%B3%E4%B9%90%20%26"));
        CHECK(url.contains("device_id=test-device"));
        signed_request.set_value(value);
    };
    REQUIRE(run(c.loop, c.service.client.call("/luna/pc/test", {{"item_types", "album"}, {"item_types", "playlist"}, {"q", "音乐 &"}}, payload, qishui::Operation::write)));
    CHECK(net::CookieJar::from_netscape(c.service.session().at("cookie_jar")).get("sessionid", "127.0.0.1") == "renewed");
}

TEST_CASE("Qishui connection import validates before replacing a session and can be cancelled") {
    Context c("");
    std::atomic<bool> cancel = false;
    test::Server api([&](std::string_view request) {
        CHECK(request.starts_with("GET /luna/pc/me?"));
        if (cancel.exchange(false)) {
            c.loop.post([&] {
                c.service.cancel_login();
            });
        }
        return test::ok(R"({"my_info":{"id":"9007199254740993","nickname":"测试账号"}})");
    });
    c.service.client.api_base = api.url();
    const auto original = c.service.session();
    CHECK_FALSE(run(c.loop, c.service.import_session(R"({"cookie":"incomplete"})")));
    CHECK(c.service.session() == original);
    cancel = true;
    CHECK_FALSE(run(c.loop, c.service.import_session(c.connection())));
    CHECK(c.service.session() == original);
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
        const auto value = parse_json(body(request));
        CHECK(lyren::json::text(value, "playlist_id") == "9007199254740995");
        const auto rows = lyren::json::array(lyren::json::field(value, "media"));
        REQUIRE(rows.size() == 2);
        CHECK(lyren::json::field(rows[0], "id").is_string());
        CHECK(lyren::json::text(rows[0], "id") == "18446744073709551615");
        CHECK(lyren::json::text(rows[1], "id") == "9007199254740993");
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
        auto value = parse_json(body(request));
        CHECK(lyren::json::text(value, "playlist_id") == "9007199254740995");
        auto media = lyren::json::array(lyren::json::field(value, "media"));
        REQUIRE(media.size() == 2);
        CHECK(lyren::json::field(media[0], "id").is_string());
        CHECK(lyren::json::text(media[0], "id") == "9007199254740993");
        CHECK(lyren::json::text(media[1], "id") == "18446744073709551615");
        CHECK(lyren::json::text(media[0], "type") == "track");
        CHECK(lyren::json::text(media[1], "type") == "track");
        ++writes;
        return lost ? std::string{} : test::ok(R"({"status_info":{"status_msg":""}})");
    });
    Context c(api.url());
    c.check_signature = [&](const Json &value) {
        auto payload = parse_json(lyren::json::text(value, "body"));
        CHECK(lyren::json::array(lyren::json::field(payload, "media")).size() == 2);
    };
    std::vector<Track> tracks{{.id = "9007199254740993", .source = Source::qishui}, {.id = "18446744073709551615", .source = Source::qishui}};
    auto result = run(c.loop, c.service.edit_playlist("9007199254740995", std::move(tracks), add));
    CHECK(result.has_value() == !lost);
    CHECK(writes == 1);
}

TEST_CASE("Qishui comment replies keep their root and uncertain writes are not repeated") {
    std::atomic<int> actions = 0;
    test::Server api([&](std::string_view request) {
        const auto value = parse_json(body(request));
        CHECK(lyren::json::text(value, "comment_id") == "root");
        CHECK(lyren::json::text(value, "reply_id") == "reply");
        CHECK(lyren::json::number(lyren::json::field(value, "type")) == 3);
        ++actions;
        return std::string{};
    });
    Context c(api.url());
    const auto response = parse_json(
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
        auto response = test::ok(R"({"status_code":1000016,"status_info":{"status_msg":"expired"}})");
        response.insert(response.find("\r\n") + 2, "Set-Cookie: sessionid=; Max-Age=0; Path=/\r\n");
        return response;
    });
    Context c(api.url());
    auto verification = run(c.loop, c.service.client.call("/verify"));
    REQUIRE_FALSE(verification);
    CHECK(verification.error().contains("verification"));
    CHECK(c.service.logged_in());
    int changes = 0;
    c.service.on_session_changed([&] {
        ++changes;
    });
    CHECK_FALSE(run(c.loop, c.service.account()));
    CHECK(changes == 1);
    CHECK_FALSE(c.service.session().contains("user_id"));
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
    auto response = parse_json(
        R"({"track_player":{"video_model":{"video_list":[{"main_url":"https://audio.example/preview","quality":"lossless","duration":30},{"main_url":"https://audio.example/full?auth=keep","quality":"highest","duration":240}]}}})");
    response["track_player"]["video_model"]["encrypt_info"]["spade_a"] = auth;
    auto stream = qishui::parse_stream(response, Quality::lossless, 240);
    REQUIRE(stream);
    CHECK(stream->url == "https://audio.example/full?auth=keep");
    CHECK_FALSE(stream->trial);
    REQUIRE(stream->decrypt_key);
    CHECK(*stream->decrypt_key == expected);
    auto model = qishui::parse_stream(
        parse_json(
            R"({"track_player":{"video_model":{"video_duration":240,"video_list":[{"main_url":"https://audio.example/high","video_meta":{"quality":"highest","bitrate":257475}},{"main_url":"https://audio.example/low","video_meta":{"quality":"medium","bitrate":65437}}]}}})"),
        Quality::standard, 240);
    REQUIRE(model);
    CHECK(model->url == "https://audio.example/low");
    CHECK(model->level == "medium");
    auto preview = qishui::parse_stream(
        parse_json(R"({"Result":{"Data":{"PlayInfoList":[{"MainPlayUrl":"","BackupPlayUrl":"https://audio.example/clip","Quality":"medium","Duration":30}]}}})"), Quality::hires,
        240);
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
        const auto value = parse_json(body(request));
        CHECK(value.get_object().size() == 1);
        CHECK(lyren::json::number(lyren::json::field(value, "category_id")) == 0);
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
            Json response = parse_json(R"({"seo_track":{"track":{"id":"123","duration":1000}},"lyric":{"content":"[00:00.00]音乐"},"track_player":{}})");
            response["track_player"]["url_player_info"] = base + "/player";
            return test::ok(glz::write_json(response).value());
        }
        if (request.starts_with("GET /player ")) {
            Json response = parse_json(
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

TEST_CASE("Qishui profiles, other user playlists, and comment user IDs") {
    test::Server api([](std::string_view request) {
        if (request.starts_with("GET /luna/pc/me?")) {
            return test::ok(
                R"({"my_info":{"id":"7","nickname":"自己","larger_avatar_url":{"urls":["https://example/me"],"uri":"a.jpg"},"bg_url":{"urls":["https://example/bg"],"uri":"b.jpg"},"signature":"晚安","count_following":15,"count_follower":99,"is_vip":true}})");
        }
        if (request.starts_with("GET /luna/pc/user/playlist?")) {
            CHECK(request.contains("user_id=99"));
            return test::ok(R"({"playlists":[{"id":"l99","title":"Ta喜欢的","type":1},{"id":"p99","title":"夜间歌单","type":0}],"has_more":false})");
        }
        if (request.starts_with("GET /luna/pc/comments?")) {
            return test::ok(R"({"comments":[{"id":"c1","content":"好听","user":{"id":"99","nickname":"听友"},"count_digged":2}],"has_more":false,"count":1})");
        }
        return test::ok(R"({})");
    });
    Context c(api.url());
    CHECK(c.service.capabilities.profiles);
    CHECK(c.service.capabilities.profile_playlists);

    const auto self_profile = run(c.loop, c.service.profile("7"));
    REQUIRE(self_profile);
    CHECK(self_profile->name == "自己");
    CHECK(self_profile->signature == "晚安");
    CHECK(self_profile->follows == 15);
    CHECK(self_profile->fans == 99);
    CHECK(self_profile->vip);

    const auto other_profile = run(c.loop, c.service.profile("99"));
    REQUIRE(other_profile);
    CHECK(other_profile->id == "99");

    const auto other_lists = run(c.loop, c.service.user_playlists("99"));
    REQUIRE(other_lists);
    CHECK(other_lists->first.id == "l99");
    REQUIRE(other_lists->second.size() == 1);
    CHECK(other_lists->second[0].id == "p99");
    CHECK(other_lists->second[0].owned);

    const auto page = run(c.loop, c.service.comments(Subject::song, "song", true, 0, ""));
    REQUIRE(page);
    REQUIRE(page->comments.size() == 1);
    CHECK(page->comments[0].user_id == "99");
}

TEST_CASE("Qishui lenient cookie parsing, signer error attribution, and connection diagnostics") {
    SUBCASE("lenient cookie formats and Netscape cookie_jar") {
        auto trailing = qishui::parse_cookie_input("  Cookie: sessionid=abc; sid_tt=123; ;  \r\n");
        REQUIRE(trailing);
        const auto jar = net::CookieJar::from_netscape(*trailing);
        CHECK(jar.get("sessionid", "api.qishui.com") == "abc");
        CHECK(jar.get("sessionid", "www.qishui.com") == "abc");
        CHECK(jar.get("sid_tt", "qishui.com") == "123");
        CHECK(jar.header("other.example").empty());

        auto netscape = qishui::parse_cookie_input("# Netscape HTTP Cookie File\n.qishui.com\tTRUE\t/\tTRUE\t0\tsessionid\tjar_tok\n");
        REQUIRE(netscape);
        CHECK(net::CookieJar::from_netscape(*netscape).get("sessionid", "api.qishui.com") == "jar_tok");
    }

    SUBCASE("signer HTTP errors point to the signing service and check_session reports step-by-step status") {
        std::atomic<int> signer_mode{404};
        test::Server bad_signer([&](std::string_view req) {
            const int m = signer_mode.load();
            if (m == 401) {
                return std::string("HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            }
            if (m == 404) {
                return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found");
            }
            if (req.starts_with("GET /config ")) {
                return test::ok(R"({"ok":true,"identity":{"device_id":"d1","iid":"i1","fp":"f1"}})");
            }
            return test::ok(R"({"ok":true,"headers":{"x-helios":"h","x-medusa":"m"}})");
        });
        test::Server api([](std::string_view) {
            return test::ok(R"({"my_info":{"id":"42","nickname":"阿弦"}})");
        });

        Loop loop;
        net::Http http;
        qishui::Qishui qs(loop, http);
        qs.client.api_base = api.url();

        const lyren::test::LanguageGuard language_guard;
        set_language(Language::en);
        auto r404_en = run(loop, qs.import_session(glz::write_json(glz::obj{"signer_url", bad_signer.url()}).value()));
        REQUIRE_FALSE(r404_en);
        CHECK(r404_en.error().contains("signing service"));
        set_language(Language::zh);
        auto r404_zh = run(loop, qs.import_session(glz::write_json(glz::obj{"signer_url", bad_signer.url()}).value()));
        REQUIRE_FALSE(r404_zh);
        CHECK(r404_zh.error().contains("签名服务"));

        signer_mode = 401;
        auto chk401 = run(loop, qs.check_session(glz::write_json(glz::obj{"signer_url", bad_signer.url()}).value()));
        CHECK(chk401.signer.state == SessionCheck::State::failed);
        CHECK_FALSE(chk401.ok());

        signer_mode = 200;
        auto chk_anon = run(loop, qs.check_session(glz::write_json(glz::obj{"signer_url", bad_signer.url()}).value()));
        auto chk_user = run(loop, qs.check_session(glz::write_json(glz::obj{"signer_url", bad_signer.url(), "cookie", "Cookie: sessionid=ok; "}).value()));
        CHECK(chk_anon.ok());
        CHECK(chk_anon.account.state == SessionCheck::State::skipped);
        CHECK(chk_user.ok());
        CHECK(chk_user.account.state == SessionCheck::State::ok);
        CHECK(chk_user.account.detail.contains("阿弦"));
    }

    SUBCASE("user_playlists fallback uses session user_id and album() does not block on collection pagination") {
        std::atomic<int> mixed_fetches{0};
        test::Server api([&](std::string_view req) {
            if (req.starts_with("GET /luna/pc/me/playlist?")) {
                return test::ok(R"({"playlists":[{"id":"created1","title":"自建","type":0}],"has_more":false})");
            }
            if (req.starts_with("GET /luna/pc/user/playlist?")) {
                CHECK(req.contains("user_id=7"));
                CHECK_FALSE(req.contains("user_id=&"));
                return test::ok(R"({"playlists":[{"id":"liked7","title":"我喜欢","type":1}],"has_more":false})");
            }
            if (req.starts_with("GET /luna/me/collection/mixed?")) {
                ++mixed_fetches;
                if (req.contains("item_types=playlist")) {
                    return test::ok(R"({"mixed_collections":[],"has_more":false})");
                }
                return test::ok(R"({"mixed_collections":[{"album":{"id":"alb-1","name":"已藏专辑"}}],"has_more":false})");
            }
            if (req.starts_with("GET /luna/pc/albums/")) {
                return test::ok(R"({"album_info":{"id":"alb","name":"测试专辑","artists":[{"id":"ar1","name":"歌手"}]},"tracks":[]})");
            }
            return test::ok(R"({})");
        });
        Context c(api.url());
        const auto lists = run(c.loop, c.service.user_playlists(""));
        REQUIRE(lists);
        CHECK(lists->first.id == "liked7");
        mixed_fetches = 0;

        const auto a1 = run(c.loop, c.service.album("alb-1"));
        REQUIRE(a1);
        CHECK_FALSE(a1->collected.has_value());
        CHECK(mixed_fetches == 0);

        const auto saved = run(c.loop, c.service.collected_albums());
        REQUIRE(saved);
        REQUIRE(saved->size() == 1);
        CHECK(saved->front().id == "alb-1");
        CHECK(mixed_fetches == 1);
    }

    SUBCASE("custom device_id, iid, and fp override /config and persist for editing") {
        std::atomic<bool> empty_identity{true};
        test::Server signer([&](std::string_view req) {
            if (req.starts_with("GET /config ")) {
                return empty_identity.load() ? test::ok(R"({"ok":true,"identity":{}})") : test::ok(R"({"ok":true,"identity":{"device_id":"cfg_d","iid":"cfg_i","fp":"cfg_f"}})");
            }
            return test::ok(R"({"ok":true,"headers":{"x-helios":"h","x-medusa":"m"}})");
        });
        Loop loop;
        net::Http http;
        qishui::Qishui qs(loop, http);

        // 1. Empty /config identity succeeds when manual device_id and iid are provided.
        auto r1 = run(loop, qs.import_session(glz::write_json(glz::obj{"signer_url", signer.url(), "device_id", "man_d", "iid", "man_i"}).value()));
        REQUIRE_MESSAGE(r1.has_value(), (r1 ? std::string{} : r1.error()));
        CHECK(qs.client.get("device_id") == "man_d");
        CHECK(qs.client.get("iid") == "man_i");
        CHECK(qs.client.get("fp") == "man_d");
        CHECK(qs.client.get("custom_device_id") == "man_d");
        CHECK(qs.client.get("custom_iid") == "man_i");
        CHECK(qs.client.get("custom_fp").empty());

        // 2. Non-empty /config identity is overridden selectively by custom fields without marking defaults as custom.
        empty_identity = false;
        auto chk = run(loop, qs.check_session(glz::write_json(glz::obj{"signer_url", signer.url(), "device_id", "override_d"}).value()));
        CHECK(chk.ok());
        CHECK(chk.identity.detail.contains("override_d"));

        auto r2 = run(loop, qs.import_session(glz::write_json(glz::obj{"signer_url", signer.url(), "device_id", "override_d"}).value()));
        REQUIRE_MESSAGE(r2.has_value(), (r2 ? std::string{} : r2.error()));
        CHECK(qs.client.get("device_id") == "override_d");
        CHECK(qs.client.get("iid") == "cfg_i");
        CHECK(qs.client.get("fp") == "cfg_f");
        CHECK(qs.client.get("custom_device_id") == "override_d");
        CHECK(qs.client.get("custom_iid").empty());
    }
}
