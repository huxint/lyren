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
        CHECK(drew(playback, "FLAC · 16-bit"));
        CHECK(drew(playback, "Skip previews"));
        CHECK_FALSE(drew(playback, "Accent color"));

        const auto *lyrics_tab = scene.find("Lyrics");
        REQUIRE(lyrics_tab != nullptr);
        const auto lyrics = scene.click(*lyrics_tab, w, h);
        CHECK(drew(lyrics, "Desktop lyrics"));
        CHECK(drew(lyrics, "Floating lyrics"));
        CHECK(drew(lyrics, "Dynamic Island"));

        const auto *services_tab = scene.find("Services");
        REQUIRE(services_tab != nullptr);
        const auto services = scene.click(*services_tab, w, h);
        CHECK(drew(services, "NetEase Music"));
        CHECK(drew(services, "QQ Music"));
        CHECK(drew(services, "Qishui Music"));
        if (h >= 760) {
            CHECK(drew(services, "Kugou Music"));
            CHECK(drew(services, "Bodian Music"));
        }

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
        CHECK(drew(runs, "Overview"));
        CHECK(drew(runs, "Collection"));
        CHECK(drew(runs, "People"));
        CHECK(drew(runs, "Following"));
        CHECK(drew(runs, "Followers"));
        if (w >= 1180) {
            CHECK(drew(runs, "On repeat"));
        }
    }
}

TEST_CASE("profile head keeps badges and counts inside a narrow window") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open_own_profile();
    constexpr float w = 420, h = 700;
    scene.draw(w, h);
    const auto runs = scene.draw(w, h);
    CHECK(drew(runs, "Mira Chen"));
    // With no room beside the name the badges stand among the facts, and the post button is its icon alone.
    CHECK(drew(runs, "VIP"));
    CHECK(drew(runs, "Lv.8"));
    CHECK_FALSE(drew(runs, "New post"));
    CHECK(scene.find("square-pen", true) != nullptr);
    for (const std::string_view label : {"Following", "Followers", "Posts", "Songs played"}) {
        CAPTURE(label);
        const auto *run = scene.find(label);
        REQUIRE(run != nullptr);
        CHECK_FALSE(run->cut);
        CHECK(run->box.x >= 16 - 1);
        CHECK(run->box.right() <= w - 16 + 1);
    }
}

TEST_CASE("a press held on one listener's follow button does not land on the next listener's") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    auto followed = [&] {
        return std::ranges::count_if(scene.service.calls, [](const std::string &call) {
            return call.starts_with("follow ");
        });
    };
    scene.shell.browser.open_profile(Source::netease, std::string(test::Service::friend_id), "Ada Lovelace", "");
    scene.draw();
    scene.draw();
    const auto *follow = scene.find("Follow");
    REQUIRE(follow != nullptr);
    const float x = follow->box.x + follow->box.w / 2, y = follow->box.y + follow->box.h / 2;
    scene.button(x, y, true);
    scene.shell.browser.open_profile(Source::netease, "user-2", "Grace", "");
    scene.draw();
    scene.button(x, y, false);
    scene.draw();
    CHECK(followed() == 0);
    // The same button, pressed and released on the page it belongs to, follows them.
    REQUIRE(scene.find("Follow") != nullptr);
    scene.button(x, y, true);
    scene.button(x, y, false);
    scene.draw();
    CHECK(followed() == 1);
}

TEST_CASE("profile refresh menu only refreshes the visit it was opened on") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open_profile(Source::netease, std::string(test::Service::friend_id), "Ada Lovelace", "");
    scene.draw();
    scene.draw();
    const auto *more = scene.find("ellipsis", true);
    REQUIRE(more != nullptr);
    REQUIRE(more->box.y < 300);
    scene.click(*more);
    const auto *refresh = scene.find("Refresh");
    REQUIRE(refresh != nullptr);
    const float rx = refresh->box.x + refresh->box.w / 2, ry = refresh->box.y + refresh->box.h / 2;

    // Navigating away does not dismiss an open menu: choosing Refresh from Ada's menu must leave Grace's visit alone.
    scene.shell.browser.open_profile(Source::netease, "user-2", "Grace", "");
    scene.draw();
    REQUIRE(scene.ui.menu_open());
    const auto grace = scene.shell.browser.route().profile;
    REQUIRE(grace != nullptr);
    scene.click(rx, ry);
    CHECK(scene.shell.browser.route().profile == grace);

    // Refresh opened on Grace's own visit replaces it.
    more = scene.find("ellipsis", true);
    REQUIRE(more != nullptr);
    scene.click(*more);
    refresh = scene.find("Refresh");
    REQUIRE(refresh != nullptr);
    scene.click(*refresh);
    CHECK(scene.shell.browser.route().profile != grace);
}

TEST_CASE("a profile head navigation click stops later clicks in the same frame from touching the old profile") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.service.capabilities.posts = false;
    scene.shell.browser.open_own_profile();
    scene.draw();
    scene.draw();
    const auto page = scene.shell.browser.route().profile;
    REQUIRE(page != nullptr);
    REQUIRE(page->info.has_value());
    // With listened hidden, the four head stats are Following, Followers, Liked songs, Playlists.
    page->info->listened = -1;
    scene.draw();
    CHECK(page->section == Social::Section::overview);

    const auto *liked = scene.find("Liked songs");
    const auto *people_tab = scene.find("People");
    REQUIRE(liked != nullptr);
    REQUIRE(people_tab != nullptr);
    const float lx = liked->box.x + liked->box.w / 2, ly = liked->box.y + liked->box.h / 2;
    const float px = people_tab->box.x + people_tab->box.w / 2, py = people_tab->box.y + people_tab->box.h / 2;

    // A second click in the same frame on the old page's Playlists stat or People tab must be ignored.
    const ui::Painter::Run *playlists_stat = nullptr;
    for (const auto &run : scene.painter.runs()) {
        if (!run.icon && run.text == "Playlists" && std::abs(run.box.y - liked->box.y) < 12) {
            playlists_stat = &run;
            break;
        }
    }
    REQUIRE(playlists_stat != nullptr);
    const float sx = playlists_stat->box.x + playlists_stat->box.w / 2, sy = playlists_stat->box.y + playlists_stat->box.h / 2;

    scene.clicks({{lx, ly}, {sx, sy}, {px, py}});
    CHECK(scene.shell.browser.route().page == Page::liked);
    CHECK(page->section == Social::Section::overview);
}

TEST_CASE("switching profile tabs while scrolled restores the section top") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open_own_profile();
    constexpr float w = 1180, h = 460;
    scene.draw(w, h);
    scene.draw(w, h);
    const ui::Id scroll = ui::sub_id(scene.ui.id("profile"), scene.shell.browser.route().serial);
    scene.ui.scroll_to(scroll, 380.f);
    scene.draw(w, h);
    REQUIRE(scene.ui.scroll_offset(scroll) > 260.f);
    const auto *col_tab = scene.find("Collection");
    REQUIRE(col_tab != nullptr);
    scene.click(*col_tab, w, h);
    CHECK(scene.shell.browser.route().profile->section == Social::Section::collection);
    CHECK(scene.ui.scroll_offset(scroll) <= 220.f);
}

TEST_CASE("comments docked composer expands, cancels reply safely, posts, and isolates visits") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.open_comments(Source::netease, Subject::song, "song-0", "Night Train · Mira Chen", "");
    scene.draw();
    const auto initial = scene.draw();
    CHECK(drew(initial, "Top comment"));
    CHECK(drew(initial, "What does this music bring to mind?"));

    // Reply opens the docked composer; cancelling the reply in the same frame must not dereference freed memory.
    const auto *reply_btn = scene.find("Reply");
    REQUIRE(reply_btn != nullptr);
    const auto replying = scene.click(*reply_btn);
    CHECK(drew(replying, "Replying to Ada Lovelace"));
    CHECK(drew(replying, "Post"));

    const ui::Painter::Run *cancel_reply = nullptr;
    for (const auto &run : scene.painter.runs()) {
        if (run.icon && run.text == "x" && run.box.y > 400) {
            cancel_reply = &run;
            break;
        }
    }
    REQUIRE(cancel_reply != nullptr);
    const auto after_cancel = scene.click(*cancel_reply);
    CHECK_FALSE(drew(after_cancel, "Replying to Ada Lovelace"));
    CHECK(drew(after_cancel, "Post"));

    // Typing and clicking Post sends the comment and collapses the dock.
    scene.shell.comments.thread().draft->text = "Great bridge.";
    scene.draw();
    const auto *post_btn = scene.find("Post");
    REQUIRE(post_btn != nullptr);
    scene.click(*post_btn);
    CHECK(std::ranges::any_of(scene.service.calls, [](std::string_view c) {
        return c == "post-comment song-0 - Great bridge.";
    }));
    CHECK_FALSE(scene.shell.comments.thread().draft->expanded);

    // A held press on song-0's composer trigger must not expand song-1's composer after a subject switch.
    const auto *trigger = scene.find("What does this music bring to mind?");
    REQUIRE(trigger != nullptr);
    const float tx = trigger->box.x + trigger->box.w / 2, ty = trigger->box.y + trigger->box.h / 2;
    scene.button(tx, ty, true);
    scene.shell.open_comments(Source::netease, Subject::song, "song-1", "Harbour · Mira Chen", "");
    scene.draw();
    scene.button(tx, ty, false);
    scene.draw();
    CHECK_FALSE(scene.shell.comments.thread().draft->expanded);

    // Opening replies shows the pinned root comment and replies, and Back returns to the comment list.
    const auto *replies_btn = scene.find("2 replies");
    REQUIRE(replies_btn != nullptr);
    const auto in_replies = scene.click(*replies_btn);
    CHECK(drew(in_replies, "Replies"));
    CHECK(drew(in_replies, "Same here."));
    const ui::Painter::Run *back = nullptr;
    for (const auto &run : scene.painter.runs()) {
        if (run.icon && run.text == "chevron-left" && run.box.x > 700) {
            back = &run;
            break;
        }
    }
    REQUIRE(back != nullptr);
    const auto back_list = scene.click(*back);
    CHECK(drew(back_list, "Popular"));
}

TEST_CASE("comment delete confirmation targets the subject where Delete was clicked") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.open_comments(Source::netease, Subject::song, "song-0", "Night Train · Mira Chen", "");
    scene.draw();
    scene.draw();
    const auto *trash = scene.find("trash", true);
    REQUIRE(trash != nullptr);
    const auto modal = scene.click(*trash);
    CHECK(drew(modal, "Delete comment?"));

    // Switch to another song before confirming deletion.
    scene.shell.open_comments(Source::netease, Subject::song, "song-1", "Harbour · Mira Chen", "");
    scene.draw();
    const auto *confirm = scene.find("Delete");
    REQUIRE(confirm != nullptr);
    scene.click(*confirm);
    CHECK(std::ranges::any_of(scene.service.calls, [](std::string_view c) {
        return c == "delete-comment song-0 c3";
    }));
    CHECK_FALSE(std::ranges::any_of(scene.service.calls, [](std::string_view c) {
        return c.starts_with("delete-comment song-1 ");
    }));
}

TEST_CASE("profile overview, collection, people tabs and stat shortcuts work cleanly") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open_own_profile();
    scene.draw();
    const auto overview = scene.draw();
    REQUIRE(scene.shell.browser.route().profile != nullptr);
    CHECK(scene.shell.browser.route().profile->section == Social::Section::overview);
    CHECK_FALSE(drew(overview, "QQ Music"));
    CHECK_FALSE(drew(overview, "Qishui"));
    CHECK_FALSE(drew(overview, "Bodian"));
    CHECK(drew(overview, "Daily mix"));
    CHECK(drew(overview, "Recently played"));
    CHECK(drew(overview, "Mixtape 1"));
    CHECK(drew(overview, "On repeat"));

    // Clicking the Followers stat jumps straight into the People section on the Followers sub-tab.
    const auto *followers_stat = scene.find("Followers");
    REQUIRE(followers_stat != nullptr);
    const auto fans = scene.click(*followers_stat);
    CHECK(scene.shell.browser.route().profile->section == Social::Section::people);
    CHECK(scene.shell.browser.route().profile->people_tab == 1);
    CHECK(drew(fans, "Tom"));

    // Switching to the Collection tab shows saved albums and lets the user toggle to followed artists.
    const auto *col_tab = scene.find("Collection");
    REQUIRE(col_tab != nullptr);
    const auto albums = scene.click(*col_tab);
    CHECK(scene.shell.browser.route().profile->section == Social::Section::collection);
    CHECK(drew(albums, "Harbour Tapes"));

    const auto *artists_btn = scene.find("Artists");
    REQUIRE(artists_btn != nullptr);
    const auto artists = scene.click(*artists_btn);
    CHECK(scene.shell.browser.route().profile->collection_tab == 1);
    CHECK(drew(artists, "YOASOBI"));

    // Switching platforms while on own profile stays on own profile when signed in on the target service.
    spawn(scene.shell.library.load(Source::qq));
    scene.shell.browser.switch_platform(Source::qq);
    const auto switched = scene.draw();
    CHECK(scene.shell.browser.active() == Source::qq);
    CHECK(scene.shell.browser.route().page == Page::profile);
    CHECK(scene.shell.browser.route().card.source == Source::qq);
    CHECK(scene.shell.browser.route().card.owned);
    CHECK(drew(switched, "Mira Chen"));
}

TEST_CASE("profile and settings morph shared components across window resizes and settle idle") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.motion = 1.0, .queue = {test::Service::song(0)}, .current = 0});

    // 1. Profile "On repeat" morphs smoothly from the right studio rail into the narrow inline column.
    scene.shell.browser.open_own_profile();
    for (int i = 0; i < 6; ++i) {
        scene.draw(1180, 760);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *wide_rep = scene.find("On repeat");
    REQUIRE(wide_rep != nullptr);
    const float wide_x = wide_rep->box.x;

    // Cross the breakpoint into a narrow viewport and sample mid-flight (100ms later).
    scene.draw_dt(760, 600, 0.016);
    CHECK(scene.ui.animating());
    scene.draw_dt(760, 600, 0.09);
    const auto *mid_rep = scene.find("On repeat");
    REQUIRE(mid_rep != nullptr);
    const float mid_x = mid_rep->box.x;

    // Settle in the narrow viewport.
    for (int i = 0; i < 6; ++i) {
        scene.draw(760, 600);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *narrow_rep = scene.find("On repeat");
    REQUIRE(narrow_rep != nullptr);
    const float narrow_x = narrow_rep->box.x;
    CHECK(wide_x > narrow_x + 200);
    CHECK(mid_x < wide_x);
    CHECK(mid_x > narrow_x);

    // 2. Settings morphs smoothly between left rail and top tabs, and every tab settles to zero idle animation.
    scene.shell.browser.open(Page::settings);
    for (int i = 0; i < 6; ++i) {
        scene.draw(1180, 760);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *wide_title = scene.find("Settings");
    REQUIRE(wide_title != nullptr);
    const float wide_title_x = wide_title->box.x;

    scene.draw_dt(760, 600, 0.016);
    CHECK(scene.ui.animating());
    scene.draw_dt(760, 600, 0.09);
    const auto *mid_title = scene.find("Settings");
    REQUIRE(mid_title != nullptr);
    const float mid_title_x = mid_title->box.x;

    for (int i = 0; i < 6; ++i) {
        scene.draw(760, 600);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *narrow_title = scene.find("Settings");
    REQUIRE(narrow_title != nullptr);
    const float narrow_title_x = narrow_title->box.x;
    CHECK(mid_title_x != wide_title_x);
    CHECK(mid_title_x != narrow_title_x);

    // Every settings category must settle completely idle when paused (zero background CPU/GPU burn).
    for (int i = 0; i < 6; ++i) {
        scene.draw(1180, 760);
    }
    for (std::string_view tab : {"Playback", "Lyrics", "Services", "General", "Appearance"}) {
        const auto *btn = scene.find(tab);
        REQUIRE(btn != nullptr);
        scene.click(*btn, 1180, 760);
        for (int i = 0; i < 6; ++i) {
            scene.draw(1180, 760);
        }
        CAPTURE(tab);
        CHECK_FALSE(scene.ui.animating());
    }
}

TEST_CASE("unliking a song on the liked songs page defers list erasure until the frame finishes") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.browser.open(Page::liked);
    scene.draw();
    const auto before = scene.draw();
    REQUIRE(scene.shell.browser.route().list != nullptr);
    const size_t initial_count = scene.shell.browser.route().list->tracks.size();
    REQUIRE(initial_count >= 2);
    CHECK(drew(before, "Night Train"));

    const ui::Painter::Run *row_heart = nullptr;
    for (const auto &run : scene.painter.runs()) {
        if (run.icon && run.text == "heart-fill" && run.box.x > 800 && run.box.y > 220 && run.box.y < 640) {
            row_heart = &run;
            break;
        }
    }
    REQUIRE(row_heart != nullptr);
    scene.click(*row_heart);
    CHECK(std::ranges::any_of(scene.service.calls, [](std::string_view c) {
        return c.starts_with("like ") && c.ends_with(" false");
    }));
    CHECK(scene.shell.browser.route().list->tracks.size() == initial_count - 1);
    scene.draw();
}

TEST_CASE("profile collection failure renders error once without retrying every frame") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    int album_requests = 0;
    scene.service.on_collected_albums = [&]() -> Task<Result<std::vector<Card>>> {
        ++album_requests;
        co_return std::unexpected("Collection service unavailable");
    };
    scene.shell.browser.open_own_profile();
    scene.draw();
    scene.draw();
    album_requests = 0;
    const auto *col_tab = scene.find("Collection");
    REQUIRE(col_tab != nullptr);
    auto runs = scene.click(*col_tab);
    for (int i = 0; i < 6; ++i) {
        runs = scene.draw();
    }
    CHECK(album_requests == 1);
    CHECK(drew(runs, "Collection service unavailable"));
    CHECK_FALSE(scene.ui.animating());
}

TEST_CASE("card grid keeps springs warm at rest and glides smoothly across column breakpoints") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.motion = 1.0});
    for (int i = 0; i < 8; ++i) {
        scene.draw(1400, 1250);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *wide_card = scene.find("Late Night Tape 5");
    REQUIRE(wide_card != nullptr);
    const ui::Rect wide_box = wide_card->box;

    scene.draw_dt(760, 1250, 0.016);
    CHECK(scene.ui.animating());
    scene.draw_dt(760, 1250, 0.08);
    const auto *mid_card = scene.find("Late Night Tape 5");
    REQUIRE(mid_card != nullptr);
    const ui::Rect mid_box = mid_card->box;

    for (int i = 0; i < 8; ++i) {
        scene.draw(760, 1250);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *narrow_card = scene.find("Late Night Tape 5");
    REQUIRE(narrow_card != nullptr);
    const ui::Rect narrow_box = narrow_card->box;

    // Crossing the column breakpoint must animate through an intermediate position instead of teleporting on frame 1.
    const bool moved_x = std::abs(wide_box.x - narrow_box.x) > 20.f;
    const bool moved_y = std::abs(wide_box.y - narrow_box.y) > 20.f;
    REQUIRE((moved_x || moved_y));
    if (moved_x) {
        CHECK(std::abs(mid_box.x - wide_box.x) > 1.f);
        CHECK(std::abs(mid_box.x - narrow_box.x) > 1.f);
    }
    if (moved_y) {
        CHECK(std::abs(mid_box.y - wide_box.y) > 1.f);
        CHECK(std::abs(mid_box.y - narrow_box.y) > 1.f);
    }
}

TEST_CASE("track context menu Play action owns a snapshot and survives source list mutation") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    std::vector<std::string> attempted;
    scene.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        attempted.push_back(track.id);
        co_return std::unexpected("Stopped");
    };
    scene.shell.browser.open(Page::daily);
    scene.draw();
    scene.draw();
    const auto *title = scene.find("Night Train");
    REQUIRE(title != nullptr);

    const auto menu = scene.click(title->box.x + 10, title->box.y + 8, 1180, 760, BTN_RIGHT);
    CHECK(drew(menu, "Play"));
    const auto *play_item = scene.find("Play");
    REQUIRE(play_item != nullptr);

    // Mutate and reallocate the backing list while the popup menu is open before clicking Play.
    REQUIRE(scene.shell.browser.route().list != nullptr);
    scene.shell.browser.route().list->tracks.clear();
    scene.shell.browser.route().list->tracks.shrink_to_fit();

    scene.click(*play_item);
    REQUIRE_FALSE(attempted.empty());
    CHECK(attempted.front() == "song-0");
}

TEST_CASE("structured session-import account dialog unpacks JSON, diagnoses steps, guards empty URL, and restores custom identity") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.signed_in = false});
    scene.service.capabilities.qr_login = false;
    scene.service.capabilities.session_import = true;

    int import_calls = 0;
    std::string last_imported;
    scene.service.on_check_session = [&](std::string) -> Task<SessionCheck> {
        SessionCheck c;
        c.signer = {SessionCheck::State::ok, "http://127.0.0.1:8000"};
        c.identity = {SessionCheck::State::ok, "device_id custom-did"};
        c.account = {SessionCheck::State::ok, "Mira Chen (me)"};
        co_return c;
    };
    scene.service.on_import_session = [&](std::string json) -> Task<Result<void>> {
        ++import_calls;
        last_imported = std::move(json);
        scene.service.session()["signer_url"] = "http://127.0.0.1:8000";
        scene.service.session()["cookie_jar"] = ".qishui.com\tTRUE\t/\tTRUE\t0\tsessionid\tabc\n";
        scene.service.session()["custom_device_id"] = "custom-did";
        scene.service.session()["custom_iid"] = "custom-iid";
        scene.service.signed_in = true;
        co_return Result<void>{};
    };

    scene.draw();
    scene.shell.account.open(scene.ui);
    const auto list_runs = scene.draw();
    CHECK(drew(list_runs, "Not signed in · Anonymous playback ready"));
    const auto *connect_acct = scene.find("Connect account");
    REQUIRE(connect_acct != nullptr);
    const auto form_runs = scene.click(*connect_acct);
    CHECK(drew(form_runs, "Signing service URL"));
    CHECK(drew(form_runs, "Test connection"));

    // Paste full JSON into the cookie field; the next frame auto-unpacks it into structured fields and opens advanced identity.
    scene.shell.accounts.sign_in().cookie = R"({"signer_url":"http://127.0.0.1:8000","cookie":"sessionid=abc","device_id":"custom-did","iid":"custom-iid"})";
    scene.draw();
    CHECK(scene.shell.accounts.sign_in().signer_url == "http://127.0.0.1:8000");
    CHECK(scene.shell.accounts.sign_in().cookie == "sessionid=abc");
    CHECK(scene.shell.accounts.sign_in().device_id == "custom-did");
    CHECK(scene.shell.accounts.sign_in().iid == "custom-iid");
    CHECK(scene.shell.accounts.sign_in().show_advanced);

    const auto *test_btn = scene.find("Test connection");
    REQUIRE(test_btn != nullptr);
    scene.click(*test_btn);
    const auto diag_runs = scene.draw();
    CHECK(drew(diag_runs, "Signing service"));
    CHECK(drew(diag_runs, "Device identity"));
    CHECK(drew(diag_runs, "Account session"));
    CHECK(scene.shell.accounts.sign_in().check.has_value());

    // Clearing signer_url clears stale diagnostics and disables Connect instead of importing stale settings.
    scene.shell.accounts.sign_in().signer_url.clear();
    scene.draw();
    const auto *connect_btn = scene.find("Connect");
    REQUIRE(connect_btn != nullptr);
    scene.click(*connect_btn);
    CHECK(import_calls == 0);

    // Restoring signer_url and clicking Connect imports and signs in; Edit connection restores custom device identity.
    scene.shell.accounts.sign_in().signer_url = "http://127.0.0.1:8000";
    scene.draw();
    connect_btn = scene.find("Connect");
    REQUIRE(connect_btn != nullptr);
    scene.click(*connect_btn);
    CHECK(import_calls == 1);
    CHECK(last_imported.contains("custom-did"));
    CHECK(scene.service.signed_in);

    scene.shell.accounts.begin(Source::qishui);
    CHECK(scene.shell.accounts.sign_in().signer_url == "http://127.0.0.1:8000");
    CHECK(scene.shell.accounts.sign_in().device_id == "custom-did");
    CHECK(scene.shell.accounts.sign_in().iid == "custom-iid");
    CHECK(scene.shell.accounts.sign_in().show_advanced);
}

TEST_CASE("profile studio rail stays invariant across vertical scroll and transitions monotonically across window heights") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.motion = 1.0});
    scene.shell.browser.open_own_profile();
    for (int i = 0; i < 8; ++i) {
        scene.draw(1180, 620);
    }
    CHECK_FALSE(scene.ui.animating());

    const auto page = scene.shell.browser.route().profile;
    REQUIRE(page != nullptr);
    REQUIRE_FALSE(page->charts[0].ranking.tracks.empty());
    REQUIRE(page->people[0].items.size() >= 2);
    const std::string first_person = page->people[0].items[0].name;
    const std::string second_person = page->people[0].items[1].name;

    auto count_repeat_ranks = [&](std::span<const ui::Painter::Run> runs) {
        int ranks = 0;
        for (int r = 1; r <= 5; ++r) {
            const std::string s = std::to_string(r);
            const bool found = std::ranges::any_of(runs, [&](const ui::Painter::Run &run) {
                return !run.icon && run.text == s && run.box.x > 760.f;
            });
            if (found) {
                ++ranks;
            }
        }
        return ranks;
    };

    // At desktop height (920), both On repeat and Following list rows are visible from scroll = 0 and stay invariant across the entire scroll.
    for (int i = 0; i < 8; ++i) {
        scene.draw(1180, 920);
    }
    CHECK_FALSE(scene.ui.animating());
    auto base_runs = scene.draw(1180, 920);
    const int base_ranks = count_repeat_ranks(base_runs);
    REQUIRE(base_ranks == 5);
    REQUIRE(drew(base_runs, first_person));
    REQUIRE(drew(base_runs, second_person));

    const ui::Id scroll = ui::sub_id(scene.ui.id("profile"), scene.shell.browser.route().serial);
    float prev_person_y = scene.find(first_person)->box.y;
    for (int step = 1; step <= 12; ++step) {
        scene.ui.scroll_to(scroll, static_cast<float>(step) * 32.f);
        const auto runs = scene.draw(1180, 920);
        CHECK(count_repeat_ranks(runs) == base_ranks);
        REQUIRE(drew(runs, first_person));
        CHECK(drew(runs, second_person));
        const float y = scene.find(first_person)->box.y;
        // Moves monotonically upward until the tabs/rail pin, then stays completely still without jitter.
        CHECK(y <= prev_person_y + 0.01f);
        prev_person_y = y;
    }
    const float pinned_y = prev_person_y;
    scene.ui.scroll_to(scroll, 440.f);
    scene.draw(1180, 920);
    CHECK(std::abs(scene.find(first_person)->box.y - pinned_y) < 0.01f);

    for (int step = 12; step >= 0; --step) {
        scene.ui.scroll_to(scroll, static_cast<float>(step) * 32.f);
        const auto runs = scene.draw(1180, 920);
        CHECK(count_repeat_ranks(runs) == base_ranks);
        CHECK(drew(runs, first_person));
        CHECK(drew(runs, second_person));
    }
    CHECK_FALSE(scene.ui.animating());

    // Across every window height from 520 to 960 (pinned), once Following upgrades to list mode it never drops back to avatar-only when On repeat grows.
    scene.ui.scroll_to(scroll, 360.f);
    bool saw_list = false;
    int prev_ranks = 0;
    for (int h = 520; h <= 960; h += 28) {
        auto runs = scene.draw(1180, h);
        for (int settle = 0; settle < 8; ++settle) {
            runs = scene.draw(1180, h);
        }
        const int ranks = count_repeat_ranks(runs);
        const bool is_list = drew(runs, first_person) && drew(runs, second_person);
        if (saw_list) {
            CHECK(is_list);
        }
        saw_list |= is_list;
        CHECK(ranks >= prev_ranks);
        prev_ranks = ranks;
    }
    CHECK(saw_list);

    // Shrinking down to a compact height triggers smooth spring/fade animation rather than an instant 1-frame snap.
    scene.draw_dt(1180, 380, 0.016);
    CHECK(scene.ui.animating());
    for (int i = 0; i < 10; ++i) {
        scene.draw(1180, 380);
    }
    CHECK_FALSE(scene.ui.animating());
}
