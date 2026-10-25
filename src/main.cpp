#include <malloc.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <string_view>

#include "app/app.hpp"
#include "core/log.hpp"
#include "gfx/context.hpp"

int main(int argc, char** argv) {
    bool overlay = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--debug") lyren::log::min_level = lyren::log::Level::debug;
        else if (a == "--overlay") overlay = true;
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
    lyren::App app(loop, std::move(config), overlay);
    loop.run();
    return 0;
}
