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
    CHECK(share_url(Track{.id = "18446744073709551615", .source = Source::qq}) == "https://i.y.qq.com/v8/playsong.html?songid=18446744073709551615");
    CHECK(share_url(Track{.id = "18446744073709551615"}) == "https://music.163.com/song?id=18446744073709551615");
}

TEST_CASE("identical songs on different platforms keep independent identities") {
    Track n{.id = "1", .title = "Love Story", .artist = "Taylor Swift", .duration = 235, .paid = true};
    Track q{.id = "1", .mid = "mid", .media = "media", .title = n.title, .artist = n.artist, .duration = 235, .source = Source::qq};
    CHECK_FALSE(same(n, q));
    CHECK(key(n) != key(q));
    CHECK(same(n, Track{.id = "1"}));
    Card nc{.id = "5"}, qc{.source = Source::qq, .id = "5"};
    CHECK_FALSE(same(nc, qc));
}

TEST_CASE("saved platform survives a round trip") {
    auto parsed = parse_state(R"({"queue":[{"source":1,"id":"1"}],"current":0})");
    REQUIRE(parsed);
    SavedState saved = std::move(*parsed);
    CHECK(saved.source == Source::netease);
    REQUIRE(saved.queue.size() == 1);
    CHECK(saved.queue[0].source == Source::qq);
    for (const auto &entry : platforms) {
        CAPTURE(entry.id);
        saved.source = entry.source;
        saved.queue[0].source = entry.source;
        std::string json;
        REQUIRE_FALSE(glz::write_json(saved, json));
        auto restored = parse_state(json);
        REQUIRE(restored);
        CHECK(restored->source == entry.source);
        CHECK(restored->queue[0].source == entry.source);
        CHECK(restored->queue[0].id == "1");
    }
}

TEST_CASE("saved state view serializes queue and recent spans identically to owned state") {
    SavedState owned{
        .width = 1280,
        .height = 800,
        .source = Source::qq,
        .volume = 0.42f,
        .muted = true,
        .mode = PlayMode::shuffle,
        .queue = {Track{.id = "q1", .title = "One", .source = Source::netease}, Track{.id = "q2", .title = "Two", .source = Source::qq}},
        .current = 1,
        .position = 37.5,
        .recent = {Track{.id = "r1", .title = "Recent", .source = Source::kugou}},
        .lyrics_x = -24,
        .lyrics_bottom = 64,
        .island_x = 12,
        .island_y = 18,
        .lyrics_locked = true,
        .lyrics_enabled = true,
        .lyrics_island = true,
        .lyrics_translation = false,
    };
    const SavedStateView view{
        .width = owned.width,
        .height = owned.height,
        .source = owned.source,
        .volume = owned.volume,
        .muted = owned.muted,
        .mode = owned.mode,
        .queue = owned.queue,
        .current = owned.current,
        .position = owned.position,
        .recent = owned.recent,
        .lyrics_x = owned.lyrics_x,
        .lyrics_bottom = owned.lyrics_bottom,
        .island_x = owned.island_x,
        .island_y = owned.island_y,
        .lyrics_locked = owned.lyrics_locked,
        .lyrics_enabled = owned.lyrics_enabled,
        .lyrics_island = owned.lyrics_island,
        .lyrics_translation = owned.lyrics_translation,
    };
    std::string owned_json;
    REQUIRE_FALSE(glz::write_json(owned, owned_json));
    const std::string view_json = format_state(view);
    CHECK(view_json == owned_json);
    const auto restored = parse_state(view_json);
    REQUIRE(restored);
    CHECK(restored->source == Source::qq);
    CHECK(restored->mode == PlayMode::shuffle);
    CHECK(restored->current == 1);
    CHECK(restored->position == doctest::Approx(37.5));
    REQUIRE(restored->queue.size() == 2);
    CHECK(restored->queue[0].id == "q1");
    CHECK(restored->queue[1].source == Source::qq);
    REQUIRE(restored->recent.size() == 1);
    CHECK(restored->recent[0].source == Source::kugou);
}

TEST_CASE("saved state rejects unregistered platforms and modes") {
    for (std::string_view text : {R"({"source":5})", R"({"queue":[{"source":5,"id":"1"}]})", R"({"recent":[{"source":5,"id":"1"}]})"}) {
        CAPTURE(text);
        const auto state = parse_state(text);
        REQUIRE_FALSE(state);
        CHECK(state.error() == "Unknown platform in saved state");
    }
    CHECK(parse_state(R"({"mode":3})").error() == "Unknown playback mode in saved state");
}

TEST_CASE("sharing preserves platform resource namespaces") {
    CHECK(share_url(Track{.id = "18446744073709551615", .source = Source::qishui}) == "https://www.qishui.com/track/18446744073709551615");
    CHECK(share_url(Card{.source = Source::qishui, .id = "123"}) == "https://www.qishui.com/playlist/123");
    CHECK(share_url(Card{.kind = CardKind::album, .source = Source::qishui, .id = "123"}) == "https://www.qishui.com/share/album?album_id=123");
    CHECK(share_url(Card{.source = Source::qishui, .id = "top:123"}).empty());
    CHECK(share_url(Card{.source = Source::kugou, .id = "123"}) == "https://www.kugou.com/yy/special/single/123.html");
    CHECK(share_url(Card{.source = Source::kugou, .id = "account:123"}).empty());
    CHECK(share_url(Card{.source = Source::kugou, .id = "top:123"}).empty());
    CHECK(share_url(Track{.id = "123", .source = Source::bodian}) == "https://h5app.kuwo.cn/m/bodian/playMusic.html?uid=-1&musicId=123&opusId=");
    CHECK(share_url(Card{.source = Source::bodian, .id = "13:123"}) == "https://h5app.kuwo.cn/m/bodian/collection.html?uid=-1&playlistId=123&source=13");
    CHECK(share_url(Card{.kind = CardKind::album, .source = Source::bodian, .id = "123"}) == "https://h5app.kuwo.cn/m/bodian/album.html?uid=-1&albumid=123");
    CHECK(share_url(Card{.source = Source::bodian, .id = "module:123"}).empty());
    CHECK(share_url(Card{.source = Source::bodian, .id = "ai:123:mix"}).empty());
}

TEST_CASE("Image URLs expand size templates and encode spaces across provider CDNs") {
    constexpr std::string_view url = "http://imge.kugou.com/mcommon/{size}/20241219/20241219164209670219.png";
    CHECK(cover_url(url, 96) == "https://imge.kugou.com/mcommon/150/20241219/20241219164209670219.png");
    CHECK(cover_url(url, 192) == "https://imge.kugou.com/mcommon/240/20241219/20241219164209670219.png");
    CHECK(cover_url(url, 640) == "https://imge.kugou.com/mcommon/480/20241219/20241219164209670219.png");
    CHECK(picture_url(url, 320) == cover_url(url, 320));
    CHECK(cover_url("http://c1.kgimg.com/stdmusic/{size}/20170105/20170105105545786343.jpg", 128) == "https://c1.kgimg.com/stdmusic/150/20170105/20170105105545786343.jpg");
    CHECK(cover_url("https://imge.kugou.com/mcommon/480/image.png?token=keep", 96) == "https://imge.kugou.com/mcommon/480/image.png?token=keep");
    CHECK(cover_url("https://p3-luna.douyinpic.com/img/cover~tplv-x-resize:{size}:{size}.png", 128) == "https://p3-luna.douyinpic.com/img/cover~tplv-x-resize:150:150.png");
    CHECK(cover_url("https://p3-luna.douyinpic.com/img/ies-music/团体 (1).jpg~tplv-b829550vbb-resize:{size}:{size}.png", 128) ==
          "https://p3-luna.douyinpic.com/img/ies-music/团体%20(1).jpg~tplv-b829550vbb-resize:150:150.png");
    CHECK(picture_url("https://images.example/a%20b c.jpg?token=a+b%2Fc", 320) == "https://images.example/a%20b%20c.jpg?token=a+b%2Fc");
}

TEST_CASE("saved slider positions and selections are normalized at the file boundary") {
    const auto state = parse_state(R"({"volume":2,"position":-5,"current":4,"queue":[{"id":"1"}],"lyrics_bottom":-2,"island_x":-300,"island_y":9000})");
    REQUIRE(state);
    CHECK(state->volume == 1);
    CHECK(state->position == 0);
    CHECK(state->current == -1);
    CHECK(state->lyrics_bottom == 0);
    CHECK(state->island_x == -300); // signed: left of the centre
    CHECK(state->island_y == 9000); // screen bounds are applied after the output is configured
    CHECK(parse_state(R"({"island_y":-4})")->island_y == 0);
}

TEST_CASE("credentials round-trip every platform") {
    auto parsed = creds::from_json(R"({"netease":{"MUSIC_U":"test-cookie"}})");
    REQUIRE(parsed);
    auto &c = *parsed;
    CHECK(c["netease"].at("MUSIC_U") == "test-cookie");
    CHECK(c["qq"].empty());
    c["qq"] = {{"musicid", "123"}, {"musickey", "test-key"}, {"refresh_token", "test-refresh"}};
    c["extension"] = {{"token", "future-session"}};
    auto restored = creds::from_json(creds::to_json(c));
    REQUIRE(restored == c);
    (*restored)["netease"].clear();
    restored = creds::from_json(creds::to_json(*restored));
    REQUIRE(restored);
    CHECK(restored->at("qq") == c.at("qq"));
    CHECK(restored->at("extension") == c.at("extension"));
    CHECK_FALSE(creds::from_json(R"({"netease":{"MUSIC_U":"test-cookie"},"qq":)"));
}
