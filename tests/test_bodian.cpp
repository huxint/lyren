#include <doctest/doctest.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "bodian/bodian.hpp"
#include "core/encoding.hpp"
#include "http_server.hpp"

using namespace lyren;

namespace {
    template <class T>
    Result<T> run(Loop &loop, Task<Result<T>> task) {
        std::optional<Result<T>> result;
        const auto timeout = loop.after(5s, [&loop] {
            loop.quit();
        });
        spawn([](Loop *loop, Task<Result<T>> task, std::optional<Result<T>> *result) -> Task<void> {
            *result = co_await std::move(task);
            loop->quit();
        }(&loop, std::move(task), &result));
        if (!result) {
            loop.run();
        }
        loop.cancel(timeout);
        REQUIRE(result.has_value());
        return std::move(*result);
    }

    std::string_view body(std::string_view request) {
        return request.substr(request.find("\r\n\r\n") + 4);
    }

    bodian::Reply reply(std::string data) {
        return {.code = 200, .body = "{\"code\":200,\"data\":" + data + '}'};
    }
} // namespace

TEST_CASE("Bodian signs form-encoded query and the exact GET body") {
    Loop loop;
    net::Http http;
    bodian::Client client(loop, http);
    client.session = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
    const std::string payload = R"({"devId":"device","musicId":9007199254740993,"br":"2000kflac","freeSign":"a ~&+*"})";
    auto request = client.request({.path = "play/music/v2/audioUrl",
                                   .query = {{"devId", "device"}, {"musicId", "9007199254740993"}, {"br", "2000kflac"}, {"freeSign", "a ~&+*"}},
                                   .body = payload,
                                   .method = "GET",
                                   .sign = true},
                                  1700000000123);
    CHECK(request.method == "GET");
    CHECK(request.body == payload);
    CHECK(request.url ==
          "https://bd-api.kuwo.cn/api/play/music/v2/"
          "audioUrl?devId=device&musicId=9007199254740993&br=2000kflac&freeSign=a+%7E%26%2B*&uid=42&token=session&timestamp=1700000000123&sign=860e6ad34785dcd4ab90b9c740499c67");
    CHECK(bodian::form_encode("晴天 ~&+*") == "%E6%99%B4%E5%A4%A9+%7E%26%2B*");
}

TEST_CASE("Bodian keeps large resource IDs and server page boundaries") {
    auto search = bodian::parse_search(reply(R"({"total":45,"resultList":[{"id":9007199254740993,"name":"晴天","duration":269,"artists":[{"id":336,"name":"周杰伦"}]}]})"),
                                       SearchKind::tracks, 0, 30);
    REQUIRE(search);
    REQUIRE(search->tracks.size() == 1);
    CHECK(search->tracks.front().id == "9007199254740993");
    CHECK(search->tracks.front().source == Source::bodian);
    CHECK(search->tracks.front().artist_id == "336");
    CHECK(search->next_offset == 30);
    auto playlists =
        bodian::parse_search(reply(R"({"total":2,"resultList":[{"id":99,"name":"歌单","musicnum":8,"source":13},{"name":"外部入口"}]})"), SearchKind::playlists, 0, 40);
    REQUIRE(playlists);
    REQUIRE(playlists->cards.size() == 1);
    CHECK(playlists->cards.front().id == "13:99");
    CHECK(playlists->cards.front().count == 8);
    CHECK(playlists->next_offset == -1);

    std::vector<std::string> requests;
    Result<std::vector<Track>> tracks;
    {
        test::Server server([&](std::string_view request) {
            requests.emplace_back(request);
            if (request.contains("pn=1&rn=100")) {
                return test::ok(R"({"code":200,"data":{"total":102,"list":[{"id":1,"name":"一"},{"id":2,"name":"二"}]}})");
            }
            return test::ok(R"({"code":200,"data":{"total":102,"list":[{"id":3,"name":"三"}]}})");
        });
        Loop loop;
        net::Http http;
        bodian::Bodian provider(loop, http, server.url() + '/');
        tracks = run(loop, provider.tracks({.source = Source::bodian, .id = "13:99"}));
    }
    REQUIRE(tracks);
    CHECK(tracks->size() == 3);
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].starts_with("GET /service/playlist/99/musicList?source=13&pn=1&rn=100&"));
    CHECK(requests[1].starts_with("GET /service/playlist/99/musicList?source=13&pn=2&rn=100&"));
}

TEST_CASE("Bodian playback checks rights before requesting a supported audio format") {
    int status = 4;
    SUBCASE("full playback") {}
    SUBCASE("preview") {
        status = 3;
    }
    SUBCASE("unrecognized rights cannot grant playback") {
        status = 99;
    }
    std::vector<std::string> requests;
    Result<Stream> stream;
    {
        test::Server server([&](std::string_view request) {
            requests.emplace_back(request);
            if (request.starts_with("GET /service/music/info?")) {
                return test::ok(
                    R"({"code":200,"data":{"id":9007199254740993,"name":"歌曲","audios":[{"level":"zp","format":"mflac","bitrate":"20900"},{"level":"ff","format":"flac","bitrate":"2000"},{"level":"p","format":"mp3","bitrate":"320"}],"payInfo":{"freeSign":"from-server","refrain_start":"1000","refrain_end":4000},"mediaBasicInfo":{"gain":-6.5,"peak":0.9}}})");
            }
            if (request.starts_with("GET /play/music/v2/checkRight?")) {
                return test::ok("{\"code\":200,\"data\":{\"status\":" + std::to_string(status) +
                                R"(,"audition":{"https":"","url":"https://audio.example/preview.mp3","start":30,"end":60,"format":"mp3","br":128}}})");
            }
            return test::ok(R"({"code":200,"data":{"audioHttpsUrl":"","audioUrl":"https://audio.example/song.mp3?grant=one","format":"mp3","bitrate":320}})");
        });
        Loop loop;
        net::Http http;
        bodian::Bodian provider(loop, http, server.url() + '/');
        provider.session()["devid"] = "device";
        stream = run(loop, provider.stream({.id = "9007199254740993", .source = Source::bodian}, Quality::lossless));
    }
    REQUIRE(requests.size() >= 2);
    CHECK(body(requests[1]) == R"({"musicId":9007199254740993,"freeSign":"from-server"})");
    CHECK(requests[1].contains("musicId=9007199254740993&freeSign=from-server&"));
    if (status == 99) {
        CHECK_FALSE(stream);
        CHECK(requests.size() == 2);
    } else {
        REQUIRE(stream);
        CHECK(stream->highlight_from == 1);
        CHECK(stream->highlight_to == 4);
        CHECK(stream->gain == doctest::Approx(-6.5));
        if (status == 3) {
            CHECK(stream->trial);
            CHECK(stream->trial_start == 30);
            CHECK(stream->url == "https://audio.example/preview.mp3");
            CHECK(requests.size() == 2);
        } else {
            REQUIRE(requests.size() == 3);
            CHECK(requests[2].starts_with("GET /play/music/v2/audioUrl?"));
            CHECK(body(requests[2]) == R"({"devId":"device","musicId":9007199254740993,"br":"2000kflac","freeSign":"from-server"})");
            CHECK(stream->level == "exhigh");
            CHECK(stream->url == "https://audio.example/song.mp3?grant=one");
            CHECK_FALSE(stream->trial);
        }
    }
}

TEST_CASE("Bodian word lyrics decode octal coefficients and identify translations by timing") {
    const std::string text = "[kuwo:127]\n[00:10.000]<0,0>译文\n[00:10.000]<1120,-1120>晴<2400,160>天\n[00:12.000]<1120,-1120>你好\n";
    auto lines = bodian::parse_lyric(reply("{\"content\":\"" + encoding::base64(text) + "\"}"));
    REQUIRE(lines);
    REQUIRE(lines->size() == 2);
    CHECK(lines->front().text == "晴天");
    CHECK(lines->front().translation == "译文");
    REQUIRE(lines->front().words.size() == 2);
    CHECK(lines->front().words[0].start_ms == 10000);
    CHECK(lines->front().words[0].duration_ms == 160);
    CHECK(lines->front().words[1].start_ms == 10160);
    CHECK(lines->front().words[1].from == 3);
    CHECK(lines->front().words[1].to == 6);
    auto invalid = bodian::parse_lyric(reply(R"({"content":"not-base64"})"));
    CHECK_FALSE(invalid);
    auto empty = bodian::parse_lyric(reply(R"({"content":""})"));
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("Bodian QR exchange persists only a complete current login") {
    bool cancel_exchange = false;
    SUBCASE("confirmed session") {}
    SUBCASE("cancelled exchange cannot log back in") {
        cancel_exchange = true;
    }
    Loop loop;
    net::Http http;
    bodian::Bodian *active = nullptr;
    std::atomic<int> exchanges = 0;
    test::Server server([&](std::string_view request) {
        if (request.starts_with("GET /ucenter/login/qrCode?")) {
            return test::ok(R"({"code":200,"data":{"qrCode":"challenge"}})");
        }
        if (request.starts_with("GET /ucenter/login/qrCodeStatus?")) {
            return test::ok(R"({"code":200,"data":{"status":3}})");
        }
        if (++exchanges == 1) {
            return test::ok(R"({"code":11027,"msg":"pending"})");
        }
        if (cancel_exchange) {
            loop.post([&] {
                active->cancel_login();
            });
        }
        return test::ok(R"({"code":200,"data":{"id":"42","bid":42,"token":"new-session","userInfo":{"id":42,"nickname":"听众","headImg":"https://example/avatar"}}})");
    });
    bodian::Bodian provider(loop, http, server.url() + '/');
    active = &provider;
    auto qr = run(loop, provider.qr_start());
    REQUIRE(qr);
    CHECK(qr->text == "https://bodian-oia.kuwo.cn/bodian/download.html?pageName=login_pc&pt=3&id=challenge");
    auto pending = run(loop, provider.qr_poll());
    REQUIRE(pending);
    CHECK(pending->state == QrState::scanned);
    CHECK_FALSE(provider.logged_in());
    auto confirmed = run(loop, provider.qr_poll());
    REQUIRE(confirmed);
    if (cancel_exchange) {
        CHECK(confirmed->state == QrState::expired);
        CHECK_FALSE(provider.logged_in());
    } else {
        CHECK(confirmed->state == QrState::confirmed);
        CHECK(provider.logged_in());
        CHECK(provider.session().at("uid") == "42");
        CHECK(provider.session().at("nickname") == "听众");
    }
}

TEST_CASE("Bodian expires only the rejected account session") {
    bool replace_session = false;
    SUBCASE("expired token clears account but retains the device") {}
    SUBCASE("an older response cannot clear a newer login") {
        replace_session = true;
    }
    Loop loop;
    net::Http http;
    bodian::Bodian *active = nullptr;
    test::Server server([&](std::string_view) {
        if (replace_session) {
            loop.post([&] {
                active->session()["token"] = "new-session";
            });
        }
        return test::ok(R"({"code":11012,"msg":"expired"})");
    });
    bodian::Bodian provider(loop, http, server.url() + '/');
    active = &provider;
    provider.session() = {{"uid", "42"}, {"token", "old-session"}, {"nickname", "听众"}, {"devid", "device"}};
    int changes = 0;
    provider.on_session_changed([&] {
        ++changes;
    });
    auto account = run(loop, provider.account());
    REQUIRE_FALSE(account);
    CHECK(account.error().contains("11012"));
    CHECK(provider.session().at("devid") == "device");
    CHECK(provider.logged_in() == replace_session);
    CHECK(changes == (replace_session ? 0 : 1));
    if (replace_session) {
        CHECK(provider.session().at("token") == "new-session");
    } else {
        CHECK_FALSE(provider.session().contains("nickname"));
    }
}

TEST_CASE("Bodian mixed collections keep resource kinds and playlist sources") {
    std::vector<std::string> requests;
    Result<std::pair<Card, std::vector<Card>>> library;
    Result<std::vector<Card>> albums;
    {
        test::Server server([&](std::string_view request) {
            requests.emplace_back(request);
            if (request.starts_with("GET /service/playlist/fond?")) {
                return test::ok(R"({"code":200,"data":{"id":8,"name":"喜欢"}})");
            }
            if (request.starts_with("GET /service/playlist/userCreate?")) {
                return test::ok(R"({"code":200,"data":{"playLists":[{"id":8,"name":"喜欢"},{"id":9,"name":"自建","isPrivate":1}]}})");
            }
            return test::ok(
                R"({"code":200,"data":{"total":3,"playLists":[{"id":10,"sourceType":13,"name":"推荐歌单"},{"id":11,"sourceType":6,"name":"专辑","artist":"歌手"},{"id":12,"albumId":13,"name":"另一张专辑"}]}})");
        });
        Loop loop;
        net::Http http;
        bodian::Bodian provider(loop, http, server.url() + '/');
        provider.session() = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
        library = run(loop, provider.user_playlists("42"));
        albums = run(loop, provider.collected_albums());
    }
    REQUIRE(library);
    CHECK(library->first.id == "5:8");
    REQUIRE(library->second.size() == 2);
    CHECK(library->second[0].id == "5:9");
    CHECK(library->second[0].owned);
    CHECK_FALSE(library->second[0].collectable);
    CHECK(library->second[1].id == "13:10");
    REQUIRE(albums);
    REQUIRE(albums->size() == 2);
    CHECK(albums->front().kind == CardKind::album);
    CHECK(albums->front().id == "11");
    CHECK(albums->back().id == "13");
    REQUIRE(requests.size() == 4);
    CHECK(requests[1].starts_with("GET /service/playlist/userCreate?userId=42&"));
    CHECK_FALSE(requests[1].contains("pn="));
    CHECK(requests[3].starts_with("GET /service/collect/4/list?userId=42&fromUid=42&pn=1&rn=100&"));
}

TEST_CASE("Bodian empty collections preserve liked and created playlists") {
    test::Server server([](std::string_view request) {
        if (request.starts_with("GET /service/playlist/fond?")) {
            return test::ok(R"({"code":200,"data":{"id":8,"name":"喜欢"}})");
        }
        if (request.starts_with("GET /service/playlist/userCreate?")) {
            return test::ok(R"({"code":200,"data":{"playLists":[{"id":9,"name":"自建"}]}})");
        }
        return test::ok(R"({"code":200,"data":{}})");
    });
    Loop loop;
    net::Http http;
    bodian::Bodian provider(loop, http, server.url() + '/');
    provider.session() = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
    auto library = run(loop, provider.user_playlists("42"));
    REQUIRE(library);
    CHECK(library->first.id == "5:8");
    REQUIRE(library->second.size() == 1);
    CHECK(library->second.front().id == "5:9");
    CHECK(library->second.front().owned);
    auto albums = run(loop, provider.collected_albums());
    REQUIRE(albums);
    CHECK(albums->empty());
    auto artists = run(loop, provider.followed_artists());
    REQUIRE(artists);
    CHECK(artists->empty());
}

TEST_CASE("Bodian collections still reject incomplete nonempty pages") {
    std::string data;
    SUBCASE("items without a total") {
        data = R"({"playLists":[{"id":10,"name":"收藏"}]})";
    }
    SUBCASE("a nonzero total without items") {
        data = R"({"total":1})";
    }
    test::Server server([&](std::string_view) {
        return test::ok("{\"code\":200,\"data\":" + data + '}');
    });
    Loop loop;
    net::Http http;
    bodian::Bodian provider(loop, http, server.url() + '/');
    provider.session() = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
    CHECK_FALSE(run(loop, provider.collected_playlists()));
}

TEST_CASE("Bodian playlist edits preserve metadata and never replay an uncertain write") {
    std::vector<std::string> requests;
    Result<void> result;
    {
        test::Server server([&](std::string_view request) {
            requests.emplace_back(request);
            if (request.starts_with("GET /service/playlist/info/9?")) {
                return test::ok(R"({"code":200,"data":{"id":9,"name":"原名","description":"原简介","pic":"https://example/original?size=900","categories":[{"id":4}]}})");
            }
            return std::string{};
        });
        Loop loop;
        net::Http http;
        bodian::Bodian provider(loop, http, server.url() + '/');
        provider.session() = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
        result = run(loop, provider.rename_playlist("5:9", "新名字"));
    }
    CHECK_FALSE(result);
    REQUIRE(requests.size() == 2);
    CHECK(requests.back().starts_with("PUT /service/playlist?"));
    CHECK(body(requests.back()) == R"({"id":9,"name":"新名字","description":"原简介","pic":"https://example/original?size=900","categoryList":[4]})");
}

TEST_CASE("Bodian comment replies keep root IDs for publishing and liking") {
    auto page = bodian::parse_comments(
        reply(R"({"comment_total":1,"more":false,"comment_list":[{"id":20,"parentId":10,"uid":42,"userInfo":{"nickname":"听众"},"content":"回复","likeCount":3,"like":1}]})"),
        "42");
    REQUIRE(page);
    REQUIRE(page->comments.size() == 1);
    CHECK(page->comments.front().id == "10:20");
    CHECK(page->comments.front().owned);
    CHECK(page->comments.front().liked);
    std::vector<std::string> requests;
    {
        test::Server server([&](std::string_view request) {
            requests.emplace_back(request);
            return test::ok(R"({"code":200,"data":{}})");
        });
        Loop loop;
        net::Http http;
        bodian::Bodian provider(loop, http, server.url() + '/');
        provider.session() = {{"uid", "42"}, {"token", "session"}, {"devid", "device"}};
        CHECK(run(loop, provider.like_comment(Subject::song, "99", "10:20", true)));
        auto posted = run(loop, provider.post_comment(Subject::song, "99", "新的回复", "10:20", "10"));
        REQUIRE(posted);
        CHECK(posted->empty());
    }
    REQUIRE(requests.size() == 2);
    CHECK(body(requests[0]) == R"({"moduleId":99,"moduleType":2,"uid":42,"commentId":20,"op":1,"parentId":10})");
    CHECK(body(requests[1]) == R"({"moduleId":99,"moduleType":2,"uid":42,"content":"新的回复","parentId":10,"replyId":20,"anonymous":0})");
}

TEST_CASE("Bodian search continues when total only counts the current page") {
    const auto full = reply(R"({"total":1,"rn":1,"resultList":[{"id":42,"name":"一首歌"}]})");
    const auto first = bodian::parse_search(full, SearchKind::tracks, 0, 30);
    REQUIRE(first);
    CHECK(first->next_offset == 30);
    const auto second = bodian::parse_search(full, SearchKind::tracks, first->next_offset, 30);
    REQUIRE(second);
    CHECK(second->next_offset == 60);
    CHECK(second->total >= 3);
    const auto end = bodian::parse_search(reply(R"({"total":0,"rn":1,"resultList":[]})"), SearchKind::tracks, second->next_offset, 30);
    REQUIRE(end);
    CHECK(end->next_offset == -1);
}
