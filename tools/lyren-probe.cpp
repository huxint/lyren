// Development CLI.
//
//   lyren-probe play <url|file> [step...]
//   lyren-probe search <query> [tracks|albums|artists|playlists]
//   lyren-probe url <song id> [standard|higher|exhigh|lossless|hires]
//   lyren-probe lyrics <song id>
//   lyren-probe login     QR login in the terminal; saves the credentials like the app does
//   lyren-probe account
//   lyren-probe call <uri> [fields] [weapi|clientlog]   raw request, e.g. call /api/song/lyric/v1 '"id":186016'; prints the JSON
//
// Steps run in order: wait:MS  seek:SECONDS  pause  play  vol:0..1  open:URL|FILE  quit. Without steps it is interactive:
// space pauses, ←/→ seek 10 s, ↑/↓ change the volume, q quits. Exits when the track fails, or ends after the last step.
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#include <qrencode.h>

#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include "app/lyrics.hpp"
#include "audio/player.hpp"
#include "core/log.hpp"
#include "core/memstat.hpp"
#include "ncm/netease.hpp"
#include "net/http.hpp"
#include "sys/creds.hpp"

using namespace lyren;

static const char *state_name(audio::Player::State s) {
    static constexpr const char *names[] = {"idle", "loading", "playing", "paused", "ended", "failed"};
    return names[int(s)];
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
                        f.rate, f.channels, player.volume(), size ? 100.0 * double(have) / double(size) : 0.0, player.buffering() ? "  buffering" : "");
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
        raw.c_lflag &= ~tcflag_t(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        std::println("space pause · ←/→ seek 10 s · ↑/↓ volume · q quit");
        loop.watch(STDIN_FILENO, EPOLLIN, [&](uint32_t) {
            char buf[16];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            std::string_view k(buf, n > 0 ? size_t(n) : 0);
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
                return void(loop.after(std::chrono::milliseconds(std::stoi(std::string(arg))), self));
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
    MemStat m = mem_sample();
    std::println("RSS {:.1f} MB (anon {:.1f}, file {:.1f})", m.rss / 1024.0, m.anon / 1024.0, m.file / 1024.0);
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
        ncm.client.cookies = creds::from_json(saved.value_or("")).netease;
        ncm.client.ensure_fingerprint();
        const std::string &cmd = args[0];
        if (cmd == "search") {
            static constexpr const char *kinds[] = {"tracks", "albums", "artists", "playlists"};
            int kind = 0;
            for (int k = 0; k < 4; ++k) {
                if (args.size() > 2 && args[2] == kinds[k]) {
                    kind = k;
                }
            }
            auto r = co_await ncm.search(args.at(1), SearchKind(kind), 0, 30);
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
            static constexpr const char *names[] = {"standard", "higher", "exhigh", "lossless", "hires"};
            int q = 3;
            for (int k = 0; k < 5; ++k) {
                if (args.size() > 2 && args[2] == names[k]) {
                    q = k;
                }
            }
            auto r = co_await ncm.stream(Track{.id = args.at(1)}, Quality(q));
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
            print_qr(*url);
            std::println("用网易云音乐 App 扫码");
            for (QrState last = QrState::waiting;;) {
                co_await lyren::sleep{loop, 2s};
                auto s = co_await ncm.qr_poll();
                if (!s) {
                    co_return fail(s.error());
                }
                if (s->state != last && s->state == QrState::scanned) {
                    std::println("已扫码：{}，请在手机上确认", s->nickname);
                }
                last = s->state;
                if (s->state == QrState::expired) {
                    co_return fail("二维码已过期");
                }
                if (s->state == QrState::confirmed) {
                    break;
                }
            }
            auto saved = co_await creds::save(loop, creds::to_json({.netease = ncm.client.cookies}));
            if (!saved) {
                co_return fail(saved.error());
            }
            std::println("已登录，凭据已加密保存");
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
    for (const char *cmd : {"search", "url", "lyrics", "login", "account", "call"}) {
        if (!args.empty() && args[0] == cmd) {
            return ncm_command(args);
        }
    }
    std::println(stderr, "usage: lyren-probe [--debug] play <url|file> [wait:MS seek:S pause play vol:V open:SRC quit ...]\n"
                         "       lyren-probe search <query> [tracks|albums|artists|playlists] | url <id> [quality] | lyrics <id> | login | account\n"
                         "       lyren-probe call <uri> [fields] [weapi|clientlog]");
    return 2;
}
