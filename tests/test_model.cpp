#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/env.hpp"
#include "async.hpp"
#include "service.hpp"
#include "temp_dir.hpp"
#include "app/model/browser.hpp"
#include "app/model/fetch.hpp"
#include "app/model/library.hpp"
#include "app/model/playback.hpp"
#include "app/model/social.hpp"
#include "app/state.hpp"

using namespace lyren;

namespace {

    struct LibraryService : test::Service {
        Account current_account{.id = "u1", .name = "Alice"};
        std::vector<Card> playlists;
        test::Gate write;
        bool fail_writes = false;
        int like_calls = 0;
        std::vector<std::string> deleted_playlists;

        Task<Result<Account>> account() override {
            co_return current_account;
        }
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string) override {
            co_return std::pair{Card{.id = "liked", .owned = true}, playlists};
        }
        Task<Result<void>> like(Track, bool) override {
            ++like_calls;
            co_await write;
            if (fail_writes) {
                co_return std::unexpected("write rejected");
            }
            co_return Result<void>{};
        }
        Task<Result<void>> delete_playlist(std::string id) override {
            deleted_playlists.push_back(id);
            co_await write;
            if (fail_writes) {
                co_return std::unexpected("write rejected");
            }
            std::erase_if(playlists, [&](const Card &card) {
                return card.id == id;
            });
            co_return Result<void>{};
        }
    };

    template <class P = test::Service>
    struct ModelRig {
        Loop loop;
        net::Http http;
        Config config;
        double clock = 100.0;
        P service;
        test::Service other;
        Env env{.loop = loop,
                .http = http,
                .providers = {service, other, other, other, other},
                .config = config,
                .redraw = [] {},
                .now =
                    [this] {
                        return clock;
                    }};
    };

    Track make_track(std::string id) {
        return {.id = std::move(id), .title = "Song", .artist = "Artist", .duration = 180};
    }

} // namespace

TEST_CASE("Fetch, Paged, and LruCache maintain freshness, paging, and eviction") {
    Fetch f;
    CHECK_FALSE(f.busy());
    f.begin();
    CHECK(f.loading);
    CHECK(f.busy());
    f.succeed(50.0, true);
    CHECK(f.loaded);
    CHECK_FALSE(f.busy());
    CHECK(f.since == 50.0);
    CHECK_FALSE(f.due(80.0, 60.0, 10.0));
    CHECK(f.due(110.0, 60.0, 10.0));
    f.stale = true;
    CHECK(f.due(60.0, 60.0, 10.0));
    f.begin(true);
    CHECK(f.refreshing);
    f.fail("timeout", 70.0, true, 15.0);
    CHECK_FALSE(f.busy());
    CHECK(f.error.empty());
    CHECK_FALSE(f.due(80.0, 60.0, 10.0));
    CHECK(f.due(85.0, 60.0, 10.0));

    Paged<int> p;
    CHECK(p.open());
    p.error = "first failed";
    CHECK_FALSE(p.open());
    CHECK(p.open(true));
    p.error.clear();
    p.append(std::vector{1, 2}, "c1", true, 10.0, 5);
    CHECK(p.page == 1);
    CHECK(p.total == 5);
    CHECK(p.since == 10.0);
    CHECK(p.items == std::vector{1, 2});
    p.append(std::vector{3}, "", false, 20.0, 0);
    CHECK(p.page == 2);
    CHECK(p.total == 5);
    CHECK_FALSE(p.open(true));

    LruCache<int, 3> cache;
    cache.push(1);
    cache.push(2);
    cache.push(3);
    REQUIRE(cache.find(
                [](int x) {
                    return x == 1;
                },
                true) != nullptr);
    cache.push(4); // evicts 2 (LRU), keeps 3, 1, 4
    CHECK(cache.find(
              [](int x) {
                  return x == 2;
              },
              false) == nullptr);
    CHECK(cache.find(
              [](int x) {
                  return x == 1;
              },
              false) != nullptr);
    CHECK(cache.find(
              [](int x) {
                  return x == 4;
              },
              false) != nullptr);
}

TEST_CASE("Library radio_loading never strands when account changes mid-fetch") {
    ModelRig<LibraryService> rig;
    Library lib(rig.env);
    test::Gate fm;
    rig.service.on_fm = [&]() -> Task<Result<std::vector<Track>>> {
        if (rig.service.current_account.id == "u1") {
            co_await fm;
        }
        co_return std::vector{make_track("fm1"), make_track("fm2")};
    };
    spawn(lib.load(Source::netease));
    REQUIRE(lib.shelf(Source::netease).account.has_value());
    CHECK(lib.shelf(Source::netease).account->id == "u1");

    // Start slow radio fetch, then switch account so Library::load triggers reset().
    spawn(lib.load_radio(Source::netease));
    CHECK(lib.shelf(Source::netease).radio_loading);
    rig.service.current_account = {.id = "u2", .name = "Bob"};
    spawn(lib.load(Source::netease));
    fm.release();

    // The old radio fetch belonged to u1 (old radio_gen) and was discarded; radio_loading must be false.
    CHECK(lib.shelf(Source::netease).account->id == "u2");
    CHECK_FALSE(lib.shelf(Source::netease).radio_loading);
    CHECK(lib.shelf(Source::netease).radio.empty());

    // A fresh radio fetch now succeeds cleanly.
    spawn(lib.load_radio(Source::netease));
    CHECK_FALSE(lib.shelf(Source::netease).radio_loading);
    CHECK(lib.shelf(Source::netease).radio.size() == 2);
}

TEST_CASE("Library optimistic likes roll back on failure and reconcile with server reads") {
    ModelRig<LibraryService> rig;
    Library lib(rig.env);
    const Track t = make_track("t1");

    lib.toggle_like(t);
    CHECK(lib.liked(t));
    CHECK(lib.liked_at(t) == rig.clock);
    rig.service.write.release();
    CHECK(lib.liked(t));
    CHECK(rig.service.like_calls == 1);

    // Server write failure rolls back the optimistic unlike.
    rig.service.fail_writes = true;
    lib.toggle_like(t);
    CHECK_FALSE(lib.liked(t));
    rig.service.write.release();
    CHECK(lib.liked(t));

    // Reconciling a stale read (older revision) does not overwrite a confirmed newer write.
    const uint64_t stale_rev = 0;
    CHECK(lib.reconcile(Source::netease, Library::Preference::song, t.id, false, stale_rev));
    CHECK(lib.liked(t));
    // Reconciling a newer read updates confirmed state.
    const uint64_t fresh_rev = lib.shelf(Source::netease).preference_gen;
    CHECK_FALSE(lib.reconcile(Source::netease, Library::Preference::song, t.id, false, fresh_rev));
    CHECK_FALSE(lib.liked(t));
}

TEST_CASE("a local timed lyric is available offline only for its own platform and track") {
    struct Service : test::Service {
        int requests = 0;
        Task<Result<std::vector<LyricLine>>> lyrics(Track) override {
            ++requests;
            co_return std::unexpected("Offline");
        }
    } service;
    Loop loop;
    test::TempDir dir;
    std::filesystem::create_directory(dir.path / "netease");
    const auto path = dir.path / "netease" / "42.lrc";
    std::ofstream(path) << "\xEF\xBB\xBF[00:01.00]first\n[01:05.22]next";
    Track track{.id = "42"};
    const auto lines = test::run(loop, service.playback_lyrics(track, dir.path));
    REQUIRE(lines);
    REQUIRE(lines->size() == 2);
    CHECK(lyric_at(*lines, 59.04) == 0);
    CHECK(lyric_at(*lines, 65.22) == 1);
    CHECK(service.requests == 0);

    track.source = Source::qq;
    CHECK_FALSE(test::run(loop, service.playback_lyrics(track, dir.path)));
    std::ofstream(path) << "[ar:Singer]\nno time cues";
    track.source = Source::netease;
    CHECK_FALSE(test::run(loop, service.playback_lyrics(track, dir.path)));
    CHECK(service.requests == 2);
}

TEST_CASE("Playback queue edits and skip-all-failed stop cleanly") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);

    std::vector<std::string> attempts;
    test::Gate stream;
    rig.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        attempts.push_back(track.id);
        co_await stream;
        co_return std::unexpected("geo-blocked");
    };

    const std::vector<Track> list{make_track("a"), make_track("b"), make_track("c")};
    playback.play(list, 0);
    CHECK(playback.queue().size() == 3);
    CHECK(playback.index() == 0);
    CHECK(playback.playing());

    // All 3 tracks fail; playback must try each once and then stop instead of looping forever.
    for (size_t i = 0; i < list.size(); ++i) {
        stream.release();
    }
    CHECK(attempts == std::vector<std::string>{"a", "b", "c"});
    CHECK_FALSE(playback.playing());
    REQUIRE_FALSE(rig.env.notices.empty());
    CHECK(rig.env.notices.back().text.contains(trf("Playback stopped after {} unavailable songs", 3)));
    CHECK(rig.env.notices.back().text.contains("geo-blocked"));

    // Queue manipulation: play_next, remove before/current, clear.
    playback.play_next(make_track("d"));
    REQUIRE(playback.queue().size() == 4);
    const int cur = playback.index();
    CHECK(playback.queue()[static_cast<size_t>(cur + 1)].id == "d");
    if (cur > 0) {
        playback.remove(0);
        CHECK(playback.index() == cur - 1);
    }
    playback.clear();
    CHECK(playback.queue().empty());
    CHECK(playback.index() == -1);
    CHECK(playback.current() == nullptr);
}

TEST_CASE("Playback queue enqueue, batch play_next, move, keep_only_current, and undo snapshot") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    saved.queue = {make_track("a"), make_track("b"), make_track("c")};
    saved.current = 1;
    Playback playback(rig.env, player, true, saved);
    REQUIRE(playback.index() == 1);

    // Enqueue single and batch append to the end without disturbing the current track.
    playback.enqueue(make_track("d"));
    const std::array extra{make_track("e"), make_track("f")};
    playback.enqueue(extra);
    REQUIRE(playback.queue().size() == 6);
    CHECK(playback.queue()[3].id == "d");
    CHECK(playback.queue()[4].id == "e");
    CHECK(playback.queue()[5].id == "f");
    CHECK(playback.index() == 1);

    // Batch play_next inserts in order right after current.
    const std::array next_batch{make_track("n1"), make_track("n2")};
    playback.play_next(next_batch);
    REQUIRE(playback.queue().size() == 8);
    CHECK(playback.queue()[1].id == "b");
    CHECK(playback.queue()[2].id == "n1");
    CHECK(playback.queue()[3].id == "n2");
    CHECK(playback.queue()[4].id == "c");

    // Move tracks across the current index: current_ tracks the same song.
    playback.move(4, 0); // move "c" from after current to top
    CHECK(playback.queue()[0].id == "c");
    CHECK(playback.index() == 2);
    CHECK(playback.current()->id == "b");
    playback.move(2, 7); // move current "b" to bottom
    CHECK(playback.index() == 7);
    CHECK(playback.current()->id == "b");
    playback.move(0, 7); // move "c" from before current to bottom
    CHECK(playback.index() == 6);
    CHECK(playback.current()->id == "b");

    // Snapshot before keep_only_current / clear and restore cleanly.
    auto snap = playback.snapshot();
    playback.keep_only_current();
    REQUIRE(playback.queue().size() == 1);
    CHECK(playback.index() == 0);
    CHECK(playback.current()->id == "b");

    playback.clear();
    CHECK(playback.queue().empty());
    CHECK(playback.index() == -1);

    playback.restore(std::move(snap));
    REQUIRE(playback.queue().size() == 8);
    CHECK(playback.index() == 6);
    CHECK(playback.current()->id == "b");
}

TEST_CASE("Browser search_on and open_mention resolve profiles, fall back to search, and cancel stale lookups") {
    ModelRig rig;
    Library lib(rig.env);
    Social social(rig.env, lib);
    Browser browser(rig.env, lib, social, Source::netease);

    test::Gate gate;
    bool delay = false;
    std::vector<Card> users;
    rig.service.on_search = [&](std::string, SearchKind kind, int, int) -> Task<Result<Batch>> {
        if (delay) {
            co_await gate;
        }
        if (kind == SearchKind::users) {
            co_return Batch{.cards = users, .total = static_cast<int>(users.size())};
        }
        co_return Batch{};
    };

    // Exact case-insensitive match opens the user's profile directly.
    users = {
        {.kind = CardKind::user, .source = Source::netease, .id = "u-1", .title = "Other"},
        {.kind = CardKind::user, .source = Source::netease, .id = "u-ada", .title = "Ada"},
    };
    browser.open_mention(Source::netease, "ada");
    CHECK(browser.route().page == Page::profile);
    CHECK(browser.route().card.id == "u-ada");

    // Ambiguous non-exact results fall back to Search on the Users tab.
    users = {
        {.kind = CardKind::user, .source = Source::netease, .id = "u-1", .title = "Ada One"},
        {.kind = CardKind::user, .source = Source::netease, .id = "u-2", .title = "Ada Two"},
    };
    browser.open_mention(Source::netease, "Ada");
    CHECK(browser.route().page == Page::search);
    CHECK(browser.searched() == "Ada");
    REQUIRE(browser.route().search != nullptr);
    CHECK(browser.route().search->tab == 4);

    // Navigating away while a mention lookup is in flight cancels the stale navigation.
    delay = true;
    users = {{.kind = CardKind::user, .source = Source::netease, .id = "u-late", .title = "Late"}};
    browser.open_mention(Source::netease, "Late");
    browser.open(Page::settings);
    gate.release();
    CHECK(browser.route().page == Page::settings);

    // search_on switches platform and sets query + tab.
    delay = false;
    browser.search_on(Source::netease, "NightTrain", 0);
    CHECK(browser.route().page == Page::search);
    CHECK(browser.searched() == "NightTrain");
    REQUIRE(browser.route().search != nullptr);
    CHECK(browser.route().search->tab == 0);
}

TEST_CASE("Browser navigation, selection, playlist deletion, and async album collection") {
    ModelRig<LibraryService> rig;
    Library lib(rig.env);
    Social social(rig.env, lib);
    Browser browser(rig.env, lib, social, Source::netease);

    const Card pl{.kind = CardKind::playlist, .source = Source::netease, .id = "pl1", .title = "Mix", .owned = true};
    const Card alb{.kind = CardKind::album, .source = Source::netease, .id = "al1", .title = "Album"};
    rig.service.playlists = {pl};
    spawn(lib.load(Source::netease));

    browser.open(pl);
    CHECK(browser.route().page == Page::playlist);
    CHECK(browser.can_go_back());

    // Opening album triggers async collection load without blocking album() itself.
    test::Gate albums;
    int collection_reads = 0;
    rig.service.on_collected_albums = [&]() -> Task<Result<std::vector<Card>>> {
        ++collection_reads;
        co_await albums;
        co_return std::vector{alb};
    };
    browser.open(alb);
    CHECK(browser.route().page == Page::album);
    REQUIRE(browser.route().list != nullptr);
    REQUIRE(browser.route().list->album.artist == "Mira Chen");
    const auto reported = browser.route().list->album.collected;
    CHECK_FALSE(reported.has_value());
    CHECK_FALSE(lib.kept(Source::netease, Library::Preference::album, "al1", reported).has_value());
    albums.release();
    rig.service.on_collected_albums = nullptr;
    CHECK(collection_reads == 1);
    CHECK(lib.kept(Source::netease, Library::Preference::album, "al1", reported) == true);
    CHECK(lib.kept(Source::netease, Library::Preference::album, "other", std::nullopt) == false);

    // Back / forward navigation.
    CHECK(browser.back());
    CHECK(browser.route().page == Page::playlist);
    CHECK(browser.forward());
    CHECK(browser.route().page == Page::album);

    // Deleting a playlist purges it from history and forwards.
    CHECK(browser.back());
    CHECK(browser.route().page == Page::playlist);
    rig.service.fail_writes = true;
    browser.delete_playlist(pl);
    CHECK(browser.route().page == Page::playlist);
    rig.service.write.release();
    CHECK(browser.route().page == Page::playlist);
    CHECK(browser.can_go_forward());
    rig.service.fail_writes = false;
    browser.delete_playlist(pl);
    CHECK(browser.route().page == Page::playlist);
    rig.service.write.release();
    CHECK(browser.route().page == Page::recommend);
    REQUIRE(browser.forward());
    CHECK(browser.route().card.id == alb.id);
    CHECK_FALSE(browser.forward());
    CHECK(rig.service.deleted_playlists == std::vector<std::string>{"pl1", "pl1"});

    // Selection range & choose_all.
    const std::vector<Track> tracks{make_track("1"), make_track("2"), make_track("3"), make_track("4")};
    browser.selection().list = &tracks;
    browser.choose(tracks, 1, false);
    browser.choose(tracks, 3, true);
    auto picked = browser.selection().of(tracks);
    REQUIRE(picked.size() == 3);
    CHECK(picked[0].id == "2");
    CHECK(picked[2].id == "4");
    browser.choose_all();
    CHECK(browser.selection().chosen.size() == 4);
}

TEST_CASE("shuffle skips unplayable tracks without repeating failed picks") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);
    std::vector<Track> tracks;
    for (int i = 0; i < 5; ++i) {
        Track t = test::Service::song(i);
        t.id = i == 3 ? "playable" : std::format("bad-{}", i);
        tracks.push_back(std::move(t));
    }
    std::vector<std::string> attempted;
    test::Gate pending;
    rig.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        attempted.push_back(track.id);
        if (track.id == "playable") {
            co_await pending;
            co_return std::unexpected("Stopped");
        }
        co_return std::unexpected("Unavailable");
    };
    playback.set_mode(PlayMode::shuffle);
    playback.play(tracks, 0);
    REQUIRE(playback.current() != nullptr);
    CHECK(playback.current()->id == "playable");
    // Every failed track is tried at most once before reaching the playable one.
    std::unordered_set<std::string> unique(attempted.begin(), attempted.end());
    CHECK(unique.size() == attempted.size());
    REQUIRE(pending.waiter);
    playback.stop();
    pending.release();
}

TEST_CASE("a large queue stops after five consecutive failures and a manual skip starts a fresh run") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);
    std::vector<Track> tracks;
    for (int i = 0; i < 40; ++i) {
        tracks.push_back(test::Service::song(i));
    }
    int attempts = 0;
    rig.service.on_stream = [&](Track, Quality) -> Task<Result<Stream>> {
        ++attempts;
        co_return std::unexpected("Unavailable");
    };

    playback.play(tracks, 10);
    CHECK(attempts == 5);
    CHECK(playback.index() == 14);
    CHECK_FALSE(playback.playing());
    playback.skip(-1);
    CHECK(attempts == 10);
    CHECK(playback.index() == 9);
    CHECK_FALSE(playback.playing());
}

TEST_CASE("radio bounds consecutive skipped previews across asynchronous batches") {
    ModelRig rig;
    rig.config.playback.trial = TrialMode::skip;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);
    Library library(rig.env);
    playback.connect(library);
    int fm_calls = 0, attempts = 0;
    bool empty = false;
    test::Gate next_batch;
    rig.service.on_fm = [&]() -> Task<Result<std::vector<Track>>> {
        if (++fm_calls == 1) {
            co_return std::vector{test::Service::song(0), test::Service::song(1)};
        }
        co_await next_batch;
        if (empty) {
            co_return std::vector<Track>{};
        }
        co_return std::vector{test::Service::song(2), test::Service::song(3), test::Service::song(4)};
    };
    rig.service.on_stream = [&](Track, Quality) -> Task<Result<Stream>> {
        ++attempts;
        co_return Stream{.trial = true};
    };

    constexpr auto source = Source::netease;
    spawn(library.load_radio(source));
    const auto &shelf = library.shelf(source);
    playback.play_radio(source, shelf.radio, 0);
    REQUIRE(attempts == 2);
    REQUIRE(shelf.radio_pending);
    SUBCASE("the next batch has no more songs") {
        empty = true;
        next_batch.release();
        CHECK(attempts == 2);
        CHECK(fm_calls == 2);
    }
    SUBCASE("a late batch cannot restart playback after the fifth skipped preview") {
        next_batch.release();
        REQUIRE(attempts == 5);
        REQUIRE_FALSE(playback.radio(source));
        // Browsing past the final failed track completes the outstanding prefetch with a radio_ready event.
        REQUIRE(shelf.radio_loading);
        CHECK_FALSE(library.advance_radio(source));
        next_batch.release();
        CHECK(attempts == 5);
        CHECK(fm_calls == 3);
    }
    REQUIRE(rig.env.notices.size() == 2);
    CHECK(rig.env.notices.back().text.contains(trf("Playback stopped after {} unavailable songs", attempts)));
    CHECK_FALSE(playback.radio(source));
    CHECK_FALSE(playback.playing());
}

TEST_CASE("radio waits for the next batch instead of replaying its last track") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);
    int fm_calls = 0;
    test::Gate pending_fm;
    rig.service.on_fm = [&]() -> Task<Result<std::vector<Track>>> {
        const int batch = ++fm_calls;
        if (batch == 1) {
            Track first = test::Service::song(0);
            first.id = "radio-0";
            co_return std::vector<Track>{std::move(first)};
        }
        co_await pending_fm;
        Track second = test::Service::song(1);
        second.id = "radio-1";
        co_return std::vector<Track>{std::move(second)};
    };
    std::vector<std::string> started;
    std::array<test::Gate, 2> streams;
    rig.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        started.push_back(track.id);
        REQUIRE(started.size() <= streams.size());
        co_await streams[started.size() - 1];
        co_return std::unexpected("Stopped");
    };

    Library library(rig.env);
    playback.connect(library);
    spawn(library.load(Source::netease));
    spawn(library.load_radio(Source::netease));
    auto &shelf = library.shelf(Source::netease);
    REQUIRE(shelf.radio.size() == 1);
    playback.play_radio(Source::netease, shelf.radio, 0);
    REQUIRE(started.size() == 1);
    CHECK(started.back() == "radio-0");

    // Skipping at the end of the batch must not replay radio-0 while batch 2 is still in flight.
    playback.skip(1);
    CHECK(fm_calls == 2);
    CHECK(shelf.radio_pos == 0);
    CHECK(shelf.radio_pending);
    CHECK(started.size() == 1);
    CHECK_FALSE(playback.playing());

    // Once batch 2 arrives, the radio moves on and starts playing the new track automatically.
    pending_fm.release();
    CHECK(shelf.radio_pos == 1);
    CHECK_FALSE(shelf.radio_pending);
    REQUIRE(playback.current() != nullptr);
    CHECK(playback.current()->id == "radio-1");
    REQUIRE(started.size() == 2);
    CHECK(started.back() == "radio-1");
    playback.stop();
    for (auto &gate : streams) {
        gate.release();
    }
}

TEST_CASE("route and shelf caches reuse data, preserve stale reads and retry after backoff") {
    ModelRig rig;
    Library library(rig.env);
    Social social(rig.env, library);
    Browser browser(rig.env, library, social, Source::netease);
    spawn(library.load(Source::netease));
    int track_fetches = 0, daily_fetches = 0, radar_fetches = 0, album_fetches = 0, artist_fetches = 0, profile_fetches = 0, collection_fetches = 0, recent_fetches = 0;
    bool fail_tracks = false, fail_collection = false;
    rig.service.on_tracks = [&](Card card) -> Task<Result<std::vector<Track>>> {
        ++track_fetches;
        if (fail_tracks) {
            co_return std::unexpected("Temporary error");
        }
        std::vector<Track> out;
        for (int i = 0; i < 24; ++i) {
            Track t = test::Service::song(i);
            t.id = card.id + "-s" + std::to_string(i);
            out.push_back(std::move(t));
        }
        co_return out;
    };
    rig.service.features.daily.songs = [&]() -> Task<Result<std::vector<Track>>> {
        ++daily_fetches;
        co_return std::vector<Track>{test::Service::song(0), test::Service::song(1), test::Service::song(2)};
    };
    rig.service.features.radar = [&]() -> Task<Result<std::vector<Track>>> {
        ++radar_fetches;
        co_return std::vector<Track>{test::Service::song(2), test::Service::song(3)};
    };
    rig.service.features.recent_plays = [&]() -> Task<Result<std::vector<Track>>> {
        ++recent_fetches;
        co_return std::vector<Track>{test::Service::song(4), test::Service::song(1)};
    };
    rig.service.on_album = [&](std::string) -> Task<Result<AlbumInfo>> {
        ++album_fetches;
        co_return AlbumInfo{.artist = "Mira Chen", .artist_id = "art-1", .date = "2026-01-01", .collected = false, .tracks = {test::Service::song(0), test::Service::song(1)}};
    };
    rig.service.on_artist = [&](std::string id) -> Task<Result<ArtistInfo>> {
        ++artist_fetches;
        co_return ArtistInfo{.name = "Artist " + id, .songs = 10, .albums = 2};
    };
    rig.service.on_profile = [&](std::string uid) -> Task<Result<Profile>> {
        ++profile_fetches;
        co_return Profile{.id = uid, .name = uid == test::Service::self ? "Mira Chen" : "Ada Lovelace", .follows = 10, .fans = 20};
    };
    rig.service.on_collected_albums = [&]() -> Task<Result<std::vector<Card>>> {
        ++collection_fetches;
        if (fail_collection) {
            co_return std::unexpected("Collection down");
        }
        co_return std::vector<Card>{
            {.kind = CardKind::album, .id = "alb-1", .title = "Harbour Tapes", .subtitle = "Mira Chen", .count = 10, .count_kind = Card::Count::songs},
        };
    };

    // 1. Built-in pages (liked, daily, radar), dynamic playlists, albums, artists, and profiles reuse caches via open().
    browser.open(Page::liked);
    const auto liked_list = browser.route().list;
    REQUIRE(liked_list != nullptr);
    CHECK(track_fetches == 1);
    browser.open(Page::daily);
    CHECK(daily_fetches == 1);
    REQUIRE(browser.back());
    CHECK(browser.route().list == liked_list);
    CHECK(track_fetches == 1);

    // Re-opening Liked songs via open() (sidebar) or via its playlist card reuses the exact cached TrackList.
    browser.open(Page::radar);
    CHECK(radar_fetches == 1);
    browser.open(Page::liked);
    CHECK(browser.route().list == liked_list);
    CHECK(track_fetches == 1);
    browser.open(Page::recommend);
    browser.open(Page::playlist, library.shelf(Source::netease).liked);
    CHECK(browser.route().page == Page::liked);
    CHECK(browser.route().list == liked_list);
    CHECK(track_fetches == 1);

    // Dynamic playlists, albums, and artists reuse LRU cache entries across open().
    const Card pl1{.kind = CardKind::playlist, .source = Source::netease, .id = "pl-1", .title = "Evening Mix"};
    const Card alb1{.kind = CardKind::album, .source = Source::netease, .id = "alb-1", .title = "Harbour Tapes"};
    const Card art1{.kind = CardKind::artist, .source = Source::netease, .id = "art-1", .title = "Mira Chen"};
    browser.open(pl1);
    const auto pl1_list = browser.route().list;
    CHECK(track_fetches == 2);
    browser.open(alb1);
    CHECK(album_fetches == 1);
    browser.open(art1);
    const auto art1_ptr = browser.route().artist;
    CHECK(artist_fetches == 1);
    browser.open(pl1);
    CHECK(browser.route().list == pl1_list);
    CHECK(track_fetches == 2);
    browser.open(alb1);
    CHECK(album_fetches == 1);
    browser.open(art1);
    CHECK(browser.route().artist == art1_ptr);
    CHECK(artist_fetches == 1);

    // 2. Collection 60s SWR keeps cached cards on transient refresh failure and Library::kept consults loaded collection.
    browser.open(Page::collection);
    CHECK(collection_fetches == 1);
    CHECK(library.kept(Source::netease, Library::Preference::album, "alb-1", false) == true);
    browser.open(Page::recommend);
    browser.open(Page::collection);
    CHECK(collection_fetches == 1);

    rig.clock += 65.0;
    fail_collection = true;
    browser.open(Page::recommend);
    browser.open(Page::collection);
    CHECK(collection_fetches == 2);
    // Transient background failure keeps previously cached album cards intact and backs off for 15s.
    CHECK(library.shelf(Source::netease).collection[0].cards.size() == 1);
    browser.open(Page::recommend);
    browser.open(Page::collection);
    CHECK(collection_fetches == 2);
    fail_collection = false;

    // 3. Cloud recent plays are shared between Page::recent (Cloud) and Own Profile, and note_cloud_play prepends optimistically.
    library.shelf(Source::netease).recent_tab = 1;
    browser.open(Page::recent);
    CHECK(recent_fetches == 1);
    browser.open_own_profile();
    const auto own_prof = browser.route().profile;
    CHECK(profile_fetches == 1);
    CHECK(recent_fetches == 1);
    browser.open(Page::recommend);
    browser.open_own_profile();
    CHECK(browser.route().profile == own_prof);
    CHECK(profile_fetches == 1);
    // Profile More -> Refresh creates a fresh Social::Page while preserving the active section tab.
    social.show(own_prof, Social::Section::collection);
    browser.reload_profile();
    CHECK(browser.route().profile != own_prof);
    CHECK(browser.route().profile->section == Social::Section::collection);
    CHECK(profile_fetches == 2);
    library.note_cloud_play(test::Service::song(0), false);
    REQUIRE_FALSE(library.shelf(Source::netease).recent_plays.tracks.empty());
    CHECK(library.shelf(Source::netease).recent_plays.tracks.front().id == "song-0");

    // 4. Playlist SWR after 300s preserves since & tracks on silent failure with 15s backoff, then refreshes calmly.
    const double pl1_since = pl1_list->since;
    rig.clock += 310.0;
    fail_tracks = true;
    browser.open(pl1);
    CHECK(track_fetches == 3);
    CHECK(pl1_list->tracks.size() == 24);
    CHECK(pl1_list->error.empty());
    CHECK(pl1_list->since == doctest::Approx(pl1_since));
    // Backoff prevents immediate retry on rapid switch-back.
    browser.open(Page::recommend);
    browser.open(pl1);
    CHECK(track_fetches == 3);
    // After 15s backoff, silent SWR succeeds without changing since.
    rig.clock += 20.0;
    fail_tracks = false;
    browser.open(Page::recommend);
    browser.open(pl1);
    CHECK(track_fetches == 4);
    CHECK(pl1_list->since == doctest::Approx(pl1_since));

    browser.reload_list();
    CHECK(track_fetches == 5);

    // 5. Off-screen Kind::liked + reload_after_edit marks cached Liked songs stale and updates tracks immediately.
    rig.service.reload_after_edit = true;
    browser.open(Page::recommend);
    Track added = test::Service::song(4);
    added.id = "brand-new-like";
    library.toggle_like(added);
    CHECK(liked_list->tracks.front().id == "brand-new-like");
    CHECK(liked_list->stale);
    browser.open(Page::liked);
    CHECK(track_fetches == 6);
    CHECK_FALSE(liked_list->stale);

    // 6. Failed initial load retries automatically on re-entry.
    const Card pl_err{.kind = CardKind::playlist, .source = Source::netease, .id = "pl-err", .title = "Flaky"};
    fail_tracks = true;
    browser.open(pl_err);
    CHECK(track_fetches == 7);
    CHECK_FALSE(browser.route().list->error.empty());
    fail_tracks = false;
    browser.open(Page::recommend);
    browser.open(pl_err);
    CHECK(track_fetches == 8);
    CHECK(browser.route().list->error.empty());
    CHECK(browser.route().list->tracks.size() == 24);

    // 8. Sign-out / account reset (Kind::left) flushes all cached lists, artists, and profiles.
    library.reset(Source::netease);
    browser.open(art1);
    CHECK(artist_fetches == 2);
}

TEST_CASE("a heartbeat response arriving after the listener chooses a song preserves their queue") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);
    test::Gate mix;
    rig.service.features.heartbeat = [&](std::string, std::string) -> Task<Result<std::vector<Track>>> {
        co_await mix;
        co_return std::vector{make_track("late")};
    };
    spawn(playback.heartbeat(make_track("seed"), "liked"));
    playback.play({make_track("chosen")}, 0);
    mix.release();
    REQUIRE(playback.queue().size() == 1);
    CHECK(playback.queue().front().id == "chosen");
}
