#include <doctest/doctest.h>
#include <glaze/glaze.hpp>

#include "media/types.hpp"
#include "app/state.hpp"
#include "sys/creds.hpp"

using namespace lyren;

TEST_CASE("QQ sharing uses public resource identity instead of private directory and comment IDs") {
    Card directory{.source = Source::qq, .id = "dir:123", .thread = "4714021623", .owned = true};
    CHECK(share_url(directory) == "https://y.qq.com/n/ryqq/playlist/4714021623");
    directory.thread.clear();
    CHECK(share_url(directory).empty());
    CHECK(share_url(Card{.source = Source::qq, .id = "top:4"}) == "https://y.qq.com/n/ryqq/toplist/4");
    CHECK(share_url(Card{.kind = CardKind::album, .source = Source::qq, .id = "album-mid", .thread = "123"}) == "https://y.qq.com/n/ryqq/albumDetail/album-mid");
    CHECK(share_url(Card{.kind = CardKind::artist, .source = Source::qq, .id = "artist-mid"}) == "https://y.qq.com/n/ryqq/singer/artist-mid");
    CHECK(share_url(Track{.source = Source::qq, .id = "18446744073709551615"}) == "https://i.y.qq.com/v8/playsong.html?songid=18446744073709551615");
    CHECK(share_url(Track{.id = "18446744073709551615"}) == "https://music.163.com/song?id=18446744073709551615");
}

TEST_CASE("identical songs on different platforms keep independent identities") {
    Track n{.id = "1", .title = "Love Story", .artist = "Taylor Swift", .duration = 235, .paid = true};
    Track q{.source = Source::qq, .id = "1", .mid = "mid", .media = "media", .title = n.title, .artist = n.artist, .duration = 235};
    CHECK_FALSE(same(n, q));
    CHECK(key(n) != key(q));
    CHECK(same(n, Track{.id = "1"}));
    Card nc{.id = "5"}, qc{.source = Source::qq, .id = "5"};
    CHECK_FALSE(same(nc, qc));
}

TEST_CASE("saved platform survives a round trip and old queues remain readable") {
    auto parsed = parse_state(R"({"queue":[{"source":1,"id":"1","alts":[{"source":0,"id":"2"}]}],"current":0})");
    REQUIRE(parsed);
    SavedState saved = std::move(*parsed);
    CHECK(saved.source == Source::netease);
    REQUIRE(saved.queue.size() == 1);
    CHECK(saved.queue[0].source == Source::qq);
    saved.source = Source::qq;
    std::string json;
    REQUIRE_FALSE(glz::write_json(saved, json));
    auto restored = parse_state(json);
    REQUIRE(restored);
    CHECK(restored->source == Source::qq);
    CHECK(restored->queue[0].source == Source::qq);
    CHECK(restored->queue[0].id == "1");
}

TEST_CASE("saved state rejects unregistered platforms before indexing providers") {
    for (std::string_view text : {R"({"source":2})", R"({"queue":[{"source":2,"id":"1"}]})", R"({"recent":[{"source":2,"id":"1"}]})"}) {
        CAPTURE(text);
        const auto state = parse_state(text);
        REQUIRE_FALSE(state);
        CHECK(state.error() == "Unknown platform in saved state");
    }
}

TEST_CASE("saved state rejects playback modes outside the registered values") {
    const auto state = parse_state(R"({"mode":3})");
    REQUIRE_FALSE(state);
    CHECK(state.error() == "Unknown playback mode in saved state");
}

TEST_CASE("saved slider positions and selections are normalized at the file boundary") {
    const auto state = parse_state(R"({"volume":2,"position":-5,"current":4,"queue":[{"id":"1"}],"lyrics_bottom":-2})");
    REQUIRE(state);
    CHECK(state->volume == 1);
    CHECK(state->position == 0);
    CHECK(state->current == -1);
    CHECK(state->lyrics_bottom == 0);
}

TEST_CASE("credentials preserve registered and future platforms and read the old format") {
    auto c = creds::from_json(R"({"netease":{"MUSIC_U":"test-cookie"}})");
    CHECK(c["netease"].at("MUSIC_U") == "test-cookie");
    CHECK(c["qq"].empty());
    c["qq"] = {{"musicid", "123"}, {"musickey", "test-key"}, {"refresh_token", "test-refresh"}};
    c["extension"] = {{"token", "future-session"}};
    auto restored = creds::from_json(creds::to_json(c));
    CHECK(restored == c);
    restored["netease"].clear();
    restored = creds::from_json(creds::to_json(restored));
    CHECK(restored["qq"] == c["qq"]);
    CHECK(restored["extension"] == c["extension"]);
}

TEST_CASE("malformed credentials do not retain partially parsed sessions") {
    CHECK(creds::from_json(R"({"netease":{"MUSIC_U":"test-cookie"},"qq":)").empty());
}
