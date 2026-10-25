// Development CLI.
//
//   lyren-probe play <url|file> [step...]
//
// Steps run in order: wait:MS  seek:SECONDS  pause  play  vol:0..1  open:URL|FILE  quit. Without steps it is interactive:
// space pauses, ←/→ seek 10 s, ↑/↓ change the volume, q quits. Exits when the track fails, or ends after the last step.
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include "audio/player.hpp"
#include "core/log.hpp"
#include "core/memstat.hpp"
#include "net/http.hpp"

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
    std::println(stderr, "usage: lyren-probe [--debug] play <url|file> [wait:MS seek:S pause play vol:V open:SRC quit ...]");
    return 2;
}
