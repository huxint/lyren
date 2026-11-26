#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "app/env.hpp"
#include "app/model/browser.hpp"
#include "app/model/fetch.hpp"
#include "app/model/library.hpp"
#include "app/model/playback.hpp"
#include "app/model/social.hpp"
#include "app/state.hpp"
#include "bodian/bodian.hpp"
#include "kugou/kugou.hpp"
#include "ncm/netease.hpp"
#include "qq/qq.hpp"
#include "qishui/qishui.hpp"

using namespace lyren;

namespace {

    class StubProvider : public Provider {
    public:
        explicit StubProvider(Loop &loop, Source source = Source::netease)
        : loop_(loop),
          source_(source) {
            capabilities.likes = true;
            capabilities.library = true;
            capabilities.radio = true;
            capabilities.collect_albums = true;
            capabilities.follow_artists = true;
            capabilities.playlist_edit = true;
            capabilities.toplists = true;
        }

        StringMap<std::string> &session() override {
            return session_;
        }
        void on_session_changed(std::move_only_function<void()> cb) override {
            changed_ = std::move(cb);
        }
        Task<Result<void>> initialize() override {
            co_return Result<void>{};
        }
        Task<Result<Batch>> search(std::string, SearchKind, int, int) override {
            co_return Batch{};
        }
        Task<Result<Stream>> stream(Track track, Quality) override {
            if (on_stream) {
                co_return co_await on_stream(std::move(track));
            }
            co_return std::unexpected("unavailable");
        }
        Task<Result<std::vector<LyricLine>>> lyrics(Track) override {
            co_return std::vector<LyricLine>{};
        }
        Task<Result<std::vector<Track>>> tracks(Card) override {
            co_return std::vector<Track>{};
        }
        bool logged_in() const override {
            return signed_in;
        }
        Task<Result<Account>> account() override {
            if (account_delay > 0ms) {
                co_await lyren::sleep{loop_, account_delay};
            }
            co_return current_account;
        }
        Task<void> logout() override {
            signed_in = false;
            co_return;
        }
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string) override {
            co_return std::pair{Card{.kind = CardKind::playlist, .source = source_, .id = "liked", .title = "Liked", .owned = true}, playlists};
        }
        Task<Result<std::vector<std::string>>> liked_ids(std::string) override {
            co_return liked;
        }
        Task<Result<void>> like(Track, bool) override {
            ++like_calls;
            co_await lyren::sleep{loop_, 1ms};
            if (fail_writes) {
                co_return std::unexpected("write rejected");
            }
            co_return Result<void>{};
        }
        Task<Result<void>> follow(std::string, bool) override {
            if (fail_writes) {
                co_return std::unexpected("write rejected");
            }
            co_return Result<void>{};
        }
        Task<Result<void>> collect(std::string, bool) override {
            if (fail_writes) {
                co_return std::unexpected("write rejected");
            }
            co_return Result<void>{};
        }
        Task<Result<std::vector<Card>>> collected_albums() override {
            ++collected_albums_calls;
            co_await lyren::sleep{loop_, 1ms};
            co_return saved_albums;
        }
        Task<Result<std::vector<Card>>> followed_artists() override {
            ++followed_artists_calls;
            co_return saved_artists;
        }
        Task<Result<AlbumInfo>> album(std::string) override {
            co_return AlbumInfo{.artist = "Artist"};
        }
        Task<Result<ArtistInfo>> artist(std::string) override {
            co_return ArtistInfo{.name = "Artist"};
        }
        Task<Result<Batch>> artist_songs(std::string, int) override {
            co_return Batch{};
        }
        Task<Result<std::vector<Track>>> fm() override {
            ++fm_calls;
            if (fm_delay > 0ms) {
                co_await lyren::sleep{loop_, fm_delay};
            }
            co_return fm_batch;
        }
        Task<Result<void>> delete_playlist(std::string id) override {
            deleted_playlists.push_back(id);
            std::erase_if(playlists, [&](const Card &c) {
                return c.id == id;
            });
            co_return Result<void>{};
        }

        Loop &loop_;
        Source source_;
        StringMap<std::string> session_;
        std::move_only_function<void()> changed_;
        bool signed_in = true;
        bool fail_writes = false;
        std::chrono::milliseconds account_delay{0};
        std::chrono::milliseconds fm_delay{0};
        Account current_account{.id = "u1", .name = "Alice"};
        std::vector<Card> playlists;
        std::vector<std::string> liked;
        std::vector<Card> saved_albums;
        std::vector<Card> saved_artists;
        std::vector<Track> fm_batch;
        std::vector<std::string> deleted_playlists;
        int like_calls = 0;
        int fm_calls = 0;
        int collected_albums_calls = 0;
        int followed_artists_calls = 0;
        std::move_only_function<Task<Result<Stream>>(Track)> on_stream;
    };

    struct ModelRig {
        Loop loop;
        net::Http http;
        Config config;
        double clock = 100.0;
        int redraws = 0;
        StubProvider p0{loop, Source::netease};
        StubProvider p1{loop, Source::qq};
        StubProvider p2{loop, Source::qishui};
        StubProvider p3{loop, Source::kugou};
        StubProvider p4{loop, Source::bodian};
        Env env{.loop = loop,
                .http = http,
                .providers = {p0, p1, p2, p3, p4},
                .config = config,
                .redraw =
                    [this] {
                        ++redraws;
                    },
                .now =
                    [this] {
                        return clock;
                    }};

        void run_for(std::chrono::milliseconds ms) {
            loop.after(ms, [this] {
                loop.quit();
            });
            loop.run();
        }
    };

    Track make_track(std::string id, Source source = Source::netease) {
        return {.id = std::move(id), .title = "Song", .artist = "Artist", .duration = 180, .source = source};
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
    ModelRig rig;
    Library lib(rig.env);
    rig.p0.fm_delay = 25ms;
    rig.p0.fm_batch = {make_track("fm1"), make_track("fm2")};
    rig.p0.current_account = {.id = "u1", .name = "Alice"};
    spawn(lib.load(Source::netease));
    rig.run_for(5ms);
    REQUIRE(lib.shelf(Source::netease).account.has_value());
    CHECK(lib.shelf(Source::netease).account->id == "u1");

    // Start slow radio fetch, then switch account so Library::load triggers reset().
    spawn(lib.load_radio(Source::netease));
    CHECK(lib.shelf(Source::netease).radio_loading);
    rig.p0.current_account = {.id = "u2", .name = "Bob"};
    spawn(lib.load(Source::netease));
    rig.run_for(40ms);

    // The old radio fetch belonged to u1 (old radio_gen) and was discarded; radio_loading must be false.
    CHECK(lib.shelf(Source::netease).account->id == "u2");
    CHECK_FALSE(lib.shelf(Source::netease).radio_loading);
    CHECK(lib.shelf(Source::netease).radio.empty());

    // A fresh radio fetch now succeeds cleanly.
    rig.p0.fm_delay = 1ms;
    spawn(lib.load_radio(Source::netease));
    rig.run_for(15ms);
    CHECK_FALSE(lib.shelf(Source::netease).radio_loading);
    CHECK(lib.shelf(Source::netease).radio.size() == 2);
}

TEST_CASE("Library optimistic likes roll back on failure and reconcile with server reads") {
    ModelRig rig;
    Library lib(rig.env);
    const Track t = make_track("t1");

    rig.p0.fail_writes = false;
    lib.toggle_like(t);
    CHECK(lib.liked(t));
    CHECK(lib.liked_at(t) == rig.clock);
    rig.run_for(5ms);
    CHECK(lib.liked(t));
    CHECK(rig.p0.like_calls == 1);

    // Server write failure rolls back the optimistic unlike.
    rig.p0.fail_writes = true;
    lib.toggle_like(t);
    CHECK_FALSE(lib.liked(t));
    rig.run_for(5ms);
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

TEST_CASE("Playback queue edits and skip-all-failed stop cleanly") {
    ModelRig rig;
    audio::Player player(rig.loop, rig.http);
    SavedState saved;
    Playback playback(rig.env, player, true, saved);

    int stream_attempts = 0;
    rig.p0.on_stream = [&](Track) -> Task<Result<Stream>> {
        ++stream_attempts;
        co_await lyren::sleep{rig.loop, 1ms};
        co_return std::unexpected("geo-blocked");
    };

    const std::vector<Track> list{make_track("a"), make_track("b"), make_track("c")};
    playback.play(list, 0);
    CHECK(playback.queue().size() == 3);
    CHECK(playback.index() == 0);
    CHECK(playback.playing());

    // All 3 tracks fail; playback must try each once and then stop instead of looping forever.
    rig.run_for(30ms);
    CHECK(stream_attempts == 3);
    CHECK_FALSE(playback.playing());

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

TEST_CASE("Browser navigation, selection, playlist deletion, and async album collection") {
    ModelRig rig;
    Library lib(rig.env);
    Social social(rig.env, lib);
    Browser browser(rig.env, lib, social, Source::netease);

    const Card pl{.kind = CardKind::playlist, .source = Source::netease, .id = "pl1", .title = "Mix", .owned = true};
    const Card alb{.kind = CardKind::album, .source = Source::netease, .id = "al1", .title = "Album"};
    rig.p0.playlists = {pl};
    rig.p0.saved_albums = {alb};
    spawn(lib.load(Source::netease));
    rig.run_for(5ms);

    browser.open(pl);
    CHECK(browser.route().page == Page::playlist);
    CHECK(browser.can_go_back());

    // Opening album triggers async collection load without blocking album() itself.
    browser.open(alb);
    CHECK(browser.route().page == Page::album);
    REQUIRE(browser.route().list != nullptr);
    const auto reported = browser.route().list->album.collected;
    CHECK_FALSE(reported.has_value());
    CHECK_FALSE(lib.kept(Source::netease, Library::Preference::album, "al1", reported).has_value());
    rig.run_for(10ms);
    CHECK(rig.p0.collected_albums_calls == 1);
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
    browser.delete_playlist(pl);
    CHECK(browser.route().page == Page::recommend);
    rig.run_for(10ms);
    CHECK(rig.p0.deleted_playlists == std::vector<std::string>{"pl1"});

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

TEST_CASE("All five providers expose coherent capabilities") {
    Loop loop;
    net::Http http;
    ncm::Netease ncm(loop, http);
    qq::Qq qq(loop, http);
    qishui::Qishui qishui(loop, http);
    kugou::Kugou kugou(loop, http);
    bodian::Bodian bodian(loop, http);
    const std::array<const Provider *, 5> all{&ncm, &qq, &qishui, &kugou, &bodian};

    for (const Provider *p : all) {
        const auto &c = p->capabilities;
        CHECK(c.search[0]); // every platform searches songs
        CHECK_FALSE((c.qr_login && c.session_import));
        if (c.playlist_edit) {
            CHECK(c.library);
        }
        if (c.playlist_order) {
            CHECK(c.playlist_edit);
        }
        if (c.comment_sort || c.comment_replies || c.comment_likes || c.comment_post || c.comment_delete) {
            CHECK(std::ranges::any_of(c.comments, std::identity{}));
        }
        if (c.radio_dislike) {
            CHECK(c.radio);
        }
    }
    // Qishui uses session import; the other four use QR login.
    CHECK(qishui.capabilities.session_import);
    CHECK(ncm.capabilities.qr_login);
    CHECK(qq.capabilities.qr_login);
    CHECK(kugou.capabilities.qr_login);
    CHECK(bodian.capabilities.qr_login);
}
