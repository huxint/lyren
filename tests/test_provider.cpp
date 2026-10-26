#include <doctest/doctest.h>

#include <map>

#include "app/provider.hpp"
#include "sys/creds.hpp"

using namespace lyren;

TEST_CASE("merged songs retain recordings and prefer the free listing") {
    Track n{.id = "1", .title = "Love Story", .artist = "Taylor Swift", .album = "Fearless", .duration = 235, .paid = true};
    Track q{.source = Source::qq, .id = "2", .mid = "mid", .media = "media", .title = "Love Story",
            .artist = "Taylor Swift", .album = "Fearless", .duration = 232};
    Track live{.id = "3", .title = "Love Story (Live)", .artist = "Taylor Swift", .duration = 234};
    auto songs = merge_tracks({n, live}, {q});
    REQUIRE(songs.size() == 2);
    CHECK(key(songs[0]) == "qq:2");
    CHECK(songs[0].media == "media");
    REQUIRE(songs[0].alts.size() == 1);
    CHECK(key(songs[0].alts[0]) == "ncm:1");
    CHECK(same(songs[0], n));
    CHECK(same(songs[0], q));
    CHECK(songs[1].title == live.title);

    q.duration = 231;
    CHECK(merge_tracks({n}, {q}).size() == 2);
    q.duration = 235;
    q.artist = "A cover artist";
    CHECK(merge_tracks({n}, {q}).size() == 2);
}

TEST_CASE("song identity spans alternatives and keeps service ids separate") {
    Track n{.id = "1", .title = "Lyren", .artist = "A / B", .duration = 180};
    Track q{.source = Source::qq, .id = "1", .title = "lyren", .artist = "B / A", .duration = 180};
    CHECK_FALSE(same(n, q));
    CHECK(key(n) != key(q));
    auto songs = merge_tracks({n}, {q});
    REQUIRE(songs.size() == 1);
    CHECK(same(songs[0], q));
    Track a{.id = "2", .alts = {q}};
    Track b{.id = "3", .alts = {q}};
    CHECK(same(a, b));
    Track other{.id = "4", .title = n.title, .artist = n.artist, .duration = n.duration};
    CHECK(merge_tracks({n, other}, {}).size() == 2);
    Card nc{.id = "5"}, qc{.source = Source::qq, .id = "5"};
    CHECK_FALSE(same(nc, qc));
}

struct Streams {
    std::map<std::string, Result<Stream>> replies;
    std::vector<std::string> calls;
    Task<Result<Stream>> get(Track t, Quality) {
        calls.push_back(key(t));
        co_return replies.at(key(t));
    }
};

static Result<Playback> resolved(Track t, Streams& streams) {
    std::optional<Result<Playback>> out;
    spawn([](Track t, Streams* streams, std::optional<Result<Playback>>* out) -> Task<void> {
        *out = co_await resolve_stream(std::move(t), Quality::lossless, [streams](Track t, Quality quality) {
            return streams->get(std::move(t), quality);
        });
    }(std::move(t), &streams, &out));
    return std::move(*out);
}

TEST_CASE("playback tries alternatives before accepting a preview") {
    Track q{.source = Source::qq, .id = "2"};
    Track n{.id = "1", .alts = {q}};
    Streams streams;
    streams.replies["ncm:1"] = Stream{.url = "preview", .trial = true, .trial_start = 30};
    streams.replies["qq:2"] = Stream{.url = "full"};
    SUBCASE("the other service has a full stream") {
        auto p = resolved(n, streams);
        REQUIRE(p);
        CHECK(p->stream.url == "full");
        CHECK(key(p->track) == "qq:2");
        CHECK(same(p->track, n));
        REQUIRE(p->track.alts.size() == 1);
        CHECK(p->track.alts[0].alts.empty());
        CHECK(streams.calls == (std::vector<std::string>{"ncm:1", "qq:2"}));
    }
    SUBCASE("an unavailable recording still allows the other one") {
        streams.replies["ncm:1"] = std::unexpected("unavailable");
        auto p = resolved(n, streams);
        REQUIRE(p);
        CHECK(p->stream.url == "full");
    }
    SUBCASE("the preferred full stream needs no extra request") {
        streams.replies["ncm:1"] = Stream{.url = "preferred"};
        auto p = resolved(n, streams);
        REQUIRE(p);
        CHECK(p->stream.url == "preferred");
        CHECK(streams.calls.size() == 1);
    }
    SUBCASE("a preview is retained when no full recording is available") {
        streams.replies["qq:2"] = std::unexpected("unavailable");
        auto p = resolved(n, streams);
        REQUIRE(p);
        CHECK(p->stream.trial);
        CHECK(p->stream.trial_start == 30);
        CHECK(key(p->track) == "ncm:1");
        CHECK(streams.calls.size() == 2);
    }
    SUBCASE("both errors remain a playback failure") {
        streams.replies["ncm:1"] = std::unexpected("first error");
        streams.replies["qq:2"] = std::unexpected("second error");
        auto p = resolved(n, streams);
        REQUIRE_FALSE(p);
        CHECK(p.error() == "second error");
    }
}

TEST_CASE("credentials preserve both accounts and read the old format") {
    auto c = creds::from_json(R"({"netease":{"MUSIC_U":"test-cookie"}})");
    CHECK(c.netease.at("MUSIC_U") == "test-cookie");
    CHECK(c.qq.empty());
    c.qq = {{"musicid", "123"}, {"musickey", "test-key"}, {"refresh_token", "test-refresh"}};
    auto restored = creds::from_json(creds::to_json(c));
    CHECK(restored.netease == c.netease);
    CHECK(restored.qq == c.qq);
    restored.netease.clear();
    CHECK(creds::from_json(creds::to_json(restored)).qq == c.qq);
}
