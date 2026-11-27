#include <doctest/doctest.h>
#include <xkbcommon/xkbcommon-keysyms.h>

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

TEST_CASE("navigation renders the current page and content in both themes") {
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
        CHECK(drew(comments, "Popular"));
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
    CHECK(drew(menu, "Play"));
    CHECK(drew(menu, "Play next"));
    CHECK(drew(menu, "Copy title and artist"));
    // The page's multi-select bar at the top draws "Play selected", but the player bar popup menu must not.
    CHECK(std::ranges::none_of(menu, [](const ui::Painter::Run &run) {
        return !run.icon && run.text == "Play selected" && run.box.y > 400;
    }));
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
        CHECK(drew(general, "Close to background"));
        if (h >= 760) {
            CHECK(drew(general, "Config file"));
        }

        // Return to Appearance for the next viewport size.
        const auto *back_tab = scene.find("Appearance");
        REQUIRE(back_tab != nullptr);
        scene.click(*back_tab, w, h);
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

TEST_CASE("playlist and album comment buttons switch subjects and close the active panel") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.service.on_album = [](std::string) -> Task<Result<AlbumInfo>> {
        co_return AlbumInfo{.thread = "thread-id", .tracks = {test::Service::song(0)}};
    };
    auto click_comments = [&] {
        const auto *button = scene.find("message-square", true);
        REQUIRE(button);
        scene.click(*button);
    };
    auto &shell = scene.shell;
    shell.browser.open(Card{.id = "thread-id", .title = "Playlist"});
    scene.draw();
    click_comments();
    CHECK(shell.comments_open(Source::netease, Subject::playlist, "thread-id"));
    click_comments();
    CHECK(shell.panel.kind() == SidePanel::Kind::none);
    click_comments();

    shell.browser.open(Card{.kind = CardKind::album, .id = "album", .title = "Album"});
    scene.draw();
    REQUIRE(shell.comments_open(Source::netease, Subject::playlist, "thread-id"));
    click_comments();
    CHECK(shell.comments_open(Source::netease, Subject::album, "thread-id"));
    click_comments();
    CHECK(shell.panel.kind() == SidePanel::Kind::none);
}

TEST_CASE("comments docked composer expands, cancels reply safely, posts, and isolates visits") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.shell.open_comments(Source::netease, Subject::song, "song-0", "Night Train · Mira Chen", "");
    scene.draw();
    const auto initial = scene.draw();
    CHECK(drew(initial, "Ada Lovelace"));
    CHECK_FALSE(drew(initial, "Top comment"));
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

TEST_CASE("an empty playlist and a refused playlist use distinct page states") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    scene.service.on_tracks = [](Card card) -> Task<Result<std::vector<Track>>> {
        if (card.id == "private") {
            co_return std::unexpected("Access denied");
        }
        co_return std::vector<Track>{};
    };
    scene.shell.browser.open(Card{.id = "empty", .title = "Empty playlist"});
    auto runs = scene.draw();
    CHECK(drew(runs, "No songs here yet"));
    CHECK_FALSE(drew(runs, "Could not load this"));
    scene.shell.browser.open(Card{.id = "private", .title = "Private playlist"});
    runs = scene.draw();
    CHECK(drew(runs, "Could not load this"));
    CHECK(drew(runs, "Access denied"));
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

TEST_CASE("recommend sticky header morphs, latches, stays clickable above scrolled cards, and settles idle") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.motion = 1.0});
    auto &shelf = scene.shell.library.shelf(Source::netease);
    // Populate both daily playlists and recommended playlists so the page scrolls well past the hero section.
    shelf.daily_lists.push_back({.id = "daily-list-0", .title = "Morning Coffee", .count = 20, .count_kind = Card::Count::songs});
    for (int i = 6; i < 36; ++i) {
        shelf.recommended.cards.push_back({.id = std::format("rec-{}", i), .title = std::format("Late Night Tape {}", i + 1), .count = 18 + i, .count_kind = Card::Count::songs});
    }

    auto find_in_page = [](std::span<const ui::Painter::Run> runs, std::string_view text, bool icon = false, float min_x = 220.f) -> const ui::Painter::Run * {
        for (const auto &run : runs) {
            if (run.icon == icon && run.text == text && run.box.x >= min_x) {
                return &run;
            }
        }
        return nullptr;
    };

    // Wide layout (single-row hero strip -> continuous scroll-linked morph into sticky pills).
    std::span<const ui::Painter::Run> rest;
    for (int i = 0; i < 12; ++i) {
        rest = scene.draw(1440, 760);
    }
    CHECK_FALSE(scene.ui.animating());
    const auto *daily_rest = find_in_page(rest, "Daily mix");
    const auto *fm_rest = find_in_page(rest, "Personal FM");
    REQUIRE(daily_rest != nullptr);
    REQUIRE(fm_rest != nullptr);
    const float rest_y = daily_rest->box.y;
    CHECK(std::abs(rest_y - fm_rest->box.y) < 1.f);

    const ui::Id scroll = scene.ui.id("recommend");
    scene.ui.scroll_to(scroll, 104.f, false);
    const auto early = scene.draw(1440, 760);
    const auto *daily_early = find_in_page(early, "Daily mix");
    REQUIRE(daily_early);
    CHECK(daily_early->box.y < rest_y);

    // Mid-morph: cards stay cleanly separated without horizontal overlap and rise smoothly toward the top bar.
    scene.ui.scroll_to(scroll, 120.f, false);
    const auto mid = scene.draw(1440, 760);
    const auto *daily_mid = find_in_page(mid, "Daily mix");
    const auto *fm_mid = find_in_page(mid, "Personal FM");
    REQUIRE(daily_mid != nullptr);
    REQUIRE(fm_mid != nullptr);
    CHECK_FALSE(overlaps(daily_mid->box, fm_mid->box));
    CHECK(daily_mid->box.y < rest_y);
    CHECK_FALSE(drew(mid, "Fresh picks, every day"));

    // Fully docked: pills stay pinned at the top even as scroll keeps advancing.
    scene.ui.scroll_to(scroll, 320.f, false);
    const auto docked_a = scene.draw(1440, 760);
    const auto *daily_dock = find_in_page(docked_a, "Daily mix");
    const auto *fm_dock = find_in_page(docked_a, "Personal FM");
    REQUIRE(daily_dock != nullptr);
    REQUIRE(fm_dock != nullptr);
    CHECK_FALSE(daily_dock->cut);
    CHECK_FALSE(fm_dock->cut);
    CHECK_FALSE(overlaps(daily_dock->box, fm_dock->box));
    CHECK(daily_dock->box.y < 100.f);
    CHECK(std::abs(daily_dock->box.y - fm_dock->box.y) < 1.f);
    const float pinned_y = daily_dock->box.y;

    scene.ui.scroll_to(scroll, 520.f, false);
    const auto docked_b = scene.draw(1440, 760);
    const auto *daily_deeper = find_in_page(docked_b, "Daily mix");
    REQUIRE(daily_deeper != nullptr);
    CHECK(std::abs(daily_deeper->box.y - pinned_y) < 0.01f);
    CHECK_FALSE(scene.ui.animating());

    // Narrower multi-row layout: row 0 docks first while row 1 shrinks, slides rightward clear of row 0, and curves up
    // into its sticky slot with zero overlap at every intermediate scroll offset.
    for (int i = 0; i < 12; ++i) {
        scene.draw(760, 760);
    }
    for (const float step_off : {120.f, 142.f, 170.f, 192.f, 220.f, 240.f, 420.f}) {
        scene.ui.scroll_to(scroll, step_off, false);
        const auto frame = scene.draw(760, 760);
        const auto *d = find_in_page(frame, "Daily mix", false, 80.f);
        const auto *f = find_in_page(frame, "Personal FM", false, 80.f);
        const auto *rad = find_in_page(frame, "Radar", false, 80.f);
        REQUIRE(d != nullptr);
        REQUIRE(f != nullptr);
        REQUIRE(rad != nullptr);
        CHECK_FALSE(overlaps(d->box, f->box));
        CHECK_FALSE(overlaps(d->box, rad->box));
        CHECK_FALSE(overlaps(f->box, rad->box));
    }

    // Mid-flight on row 1 (off = 192): clicking Daily mix opens Daily mix rather than row 1 or any playlist below.
    scene.ui.scroll_to(scroll, 192.f, false);
    const auto mid_narrow = scene.draw(760, 760);
    const auto *mid_daily = find_in_page(mid_narrow, "Daily mix", false, 80.f);
    REQUIRE(mid_daily != nullptr);
    CHECK(mid_daily->box.y < 100.f);
    scene.click(*mid_daily, 760, 760);
    CHECK(scene.shell.browser.route().page == Page::daily);
    scene.shell.browser.back();
    scene.draw(760, 760);

    // Fully docked narrow layout: all 3 pills align on one sticky row and clicking Personal FM opens FM.
    scene.ui.scroll_to(scroll, 520.f, false);
    const auto narrow_docked = scene.draw(760, 760);
    const auto *narrow_daily = find_in_page(narrow_docked, "Daily mix", false, 80.f);
    const auto *narrow_fm = find_in_page(narrow_docked, "Personal FM", false, 80.f);
    const auto *narrow_radar = find_in_page(narrow_docked, "Radar", false, 80.f);
    REQUIRE(narrow_daily != nullptr);
    REQUIRE(narrow_fm != nullptr);
    REQUIRE(narrow_radar != nullptr);
    CHECK_FALSE(narrow_fm->cut);
    CHECK(narrow_fm->box.y < 100.f);
    CHECK(std::abs(narrow_daily->box.y - narrow_radar->box.y) < 1.f);
    scene.click(*narrow_fm, 760, 760);
    CHECK(scene.shell.browser.route().page == Page::fm);

    // Large font_size (32) and large radius (24): rest-state hero title/subtitle do not overlap, and compact day
    // numeral inside the docked pill does not truncate.
    Scene large({.font_size = 32.f, .radius = 24.f});
    auto &large_shelf = large.shell.library.shelf(Source::netease);
    for (int i = 6; i < 36; ++i) {
        large_shelf.recommended.cards.push_back(
            {.id = std::format("rec-{}", i), .title = std::format("Late Night Tape {}", i + 1), .count = 18 + i, .count_kind = Card::Count::songs});
    }
    const auto large_rest = large.draw(1440, 760);
    const auto *large_title = find_in_page(large_rest, "Daily mix");
    const auto *large_sub = find_in_page(large_rest, "Fresh picks, every day");
    REQUIRE(large_title != nullptr);
    REQUIRE(large_sub != nullptr);
    CHECK_FALSE(overlaps(large_title->box, large_sub->box));
    large.ui.scroll_to(large.ui.id("recommend"), 420.f, false);
    const auto large_docked = large.draw(1440, 760);
    const std::string today_day(large.shell.browser.today().substr(3));
    const auto *day_run = find_in_page(large_docked, today_day);
    REQUIRE(day_run != nullptr);
    CHECK_FALSE(day_run->cut);

    // Loading playlist covers render shimmering skeletons (animating while pending, without the empty-cover icon).
    Scene loading_covers({.motion = 1.0});
    auto &loading_shelf = loading_covers.shell.library.shelf(Source::netease);
    for (auto &c : loading_shelf.recommended.cards) {
        c.cover = "https://example.invalid/cover/" + c.id + ".jpg";
    }
    for (int i = 0; i < 8; ++i) {
        loading_covers.draw(1180, 760);
    }
    const auto loading_runs = loading_covers.draw(1180, 760);
    CHECK(loading_covers.ui.animating());
    const bool card_icon = std::ranges::any_of(loading_runs, [](const ui::Painter::Run &run) {
        return run.icon && run.text == "list-music" && run.box.x >= 220.f && run.box.y < 660.f;
    });
    CHECK_FALSE(card_icon);
}

TEST_CASE("an unknown album preference retries the collection before offering a write") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    const Card album{.kind = CardKind::album, .id = "saved-album", .title = "Saved album"};
    int reads = 0;
    scene.service.on_collected_albums = [&]() -> Task<Result<std::vector<Card>>> {
        if (++reads == 1) {
            co_return std::unexpected("Collection unavailable");
        }
        co_return std::vector{album};
    };
    scene.shell.browser.open(album);
    scene.draw();
    CHECK(scene.find("Save") == nullptr);
    const auto *retry = scene.find("Retry");
    REQUIRE(retry);
    scene.click(*retry);
    CHECK(reads == 2);
    CHECK(scene.find("Saved") != nullptr);
}

TEST_CASE("returning to a list restores its scroll and its refresh button reloads the list") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene;
    int reads = 0;
    scene.service.on_tracks = [&](Card) -> Task<Result<std::vector<Track>>> {
        ++reads;
        std::vector<Track> tracks;
        for (int i = 0; i < 24; ++i) {
            tracks.push_back(test::Service::song(i));
        }
        co_return tracks;
    };
    scene.shell.browser.open(Card{.id = "pl-1", .title = "Evening Mix"});
    scene.draw();
    const auto scroll = ui::sub_id(scene.ui.id("tracks"), scene.shell.browser.route().serial);
    scene.ui.scroll_to(scroll, 320.f);
    scene.draw();
    REQUIRE(scene.ui.scroll_offset(scroll) > 200.f);
    scene.shell.browser.open(Page::daily);
    scene.draw();
    const auto daily_scroll = ui::sub_id(scene.ui.id("tracks"), scene.shell.browser.route().serial);
    CHECK(scene.ui.scroll_offset(daily_scroll) == doctest::Approx(0.f));
    REQUIRE(scene.shell.browser.back());
    scene.draw();
    CHECK(scene.ui.scroll_offset(scroll) > 200.f);
    const auto *refresh = scene.find("refresh-cw", true);
    REQUIRE(refresh);
    scene.click(*refresh);
    CHECK(reads == 2);
}

TEST_CASE("profile timeline groups posts by sticky left year rail and yields cleanly across years") {
    gfx::select_driver(GpuChoice::integrated);
    auto make_ms = [](int year, int month, int day, int hour, int minute) -> int64_t {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_isdst = -1;
        return static_cast<int64_t>(std::mktime(&tm)) * 1000;
    };
    auto build_posts = [&](const Person &author) {
        std::vector<Post> out;
        auto add = [&](std::string id, int y, int m, int d, int hh, int mm, std::string text) {
            Post p = test::Service::post(std::move(id), author, std::move(text), false);
            p.time = make_ms(y, m, d, hh, mm);
            out.push_back(std::move(p));
        };
        add("y26-1", 2026, 5, 18, 21, 15, "Late spring rain over the river.\nSecond line to give this post room.");
        add("y26-2", 2026, 3, 9, 14, 30, "Afternoon vinyl session on the desk.");
        add("y26-3", 2026, 1, 22, 9, 5, "First track of the year, played twice.");
        add("y25-1", 2025, 11, 28, 22, 40, "November harbour fog and slow drums.");
        add("y25-2", 2025, 6, 7, 18, 20, "Summer evening tape.");
        add("y25-3", 2025, 1, 7, 8, 10, "Cold morning commute.");
        add("y24-1", 2024, 10, 3, 19, 0, "Old cassette found in a drawer.");
        add("y24-2", 2024, 4, 12, 11, 45, "Opening notes from 2024.");
        return out;
    };

    Scene scene;
    scene.service.on_posts = [&](std::string, std::string) -> Task<Result<PostPage>> {
        co_return PostPage{.posts = build_posts(test::Service::person(0))};
    };
    scene.shell.browser.open_profile(Source::netease, std::string(test::Service::friend_id), "Ada Lovelace", "");
    scene.draw(1180, 620);
    const auto rest = scene.draw(1180, 620);

    // The year labels its chapter in the left rail.
    const auto count_text = [](std::span<const ui::Painter::Run> runs, std::string_view s) {
        return std::ranges::count_if(runs, [&](const ui::Painter::Run &r) {
            return !r.icon && r.text == s;
        });
    };
    CHECK(count_text(rest, "2026") == 1);
    const auto *y2026_rest = scene.find("2026");
    const auto *d18_rest = scene.find("18");
    const auto *may_rest = scene.find("May");
    REQUIRE(y2026_rest != nullptr);
    REQUIRE(d18_rest != nullptr);
    REQUIRE(may_rest != nullptr);
    const float rest_year_w = y2026_rest->box.w;
    CHECK(y2026_rest->box.right() < d18_rest->box.x);
    CHECK_FALSE(overlaps(y2026_rest->box, d18_rest->box));
    CHECK_FALSE(overlaps(d18_rest->box, may_rest->box));

    // Scrolling through 2026 latches "2026" sticky at the viewport top while posts glide upward past it.
    const ui::Id scroll = ui::sub_id(scene.ui.id("profile"), scene.shell.browser.route().serial);
    scene.ui.scroll_to(scroll, 260.f);
    scene.draw(1180, 620);
    const auto *y2026_pin_a = scene.find("2026");
    const auto *d09_a = scene.find("09");
    REQUIRE(y2026_pin_a != nullptr);
    REQUIRE(d09_a != nullptr);
    const float pinned_y = y2026_pin_a->box.y;
    const float d09_y_a = d09_a->box.y;

    scene.ui.scroll_to(scroll, 340.f);
    scene.draw(1180, 620);
    const auto *y2026_pin_b = scene.find("2026");
    const auto *d09_b = scene.find("09");
    REQUIRE(y2026_pin_b != nullptr);
    REQUIRE(d09_b != nullptr);
    CHECK(std::abs(y2026_pin_b->box.y - pinned_y) < 0.5f);
    CHECK(d09_b->box.y < d09_y_a - 40.f);

    // Scrolling across the 2026 -> 2025 -> 2024 boundaries eases the outgoing year upward and fades it cleanly without
    // overlap, scale shimmer, or post-scroll spring bounce.
    bool saw_both = false;
    bool saw_push_handoff = false;
    bool saw_2025_pinned = false;
    float min_push_alpha = 1.f;
    float prev_y26 = pinned_y;
    for (float off = 360.f; off <= 960.f; off += 6.f) {
        scene.ui.scroll_to(scroll, off);
        scene.draw(1180, 620);
        const auto *y26 = scene.find("2026");
        const auto *y25 = scene.find("2025");
        if (y26 && y25) {
            saw_both = true;
            CHECK_FALSE(overlaps(y26->box, y25->box));
            CHECK(y26->box.bottom() < y25->box.y);
            CHECK(y26->box.y <= prev_y26 + 0.01f);
            prev_y26 = y26->box.y;
            if (y26->box.y < pinned_y - 1.f) {
                saw_push_handoff = true;
                min_push_alpha = std::min(min_push_alpha, y26->alpha);
                CHECK(y26->alpha < 0.96f);
                CHECK(std::abs(y26->box.w - rest_year_w) < 0.5f);
            }
        }
        if (!y26 && y25 && std::abs(y25->box.y - pinned_y) < 0.5f) {
            saw_2025_pinned = true;
        }
    }
    CHECK(saw_both);
    CHECK(saw_push_handoff);
    CHECK(min_push_alpha < 0.15f);
    CHECK(saw_2025_pinned);
    CHECK_FALSE(scene.ui.animating());

    // Narrow viewport switches gracefully to inline sticky year headers: occluded post content is clipped below the
    // pinned header (no run overlap, no hover/cursor leak), and 2026 -> 2025 pushes cleanly without overlap.
    scene.ui.scroll_to(scroll, 260.f);
    scene.draw(440, 620);
    const auto narrow = scene.draw(440, 620);
    const auto *narrow_y26 = scene.find("2026");
    REQUIRE(narrow_y26 != nullptr);
    CHECK(drew(narrow, "2026"));
    for (const auto &run : narrow) {
        if (!run.icon && (run.text == "18" || run.text == "09" || run.text == "May" || run.text == "Mar")) {
            CHECK_FALSE(overlaps(narrow_y26->box, run.box));
        }
    }
    // Pointing over the right edge of the pinned narrow year header must not leak pointer cursor from hidden "More" buttons.
    scene.point(395.f, narrow_y26->box.y + narrow_y26->box.h / 2);
    scene.draw(440, 620);
    CHECK(scene.ui.cursor == wl::Cursor::arrow);
    bool narrow_saw_both = false;
    for (float off = 320.f; off <= 960.f; off += 12.f) {
        scene.ui.scroll_to(scroll, off);
        scene.draw(440, 620);
        const auto *ny26 = scene.find("2026");
        const auto *ny25 = scene.find("2025");
        if (ny26 && ny25) {
            narrow_saw_both = true;
            CHECK_FALSE(overlaps(ny26->box, ny25->box));
            CHECK(ny26->box.bottom() < ny25->box.y);
        }
    }
    CHECK(narrow_saw_both);

    // Undated posts (time == 0) inside a dated chapter stay with that chapter without splitting it, and pagination +
    // deletion keep the year index accurate.
    {
        Scene edge;
        int page_calls = 0;
        edge.service.on_posts = [&](std::string, std::string) -> Task<Result<PostPage>> {
            ++page_calls;
            std::vector<Post> batch;
            if (page_calls == 1) {
                Post a = test::Service::post("e1", test::Service::me(), "Dated first.", true);
                a.time = make_ms(2026, 4, 10, 12, 0);
                Post b = test::Service::post("e2", test::Service::me(), "Undated middle.", true);
                b.time = 0;
                Post c = test::Service::post("e3", test::Service::me(), "Dated third.", true);
                c.time = make_ms(2026, 2, 5, 9, 0);
                batch = {std::move(a), std::move(b), std::move(c)};
                co_return PostPage{.posts = std::move(batch), .more = true, .cursor = "p2"};
            }
            Post d = test::Service::post("e4", test::Service::me(), "Older year page.", true);
            d.time = make_ms(2024, 9, 1, 18, 0);
            batch = {std::move(d)};
            co_return PostPage{.posts = std::move(batch), .more = false};
        };
        edge.shell.browser.open_own_profile();
        edge.draw(1180, 1200);
        const auto *posts_tab = edge.find("Posts");
        REQUIRE(posts_tab != nullptr);
        edge.click(*posts_tab, 1180, 1200);
        const auto p1 = edge.draw(1180, 1200);
        // Single "2026" chapter (not split around the undated post) and page 2's "2024" appended cleanly.
        CHECK(count_text(p1, "2026") == 1);
        CHECK(drew(p1, "Undated middle."));
        const auto page = edge.shell.browser.route().profile;
        REQUIRE(page != nullptr);
        CHECK(page->posts->items.size() == 4);
        CHECK(drew(p1, "2024"));
        // Deleting the last remaining 2024 post updates the year index cleanly.
        edge.shell.social.remove(Source::netease, page->posts->items.back());
        const auto after_del = edge.draw(1180, 1200);
        CHECK_FALSE(drew(after_del, "2024"));
        CHECK(count_text(after_del, "2026") == 1);
    }

    // Chinese locale formats month labels concisely as "5月" / "11月".
    struct RestoreLang {
        Language prev;
        ~RestoreLang() {
            set_language(prev);
        }
    } restore_lang{language()};
    Scene zh_scene({.language = Language::zh});
    zh_scene.service.on_posts = [&](std::string, std::string) -> Task<Result<PostPage>> {
        co_return PostPage{.posts = build_posts(test::Service::person(0))};
    };
    zh_scene.shell.browser.open_profile(Source::netease, std::string(test::Service::friend_id), "Ada Lovelace", "");
    zh_scene.draw(1180, 620);
    const auto zh_runs = zh_scene.draw(1180, 620);
    CHECK(drew(zh_runs, "2026"));
    CHECK(drew(zh_runs, "5月"));
}

TEST_CASE("playlist picker culls off-screen rows and stays valid through modal fade-out") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.motion = 1.0});
    auto &playlists = scene.shell.library.shelf(Source::netease).playlists;
    playlists.clear();
    for (int i = 0; i < 80; ++i) {
        playlists.push_back({.kind = CardKind::playlist, .source = Source::netease, .id = std::format("own-{}", i), .title = std::format("Owned {}", i), .owned = true});
    }
    auto drew_in_modal = [](std::span<const ui::Painter::Run> runs, std::string_view text) {
        return std::ranges::any_of(runs, [&](const ui::Painter::Run &run) {
            return !run.icon && run.text == text && run.box.x > 300.f;
        });
    };
    auto settle_modal = [&] {
        std::span<const ui::Painter::Run> out;
        for (int i = 0; i < 12; ++i) {
            out = scene.draw_dt(1180, 760, 0.05);
        }
        return out;
    };

    scene.shell.dialogs.pick_playlist(scene.ui, {test::Service::song(0)});
    const auto top = settle_modal();
    CHECK(drew_in_modal(top, "Add to playlist"));
    CHECK(drew_in_modal(top, "Night Train · Mira Chen"));
    CHECK(drew_in_modal(top, "New playlist"));
    CHECK(drew_in_modal(top, "Owned 0"));
    CHECK_FALSE(drew_in_modal(top, "Owned 50"));
    CHECK_FALSE(drew_in_modal(top, "Owned 79"));

    // Scrolling reveals lower rows and culls upper ones inside the picker.
    scene.ui.scroll_to(scene.ui.id("picker"), 60.f * 56.f);
    const auto scrolled = scene.draw_dt(1180, 760, 0.05);
    CHECK_FALSE(drew_in_modal(scrolled, "Owned 0"));
    const auto *row60 = scene.find("Owned 60");
    REQUIRE(row60 != nullptr);
    CHECK(row60->box.x > 300.f);

    // Picking a playlist moves tracks out and starts a 160ms fade-out; drawing mid-fade must stay valid and keep its subtitle.
    const float rx = row60->box.x + row60->box.w / 2, ry = row60->box.y + row60->box.h / 2;
    scene.button(rx, ry, true);
    scene.button(rx, ry, false);
    CHECK_FALSE(scene.ui.modal_open());
    const auto fading = scene.draw_dt(1180, 760, 0.05);
    CHECK(drew_in_modal(fading, "Add to playlist"));
    CHECK(drew_in_modal(fading, "Night Train · Mira Chen"));
    const auto closed = scene.draw_dt(1180, 760, 0.3);
    CHECK_FALSE(drew_in_modal(closed, "Add to playlist"));

    // Transitioning from picker to "New playlist" and cancelling closes cleanly without leaking pending tracks.
    scene.shell.dialogs.pick_playlist(scene.ui, {test::Service::song(0)});
    scene.ui.scroll_to(scene.ui.id("picker"), 0.f);
    settle_modal();
    const auto *create_row = scene.find("New playlist");
    REQUIRE(create_row != nullptr);
    const auto editor = scene.click(*create_row);
    CHECK(drew_in_modal(editor, "Create"));
    scene.ui.close_modal();
    scene.draw_dt(1180, 760, 0.05);
    const auto after_cancel = scene.draw_dt(1180, 760, 0.3);
    CHECK_FALSE(drew_in_modal(after_cancel, "Create"));
}

TEST_CASE("global shortcuts, keyboard list navigation, multi-select without playlist_edit, filter, sort, and VIP badge") {
    gfx::select_driver(GpuChoice::integrated);
    Track paid = test::Service::song(0);
    paid.paid = true;
    Scene scene({.queue = {paid, test::Service::song(1)}, .current = 0});
    scene.service.capabilities.playlist_edit = false;
    scene.service.features.daily.songs = []() -> Task<Result<std::vector<Track>>> {
        std::vector<Track> out;
        for (int i = 0; i < 16; ++i) {
            Track t = test::Service::song(i);
            t.id = std::format("track-{:02}", i);
            t.title = i == 0 ? "Zeta Song" : i == 1 ? "Alpha Song" : std::format("Track {:02}", i);
            t.duration = 100 + static_cast<uint32_t>(i * 15);
            t.paid = i == 0;
            out.push_back(std::move(t));
        }
        co_return out;
    };
    scene.shell.browser.open(Page::daily);
    scene.draw();
    const auto list_runs = scene.draw();

    // Long lists (>= 15) auto-show the keyword filter; paid tracks render a quiet VIP badge.
    CHECK(drew(list_runs, "Filter songs"));
    CHECK(drew(list_runs, "VIP"));

    // Global shortcuts: M mute, L stage, Q queue, C comments, D desktop lyrics, ? shortcuts modal, Up/Down volume when no row selected.
    const float vol0 = scene.shell.playback.volume();
    scene.key(XKB_KEY_Down);
    CHECK(scene.shell.playback.volume() == doctest::Approx(vol0 - 0.05f));
    scene.key(XKB_KEY_m);
    CHECK(scene.shell.playback.muted());
    scene.key(XKB_KEY_m);
    CHECK_FALSE(scene.shell.playback.muted());

    scene.key(XKB_KEY_q);
    CHECK(scene.shell.panel.kind() == SidePanel::Kind::queue);
    scene.key(XKB_KEY_q);
    CHECK(scene.shell.panel.kind() == SidePanel::Kind::none);

    scene.key(XKB_KEY_c);
    CHECK(scene.shell.panel.kind() == SidePanel::Kind::comments);
    scene.key(XKB_KEY_c);
    CHECK(scene.shell.panel.kind() == SidePanel::Kind::none);

    scene.key(XKB_KEY_d);
    CHECK(scene.shell.desktop.enabled());
    scene.key(XKB_KEY_d);
    CHECK_FALSE(scene.shell.desktop.enabled());

    scene.key(XKB_KEY_l);
    CHECK(scene.shell.stage.open());
    scene.key(XKB_KEY_l);
    CHECK_FALSE(scene.shell.stage.open());

    const auto sheet = scene.key(XKB_KEY_question);
    CHECK(scene.ui.modal_open());
    CHECK(drew(sheet, "Keyboard shortcuts"));
    scene.ui.close_modal();
    scene.draw();

    // Selecting a row switches Up/Down to row navigation and Enter plays the selected row.
    std::vector<std::string> played;
    scene.service.on_stream = [&](Track track, Quality) -> Task<Result<Stream>> {
        played.push_back(track.title);
        co_return std::unexpected("Stopped");
    };
    const auto *zeta = scene.find("Zeta Song");
    REQUIRE(zeta != nullptr);
    scene.click(*zeta);
    CHECK(scene.shell.browser.selection().selected == 0);
    const float vol_before = scene.shell.playback.volume();
    scene.key(XKB_KEY_Down);
    CHECK(scene.shell.browser.selection().selected == 1);
    CHECK(scene.shell.playback.volume() == doctest::Approx(vol_before));
    scene.key(XKB_KEY_Return);
    REQUIRE_FALSE(played.empty());
    CHECK(played.front() == "Alpha Song");

    // Ctrl+A works even when playlist_edit is false; playlist actions stay hidden while queue actions show.
    const auto multi = scene.key(XKB_KEY_a, wl::mod_ctrl);
    CHECK(scene.shell.browser.selection().choosing);
    CHECK(scene.shell.browser.selection().chosen.size() == 16);
    CHECK(drew(multi, "Play selected"));
    CHECK(drew(multi, "Play next"));
    CHECK(drew(multi, "Add to queue"));
    CHECK_FALSE(drew(multi, "Add to playlist"));

    // Exit selection via Escape, then test header sort (Title asc -> desc -> default).
    scene.key(XKB_KEY_Escape);
    CHECK_FALSE(scene.shell.browser.selection().choosing);
    const auto *title_hdr = scene.find("Title");
    REQUIRE(title_hdr != nullptr);
    scene.click(*title_hdr);
    const auto *alpha_asc = scene.find("Alpha Song");
    const auto *zeta_asc = scene.find("Zeta Song");
    REQUIRE(alpha_asc != nullptr);
    CHECK((zeta_asc == nullptr || alpha_asc->box.y < zeta_asc->box.y));

    title_hdr = scene.find("Title");
    REQUIRE(title_hdr != nullptr);
    scene.click(*title_hdr);
    const auto *zeta_desc = scene.find("Zeta Song");
    const auto *alpha_desc = scene.find("Alpha Song");
    REQUIRE(zeta_desc != nullptr);
    CHECK((alpha_desc == nullptr || zeta_desc->box.y < alpha_desc->box.y));
}

TEST_CASE("queue panel reorders rows, trims to current, and restores cleared queue via Undo toast") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.queue = {test::Service::song(0), test::Service::song(1), test::Service::song(2)}, .current = 1});
    scene.shell.panel.show(SidePanel::Kind::queue);
    scene.draw();
    const auto qruns = scene.draw();
    CHECK(drew(qruns, "Queue"));

    // Right-click the third row ("Paper Planes...") and move it to top.
    const auto *third = scene.find("Paper Planes Over the Harbour at Dawn");
    REQUIRE(third != nullptr);
    const auto qmenu = scene.click(third->box.x + 10, third->box.y + 8, 1180, 760, BTN_RIGHT);
    CHECK(drew(qmenu, "Move to top"));
    const auto *to_top = scene.find("Move to top");
    REQUIRE(to_top != nullptr);
    scene.click(*to_top);
    REQUIRE(scene.shell.playback.queue().size() == 3);
    CHECK(scene.shell.playback.queue()[0].id == "song-2");
    CHECK(scene.shell.playback.index() == 2);

    // Clear queue shows an actionable Undo toast; clicking Undo restores the snapshot.
    const auto *clear_btn = scene.find("Clear");
    REQUIRE(clear_btn != nullptr);
    const auto after_clear = scene.click(*clear_btn);
    CHECK(scene.shell.playback.queue().empty());
    CHECK(drew(after_clear, "Queue cleared"));
    const auto *undo_btn = scene.find("Undo");
    REQUIRE(undo_btn != nullptr);
    scene.click(*undo_btn);
    REQUIRE(scene.shell.playback.queue().size() == 3);
    CHECK(scene.shell.playback.queue()[0].id == "song-2");
    CHECK(scene.shell.playback.index() == 2);
}

TEST_CASE("stage lyric menu, audio info modal, comment mentions/copy, and settings cache controls") {
    gfx::select_driver(GpuChoice::integrated);
    Scene scene({.queue = {test::Service::song(0)}, .current = 0});
    scene.service.on_stream = [](Track, Quality) -> Task<Result<Stream>> {
        co_return Stream{.url = "", .level = "lossless", .highlight_from = 60.0, .highlight_to = 90.0, .gain = -3.2f, .peak = 0.91f};
    };
    scene.service.on_lyrics = [](Track) -> Task<Result<std::vector<LyricLine>>> {
        co_return std::vector<LyricLine>{
            {.time = 10.0, .text = "Midnight platform lights", .translation = "午夜站台灯光"},
            {.time = 25.0, .text = "Train pulls into rain"},
        };
    };
    scene.service.on_comments = [](Subject, std::string, bool, int, std::string) -> Task<Result<CommentPage>> {
        CommentPage page{.total = 1};
        page.comments.push_back({.id = "c1",
                                 .user_id = "ada",
                                 .user = "Ada Lovelace",
                                 .text = "Love #NightTrain# with @Ada",
                                 .time = "just now",
                                 .image = "https://example.invalid/pic.png",
                                 .image_w = 200,
                                 .image_h = 150});
        co_return page;
    };
    scene.service.on_search = [](std::string q, SearchKind kind, int, int) -> Task<Result<Batch>> {
        if (kind == SearchKind::users) {
            co_return Batch{.cards = {{.kind = CardKind::user, .source = Source::netease, .id = "ada", .title = "Ada"}}, .total = 1};
        }
        Track t = test::Service::song(0);
        t.title = "Found " + q;
        co_return Batch{.tracks = {std::move(t)}, .total = 1};
    };

    scene.shell.playback.jump(0);
    scene.shell.stage.show(true);
    scene.draw();
    const auto stage_runs = scene.draw();
    CHECK(drew(stage_runs, "Midnight platform lights"));
    CHECK(drew(stage_runs, "Lossless"));

    // Click audio format line on Stage to open Audio info modal.
    const auto *fmt = scene.find("Lossless");
    REQUIRE(fmt != nullptr);
    const auto info_runs = scene.click(*fmt);
    CHECK(scene.ui.modal_open());
    CHECK(drew(info_runs, "Audio info"));
    CHECK(drew(info_runs, "Output latency"));
    scene.ui.close_modal();
    scene.draw();

    // Right-click a lyric line to copy LRC, then quote in comment (verifying composer opens and inserts quote).
    const auto *lyric = scene.find("Midnight platform lights");
    REQUIRE(lyric != nullptr);
    const auto lmenu = scene.click(lyric->box.x + 10, lyric->box.y + 8, 1180, 760, BTN_RIGHT);
    CHECK(drew(lmenu, "Copy line"));
    CHECK(drew(lmenu, "Copy line with translation"));
    CHECK(drew(lmenu, "Copy all lyrics"));
    CHECK(drew(lmenu, "Copy LRC"));
    CHECK(drew(lmenu, "Quote in comment"));
    const auto *copy_lrc = scene.find("Copy LRC");
    REQUIRE(copy_lrc != nullptr);
    scene.click(*copy_lrc);
    REQUIRE(scene.ui.copy.has_value());
    CHECK(scene.ui.copy->contains("[00:10.00]Midnight platform lights"));
    scene.ui.copy.reset();

    lyric = scene.find("Midnight platform lights");
    REQUIRE(lyric != nullptr);
    scene.click(lyric->box.x + 10, lyric->box.y + 8, 1180, 760, BTN_RIGHT);
    const auto *quote_comment = scene.find("Quote in comment");
    REQUIRE(quote_comment != nullptr);
    scene.click(*quote_comment);
    scene.draw();
    CHECK(scene.shell.panel.kind() == SidePanel::Kind::comments);
    CHECK(scene.shell.comments.thread().draft->expanded);
    CHECK(scene.shell.comments.thread().draft->text == "\"Midnight platform lights\"");

    // Comment right-click copies comment text; clicking #NightTrain# searches and @Ada opens profile.
    const auto *topic = scene.find("#NightTrain#");
    REQUIRE(topic != nullptr);
    const auto cmenu = scene.click(topic->box.x + 10, topic->box.y + 6, 1180, 760, BTN_RIGHT);
    CHECK(drew(cmenu, "Copy comment"));
    CHECK(drew(cmenu, "Copy image"));
    CHECK(drew(cmenu, "Save image"));
    const auto *copy_comment = scene.find("Copy comment");
    REQUIRE(copy_comment != nullptr);
    scene.click(*copy_comment);
    REQUIRE(scene.ui.copy.has_value());
    CHECK(*scene.ui.copy == "Love #NightTrain# with @Ada");
    scene.ui.copy.reset();

    topic = scene.find("#NightTrain#");
    REQUIRE(topic != nullptr);
    scene.click(*topic);
    CHECK(scene.shell.browser.route().page == Page::search);
    CHECK(scene.shell.browser.searched() == "NightTrain");

    // Re-open comments and click @Ada to jump directly to Ada's profile.
    scene.shell.open_comments(Source::netease, Subject::song, "song-0", "Night Train · Mira Chen", "");
    scene.draw();
    scene.draw();
    const auto *mention = scene.find("@Ada");
    REQUIRE(mention != nullptr);
    scene.click(*mention);
    CHECK(scene.shell.browser.route().page == Page::profile);
    CHECK(scene.shell.browser.route().card.id == "ada");

    // Click chorus highlight marker under the progress bar to seek to chorus start (60s).
    scene.draw();
    {
        const ui::Rect strip{0, 760 - PlayerBar::height, 1180, PlayerBar::height};
        const auto bar_l = PlayerBar::layout(strip);
        const ui::Rect centre = ui::center(bar_l.centre, std::min(bar_l.centre.w, 600.f), bar_l.centre.h);
        const float cy = bar_l.card.y + 46 + 10;
        // 60..90s out of 200s sits around 30%..45% of the progress string.
        const float cx = centre.x + centre.w * 0.375f;
        scene.click(cx, cy + 5);
        CHECK(scene.shell.playback.song_time() == doctest::Approx(60.0));
    }

    // Settings -> General shows Close to background, Cache, Keyboard shortcuts, and settles idle.
    scene.shell.playback.stop();
    scene.shell.panel.show(SidePanel::Kind::none);
    scene.shell.browser.open(Page::settings);
    scene.draw();
    const auto *gen_tab = scene.find("General");
    REQUIRE(gen_tab != nullptr);
    const auto gen = scene.click(*gen_tab);
    CHECK(drew(gen, "Close to background"));
    CHECK(drew(gen, "Cache"));
    CHECK(drew(gen, "Keyboard shortcuts"));
    const auto *clear_cache = scene.find("Clear");
    REQUIRE(clear_cache != nullptr);
    const auto cleared = scene.click(*clear_cache);
    CHECK(drew(cleared, "Cache cleared"));
    scene.draw();
    CHECK_FALSE(scene.ui.animating());
}
