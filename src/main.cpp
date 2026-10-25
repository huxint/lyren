#include <malloc.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "core/log.hpp"
#include "gfx/context.hpp"

// --debug     debug logging
// --overlay   open the F12 memory overlay at start
// --script F  replay synthetic input from F, one command per line (window geometry coordinates):
//             wait MS | move X Y | click X Y [right|double|back] | scroll DY [wheel] | key [ctrl+][shift+][alt+]KEYSYM
//             commit TEXT | preedit TEXT (input method) | shot FILE.png (next frame) | exec SHELL-COMMAND | quit
int main(int argc, char** argv) {
    bool overlay = false;
    std::vector<std::string> script;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--debug") lyren::log::min_level = lyren::log::Level::debug;
        else if (a == "--overlay") overlay = true;
        else if (a == "--script" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            for (std::string line; std::getline(in, line);) script.push_back(line);
        }
    }
    mallopt(M_ARENA_MAX, 2);

    lyren::ConfigFile config = lyren::load_config();
    lyren::gfx::select_driver(config.config.gpu.device);

    // Signals arrive through the loop; threads created later inherit the blocked mask.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    lyren::Loop loop;
    loop.watch(sfd, EPOLLIN, [&](uint32_t) {
        signalfd_siginfo info;
        read(sfd, &info, sizeof info);
        loop.quit();
    });
    lyren::App app(loop, std::move(config), overlay, std::move(script));
    loop.run();
    return 0;
}
