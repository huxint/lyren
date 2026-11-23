#include <doctest/doctest.h>

#include "scene.hpp"

using namespace lyren;
using test::Scene;

namespace {
    // What a frame drew, by its words.
    bool drew(std::span<const ui::Painter::Run> runs, std::string_view text) {
        return std::ranges::any_of(runs, [&](const ui::Painter::Run &run) {
            return !run.icon && run.text == text;
        });
    }

    bool overlaps(ui::Rect a, ui::Rect b) {
        return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    }
} // namespace

TEST_CASE("trace: the redesigned surfaces as laid out") {
    gfx::select_driver(GpuChoice::integrated);
    for (const bool dark : {true, false}) {
        Scene scene({.dark = dark, .queue = {test::Service::song(0), test::Service::song(2)}, .current = 1});
        const std::string tag = dark ? "dark" : "light";
        const auto home = scene.draw();
        CHECK(drew(home, "For you"));
        CHECK(drew(home, "Daily mix"));
        CHECK(drew(home, "/"));
        scene.trace("home-" + tag);

        scene.shell.browser.open_own_profile();
        scene.draw();
        const auto own = scene.draw();
        CHECK(drew(own, "Mira Chen"));
        CHECK(drew(own, "Following"));
        CHECK(drew(own, "Followers"));
        // Profile stat numerals and labels stay cleanly separated.
        const auto *following_lbl = scene.find("Following");
        const auto *count_lbl = scene.find("128");
        REQUIRE(following_lbl != nullptr);
        REQUIRE(count_lbl != nullptr);
        CHECK_FALSE(overlaps(count_lbl->box, following_lbl->box));
        scene.trace("profile-own-" + tag);

        scene.draw(760, 600);
        scene.draw(760, 600);
        scene.trace("profile-own-narrow-" + tag);

        scene.shell.browser.open_profile(Source::netease, std::string(test::Service::friend_id), "Ada Lovelace", "");
        scene.draw();
        const auto other = scene.draw();
        CHECK(drew(other, "Ada Lovelace"));
        scene.trace("profile-other-" + tag);

        scene.shell.browser.open(Page::settings);
        scene.draw();
        const auto settings = scene.draw();
        CHECK(drew(settings, "Settings"));
        CHECK(drew(settings, "Appearance"));
        scene.trace("settings-" + tag);

        scene.shell.browser.open(Page::fm);
        scene.draw();
        const auto fm = scene.draw();
        CHECK(drew(fm, "Night Train"));
        CHECK(drew(fm, "Up next · 夜に駆ける — YOASOBI"));
        scene.trace("fm-" + tag);

        scene.shell.open_comments(Source::netease, Subject::song, "song-0", "Night Train · Mira Chen", "");
        scene.draw();
        const auto comments = scene.draw();
        CHECK(drew(comments, "Top comment"));
        CHECK(drew(comments, "The bridge at two minutes in is the whole song."));
        scene.trace("comments-" + tag);

        scene.shell.panel.show(SidePanel::Kind::none);
        scene.shell.stage.show(true);
        scene.draw();
        const auto stage = scene.draw();
        CHECK(drew(stage, "Now playing"));
        CHECK(drew(stage, "Paper Planes Over the Harbour at Dawn"));
        scene.trace("stage-" + tag);
    }
}

TEST_CASE("browser back and forward navigation work through title bar buttons") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.draw();
    CHECK_FALSE(scene.shell.browser.can_go_back());
    CHECK_FALSE(scene.shell.browser.can_go_forward());

    scene.shell.browser.open(Page::settings);
    scene.draw();
    CHECK(scene.shell.browser.route().page == Page::settings);
    CHECK(scene.shell.browser.can_go_back());
    CHECK_FALSE(scene.shell.browser.can_go_forward());

    const auto *back = scene.find("chevron-left", true);
    REQUIRE(back != nullptr);
    scene.click(*back);
    CHECK(scene.shell.browser.route().page == Page::recommend);
    CHECK_FALSE(scene.shell.browser.can_go_back());
    CHECK(scene.shell.browser.can_go_forward());

    const auto *forward = scene.find("chevron-right", true);
    REQUIRE(forward != nullptr);
    scene.click(*forward);
    CHECK(scene.shell.browser.route().page == Page::settings);
    CHECK(scene.shell.browser.can_go_back());
    CHECK_FALSE(scene.shell.browser.can_go_forward());
}

TEST_CASE("shuffle skips unplayable tracks without repeating failed picks") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    std::vector<Track> tracks;
    for (int i = 0; i < 8; ++i) {
        Track t = test::Service::song(i);
        t.id = i == 5 ? "playable" : std::format("bad-{}", i);
        tracks.push_back(std::move(t));
    }
    std::vector<std::string> attempted;
    std::coroutine_handle<> pending;
    struct Hold {
        std::coroutine_handle<> &slot;
        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            slot = h;
        }
        void await_resume() const noexcept {}
    };
    scene.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        attempted.push_back(track.id);
        if (track.id == "playable") {
            co_await Hold{pending};
            co_return std::unexpected("Stopped");
        }
        co_return std::unexpected("Unavailable");
    };
    scene.shell.playback.set_mode(PlayMode::shuffle);
    scene.shell.playback.play(tracks, 0);
    REQUIRE(scene.shell.playback.current() != nullptr);
    CHECK(scene.shell.playback.current()->id == "playable");
    // Every failed track is tried at most once before reaching the playable one.
    std::unordered_set<std::string> unique(attempted.begin(), attempted.end());
    CHECK(unique.size() == attempted.size());
    CHECK(pending);
    scene.shell.playback.stop();
    if (pending) {
        pending.resume();
    }
}

TEST_CASE("radio waits for the next batch instead of replaying its last track") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    int fm_calls = 0;
    std::coroutine_handle<> pending_fm;
    struct Hold {
        std::coroutine_handle<> &slot;
        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            slot = h;
        }
        void await_resume() const noexcept {}
    };
    scene.service.on_fm = [&]() -> Task<Result<std::vector<Track>>> {
        const int batch = ++fm_calls;
        if (batch == 1) {
            Track first = test::Service::song(0);
            first.id = "radio-0";
            co_return std::vector<Track>{std::move(first)};
        }
        co_await Hold{pending_fm};
        Track second = test::Service::song(1);
        second.id = "radio-1";
        co_return std::vector<Track>{std::move(second)};
    };
    std::vector<std::string> started;
    std::vector<std::coroutine_handle<>> pending_streams;
    struct HoldAll {
        std::vector<std::coroutine_handle<>> &slots;
        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            slots.push_back(h);
        }
        void await_resume() const noexcept {}
    };
    scene.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        started.push_back(track.id);
        co_await HoldAll{pending_streams};
        co_return std::unexpected("Stopped");
    };

    scene.shell.browser.open(Page::fm);
    scene.draw();
    auto &shelf = scene.shell.library.shelf(Source::netease);
    REQUIRE(shelf.radio.size() == 1);
    scene.shell.playback.play_radio(Source::netease, shelf.radio, 0);
    REQUIRE(started.size() == 1);
    CHECK(started.back() == "radio-0");

    // Skipping at the end of the batch must not replay radio-0 while batch 2 is still in flight.
    scene.shell.playback.skip(1);
    CHECK(fm_calls == 2);
    CHECK(shelf.radio_pos == 0);
    CHECK(shelf.radio_pending);
    CHECK(started.size() == 1);
    CHECK_FALSE(scene.shell.playback.playing());

    // Once batch 2 arrives, the radio moves on and starts playing the new track automatically.
    REQUIRE(pending_fm);
    std::exchange(pending_fm, nullptr).resume();
    CHECK(shelf.radio_pos == 1);
    CHECK_FALSE(shelf.radio_pending);
    REQUIRE(scene.shell.playback.current() != nullptr);
    CHECK(scene.shell.playback.current()->id == "radio-1");
    REQUIRE(started.size() == 2);
    CHECK(started.back() == "radio-1");
    scene.shell.playback.stop();
    for (auto h : pending_streams) {
        h.resume();
    }
}

TEST_CASE("player bar context menu ignores page multi-selection") {
    gfx::select_driver(GpuChoice::integrated);
    Track playing = test::Service::song(4);
    playing.id = "playing-track";
    Scene scene({.queue = {playing}, .current = 0});
    scene.shell.browser.open(Page::daily);
    scene.draw();
    auto &list = scene.shell.browser.route().list;
    REQUIRE(list != nullptr);
    REQUIRE(list->tracks.size() >= 2);
    scene.shell.browser.choose(list->tracks, 0, false);
    scene.shell.browser.choose(list->tracks, 1, false);
    REQUIRE(scene.shell.browser.selection().choosing);
    REQUIRE(scene.shell.browser.selection().chosen.size() == 2);
    scene.draw();

    // Right-click the player bar cover (bottom-left of the window).
    const auto menu = scene.click(40, 760 - 40, 1180, 760, BTN_RIGHT);
    CHECK(drew(menu, "Play next"));
    CHECK(drew(menu, "Copy title and artist"));
    CHECK_FALSE(drew(menu, "Play selected"));
}

TEST_CASE("browser history restores previous search query and results") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.draw();

    scene.shell.browser.query = "alpha";
    scene.shell.browser.search();
    const auto first = scene.draw();
    CHECK(scene.shell.browser.searched() == "alpha");
    CHECK(drew(first, "\"alpha\""));
    CHECK(drew(first, "Result for alpha"));

    scene.shell.browser.query = "beta";
    scene.shell.browser.search();
    const auto second = scene.draw();
    CHECK(scene.shell.browser.searched() == "beta");
    CHECK(drew(second, "\"beta\""));
    CHECK(drew(second, "Result for beta"));

    REQUIRE(scene.shell.browser.back());
    const auto back = scene.draw();
    CHECK(scene.shell.browser.query == "alpha");
    CHECK(scene.shell.browser.searched() == "alpha");
    CHECK(drew(back, "\"alpha\""));
    CHECK(drew(back, "Result for alpha"));
    CHECK_FALSE(drew(back, "Result for beta"));

    REQUIRE(scene.shell.browser.forward());
    const auto fwd = scene.draw();
    CHECK(scene.shell.browser.query == "beta");
    CHECK(scene.shell.browser.searched() == "beta");
    CHECK(drew(fwd, "\"beta\""));
    CHECK(drew(fwd, "Result for beta"));
}

TEST_CASE("picture preview stops spinning and shows an error state when loading fails") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.draw();
    scene.shell.dialogs.show_picture(scene.ui, "", 1.f);
    const auto runs = scene.draw();
    CHECK(drew(runs, "Could not load this"));
    CHECK_FALSE(scene.ui.animating());
}

TEST_CASE("settings switches categories cleanly across narrow, medium and ultrawide windows") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.queue = {test::Service::song(0)}, .current = 0});
    scene.shell.browser.open(Page::settings);
    for (auto [w, h] : std::to_array<std::pair<float, float>>({{760, 600}, {1180, 760}, {2400, 1360}})) {
        CAPTURE(w);
        CAPTURE(h);
        scene.draw(w, h);
        const auto appearance = scene.draw(w, h);
        CHECK(drew(appearance, "Settings"));
        CHECK(drew(appearance, "Accent color"));
        CHECK_FALSE(drew(appearance, "Audio quality"));

        const auto *playback_tab = scene.find("Playback");
        REQUIRE(playback_tab != nullptr);
        const auto playback = scene.click(*playback_tab, w, h);
        CHECK(drew(playback, "Audio quality"));
        CHECK(drew(playback, "Skip previews"));
        CHECK_FALSE(drew(playback, "Accent color"));

        const auto *lyrics_tab = scene.find("Lyrics");
        REQUIRE(lyrics_tab != nullptr);
        const auto lyrics = scene.click(*lyrics_tab, w, h);
        CHECK(drew(lyrics, "Desktop lyrics"));
        CHECK(drew(lyrics, "Floating lyrics"));
        CHECK(drew(lyrics, "Dynamic Island"));

        const auto *general_tab = scene.find("General");
        REQUIRE(general_tab != nullptr);
        const auto general = scene.click(*general_tab, w, h);
        CHECK(drew(general, "Language"));
        CHECK(drew(general, "Config file"));

        // Return to Appearance for the next viewport size.
        const auto *back_tab = scene.find("Appearance");
        REQUIRE(back_tab != nullptr);
        scene.click(*back_tab, w, h);
    }
}

TEST_CASE("profile layout adapts cleanly across narrow, medium and ultrawide windows") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open_own_profile();
    for (auto [w, h] : std::to_array<std::pair<float, float>>({{760, 600}, {1180, 760}, {2400, 1360}})) {
        CAPTURE(w);
        CAPTURE(h);
        scene.draw(w, h);
        const auto runs = scene.draw(w, h);
        CHECK(drew(runs, "Mira Chen"));
        CHECK(drew(runs, "Following"));
        CHECK(drew(runs, "Followers"));
        if (w >= 1180) {
            CHECK(drew(runs, "On repeat"));
        }
    }
}
