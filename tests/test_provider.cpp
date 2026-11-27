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
    for (std::string_view text : {R"({"source":7})", R"({"queue":[{"source":7,"id":"1"}]})", R"({"recent":[{"source":7,"id":"1"}]})"}) {
        CAPTURE(text);
        const auto state = parse_state(text);
        REQUIRE_FALSE(state);
        CHECK(state.error() == "Unknown platform in saved state");
    }
    const auto sub = parse_state(R"({"source":6,"source_instance":"home"})");
    REQUIRE(sub);
    CHECK(sub->source == Source::subsonic);
    CHECK(sub->source_instance == "home");
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

TEST_CASE("parse_share_url reverse-parses share links across all five platforms and surrounding share text") {
    auto check = [](std::string_view input, Source source, ShareLink::Kind kind, std::string_view id) {
        const auto parsed = parse_share_url(input);
        REQUIRE(parsed.has_value());
        CHECK(parsed->source == source);
        CHECK(parsed->kind == kind);
        CHECK(parsed->id == id);
    };

    // NetEase Cloud Music (direct, hash fragment, mobile, user, embedded in share text).
    check(share_url(Track{.id = "186016", .source = Source::netease}), Source::netease, ShareLink::Kind::song, "186016");
    check("https://music.163.com/#/playlist?id=24381616", Source::netease, ShareLink::Kind::playlist, "24381616");
    check("https://y.music.163.com/m/album?id=18907", Source::netease, ShareLink::Kind::album, "18907");
    check("https://music.163.com/artist?id=6452", Source::netease, ShareLink::Kind::artist, "6452");
    check("https://music.163.com/#/user/home?id=32953014", Source::netease, ShareLink::Kind::user, "32953014");
    check("分享单曲 https://music.163.com/song?id=186016&userid=1 (@网易云音乐)", Source::netease, ShareLink::Kind::song, "186016");

    // QQ Music (ryqq paths, playsong songid/songmid, toplist prefix, legacy taoge).
    check(share_url(Track{.id = "97773", .source = Source::qq}), Source::qq, ShareLink::Kind::song, "97773");
    check("https://y.qq.com/n/ryqq/songDetail/0039MnYb0qxYhV", Source::qq, ShareLink::Kind::song, "0039MnYb0qxYhV");
    check("https://y.qq.com/n/ryqq/playlist/7532145678", Source::qq, ShareLink::Kind::playlist, "7532145678");
    check("https://y.qq.com/n/ryqq/toplist/26", Source::qq, ShareLink::Kind::playlist, "top:26");
    check("https://y.qq.com/n/ryqq/albumDetail/000MkMni19ClKG", Source::qq, ShareLink::Kind::album, "000MkMni19ClKG");
    check("https://y.qq.com/n/ryqq/singer/0025NhlN2yWrP4", Source::qq, ShareLink::Kind::artist, "0025NhlN2yWrP4");
    check("https://i.y.qq.com/n2/m/share/details/taoge.html?id=123456789", Source::qq, ShareLink::Kind::playlist, "123456789");

    // Qishui Music.
    check(share_url(Track{.id = "71234567890", .source = Source::qishui}), Source::qishui, ShareLink::Kind::song, "71234567890");
    check("https://www.qishui.com/playlist/700112233", Source::qishui, ShareLink::Kind::playlist, "700112233");
    check("https://www.qishui.com/share/album?album_id=889900", Source::qishui, ShareLink::Kind::album, "889900");
    check("https://music.douyin.com/qishui/share/track?track_id=998877", Source::qishui, ShareLink::Kind::song, "998877");

    // Kugou Music.
    check(share_url(Card{.source = Source::kugou, .id = "554433"}), Source::kugou, ShareLink::Kind::playlist, "554433");
    check("https://www.kugou.com/yy/album/single/98765.html", Source::kugou, ShareLink::Kind::album, "98765");
    check("https://www.kugou.com/yy/singer/home/3520.html", Source::kugou, ShareLink::Kind::artist, "3520");
    check("https://www.kugou.com/song/#hash=ABCDEF0123456789ABCDEF0123456789", Source::kugou, ShareLink::Kind::song, "ABCDEF0123456789ABCDEF0123456789");

    // Bodian Music.
    check(share_url(Track{.id = "228908", .source = Source::bodian}), Source::bodian, ShareLink::Kind::song, "228908");
    check(share_url(Card{.source = Source::bodian, .id = "13:123"}), Source::bodian, ShareLink::Kind::playlist, "13:123");
    check(share_url(Card{.kind = CardKind::album, .source = Source::bodian, .id = "7788"}), Source::bodian, ShareLink::Kind::album, "7788");
    check(share_url(Card{.kind = CardKind::artist, .source = Source::bodian, .id = "336"}), Source::bodian, ShareLink::Kind::artist, "336");

    // Unrecognized / invalid inputs.
    CHECK_FALSE(parse_share_url("Night Train").has_value());
    CHECK_FALSE(parse_share_url("https://example.com/song?id=1").has_value());
    CHECK_FALSE(parse_share_url("https://music.163.com/song").has_value());
}

TEST_CASE("track_artists preserves structured collaborators and falls back to splitting Track::artist") {
    Track structured{.id = "1", .title = "Duet", .artist = "Jay Chou / Fei Yu-ching", .artist_id = "a1", .artists = {{"a1", "Jay Chou"}, {"a2", "Fei Yu-ching"}}};
    const auto s = track_artists(structured);
    REQUIRE(s.size() == 2);
    CHECK(s[0].id == "a1");
    CHECK(s[1].id == "a2");
    CHECK(s[1].name == "Fei Yu-ching");

    Track legacy{.id = "2", .title = "Legacy", .artist = "Mira Chen / YOASOBI", .artist_id = "m1"};
    const auto l = track_artists(legacy);
    REQUIRE(l.size() == 2);
    CHECK(l[0].id == "m1");
    CHECK(l[0].name == "Mira Chen");
    CHECK(l[1].id.empty());
    CHECK(l[1].name == "YOASOBI");
}
