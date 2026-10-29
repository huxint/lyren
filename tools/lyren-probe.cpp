// Development CLI.
//
//   lyren-probe play <url|file> [step...]
//   lyren-probe search <query> [tracks|albums|artists|playlists]
//   lyren-probe url <song id> [standard|higher|exhigh|lossless|hires]
//   lyren-probe lyrics <song id>
//   lyren-probe login     QR login in the terminal; saves the credentials like the app does
//   lyren-probe account
//   lyren-probe call <uri> [fields] [weapi|clientlog]   raw request, e.g. call /api/song/lyric/v1 '"id":186016'; prints the JSON
//   lyren-probe qq search <query> [tracks|albums|artists|playlists] | url <song mid> [quality] | lyrics <song id>
//   lyren-probe qq album <mid> | artist <mid> | playlist <id> | top | recommend | comments <song id> [new]
//   lyren-probe qq guess | radar | new-songs [type] | new-albums [area] [offset] | saved-playlists
//   lyren-probe qq collect-playlist <public id> on|off
//   lyren-probe qq dislikes [songs|artists|styles|all] | dislike <songs|artists|styles> <id> on|off [name]
//   lyren-probe qq reorder <dir:ID> <song id>...   submit every song once in the desired order, then read back
//   lyren-probe qq call <module> <method> [param]   one signed RPC part; prints the JSON
//
// Steps run in order: wait:MS  seek:SECONDS  pause  play  vol:0..1  open:URL|FILE  quit. Without steps it is interactive:
// space pauses, ←/→ seek 10 s, ↑/↓ change the volume, q quits. Exits when the track fails, or ends after the last step.
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#include <qrencode.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <vector>
#include <unordered_set>

#include "media/lyrics.hpp"
#include "audio/player.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "ncm/netease.hpp"
#include "net/http.hpp"
#include "qq/qq.hpp"
#include "sys/creds.hpp"
#include "sys/paths.hpp"

using namespace lyren;

static const char *state_name(audio::Player::State s) {
    static constexpr std::array names = {"idle", "loading", "playing", "paused", "ended", "failed"};
    return names[static_cast<int>(s)];
}

static int play(std::string src, std::vector<std::string> steps) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    Loop loop;
    net::Http http;
    audio::Player player(loop, http);
    bool tty = isatty(STDOUT_FILENO), interactive = steps.empty() && isatty(STDIN_FILENO);

    loop.watch(sfd, EPOLLIN, [&](uint32_t) {
        loop.quit();
    });

    auto status = [&] {
        auto [have, size] = player.spool() ? player.spool()->progress() : std::pair<uint64_t, uint64_t>{};
        auto f = player.format();
        std::string line =
            std::format("{:8} {:7.2f} / {:6.2f}  {} {} Hz {} ch  vol {:.2f}  cached {:3.0f}%{}", state_name(player.state()), player.position(), player.duration(), player.codec(),
                        f.rate, f.channels, player.volume(), size ? 100.0 * static_cast<double>(have) / static_cast<double>(size) : 0.0, player.buffering() ? "  buffering" : "");
        if (tty) {
            std::print("\r{}\x1b[K", line);
        } else {
            std::println("{}", line);
        }
        std::fflush(stdout);
    };
    auto tick = [&](this auto &self) -> void {
        status();
        loop.after(tty ? 100ms : 500ms, self);
    };
    tick();

    auto open = [&](const std::string &src) {
        if (src.starts_with("http://") || src.starts_with("https://")) {
            player.open_url(src);
        } else {
            player.open_file(src);
        }
    };

    size_t step = 0;
    audio::Player::State last = player.state();
    player.on_change = [&] {
        auto s = player.state();
        if (s == last) {
            return;
        }
        last = s;
        if (s == audio::Player::State::failed) {
            std::println("{}failed: {}", tty ? "\n" : "", player.error());
            loop.quit();
        } else if (s == audio::Player::State::ended && step >= steps.size()) {
            loop.quit();
        }
    };

    termios saved{};
    if (interactive) {
        tcgetattr(STDIN_FILENO, &saved);
        termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        std::println("space pause · ←/→ seek 10 s · ↑/↓ volume · q quit");
        loop.watch(STDIN_FILENO, EPOLLIN, [&](uint32_t) {
            std::array<char, 16> buf;
            ssize_t n = read(STDIN_FILENO, buf.data(), buf.size());
            std::string_view k(buf.data(), n > 0 ? static_cast<size_t>(n) : 0);
            if (k == " ") {
                player.toggle();
            } else if (k == "q") {
                loop.quit();
            } else if (k == "\x1b[C") {
                player.seek(player.position() + 10);
            } else if (k == "\x1b[D") {
                player.seek(player.position() - 10);
            } else if (k == "\x1b[A") {
                player.set_volume(player.volume() + 0.05f);
            } else if (k == "\x1b[B") {
                player.set_volume(player.volume() - 0.05f);
            }
        });
    }

    // Steps run one after another; wait:MS pauses the sequence. The track may end on its own before the last step.
    auto run_steps = [&](this auto &self) -> void {
        while (step < steps.size()) {
            std::string_view s = steps[step++];
            auto arg = s.substr(s.find(':') + 1);
            if (!tty) {
                std::println("> {}", s);
            }
            if (s.starts_with("wait:")) {
                loop.after(std::chrono::milliseconds(std::stoi(std::string(arg))), self);
                return;
            }
            if (s.starts_with("seek:")) {
                player.seek(std::stod(std::string(arg)));
            } else if (s.starts_with("vol:")) {
                player.set_volume(std::stof(std::string(arg)));
            } else if (s.starts_with("open:")) {
                open(std::string(arg));
            } else if (s == "pause") {
                player.pause();
            } else if (s == "play") {
                player.play();
            } else if (s == "quit") {
                return loop.quit();
            } else {
                std::println(stderr, "unknown step {}", s);
            }
        }
    };
    open(src);
    run_steps();
    loop.run();

    status();
    if (tty) {
        std::println();
    }
    if (interactive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    return player.state() == audio::Player::State::failed ? 1 : 0;
}

// Two modules per character: the upper and lower half blocks.
static void print_qr(const std::string &text) {
    QRcode *q = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    int n = q->width;
    auto on = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < n && y < n && (q->data[y * n + x] & 1);
    };
    for (int y = -2; y < n + 2; y += 2) {
        std::string line;
        for (int x = -2; x < n + 2; ++x) {
            bool top = on(x, y), bottom = on(x, y + 1);
            // Light modules are drawn, so the code reads on dark terminals.
            line += !top && !bottom ? "█" : !top ? "▀" : !bottom ? "▄" : " ";
        }
        std::println("{}", line);
    }
    QRcode_free(q);
}

static int ncm_command(const std::vector<std::string> &args) {
    Loop loop;
    net::Http http;
    ncm::Netease ncm(loop, http);
    int rc = 0;
    auto fail = [&](const std::string &e) {
        std::println(stderr, "error: {}", e);
        rc = 1;
    };
    auto run = [&]() -> Task<void> {
        auto saved = co_await creds::load(loop);
        if (!saved) {
            fail(saved.error());
        }
        auto credentials = creds::from_json(saved.value_or(""));
        ncm.client.cookies = std::move(credentials["netease"]);
        ncm.client.ensure_fingerprint();
        const std::string &cmd = args[0];
        if (cmd == "search") {
            static constexpr std::array kinds = {"tracks", "albums", "artists", "playlists"};
            int kind = 0;
            for (int k = 0; k < 4; ++k) {
                if (args.size() > 2 && args[2] == kinds[k]) {
                    kind = k;
                }
            }
            auto r = co_await ncm.search(args.at(1), static_cast<SearchKind>(kind), 0, 30);
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} total", r->total);
            for (auto &t : r->tracks) {
                std::println("{:>12}  {} ({})  {} · {}  {}:{:02}", t.id, t.title, t.alias, t.artist, t.album, t.duration / 60, t.duration % 60);
            }
            for (auto &c : r->cards) {
                std::println("{:>12}  {}  {}  {}", c.id, c.title, c.subtitle, c.cover);
            }
        } else if (cmd == "url") {
            static constexpr std::array names = {"standard", "higher", "exhigh", "lossless", "hires"};
            int q = 3;
            for (int k = 0; k < 5; ++k) {
                if (args.size() > 2 && args[2] == names[k]) {
                    q = k;
                }
            }
            auto r = co_await ncm.stream(Track{.id = args.at(1)}, static_cast<Quality>(q));
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{}\nlevel {}{}", r->url, r->level, r->trial ? std::format(", trial clip only (from {} s)", r->trial_start) : "");
        } else if (cmd == "lyrics") {
            auto r = co_await ncm.lyrics(Track{.id = args.at(1)});
            if (!r) {
                co_return fail(r.error());
            }
            for (auto &l : *r) {
                std::println("{:6.2f}  {}{}{}{}", l.time, l.text, l.translation.empty() ? "" : "  /  ", l.translation,
                             l.words.empty() ? "" : std::format("  ({} words)", l.words.size()));
            }
        } else if (cmd == "login") {
            auto url = co_await ncm.qr_start();
            if (!url) {
                co_return fail(url.error());
            }
            print_qr(url->text);
            std::println("Scan with the NetEase Music app");
            for (QrState last = QrState::waiting;;) {
                co_await lyren::sleep{loop, 2s};
                auto s = co_await ncm.qr_poll();
                if (!s) {
                    co_return fail(s.error());
                }
                if (s->state != last && s->state == QrState::scanned) {
                    std::println("Scanned: {}; confirm on your phone", s->nickname);
                }
                last = s->state;
                if (s->state == QrState::expired) {
                    co_return fail("QR code expired");
                }
                if (s->state == QrState::confirmed) {
                    break;
                }
            }
            credentials["netease"] = ncm.client.cookies;
            auto saved = co_await creds::save(loop, creds::to_json(credentials));
            if (!saved) {
                co_return fail(saved.error());
            }
            std::println("Signed in; credentials saved with encryption");
            auto a = co_await ncm.account();
            if (a) {
                std::println("{} ({})", a->name, a->id);
            }
        } else if (cmd == "account") {
            auto a = co_await ncm.account();
            if (!a) {
                co_return fail(a.error());
            }
            std::println("{} ({}) {}", a->name, a->id, a->avatar);
        } else if (cmd == "call") {
            std::string api = args.size() > 3 ? args[3] : "";
            auto r = co_await ncm.client.call(args.at(1), args.size() > 2 ? args[2] : "",
                                              api == "weapi"       ? ncm::Api::weapi
                                              : api == "clientlog" ? ncm::Api::clientlog
                                                                   : ncm::Api::eapi);
            if (!r.error.empty()) {
                fail(r.error);
            }
            std::println("{}", r.body);
        }
    };
    auto main_task = [&]() -> Task<void> {
        co_await run();
        loop.quit();
    };
    spawn(main_task());
    loop.run();
    return rc;
}

static void print_tracks(const std::vector<Track> &tracks) {
    for (auto &t : tracks) {
        std::println("{:>12} {:>14} {:>14}  {}{}  {} · {}  {}:{:02}{}", t.id, t.mid, t.media, t.title, t.alias.empty() ? "" : " (" + t.alias + ')', t.artist, t.album,
                     t.duration / 60, t.duration % 60, t.paid ? "  paid" : "");
    }
}

static void print_cards(const std::vector<Card> &cards) {
    for (auto &c : cards) {
        std::println("{:>16}  {}  {}  {}", c.id, c.title, c.subtitle, c.cover);
    }
}

static std::string_view prefix(std::string_view text, size_t bytes) {
    size_t i = 0, end = 0;
    while (i < text.size() && i < bytes) {
        i = utf8::next(text, i);
        if (i <= bytes) {
            end = i;
        }
    }
    return text.substr(0, end);
}

static int qq_command(const std::vector<std::string> &args) {
    Loop loop;
    net::Http http;
    qq::Qq q(loop, http);
    int rc = 0;
    auto fail = [&](const std::string &e) {
        std::println(stderr, "error: {}", e);
        rc = 1;
    };
    auto run = [&]() -> Task<void> {
        auto saved = co_await creds::load(loop);
        if (!saved) {
            co_return fail(saved.error());
        }
        auto credentials = creds::from_json(*saved);
        q.client.session = credentials["qq"];
        const std::string &cmd = args.at(1);
        auto arg = [&](size_t i) {
            return args.size() > i ? args[i] : std::string{};
        };
        if (cmd == "search") {
            static constexpr std::array kinds = {"tracks", "albums", "artists", "playlists"};
            int kind = 0;
            for (int k = 0; k < 4; ++k) {
                if (arg(3) == kinds[k]) {
                    kind = k;
                }
            }
            auto r = co_await q.search(args.at(2), static_cast<SearchKind>(kind), 0, 30);
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} total", r->total);
            print_tracks(r->tracks);
            print_cards(r->cards);
        } else if (cmd == "url") {
            static constexpr std::array names = {"standard", "higher", "exhigh", "lossless", "hires"};
            int quality = 3;
            for (int k = 0; k < 5; ++k) {
                if (arg(3) == names[k]) {
                    quality = k;
                }
            }
            auto d = co_await q.song_detail(args.at(2));
            if (!d) {
                co_return fail(d.error());
            }
            auto r = co_await q.stream(*d, static_cast<Quality>(quality));
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{}\nlevel {}, gain {:+.2f} dB, peak {:.3f}{}", r->url, r->level, r->gain, r->peak,
                         r->trial ? std::format(", trial clip only (from {} s)", r->trial_start) : "");
        } else if (cmd == "lyrics") {
            auto r = co_await q.lyrics(Track{.source = Source::qq, .id = args.at(2)});
            if (!r) {
                co_return fail(r.error());
            }
            for (auto &l : *r) {
                std::println("{:7.3f}  {}{}{}{}", l.time, l.text, l.translation.empty() ? "" : "  /  ", l.translation,
                             l.words.empty() ? "" : std::format("  ({} words)", l.words.size()));
            }
        } else if (cmd == "album") {
            auto r = co_await q.album(args.at(2));
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} ({}) · {} · {} · comments {}\n{}", r->artist, r->artist_id, r->date, r->company, r->thread, prefix(r->about, 300));
            print_tracks(r->tracks);
        } else if (cmd == "artist") {
            auto a = co_await q.artist(args.at(2));
            if (!a) {
                co_return fail(a.error());
            }
            std::println("{} ({}) songs {} albums {} {}", a->name, a->alias, a->songs, a->albums, a->avatar);
            auto s = co_await q.artist_songs(args.at(2), 0);
            if (s) {
                print_tracks(std::vector(s->tracks.begin(), s->tracks.begin() + std::min<ptrdiff_t>(10, ssize(s->tracks))));
            }
            auto al = co_await q.artist_albums(args.at(2), 0);
            if (al) {
                print_cards(al->cards);
            }
            auto intro = co_await q.artist_intro(args.at(2));
            if (intro) {
                for (auto &[head, text] : *intro) {
                    std::println("## {}\n{}", head, prefix(text, 200));
                }
            }
        } else if (cmd == "playlist") {
            auto r = co_await q.tracks(Card{.source = Source::qq, .id = args.at(2)});
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} tracks", r->size());
            print_tracks(*r);
        } else if (cmd == "reorder") {
            if (args.size() < 4 || !arg(2).starts_with("dir:")) {
                co_return fail("Expected reorder <dir:ID> <song id>...");
            }
            const std::string id = arg(2);
            int directory = 0;
            const auto parsed = std::from_chars(id.data() + 4, id.data() + id.size(), directory);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || directory <= 0) {
                co_return fail("Expected a numeric playlist directory ID");
            }
            if (!q.logged_in()) {
                co_return fail("Sign in to QQ Music first");
            }
            auto library = co_await q.user_playlists(q.client.get("musicid"));
            if (!library) {
                co_return fail(library.error());
            }
            Card playlist;
            if (id == library->first.id) {
                playlist = library->first;
            } else {
                auto it = std::ranges::find(library->second, id, &Card::id);
                if (it == library->second.end() || !it->owned) {
                    co_return fail("Choose a playlist owned by the signed-in QQ account");
                }
                playlist = *it;
            }
            auto songs = co_await q.tracks(playlist);
            if (!songs) {
                co_return fail(songs.error());
            }
            if (args.size() - 3 != songs->size()) {
                co_return fail("Use every song in the playlist exactly once");
            }
            std::unordered_set<std::string> used;
            std::vector<Track> order;
            for (size_t i = 3; i < args.size(); ++i) {
                auto song = std::ranges::find(*songs, args[i], &Track::id);
                if (song == songs->end() || !used.insert(args[i]).second) {
                    co_return fail("Use every song in the playlist exactly once");
                }
                order.push_back(*song);
            }
            auto written = co_await q.reorder_playlist(id, order);
            auto fresh = co_await q.tracks(playlist);
            if (!fresh) {
                co_return fail(fresh.error());
            }
            if (!std::ranges::equal(*fresh, order, [](const Track &a, const Track &b) {
                    return same(a, b) && a.type == b.type;
                })) {
                co_return fail(written ? "The server did not keep the requested playlist order" : written.error());
            }
            std::println("Playlist order saved and confirmed");
            print_tracks(*fresh);
        } else if (cmd == "top" || cmd == "recommend") {
            Result<std::vector<Card>> r;
            if (cmd == "top") {
                r = co_await q.toplists();
            } else {
                r = co_await q.recommended_playlists();
            }
            if (!r) {
                co_return fail(r.error());
            }
            print_cards(*r);
        } else if (cmd == "guess" || cmd == "radar") {
            Result<std::vector<Track>> r;
            if (cmd == "guess") {
                r = co_await q.fm();
            } else {
                r = co_await q.radar();
            }
            if (!r) {
                co_return fail(r.error());
            }
            print_tracks(*r);
        } else if (cmd == "dislikes" || cmd == "dislike") {
            static constexpr std::array<std::string_view, 3> kinds = {"songs", "artists", "styles"};
            if (!q.logged_in()) {
                co_return fail("Sign in to QQ Music first");
            }
            if (cmd == "dislikes") {
                if (args.size() > 3 || (!arg(2).empty() && arg(2) != "all" && std::ranges::find(kinds, arg(2)) == kinds.end())) {
                    co_return fail("Expected dislikes [songs|artists|styles|all]");
                }
                for (size_t i = 0; i < kinds.size(); ++i) {
                    if (!arg(2).empty() && arg(2) != "all" && arg(2) != kinds[i]) {
                        continue;
                    }
                    auto r = co_await q.dislikes(static_cast<DislikeKind>(i));
                    if (!r) {
                        co_return fail(r.error());
                    }
                    std::println("{}: {} items", kinds[i], r->size());
                    for (const auto &item : *r) {
                        std::println("  {}  type {}  {}  {}", item.id, item.id_type, item.name, item.cover);
                    }
                }
            } else {
                auto category = std::ranges::find(kinds, arg(2));
                if (args.size() < 5 || args.size() > 6 || category == kinds.end() || (arg(4) != "on" && arg(4) != "off") || arg(3).empty()) {
                    co_return fail("Expected dislike <songs|artists|styles> <id> on|off [name]");
                }
                bool on = arg(4) == "on";
                DislikeKind kind = static_cast<DislikeKind>(category - kinds.begin());
                Dislike item{.source = Source::qq, .kind = kind, .id = arg(3), .name = arg(5)};
                if (on) {
                    uint64_t id = 0;
                    const auto parsed = std::from_chars(item.id.data(), item.id.data() + item.id.size(), id);
                    if (parsed.ec != std::errc{} || parsed.ptr != item.id.data() + item.id.size() || !id) {
                        co_return fail("Expected a numeric resource ID");
                    }
                } else {
                    auto items = co_await q.dislikes(kind);
                    if (!items) {
                        co_return fail(items.error());
                    }
                    auto found = std::ranges::find(*items, item.id, &Dislike::id);
                    if (found == items->end()) {
                        co_return fail("This item is not in the dislike list");
                    }
                    item = *found;
                }
                auto r = co_await q.dislike(std::move(item), on);
                if (!r) {
                    co_return fail(r.error());
                }
                std::println("{}", on ? "Hidden from recommendations" : "Restored to recommendations");
            }
        } else if (cmd == "new-songs" || cmd == "new-albums") {
            int tab = cmd == "new-albums";
            const auto &categories = q.features.release_categories[tab];
            std::string category = arg(2).empty() ? categories.front().id : arg(2);
            if (!std::ranges::any_of(categories, [&](const Category &entry) {
                    return entry.id == category;
                })) {
                co_return fail("Expected a supported release category ID");
            }
            if (tab == 0) {
                auto r = co_await q.new_songs(category);
                if (!r) {
                    co_return fail(r.error());
                }
                print_tracks(*r);
            } else {
                int offset = 0;
                if (!arg(3).empty()) {
                    const auto value = arg(3);
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), offset);
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || offset < 0) {
                        co_return fail("Expected a nonnegative album offset");
                    }
                }
                auto r = co_await q.new_albums(category, offset);
                if (!r) {
                    co_return fail(r.error());
                }
                std::println("{} total", r->total);
                print_cards(r->cards);
            }
        } else if (cmd == "collect-playlist") {
            if (args.size() != 4 || (arg(3) != "on" && arg(3) != "off")) {
                co_return fail("Expected collect-playlist <public id> on|off");
            }
            const auto id = arg(2);
            uint64_t numeric = 0;
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), numeric);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || !numeric) {
                co_return fail("Expected a numeric public playlist ID");
            }
            if (!q.logged_in()) {
                co_return fail("Sign in to QQ Music first");
            }
            auto r = co_await q.collect_playlist(id, arg(3) == "on");
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{}", arg(3) == "on" ? "Playlist saved" : "Playlist removed");
        } else if (cmd == "saved-playlists") {
            auto r = co_await q.collected_playlists();
            if (!r) {
                co_return fail(r.error());
            }
            print_cards(*r);
        } else if (cmd == "categories") {
            auto r = co_await q.categories();
            if (!r) {
                co_return fail(r.error());
            }
            for (const auto &[heading, categories] : r->groups) {
                std::println("{}", heading);
                for (const auto &category : categories) {
                    std::println("  {}  {}", category.id, category.name);
                }
            }
        } else if (cmd == "square") {
            int offset = 0;
            if (!arg(3).empty()) {
                const auto value = arg(3);
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), offset);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || offset < 0) {
                    co_return fail("Expected a nonnegative playlist offset");
                }
            }
            auto r = co_await q.square(arg(2) == "all" ? "" : arg(2), offset);
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} total", r->total);
            print_cards(r->cards);
        } else if (cmd == "similar") {
            auto r = co_await q.similar(args.at(2));
            if (!r) {
                co_return fail(r.error());
            }
            print_tracks(*r);
        } else if (cmd == "comments") {
            std::string cursor;
            for (int page = 1; page <= 2; ++page) {
                auto r = co_await q.comments(Subject::song, args.at(2), arg(3) != "new", page, cursor);
                if (!r) {
                    co_return fail(r.error());
                }
                std::println("page {}: {} of {}, more {}, cursor {}", page, r->comments.size(), r->total, r->more, r->cursor);
                for (auto &c : r->comments) {
                    std::println("  [{}] {} ({}): {}{}", c.time, c.user, c.likes, prefix(c.text, 60), c.reply.empty() ? "" : " ← " + c.reply_user);
                }
                cursor = r->cursor;
            }
        } else if (cmd == "qr" || cmd == "login") {
            auto code = co_await q.qr_start();
            if (!code) {
                co_return fail(code.error());
            }
            auto path = arg(2).empty() ? paths::state_dir() / "qq-qr.png" : std::filesystem::path(arg(2));
            {
                std::ofstream out(path, std::ios::binary);
                out.write(code->image.data(), static_cast<std::streamsize>(code->image.size()));
                if (!out) {
                    co_return fail("Could not save the QR code image");
                }
            }
            std::println("QR code: {}\nScan with QQ and confirm QQ Music sign-in", path.string());
            if (cmd == "qr") {
                auto s = co_await q.qr_poll();
                if (!s) {
                    co_return fail(s.error());
                }
                std::println("state {}", static_cast<int>(s->state));
                co_return;
            }
            for (QrState last = QrState::waiting;;) {
                co_await lyren::sleep{loop, 2s};
                auto s = co_await q.qr_poll();
                if (!s) {
                    co_return fail(s.error());
                }
                if (s->state != last) {
                    std::println("state {}", static_cast<int>(s->state));
                }
                last = s->state;
                if (s->state == QrState::expired) {
                    co_return fail("QR code expired");
                }
                if (s->state == QrState::confirmed) {
                    break;
                }
            }
            credentials["qq"] = q.client.session;
            auto stored = co_await creds::save(loop, creds::to_json(credentials));
            if (!stored) {
                co_return fail(stored.error());
            }
            std::filesystem::remove(path);
            std::println("Signed in to QQ Music; credentials saved with encryption");
        } else if (cmd == "renew") {
            if (!q.logged_in()) {
                co_return fail("Sign in to QQ Music first");
            }
            auto r = co_await q.renew();
            if (!r) {
                co_return fail(r.error());
            }
            credentials["qq"] = q.client.session;
            auto stored = co_await creds::save(loop, creds::to_json(credentials));
            if (!stored) {
                co_return fail(stored.error());
            }
            std::println("QQ Music credentials renewed");
        } else if (cmd == "account") {
            auto r = co_await q.account();
            if (!r) {
                co_return fail(r.error());
            }
            std::println("{} ({})", r->name, r->id);
        } else if (cmd == "library") {
            auto r = co_await q.user_playlists(q.client.get("musicid"));
            if (!r) {
                co_return fail(r.error());
            }
            print_cards(r->second);
            auto liked = co_await q.tracks(r->first);
            if (!liked) {
                co_return fail(liked.error());
            }
            std::println("{} liked tracks", liked->size());
            print_tracks(*liked);
        } else if (cmd == "collection") {
            auto a = co_await q.collected_albums();
            if (!a) {
                co_return fail(a.error());
            }
            print_cards(*a);
            auto b = co_await q.followed_artists();
            if (!b) {
                co_return fail(b.error());
            }
            print_cards(*b);
        } else if (cmd == "call") {
            auto r = co_await q.client.call({{args.at(2), args.at(3), args.size() > 4 ? args[4] : "{}"}});
            if (!r.error.empty()) {
                fail(r.error);
            }
            std::println("{}", r.body);
        } else {
            fail("unknown command " + cmd);
        }
    };
    auto main_task = [&]() -> Task<void> {
        co_await run();
        loop.quit();
    };
    spawn(main_task());
    loop.run();
    return rc;
}

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::erase_if(args, [](const std::string &a) {
        if (a != "--debug") {
            return false;
        }
        log::min_level = log::Level::debug;
        return true;
    });
    if (args.size() >= 2 && args[0] == "play") {
        return play(args[1], {args.begin() + 2, args.end()});
    }
    if (args.size() >= 2 && args[0] == "qq") {
        return qq_command(args);
    }
    for (const char *cmd : {"search", "url", "lyrics", "login", "account", "call"}) {
        if (!args.empty() && args[0] == cmd) {
            return ncm_command(args);
        }
    }
    std::println(
        stderr,
        "usage: lyren-probe [--debug] play <url|file> [wait:MS seek:S pause play vol:V open:SRC quit ...]\n"
        "       lyren-probe search <query> [tracks|albums|artists|playlists] | url <id> [quality] | lyrics <id> | login | account\n"
        "       lyren-probe call <uri> [fields] [weapi|clientlog]\n"
        "       lyren-probe qq search|url|lyrics|album|artist|playlist|top|recommend|guess|radar|new-songs|new-albums|saved-playlists|categories|square|similar|comments|call ...");
    return 2;
}
