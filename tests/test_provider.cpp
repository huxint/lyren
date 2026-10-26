#include <doctest/doctest.h>
#include <glaze/glaze.hpp>

#include "app/provider.hpp"
#include "app/state.hpp"
#include "sys/creds.hpp"

using namespace lyren;

TEST_CASE("identical songs on different platforms keep independent identities") {
    Track n{.id = "1", .title = "Love Story", .artist = "Taylor Swift", .duration = 235, .paid = true};
    Track q{.source = Source::qq, .id = "1", .mid = "mid", .media = "media", .title = n.title,
            .artist = n.artist, .duration = 235};
    CHECK_FALSE(same(n, q));
    CHECK(key(n) != key(q));
    CHECK(same(n, Track{.id = "1"}));
    std::array<Batch, 2> sources{Batch{.tracks = {n}}, Batch{.tracks = {q}}};
    auto songs = interleave(sources, &Batch::tracks);
    REQUIRE(songs.size() == 2);
    CHECK(songs[0].source == Source::netease);
    CHECK(songs[0].paid);
    CHECK(songs[1].source == Source::qq);
    CHECK(songs[1].media == "media");
    Card nc{.id = "5"}, qc{.source = Source::qq, .id = "5"};
    CHECK_FALSE(same(nc, qc));
}

TEST_CASE("search rankings retain every listing across uneven and empty batches") {
    std::array<Batch, 3> sources{
        Batch{.tracks = {{.id = "a1"}, {.id = "a2"}, {.id = "a3"}}, .cards = {{.id = "album-a"}}},
        Batch{},
        Batch{.tracks = {{.source = Source::qq, .id = "b1"}, {.source = Source::qq, .id = "b2"}},
              .cards = {{.source = Source::qq, .id = "album-b"}, {.source = Source::qq, .id = "album-c"}}},
    };
    auto tracks = interleave(sources, &Batch::tracks);
    REQUIRE(tracks.size() == 5);
    std::vector<std::string> ids;
    for (const auto& track : tracks) ids.push_back(track.id);
    CHECK(ids == (std::vector<std::string>{"a1", "b1", "a2", "b2", "a3"}));
    auto cards = interleave(sources, &Batch::cards);
    REQUIRE(cards.size() == 3);
    CHECK(cards[0].id == "album-a");
    CHECK(cards[1].id == "album-b");
    CHECK(cards[2].id == "album-c");
    CHECK(interleave(std::array<Batch, 3>{}, &Batch::tracks).empty());
}

TEST_CASE("saved platform survives a round trip and old queues remain readable") {
    SavedState saved;
    constexpr glz::opts options{.error_on_unknown_keys = false};
    auto error = glz::read<options>(saved, R"({"queue":[{"source":1,"id":"1","alts":[{"source":0,"id":"2"}]}],"current":0})");
    REQUIRE_FALSE(error);
    CHECK(saved.source == Source::netease);
    REQUIRE(saved.queue.size() == 1);
    CHECK(saved.queue[0].source == Source::qq);
    saved.source = Source::qq;
    std::string json;
    REQUIRE_FALSE(glz::write_json(saved, json));
    SavedState restored;
    REQUIRE_FALSE(glz::read<options>(restored, json));
    CHECK(restored.source == Source::qq);
    CHECK(same(saved.queue[0], restored.queue[0]));
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
