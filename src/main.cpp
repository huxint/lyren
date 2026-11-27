#include <malloc.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "core/log.hpp"
#include "gfx/context.hpp"
#include "sys/mpris.hpp"

// --debug     debug logging
// --script F  replay synthetic input from F, one command per line (window geometry coordinates):
//             wait MS | move X Y | click X Y [right|double|back|ctrl|shift] | press X Y | release X Y | scroll DY [wheel]
//             key [ctrl+][shift+][alt+]KEYSYM | commit TEXT | preedit TEXT (input method) | shot FILE.png (next frame)
//             exec SHELL-COMMAND | resize W H | quit; "lyrics move|click|press|release|shot …" targets the desktop lyrics
int main(int argc, char **argv) {
    std::vector<std::string> script;
    bool scripted = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            std::println("Usage: lyren [--debug] [--script FILE]");
            return 0;
        }
        if (a == "--debug") {
            lyren::log::min_level = lyren::log::Level::debug;
        } else if (a == "--script") {
            if (i + 1 >= argc) {
                std::println(stderr, "lyren: --script requires a file path");
                return 1;
            }
            scripted = true;
            const char *path = argv[++i];
            std::ifstream in(path);
            if (!in) {
                std::println(stderr, "lyren: cannot open script: {}", path);
                return 1;
            }
            for (std::string line; std::getline(in, line);) {
                script.push_back(std::move(line));
            }
        } else {
            std::println(stderr, "lyren: unknown option: {}", a);
            return 2;
        }
    }
    if (scripted && script.empty()) {
        return 0;
    }
    mallopt(M_ARENA_MAX, 2);

    // Signals arrive through the loop; threads created later inherit the blocked mask.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction chld{};
    chld.sa_handler = SIG_DFL;
    chld.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &chld, nullptr);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    lyren::Loop loop;
    lyren::Bus bus(loop);
    // One Lyren per session: a second launch brings the running one forward and leaves. Scripted runs never do.
    if (!scripted && bus.get() && lyren::hand_over_to_running(bus)) {
        lyren::log::info("Lyren is already running; brought it forward");
        return 0;
    }

    lyren::ConfigFile config = lyren::load_config();
    lyren::gfx::select_driver(config.config.gpu.device);
    loop.watch(sfd, EPOLLIN, [&](uint32_t) {
        signalfd_siginfo info;
        read(sfd, &info, sizeof info);
        loop.quit();
    });
    lyren::App app(loop, bus, std::move(config), std::move(script));
    loop.run();
    return 0;
}
