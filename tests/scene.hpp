#pragma once
#include <doctest/doctest.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/images.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"
#include "sys/bus.hpp"
#include "text/atlas.hpp"
#include "ui/ui.hpp"
#include "wl/display.hpp"

// The window's content without a window: the shell over a music service that answers at once, drawn a frame at a time,
// with what each frame drew to check its layout by.
namespace lyren::test {

    struct Graphics {
        struct Validation {
            const gfx::Context &context;
            ~Validation() {
                CHECK(context.validation_errors() == 0);
            }
        };

        Loop loop;
        wl::Display display{loop};
        gfx::Context context{display.display};
        Validation validation{context};
        gfx::Renderer renderer{context};
    };

    // A music service with everything at hand: an account, listeners who follow one another, what they post and play,
    // and the playlists they keep. Every request is answered before it returns, so the frame after an action shows
    // its outcome; what was written is kept in calls.
    class Service final : public Provider {
    public:
        static constexpr std::string_view self = "me", friend_id = "ada";

        bool signed_in = true;
        std::vector<std::string> calls;
        std::move_only_function<Task<Result<std::vector<Track>>>()> on_fm;
        std::move_only_function<Task<Result<Stream>>(Track, Quality)> on_stream;
        std::move_only_function<Task<Result<std::vector<Card>>>()> on_collected_albums;
        std::move_only_function<Task<Result<void>>(std::string)> on_import_session;
        std::move_only_function<Task<SessionCheck>(std::string)> on_check_session;

        Service() {
            capabilities.library = capabilities.likes = capabilities.playlist_edit = true;
            capabilities.collect_albums = capabilities.follow_artists = true;
            capabilities.profiles = capabilities.people = capabilities.follow_users = true;
            capabilities.posts = capabilities.post_write = capabilities.ranking = capabilities.profile_playlists = true;
            capabilities.comments = {true, true, true, true};
            capabilities.comment_sort = capabilities.comment_replies = capabilities.comment_likes = capabilities.comment_post = true;
            capabilities.radio = capabilities.radio_dislike = true;
            features.daily.songs = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(0), song(1), song(2)};
            };
            features.radar = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(2), song(3), song(4)};
            };
            features.recent_plays = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(4), song(1), song(0)};
            };
        }

        static Track song(int n) {
            static constexpr std::array<std::string_view, 5> titles{"Night Train", "夜に駆ける", "Paper Planes Over the Harbour at Dawn", "回声", "Slow Radio"};
            static constexpr std::array<std::string_view, 5> artists{"Mira Chen", "YOASOBI", "The Long Afternoon Ensemble", "陈粒", "Low Tide"};
            const auto k = static_cast<size_t>(n) % titles.size();
            return {.id = std::format("song-{}", n),
                    .title = std::string(titles[k]),
                    .artist = std::string(artists[k]),
                    .album = "Tapes",
                    .duration = 200 + 10 * static_cast<uint32_t>(n)};
        }
        static Person person(int n) {
            static constexpr std::array<std::string_view, 6> names{"Ada Lovelace", "林夕", "Grace", "A Listener With A Remarkably Long Display Name", "小野", "Tom"};
            static constexpr std::array<std::string_view, 6> words{
                "Counting in the dark.", "词在心里，歌在路上。", "", "Collects cassettes, bootlegs and train timetables from everywhere.", "", "Loud."};
            const auto k = static_cast<size_t>(n) % names.size();
            return {.id = n == 0 ? std::string(friend_id) : std::format("user-{}", n),
                    .name = std::string(names[k]),
                    .signature = std::string(words[k]),
                    .fans = n == 2 ? -1 : 120 * (n + 1) * (n + 1),
                    .followed = n % 2 == 1,
                    .follows_me = n % 3 == 0};
        }
        static Post post(std::string id, const Person &author, std::string text, bool owned) {
            return {
                .id = id, .thread = "thread-" + id, .author = author, .text = std::move(text), .time = 1731600000000, .likes = 12, .comments = 3, .forwards = 1, .owned = owned};
        }
        static Person me() {
            return {.id = std::string(self), .name = "Mira Chen", .signature = "Tapes, trains and late radio."};
        }
        // A listener's timeline: words with a topic and a mention, a song shared, another post passed on, and one
        // with nothing counted on it yet.
        static std::vector<Post> timeline(const Person &author, bool owned) {
            std::vector<Post> out;
            out.push_back(post("p1", author,
                               "Rain on the window and #NightTrain# on repeat. @Ada would call this weather for counting sheep, slowly, one at a time, until the tape runs out.",
                               owned));
            out.push_back(post("p2", author, "今晚的歌单：城市、雨和慢下来的鼓点。", owned));
            out.back().track = song(1);
            out.back().liked = true;
            out.push_back(post("p3", author, "This.", owned));
            out.back().forwarded.push_back(post("p0", person(1), "把这首歌循环了一整天。", false));
            out.back().forwarded.back().track = song(3);
            out.push_back(post("p4", author, "First light.", owned));
            out.back().likes = out.back().comments = out.back().forwards = 0;
            return out;
        }

        StringMap<std::string> &session() override {
            return cookies_;
        }
        void on_session_changed(std::move_only_function<void()>) override {}
        Task<Result<void>> initialize() override {
            co_return Result<void>{};
        }
        bool logged_in() const override {
            return signed_in;
        }
        Task<Result<void>> import_session(std::string json) override {
            if (on_import_session) {
                co_return co_await on_import_session(std::move(json));
            }
            co_return co_await Provider::import_session(std::move(json));
        }
        Task<SessionCheck> check_session(std::string json) override {
            if (on_check_session) {
                co_return co_await on_check_session(std::move(json));
            }
            co_return co_await Provider::check_session(std::move(json));
        }
        Task<Result<Account>> account() override {
            if (!signed_in) {
                co_return std::unexpected("Not signed in");
            }
            co_return Account{.id = std::string(self), .name = "Mira Chen"};
        }
        Task<void> logout() override {
            signed_in = false;
            co_return;
        }
        Task<Result<Batch>> search(std::string q, SearchKind kind, int, int) override {
            if (kind == SearchKind::tracks) {
                Track t = song(0);
                t.id = "search-" + q;
                t.title = "Result for " + q;
                co_return Batch{.tracks = {std::move(t)}, .total = 1};
            }
            co_return Batch{};
        }
        Task<Result<std::vector<Track>>> tracks(Card) override {
            co_return std::vector<Track>{song(0), song(1), song(2)};
        }
        Task<Result<std::vector<Card>>> recommended_playlists() override {
            std::vector<Card> out;
            for (int i = 0; i < 6; ++i) {
                out.push_back({.id = std::format("rec-{}", i), .title = std::format("Late Night Tape {}", i + 1), .count = 18 + i * 3, .count_kind = Card::Count::songs});
            }
            co_return out;
        }
        Task<Result<std::vector<Track>>> fm() override {
            if (on_fm) {
                co_return co_await on_fm();
            }
            co_return std::vector<Track>{song(0), song(1), song(2)};
        }
        Task<Result<std::vector<LyricLine>>> lyrics(Track) override {
            co_return std::vector<LyricLine>{};
        }
        Task<Result<Stream>> stream(Track track, Quality quality) override {
            if (on_stream) {
                co_return co_await on_stream(std::move(track), quality);
            }
            co_return std::unexpected("Unavailable");
        }
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override {
            std::vector<Card> lists;
            for (int i = 0; i < 4; ++i) {
                lists.push_back({.id = std::format("{}-list-{}", uid, i),
                                 .title = i == 2 ? "A Playlist Whose Name Runs On Past Its Cover" : std::format("Mixtape {}", i + 1),
                                 .count = 12 + i,
                                 .count_kind = Card::Count::songs,
                                 .owned = i < 2});
            }
            co_return std::pair{Card{.id = uid + "-liked", .title = "Liked songs", .count = 40, .count_kind = Card::Count::songs, .owned = true}, std::move(lists)};
        }
        Task<Result<std::vector<std::string>>> liked_ids(std::string) override {
            co_return std::vector<std::string>{"song-1"};
        }
        Task<Result<std::vector<Card>>> collected_albums() override {
            if (on_collected_albums) {
                co_return co_await on_collected_albums();
            }
            co_return std::vector<Card>{
                {.kind = CardKind::album, .id = "alb-1", .title = "Harbour Tapes", .subtitle = "Mira Chen", .count = 10, .count_kind = Card::Count::songs},
                {.kind = CardKind::album, .id = "alb-2", .title = "Midnight Lines", .subtitle = "Low Tide", .count = 8, .count_kind = Card::Count::songs},
            };
        }
        Task<Result<std::vector<Card>>> followed_artists() override {
            co_return std::vector<Card>{
                {.kind = CardKind::artist, .id = "art-1", .title = "YOASOBI", .count = 14, .count_kind = Card::Count::albums},
                {.kind = CardKind::artist, .id = "art-2", .title = "Low Tide", .count = 4, .count_kind = Card::Count::albums},
            };
        }
        Task<Result<void>> like(Track track, bool on) override {
            calls.push_back(std::format("like {} {}", track.id, on));
            co_return Result<void>{};
        }

        Task<Result<Profile>> profile(std::string uid) override {
            if (uid == self) {
                co_return Profile{.id = uid,
                                  .name = "Mira Chen",
                                  .signature = "Tapes, trains and late radio.",
                                  .location = "Hangzhou",
                                  .level = 8,
                                  .follows = 128,
                                  .fans = 2304,
                                  .posts = 37,
                                  .listened = 15872,
                                  .days = 2100,
                                  .vip = true};
            }
            co_return Profile{.id = uid,
                              .name = "Ada Lovelace",
                              .signature = "Counting in the dark.\nSecond line of what she says of herself.",
                              .location = "London",
                              .level = 6,
                              .follows = 56,
                              .fans = 1234567,
                              .posts = 4,
                              .listened = 980,
                              .days = 400,
                              .follows_me = true};
        }
        Task<Result<PeoplePage>> people(std::string, bool fans, std::string) override {
            PeoplePage page{.total = fans ? 2304 : 128};
            for (int i = 0; i < 6; ++i) {
                page.people.push_back(person(fans ? 5 - i : i));
            }
            co_return page;
        }
        Task<Result<void>> follow_user(std::string uid, bool on) override {
            calls.push_back(std::format("follow {} {}", uid, on));
            co_return Result<void>{};
        }
        Task<Result<PostPage>> posts(std::string uid, std::string) override {
            co_return PostPage{.posts = timeline(uid == self ? me() : person(0), uid == self)};
        }
        Task<Result<PostPage>> feed(std::string) override {
            PostPage page;
            page.posts.push_back(post("f1", person(0), "Found the B-side at last.", false));
            page.posts.back().track = song(4);
            page.posts.push_back(post("f2", person(3), "Nothing to add.", false));
            co_return page;
        }
        Task<Result<void>> like_post(Post post, bool on) override {
            calls.push_back(std::format("like-post {} {}", post.id, on));
            co_return Result<void>{};
        }
        Task<Result<void>> delete_post(std::string id) override {
            calls.push_back("delete-post " + id);
            co_return Result<void>{};
        }
        Task<Result<void>> share(std::string text, std::optional<Track> track) override {
            calls.push_back(std::format("share {} {}", text, track ? track->id : "-"));
            co_return Result<void>{};
        }
        Task<Result<void>> forward(Post post, std::string text) override {
            calls.push_back(std::format("forward {} {}", post.id, text));
            co_return Result<void>{};
        }
        Task<Result<Ranking>> ranking(std::string, bool all_time) override {
            Ranking ranking;
            for (int i = 0; i < 5; ++i) {
                ranking.tracks.push_back(song(i));
                ranking.plays.push_back((all_time ? 400 : 40) - 7 * i);
            }
            co_return ranking;
        }
        Task<Result<CommentPage>> comments(Subject, std::string, bool, int, std::string) override {
            CommentPage page{.total = 3};
            page.comments.push_back({.id = "c1",
                                     .user_id = std::string(friend_id),
                                     .user = "Ada Lovelace",
                                     .text = "The bridge at two minutes in is the whole song.",
                                     .time = "2 hours ago",
                                     .likes = 1024,
                                     .replies = 2});
            page.comments.push_back({.id = "c2", .user = "林夕", .text = "听了十年。", .time = "yesterday", .likes = 8});
            page.comments.push_back({.id = "c3", .user = "Tom", .text = "Loud.", .time = "2024", .reply_user = "林夕", .reply = "听了十年。", .owned = true});
            co_return page;
        }

    private:
        StringMap<std::string> cookies_;
    };

    struct SceneOptions {
        Language language = Language::en;
        bool signed_in = true;
        bool dark = true;
        double motion = 0;
        std::vector<Track> queue;
        int current = -1;
    };

    class Scene {
    public:
        using Options = SceneOptions;
        static constexpr float titlebar = 48;

        explicit Scene(Options options = {})
        : images_(
              g.loop, http_, g.renderer, [] {},
              [this] {
                  return now;
              }),
          config_(configure(options)),
          shell(deps(options), saved(options)) {
            std::string errors;
            theme_ = ui::Theme::from(config_, errors);
            stage_ = theme_.stage(theme_.accent);
            REQUIRE(errors.empty());
            painter.record(true);
            if (options.signed_in) {
                spawn(shell.library.load(Source::netease));
            }
        }

        // A frame of a window w by h, a second after the one before; returns what it drew.
        std::span<const ui::Painter::Run> draw(float w = 1180, float h = 760) {
            return draw(w, h, pointer_, 1.0);
        }
        // A frame advanced by dt seconds, for testing in-flight transitions when motion > 0.
        std::span<const ui::Painter::Run> draw_dt(float w, float h, double dt) {
            return draw(w, h, pointer_, dt);
        }
        // The pointer comes to rest at (x, y).
        void point(float x, float y) {
            pointer_ = {.x = x, .y = y, .inside = true};
        }
        // A press and release at (x, y), then a frame for what it set off to show.
        std::span<const ui::Painter::Run> click(float x, float y, float w = 1180, float h = 760, uint32_t button = BTN_LEFT) {
            ui::FrameInput input{.x = x, .y = y, .inside = true};
            input.actions = {{.x = x, .y = y, .button = button, .press = true}, {.x = x, .y = y, .button = button, .press = false}};
            draw(w, h, input);
            point(x, y);
            return draw(w, h);
        }
        // Half a click at (x, y): the button goes down, or comes up, and a frame is drawn.
        std::span<const ui::Painter::Run> button(float x, float y, bool press, float w = 1180, float h = 760) {
            pointer_ = {.x = x, .y = y, .inside = true, .left_down = press};
            pointer_.actions = {{.x = x, .y = y, .button = BTN_LEFT, .press = press}};
            return draw(w, h, pointer_);
        }
        // Several clicks batched into a single frame, then a frame for what they set off to show.
        std::span<const ui::Painter::Run> clicks(std::initializer_list<std::pair<float, float>> points, float w = 1180, float h = 760) {
            ui::FrameInput input{.inside = true};
            for (const auto &[x, y] : points) {
                input.x = x;
                input.y = y;
                input.actions.push_back({.x = x, .y = y, .button = BTN_LEFT, .press = true});
                input.actions.push_back({.x = x, .y = y, .button = BTN_LEFT, .press = false});
            }
            draw(w, h, input);
            point(input.x, input.y);
            return draw(w, h);
        }
        std::span<const ui::Painter::Run> click(const ui::Painter::Run &run, float w = 1180, float h = 760) {
            return click(run.box.x + run.box.w / 2, run.box.y + run.box.h / 2, w, h);
        }
        // The first thing drawn last frame with this text, or this icon's name.
        const ui::Painter::Run *find(std::string_view text, bool icon = false) const {
            const auto runs = painter.runs();
            const auto at = std::ranges::find_if(runs, [&](const ui::Painter::Run &run) {
                return run.icon == icon && run.text == text;
            });
            return at == runs.end() ? nullptr : &*at;
        }

        // With LYREN_TRACE_DIR set, writes what the last frame drew to <dir>/<name>.txt, one primitive, text run or
        // icon to a line with its box: the layout to read when a picture of it cannot be looked at.
        void trace(std::string_view name) const {
            const char *dir = std::getenv("LYREN_TRACE_DIR");
            if (!dir) {
                return;
            }
            std::ofstream out(std::format("{}/{}.txt", dir, name));
            static constexpr std::array<std::string_view, 6> kinds{"rect", "shadow", "glyph", "image", "arc", "wave"};
            for (const gfx::Prim &prim : painter.prims()) {
                const auto kind = static_cast<gfx::Kind>(prim.info[0]);
                if (kind == gfx::Kind::glyph) {
                    continue;
                }
                const float x0 = std::max(prim.rect[0], prim.clip[0]), y0 = std::max(prim.rect[1], prim.clip[1]);
                const float x1 = std::min(prim.rect[0] + prim.rect[2], prim.clip[2]), y1 = std::min(prim.rect[1] + prim.rect[3], prim.clip[3]);
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                const auto &c = prim.color, &d = prim.color2;
                const float a = c[3] > 0 ? c[3] : 1, b = d[3] > 0 ? d[3] : 1;
                out << std::format("{} {:.1f} {:.1f} {:.1f} {:.1f} rgba({:.2f},{:.2f},{:.2f},{:.2f}) rgba2({:.2f},{:.2f},{:.2f},{:.2f}) r={:.1f} f={}\n",
                                   kinds[static_cast<size_t>(prim.info[0])], x0, y0, x1 - x0, y1 - y0, c[0] / a, c[1] / a, c[2] / a, c[3], d[0] / b, d[1] / b, d[2] / b, d[3],
                                   prim.radii[0], prim.info[2]);
            }
            for (const ui::Painter::Run &run : painter.runs()) {
                out << std::format("{} {:.1f} {:.1f} {:.1f} {:.1f} a={:.2f}{} \"{}\"\n", run.icon ? "icon" : "text", run.box.x, run.box.y, run.box.w, run.box.h, run.alpha,
                                   run.cut ? " CUT" : "", run.text);
            }
        }

        Graphics g;
        text::Fonts fonts{"sans-serif"};
        text::Shaper shaper{fonts};
        text::Atlas atlas{g.renderer, fonts};
        ui::Painter painter{atlas, shaper, fonts};
        Service service;
        double now = 1;

    private:
        static Config configure(const Options &options) {
            Config config;
            config.theme.motion = options.motion;
            config.theme.mode = options.dark ? ThemeMode::dark : ThemeMode::light;
            config.interface.language = options.language;
            return config;
        }
        ShellDeps deps(const Options &options) {
            service.signed_in = options.signed_in;
            return {.loop = g.loop,
                    .http = http_,
                    .player = player_,
                    .providers = {service, service, service, service, service},
                    .images = images_,
                    .config = config_,
                    .set_setting = [](std::string_view, std::string_view, std::string_view) {},
                    .desktop_changed = [] {},
                    .open_config = [] {},
                    .redraw = [] {},
                    .now =
                        [this] {
                            return now;
                        },
                    .silent = true,
                    .bus = bus_,
                    .raise = [](std::string_view) {}};
        }
        static SavedState saved(const Options &options) {
            SavedState state;
            state.queue = options.queue;
            state.current = options.current;
            return state;
        }
        std::span<const ui::Painter::Run> draw(float w, float h, ui::FrameInput &input, double dt = 1.0) {
            now += dt;
            painter.begin(1, 0, 0);
            ui.begin(painter, theme_, w, h, now, input);
            ui::Rect area{0, 0, w, h};
            ui::Rect bar = ui::cut_top(area, titlebar);
            // As the app does: the title bar takes input first and is drawn over the page.
            painter.defer(true);
            ui::cut_left(bar, 8);
            shell.chrome.top_bar(ui, bar, w / 2);
            painter.defer(false);
            shell.draw(ui, area, stage_);
            painter.draw_deferred();
            ui.end();
            input.consumed();
            return painter.runs();
        }

        net::Http http_;
        audio::Player player_{g.loop, http_};
        Images images_;
        Bus bus_{g.loop};
        Config config_;
        ui::Theme theme_, stage_;
        ui::FrameInput pointer_;

    public:
        Shell shell;
        ui::Ui ui;
    };

} // namespace lyren::test
